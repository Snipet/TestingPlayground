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
// What a host may send, each on a fresh processor at the defaults, with the same input:
//   EMPTY   a block of 0 samples between two blocks changes nothing: the output is the run's without it, bit for bit
//   OVER    a block of 1,061 samples after prepareToPlay announced 256: the Engine gets it in parts of at most 256
//           samples, and the output is the run's in blocks of 256, bit for bit
//   BYPASS  while a host bypasses the processor (processBlockBypassed) from its first block on, the input passes
//           through, bit for bit, and the Engine keeps running (on the input, unheard)
//   EDGE    bypass and un-bypass mid-stream do not click (GAIN -24 dB, so the processed sound is 1/16 of the input; a
//           400 Hz sine at -12 dBFS left and 600 Hz at -18 dBFS right; blocks of 256, bypassed from block 6 to block
//           24, edges where both sines are near their peaks): from 1 ms before each edge to 1 ms after its 20 ms
//           fade, no step between neighbouring samples exceeds the steeper steady signal's own (the never-bypassed
//           output's or the input's) by more than 5 %, on each channel, where a hard switch would step at least 3
//           times as far (the `abrupt` row, from the steady runs, shows the rows can fail); 20 ms after the bypass
//           edge the output is the input, and 20 ms after the un-bypass edge the never-bypassed output, bit for bit
//           (the Engine ran through the bypass), each within 0.1 ms
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
#include <string>
#include <vector>

namespace tps = testingplaygroundsynth;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kAmpL = 0.25118864315095801;             // -12 dBFS
    constexpr double kAmpR = 0.12589254117941673;             // -18 dBFS
    constexpr double kFadeMs = 20.0;                          // a bypass edge's crossfade

    double onePoleDb(double f, double fc)
    {
        const double r = std::tan(std::numbers::pi * f / kFs) / std::tan(std::numbers::pi * fc / kFs);
        return -10.0 * std::log10(1.0 + r * r);
    }

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

    double levelDb(const std::vector<float>& x, double hz, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, hz, kFs, from, to - from));
    }

    // How a host drives a fresh processor: the blocks' lengths (0 is a block a host may send), the size prepareToPlay
    // announces, the blocks it bypasses (indices in [bypassFrom, bypassTo): processBlockBypassed), and GAIN.
    struct HostPlan
    {
        std::vector<int> blocks;
        int prepared = kBlock;
        std::size_t bypassFrom = 0, bypassTo = 0;
        float gainDb = 0.0f;
    };

    // `in` through a fresh processor as `plan` says.
    probe::Stereo hostRun(const probe::Stereo& in, const HostPlan& plan, int* largestEngineCall = nullptr)
    {
        tps::Processor p;
        p.setRateAndBufferSizeDetails(kFs, plan.prepared);
        p.prepareToPlay(kFs, plan.prepared);
        setPlain(p, tps::param::gain, plan.gainDb);
        probe::Stereo out(in.size());
        juce::MidiBuffer midi;
        std::size_t pos = 0;
        for (std::size_t block = 0; block < plan.blocks.size(); ++block)
        {
            const int len = plan.blocks[block];
            juce::AudioBuffer<float> buffer(2, std::max(len, 1));
            for (int i = 0; i < len; ++i)
            {
                buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
                buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
            }
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, len);
            if (block >= plan.bypassFrom && block < plan.bypassTo)
                p.processBlockBypassed(view, midi);
            else
                p.processBlock(view, midi);
            for (int i = 0; i < len; ++i)
            {
                out.l[pos + static_cast<std::size_t>(i)] = view.getSample(0, i);
                out.r[pos + static_cast<std::size_t>(i)] = view.getSample(1, i);
            }
            pos += static_cast<std::size_t>(len);
        }
        if (largestEngineCall != nullptr)
            *largestEngineCall = p.largestEngineCall();
        return out;
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    tps::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    const std::size_t n = static_cast<std::size_t>(3.0 * kFs);
    probe::Stereo in;
    in.l = probe::sine(1000.0, kAmpL, kFs, n);
    in.r = probe::sine(3000.0, kAmpR, kFs, n);
    probe::Stereo out(n);
    std::vector<tps::ParamValues> blockValues;
    tps::ParamValues held = tps::defaultValues();

    setPlain(processor, tps::param::gain, 12.0f);
    setPlain(processor, tps::param::tone, 20000.0f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t pos = 0; pos < n; pos += kBlock)
    {
        if (pos == 96 * kBlock)                                 // 0.512 s
        {
            setPlain(processor, tps::param::gain, -6.0f);
            setPlain(processor, tps::param::tone, 1000.0f);
        }
        if (pos == 192 * kBlock)                                // 1.024 s: a host writes a NaN
            processor.getParameters()[static_cast<int>(tps::param::gain)]->setValueNotifyingHost(
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
        tps::Engine engine;
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

    // EMPTY, OVER, BYPASS: 20 blocks' worth of the input, at the defaults.
    {
        constexpr std::size_t kLen = 20 * kBlock;
        probe::Stereo part(kLen);
        std::copy(in.l.begin(), in.l.begin() + kLen, part.l.begin());
        std::copy(in.r.begin(), in.r.begin() + kLen, part.r.begin());
        const HostPlan plain{ .blocks = std::vector<int>(20, kBlock) };
        const probe::Stereo want = hostRun(part, plain);

        HostPlan withEmpty = plain;
        withEmpty.blocks.insert(withEmpty.blocks.begin() + 10, 0);
        const probe::Stereo empty = hostRun(part, withEmpty);
        const bool same = probe::sameBits(empty.l, want.l) && probe::sameBits(empty.r, want.r);
        P.eq("empty_block.changes_nothing", same ? 1 : 0, 1);

        constexpr int kOver = 4 * kBlock + 37;                    // 1,061 samples, then the rest
        int largest = 0;
        const probe::Stereo over = hostRun(
            part, { .blocks = { kBlock, kOver, static_cast<int>(kLen) - kBlock - kOver } }, &largest);
        P.le("over.largest_engine_call", largest, kBlock);
        P.eq("over.same_bits", probe::sameBits(over.l, want.l) && probe::sameBits(over.r, want.r) ? 1 : 0, 1);

        HostPlan bypassAll = plain;
        bypassAll.bypassTo = plain.blocks.size();
        int bypassedLargest = 0;
        const probe::Stereo bypassed = hostRun(part, bypassAll, &bypassedLargest);
        P.eq("bypass.passes_input", probe::sameBits(bypassed.l, part.l) && probe::sameBits(bypassed.r, part.r) ? 1 : 0,
             1);
        P.eq("bypass.engine_keeps_running.largest_call", bypassedLargest, kBlock);
    }

    // EDGE: bypassed from block 6 (32 ms) to block 24 (128 ms) of 33, against the two steady runs. (The bypass does not
    // last a whole number of either sine's cycles: an Engine that stopped during it would resume a state that differs.)
    {
        constexpr std::size_t kBlocks = 33, kOff = 6, kOn = 24, kLen = kBlocks * kBlock;
        constexpr std::size_t kFade = static_cast<std::size_t>(kFs * kFadeMs / 1000.0), kPad = 48;   // 960; 1 ms
        constexpr std::size_t off = kOff * kBlock, on = kOn * kBlock;
        probe::Stereo x;
        x.l = probe::sine(400.0, kAmpL, kFs, kLen);
        x.r = probe::sine(600.0, kAmpR, kFs, kLen);
        HostPlan plan{ .blocks = std::vector<int>(kBlocks, kBlock), .gainDb = -24.0f };
        const probe::Stereo processed = hostRun(x, plan);                  // never bypassed
        plan.bypassTo = kBlocks;
        const probe::Stereo bypassed = hostRun(x, plan);                   // always: the input
        plan.bypassFrom = kOff;
        plan.bypassTo = kOn;
        const probe::Stereo got = hostRun(x, plan);

        double abrupt = 1e30;
        for (const bool left : { true, false })
        {
            const std::string ch = left ? "left" : "right";
            const std::vector<float>& y = left ? got.l : got.r;
            const std::vector<float>& wet = left ? processed.l : processed.r;
            const std::vector<float>& dry = left ? bypassed.l : bypassed.r;
            const probe::EdgeSteps bypassEdge = probe::edgeSteps(y, wet, dry, off, kFade, kPad);
            const probe::EdgeSteps unbypassEdge = probe::edgeSteps(y, dry, wet, on, kFade, kPad);
            P.le("edge.bypass." + ch + ".max_step_over_own", bypassEdge.stepOverOwn, 1.05);
            P.le("edge.unbypass." + ch + ".max_step_over_own", unbypassEdge.stepOverOwn, 1.05);
            abrupt = std::min({ abrupt, bypassEdge.abruptOverOwn, unbypassEdge.abruptOverOwn });
        }
        P.ge("edge.abrupt.step_over_own_min", abrupt, 3.0);

        const std::size_t dryFrom = std::max(probe::settledFrom(got.l, bypassed.l, off, on),
                                             probe::settledFrom(got.r, bypassed.r, off, on));
        P.near("edge.bypass.fade_ms", static_cast<double>(dryFrom - off) * 1000.0 / kFs, kFadeMs, 0.1);
        const std::size_t wetFrom = std::max(probe::settledFrom(got.l, processed.l, on, kLen),
                                             probe::settledFrom(got.r, processed.r, on, kLen));
        P.near("edge.unbypass.fade_ms", static_cast<double>(wetFrom - on) * 1000.0 / kFs, kFadeMs, 0.1);
    }
    return P.finish();
}
