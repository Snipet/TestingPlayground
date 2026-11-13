// Source/dsp/Faults.cpp: see Faults.h.
#include "dsp/Faults.h"

#include <array>
#include <atomic>
#include <new>
#include <stdexcept>

namespace playground::faults
{
    // Declared in Faults.h for the probes alone (FP_PROBES); defined in every build, which the plugin never calls.
    void plantForTests(Stage stage, Kind kind, int times);
    int plantedForTests(Stage stage);

    namespace
    {
        constexpr std::size_t kStages = static_cast<std::size_t>(Stage::count);
        std::array<std::atomic<int>, kStages> planted{};      // the throws left at each stage
        std::array<std::atomic<int>, kStages> kinds{};
    }

    std::string words(std::exception_ptr e)
    {
        try
        {
            if (e)
                std::rethrow_exception(e);
        }
        catch (const std::bad_alloc&)
        {
            return "out of memory";
        }
        catch (const std::exception& x)
        {
            std::string out;
            for (const char* c = x.what(); c != nullptr && *c != '\0' && out.size() < 300; ++c)
                out += *c == '\n' || *c == '\r' || *c == '\t' ? ' ' : *c;
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            if (!out.empty())
                return out;
        }
        catch (...)
        {
        }
        return "an unknown error";
    }

    void reach(Stage stage)
    {
        const auto i = static_cast<std::size_t>(stage);
        if (i >= kStages)
            return;
        int left = planted[i].load(std::memory_order_relaxed);
        while (left > 0 && !planted[i].compare_exchange_weak(left, left - 1, std::memory_order_relaxed))
        {
        }
        if (left <= 0)
            return;
        switch (static_cast<Kind>(kinds[i].load(std::memory_order_relaxed)))
        {
            case Kind::outOfMemory: throw std::bad_alloc();
            case Kind::error: throw std::runtime_error("a planted failure\nin two lines");
            case Kind::unknown: throw 42;
        }
    }

    void plantForTests(Stage stage, Kind kind, int times)
    {
        const auto i = static_cast<std::size_t>(stage);
        if (i >= kStages)
            return;
        kinds[i].store(static_cast<int>(kind), std::memory_order_relaxed);
        planted[i].store(times > 0 ? times : 0, std::memory_order_relaxed);
    }

    int plantedForTests(Stage stage)
    {
        const auto i = static_cast<std::size_t>(stage);
        return i < kStages ? planted[i].load(std::memory_order_relaxed) : 0;
    }
}
