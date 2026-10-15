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
#include <vector>

namespace fpt = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    void setPlain(fpt::Processor& p, std::size_t i, float plain)
    {
        p.getParameters()[static_cast<int>(i)]->setValueNotifyingHost(fpt::toNormalised(fpt::kParams[i], plain));
    }

    // What the processor must hand Engine::setParams: the parameters' plain values, a non-finite one keeping the last
    // good value, clamped to the table's range.
    void readValues(fpt::Processor& p, fpt::ParamValues& v)
    {
        for (std::size_t i = 0; i < fpt::kParams.size(); ++i)
        {
            const float x = p.parameters().getRawParameterValue(fpt::kParams[i].id)->load();
            if (std::isfinite(x))
                v[i] = std::clamp(x, fpt::kParams[i].min, fpt::kParams[i].max);
        }
    }

    // The level (dBFS) of A4 over [fromSec, toSec): a whole number of its cycles (440 Hz: 0.05 s holds 22).
    double levelDb(const std::vector<float>& x, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, 440.0, kFs, from, to - from));
    }

    struct BlockNote { std::size_t block; int offset; bool on; };
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    fpt::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    constexpr int kNote = 69;
    constexpr juce::uint8 kVelocity = 64;
    const BlockNote notes[] = { { 4, 100, true }, { 700, 37, false } };          // on at 1124; off at 3.73 s
    const std::size_t onsetAt = 4 * kBlock + 100, offAt = 700 * kBlock + 37;
    const std::size_t numBlocks = 800, n = numBlocks * kBlock;                   // 4.27 s

    probe::Stereo out(n);
    std::vector<fpt::ParamValues> blockValues;
    fpt::ParamValues held = fpt::defaultValues();
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t block = 0; block < numBlocks; ++block)
    {
        if (block == 200)                                                        // 1.07 s: GAIN +6 dB
            setPlain(processor, fpt::param::gain, 6.0f);
        if (block == 260)                                                        // 1.39 s: a host writes a NaN
            processor.getParameters()[static_cast<int>(fpt::param::gain)]->setValueNotifyingHost(
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
        fpt::Engine engine;
        engine.prepare(kFs, kBlock);
        probe::Stereo ref(n);
        for (std::size_t block = 0; block < numBlocks; ++block)
        {
            std::vector<fpt::Event> events;
            for (const BlockNote& e : notes)
                if (e.block == block)
                {
                    fpt::Event ev;
                    ev.offset = static_cast<uint32_t>(e.offset);
                    ev.kind = e.on ? fpt::EventKind::noteOn : fpt::EventKind::noteOff;
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
    return P.finish();
}
