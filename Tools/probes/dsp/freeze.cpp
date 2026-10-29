// PROBE name=dsp.freeze timeout=300
//
// dsp.freeze: frozen dials (Patch.h, Engine.h): the program compiled again with its dials as constants, in the
// program's own math (FMinus's Math::Vector), which runs the same instances. Two Engines play the same program (a
// filter whose coefficients a voice computes each call, a level, an int's choice of wave and a bool), one of them
// frozen, at 48 kHz in blocks of 256. FMinus's kernels give a float one result wherever it is computed, from a frozen
// parameter or a knob, so the frozen copy plays the program's very bits, and that is what this checks; beside it, a
// copy in FMinus's Math::Fast (Patch::freeze's fastMath, the plugin's copy until FMinus c75caab) is measured, never
// asserted: its time, and its sound against the program's ("the difference": its energy under the signal's; "bands":
// each octave band's level, 63 Hz to 16 kHz, every half second, the largest difference):
//   STILL    dials() says the program, how long the dials have held still (from the program's arrival to the last
//            block's start), their values, and that nothing is frozen yet; the same bits while nothing is
//   FROZEN   the copy offered at the dials as they stand plays from the next block; the same bits as the unfrozen
//            Engine's; dials() says frozen
//   MOVE     a dial moved (in both): the copy stops playing in that block, the same bits still; the dials count as
//            still again from the end of its 20 ms glide (the start of the last block it moved in)
//   AGAIN    a copy at the new values (the sine: sinf a sample at a time) plays, the same bits
//   REFUSED  a copy at other values than the dials' is not taken; nor a copy of another program
//   BLOCKS   the frozen copy in blocks of 64 and of 333: the same bits as each other and as the unfrozen program
//   SPEED    sixteen voices for two seconds, live and frozen: the time each takes (printed)
//   EXAMPLES each example, frozen at its defaults (an instrument's with eight voices): it plays, the same bits as
//            unfrozen; the time each takes (the best of five), and the fast copy's time and differences (printed)
//   LONG     each example and RANDOM's programs of seeds 1 to kLongSeeds, ten seconds (an instrument's chord held six
//            and released; an effect's noise and a sine), the frozen copy in blocks of irregular sizes against the
//            unfrozen program in blocks of 256: it plays, finite, the same bits; the time it takes against the
//            program's, and the programs more than 5% slower frozen, by name (printed); the same for the fast copy,
//            and its sound's worst (the difference, the bands, every half second's level, the peak: printed)
#include "Probe.h"

#include "Rig.h"

#include "fminus/Patch.h"

#include "fminus/Random.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
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

    // A copy's output against the program's: their difference's energy against the signal's, in dB, over
    // [from, from + count) of both channels (-300 when they are the same bits; +300 when only the difference sounds).
    constexpr int kLongSeeds = 64;

    double differenceDb(const probe::Stereo& a, const probe::Stereo& b, std::size_t from = 0, std::size_t count = ~std::size_t{ 0 })
    {
        double signal = 0.0, diff = 0.0;
        const std::size_t end = std::min({ a.size(), b.size(), count == ~std::size_t{ 0 } ? a.size() : from + count });
        for (std::size_t i = from; i < end; ++i)
            for (const auto* ch : { &a.l, &a.r })
            {
                const std::vector<float>& other = ch == &a.l ? b.l : b.r;
                const double x = static_cast<double>((*ch)[i]), y = static_cast<double>(other[i]);
                signal += x * x;
                diff += (x - y) * (x - y);
            }
        if (diff == 0.0)
            return -300.0;
        if (signal == 0.0)
            return 300.0;
        return 10.0 * std::log10(diff / signal);
    }

    bool finite(const probe::Stereo& s) { return probe::allFinite(s.l) && probe::allFinite(s.r); }

    // The program's very bits, and finite.
    bool sameBits(const probe::Stereo& a, const probe::Stereo& b) { return finite(a) && finite(b) && same(a, b); }

    // The largest difference, in dB, between a's and b's level in an octave band (63 Hz to 16 kHz) over a half
    // second, of each channel, where a's is above -80 dBFS in that band.
    double bandsDb(const probe::Stereo& a, const probe::Stereo& b)
    {
        const std::size_t window = static_cast<std::size_t>(kFs / 2.0);
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            const std::vector<float>& fa = ch == 0 ? a.l : a.r;
            const std::vector<float>& fb = ch == 0 ? b.l : b.r;
            const std::vector<double> da(fa.begin(), fa.end()), db(fb.begin(), fb.end());
            for (double hz = 62.5; hz < 17000.0; hz *= 2.0)
            {
                const std::vector<double> ba = probe::bandPass(da, hz, 1.41, kFs), bb = probe::bandPass(db, hz, 1.41, kFs);
                for (std::size_t w = 0; w + window <= ba.size(); w += window)
                {
                    double ea = 0.0, eb = 0.0;
                    for (std::size_t i = w; i < w + window; ++i)
                    {
                        ea += ba[i] * ba[i];
                        eb += bb[i] * bb[i];
                    }
                    ea /= static_cast<double>(window);
                    eb /= static_cast<double>(window);
                    if (ea > 1e-8)                           // -80 dBFS
                        worst = std::max(worst, std::fabs(10.0 * std::log10(std::max(eb, 1e-30) / ea)));
                }
            }
        }
        return worst;
    }

    double stereoRmsDb(const probe::Stereo& s, std::size_t from, std::size_t count)
    {
        double e = 0.0;
        const std::size_t end = std::min(s.size(), from + count);
        for (std::size_t i = from; i < end; ++i)
            e += static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i];
        return probe::dbFromLin(std::sqrt(e / static_cast<double>(2 * std::max<std::size_t>(end - from, 1))));
    }

    double peakDb(const probe::Stereo& s)
    {
        return probe::dbFromLin(std::max(static_cast<double>(probe::peak(s.l)), static_cast<double>(probe::peak(s.r))));
    }

    pg::Sources filesOf(const pg::random::Program& p)
    {
        pg::Sources s{ { pg::kProgramName, p.text } };
        for (const std::string& name : p.library)
            if (const pg::LibraryFile* f = pg::findLibraryFile(name))
                s.push_back({ name, f->source });
        return s;
    }

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
                notes.push_back({ 0, pg::EventKind::noteOn, 40 + 3 * k, 0.8f });
        return notes;
    }

    // The time each rig takes to play `in` in blocks of 256, the best of `tries` (taken in turn); an instrument's
    // notes struck again at the start of each.
    std::vector<double> timeEach(const std::vector<probe::Rig*>& rigs, const probe::Stereo& in,
                                 const std::vector<probe::Note>& notes, int tries)
    {
        std::vector<double> best(rigs.size(), 1e9);
        for (int k = 0; k < tries; ++k)
            for (std::size_t r = 0; r < rigs.size(); ++r)
            {
                const auto t0 = std::chrono::steady_clock::now();
                rigs[r]->run(in, 256, notes);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                best[r] = std::min(best[r], ms);
            }
        return best;
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
    pg::DialState d;
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
    std::printf("frozen: the difference %.1f dB\n", differenceDb(a2, b2));
    P.eq("frozen.same_bits", sameBits(a2, b2) ? 1 : 0, 1);
    P.ge("frozen.audible", probe::rmsDb(b2.l, 0, second), -60.0);

    // ---- MOVE ----------------------------------------------------------------------------------------------------------
    for (probe::Rig* r : { &live, &frozen })
    {
        r->norm()[0] = 0.25f;                                // cutoff
        r->norm()[2] = 0.0f;                                 // shape: the sine
    }
    const probe::Stereo a3 = live.run(input(second / 2, 3), 256), b3 = frozen.run(input(second / 2, 3), 256);
    P.eq("move.dropped", frozen.engine().frozen() ? 0 : 1, 1);
    std::printf("move: the difference %.1f dB\n", differenceDb(a3, b3));
    P.eq("move.same_bits", sameBits(a3, b3) ? 1 : 0, 1);
    frozen.engine().dials(d);
    // 24000 samples: the last block (192) starts at 23808; the glide's 960 samples end in the fourth block (at 768).
    P.near("move.still_from_the_glide", d.stillSeconds, (23808.0 - 768.0) / kFs, 1e-6);

    // ---- AGAIN ---------------------------------------------------------------------------------------------------------
    P.eq("again.compiles", frozen.freeze() ? 1 : 0, 1);
    const probe::Stereo a4 = live.run(input(second / 2, 4), 256), b4 = frozen.run(input(second / 2, 4), 256);
    P.eq("again.plays", frozen.engine().frozen() ? 1 : 0, 1);
    std::printf("again: the difference %.1f dB\n", differenceDb(a4, b4));
    P.eq("again.same_bits", sameBits(a4, b4) ? 1 : 0, 1);

    // ---- REFUSED -------------------------------------------------------------------------------------------------------
    {
        frozen.norm()[1] = 0.5f;                             // level: the copy goes
        frozen.run(input(1024, 5), 256);
        frozen.engine().dials(d);
        std::array<float, pg::kMaxControls> other = d.plain;
        other[1] += 1.0f;                                    // not the dial's value
        std::string w;
        std::unique_ptr<pg::Patch::Frozen> f = pg::Patch::freeze(frozen.recipe(), other, w);
        P.eq("refused.compiles", f != nullptr ? 1 : 0, 1);
        frozen.engine().offerFrozen(std::move(f));
        frozen.run(input(1024, 6), 256);
        P.eq("refused.other_values", frozen.engine().frozen() ? 0 : 1, 1);
        frozen.engine().collect();
        // A copy of the program before another came.
        const pg::Patch::Recipe before = frozen.recipe();
        frozen.load(program());                              // program 2
        frozen.run(input(1024, 7), 256);
        frozen.engine().dials(d);
        std::unique_ptr<pg::Patch::Frozen> old = pg::Patch::freeze(before, d.plain, w);
        frozen.engine().offerFrozen(std::move(old));
        frozen.run(input(1024, 8), 256);
        P.eq("refused.other_program", frozen.engine().frozen() ? 0 : 1, 1);
        frozen.engine().collect();
    }

    // ---- BLOCKS --------------------------------------------------------------------------------------------------------
    {
        probe::Rig plain(kFs, 512), f64(kFs, 512), f333(kFs, 512);
        for (probe::Rig* r : { &plain, &f64, &f333 })
        {
            r->load(program());
            r->run(input(4096, 14), 256, chord(4));
        }
        P.eq("blocks.frozen", f64.freeze() && f333.freeze() ? 1 : 0, 1);
        const probe::Stereo in = input(second, 15);
        const probe::Stereo o = plain.run(in, 256), o64 = f64.run(in, 64), o333 = f333.run(in, 333);
        P.eq("blocks.plays", f64.engine().frozen() && f333.engine().frozen() ? 1 : 0, 1);
        std::printf("blocks: 64 against 333 %.1f dB, 64 against unfrozen %.1f dB, 333 against unfrozen %.1f dB\n",
                    differenceDb(o64, o333), differenceDb(o, o64), differenceDb(o, o333));
        P.eq("blocks.64_333_same_bits", sameBits(o64, o333) ? 1 : 0, 1);
        P.eq("blocks.same_bits_as_unfrozen", sameBits(o, o64) && sameBits(o, o333) ? 1 : 0, 1);
    }

    // ---- SPEED ---------------------------------------------------------------------------------------------------------
    {
        const int voices = kInstrument ? 16 : 1;
        probe::Rig own(kFs, 256), copy(kFs, 256);
        own.load(program());
        copy.load(program());
        own.run(input(4096, 9), 256, chord(voices));
        copy.run(input(4096, 9), 256, chord(voices));
        copy.freeze();
        copy.run(input(256, 10), 256);
        P.eq("speed.frozen", copy.engine().frozen() ? 1 : 0, 1);
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
        const double liveMs = time(own), frozenMs = time(copy);
        std::printf("speed: %d voice(s), 2 s: live %.2f ms, frozen %.2f ms (%.0f%%)\n", voices, liveMs, frozenMs,
                    100.0 * frozenMs / std::max(liveMs, 1e-9));
    }
    // ---- EXAMPLES ------------------------------------------------------------------------------------------------------
    for (const pg::Example& e : pg::kExamples)
    {
        probe::Rig a(kFs, 256), b(kFs, 256), c(kFs, 256);   // c: a copy in fast math, measured only
        if (!a.load(e.source) || !b.load(e.source) || !c.load(e.source))
            continue;
        const int voices = kInstrument ? 8 : 1;
        a.run(input(4096, 12), 256, chord(voices));
        b.run(input(4096, 12), 256, chord(voices));
        c.run(input(4096, 12), 256, chord(voices));
        b.freeze();
        c.freeze(nullptr, true);
        const probe::Stereo in = input(2 * second, 13);
        const probe::Stereo oa = a.run(in, 256), ob = b.run(in, 256), oc = c.run(in, 256);
        const std::vector<double> t = timeEach({ &a, &b, &c }, in, chord(voices), 5);
        std::printf("example %-16s live %7.2f ms, frozen %7.2f ms (%3.0f%%), %s; fast math %7.2f ms (%3.0f%%), the "
                    "difference %.1f dB, bands %.4f dB\n",
                    e.title, t[0], t[1], 100.0 * t[1] / std::max(t[0], 1e-9), same(oa, ob) ? "the same bits" : "OTHER BITS",
                    t[2], 100.0 * t[2] / std::max(t[0], 1e-9), differenceDb(oa, oc), bandsDb(oa, oc));
        std::string key = std::string("examples.") + e.title;
        for (char& ch : key)
            ch = ch == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        P.eq(key + "_frozen", b.engine().frozen() ? 1 : 0, 1);
        P.eq(key + "_same_bits", sameBits(oa, ob) ? 1 : 0, 1);
    }

    // ---- LONG ----------------------------------------------------------------------------------------------------------
    {
        struct Case
        {
            std::string name;
            pg::Sources files;
        };
        std::vector<Case> cases;
        for (const pg::Example& e : pg::kExamples)
            cases.push_back({ e.title, pg::singleFile(e.source) });
        for (uint32_t seed = 1; seed <= static_cast<uint32_t>(kLongSeeds); ++seed)
        {
            const pg::random::Program p = pg::random::make(seed);
            cases.push_back({ "random " + std::to_string(seed) + " " + p.title, filesOf(p) });
        }
        const std::size_t length = 10 * second, window = second / 2;
        int loaded = 0, frozenCount = 0, finiteCount = 0, sameCount = 0;
        // The time a copy takes against the program's: the geometric mean, the range, those more than 5% slower.
        struct Times
        {
            double log = 0.0, slowest = 0.0, fastest = 1e9;
            int timed = 0, slower = 0;
            std::string names;
            void add(const std::string& name, double ratio)
            {
                log += std::log(ratio);
                slowest = std::max(slowest, ratio);
                fastest = std::min(fastest, ratio);
                ++timed;
                if (ratio > 1.05)
                {
                    ++slower;
                    char line[160];
                    std::snprintf(line, sizeof line, "%s%s %.0f%%", names.empty() ? "" : ", ", name.c_str(), 100.0 * ratio);
                    names += line;
                }
            }
            void print(const char* what) const
            {
                std::printf("long: %s against live (two seconds, the best of three): %.0f%% (the geometric mean), %.0f%% to "
                            "%.0f%%, %d of %d more than 5%% slower%s%s\n", what, 100.0 * std::exp(log / std::max(timed, 1)),
                            100.0 * fastest, 100.0 * slowest, slower, timed, names.empty() ? "" : ": ", names.c_str());
            }
        } own, fast;
        // The copy in fast math (Math::Fast), measured only: what the plugin would gain or lose with it, and its sound.
        int fastFrozen = 0, fastSame = 0;
        double worst = -300.0, worstLevel = 0.0, worstPeak = 0.0, worstBands = 0.0;
        std::string worstName, worstBandsName;
        for (const Case& c : cases)
        {
            probe::Rig a(kFs, 512), b(kFs, 512), x(kFs, 512);
            if (!a.load(c.files) || !b.load(c.files) || !x.load(c.files))
            {
                std::printf("long: %s does not compile\n", c.name.c_str());
                continue;
            }
            ++loaded;
            std::vector<probe::Note> on, off;
            if constexpr (kInstrument)
                for (const int n : { 48, 55, 60, 64 })
                {
                    on.push_back({ 0, pg::EventKind::noteOn, n, 0.8f });
                    off.push_back({ 6 * second, pg::EventKind::noteOff, n, 0.0f });
                }
            probe::Stereo in(length);
            if constexpr (!kInstrument)
            {
                in = probe::noise(length, 0.2, 16);
                const std::vector<float> sine = probe::sine(220.0, 0.25, kFs, length);
                for (std::size_t i = 0; i < length; ++i)
                {
                    in.l[i] += sine[i];
                    in.r[i] += sine[i];
                }
            }
            a.run(probe::Stereo(1024), 256, on);
            b.run(probe::Stereo(1024), 256, on);
            x.run(probe::Stereo(1024), 256, on);
            if (!b.freeze())
            {
                std::printf("long: %s does not freeze\n", c.name.c_str());
                continue;
            }
            const bool fastOne = x.freeze(nullptr, true);
            const std::vector<int> irregular{ 333, 64, 512, 97, 256, 1 };
            const probe::Stereo oa = a.run(in, 256, off), ob = b.run(in, 256, off, irregular);
            frozenCount += b.engine().frozen() ? 1 : 0;
            const bool fin = finite(oa) && finite(ob);
            finiteCount += fin ? 1 : 0;
            const bool bits = fin && same(oa, ob);
            sameCount += bits ? 1 : 0;
            if (!fin || !bits)
                std::printf("long: %s: finite %d, the difference %.1f dB (not the program's bits)\n", c.name.c_str(),
                            fin ? 1 : 0, differenceDb(oa, ob));
            if (fastOne)
            {
                const probe::Stereo ox = x.run(in, 256, off, irregular);
                fastFrozen += x.engine().frozen() ? 1 : 0;
                fastSame += same(oa, ox) ? 1 : 0;
                double level = 0.0;
                for (std::size_t w = 0; w + window <= length; w += window)
                {
                    const double la = stereoRmsDb(oa, w, window), lx = stereoRmsDb(ox, w, window);
                    if (la > -80.0)
                        level = std::max(level, std::fabs(la - lx));
                }
                const bool fx = finite(oa) && finite(ox);
                const double pk = std::fabs(peakDb(oa) - peakDb(ox));
                const double diff = differenceDb(oa, ox), bands = fx ? bandsDb(oa, ox) : 1e9;
                worstLevel = std::max(worstLevel, level);
                worstPeak = std::max(worstPeak, pk);
                if (diff > worst)
                {
                    worst = diff;
                    worstName = c.name;
                }
                if (bands > worstBands)
                {
                    worstBands = bands;
                    worstBandsName = c.name;
                }
            }
            {
                probe::Stereo slice(2 * second);             // the input's first two seconds
                std::copy_n(in.l.begin(), slice.size(), slice.l.begin());
                std::copy_n(in.r.begin(), slice.size(), slice.r.begin());
                const std::vector<double> t = timeEach(fastOne ? std::vector<probe::Rig*>{ &a, &b, &x } : std::vector<probe::Rig*>{ &a, &b },
                                                       slice, on, 3);
                own.add(c.name, t[1] / std::max(t[0], 1e-9));
                if (fastOne)
                    fast.add(c.name, t[2] / std::max(t[0], 1e-9));
            }
        }
        std::printf("long: %d programs, %d of them the program's bits frozen, in irregular blocks\n", loaded, sameCount);
        own.print("the time frozen");
        fast.print("a copy in fast math (Math::Fast)");
        std::printf("long: the fast copy's sound: %d of %d frozen, %d of them the program's bits; the worst: bands %.4f dB "
                    "(%s), the difference %.1f dB (%s), level %.4f dB, peak %.4f dB\n", fastFrozen, fast.timed, fastSame,
                    worstBands, worstBandsName.c_str(), worst, worstName.c_str(), worstLevel, worstPeak);
        const auto all = static_cast<int64_t>(cases.size());
        P.eq("long.compile", loaded, all);
        P.eq("long.frozen", frozenCount, all);
        P.eq("long.finite", finiteCount, all);
        P.eq("long.same_bits", sameCount, all);
    }
    return P.finish();
}
