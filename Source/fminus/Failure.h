// Source/fminus/Failure.h: an exception on one of the plugin's own threads, said in words, and the probes' seam that
// plants one (plain C++, the lowest layer, so every side includes it).
//
// A thread the plugin starts (the compiler's, the freezer's, the self-test's, the shaders', a turn of the chat, the MCP
// link's, the previews', ...) catches at its top: what it was doing fails in words where that work already reports (a
// compile's errors, a status, a tool's result, a note), and the thread lives on. An exception that left one would reach
// std::terminate and take the host with it. Never on the audio thread: nothing there throws, and none of this runs
// there.
//
//   caught()          inside a catch: what was caught, in a few words ("out of memory", what() of a std::exception, "an
//                     unknown error")
//   sentence(doing)   inside a catch: "the plugin itself failed while <doing>: <caught>; this is not a fault in your
//                     program", the words a user sees
//   at(stage)         a seam: throws what a probe planted for `stage` ("compile", "check", ...), else nothing (one relaxed
//                     load). plant() exists only where FP_PROBES is defined (the probe executables): a plugin has no way
//                     to set one, so its at() never throws.
#pragma once

#include <atomic>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>

namespace playground::failure
{
    inline std::string caught()
    {
        try
        {
            throw;
        }
        catch (const std::bad_alloc&)
        {
            return "out of memory";
        }
        catch (const std::exception& e)
        {
            const char* what = e.what();
            return what != nullptr && *what != '\0' ? std::string(what) : std::string("an unknown error");
        }
        catch (...)
        {
            return "an unknown error";
        }
    }

    inline std::string sentence(const char* doing)
    {
        return std::string("the plugin itself failed while ") + doing + ": " + caught()
             + "; this is not a fault in your program";
    }

    // The planted exception's kinds (plant()).
    enum class Kind : int
    {
        badAlloc,                                // std::bad_alloc
        runtime,                                 // std::runtime_error with a message
        empty,                                   // std::runtime_error("")
        other                                    // an int: no std::exception at all
    };

    namespace detail
    {
        struct Planted
        {
            std::atomic<bool> armed{ false };
            std::atomic<int> times{ 0 };         // how many more at()s of the stage throw
            std::atomic<int> kind{ 0 };
            std::atomic<int> fired{ 0 };         // how many have thrown since plant()
            char stage[32] = {};                 // written before `armed` is set (release), read after (acquire)
        };
        inline Planted planted;

        [[noreturn]] inline void raise(Kind k)
        {
            switch (k)
            {
            case Kind::badAlloc: throw std::bad_alloc();
            case Kind::runtime: throw std::runtime_error("a planted failure");
            case Kind::empty: throw std::runtime_error("");
            case Kind::other: break;
            }
            throw 42;
        }
    }

    inline void at(const char* stage)
    {
        detail::Planted& p = detail::planted;
        if (!p.armed.load(std::memory_order_acquire)) [[likely]]
            return;
        if (std::strncmp(stage, p.stage, sizeof p.stage) != 0)
            return;
        for (int left = p.times.load(std::memory_order_relaxed); left > 0;)
            if (p.times.compare_exchange_weak(left, left - 1, std::memory_order_acq_rel))
            {
                p.fired.fetch_add(1, std::memory_order_relaxed);
                detail::raise(static_cast<Kind>(p.kind.load(std::memory_order_relaxed)));
            }
    }

#if defined(FP_PROBES)
    // Probes only: the next `times` at(stage)s throw `kind`. One stage at a time; unplant() ends it. Plant while no
    // thread is at that stage's seam (a probe plants, then asks for the work).
    inline void plant(const char* stage, Kind kind, int times = 1)
    {
        detail::Planted& p = detail::planted;
        p.armed.store(false, std::memory_order_release);
        std::strncpy(p.stage, stage, sizeof p.stage - 1);
        p.stage[sizeof p.stage - 1] = '\0';
        p.kind.store(static_cast<int>(kind), std::memory_order_relaxed);
        p.fired.store(0, std::memory_order_relaxed);
        p.times.store(times, std::memory_order_relaxed);
        p.armed.store(true, std::memory_order_release);
    }

    inline void unplant()
    {
        detail::planted.armed.store(false, std::memory_order_release);
        detail::planted.times.store(0, std::memory_order_relaxed);
    }

    // How many planted exceptions have been thrown since plant().
    inline int fired()
    {
        return detail::planted.fired.load(std::memory_order_relaxed);
    }
#endif
}
