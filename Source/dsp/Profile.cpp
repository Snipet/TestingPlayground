// Source/dsp/Profile.cpp: measure_output's profile (Profile.h): a sampler of the rendering thread, per platform, and
// its samples in words.
#include "dsp/Profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_param.h>
#include <pthread.h>
#define FP_PROFILE_MACH 1
#elif defined(_WIN32)
#include <intrin.h>
#include <windows.h>
#if defined(_M_X64)
#define FP_PROFILE_WIN 1
#endif
#if !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803's, which older SDKs lack
#endif
#endif

namespace playground::profile
{
    namespace
    {
        std::atomic<uint64_t> gMade{ 0 };
        // What the plugin's own image is to the program's calls into it.
        constexpr const char* kRuntime = "FMinus's runtime (an FFT, a called math function, the threads' pool)";

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

    bool chargesCalls() noexcept
    {
#if defined(FP_PROFILE_MACH)
        return true;
#else
        return false;
#endif
    }

    uint64_t made() noexcept { return gMade.load(std::memory_order_relaxed); }

#if defined(_WIN32)
    double tscPerMs() noexcept
    {
        static const double rate = [] {
            LARGE_INTEGER f{}, a{}, b{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            const unsigned long long t0 = __rdtsc();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            QueryPerformanceCounter(&b);
            const unsigned long long t1 = __rdtsc();
            const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
            return ms > 0.0 ? static_cast<double>(t1 - t0) / ms : 1.0e6;
        }();
        return rate;
    }
#endif


    struct Sampler::Impl
    {
        std::vector<Patch::CodeRange> code;
        // What the sampler counts (its thread's while armed; read between disarm and arm): each range's weight (the
        // thread's CPU time, in its clock's units) and samples, of that weight what the calls it made out of the
        // program's code took (charged to it by the stack), and outside the program's code the first kOutsideKept
        // addresses with their weights and the range charged (-1: none).
        std::vector<double> weight, called;
        double outsideWeight = 0.0, total = 0.0;
        std::size_t samples = 0;
        struct Kept
        {
            std::uintptr_t pc = 0;
            double weight = 0.0;
            long range = -1;
        };
        std::vector<Kept> outside;
        // The arming (arm, disarm) and the sampler's own state.
        std::mutex mutex;
        std::condition_variable cv;
        bool armed = false, parked = true, quit = false;
        std::atomic<bool> going{ false };
        std::thread thread;
#if defined(FP_PROFILE_MACH)
        thread_act_t target = MACH_PORT_NULL;
        std::uintptr_t stackBottom = 0, stackTop = 0;  // the sampled thread's stack, [bottom, top)
#elif defined(FP_PROFILE_WIN)
        HANDLE target = nullptr;
        HANDLE timer = nullptr;   // the sampler's: a high-resolution waitable timer, where Sleep waits 1 ms at best
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

        // The sampled thread's own CPU time so far, in its clock's units (macOS: microseconds; Windows: cycles).
        bool cpuOf(uint64_t& cpu) const noexcept
        {
#if defined(FP_PROFILE_MACH)
            thread_basic_info_data_t info{};
            mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
            if (thread_info(target, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
                return false;
            cpu = (static_cast<uint64_t>(info.user_time.seconds) + static_cast<uint64_t>(info.system_time.seconds)) * 1000000u
                + static_cast<uint64_t>(info.user_time.microseconds) + static_cast<uint64_t>(info.system_time.microseconds);
            return true;
#elif defined(FP_PROFILE_WIN)
            ULONG64 cycles = 0;
            if (target == nullptr || QueryThreadCycleTime(target, &cycles) == 0)
                return false;
            cpu = static_cast<uint64_t>(cycles);
            return true;
#else
            (void) cpu;
            return false;
#endif
        }

#if defined(FP_PROFILE_MACH)
        // A code address as the thread holds it. On arm64 a return address that the system's arm64e code saved carries
        // its pointer authentication's bits above the address, which the user addresses' mask takes off.
        static std::uintptr_t codeAddress(std::uintptr_t a) noexcept
        {
#if defined(__arm64__) && defined(MACH_VM_ADDRESS_MASK)
            return a & static_cast<std::uintptr_t>(MACH_VM_ADDRESS_MASK);
#elif defined(__arm64__)
            return a & static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFULL);   // its value in macOS 27's SDK
#else
            return a;
#endif
        }

        // While the thread is suspended, for a sample outside the program's code: the range of the program's function
        // that called the code it is in (FMinus's runtime, a system library), or -1. First the return address (arm64's
        // lr, what a function that has made no call of its own returns to), then up to kWalked frame records up the
        // stack, each {its caller's record, its return address} (the ABI's frame pointer chain), each read only once it
        // is checked to lie between the stack pointer and the stack's top, aligned, above the one before. Loads alone:
        // nothing allocated, nothing locked. The sanitisers are kept off it: it reads a stopped thread's stack.
        __attribute__((no_sanitize("address", "thread"))) long callerRange(std::uintptr_t lr, std::uintptr_t fp,
                                                                         std::uintptr_t sp) const noexcept
        {
            if (lr != 0)
                if (const long r = rangeOf(codeAddress(lr)); r >= 0)
                    return r;
            const std::uintptr_t low = std::max(sp, stackBottom);
            for (int k = 0; k < kWalked; ++k)
            {
                if (fp < low || fp % 16 != 0 || stackTop < 16 || fp > stackTop - 16)
                    return -1;
                std::uintptr_t record[2];
                std::memcpy(record, reinterpret_cast<const void*>(fp), sizeof record);
                if (const long r = rangeOf(codeAddress(record[1])); r >= 0)
                    return r;
                if (record[0] <= fp)                           // a caller's record lies above its callee's
                    return -1;
                fp = record[0];
            }
            return -1;
        }
#endif

        // One sample: the thread suspended, its registers and its CPU time read (and on macOS, outside the program's
        // code, its stack's frame records), the thread resumed; then counted. Nothing between the suspend and the resume
        // allocates or locks.
        void sample() noexcept
        {
            std::uintptr_t pc = 0;
            long range = -1, charged = -1;
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
            if (got == KERN_SUCCESS && cpuOf(cpu))
            {
#if defined(__arm64__)
                pc = codeAddress(static_cast<std::uintptr_t>(arm_thread_state64_get_pc(state)));
                const auto lr = static_cast<std::uintptr_t>(arm_thread_state64_get_lr(state));
                const auto fp = static_cast<std::uintptr_t>(arm_thread_state64_get_fp(state));
                const auto sp = static_cast<std::uintptr_t>(arm_thread_state64_get_sp(state));
#else
                pc = static_cast<std::uintptr_t>(state.__rip);
                const std::uintptr_t lr = 0;
                const auto fp = static_cast<std::uintptr_t>(state.__rbp);
                const auto sp = static_cast<std::uintptr_t>(state.__rsp);
#endif
                range = rangeOf(pc);
                if (range < 0)
                    charged = callerRange(lr, fp, sp);
                ok = true;
            }
            thread_resume(target);
#elif defined(FP_PROFILE_WIN)
            if (SuspendThread(target) == static_cast<DWORD>(-1))
                return;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;            // GetThreadContext also waits until it is suspended
            const BOOL got = GetThreadContext(target, &context);
            const bool timed = cpuOf(cpu);
            ResumeThread(target);
            if (got != 0 && timed)
            {
                pc = static_cast<std::uintptr_t>(context.Rip);
                range = rangeOf(pc);
                ok = true;
            }
#endif
            if (!ok)
                return;
            if (!std::exchange(baseSet, true))                 // no base from arm(): this sample sets it
            {
                lastCpu = cpu;
                return;
            }
            const double w = cpu > lastCpu ? static_cast<double>(cpu - lastCpu) : 0.0;
            lastCpu = cpu;
            ++samples;
            total += w;
            if (range >= 0)
            {
                weight[static_cast<std::size_t>(range)] += w;
                return;
            }
            if (charged >= 0)
            {
                weight[static_cast<std::size_t>(charged)] += w;
                called[static_cast<std::size_t>(charged)] += w;
            }
            else
                outsideWeight += w;
            if (outside.size() < kOutsideKept)
                outside.push_back({ pc, w, charged });
        }

        // The sampler's wait between two samples.
        void pause() noexcept
        {
#if defined(FP_PROFILE_WIN)
            if (timer != nullptr)
            {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(kIntervalMicros) * 10;   // relative, in 100 ns
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0)
                {
                    WaitForSingleObject(timer, INFINITE);
                    return;
                }
            }
#endif
            std::this_thread::sleep_for(std::chrono::microseconds(kIntervalMicros));
        }

        void run()
        {
#if defined(FP_PROFILE_MACH)
            pthread_setname_np("measure profile");
#elif defined(FP_PROFILE_WIN)
            timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            // Above the threads it samples beside: a program's parallel regions keep every core busy with the pool's
            // threads and the rendering one, and a sampler at their priority waits out a scheduler's quantum for a core
            // (a server's is long), taking no sample in a render that ends first. It sleeps between samples.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            struct Close
            {
                HANDLE& h;
                ~Close()
                {
                    if (h != nullptr)
                        CloseHandle(h);
                }
            } closing{ timer };
#endif
            std::unique_lock<std::mutex> lock(mutex);
            for (;;)
            {
                cv.wait(lock, [this] { return quit || armed; });
                if (quit)
                    return;
                parked = false;
                lock.unlock();
                while (going.load(std::memory_order_acquire))
                {
                    pause();
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
        impl_->called.assign(impl_->code.size(), 0.0);
        impl_->outside.reserve(kOutsideKept);
#if defined(FP_PROFILE_MACH)
        const pthread_t self = pthread_self();
        impl_->target = pthread_mach_thread_np(self);   // no reference taken: nothing to give back
        impl_->stackTop = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
        const std::size_t size = pthread_get_stacksize_np(self);
        impl_->stackBottom = impl_->stackTop > size ? impl_->stackTop - size : 0;
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
        // This thread's own time as its timed calls start: the first sample's weight counts from here.
        uint64_t cpu = 0;
        const bool based = impl_->cpuOf(cpu);
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->armed)
                return;
            impl_->armed = true;
            impl_->baseSet = based;                     // the sampler is parked: what it reads, it reads after this
            impl_->lastCpu = cpu;
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
            {
                Share s;
                s.function = r.function;
                s.module = r.module;
                out.push_back(std::move(s));
            }
            Share& s = out[it->second];
            s.share += m.weight[i] / m.total;
            s.calls += m.called[i] / m.total;
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
        double msPerUnit = 0.0;
#if defined(FP_PROFILE_MACH)
        msPerUnit = 0.001;
#elif defined(FP_PROFILE_WIN)
        msPerUnit = 1.0 / tscPerMs();
#endif
        std::string every;
        if (msPerUnit > 0.0)
        {
            char text[64];
            std::snprintf(text, sizeof text, ", about every %.2f ms of its time", m.total / static_cast<double>(m.samples) * msPerUnit);
            every = text;
        }
        // Where the samples outside the program's code fell (on macOS, by dladdr): the plugin's own image (FMinus's
        // runtime, the Engine), the system's libraries (named by their symbols) or no image's (other generated code);
        // per function, those charged to it (its calls), and those charged to none.
        struct Split
        {
            double own = 0.0, system = 0.0, none = 0.0, kept = 0.0;
            std::map<std::string, double> named;
        };
        Split around;
        std::map<std::string, Split> callsOf;
#if defined(FP_PROFILE_MACH)
        {
            Dl_info self{};                                // this image's (gMade's): the plugin's own code
            const void* base = dladdr(&gMade, &self) != 0 ? self.dli_fbase : nullptr;
            for (const Impl::Kept& k : m.outside)
            {
                Split& s = k.range >= 0 ? callsOf[m.code[static_cast<std::size_t>(k.range)].function] : around;
                s.kept += k.weight;
                Dl_info d{};
                if (dladdr(reinterpret_cast<const void*>(k.pc), &d) == 0)
                    s.none += k.weight;
                else if (d.dli_fbase == base)
                    s.own += k.weight;
                else
                {
                    s.system += k.weight;
                    if (d.dli_sname != nullptr)
                        s.named[d.dli_sname] += k.weight;
                }
            }
        }
#endif
        // A split's words, its parts scaled to `share` of the thread's time: "the plugin's own code around it 1.2%, the
        // system's libraries 0.4%: memset" (the named part last, so that its names end the list).
        const auto splitWords = [](const Split& s, double share, const std::string& ownIs) {
            std::string split;
            if (share <= 0.0 || s.kept <= 0.0)
                return split;
            const double scale = share / s.kept;
            const auto part = [&](double w, const std::string& what, const std::string& names = {}) {
                if (w * scale >= 0.0005)
                    split += (split.empty() ? "" : ", ") + what + " " + pct(w * scale) + names;
            };
            part(s.own, ownIs);
            std::string names;
            std::vector<std::pair<double, std::string>> byWeight;
            for (const auto& [name, w] : s.named)
                byWeight.emplace_back(w, name);
            std::sort(byWeight.begin(), byWeight.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t k = 0; k < byWeight.size() && k < 3; ++k)
                names += (k == 0 ? ": " : ", ") + byWeight[k].second;
            part(s.none, "other generated code");
            part(s.system, "the system's libraries", names);
            return split;
        };
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
            std::string about;
            if (!s.inlined.empty())
            {
                about = "inlined into it: ";
                for (std::size_t k = 0; k < s.inlined.size() && k < kListed; ++k)
                    about += (k == 0 ? "" : ", ") + s.inlined[k];
                if (s.inlined.size() > kListed)
                    about += " and " + std::to_string(s.inlined.size() - kListed) + " more";
            }
            // What it called out of the program's code, charged to it by the stack (macOS).
            if (s.calls >= 0.0005)
            {
                about += (about.empty() ? "" : "; ") + std::string("in its calls out of the program's code ") + pct(s.calls);
                const auto c = callsOf.find(s.function);
                if (const std::string split = c != callsOf.end() ? splitWords(c->second, s.calls, kRuntime) : std::string();
                    !split.empty())
                    about += ": " + split;
            }
            if (!about.empty())
                listed += " (" + about + ")";
        }
        if (more > 0)
            listed += (listed.empty() ? "" : ", ") + std::to_string(more) + (more == 1 ? " more " : " others ") + pct(rest);
        out += listed.empty() ? std::string("none in the program's code") : listed;
        const double outsideShare = outside();
        out += "; outside the program's code " + pct(outsideShare);
        if (const std::string split = splitWords(around, outsideShare, "the plugin's own code around it"); !split.empty())
            out += " (" + split + ")";
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
        // Only this thread is sampled: a program with a parallel region runs some of its work on the pool's other
        // threads, which no sample sees. Said of the program, not of the functions sampled: the samples of a region's
        // work on this thread fall as often in what it calls as in the function that holds it, and with few samples
        // (a coarse sampler's) that one may take none.
        if (std::any_of(m.code.begin(), m.code.end(), [](const Patch::CodeRange& r) { return r.parallel; }))
            out += " Only this thread is sampled: a parallel region's work on the pool's other threads is not in it.";
        if (m.samples < kFewSamples)
            out += " Few samples: hold the notes (or the signals) longer for a steadier profile.";
        return out + "\n";
    }
}
