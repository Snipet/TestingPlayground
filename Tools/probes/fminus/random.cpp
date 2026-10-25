// PROBE name=fminus.random timeout=900
//
// fminus.random: RANDOM's programs (Source/fminus/Random.h) for this plugin's kind, seeds 1 to kSeeds, at 48 kHz:
//   SAME     a seed gives the same program twice; the seeds give different programs
//   FORM     its first line names it with its seed; it #includes the library files it lists, which are the library's
//   COMPILE  each compiles with its library files beside it, with no error and no note (its panel's lines too)
//   PANEL    at most 32 dials, every one in a titled section
//   PLAYS    an instrument: a chord held a second and released, finite, audible, under 0 dBFS, no alert, every voice
//            ended within five seconds; an effect: noise and a sine through it, finite, audible, under +6 dBFS, no
//            alert, and silence through a fresh one silent
//   BLOCKS   the first kBlockSeeds give the same bits in blocks of 64 and 333
//   PARTS    every part make() can choose is among the seeds' choices
#include "Probe.h"

#include "Audio.h"
#include "Rig.h"

#include "fminus/Random.h"

#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;
    constexpr int kSeeds = 160;
    constexpr int kBlockSeeds = 16;
    constexpr double kFs = 48000.0;

    tp::Sources filesOf(const tp::random::Program& p)
    {
        tp::Sources s{ { tp::kProgramName, p.text } };
        for (const std::string& name : p.library)
            if (const tp::LibraryFile* f = tp::findLibraryFile(name))
                s.push_back({ name, f->source });
        return s;
    }

    bool finite(const probe::Stereo& s)
    {
        for (std::size_t i = 0; i < s.size(); ++i)
            if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i]))
                return false;
        return true;
    }

    double peakOf(const probe::Stereo& s)
    {
        return std::max(static_cast<double>(probe::peak(s.l)), static_cast<double>(probe::peak(s.r)));
    }

    // The program's output for its test: an instrument's chord, or an effect's noise and sine.
    probe::Stereo play(probe::Rig& rig, int block, std::size_t length)
    {
        if constexpr (kInstrument)
        {
            std::vector<probe::Note> notes;
            for (const int n : { 48, 55, 60, 64 })
                notes.push_back({ 0, tp::EventKind::noteOn, n, 0.8f });
            for (const int n : { 48, 55, 60, 64 })
                notes.push_back({ static_cast<std::size_t>(kFs), tp::EventKind::noteOff, n, 0.0f });
            return rig.run(probe::Stereo(length), block, notes);
        }
        else
        {
            probe::Stereo in = probe::noise(length, 0.2);
            const std::vector<float> sine = probe::sine(220.0, 0.25, kFs, length);
            for (std::size_t i = 0; i < length; ++i)
            {
                in.l[i] += sine[i];
                in.r[i] += sine[i];
            }
            return rig.run(in, block);
        }
    }
}

PROBE(fminus, random)
{
    int same = 0, formed = 0, compiled = 0, panelled = 0, plays = 0, ends = 0, quiet = 0, blocks = 0;
    std::set<std::string> texts, titles, parts;
    const std::size_t length = static_cast<std::size_t>(kFs * (kInstrument ? 6.0 : 2.0));
    for (uint32_t seed = 1; seed <= static_cast<uint32_t>(kSeeds); ++seed)
    {
        const tp::random::Program p = tp::random::make(seed);
        const tp::random::Program again = tp::random::make(seed);
        same += p.text == again.text && p.title == again.title ? 1 : 0;
        texts.insert(p.text);
        titles.insert(p.title);
        parts.insert(p.parts.begin(), p.parts.end());

        // FORM
        bool form = p.text.rfind("// " + p.title + " (random " + std::to_string(seed) + "): ", 0) == 0;
        for (const std::string& f : p.library)
            form = form && tp::findLibraryFile(f) != nullptr && p.text.find("#include \"" + f + "\"") != std::string::npos;
        formed += form ? 1 : 0;

        // COMPILE, PANEL
        probe::Rig rig(kFs, 512);
        std::string messages;
        const bool ok = rig.load(filesOf(p), &messages);
        if (!ok || !messages.empty())
        {
            std::printf("seed %u (%s) does not compile cleanly:\n%s\n----\n%s\n", seed, p.title.c_str(), messages.c_str(),
                        p.text.c_str());
            continue;
        }
        ++compiled;
        int dials = 0, placed = 0;
        for (const tp::Control& c : rig.controls())
            dials += c.used ? 1 : 0;
        bool titled = !rig.panel().sections.empty();
        for (const tp::PanelSection& s : rig.panel().sections)
        {
            titled = titled && !s.title.empty();
            for (const tp::PanelItem& it : s.items)
                placed += it.display ? 0 : 1;
        }
        panelled += titled && placed == dials && dials <= tp::kMaxControls ? 1 : 0;

        // PLAYS
        const probe::Stereo out = play(rig, 256, length);
        const std::size_t held = static_cast<std::size_t>(kFs * 0.5);
        const double heldRms = std::max(probe::rmsDb(out.l, 0, held), probe::rmsDb(out.r, 0, held));
        const double pk = peakOf(out);
        const uint32_t alerts = rig.engine().takeAlerts();
        const bool good = finite(out) && heldRms > -60.0 && pk < (kInstrument ? 1.0 : 2.0) && alerts == 0;
        plays += good ? 1 : 0;
        if (!good)
            std::printf("seed %u (%s): finite %d, RMS %.1f dBFS, peak %.3f, alerts %u\n", seed, p.title.c_str(),
                        finite(out) ? 1 : 0, heldRms, pk, alerts);
        if constexpr (kInstrument)
        {
            const bool ended = rig.engine().activeVoices() == 0;
            ends += ended ? 1 : 0;
            if (!ended)
                std::printf("seed %u (%s): %d voices still sound after 5 s\n", seed, p.title.c_str(), rig.engine().activeVoices());
        }
        else
        {
            probe::Rig fresh(kFs, 512);
            fresh.load(filesOf(p));
            const probe::Stereo silent = fresh.run(probe::Stereo(static_cast<std::size_t>(kFs)), 256);
            const bool still = peakOf(silent) < 1e-4;
            quiet += still ? 1 : 0;
            if (!still)
                std::printf("seed %u (%s): from silence, a peak of %.6f\n", seed, p.title.c_str(), peakOf(silent));
        }

        // BLOCKS
        if (seed <= static_cast<uint32_t>(kBlockSeeds))
        {
            const std::size_t n = static_cast<std::size_t>(kFs * 1.5);
            probe::Rig a(kFs, 512), b(kFs, 512);
            a.load(filesOf(p));
            b.load(filesOf(p));
            const probe::Stereo x = play(a, 64, n), y = play(b, 333, n);
            const bool bits = x.l == y.l && x.r == y.r;
            blocks += bits ? 1 : 0;
            if (!bits)
                std::printf("seed %u (%s): blocks of 64 and 333 differ\n", seed, p.title.c_str());
        }
    }
    std::printf("%zu different programs, %zu titles, %zu parts\n", texts.size(), titles.size(), parts.size());
    std::printf("seed 1:\n%s\n", tp::random::make(1).text.c_str());

    P.eq("same.each_seed_twice", same, kSeeds);
    P.eq("same.seeds_differ", static_cast<int64_t>(texts.size()), kSeeds);
    P.ge("same.titles_vary", static_cast<double>(titles.size()), kSeeds * 0.5);
    P.eq("form.first_line_and_includes", formed, kSeeds);
    P.eq("compile.no_message", compiled, kSeeds);
    P.eq("panel.dials_in_titled_sections", panelled, kSeeds);
    P.eq("plays.finite_audible_bounded", plays, kSeeds);
    if constexpr (kInstrument)
        P.eq("plays.voices_end", ends, kSeeds);
    else
        P.eq("plays.silence_stays_silent", quiet, kSeeds);
    P.eq("blocks.same_bits", blocks, kBlockSeeds);
    int missing = 0;
    for (const std::string& part : tp::random::allParts())
        if (parts.count(part) == 0)
        {
            std::printf("no seed chose %s\n", part.c_str());
            ++missing;
        }
    P.eq("parts.all_chosen", missing, 0);
    P.eq("parts.none_unknown", static_cast<int64_t>(parts.size()), static_cast<int64_t>(tp::random::allParts().size()));
    return P.finish();
}
