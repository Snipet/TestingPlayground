// PROBE name=dsp.freeze timeout=180
//
// dsp.freeze: frozen dials (Patch.h, Engine.h): the program compiled again with its dials as constants, which runs the
// same instances. Two Engines play the same program (a filter whose coefficients a voice computes each call, a level,
// an int's choice of wave and a bool), one of them frozen, at 48 kHz in blocks of 256:
//   STILL    dials() says the program, how long the dials have held still (from the program's arrival to the last
//            block's start), their values, and that nothing is frozen yet
//   FROZEN   the copy offered at the dials as they stand plays from the next block; the output is the same, bit for
//            bit, as the unfrozen Engine's; dials() says frozen
//   MOVE     a dial moved (in both): the copy stops playing in that block, the output still the same bits; the dials
//            count as still again from the end of its 20 ms glide (the start of the last block it moved in)
//   AGAIN    a copy at the new values plays, the same bits
//   REFUSED  a copy at other values than the dials' is not taken; nor a copy of another program
//   SPEED    sixteen voices for two seconds, live and frozen: the time each takes (printed)
//   EXAMPLES each example, frozen at its defaults (an instrument's with eight voices): it plays, the same bits as
//            unfrozen; the time each takes (printed)
#include "Probe.h"

#include "Rig.h"

#include "fminus/Patch.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;
    constexpr double kFs = 48000.0;

    // The dials, in order: cutoff (Hz), level (dB), shape (an int: sine, saw, square), bright (a bool: the cutoff
    // doubled). The filter's coefficients come from the dials once a call: what freezing folds.
    const char* kParams = "    param float cutoff [[fminus::range(100, 8000)]] [[fminus::default(1200)]];\n"
                          "    param float level [[fminus::range(-36, 0)]] [[fminus::default(-12)]];\n"
                          "    param int shape [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                          "    param bool bright [[fminus::default(1)]];\n";

    const char* kCoefficients = "        const float fc = cutoff[0] * (bright[0] ? 2.0f : 1.0f);\n"
                                "        const float w = 3.14159265f * fminf(fc, 0.45f * rate) / rate;\n"
                                "        const float g = sinf(w) / cosf(w);\n"
                                "        const float a1 = 1 / (1 + g * (g + 0.7f));\n"
                                "        const float a2 = g * a1;\n"
                                "        const float a3 = g * a2;\n"
                                "        const float gain = expf(0.115129255f * level[0]);\n";

    const char* kFilter = "            const float v3 = x - ic2;\n"
                          "            const float v1 = a1 * ic1 + a2 * v3;\n"
                          "            const float v2 = ic2 + a2 * ic1 + a3 * v3;\n"
                          "            ic1 = 2 * v1 - ic1;\n"
                          "            ic2 = 2 * v2 - ic2;\n";

    std::string program()
    {
        if constexpr (kInstrument)
            return std::string("module Voice\n{\n") + kParams +
                   "    float rate, ph, step, ic1, ic2, env;\n    bool held;\n"
                   "    void init(float r) { rate = r; }\n"
                   "    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 1; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + kCoefficients +
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            float x = 2 * ph - 1;\n"
                   "            if (shape[0] == 0) x = sinf(6.28318531f * ph);\n"
                   "            if (shape[0] == 2) x = ph < 0.5f ? 1.0f : -1.0f;\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n" + kFilter +
                   "            if (!held) env *= 0.9995f;\n"
                   "            const float out = gain * env * v2;\n"
                   "            left[i] += out;\n            right[i] += out;\n        }\n"
                   "        return held || env > 0.0001f;\n    }\n};\n";
        else
            return std::string("module Effect\n{\n") + kParams +
                   "    float rate, ic1, ic2;\n"
                   "    void init(float r) { rate = r; }\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + kCoefficients +
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            float x = 0.5f * (left[i] + right[i]);\n"
                   "            if (shape[0] == 0) x = sinf(x);\n"
                   "            if (shape[0] == 2) x = x > 0 ? 0.5f : -0.5f;\n" + kFilter +
                   "            left[i] = gain * v2;\n            right[i] = gain * v2;\n        }\n    }\n};\n";
    }

    bool same(const probe::Stereo& a, const probe::Stereo& b) { return probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r); }

    probe::Stereo input(std::size_t n, uint32_t seed)
    {
        if constexpr (kInstrument)
            return probe::Stereo(n);
        else
            return probe::noise(n, 0.25, seed);
    }

    std::vector<probe::Note> chord(int voices)
    {
        std::vector<probe::Note> notes;
        if constexpr (kInstrument)
            for (int k = 0; k < voices; ++k)
                notes.push_back({ 0, tp::EventKind::noteOn, 40 + 3 * k, 0.8f });
        return notes;
    }
}

PROBE(dsp, freeze)
{
    const std::size_t second = static_cast<std::size_t>(kFs);
    probe::Rig live(kFs, 256), frozen(kFs, 256);
    std::string messages;
    P.eq("load.compiles", live.load(program(), &messages) && frozen.load(program()) ? 1 : 0, 1);
    if (!messages.empty())
        std::printf("%s", messages.c_str());

    // ---- STILL ---------------------------------------------------------------------------------------------------------
    const probe::Stereo a1 = live.run(input(second, 1), 256, chord(4)), b1 = frozen.run(input(second, 1), 256, chord(4));
    tp::DialState d;
    frozen.engine().dials(d);
    P.eq("still.program", static_cast<int64_t>(d.program), 1);
    // 48000 samples in blocks of 256: the last block, of 128, starts at 47872.
    P.near("still.seconds", d.stillSeconds, (static_cast<double>(second) - static_cast<double>(second % 256)) / kFs, 1e-6);
    P.eq("still.values", d.plain[0] == 1200.0f && d.plain[1] == -12.0f && d.plain[2] == 1.0f && d.plain[3] == 1.0f ? 1 : 0, 1);
    P.eq("still.not_frozen", !d.frozen && !frozen.engine().frozen() ? 1 : 0, 1);
    P.eq("still.same_before", same(a1, b1) ? 1 : 0, 1);

    // ---- FROZEN --------------------------------------------------------------------------------------------------------
    std::string why;
    P.eq("frozen.compiles", frozen.freeze(&why) ? 1 : 0, 1);
    if (!why.empty())
        std::printf("freeze: %s\n", why.c_str());
    const probe::Stereo a2 = live.run(input(second, 2), 256), b2 = frozen.run(input(second, 2), 256);
    P.eq("frozen.plays", frozen.engine().frozen() ? 1 : 0, 1);
    frozen.engine().dials(d);
    P.eq("frozen.dials_say", d.frozen ? 1 : 0, 1);
    P.eq("frozen.same_bits", same(a2, b2) ? 1 : 0, 1);
    P.ge("frozen.audible", probe::rmsDb(b2.l, 0, second), -60.0);

    // ---- MOVE ----------------------------------------------------------------------------------------------------------
    for (probe::Rig* r : { &live, &frozen })
    {
        r->norm()[0] = 0.25f;                                // cutoff
        r->norm()[2] = 0.0f;                                 // shape: the sine
    }
    const probe::Stereo a3 = live.run(input(second / 2, 3), 256), b3 = frozen.run(input(second / 2, 3), 256);
    P.eq("move.dropped", frozen.engine().frozen() ? 0 : 1, 1);
    P.eq("move.same_bits", same(a3, b3) ? 1 : 0, 1);
    frozen.engine().dials(d);
    // 24000 samples: the last block (192) starts at 23808; the glide's 960 samples end in the fourth block (at 768).
    P.near("move.still_from_the_glide", d.stillSeconds, (23808.0 - 768.0) / kFs, 1e-6);

    // ---- AGAIN ---------------------------------------------------------------------------------------------------------
    P.eq("again.compiles", frozen.freeze() ? 1 : 0, 1);
    const probe::Stereo a4 = live.run(input(second / 2, 4), 256), b4 = frozen.run(input(second / 2, 4), 256);
    P.eq("again.plays", frozen.engine().frozen() ? 1 : 0, 1);
    P.eq("again.same_bits", same(a4, b4) ? 1 : 0, 1);

    // ---- REFUSED -------------------------------------------------------------------------------------------------------
    {
        frozen.norm()[1] = 0.5f;                             // level: the copy goes
        frozen.run(input(1024, 5), 256);
        frozen.engine().dials(d);
        std::array<float, tp::kMaxControls> other = d.plain;
        other[1] += 1.0f;                                    // not the dial's value
        std::string w;
        std::unique_ptr<tp::Patch::Frozen> f = tp::Patch::freeze(frozen.recipe(), other, w);
        P.eq("refused.compiles", f != nullptr ? 1 : 0, 1);
        frozen.engine().offerFrozen(std::move(f));
        frozen.run(input(1024, 6), 256);
        P.eq("refused.other_values", frozen.engine().frozen() ? 0 : 1, 1);
        frozen.engine().collect();
        // A copy of the program before another came.
        const tp::Patch::Recipe before = frozen.recipe();
        frozen.load(program());                              // program 2
        frozen.run(input(1024, 7), 256);
        frozen.engine().dials(d);
        std::unique_ptr<tp::Patch::Frozen> old = tp::Patch::freeze(before, d.plain, w);
        frozen.engine().offerFrozen(std::move(old));
        frozen.run(input(1024, 8), 256);
        P.eq("refused.other_program", frozen.engine().frozen() ? 0 : 1, 1);
        frozen.engine().collect();
    }

    // ---- SPEED ---------------------------------------------------------------------------------------------------------
    {
        const int voices = kInstrument ? 16 : 1;
        probe::Rig slow(kFs, 256), fast(kFs, 256);
        slow.load(program());
        fast.load(program());
        slow.run(input(4096, 9), 256, chord(voices));
        fast.run(input(4096, 9), 256, chord(voices));
        fast.freeze();
        fast.run(input(256, 10), 256);
        P.eq("speed.frozen", fast.engine().frozen() ? 1 : 0, 1);
        const auto time = [&](probe::Rig& r) {
            double best = 1e9;
            for (int k = 0; k < 3; ++k)
            {
                const probe::Stereo in = input(2 * second, 11);
                const auto t0 = std::chrono::steady_clock::now();
                r.run(in, 256);
                best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            return best;
        };
        const double liveMs = time(slow), frozenMs = time(fast);
        std::printf("speed: %d voice(s), 2 s: live %.2f ms, frozen %.2f ms (%.0f%%)\n", voices, liveMs, frozenMs,
                    100.0 * frozenMs / std::max(liveMs, 1e-9));
    }
    // ---- EXAMPLES ------------------------------------------------------------------------------------------------------
    for (const tp::Example& e : tp::kExamples)
    {
        probe::Rig a(kFs, 256), b(kFs, 256);
        if (!a.load(e.source) || !b.load(e.source))
            continue;
        const int voices = kInstrument ? 8 : 1;
        a.run(input(4096, 12), 256, chord(voices));
        b.run(input(4096, 12), 256, chord(voices));
        b.freeze();
        const probe::Stereo in = input(2 * second, 13);
        const auto t0 = std::chrono::steady_clock::now();
        const probe::Stereo oa = a.run(in, 256);
        const auto t1 = std::chrono::steady_clock::now();
        const probe::Stereo ob = b.run(in, 256);
        const auto t2 = std::chrono::steady_clock::now();
        const double la = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double lb = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("example %-16s live %7.2f ms, frozen %7.2f ms (%3.0f%%)%s\n", e.title, la, lb, 100.0 * lb / std::max(la, 1e-9),
                    same(oa, ob) ? "" : "  BITS DIFFER");
        std::string key = std::string("examples.") + e.title;
        for (char& ch : key)
            ch = ch == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        P.eq(key + "_frozen", b.engine().frozen() ? 1 : 0, 1);
        P.eq(key + "_same_bits", same(oa, ob) ? 1 : 0, 1);
    }
    return P.finish();
}
