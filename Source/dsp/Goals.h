// Source/dsp/Goals.h: what a measurement should come to (JUCE-free). A goal is on one metric (Measure.h's metrics()):
// at least, at most, between, or near a target (within a tolerance: the goal's, else the metric's own). Claude's checks
// (the chat's set_checks: ChatAgent.h) are goals with a label and the render they are measured in, measured together
// on the editor's files (one compile; one render for those that share one), the dials they do not fix as the plugin
// has them now, as it had them when they were set, or at their defaults (DialsFrom); a failing one names the dials it
// took that are off their defaults (or its snapshot). tune_dials (Tune.h) works toward goals.
#pragma once

#include "dsp/Measure.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace playground::measure
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

    // Where a check takes the dials it does not fix from: the plugin's as they are when it is measured (so the user's
    // moves count), the plugin's as they were when the check was set (its snapshot), or the program's defaults.
    enum class DialsFrom : uint8_t { current, snapshot, defaults };

    struct Check
    {
        std::string label;                        // what it checks, in a few words ("Releases within two seconds")
        Goal goal;
        Request request;                          // the render it is measured in (its `dials` those it fixes)
        DialsFrom dialsFrom = DialsFrom::current;
        std::map<std::string, float> snapshot;    // the plugin's dials when it was set, by name (dialsFrom snapshot)
    };

    // A dial a failing check took from outside itself whose value now is off its reference: its default (dialsFrom
    // current and defaults), or the check's snapshot (dialsFrom snapshot).
    struct DialAway
    {
        std::string name;
        Control control;                          // as the compiled program declares it (for its words)
        float now = 0.0f;                         // the plugin's value now
        float reference = 0.0f;                   // its default, or the snapshot's value
    };

    struct CheckResult
    {
        bool measured = false;                    // the program compiled and the render gave the metric
        bool pass = false;
        double value = 0.0;
        std::string text;                         // "2.61 s; at most 2.00 s", or why it was not measured
        std::vector<DialAway> away;               // a failing check's: the dials it does not fix (or move) that are off
                                                  // their reference, in the program's order
    };

    // Each check measured on `files`, the dials it does not fix at `current` (the plugin's, by name), at its snapshot
    // or at the defaults, as its dialsFrom says: one compile, one render for the checks that share one. A program that
    // does not compile measures none (`errors`: its messages).
    std::vector<CheckResult> evaluate(const Sources& files, const std::vector<Check>& checks,
                                      const std::map<std::string, float>& current, std::string* errors = nullptr);
}
