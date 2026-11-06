// Source/plugin/AbMatch.h: A/B listening's matched loudness (EditorPort::ab, the processor's ProcessorAb.cpp), JUCE-free.
// Each side is rendered offline in a measure::Session of its own (never the Engine that plays) and measured as BS.1770's
// integrated loudness (measure::loudness), and the louder side is turned down to the quieter's:
//
//   an instrument  a chord (C3, E3, G3, C4) at velocity 0.8, held 1.5 s: its loudness while held, from 50 ms in
//   an effect      pink noise at -18 dBFS RMS for 2 s (one seed, the same on both sides): its output's, from 50 ms in
//
// Each side renders with its dials as they would play it: `dials` (the side's own, by name) over `current` (the plugin's
// now, by name), else the program's defaults (measure::Request's rule). A side that does not compile, or is silent there
// (under kSilentLufs, BS.1770's absolute gate), leaves the loudness unmatched (both trims 0 dB) and says why. A side is
// never turned up (nothing past the output guard's +-4 can come of it), and at most kMaxTrimDb down.
//
// Matcher measures on a thread of its own, the newest job only: a job started while another runs replaces any waiting,
// and the one running is abandoned between its sides (its result never handed out); take() hands a finished job's
// result to the caller once. Its destructor waits for the side being measured (a compile and a render of two seconds of
// sound: well under a second or two).
#pragma once

#include "dsp/Sources.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace playground::ab
{
    inline constexpr double kSilentLufs = -70.0;
    inline constexpr float kMaxTrimDb = -24.0f;

    struct Job
    {
        uint64_t id = 0;                                      // the caller's: its result carries it
        std::array<Sources, 2> files;
        std::array<std::map<std::string, float>, 2> dials;    // each side's own, by name (none: `current`'s)
        std::map<std::string, float> current;                 // the plugin's dials now, by name
        double rate = 48000.0;
    };

    struct Result
    {
        uint64_t id = 0;
        std::array<double, 2> lufs{ std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN() };
        std::array<float, 2> trimDb{ 0.0f, 0.0f };
        std::string why;                                      // why it is not matched ("" when it is)
    };

    // One side's loudness (LUFS; NaN, with why, when it does not compile).
    double loudness(const Sources& files, const std::map<std::string, float>& dials,
                    const std::map<std::string, float>& current, double rate, std::string* why = nullptr);
    // The trims for two loudnesses: the louder turned down to the quieter (at most kMaxTrimDb); 0 dB both when either is
    // not a number or under kSilentLufs.
    std::array<float, 2> trims(const std::array<double, 2>& lufs);
    // A job measured on the calling thread; `stale` (when given) is asked between the sides, and a true abandons it
    // (nothing: the caller drops it).
    std::optional<Result> measure(const Job& job, const std::function<bool()>& stale = {});

    class Matcher
    {
    public:
        Matcher() = default;
        ~Matcher();
        Matcher(const Matcher&) = delete;
        Matcher& operator=(const Matcher&) = delete;

        void start(Job job);                                  // replaces a job waiting; the one running is abandoned
        bool take(Result& out);                               // a finished job's result, once
        void cancel();                                        // nothing waiting, no result from the one running
        bool busy() const;                                    // a job waits or runs

    private:
        void loop();

        mutable std::mutex mutex_;
        std::condition_variable wake_;
        std::optional<Job> waiting_;
        std::optional<Result> done_;
        uint64_t wanted_ = 0;                                 // the newest job's id (0 after cancel): another's is stale
        bool running_ = false;
        bool stop_ = false;
        std::thread thread_;                                  // made with the first job
    };
}
