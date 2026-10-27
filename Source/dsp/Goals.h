// Source/dsp/Goals.h: what a measurement should come to (JUCE-free). A goal is on one metric (Measure.h's metrics()):
// at least, at most, between, or near a target (within a tolerance: the goal's, else the metric's own). Claude's checks
// (the chat's set_checks: ChatAgent.h) are goals with a label and the render they are measured in, measured together
// on the editor's files (one compile; one render for those that share one); tune_dials (Tune.h) works toward goals.
#pragma once

#include "dsp/Measure.h"

#include <map>
#include <string>
#include <vector>

namespace testingplayground::measure
{
    struct Goal
    {
        std::string metric;
        bool hasMin = false, hasMax = false, hasTarget = false;
        double min = 0.0, max = 0.0, target = 0.0;
        double tolerance = 0.0;                   // how near the target counts as met, in the metric's unit (0: its own)
        double weight = 1.0;                      // tune_dials: how much it counts against the other goals
    };

    // Why a goal cannot be ("" when it can): no metric of that name for this plugin's kind, neither a bound nor a
    // target, a target with a bound, a least above a most, a tolerance or a weight below 0.
    std::string invalid(const Goal& g);
    // The tolerance that applies: the goal's, else the metric's (a relative one as a share of the target or the bound,
    // never under the metric's floor).
    double toleranceOf(const Goal& g);
    // Met: within the bounds, or within the tolerance of the target. A value not measured (NaN) is never met; infinity
    // ("never") meets only "at least".
    bool met(const Goal& g, double value);
    // How far from met, in tolerances, squared (0 within the bounds; near a target too, so a search can centre it),
    // and past twenty tolerances on in a straight line, so a search far off still sees the way; a value not measured,
    // or never reached where a bound or a target is finite, counts as a million (worse than any measured).
    double penalty(const Goal& g, double value);
    // The goal in words: "at most 2.00 s", "between -14.0 and -10.0 dBFS", "1200 Hz, within 60.0 Hz".
    std::string goalText(const Goal& g);

    struct Check
    {
        std::string label;                        // what it checks, in a few words ("Releases within two seconds")
        Goal goal;
        Request request;                          // the render it is measured in (its `dials` those it fixes)
    };

    struct CheckResult
    {
        bool measured = false;                    // the program compiled and the render gave the metric
        bool pass = false;
        double value = 0.0;
        std::string text;                         // "2.61 s; at most 2.00 s", or why it was not measured
    };

    // Each check measured on `files`, the dials it does not fix at `current` (the plugin's, by name): one compile, one
    // render for the checks that share one. A program that does not compile measures none (`errors`: its messages).
    std::vector<CheckResult> evaluate(const Sources& files, const std::vector<Check>& checks,
                                      const std::map<std::string, float>& current, std::string* errors = nullptr);
}
