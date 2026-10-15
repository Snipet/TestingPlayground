// PROBE name=plugin.process timeout=120
//
// plugin.process: the processor as a host drives it (the instrument), at 48 kHz in blocks of 256, with MIDI.
//   MIDI    a note-on (A4, velocity 64) at offset 100 of block 4 sounds from that sample: the output is exactly 0
//           before it and not after it, on both channels, which are equal; it plays at 64/127 of full level (0.25),
//           within 0.05 dB
//   HOST    GAIN written through the host (setValueNotifyingHost, +6 dB) reaches the DSP: the held note is 6 dB louder
//   NAN     a NaN written by a host is ignored: the output stays finite, at the last good value's level
//   OFF     the note-off, at offset 37 of a later block, releases the note: 0.5 s after it (RELEASE 300 ms falls 80 dB
//           in its time) the output is under -90 dBFS and no voice is left sounding
//   ENGINE  processBlock's output, both channels, is the Engine's bit for bit, given the same notes at the same offsets
//           and the plain values the parameters held at each block (what the processor must hand the Engine)
// What a host may send, each on a fresh processor, from raw MIDI messages. A voice is held while its key is down; after
// its release (RELEASE 300 ms; the rows wait 373 ms) no voice sounds and the output is exactly 0.
//   CC123   All Notes Off (controller 123) at offset 77 of a block releases a four-note chord at once
//   CC120   All Sound Off (controller 120) the same
//   VEL0    a note-on at velocity 0 (0x90 nn 00) is a note-off: it releases the note, starts nothing
//   EMPTY   a note-off in a block of 0 samples (JUCE's VST3 wrapper passes such blocks on) still releases its note
//   FULL    one block of 4,096 samples with 1,100 note-ons (16 notes) and then their 16 note-offs at offset 4,000: more
//           events than one Engine call takes (kMaxEventsPerBlock); none is lost, and the block is the Engine's,
//           given all of them in one call, bit for bit
//   OVER    a block of 1,061 samples after prepareToPlay announced 256, with notes inside it: the Engine gets it in
//           parts of at most 256 samples, and the block is the Engine's in one call, bit for bit
//   BYPASS  while a host bypasses the processor (processBlockBypassed), after its 20 ms fade-out, the output is exactly
//           0; a note-off then still releases its note, and a note-on starts nothing
//   EDGE    bypass and un-bypass while a note is held do not click (A4 at velocity 127 from sample 0, held; blocks of
//           256, bypassed from block 8 to block 18, edges where the note is near its peak): from 1 ms before each edge
//           to 1 ms after its 20 ms fade, no step between neighbouring samples exceeds the held note's own steepest by
//           more than 5 %, where a hard switch would step at least 3 times as far (the `abrupt` row, from the steady
//           runs, shows the rows can fail); 20 ms after the bypass edge the output is exactly 0, and 20 ms after the
//           un-bypass edge it is the never-bypassed run's, bit for bit (the held note kept running through the bypass
//           and fades back in), each within 0.1 ms
// This file belongs to the instrument starter (scripts/starters/instrument): `plugin.py clone --type` replaces it with
// the other kind's. Change it with the algorithm.
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace tps = testingplaygroundsynth;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kFadeMs = 20.0;                          // a bypass edge's fade

    void setPlain(tps::Processor& p, std::size_t i, float plain)
    {
        p.getParameters()[static_cast<int>(i)]->setValueNotifyingHost(tps::toNormalised(tps::kParams[i], plain));
    }

    // What the processor must hand Engine::setParams: the parameters' plain values, a non-finite one keeping the last
    // good value, clamped to the table's range.
    void readValues(tps::Processor& p, tps::ParamValues& v)
    {
        for (std::size_t i = 0; i < tps::kParams.size(); ++i)
        {
            const float x = p.parameters().getRawParameterValue(tps::kParams[i].id)->load();
            if (std::isfinite(x))
                v[i] = std::clamp(x, tps::kParams[i].min, tps::kParams[i].max);
        }
    }

    // The level (dBFS) of A4 over [fromSec, toSec): a whole number of its cycles (440 Hz: 0.05 s holds 22).
    double levelDb(const std::vector<float>& x, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, 440.0, kFs, from, to - from));
    }

    struct BlockNote { std::size_t block; int offset; bool on; };

    constexpr juce::uint8 kVelocity = 64;

    using Messages = std::vector<std::pair<int, juce::MidiMessage>>;   // (offset, message)

    // MIDI messages and the same notes as Engine events.
    struct Score
    {
        Messages messages;
        std::vector<tps::Event> events;

        void add(int offset, const juce::MidiMessage& m, tps::EventKind kind, int note, float velocity)
        {
            messages.emplace_back(offset, m);
            tps::Event e;
            e.offset = static_cast<uint32_t>(offset);
            e.kind = kind;
            e.note = static_cast<uint8_t>(note);
            e.velocity = velocity;
            events.push_back(e);
        }
        void on(int offset, int note)
        {
            add(offset, juce::MidiMessage::noteOn(1, note, kVelocity), tps::EventKind::noteOn, note,
                static_cast<float>(kVelocity) / 127.0f);
        }
        void off(int offset, int note)
        {
            add(offset, juce::MidiMessage::noteOff(1, note), tps::EventKind::noteOff, note, 0.0f);
        }
    };

    // A fresh processor at kFs, played block by block as a host would.
    struct Host
    {
        explicit Host(int blockSize = kBlock, int prepared = kBlock) : buffer(2, blockSize)
        {
            p.setRateAndBufferSizeDetails(kFs, prepared);
            p.prepareToPlay(kFs, prepared);
        }

        // One block of `len` samples (up to the buffer's size; 0 is a block a host may send) with `messages`, through
        // processBlock or processBlockBypassed. Its output stays in `last`; returns its peak over both channels.
        float play(const Messages& messages, int len, bool bypassed = false)
        {
            juce::MidiBuffer midi;
            for (const auto& [offset, m] : messages)
                midi.addEvent(m, offset);
            buffer.clear();
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, len);
            if (bypassed)
                p.processBlockBypassed(view, midi);
            else
                p.processBlock(view, midi);
            last = probe::Stereo(static_cast<std::size_t>(len));
            for (int i = 0; i < len; ++i)
            {
                last.l[static_cast<std::size_t>(i)] = view.getSample(0, i);
                last.r[static_cast<std::size_t>(i)] = view.getSample(1, i);
            }
            return std::max(probe::peak(last.l), probe::peak(last.r));
        }

        // `count` blocks of kBlock samples without MIDI; their peak.
        float idle(int count, bool bypassed = false)
        {
            float pk = 0.0f;
            for (int i = 0; i < count; ++i)
                pk = std::max(pk, play({}, kBlock, bypassed));
            return pk;
        }

        // The Engine alone, prepared and set as the processor's, given `events` in one call of `len` samples.
        probe::Stereo engineOnce(const std::vector<tps::Event>& events, int len)
        {
            tps::ParamValues values = tps::defaultValues();
            readValues(p, values);
            tps::Engine engine;
            engine.prepare(kFs, len);
            engine.setParams(values);
            probe::Stereo ref(static_cast<std::size_t>(len));
            float* ch[2] = { ref.l.data(), ref.r.data() };
            engine.process(ch, 2, len, events);
            return ref;
        }

        int sounding() const { return p.engine().activeVoices(); }
        int held() const { return p.engine().heldVoices(); }

        tps::Processor p;
        juce::AudioBuffer<float> buffer;
        probe::Stereo last{ 0 };
    };

    // After a release that began in the last block: 70 blocks (373 ms; RELEASE is 300 ms), then 10 more that must be
    // exactly 0, with no voice left.
    void expectSilenceAfterRelease(funkgui::test::Probe& P, Host& h, const std::string& key)
    {
        h.idle(70);
        P.le(key + ".peak_after_release", static_cast<double>(h.idle(10)), 0.0);
        P.eq(key + ".voices_left", h.sounding(), 0);
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    tps::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    constexpr int kNote = 69;
    const BlockNote notes[] = { { 4, 100, true }, { 700, 37, false } };          // on at 1124; off at 3.73 s
    const std::size_t onsetAt = 4 * kBlock + 100, offAt = 700 * kBlock + 37;
    const std::size_t numBlocks = 800, n = numBlocks * kBlock;                   // 4.27 s

    probe::Stereo out(n);
    std::vector<tps::ParamValues> blockValues;
    tps::ParamValues held = tps::defaultValues();
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t block = 0; block < numBlocks; ++block)
    {
        if (block == 200)                                                        // 1.07 s: GAIN +6 dB
            setPlain(processor, tps::param::gain, 6.0f);
        if (block == 260)                                                        // 1.39 s: a host writes a NaN
            processor.getParameters()[static_cast<int>(tps::param::gain)]->setValueNotifyingHost(
                std::numeric_limits<float>::quiet_NaN());
        midi.clear();
        for (const BlockNote& e : notes)
            if (e.block == block)
                midi.addEvent(e.on ? juce::MidiMessage::noteOn(1, kNote, kVelocity)
                                   : juce::MidiMessage::noteOff(1, kNote), e.offset);
        buffer.clear();
        readValues(processor, held);
        blockValues.push_back(held);
        processor.processBlock(buffer, midi);
        const std::size_t pos = block * kBlock;
        for (int i = 0; i < kBlock; ++i)
        {
            out.l[pos + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.r[pos + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    const int voicesLeft = processor.engine().activeVoices();
    processor.releaseResources();

    // MIDI
    const std::vector<float> before(out.l.begin(), out.l.begin() + static_cast<std::ptrdiff_t>(onsetAt));
    const std::vector<float> after(out.l.begin() + static_cast<std::ptrdiff_t>(onsetAt),
                                   out.l.begin() + static_cast<std::ptrdiff_t>(onsetAt + 48));
    P.eq("midi.onset.silent_before", probe::peak(before) == 0.0f ? 1 : 0, 1);
    P.eq("midi.onset.sounds_from_offset", probe::peak(after) > 0.0f ? 1 : 0, 1);
    P.eq("midi.left_equals_right", probe::sameBits(out.l, out.r) ? 1 : 0, 1);
    const double velocityDb = probe::dbFromLin(0.25 * static_cast<double>(kVelocity) / 127.0);
    P.near("midi.velocity64.level_dbfs", levelDb(out.l, 0.5, 1.0), velocityDb, 0.05);

    // HOST and NAN
    P.near("host.gain6.level_dbfs", levelDb(out.l, 1.2, 1.35), velocityDb + 6.0, 0.05);
    P.eq("nan.output_finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.near("nan.level_keeps_last_good_dbfs", levelDb(out.l, 1.45, 1.65), velocityDb + 6.0, 0.05);

    // OFF
    const std::size_t silentFrom = offAt + static_cast<std::size_t>(0.5 * kFs);
    const std::vector<float> tail(out.l.begin() + static_cast<std::ptrdiff_t>(silentFrom), out.l.end());
    P.le("off.after_500ms.peak_dbfs", probe::dbFromLin(probe::peak(tail)), -90.0);
    P.eq("off.voices_left", voicesLeft, 0);

    // ENGINE: an Engine given the same notes and values, block by block.
    {
        tps::Engine engine;
        engine.prepare(kFs, kBlock);
        probe::Stereo ref(n);
        for (std::size_t block = 0; block < numBlocks; ++block)
        {
            std::vector<tps::Event> events;
            for (const BlockNote& e : notes)
                if (e.block == block)
                {
                    tps::Event ev;
                    ev.offset = static_cast<uint32_t>(e.offset);
                    ev.kind = e.on ? tps::EventKind::noteOn : tps::EventKind::noteOff;
                    ev.note = static_cast<uint8_t>(kNote);
                    ev.velocity = e.on ? static_cast<float>(kVelocity) / 127.0f : 0.0f;
                    events.push_back(ev);
                }
            engine.setParams(blockValues[block]);
            const std::size_t pos = block * kBlock;
            float* ch[2] = { ref.l.data() + pos, ref.r.data() + pos };
            engine.process(ch, 2, kBlock, events);
        }
        P.eq("engine.left.same_bits", probe::sameBits(out.l, ref.l) ? 1 : 0, 1);
        P.eq("engine.right.same_bits", probe::sameBits(out.r, ref.r) ? 1 : 0, 1);
    }

    // CC123, CC120: a chord, then the controller at offset 77 of block 20.
    for (const int cc : { 123, 120 })
    {
        const std::string key = "cc" + std::to_string(cc);
        Host h;
        Messages chord;
        for (const int note : { 60, 64, 67, 72 })
            chord.emplace_back(10, juce::MidiMessage::noteOn(1, note, kVelocity));
        h.play(chord, kBlock);
        h.idle(19);
        P.eq(key + ".chord.held_before", h.held(), 4);
        h.play({ { 77, juce::MidiMessage::controllerEvent(1, cc, 0) } }, kBlock);
        P.eq(key + ".held_after", h.held(), 0);
        expectSilenceAfterRelease(P, h, key);
    }

    // VEL0: A4, then 0x90 69 00 at offset 40 of block 20.
    {
        Host h;
        h.play({ { 5, juce::MidiMessage::noteOn(1, kNote, kVelocity) } }, kBlock);
        h.idle(19);
        P.eq("vel0.held_before", h.held(), 1);
        h.play({ { 40, juce::MidiMessage(0x90, kNote, 0) } }, kBlock);
        P.eq("vel0.note_on_is_note_off.held_after", h.held(), 0);
        expectSilenceAfterRelease(P, h, "vel0");
    }

    // EMPTY: A4, then its note-off in a block of 0 samples.
    {
        Host h;
        h.play({ { 5, juce::MidiMessage::noteOn(1, kNote, kVelocity) } }, kBlock);
        h.idle(19);
        h.play({ { 0, juce::MidiMessage::noteOff(1, kNote) } }, 0);
        P.eq("empty_block.note_off.held_after", h.held(), 0);
        expectSilenceAfterRelease(P, h, "empty_block");
    }

    // FULL: 1,100 note-ons, every 3 samples, cycling through notes 40..55, then their note-offs at offset 4,000.
    {
        constexpr int kBig = 4096, kOns = 1100, kOffAt = 4000;
        static_assert(kOns + 16 > tps::kMaxEventsPerBlock && 3 * (kOns - 1) < kOffAt);
        Score score;
        for (int i = 0; i < kOns; ++i)
            score.on(3 * i, 40 + i % 16);
        for (int note = 40; note < 56; ++note)
            score.off(kOffAt, note);
        Host h(kBig, kBig);
        h.play(score.messages, kBig);
        P.eq("full.held_after", h.held(), 0);
        const probe::Stereo ref = h.engineOnce(score.events, kBig);
        P.eq("full.engine.same_bits", probe::sameBits(h.last.l, ref.l) && probe::sameBits(h.last.r, ref.r) ? 1 : 0, 1);
        expectSilenceAfterRelease(P, h, "full");
    }

    // OVER: prepared for 256, a block of 1,061 with notes on and off inside it.
    {
        constexpr int kOver = 4 * kBlock + 37;
        Score score;
        score.on(10, 60);
        score.on(300, 64);
        score.off(700, 60);
        score.on(1000, 67);
        Host h(kOver, kBlock);
        h.play(score.messages, kOver);
        P.le("over.largest_engine_call", h.p.largestEngineCall(), kBlock);
        const probe::Stereo ref = h.engineOnce(score.events, kOver);
        P.eq("over.engine.same_bits", probe::sameBits(h.last.l, ref.l) && probe::sameBits(h.last.r, ref.r) ? 1 : 0, 1);
        P.eq("over.held_after", h.held(), 2);
    }

    // BYPASS: A4 held; the host bypasses the processor for ten blocks, in which A4 is let go and D4 played. The first
    // four (1,024 samples) hold the 20 ms fade-out; the other six must be silent.
    {
        Host h;
        h.play({ { 5, juce::MidiMessage::noteOn(1, kNote, kVelocity) } }, kBlock);
        h.idle(9);
        h.play({ { 50, juce::MidiMessage::noteOff(1, kNote) } }, kBlock, true);
        h.play({ { 20, juce::MidiMessage::noteOn(1, 62, kVelocity) } }, kBlock, true);
        P.eq("bypass.note_on_starts_nothing.held", h.held(), 0);
        h.idle(2, true);
        P.le("bypass.after_fade.output_peak", static_cast<double>(h.idle(6, true)), 0.0);
        expectSilenceAfterRelease(P, h, "bypass.note_off_during_bypass");
    }

    // EDGE: A4 held from sample 0; bypassed from block 8 (43 ms) to block 18 (96 ms) of 28, against the never-bypassed
    // run and silence.
    {
        constexpr std::size_t kBlocks = 28, kOff = 8, kOn = 18, kLen = kBlocks * kBlock;
        constexpr std::size_t kFade = static_cast<std::size_t>(kFs * kFadeMs / 1000.0), kPad = 48;   // 960; 1 ms
        constexpr std::size_t off = kOff * kBlock, on = kOn * kBlock;
        const auto run = [](std::size_t bypassFrom, std::size_t bypassTo) {
            Host h;
            probe::Stereo y(kLen);
            for (std::size_t block = 0; block < kBlocks; ++block)
            {
                Messages m;
                if (block == 0)
                    m.emplace_back(0, juce::MidiMessage::noteOn(1, kNote, static_cast<juce::uint8>(127)));
                h.play(m, kBlock, block >= bypassFrom && block < bypassTo);
                const auto at = static_cast<std::ptrdiff_t>(block * kBlock);
                std::copy(h.last.l.begin(), h.last.l.end(), y.l.begin() + at);
                std::copy(h.last.r.begin(), h.last.r.end(), y.r.begin() + at);
            }
            return y;
        };
        const probe::Stereo sounding = run(0, 0), got = run(kOff, kOn);
        const std::vector<float> silence(kLen, 0.0f);
        const probe::EdgeSteps bypassEdge = probe::edgeSteps(got.l, sounding.l, silence, off, kFade, kPad);
        const probe::EdgeSteps unbypassEdge = probe::edgeSteps(got.l, silence, sounding.l, on, kFade, kPad);
        P.le("edge.bypass.max_step_over_own", bypassEdge.stepOverOwn, 1.05);
        P.le("edge.unbypass.max_step_over_own", unbypassEdge.stepOverOwn, 1.05);
        P.ge("edge.abrupt.step_over_own_min", std::min(bypassEdge.abruptOverOwn, unbypassEdge.abruptOverOwn), 3.0);
        P.eq("edge.left_equals_right", probe::sameBits(got.l, got.r) ? 1 : 0, 1);

        const std::size_t quietFrom = probe::settledFrom(got.l, silence, off, on);
        P.near("edge.bypass.fade_ms", static_cast<double>(quietFrom - off) * 1000.0 / kFs, kFadeMs, 0.1);
        const std::size_t soundingFrom = probe::settledFrom(got.l, sounding.l, on, kLen);
        P.near("edge.unbypass.held_note.fade_ms", static_cast<double>(soundingFrom - on) * 1000.0 / kFs, kFadeMs,
               0.1);
    }
    return P.finish();
}
