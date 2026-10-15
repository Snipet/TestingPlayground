// PROBE name=plugin.process timeout=120
//
// plugin.process: the processor as a host drives it, at 48 kHz in blocks of 256, with a 1 kHz sine at -12 dBFS on the
// left and a 3 kHz sine at -18 dBFS on the right, for 3 s.
//   HOST    parameters written through the host (setValueNotifyingHost, normalised) reach the DSP: at MIX 0 % (the dry
//           path) and GAIN +12 dB the left comes out at 0 dBFS and the right at -6 dBFS; after a write of GAIN -6 dB at
//           -18 and -24 dBFS; within 0.05 dB
//   NAN     a NaN written by a host is ignored: the output stays finite, and keeps the last good value's level; the
//           host's text for the parameter is a dash, not "nan"
//   ENGINE  processBlock's output, both channels, is the Engine's bit for bit, run on the same input with the plain
//           values the parameters held at each block and the LFO tables the processor was given: the default shape
//           from the first block, and a shape the editor sets mid-stream (EditorPort::setShape, the graph's call) from
//           the next block on. MIX 50 %, RATE 5 Hz and DEPTH 2 ms from 1.33 s, so the reverb and its modulation are in
//           the comparison, and the new shape (from 2.13 s) changes the output.
// Change it with the algorithm.
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"

#include "dsp/Engine.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kAmpL = 0.25118864315095801;             // -12 dBFS
    constexpr double kAmpR = 0.12589254117941673;             // -18 dBFS

    void setPlain(tp::Processor& p, std::size_t i, float plain)
    {
        p.getParameters()[static_cast<int>(i)]->setValueNotifyingHost(tp::toNormalised(tp::kParams[i], plain));
    }

    // What the processor must hand Engine::setParams: the parameters' plain values, a non-finite one keeping the last
    // good value, clamped to the table's range.
    void readValues(tp::Processor& p, tp::ParamValues& v)
    {
        for (std::size_t i = 0; i < tp::kParams.size(); ++i)
        {
            const float x = p.parameters().getRawParameterValue(tp::kParams[i].id)->load();
            if (std::isfinite(x))
                v[i] = std::clamp(x, tp::kParams[i].min, tp::kParams[i].max);
        }
    }

    double levelDb(const std::vector<float>& x, double hz, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, hz, kFs, from, to - from));
    }

    tp::LfoShape customShape()
    {
        tp::LfoShape s;
        s.points[0] = { 0.0f, 0.2f, 0.0f };
        s.points[1] = { 0.3f, 0.9f, 3.0f };
        s.points[2] = { 0.6f, 0.4f, -2.0f };
        s.points[3] = { 1.0f, 0.2f, 0.0f };
        s.count = 4;
        return s;
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    tp::Processor processor;
    processor.setRateAndBufferSizeDetails(kFs, kBlock);
    processor.prepareToPlay(kFs, kBlock);

    const std::size_t n = static_cast<std::size_t>(3.0 * kFs);
    probe::Stereo in;
    in.l = probe::sine(1000.0, kAmpL, kFs, n);
    in.r = probe::sine(3000.0, kAmpR, kFs, n);
    probe::Stereo out(n);
    std::vector<tp::ParamValues> blockValues;
    tp::ParamValues held = tp::defaultValues();
    constexpr std::size_t kShapeBlock = 416;                    // 2.13 s

    setPlain(processor, tp::param::mix, 0.0f);
    setPlain(processor, tp::param::gain, 12.0f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    std::size_t block = 0;
    for (std::size_t pos = 0; pos < n; pos += kBlock, ++block)
    {
        if (block == 96)                                        // 0.512 s
            setPlain(processor, tp::param::gain, -6.0f);
        if (block == 192)                                       // 1.024 s: a host writes a NaN
        {
            juce::AudioProcessorParameter* gain = processor.getParameters()[static_cast<int>(tp::param::gain)];
            gain->setValueNotifyingHost(std::numeric_limits<float>::quiet_NaN());
            const juce::String text = gain->getCurrentValueAsText();
            P.eq("nan.host_text_is_dash", text == juce::String::fromUTF8("\xE2\x80\x93") ? 1 : 0, 1);
        }
        if (block == 250)                                       // 1.333 s: the reverb in, modulated
        {
            setPlain(processor, tp::param::mix, 50.0f);
            setPlain(processor, tp::param::modrate, 5.0f);
            setPlain(processor, tp::param::moddepth, 2.0f);
        }
        if (block == kShapeBlock)
            processor.editorPort().setShape(customShape());
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

    // HOST: the dry path's levels at the first values (0.25 s to 0.5 s) and after the write (0.75 s to 1.0 s).
    const double inL = probe::dbFromLin(kAmpL), inR = probe::dbFromLin(kAmpR);
    P.near("host.mix0.gain12.left.level_dbfs", levelDb(out.l, 1000.0, 0.25, 0.5), inL + 12.0, 0.05);
    P.near("host.mix0.gain12.right.level_dbfs", levelDb(out.r, 3000.0, 0.25, 0.5), inR + 12.0, 0.05);
    P.near("host.mix0.gain-6.left.level_dbfs", levelDb(out.l, 1000.0, 0.75, 1.0), inL - 6.0, 0.05);
    P.near("host.mix0.gain-6.right.level_dbfs", levelDb(out.r, 3000.0, 0.75, 1.0), inR - 6.0, 0.05);

    // NAN: finite throughout, and the level after the NaN is the last good value's.
    P.eq("nan.output_finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.near("nan.left.level_keeps_last_good_dbfs", levelDb(out.l, 1000.0, 1.05, 1.3), inL - 6.0, 0.05);

    // ENGINE: the same input through an Engine with the values and tables each block held.
    {
        tp::LfoTable initial, custom;
        tp::renderTable(tp::defaultShape(), initial);
        tp::LfoShape shape = customShape();
        tp::sanitize(shape);
        tp::renderTable(shape, custom);
        const auto run = [&](const tp::LfoTable& later) {
            tp::Engine engine;
            engine.prepare(kFs, kBlock);
            engine.setLfoTable(initial);
            probe::Stereo ref = in;
            std::size_t b = 0;
            for (std::size_t pos = 0; pos < n; pos += kBlock, ++b)
            {
                const int len = static_cast<int>(std::min<std::size_t>(kBlock, n - pos));
                if (b == kShapeBlock)
                    engine.setLfoTable(later);
                engine.setParams(blockValues[b]);
                float* ch[2] = { ref.l.data() + pos, ref.r.data() + pos };
                engine.process(ch, 2, len, {});
            }
            return ref;
        };
        const probe::Stereo ref = run(custom);
        P.eq("engine.left.same_bits", probe::sameBits(out.l, ref.l) ? 1 : 0, 1);
        P.eq("engine.right.same_bits", probe::sameBits(out.r, ref.r) ? 1 : 0, 1);
        const probe::Stereo unchanged = run(initial);           // the shape is heard: without it the output differs
        P.eq("engine.shape_changes_output", probe::sameBits(out.l, unchanged.l) ? 0 : 1, 1);
    }
    return P.finish();
}
