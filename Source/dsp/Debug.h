// Source/dsp/Debug.h: where a problem in the sound starts, and what the program's fields do, from an offline render
// (JUCE-free): the chat's trace (plugin/ChatTrace.cpp).
//
// find (nan, click, runaway, silence, dc or any): the request rendered in a Session of its own (Measure.h: its
// scenario; an effect's input its signal, else the sweep) twice.
//   Pass 1 plays it in calls of 16 samples (the Engine's grid), and after each call reads every field (Patch::fieldMap)
//   of every instance that sounds, and of Shared: when one first goes non-finite, or past 1e6. With the output guard's
//   alerts and the output itself, that finds each problem: nan (a field gone non-finite, or the guard's NaN alert),
//   runaway (a field past 1e6, or the guard's clip), click (a 10 ms window's step far above the second largest of the
//   twenty windows around it: a step that comes back, as a low saw's wrap does, is the sound's own), silence (the
//   output stops at once while a key is held or the input plays, or never sounds) and dc (a mean offset held for
//   200 ms). A field already non-finite or past 1e6 before the first call (init's sentinel, say) is a note
//   (TraceResult::fromStart), never the problem, and hides nothing after it.
//   Pass 2 plays it again the same way up to a window of at most kTraceWindow samples before the problem, and in calls
//   of one sample through it, keeping every field of the voices that sound there, sample by sample: the problem's exact
//   sample, the events within 32 samples of it (the request's notes, moves and edits; a voice that starts, ends, is
//   stolen or dropped) and up to three culprits: the fields that went non-finite first (nan) or passed 1e6 first
//   (runaway), or whose step there is at least ten times their usual step (click, silence, dc), each with its values
//   over the 4 calls before and after it and the lines that write it (fminus/Lang.h resolves each assignment's name: a
//   heuristic). Pass 2's output must equal pass 1's: when it does not, the program's output depends on how its calls
//   are split (it keeps some state per call, not per sample), which the result says, as a finding of its own; it says
//   the bits are the same only where it compared some (not after a NaN, whose voice the Engine drops a call at a time).
//   Culprits that go bad in one sample, or step alike, are named together; a step with a voice's note-on is the
//   note-on's. A NaN that lives only in a local (computed and used within a call) leaves no field bad: the result says
//   so, and names the state the map leaves out (a pointer's buffer, a large array: Patch::unmapped) when there is some.
// watch (fields: paths or globs): each field over pass 1, as the voice chosen plays it (newest: the newest voice that
//   sounds, as the panel's displays read; a note's newest voice; all: every voice pooled): min, max, mean, first, last,
//   the first non-finite moment, the largest jump between two calls of one voice and a sparkline of 32 parts; `points`
//   raw values evenly over the render, or (with `around`) a sample each around that moment, from a pass in calls of one
//   sample.
// list: the field map, a line a field (an array's elements as one), and what it leaves out.
//
// Offline only, off the audio thread: a Session's own Engine, never the one that plays. It reads the instances'
// memory as FMinus lays it out and never follows a pointer. Deterministic: the same files and request give the same
// result, but for the time limit (`seconds`) and a cancel, which stop it and say so: what pass 1 rendered is all it
// looks at, and a problem the calls of one sample did not reach is listed as pass 1 places it, "not traced".
#pragma once

#include "dsp/Measure.h"
#include "dsp/Sources.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace playground::measure
{
    enum class TraceFind : uint8_t { none, nan, click, runaway, silence, dc, any };
    inline constexpr int kTraceNewest = -1, kTraceAll = -2;              // TraceRequest::voice, besides a note
    inline constexpr std::size_t kMaxTracePatterns = 8, kMaxTraceWatched = 12, kMaxTracePoints = 64;
    inline constexpr std::size_t kTraceWindow = 4096;                    // pass 2's window, samples at most
    inline constexpr double kTraceSeconds = 90.0;                        // the time limit's default

    struct TraceRequest
    {
        Request render;                           // the render (its dials, scenario, rate and block)
        int input = 0;                            // an effect's input when the render names none (ViewInput: the sweep)
        TraceFind find = TraceFind::none;
        std::vector<std::string> fields;          // to watch: paths or globs ("filter.*", "osc[1].phase"), at most 8
        bool list = false;
        int voice = kTraceNewest;                 // a note (0..127), kTraceNewest or kTraceAll (find's default: all)
        bool voiceGiven = false;
        int points = 0;                           // raw values a watched field, 0..64
        double around = -1.0;                     // seconds: the raw values a sample each around it (-1: evenly)
        bool detail = false;                      // more of each: culprits, values, lines
        double seconds = kTraceSeconds;           // the time limit
        std::function<bool()> keepGoing;          // false stops it (a cancel)
        CircuitPins pins;                         // the netlists' configurations (a recording's: Session's `pins`)
    };

    struct TraceCulprit
    {
        std::string path;
        std::size_t at = 0;                       // the sample it went bad or stepped at
        double step = 0.0, usual = 0.0;           // a step's size and the field's usual step (click, silence, dc)
        std::vector<double> before, after;        // its values over the calls before, and from, `at`
        std::vector<std::string> writes;          // the lines that assign it: "svf.fm:44"
    };

    struct TraceProblem
    {
        TraceFind kind = TraceFind::none;
        std::size_t at = 0;                       // its sample
        int note = -1;                            // its voice's key (-1: an effect's, or no voice known)
        std::vector<std::string> near;            // the events within 32 samples of it, in words
        std::vector<TraceCulprit> culprits;
        std::string verdict;
    };

    struct TraceResult
    {
        std::string error;                        // why nothing was traced ("": it was); the compiler's messages
        std::vector<std::string> lines;           // the result in words
        std::vector<TraceProblem> problems;
        std::vector<std::string> watched;         // the paths watched
        bool splitDiffers = false;                // pass 2's output differed from pass 1's
        std::size_t splitAt = 0;                  // where first
        bool stopped = false;                     // the time limit or a cancel cut it short
        std::vector<std::string> fromStart;       // fields already non-finite or past 1e6 before the first call
    };

    TraceResult trace(const Sources& files, const TraceRequest& request);

    // A glob's match ('*' any run, '?' one character), or a module's path naming its fields ("filter": "filter.*").
    bool tracePathMatches(const std::string& pattern, const std::string& path);
}
