// PROBE name=dsp.swap timeout=120
//
// dsp.swap: the Engine (Engine.h) at 48 kHz in blocks of 256, with programs written for each row.
// An effect:
//   NONE    with no program the input passes through, bit for bit
//   FIRST   the first program fades in from the input over 20 ms: the input before the offer, the program's output
//           (the input at half) from 20 ms after it, bit for bit; no step between neighbouring samples larger than
//           the input's own steepest (a 440 Hz sine at -6 dBFS)
//   SWAP    a second program (the input at a quarter) takes over the same way, the two crossfaded; afterwards the old
//           one is handed back (collect() destroys it) and no swap is under way
// An instrument:
//   SWAP    a held note's voice fades out over 20 ms when another program arrives, which starts the key again: from
//           20 ms after the offer the output is the new program's alone
//   STEAL   seventeen held notes: sixteen sound, the seventeenth steals the oldest, which fades out over 2 ms
//   RELEASE a voice whose process returns false (here: once its key is up) is free again
// Both:
//   GLIDE   a real dial moved from 0 to 1 reaches the program as a stream that glides there in 20 ms, linearly,
//           sample by sample: half way at 10 ms, exactly 1 from 20 ms, never stepping back
//   KNOB    an integer dial reaches the program once a block, unsmoothed: the block after the move reads the new value
//   GUARD   a program that writes a NaN, an infinity or 10 leaves the Engine as 0, 0 and 4, and raises the alerts
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A program whose output (an effect's, or a held voice's) is `expr`, with `params` in its main module.
    std::string program(const std::string& params, const std::string& expr)
    {
        if constexpr (kInstrument)
            return "module Voice\n{\n" + params +
                   "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float x = 1;\n"
                   "            left[i] = " + expr + ";\n            right[i] = " + expr + ";\n        }\n"
                   "        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n" + params +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float x = left[i];\n"
                   "            left[i] = " + expr + ";\n            right[i] = " + expr + ";\n        }\n    }\n};\n";
    }

    std::unique_ptr<pg::Patch> compile(const std::string& source, float rate)
    {
        pg::Patch::Result r = pg::Patch::compile(source, rate);
        if (!r.errors.empty())
            std::printf("%s", r.errors.c_str());
        return std::move(r.patch);
    }

    probe::Stereo sineInput(std::size_t n)
    {
        probe::Stereo s(n);
        s.l = probe::sine(440.0, 0.5, kFs, n);
        s.r = s.l;
        return s;
    }
}

PROBE(dsp, swap)
{
    const std::size_t fade = static_cast<std::size_t>(0.020 * kFs);
    const probe::Note held{ 0, pg::EventKind::noteOn, 60, 1.0f };

    if constexpr (!kInstrument)
    {
        const std::size_t n = static_cast<std::size_t>(kFs);
        const probe::Stereo in = sineInput(n);
        const double steep = probe::maxStep(in.l, 0, n);

        // NONE
        probe::Rig none(kFs, 512);
        const probe::Stereo dry = none.run(in, 256);
        P.eq("none.passes", probe::sameBits(dry.l, in.l) && probe::sameBits(dry.r, in.r) ? 1 : 0, 1);

        // FIRST and SWAP: offers at blocks 40 and 120 (samples 10240 and 30720).
        probe::Rig rig(kFs, 512);
        pg::Engine& e = rig.engine();
        auto half = compile(program("", "0.5f * x"), e.rate());
        auto quarter = compile(program("", "0.25f * x"), e.rate());
        P.eq("programs.compile", half && quarter ? 1 : 0, 1);
        const std::size_t first = 40 * 256, second = 120 * 256;
        const probe::Stereo out = rig.run(in, 256, {}, {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>&) {
            if (pos == first)
                e.offer(std::move(half));
            if (pos == second)
            {
                P.eq("first.done_before_swap", e.swapping() ? 0 : 1, 1);
                e.offer(std::move(quarter));
            }
        });
        bool before = true, afterFirst = true, afterSecond = true;
        for (std::size_t i = 0; i < first; ++i)
            before = before && out.l[i] == in.l[i];
        for (std::size_t i = first + fade + 1; i < second; ++i)
            afterFirst = afterFirst && out.l[i] == 0.5f * in.l[i];
        for (std::size_t i = second + fade + 1; i < n; ++i)
            afterSecond = afterSecond && out.l[i] == 0.25f * in.l[i];
        P.eq("first.input_before", before ? 1 : 0, 1);
        P.eq("first.program_after_20ms", afterFirst ? 1 : 0, 1);
        P.eq("swap.program_after_20ms", afterSecond ? 1 : 0, 1);
        P.le("first.max_step", probe::maxStep(out.l, first - 8, fade + 16), steep * 1.0001);
        P.le("swap.max_step", probe::maxStep(out.l, second - 8, fade + 16), steep * 1.0001);
        e.collect();
        P.eq("swap.over", e.swapping() ? 0 : 1, 1);
    }
    else
    {
        // SWAP: a held note on a program of 1s, then one of 0.5s.
        probe::Rig rig(kFs, 512);
        rig.load(program("", "1.0f"));
        pg::Engine& e = rig.engine();
        auto next = compile(program("", "0.5f"), e.rate());
        const std::size_t offer = 20 * 256;
        const probe::Stereo out = rig.run(probe::Stereo(static_cast<std::size_t>(0.25 * kFs)), 256, { held }, {},
                                          [&](std::size_t pos, std::array<float, pg::kMaxControls>&) {
                                              if (pos == offer)
                                                  e.offer(std::move(next));
                                          });
        P.near("swap.before", static_cast<double>(out.l[offer - 1]), 1.0, 0.0);
        bool after = true;
        for (std::size_t i = offer + fade + 1; i < out.size(); ++i)
            after = after && out.l[i] == 0.5f;
        P.eq("swap.new_alone_after_20ms", after ? 1 : 0, 1);
        P.eq("swap.one_voice", e.activeVoices(), 1);

        // STEAL: seventeen held notes of 1/16 each.
        probe::Rig steal(kFs, 512);
        steal.load(program("", "0.0625f"));
        std::vector<probe::Note> notes;
        for (int k = 0; k < 17; ++k)
            notes.push_back({ static_cast<std::size_t>(k) * 256, pg::EventKind::noteOn, 40 + k, 1.0f });
        const probe::Stereo s = steal.run(probe::Stereo(32 * 256), 256, notes);
        P.near("steal.sixteen_sound_db", static_cast<double>(s.l.back()), 1.0, 1e-6);
        P.eq("steal.voices", steal.engine().activeVoices(), 16);

        // RELEASE
        probe::Rig rel(kFs, 512);
        rel.load(program("", "1.0f"));
        rel.run(probe::Stereo(4 * 256), 256,
                { held, { 2 * 256, pg::EventKind::noteOff, 60, 0.0f } });
        P.eq("release.voice_free", rel.engine().activeVoices(), 0);
    }

    // GLIDE: a real dial from 0 to 1 at block 8.
    {
        probe::Rig rig(kFs, 512);
        rig.load(program("    param float g [[fminus::range(0, 1)]] [[fminus::default(0)]];\n", "g[i]"));
        const std::size_t move = 8 * 256;
        const probe::Stereo out = rig.run(sineInput(32 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                 : std::vector<probe::Note>{},
                                          {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                                              norm[0] = pos >= move ? 1.0f : 0.0f;
                                          });
        P.near("glide.before", static_cast<double>(out.l[move - 1]), 0.0, 0.0);
        P.near("glide.half_at_10ms", static_cast<double>(out.l[move + fade / 2 - 1]), 0.5, 1e-3);
        P.near("glide.one_at_20ms", static_cast<double>(out.l[move + fade - 1]), 1.0, 0.0);
        bool monotonic = true;
        for (std::size_t i = move; i < move + fade + 8; ++i)
            monotonic = monotonic && out.l[i] >= out.l[i - 1];
        P.eq("glide.monotonic", monotonic ? 1 : 0, 1);
        P.le("glide.max_step", probe::maxStep(out.l, move - 1, fade + 2), 1.0 / static_cast<double>(fade) * 1.01);
    }

    // KNOB: an integer dial over 0..4 from 0 to 4 at block 8.
    {
        probe::Rig rig(kFs, 512);
        rig.load(program("    param int k [[fminus::range(0, 4)]] [[fminus::default(0)]];\n", "(float)k[i]"));
        const std::size_t move = 8 * 256;
        const probe::Stereo out = rig.run(sineInput(16 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                 : std::vector<probe::Note>{},
                                          {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                                              norm[0] = pos >= move ? 1.0f : 0.0f;
                                          });
        P.near("knob.before", static_cast<double>(out.l[move - 1]), 0.0, 0.0);
        P.near("knob.at_the_block", static_cast<double>(out.l[move]), 4.0, 0.0);
    }

    // GUARD: a NaN, an infinity, 10.
    {
        int index = 0;
        for (const char* expr : { "0.0f / 0.0f", "1.0f / 0.0f", "10.0f" })
        {
            const std::string key = "guard" + std::to_string(++index);
            probe::Rig rig(kFs, 512);
            const bool loaded = rig.load(program("", expr));
            P.eq(key + ".compiles", loaded ? 1 : 0, 1);
            const probe::Stereo out = rig.run(sineInput(4 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                     : std::vector<probe::Note>{});
            const uint32_t alerts = rig.engine().takeAlerts();
            const double want = index == 3 ? 4.0 : 0.0;
            P.near(key + ".output", static_cast<double>(out.l[300]), want, 0.0);
            P.eq(key + ".finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
            P.eq(key + ".alert", (alerts & (index == 3 ? pg::Engine::clipped : pg::Engine::nonFinite)) != 0 ? 1 : 0, 1);
        }
    }
    return P.finish();
}
