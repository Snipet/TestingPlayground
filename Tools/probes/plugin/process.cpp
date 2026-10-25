// PROBE name=plugin.process timeout=180
//
// plugin.process: the processor as a host drives it, at 48 kHz in blocks of 256, playing its default program (the first
// of the EXAMPLES: an effect's Gain and pan, an instrument's Sine). An effect gets a 1 kHz sine at -12 dBFS on the left
// and a 3 kHz sine at -18 dBFS on the right; an instrument plays A4 (440 Hz) at full velocity.
//   NAMES   P1 to P32 are the program's dials to a host: P1's name is "1 gain" (an instrument's "1 attack"), its text
//           the plain value; an unused slot's name is its number alone (P8, and the last, P32)
//   HOST    a value written through the host (setValueNotifyingHost) reaches the program: an effect's gain at +6 dB
//           puts the left out at -6 dBFS and the right at -12 dBFS; an instrument's level at -6 dB puts the note at
//           -6 dBFS; within 0.05 dB, once the dial's 20 ms glide is over
//   NAN     a NaN written by a host is ignored: the output stays finite at the last good value's level, and the
//           host's text for it is a dash
//   EMPTY   a block of 0 samples between two blocks changes nothing (an instrument's note-on in it still plays)
//   OVER    a block of 1,061 samples after prepareToPlay announced 256 reaches the Engine in parts of at most 256,
//           and the output is the run's in blocks of 256, bit for bit
//   BYPASS  bypassed (processBlockBypassed) from the first block, an effect's input passes through bit for bit and
//           an instrument is silent; bypass and un-bypass mid-stream do not click: around each edge no step between
//           neighbouring samples exceeds the steady signal's own steepest by more than 5 %, and 20 ms after the
//           un-bypass edge the output is the never-bypassed run's, bit for bit (the Engine ran through the bypass)
//   EARLY   a block before any prepareToPlay: an effect passes its input, NaN and inf as 0; an instrument is silent
//   MIDI    (an instrument) a note-on at velocity 0 is a note-off; All Notes Off (CC 123) releases every note
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr bool kInstrument = tp::product::kIsInstrument;
    constexpr double kAmpL = 0.25118864315095801;             // -12 dBFS
    constexpr double kAmpR = 0.12589254117941673;             // -18 dBFS

    void setPlain(tp::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(tp::normOf(p.control(slot), plain));
    }

    struct HostPlan
    {
        std::vector<int> blocks;
        int prepared = kBlock;
        std::size_t bypassFrom = 0, bypassTo = 0;
        std::vector<std::pair<std::size_t, juce::MidiMessage>> midi;   // absolute samples
    };

    // `in` through a fresh processor as `plan` says.
    probe::Stereo hostRun(const probe::Stereo& in, const HostPlan& plan, int* largest = nullptr)
    {
        tp::Processor p;
        p.setRateAndBufferSizeDetails(kFs, plan.prepared);
        p.prepareToPlay(kFs, plan.prepared);
        probe::Stereo out(in.size());
        std::size_t pos = 0, next = 0;                          // next: the first event not delivered yet
        for (std::size_t block = 0; block < plan.blocks.size(); ++block)
        {
            const int len = plan.blocks[block];
            juce::AudioBuffer<float> buffer(2, std::max(len, 1));
            for (int i = 0; i < len; ++i)
            {
                buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
                buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
            }
            juce::MidiBuffer midi;                              // each event once: an empty block takes those at its start
            while (next < plan.midi.size()
                   && (plan.midi[next].first < pos + static_cast<std::size_t>(len) || (len == 0 && plan.midi[next].first == pos)))
            {
                midi.addEvent(plan.midi[next].second, static_cast<int>(plan.midi[next].first - std::min(pos, plan.midi[next].first)));
                ++next;
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
        if (largest != nullptr)
            *largest = p.largestEngineCall();
        return out;
    }

    std::vector<int> blocksOf(std::size_t n, int size)
    {
        std::vector<int> b(n / static_cast<std::size_t>(size), size);
        if (const int rest = static_cast<int>(n % static_cast<std::size_t>(size)); rest > 0)
            b.push_back(rest);
        return b;
    }

    double levelDb(const std::vector<float>& x, double hz, double fromSec, double toSec)
    {
        const auto from = static_cast<std::size_t>(fromSec * kFs), to = static_cast<std::size_t>(toSec * kFs);
        return probe::dbFromLin(probe::toneAmplitude(x, hz, kFs, from, to - from));
    }
}

PROBE(plugin, process)
{
    const funkgui::HeadlessGuiScope gui;
    const std::size_t n = static_cast<std::size_t>(1.0 * kFs);
    probe::Stereo in(n);
    if constexpr (!kInstrument)
    {
        in.l = probe::sine(1000.0, kAmpL, kFs, n);
        in.r = probe::sine(3000.0, kAmpR, kFs, n);
    }
    const auto noteOn = [](int note, uint8_t velocity) { return juce::MidiMessage::noteOn(1, note, velocity); };
    std::vector<std::pair<std::size_t, juce::MidiMessage>> a4;
    if constexpr (kInstrument)
        a4.push_back({ 0, noteOn(69, 127) });

    // NAMES
    {
        tp::Processor p;
        const juce::String name = p.getParameters()[0]->getName(100);
        std::printf("P1 is \"%s\", its default's text \"%s\"; P8 is \"%s\"\n", name.toRawUTF8(),
                    p.getParameters()[0]->getCurrentValueAsText().toRawUTF8(), p.getParameters()[7]->getName(100).toRawUTF8());
        P.eq("names.p1", name == (kInstrument ? "1 attack" : "1 gain") ? 1 : 0, 1);
        P.eq("names.p8_unused", p.getParameters()[7]->getName(100) == "8" ? 1 : 0, 1);
        P.eq("names.params", static_cast<int64_t>(p.getParameters().size()) >= tp::kMaxControls ? 1 : 0, 1);
        P.eq("names.last_unused",
             p.getParameters()[tp::kMaxControls - 1]->getName(100) == juce::String(tp::kMaxControls) ? 1 : 0, 1);
        P.eq("names.p1_text", p.getParameters()[0]->getCurrentValueAsText() == (kInstrument ? "5.00" : "0.00") ? 1 : 0,
             1);
    }

    // HOST and NAN
    {
        tp::Processor p;
        p.setRateAndBufferSizeDetails(kFs, kBlock);
        p.prepareToPlay(kFs, kBlock);
        const int slot = kInstrument ? 2 : 0;                   // level, gain
        setPlain(p, slot, kInstrument ? -6.0f : 6.0f);
        probe::Stereo out(n);
        juce::MidiBuffer midi;
        for (std::size_t pos = 0; pos < n; pos += kBlock)
        {
            if (pos <= n / 2 && n / 2 < pos + kBlock)           // NAN, half way
                p.getParameters()[slot]->setValueNotifyingHost(std::numeric_limits<float>::quiet_NaN());
            juce::AudioBuffer<float> buffer(2, kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
                buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
            }
            midi.clear();
            if (kInstrument && pos == 0)
                midi.addEvent(noteOn(69, 127), 0);
            p.processBlock(buffer, midi);
            for (int i = 0; i < kBlock; ++i)
            {
                out.l[pos + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
                out.r[pos + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
            }
        }
        if constexpr (kInstrument)
        {
            P.near("host.level_db", levelDb(out.l, 440.0, 0.1, 0.4), -6.0, 0.05);
            P.near("nan.level_db", levelDb(out.l, 440.0, 0.6, 0.9), -6.0, 0.05);
        }
        else
        {
            P.near("host.left_db", levelDb(out.l, 1000.0, 0.1, 0.4), -6.0, 0.05);
            P.near("host.right_db", levelDb(out.r, 3000.0, 0.1, 0.4), -12.0, 0.05);
            P.near("nan.left_db", levelDb(out.l, 1000.0, 0.6, 0.9), -6.0, 0.05);
        }
        P.eq("nan.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
        P.eq("nan.text_dash", p.getParameters()[slot]->getCurrentValueAsText() == juce::String::fromUTF8("\xE2\x80\x93")
                                  ? 1 : 0, 1);
    }

    // EMPTY
    {
        HostPlan with, without;
        without.blocks = blocksOf(n, kBlock);
        with.blocks = without.blocks;
        with.blocks.insert(with.blocks.begin() + 3, 0);
        with.midi = a4;
        without.midi = a4;
        if constexpr (kInstrument)                             // a note-on in the empty block, at sample 768
        {
            with.midi.push_back({ 3 * kBlock, noteOn(76, 100) });
            without.midi.push_back({ 3 * kBlock, noteOn(76, 100) });
        }
        const probe::Stereo a = hostRun(in, with), b = hostRun(in, without);
        P.eq("empty.same_bits", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) ? 1 : 0, 1);
    }

    // OVER
    {
        HostPlan big, normal;
        big.blocks = blocksOf(n, 1061);
        normal.blocks = blocksOf(n, kBlock);
        big.midi = normal.midi = a4;
        int largest = 0;
        const probe::Stereo a = hostRun(in, big, &largest), b = hostRun(in, normal);
        P.le("over.largest_engine_call", largest, kBlock);
        P.eq("over.same_bits", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) ? 1 : 0, 1);
    }

    // BYPASS
    {
        HostPlan all;
        all.blocks = blocksOf(n, kBlock);
        all.bypassTo = all.blocks.size();
        all.midi = a4;
        const probe::Stereo a = hostRun(in, all);
        if constexpr (kInstrument)
            P.le("bypass.silent_peak", probe::peak(a.l), 0.0);
        else
            P.eq("bypass.passes_input", probe::sameBits(a.l, in.l) && probe::sameBits(a.r, in.r) ? 1 : 0, 1);

        HostPlan edge, steady;
        edge.blocks = steady.blocks = blocksOf(n, kBlock);
        edge.bypassFrom = 40;
        edge.bypassTo = 120;
        edge.midi = steady.midi = a4;
        const probe::Stereo e = hostRun(in, edge), s = hostRun(in, steady);
        const std::size_t fade = static_cast<std::size_t>(0.020 * kFs);
        const double steep = std::max(probe::maxStep(s.l, 2048, n - 4096), kInstrument ? 0.0 : probe::maxStep(in.l, 0, n));
        for (const std::size_t at : { edge.bypassFrom * kBlock, edge.bypassTo * kBlock })
        {
            const std::string k = "bypass.edge" + std::to_string(at);
            P.le(k + ".max_step", probe::maxStep(e.l, at - 48, fade + 96), steep * 1.05);
        }
        bool back = true;
        for (std::size_t i = edge.bypassTo * kBlock + fade + 1; i < n; ++i)
            back = back && e.l[i] == s.l[i] && e.r[i] == s.r[i];
        P.eq("bypass.unbypassed_is_steady_bits", back ? 1 : 0, 1);
    }

    // EARLY
    {
        tp::Processor p;
        juce::AudioBuffer<float> buffer(2, 64);
        for (int i = 0; i < 64; ++i)
        {
            buffer.setSample(0, i, i == 5 ? std::numeric_limits<float>::quiet_NaN() : 0.25f);
            buffer.setSample(1, i, i == 9 ? std::numeric_limits<float>::infinity() : -0.25f);
        }
        juce::MidiBuffer midi;
        p.processBlock(buffer, midi);
        if constexpr (kInstrument)
            P.le("early.silent", static_cast<double>(std::max(buffer.getMagnitude(0, 0, 64), buffer.getMagnitude(1, 0, 64))),
                 0.0);
        else
        {
            P.near("early.passes", static_cast<double>(buffer.getSample(0, 0)), 0.25, 0.0);
            P.near("early.nan_zero", static_cast<double>(buffer.getSample(0, 5)), 0.0, 0.0);
            P.near("early.inf_zero", static_cast<double>(buffer.getSample(1, 9)), 0.0, 0.0);
        }
    }

    // MIDI (an instrument)
    if constexpr (kInstrument)
    {
        HostPlan off, cc;
        off.blocks = cc.blocks = blocksOf(n, kBlock);
        off.midi = { { 0, noteOn(69, 127) }, { 4800, noteOn(69, 0) } };
        cc.midi = { { 0, noteOn(60, 127) }, { 0, noteOn(64, 127) }, { 4800, juce::MidiMessage::controllerEvent(1, 123, 0) } };
        const probe::Stereo a = hostRun(in, off), b = hostRun(in, cc);
        const auto tailDb = [&](const probe::Stereo& x) { return probe::rmsDb(x.l, n - 4800, 4800); };
        P.le("midi.velocity0_releases_db", tailDb(a), -100.0);
        P.le("midi.cc123_releases_db", tailDb(b), -100.0);
    }
    return P.finish();
}
