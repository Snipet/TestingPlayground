// Source/dsp/Tune.h: the chat's tune_dials (JUCE-free): the values of a program's dials that bring its measurements
// (Measure.h) to goals (Goals.h), found by rendering it offline and searching, instead of guessing. One compile
// (measure::Session), each render from the program as new, so a candidate's numbers never depend on the last one's.
//
// The search runs over the tuned dials' normalised ranges (a dial with a log taper is searched in its own steps): the
// dials as they are first, then a few points spread over the ranges (a Halton sequence, the same every time), then
// Nelder-Mead from the best, started again from the best when it settles while a goal is unmet, until every goal is met,
// the budget of renders is spent, `progress` says stop, or the time limit passes (`seconds`, wall-clock). The score is
// the goals' penalties, weighted (Goals.h). Deterministic: the same files and request give the same dials (unless the
// time limit stops the search).
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

    struct TuneRequest
    {
        Request render;                           // the render: notes, times, host; `dials` the dials kept where they
                                                  // say (and where a tuned dial starts), `current` the plugin's dials
        std::vector<std::string> dials;           // the dials to tune: real (float) dials of the program
        std::vector<Goal> goals;
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
        std::string text;                         // the outcome in words, for Claude
    };

    TuneResult tune(const Sources& files, const TuneRequest& request);
}
