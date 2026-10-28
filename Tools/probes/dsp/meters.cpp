// PROBE name=dsp.meters timeout=120
//
// dsp.meters: what the Engine publishes for the panel's displays (Engine::Meters, Engine::scope), at 48 kHz in blocks
// of 256, a program's #pragma panel displays naming its fields:
//   LEVELS  an effect: a sine at 0.8 in, at half out: the input's peak 0.8 and RMS 0.566 (left and right), the
//           output's 0.4 and 0.283, within 1 %; an instrument: a voice at 0.25 out
//   FALL    after the sound stops the peaks fall 20 dB a second (half a second: 10 dB), and the RMS (its power
//           smoothed over 50 ms) more than 40 dB in half a second
//   TAPS    the fields the displays show, in display order: a float, an int, a bool, as the program wrote them last
//   VOICE   an instrument's fields come from its newest sounding voice (a second note's while both sound, the first's
//           after the second's voice has ended), 0 once none sounds
//   SCOPE   the last samples of the output are the Engine's output, bit for bit (mono: the sides' mean); an effect's
//           input likewise
//   FRAME   the frame counts the blocks (a last, shorter one too)
//   PROGRAM the meters name the program their fields are of (Patch::program): the one that played the last block,
//           not one offered and not yet taken
//   READERS the levels and the scopes are measured only while someone reads them (Engine::addReader; the rows above
//           have a reader, as a panel is): with none the levels read 0 and the scope stands still while the fields
//           and the frame go on; a reader back, the next block's levels are its own (its peak and RMS, not smoothed up
//           from 0), and the scope holds that block, silence before it
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    // The panel's lines and fields for both kinds: x (a float), k (an int), g (a bool) and v (a voice's note).
    const char* kLines = "#pragma panel meter x\n#pragma panel number k\n#pragma panel led g\n#pragma panel number v\n";
}

PROBE(dsp, meters)
{
    std::string source;
    if constexpr (kInstrument)
        source = std::string(kLines) +
                 "module Voice\n{\n    float x;\n    int k;\n    bool g;\n    float v;\n    float ph;\n    bool held;\n"
                 "    void start(float note, float velocity) { v = note; held = true; ph = 0; }\n"
                 "    void stop() { held = false; }\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n"
                 "            const float s = 0.25f * sinf(6.28318531f * ph);\n            ph += 0.01f;\n"
                 "            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                 "        x = 0.3f;\n        k = 7;\n        g = true;\n        return held;\n    }\n};\n";
    else
        source = std::string(kLines) +
                 "module Effect\n{\n    float x;\n    int k;\n    bool g;\n    float v;\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= 0.5f;\n            right[i] *= 0.5f;\n        }\n"
                 "        x = 0.3f;\n        k = 7;\n        g = true;\n        v = 60;\n    }\n};\n";
    probe::Rig rig(kFs, kBlock);
    rig.engine().addReader();                                    // as a panel does: the levels and the scopes measured
    std::string messages;
    const bool ok = rig.load(source, &messages);
    if (!messages.empty())
        std::printf("%s", messages.c_str());
    P.eq("load.compiles", ok ? 1 : 0, 1);
    P.eq("load.four_displays", static_cast<int64_t>(rig.panel().displays.size()), 4);
    if (!ok)
        return P.finish();

    const std::size_t second = static_cast<std::size_t>(kFs);
    pg::Meters m;
    // LEVELS
    probe::Stereo in(second);
    if constexpr (!kInstrument)
        in.l = in.r = probe::sine(1000.0, 0.8, kFs, second);
    std::vector<probe::Note> notes;
    if constexpr (kInstrument)
        notes.push_back({ 0, pg::EventKind::noteOn, 60, 0.8f });
    const probe::Stereo out = rig.run(in, kBlock, notes);
    P.eq("levels.read", rig.engine().meters(m) ? 1 : 0, 1);
    if constexpr (kInstrument)
    {
        P.near("levels.out_peak", static_cast<double>(m.outPeak[0]), 0.25, 0.0025);
        P.near("levels.out_rms", static_cast<double>(m.outRms[1]), 0.25 / std::sqrt(2.0), 0.0025);
        P.eq("levels.no_input", m.inPeak[0] == 0.0f && m.inRms[1] == 0.0f ? 1 : 0, 1);
    }
    else
    {
        P.near("levels.in_peak", static_cast<double>(m.inPeak[0]), 0.8, 0.008);
        P.near("levels.in_rms", static_cast<double>(m.inRms[1]), 0.8 / std::sqrt(2.0), 0.006);
        P.near("levels.out_peak", static_cast<double>(m.outPeak[1]), 0.4, 0.004);
        P.near("levels.out_rms", static_cast<double>(m.outRms[0]), 0.4 / std::sqrt(2.0), 0.003);
    }
    const uint32_t frames = m.frame;
    P.eq("frame.counts_blocks", static_cast<int64_t>(frames), static_cast<int64_t>((second + kBlock - 1) / kBlock));

    // TAPS
    P.near("taps.float", static_cast<double>(m.taps[0]), 0.3, 1e-6);
    P.near("taps.int", static_cast<double>(m.taps[1]), 7.0, 1e-6);
    P.near("taps.bool", static_cast<double>(m.taps[2]), 1.0, 1e-6);
    P.near("taps.note", static_cast<double>(m.taps[3]), 60.0, 1e-6);

    // SCOPE: the last 512 samples, as the Engine wrote them.
    {
        std::vector<float> scope(512);
        rig.engine().scope(false, scope.data(), 512);
        bool same = true;
        for (std::size_t i = 0; i < 512; ++i)
        {
            const std::size_t at = second - 512 + i;
            same = same && scope[i] == 0.5f * (out.l[at] + out.r[at]);
        }
        P.eq("scope.output_bits", same ? 1 : 0, 1);
        if constexpr (!kInstrument)
        {
            rig.engine().scope(true, scope.data(), 512);
            bool sameIn = true;
            for (std::size_t i = 0; i < 512; ++i)
            {
                const std::size_t at = second - 512 + i;
                sameIn = sameIn && scope[i] == 0.5f * (in.l[at] + in.r[at]);
            }
            P.eq("scope.input_bits", sameIn ? 1 : 0, 1);
        }
    }

    // VOICE (an instrument): a second note while the first sounds, then the second's voice ends.
    if constexpr (kInstrument)
    {
        std::vector<probe::Note> two{ { 0, pg::EventKind::noteOn, 64, 0.8f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, two);
        rig.engine().meters(m);
        P.near("voice.newest", static_cast<double>(m.taps[3]), 64.0, 1e-6);
        std::vector<probe::Note> off{ { 0, pg::EventKind::noteOff, 64, 0.0f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, off);
        rig.engine().meters(m);
        P.near("voice.then_the_one_left", static_cast<double>(m.taps[3]), 60.0, 1e-6);
        std::vector<probe::Note> all{ { 0, pg::EventKind::noteOff, 60, 0.0f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, all);
        rig.engine().meters(m);
        P.near("voice.none_left", static_cast<double>(m.taps[3]), 0.0, 1e-6);
    }

    // FALL: half a second of silence (an instrument's voices have ended above).
    {
        pg::Meters before = m;
        rig.engine().meters(before);
        const probe::Stereo quiet(second / 2);
        rig.run(quiet, kBlock);
        rig.engine().meters(m);
        const double was = static_cast<double>(before.outPeak[0]);
        std::printf("the output's peak: %.4f, half a second later %.4f\n", was, static_cast<double>(m.outPeak[0]));
        if (was > 0.0)
            P.near("fall.peak_10db_in_half_a_second", 20.0 * std::log10(static_cast<double>(m.outPeak[0]) / was), -10.0, 0.3);
        if (before.outRms[0] > 0.0f)
            P.le("fall.rms_40db_in_half_a_second", 20.0 * std::log10(std::max(1e-12, static_cast<double>(m.outRms[0]))
                                                                     / static_cast<double>(before.outRms[0])), -40.0);
    }

    // PROGRAM
    {
        rig.engine().meters(m);
        P.eq("program.the_one_playing", m.program == rig.program() && m.program != 0 ? 1 : 0, 1);
        pg::Patch::Result next = pg::Patch::compile(source, static_cast<float>(kFs));
        P.eq("program.next_compiles", next.patch != nullptr ? 1 : 0, 1);
        if (next.patch)
        {
            const uint64_t id = rig.program() + 100;
            next.patch->setProgram(id);
            rig.engine().offer(std::move(next.patch));
            rig.engine().meters(m);
            P.eq("program.not_one_offered", m.program == rig.program() ? 1 : 0, 1);
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock)), kBlock);
            rig.engine().meters(m);
            P.eq("program.then_the_one_taken", m.program == id ? 1 : 0, 1);
        }
    }

    // READERS: the sound again (an instrument's a note held), with no one reading the meters, then with a reader back.
    {
        const std::size_t len = static_cast<std::size_t>(kBlock) * 8;
        probe::Stereo loud(len);
        if constexpr (!kInstrument)
            loud.l = loud.r = probe::sine(1000.0, 0.8, kFs, len);
        std::vector<probe::Note> held;
        if constexpr (kInstrument)
            held.push_back({ 0, pg::EventKind::noteOn, 67, 0.8f });
        rig.run(loud, kBlock, held);
        std::vector<float> before(512), after(512);
        rig.engine().scope(false, before.data(), 512);
        rig.engine().removeReader();
        P.eq("readers.none", rig.engine().readers(), 0);
        pg::Meters was;
        rig.engine().meters(was);
        rig.run(loud, kBlock);
        rig.engine().meters(m);
        P.eq("readers.none_levels_0", m.outPeak[0] == 0.0f && m.outRms[1] == 0.0f && m.inPeak[0] == 0.0f ? 1 : 0, 1);
        rig.engine().scope(false, after.data(), 512);
        P.eq("readers.none_scope_still", before == after ? 1 : 0, 1);
        P.eq("readers.none_frames_go_on", static_cast<int64_t>(m.frame - was.frame), 8);
        P.eq("readers.none_fields_go_on", m.taps[1] == 7.0f && m.program == was.program ? 1 : 0, 1);
        rig.engine().addReader();
        probe::Stereo one(static_cast<std::size_t>(kBlock));
        std::copy_n(loud.l.begin(), kBlock, one.l.begin());
        std::copy_n(loud.r.begin(), kBlock, one.r.begin());
        const probe::Stereo block = rig.run(one, kBlock);
        rig.engine().meters(m);
        // The block's own peak and RMS (not smoothed up from 0, which would read a third of it).
        const auto own = [](const std::vector<float>& x, double& peak, double& rms) {
            double e = 0.0;
            peak = 0.0;
            for (const float v : x)
            {
                peak = std::max(peak, std::fabs(static_cast<double>(v)));
                e += static_cast<double>(v) * static_cast<double>(v);
            }
            rms = std::sqrt(e / static_cast<double>(x.size()));
        };
        double peak = 0.0, rms = 0.0;
        own(block.l, peak, rms);
        P.near("readers.back_out_peak", static_cast<double>(m.outPeak[0]), peak, 1e-6);
        P.near("readers.back_out_rms", static_cast<double>(m.outRms[0]), rms, 1e-4 * rms);
        P.ge("readers.back_out_audible", rms, 0.1);
        if constexpr (!kInstrument)
        {
            own(one.l, peak, rms);
            P.near("readers.back_in_peak", static_cast<double>(m.inPeak[0]), peak, 1e-6);
            P.near("readers.back_in_rms", static_cast<double>(m.inRms[0]), rms, 1e-4 * rms);
        }
        rig.engine().scope(false, after.data(), 512);
        bool silentBefore = true, theBlock = true;
        for (std::size_t i = 0; i < 512 - static_cast<std::size_t>(kBlock); ++i)
            silentBefore = silentBefore && after[i] == 0.0f;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kBlock); ++i)
            theBlock = theBlock && after[512 - static_cast<std::size_t>(kBlock) + i] == 0.5f * (block.l[i] + block.r[i]);
        P.eq("readers.back_scope_silence_then_the_block", silentBefore && theBlock ? 1 : 0, 1);
    }
    return P.finish();
}
