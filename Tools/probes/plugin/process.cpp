// PROBE name=plugin.process timeout=120
//
// plugin.process: the processor as a host drives it, at 48 kHz in blocks of 256, with MIDI.
//   MIDI    a note-on (A4, velocity 64) at offset 100 of block 4 starts on that sample: the output is exactly 0 up to
//           it and in it (the sine starts at phase 0) and not 0 on the next sample, on both channels, which are equal;
//           it plays at 64/127 of full level (0.25), within 0.05 dB
//   OFFSETS a second note (E5) on at offset 17 and off at offset 240 of block 300, both inside one block: the output
//           first differs from the same run without it on sample 17 + 1 of that block, and that note is gone again
//           (exactly the run without it) 50 ms (RELEASE) after its note-off
//   HOST    GAIN written through the host (setValueNotifyingHost, +6 dB) reaches the DSP: the held note is 6 dB louder
//   NAN     a NaN written by a host is ignored: the output stays finite, at the last good value's level
//   OFF     the note-off, at offset 37 of a later block, releases the note: 0.5 s after it (RELEASE 50 ms) the output
//           is exactly 0 and no voice is left sounding
//   ENGINE  processBlock's output, both channels, is the Engine's bit for bit, given the same notes at the same offsets
//           and the plain values the parameters held at each block (what the processor must hand the Engine)
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace tps = testingplaygroundsynth;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

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

    struct BlockNote { std::size_t block; int offset; int note; bool on; };

    constexpr juce::uint8 kVelocity = 64;

    // The Engine alone, given `notes` block by block with the values the processor saw.
    probe::Stereo engineRun(const std::vector<BlockNote>& notes, const std::vector<tps::ParamValues>& values,
                            std::size_t numBlocks)
    {
        tps::Engine engine;
        engine.prepare(kFs, kBlock);
        probe::Stereo ref(numBlocks * kBlock);
        for (std::size_t block = 0; block < numBlocks; ++block)
        {
            std::vector<tps::Event> events;
            for (const BlockNote& e : notes)
                if (e.block == block)
                {
                    tps::Event ev;
                    ev.offset = static_cast<uint32_t>(e.offset);
                    ev.kind = e.on ? tps::EventKind::noteOn : tps::EventKind::noteOff;
                    ev.note = static_cast<uint8_t>(e.note);
                    ev.velocity = e.on ? static_cast<float>(kVelocity) / 127.0f : 0.0f;
                    events.push_back(ev);
                }
            engine.setParams(values[block]);
            const std::size_t pos = block * kBlock;
            float* ch[2] = { ref.l.data() + pos, ref.r.data() + pos };
            engine.process(ch, 2, kBlock, events);
        }
        return ref;
    }

    // The first index from `from` on where a and b differ; a.size() when none.
    std::size_t firstDifference(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
    {
        for (std::size_t i = from; i < a.size() && i < b.size(); ++i)
            if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
                return i;
        return a.size();
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    tps::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    constexpr int kA4 = 69, kE5 = 76;
    const std::vector<BlockNote> notes = { { 4, 100, kA4, true },      // on at 1124
                                           { 300, 17, kE5, true },     // on at 76,817, off at 77,040: one block
                                           { 300, 240, kE5, false },
                                           { 700, 37, kA4, false } };  // off at 3.73 s
    const std::size_t onsetAt = 4 * kBlock + 100, offAt = 700 * kBlock + 37;
    const std::size_t e5On = 300 * kBlock + 17, e5Off = 300 * kBlock + 240;
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
                midi.addEvent(e.on ? juce::MidiMessage::noteOn(1, e.note, kVelocity)
                                   : juce::MidiMessage::noteOff(1, e.note), e.offset);
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
    std::size_t first = 0;
    while (first < n && out.l[first] == 0.0f)
        ++first;
    P.eq("midi.onset.first_nonzero_sample", static_cast<int64_t>(first), static_cast<int64_t>(onsetAt + 1));
    P.eq("midi.left_equals_right", probe::sameBits(out.l, out.r) ? 1 : 0, 1);
    const double velocityDb = probe::dbFromLin(0.25 * static_cast<double>(kVelocity) / 127.0);
    P.near("midi.velocity64.level_dbfs", levelDb(out.l, 0.5, 1.0), velocityDb, 0.05);

    // OFFSETS: against the Engine without E5.
    {
        std::vector<BlockNote> withoutE5;
        for (const BlockNote& e : notes)
            if (e.note != kE5)
                withoutE5.push_back(e);
        const probe::Stereo ref = engineRun(withoutE5, blockValues, numBlocks);
        P.eq("offsets.e5.first_difference_sample", static_cast<int64_t>(firstDifference(out.l, ref.l, 0)),
             static_cast<int64_t>(e5On + 1));
        const std::size_t gone = e5Off + static_cast<std::size_t>(0.05 * kFs) + 1;
        P.eq("offsets.e5.gone_after_release", firstDifference(out.l, ref.l, gone) == n ? 1 : 0, 1);
    }

    // HOST and NAN
    P.near("host.gain6.level_dbfs", levelDb(out.l, 1.2, 1.35), velocityDb + 6.0, 0.05);
    P.eq("nan.output_finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.near("nan.level_keeps_last_good_dbfs", levelDb(out.l, 1.45, 1.6), velocityDb + 6.0, 0.05);

    // OFF
    const std::size_t silentFrom = offAt + static_cast<std::size_t>(0.5 * kFs);
    const std::vector<float> tail(out.l.begin() + static_cast<std::ptrdiff_t>(silentFrom), out.l.end());
    P.le("off.after_500ms.peak", static_cast<double>(probe::peak(tail)), 0.0);
    P.eq("off.voices_left", voicesLeft, 0);

    // ENGINE: an Engine given the same notes and values, block by block.
    {
        const probe::Stereo ref = engineRun(notes, blockValues, numBlocks);
        P.eq("engine.left.same_bits", probe::sameBits(out.l, ref.l) ? 1 : 0, 1);
        P.eq("engine.right.same_bits", probe::sameBits(out.r, ref.r) ? 1 : 0, 1);
    }
    return P.finish();
}
