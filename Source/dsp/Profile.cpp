// Source/dsp/Profile.cpp: measure_output's profile (Profile.h): a sampler of the rendering thread, per platform, and
// its samples in words.
#include "dsp/Profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#define FP_PROFILE_MACH 1
#elif defined(_WIN32) && defined(_M_X64)
#include <windows.h>
#define FP_PROFILE_WIN 1
#endif

namespace playground::profile
{
    namespace
    {
        std::atomic<uint64_t> gMade{ 0 };

        std::string pct(double share)
        {
            char b[32];
            std::snprintf(b, sizeof b, "%.1f%%", share * 100.0);
            return b;
        }
    }

    bool available() noexcept
    {
#if defined(FP_PROFILE_MACH) || defined(FP_PROFILE_WIN)
        return true;
#else
        return false;
#endif
    }

    uint64_t made() noexcept { return gMade.load(std::memory_order_relaxed); }

    struct Sampler::Impl
    {
        std::vector<Patch::CodeRange> code;
        // What the sampler counts (its thread's while armed; read between disarm and arm): each range's weight (the
        // thread's CPU time, in its clock's units) and samples, and outside the program's code the first kOutsideKept
        // addresses with their weights.
        std::vector<double> weight;
        double outsideWeight = 0.0, total = 0.0;
        std::size_t samples = 0;
        std::vector<std::pair<std::uintptr_t, double>> outside;
        // The arming (arm, disarm) and the sampler's own state.
        std::mutex mutex;
        std::condition_variable cv;
        bool armed = false, parked = true, quit = false;
        std::atomic<bool> going{ false };
        std::thread thread;
#if defined(FP_PROFILE_MACH)
        thread_act_t target = MACH_PORT_NULL;
#elif defined(FP_PROFILE_WIN)
        HANDLE target = nullptr;
#endif
        bool baseSet = false;
        uint64_t lastCpu = 0;

        // The range holding `pc`, or -1.
        long rangeOf(std::uintptr_t pc) const noexcept
        {
            const auto it = std::upper_bound(code.begin(), code.end(), pc,
                                             [](std::uintptr_t a, const Patch::CodeRange& r) { return a < r.address; });
            if (it == code.begin())
                return -1;
            const Patch::CodeRange& r = *(it - 1);
            return pc < r.address + r.size ? static_cast<long>(it - 1 - code.begin()) : -1;
        }

        // One sample: the thread suspended, its program counter and its CPU time read, the thread resumed; then counted.
        // Nothing between the suspend and the resume allocates or locks.
        void sample() noexcept
        {
            std::uintptr_t pc = 0;
            uint64_t cpu = 0;
            bool ok = false;
#if defined(FP_PROFILE_MACH)
            if (thread_suspend(target) != KERN_SUCCESS)
                return;
#if defined(__arm64__)
            arm_thread_state64_t state{};
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            const kern_return_t got = thread_get_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
#else
            x86_thread_state64_t state{};
            mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
            const kern_return_t got = thread_get_state(target, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
#endif
            thread_basic_info_data_t info{};
            mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;
            const kern_return_t timed = thread_info(target, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &infoCount);
            thread_resume(target);
            if (got == KERN_SUCCESS && timed == KERN_SUCCESS)
            {
#if defined(__arm64__)
                pc = static_cast<std::uintptr_t>(arm_thread_state64_get_pc(state));   // its pointer authentication undone
#else
                pc = static_cast<std::uintptr_t>(state.__rip);
#endif
                cpu = (static_cast<uint64_t>(info.user_time.seconds) + static_cast<uint64_t>(info.system_time.seconds)) * 1000000u
                    + static_cast<uint64_t>(info.user_time.microseconds) + static_cast<uint64_t>(info.system_time.microseconds);
                ok = true;
            }
#elif defined(FP_PROFILE_WIN)
            if (SuspendThread(target) == static_cast<DWORD>(-1))
                return;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;            // GetThreadContext also waits until it is suspended
            const BOOL got = GetThreadContext(target, &context);
            ULONG64 cycles = 0;
            const BOOL timed = QueryThreadCycleTime(target, &cycles);
            ResumeThread(target);
            if (got != 0 && timed != 0)
            {
                pc = static_cast<std::uintptr_t>(context.Rip);
                cpu = static_cast<uint64_t>(cycles);
                ok = true;
            }
#endif
            if (!ok)
                return;
            if (!std::exchange(baseSet, true))                 // an armed stretch's first: where its time starts
            {
                lastCpu = cpu;
                return;
            }
            const double w = cpu > lastCpu ? static_cast<double>(cpu - lastCpu) : 0.0;
            lastCpu = cpu;
            ++samples;
            total += w;
            if (const long r = rangeOf(pc); r >= 0)
                weight[static_cast<std::size_t>(r)] += w;
            else
            {
                outsideWeight += w;
                if (outside.size() < kOutsideKept)
                    outside.emplace_back(pc, w);
            }
        }

        void run()
        {
#if defined(FP_PROFILE_MACH)
            pthread_setname_np("measure profile");
#endif
            std::unique_lock<std::mutex> lock(mutex);
            for (;;)
            {
                cv.wait(lock, [this] { return quit || armed; });
                if (quit)
                    return;
                parked = false;
                baseSet = false;
                lock.unlock();
                while (going.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(kIntervalMicros));
                    if (!going.load(std::memory_order_acquire))
                        break;
                    sample();
                }
                lock.lock();
                parked = true;
                cv.notify_all();
            }
        }
    };

    Sampler::Sampler(std::vector<Patch::CodeRange> code) : impl_(std::make_unique<Impl>())
    {
        gMade.fetch_add(1, std::memory_order_relaxed);
        impl_->code = std::move(code);
        impl_->weight.assign(impl_->code.size(), 0.0);
        impl_->outside.reserve(kOutsideKept);
#if defined(FP_PROFILE_MACH)
        impl_->target = pthread_mach_thread_np(pthread_self());   // no reference taken: nothing to give back
#elif defined(FP_PROFILE_WIN)
        impl_->target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   GetCurrentThreadId());
#endif
        if (available())
            impl_->thread = std::thread([this] { impl_->run(); });
    }

    Sampler::~Sampler()
    {
        disarm();
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->quit = true;
        }
        impl_->cv.notify_all();
        if (impl_->thread.joinable())
            impl_->thread.join();
#if defined(FP_PROFILE_WIN)
        if (impl_->target != nullptr)
            CloseHandle(impl_->target);
#endif
    }

    void Sampler::arm() noexcept
    {
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->armed = true;
            impl_->going.store(true, std::memory_order_release);
        }
        impl_->cv.notify_all();
    }

    void Sampler::disarm() noexcept
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->armed = false;
        impl_->going.store(false, std::memory_order_release);
        impl_->cv.wait(lock, [this] { return impl_->parked; });
    }

    std::size_t Sampler::samples() const noexcept { return impl_->samples; }

    std::vector<Sampler::Share> Sampler::shares() const
    {
        const Impl& m = *impl_;
        std::vector<Share> out;
        if (m.total <= 0.0)
            return out;
        std::map<std::string, std::size_t> at;              // a function's entry in `out`
        for (std::size_t i = 0; i < m.code.size(); ++i)
        {
            if (m.weight[i] <= 0.0)
                continue;
            const Patch::CodeRange& r = m.code[i];
            auto [it, fresh] = at.try_emplace(r.function, out.size());
            if (fresh)
                out.push_back({ r.function, r.module, 0.0, {} });
            Share& s = out[it->second];
            s.share += m.weight[i] / m.total;
            for (const std::string& f : r.inlined)
                if (std::find(s.inlined.begin(), s.inlined.end(), f) == s.inlined.end())
                    s.inlined.push_back(f);
        }
        for (Share& s : out)
            std::sort(s.inlined.begin(), s.inlined.end());
        std::stable_sort(out.begin(), out.end(), [](const Share& a, const Share& b) { return a.share > b.share; });
        return out;
    }

    double Sampler::outside() const { return impl_->total > 0.0 ? impl_->outsideWeight / impl_->total : 0.0; }

    std::string Sampler::words(const std::string& over) const
    {
        const Impl& m = *impl_;
        if (!available())
            return "Profile: none (this platform cannot sample a thread).\n";
        if (m.code.empty())
            return "Profile: none (FMinus listed none of the program's code).\n";
        if (m.samples == 0 || m.total <= 0.0)
            return "Profile: no samples (" + over + " took too little of the thread's time: hold them longer).\n";
        const std::vector<Share> all = shares();
        // About how often a sample came, in the thread's own time: macOS counts it in microseconds, Windows in cycles.
        std::string every;
#if defined(FP_PROFILE_MACH)
        char text[64];
        std::snprintf(text, sizeof text, ", about every %.2f ms of its time", m.total / static_cast<double>(m.samples) / 1000.0);
        every = text;
#endif
        std::string out = "Profile of " + over + " (" + std::to_string(m.samples) + " samples of the rendering thread" + every
                        + "; a function's share holds what the optimiser inlined into it): ";
        std::string listed;
        double rest = 0.0;
        std::size_t more = 0;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const Share& s = all[i];
            if (i >= kListed)
            {
                rest += s.share;
                ++more;
                continue;
            }
            listed += (listed.empty() ? "" : ", ") + s.function + " " + pct(s.share);
            if (!s.inlined.empty())
            {
                listed += " (inlined into it: ";
                for (std::size_t k = 0; k < s.inlined.size() && k < kListed; ++k)
                    listed += (k == 0 ? "" : ", ") + s.inlined[k];
                if (s.inlined.size() > kListed)
                    listed += " and " + std::to_string(s.inlined.size() - kListed) + " more";
                listed += ")";
            }
        }
        if (more > 0)
            listed += (listed.empty() ? "" : ", ") + std::to_string(more) + (more == 1 ? " more " : " others ") + pct(rest);
        out += listed.empty() ? std::string("none in the program's code") : listed;
        const double outsideShare = outside();
        out += "; outside the program's code " + pct(outsideShare);
#if defined(FP_PROFILE_MACH)
        // The split of what lies outside: by image, the system's libraries named by their symbols.
        if (outsideShare > 0.0 && !m.outside.empty())
        {
            Dl_info self{};                                // this image's (gMade's): the plugin's own code
            const void* base = dladdr(&gMade, &self) != 0 ? self.dli_fbase : nullptr;
            double own = 0.0, system = 0.0, none = 0.0, kept = 0.0;
            std::map<std::string, double> named;
            for (const auto& [pc, w] : m.outside)
            {
                kept += w;
                Dl_info d{};
                if (dladdr(reinterpret_cast<const void*>(pc), &d) == 0)
                    none += w;
                else if (d.dli_fbase == base)
                    own += w;
                else
                {
                    system += w;
                    if (d.dli_sname != nullptr)
                        named[d.dli_sname] += w;
                }
            }
            if (kept > 0.0)
            {
                const double scale = outsideShare / kept;
                std::string split;
                const auto part = [&](double w, const std::string& what, const std::string& names = {}) {
                    if (w * scale >= 0.0005)
                        split += (split.empty() ? "" : ", ") + what + " " + pct(w * scale) + names;
                };
                part(own, "the plugin's own code around it");
                std::string names;
                std::vector<std::pair<double, std::string>> byWeight;
                for (const auto& [name, w] : named)
                    byWeight.emplace_back(w, name);
                std::sort(byWeight.begin(), byWeight.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (std::size_t k = 0; k < byWeight.size() && k < 3; ++k)
                    names += (k == 0 ? ": " : ", ") + byWeight[k].second;
                part(system, "the system's libraries", names);
                part(none, "other generated code");
                if (!split.empty())
                    out += " (" + split + ")";
            }
        }
#endif
        // By module: each method's share summed under its module, when more than one has a share.
        std::map<std::string, double> modules;
        for (const Share& s : all)
            if (!s.module.empty())
                modules[s.module] += s.share;
        std::vector<std::pair<double, std::string>> ranked;
        for (const auto& [name, share] : modules)
            if (share >= 0.005)
                ranked.emplace_back(share, name);
        if (ranked.size() > 1)
        {
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            out += ". By module: ";
            for (std::size_t k = 0; k < ranked.size(); ++k)
                out += (k == 0 ? "" : ", ") + ranked[k].second + " " + pct(ranked[k].first);
        }
        out += ".";
        if (m.samples < kFewSamples)
            out += " Few samples: hold the notes (or the signals) longer for a steadier profile.";
        return out + "\n";
    }
}
