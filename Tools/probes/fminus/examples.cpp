// PROBE name=fminus.examples timeout=300
//
// fminus.examples: every program of the editor's EXAMPLES (Source/fminus/examples/<product>/*.fm, the first the
// default) as the plugin compiles and plays it (Patch::compile, the Engine), at 48 kHz:
//   COMPILE  it compiles for this plugin's kind, with no message; its dials are its first parameters, at most 32,
//            each with a name, a range (min < max) and a default inside it
//   PLAY     two seconds at the defaults: an effect over stereo noise at -12 dBFS, an instrument playing a chord
//            (C4, E4, G4 at velocity 0.8, released at 1 s): the output is finite and audible (above -60 dBFS RMS over
//            its first second), and the output guard raised no alert (no NaN, nothing beyond +-4)
//   BLOCKS   the same run in blocks of 64 and of 333 samples gives the same output, bit for bit: every example keeps
//            its state per sample, so the host's block size does not reach the sound
//   END      an instrument's voices end: some time after the release (8 s at most, the longest example's decay) no
//            voice sounds and the output is silent
//   STEAL    an instrument's note on an instance a stolen voice left starts as on a fresh one (eighteen keys held
//            50 ms apart, the first seventeen silent at velocity 0, so the eighteenth steals the second's instance):
//            its first 12 samples are no louder than a fresh note's (x1.5, plus 1% of its held peak), so no note
//            skips its attack
#include "Probe.h"

#include "Rig.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace pg = playground;

PROBE(fminus, examples)
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;
    const std::size_t n = static_cast<std::size_t>(2.0 * kFs);
    const probe::Stereo input = kInstrument ? probe::Stereo(n) : probe::noise(n, 0.25);
    std::vector<probe::Note> chord;
    if constexpr (kInstrument)
        for (const int note : { 60, 64, 67 })
        {
            chord.push_back({ 0, pg::EventKind::noteOn, note, 0.8f });
            chord.push_back({ static_cast<std::size_t>(kFs), pg::EventKind::noteOff, note, 0.0f });
        }
    std::sort(chord.begin(), chord.end(), [](const probe::Note& a, const probe::Note& b) { return a.at < b.at; });

    P.ge("examples.count", static_cast<double>(std::size(pg::kExamples)), 1.0);
    int index = 0;
    for (const pg::Example& example : pg::kExamples)
    {
        const std::string key = "example" + std::to_string(++index);
        std::printf("%s: %s\n", key.c_str(), example.title);

        // COMPILE
        probe::Rig rig(kFs, 512);
        std::string messages;
        const bool ok = rig.load(example.source, &messages);
        if (!messages.empty())
            std::printf("%s", messages.c_str());
        P.eq(key + ".compiles", ok ? 1 : 0, 1);
        P.eq(key + ".no_messages", messages.empty() ? 1 : 0, 1);
        if (!ok)
            continue;
        for (int c = 0; c < pg::kMaxControls; ++c)
        {
            const pg::Control& ctl = rig.controls()[static_cast<std::size_t>(c)];
            if (!ctl.used)
                continue;
            const std::string ck = key + ".dial" + std::to_string(c + 1);
            std::printf("  dial %d: %s [%g, %g] default %g\n", c + 1, ctl.name, static_cast<double>(ctl.min),
                        static_cast<double>(ctl.max), static_cast<double>(ctl.def));
            P.eq(ck + ".named", ctl.name[0] != '\0' ? 1 : 0, 1);
            P.eq(ck + ".range", ctl.max > ctl.min ? 1 : 0, 1);
            P.in(ck + ".default_in_range", static_cast<double>(ctl.def), static_cast<double>(ctl.min),
                 static_cast<double>(ctl.max));
        }

        // PLAY
        const probe::Stereo out = rig.run(input, 256, chord);
        P.eq(key + ".finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
        const double level = std::max(probe::rmsDb(out.l, 0, static_cast<std::size_t>(kFs)),
                                      probe::rmsDb(out.r, 0, static_cast<std::size_t>(kFs)));
        std::printf("  level %.1f dBFS RMS over the first second\n", level);
        P.ge(key + ".audible_db", level, -60.0);
        P.eq(key + ".no_alerts", static_cast<int64_t>(rig.engine().takeAlerts()), 0);

        // BLOCKS
        probe::Rig a(kFs, 512), b(kFs, 512);
        a.load(example.source);
        b.load(example.source);
        const probe::Stereo in64 = a.run(input, 64, chord);
        const probe::Stereo in333 = b.run(input, 333, chord);
        P.eq(key + ".blocks_64_333_same_bits", probe::sameBits(in64.l, in333.l) && probe::sameBits(in64.r, in333.r) ? 1 : 0,
             1);

        // END
        if constexpr (kInstrument)
        {
            const probe::Stereo tail = rig.run(probe::Stereo(static_cast<std::size_t>(8.0 * kFs)), 256);
            P.eq(key + ".voices_end", rig.engine().activeVoices(), 0);
            P.le(key + ".silent_after_db", std::max(probe::rmsDb(tail.l, tail.size() - 4800, 4800),
                                                    probe::rmsDb(tail.r, tail.size() - 4800, 4800)),
                 -120.0);
        }

        // STEAL
        if constexpr (kInstrument)
        {
            constexpr int kKeys = 18;
            // 50 ms apart, on the 16-sample grid
            const std::size_t gap = static_cast<std::size_t>(0.05 * kFs), at = gap * (kKeys - 1);
            std::vector<probe::Note> keys;
            for (int k = 0; k < kKeys; ++k)
                keys.push_back({ gap * static_cast<std::size_t>(k), pg::EventKind::noteOn, 48 + k,
                                 k == kKeys - 1 ? 1.0f : 0.0f });
            const std::vector<probe::Note> one { { 0, pg::EventKind::noteOn, 48 + kKeys - 1, 1.0f } };
            probe::Rig s(kFs, 512), f(kFs, 512);
            s.load(example.source);
            f.load(example.source);
            const probe::Stereo stolen = s.run(probe::Stereo(at + gap), 256, keys);
            const probe::Stereo fresh = f.run(probe::Stereo(gap), 256, one);
            const auto peak = [](const std::vector<float>& v, std::size_t from, std::size_t count) {
                double p = 0.0;
                for (std::size_t i = from; i < from + count && i < v.size(); ++i)
                    p = std::max(p, std::fabs(static_cast<double>(v[i])));
                return p;
            };
            const double first = peak(stolen.l, at, 12), freshFirst = peak(fresh.l, 0, 12);
            const double held = peak(fresh.l, gap / 2, gap / 2);
            std::printf("  a note on a stolen voice's instance: its first 12 samples' peak %.4f, a fresh note's %.4f "
                        "(held %.4f)\n", first, freshFirst, held);
            P.le(key + ".steal.starts_as_fresh", first, 1.5 * freshFirst + 0.01 * held);
            P.eq(key + ".steal.no_alerts", static_cast<int64_t>(s.engine().takeAlerts()), 0);
        }
    }
    return P.finish();
}
