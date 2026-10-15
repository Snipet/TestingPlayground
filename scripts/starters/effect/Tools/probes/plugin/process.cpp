// PROBE name=plugin.process timeout=120
//
// plugin.process: the processor as a host drives it (the effect), at 48 kHz in blocks of 256, with a 1 kHz sine at
// -12 dBFS on the left and a 3 kHz sine at -18 dBFS on the right.
//   HOST    parameters written through the host (setValueNotifyingHost, normalised) reach the DSP: at GAIN +12 dB and
//           TONE 20 kHz the left comes out at 0 dBFS and the right at -6 dBFS (less TONE's own response at 1 and
//           3 kHz); after a write of GAIN -6 dB and TONE 1 kHz mid-stream, at -18 dBFS - 3.01 dB and at -24 dBFS less
//           the one-pole's response at 3 kHz; within 0.05 dB
//   ENGINE  processBlock's output, both channels, is the Engine's bit for bit, run on the same input with the plain
//           values the parameters held at each block (what the processor must hand the Engine)
//   NAN     a NaN written by a host is ignored: the output stays finite, and keeps the last good value's level
// This file belongs to the effect starter (scripts/starters/effect): `plugin.py clone --type` replaces it with the
// other kind's. Change it with the algorithm.
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"

#include "dsp/Engine.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace fpt = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kAmpL = 0.25118864315095801;             // -12 dBFS
    constexpr double kAmpR = 0.12589254117941673;             // -18 dBFS

    double onePoleDb(double f, double fc)
    {
        const double r = std::tan(std::numbers::pi * f / kFs) / std::tan(std::numbers::pi * fc / kFs);
        return -10.0 * std::log10(1.0 + r * r);
    }

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

    double levelDb(const std::vector<float>& x, double hz, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, hz, kFs, from, to - from));
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    fpt::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    const std::size_t n = static_cast<std::size_t>(3.0 * kFs);
    probe::Stereo in;
    in.l = probe::sine(1000.0, kAmpL, kFs, n);
    in.r = probe::sine(3000.0, kAmpR, kFs, n);
    probe::Stereo out(n);
    std::vector<fpt::ParamValues> blockValues;
    fpt::ParamValues held = fpt::defaultValues();

    setPlain(processor, fpt::param::gain, 12.0f);
    setPlain(processor, fpt::param::tone, 20000.0f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t pos = 0; pos < n; pos += kBlock)
    {
        if (pos == 96 * kBlock)                                 // 0.512 s
        {
            setPlain(processor, fpt::param::gain, -6.0f);
            setPlain(processor, fpt::param::tone, 1000.0f);
        }
        if (pos == 192 * kBlock)                                // 1.024 s: a host writes a NaN
            processor.getParameters()[static_cast<int>(fpt::param::gain)]->setValueNotifyingHost(
                std::numeric_limits<float>::quiet_NaN());
        const int len = static_cast<int>(std::min<std::size_t>(kBlock, n - pos));
        buffer.setSize(2, len, false, false, true);
        for (int i = 0; i < len; ++i)
        {
            buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
        }
        readValues(processor, held);
        blockValues.push_back(held);
        processor.processBlock(buffer, midi);
        for (int i = 0; i < len; ++i)
        {
            out.l[pos + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.r[pos + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    processor.releaseResources();

    // HOST: the levels at the first values (0.25 s to 0.5 s) and after the write (0.75 s to 1.0 s).
    const double inL = probe::dbFromLin(kAmpL), inR = probe::dbFromLin(kAmpR);
    P.near("host.gain12.left.level_dbfs", levelDb(out.l, 1000.0, 0.25, 0.5), inL + 12.0 + onePoleDb(1000.0, 20000.0),
           0.05);
    P.near("host.gain12.right.level_dbfs", levelDb(out.r, 3000.0, 0.25, 0.5), inR + 12.0 + onePoleDb(3000.0, 20000.0),
           0.05);
    P.near("host.gain-6.tone1k.left.level_dbfs", levelDb(out.l, 1000.0, 0.75, 1.0),
           inL - 6.0 + onePoleDb(1000.0, 1000.0), 0.05);
    P.near("host.gain-6.tone1k.right.level_dbfs", levelDb(out.r, 3000.0, 0.75, 1.0),
           inR - 6.0 + onePoleDb(3000.0, 1000.0), 0.05);

    // NAN: finite throughout, and the level after the NaN is the last good value's.
    P.eq("nan.output_finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.near("nan.left.level_keeps_last_good_dbfs", levelDb(out.l, 1000.0, 1.5, 2.5),
           inL - 6.0 + onePoleDb(1000.0, 1000.0), 0.05);

    // ENGINE: the same input through an Engine with the values each block held.
    {
        fpt::Engine engine;
        engine.prepare(kFs, kBlock);
        probe::Stereo ref = in;
        std::size_t block = 0;
        for (std::size_t pos = 0; pos < n; pos += kBlock, ++block)
        {
            const int len = static_cast<int>(std::min<std::size_t>(kBlock, n - pos));
            engine.setParams(blockValues[block]);
            float* ch[2] = { ref.l.data() + pos, ref.r.data() + pos };
            engine.process(ch, 2, len, {});
        }
        P.eq("engine.left.same_bits", probe::sameBits(out.l, ref.l) ? 1 : 0, 1);
        P.eq("engine.right.same_bits", probe::sameBits(out.r, ref.r) ? 1 : 0, 1);
    }
    return P.finish();
}
