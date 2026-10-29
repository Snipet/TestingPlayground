// Source/plugin/CaptureReplay.h: a take (Capture.h) replayed offline and read as numbers (JUCE-free): the render
// request that plays it again, the null test of that replay against what played, the analysis the capture tool reports
// (octave bands, the centroid, the stereo correlation) and the moments its largest steps are named by.
//
// The replay (replayOf). measure::Session renders a Request (Measure.h) that holds the take as the processor played it:
// its rate; its host blocks in order (a block of 0 samples left out: its events and values go with the next, as the
// Engine takes them; one beyond measure::kMaxBlock in parts), each played in parts of the processor's `part` and cut at
// kMaxEventsPerBlock events as Processor::play cuts it; its events at their samples; its transport as the host sent it,
// a block at a time (none: the Engine's own song position at its start, Engine::setBeat); its dials as the processor
// sent them, the 32 normalised values where they changed (Request::frames, of the program's dials: another program
// takes them by name); the Engine's clock at its start (the voices' 16-sample grid, Engine::setClock); the wheels and
// the pedal as they stood, always (the mod and pitch wheels as the request's, pressure and the pedal as events at its
// first sample); an effect's input as it reached the Engine. A part (from, to: seconds of the take) starts and ends on
// its host blocks. Rendered with the files of the program that played at its start (compiled at the take's rate), it
// gives what played bit for bit when the program started from its fresh state there (its frozen copy, Patch.h, plays
// the program's bits, though none plays while a recording is armed); a state left by what played before (voices still
// sounding, a tail, an LFO's phase) or a program swapped in the middle make it differ, and the null test says where
// from and why.
//
// Off the audio thread only: nullTest compiles and renders (a compile, and about as long as the take plays at the
// program's cost); the rest copies and counts (the live stop and status call them).
#pragma once

#include "plugin/Capture.h"

#include "dsp/Measure.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace playground::capture
{
    inline constexpr double kMatchDb = -140.0;              // the null test's "matches": every sample within it

    struct Replay
    {
        measure::Request request;                           // what plays it again (above); tailSeconds 0
        std::array<std::vector<float>, 2> played;           // what played over it (the Engine's output), left and right
        std::size_t first = 0, last = 0;                    // its blocks in the take: [first, last)
        std::size_t from = 0, length = 0;                   // its first sample in the take, and its samples
        uint64_t program = 0;                               // the program at its start
        const Sources* files = nullptr;                     // its files (null: not known), in the take
        const Controls* controls = nullptr;                 // its dials, in the take
        double swappedAt = -1.0;                            // seconds into it where another program came (-1: none)
        bool fromStart = true;                              // it starts where the take does
    };

    // The take's part from..to (seconds; to < 0: its end) as a replay; false with why when it holds nothing. Without
    // `samples`, neither what played nor an effect's input is copied (the request's plan alone: its timeline).
    bool replayOf(const Take& take, double from, double to, Replay& out, std::string& why, bool samples = true);

    // The replay rendered and held against what played, sample by sample.
    struct Fidelity
    {
        bool rendered = false;                              // its program was known and compiled
        bool identical = false;                             // every sample the same
        double firstDiffers = -1.0;                         // seconds: the first sample beyond kMatchDb (-1: none)
        double matchesFrom = -1.0;                          // from here to the end every sample is within it (-1: never)
        double worstDb = -240.0;                            // the largest difference, dBFS
        std::string line;                                   // "Replay: matches what played bit for bit." and the like
    };
    Fidelity nullTest(const Take& take, const Replay& replay);
    // Before a replay was tested: what to expect of one (fresh state, own code), and how to make one exact.
    std::string forecast(const Take& take);

    // What the tool reports of a sound: octave bands (measure's Comparison::kBands, 31.5 Hz to 16 kHz, dBFS; -240 for
    // none), the spectral centroid and the stereo correlation, from spaced windows (live: a few milliseconds).
    struct Spectrum
    {
        std::array<double, 10> bandsDb{};
        double centroidHz = 0.0;
        double correlation = 1.0;                           // left against right (1: mono or alike)
        bool silent = true;
    };
    Spectrum spectrumOf(const std::vector<float>& l, const std::vector<float>& r, double rate);

    // The replay's timeline (measure::timeline: its events, its dial moves, the host's starts and stops) and the take's
    // own moments (a program swapped in, the frozen copy, the bypass), for measure::nearestMark.
    measure::Timeline timelineOf(const Take& take, const Replay& replay, const Controls& controls);
}
