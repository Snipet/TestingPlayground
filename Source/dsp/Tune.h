// Source/dsp/Tune.h: the chat's tune_dials (JUCE-free): the values of a program's dials that bring its measurements
// (Measure.h) to goals (Goals.h), found by rendering it offline and searching, instead of guessing. One compile
// (measure::Session), each render from the program as new, so a candidate's numbers never depend on the last one's.
//
// The search runs over the tuned dials' normalised ranges (a dial with a log taper is searched in its own steps): the
// dials as they are first, then a few points spread over the ranges (a Halton sequence, the same every time), then
// Nelder-Mead from the best, started again from the best when it settles while a goal is unmet, until every goal is met,
// the budget of renders is spent, `progress` says stop, or the time limit passes (`seconds`, wall-clock). The score is
// the goals' penalties, weighted (Goals.h). A goal whose metric the render leaves unmeasured (Result::unmeasured: a
// window that holds none of it, which no dial changes) is left out of the score and named in the outcome; with every
// goal so, it does not search (an error that says why). Deterministic: the same files and request give the same dials
// (unless the time limit stops the search).
//
// A sweep (TuneRequest::sweep) searches nothing: one dial, of any type, rendered at each value given in turn, the goals'
// metrics measured at each, in a table (a row a value; two values are an A/B). A sweep's goal may name a metric alone,
// reported and not judged; one with a bound or a target is marked met or not, and the value nearest the goals (the
// least weighted penalty, the first on a tie) is named. It sets nothing: TuneResult::best stays empty.
//
// Off the audio thread only (it compiles and allocates).
#pragma once

#include "dsp/Goals.h"
#include "dsp/Measure.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace playground::measure
{
    inline constexpr int kMaxTuned = 8;           // dials one search tunes
    inline constexpr int kMaxGoals = 8;
    inline constexpr int kDefaultBudget = 150;    // renders
    inline constexpr int kMaxBudget = 400;
    inline constexpr double kDefaultSeconds = 120.0;   // the search's time limit (tune_dials' seconds)
    inline constexpr double kMaxSeconds = 170.0;       // under an MCP call's deadline (mcp/Main.cpp: 200 s)
    inline constexpr int kMaxSweep = 32;          // values one sweep renders

    struct TuneRequest
    {
        Request render;                           // the render: notes, times, host; `dials` the dials kept where they
                                                  // say (and where a tuned dial starts), `current` the plugin's dials
        std::vector<std::string> dials;           // the dials to tune: real (float) dials of the program (a sweep's:
                                                  // one, of any type)
        std::vector<Goal> goals;
        std::vector<float> sweep;                 // a sweep in place of the search: dials[0] at each of these plain
                                                  // values in turn (1 to kMaxSweep, in its range; an int's and a bool's
                                                  // whole); `budget` aside
        int budget = kDefaultBudget;              // renders, at most kMaxBudget
        // After each render (renders so far, the budget): false stops the search, which keeps the best so far.
        std::function<bool(int, int)> progress;
        double seconds = 0.0;                     // wall-clock time at most (0: no limit); past it, the best so far
    };

    struct TuneResult
    {
        std::string error;                        // why it did not run ("" when it did)
        int renders = 0;
        double seconds = 0.0;
        bool stopped = false;                     // progress said stop, or the time limit passed
        bool timedOut = false;                    // the time limit passed
        std::map<std::string, float> start, best; // the tuned dials' plain values, by name as the program spells it
        Metrics startValues, bestValues;          // the goals' metrics there
        double startScore = 0.0, bestScore = 0.0;
        std::vector<bool> startMet, bestMet;      // each goal's
        struct SweepRow
        {
            float value = 0.0f;                   // the swept dial's plain value
            Metrics values;                       // the goals' metrics there (one not measured is absent)
            double score = 0.0;                   // the judged goals' weighted penalties (Goals.h)
        };
        std::vector<SweepRow> sweep;              // a sweep's rows, in the order of its values (those rendered)
        std::string text;                         // the outcome in words, for Claude
    };

    TuneResult tune(const Sources& files, const TuneRequest& request);
}
