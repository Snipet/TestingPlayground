// Source/dsp/Profile.h: measure_output's profile (Measure.h, Request::profile; JUCE-free): where the CPU time of a
// render's timed calls goes, by the program's function, sampled.
//
// A sampler thread interrupts the thread that renders (the one that made the Sampler: the chat's worker, the MCP
// link's, the headless renderer's main thread) while the calls cpu_percent times run (arm, disarm), about every
// kIntervalMicros: it suspends the thread, reads its program counter and its own CPU time, and lets it go on (macOS:
// Mach's thread_suspend, thread_get_state and thread_info, then thread_resume; Windows: SuspendThread, GetThreadContext
// and QueryThreadCycleTime, then ResumeThread, between samples a high-resolution waitable timer). No signal handler,
// nothing process-wide; nothing is added to the program's code, which runs as it always does (FMinus's recipe 37), and
// the thread's own time is barely touched (the sampler's time is its own). Between the suspend and the resume the
// sampler only reads the thread's state: it allocates and locks nothing there, so a thread suspended inside malloc or
// holding a lock cannot stall it. Each sample weighs the thread's CPU time since the one before (the first of each
// armed stretch only sets where that starts), so a thread the system put aside meanwhile weighs nothing, and the shares
// are of its own time.
//
// A sample's address is mapped to the program's code (Patch::codeRanges: FMinus's code ranges, sorted; a binary
// search on the sampler's thread, nothing allocated) and counted against its function; the shares then group by the
// function as the program names it (the plugin's appended calls as the methods they call) and by module. A range's
// time holds what the optimiser inlined into it, which the words name ("Voice::process 81.2% (inlined into it:
// Osc::tick, Svf::tick)"): a function inlined everywhere has no share of its own. What is not the program's code is
// "outside it", and on macOS split by dladdr into the plugin's own code around it (the Engine, FMinus's runtime: a math
// kernel called rather than inlined) and the system's libraries, named (memset, sinf); on Windows it is one share.
//
// Opt-in: a measure without `profile` makes no Sampler, starts no thread and lists no code (made() counts them, for
// dsp.profile); a render then checks one null pointer as its timed calls start and end, and its bits are the same with
// or without one. Off the audio thread only: Measure's Sessions make one, never the Engine that plays.
#pragma once

#include "fminus/Patch.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playground::profile
{
    // Whether this platform samples a thread (macOS and Windows); elsewhere a profile says it has none.
    bool available() noexcept;
    // The Samplers made since the process began.
    uint64_t made() noexcept;

    inline constexpr int kIntervalMicros = 100;        // the sampler's sleep between two samples (the clock's)
    inline constexpr std::size_t kFewSamples = 200;    // under it the words say a longer render gives a steadier one
    inline constexpr std::size_t kOutsideKept = 4096;  // samples outside the program's code kept for their split
    inline constexpr std::size_t kListed = 8;          // functions named at most (the rest summed), and inlined ones

    class Sampler
    {
    public:
        // On the thread it samples, with the program's code (Patch::codeRanges, sorted by address).
        explicit Sampler(std::vector<Patch::CodeRange> code);
        ~Sampler();
        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;

        // From the thread it samples: sampled from now (arm) until now (disarm, which returns once the sampler has
        // stopped, so that what it counted is whole).
        void arm() noexcept;
        void disarm() noexcept;

        std::size_t samples() const noexcept;
        // The share of the time sampled spent in each function as the program names it, the most first (functions with
        // no sample left out), and outside the program's code; between disarm and the next arm. For the probes.
        struct Share
        {
            std::string function, module;
            double share = 0.0;                        // 0..1
            std::vector<std::string> inlined;          // what the optimiser folded into its code (its ranges')
        };
        std::vector<Share> shares() const;
        double outside() const;
        // The words, a line: `over` names what was sampled ("the notes held"). Between disarm and the next arm.
        std::string words(const std::string& over) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
