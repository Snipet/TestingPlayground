// PROBE name=dsp.profile timeout=240
//
// dsp.profile: measure_output's profile (Measure.h's Request::profile, dsp/Profile.h): the rendering thread sampled
// over the calls cpu_percent times, each sample's address mapped to the program's code (Patch::codeRanges), each
// function's share of the thread's time; each product's kind.
//   HEAVY    a program with one deliberately heavy function (Heavy::run: eight rounds of twelve math kernels a
//            sample, too large a body for the optimiser to inline) and a light one (the main module's half(): a
//            multiply): the heavy one on top in its own code, its module Heavy's, with at least 80 % of the time, and
//            the modules' shares said when the main module's is 0.5 % or more; at least kFewSamples samples (an
//            instrument's note held 2 s, an effect's test signals 1 s; where the sampler's sleeps are coarse, as a
//            virtual machine's are, held longer, up to 8 times and the longest a render may be); the shares (and
//            outside the program's code) summing to 1
//   INLINED  what the optimiser folded into what: half() is listed in the code of the main module's process (named
//            "Voice::process", "Effect::process": the plugin's appended call as the method it calls), and in no other;
//            every function a range names is the program's (no "playground_" call's name)
//   WORDS    the report's line: "Profile of the notes held (N samples of the rendering thread, about every x ms of its
//            time; ...)" (the effect's "of the signals cpu_percent times"), the heavy function and its share, process
//            when it took a sample with what was inlined into it, what lies outside the program's code; a window's
//            "Profile of the window"; with too few samples (a light program held 0.1 s) it says so (or that it has
//            none)
//   OFF      the overhead with the profile off is nil: a measure without it makes no Sampler (profile::made() the same
//            before and after: no thread, no code listed) and has no profile line nor shares; with it one Sampler; the
//            render hash and every metric but the CPU's the same with the profile on and off (sampling changes no bit)
//   CALLS    a program whose process calls FMinus's runtime (an FFT, forward and back, a few times a call): where the
//            platform charges a call to its caller (profile::chargesCalls(): macOS, the stack walked) process has
//            the time (80 % or more), most of it in its calls (Share::calls 50 % or more), outside the program's code
//            under 20 %, and the line says so ("in its calls out of the program's code ...: FMinus's runtime (an FFT,
//            a called math function, the threads' pool) ..."); elsewhere the time is outside the program's code
//   PARALLEL a program whose process has a parallel { }: its region's code listed (CodeRange::parallel), and the line
//            says that only this thread is sampled; the heavy program's (no region) says nothing of it
//   CODE     Patch::codeRanges: sorted, none overlapping, each named; a program with a module Shared names its init
//            "Shared::init" (the method's own code and the plugin's appended call, at least two ranges), no range
//            named for the appended call itself ("playground_", the main module's "shared_init"); an instrument's
//            Shared hooks named "Shared::noteOn" and "Shared::noteOff" (their appended calls too), none "note_on"
// Printed: each report's profile line, and cpu_percent with the profile off and on (not asserted: the thread's time,
// barely touched by the sampler).
#include "Probe.h"

#include "dsp/Measure.h"
#include "dsp/Profile.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr const char* kMain = kInstrument ? "Voice" : "Effect";

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // The deliberately heavy function: eight rounds of twelve math kernels a sample, too large a body to inline.
    const char* kHeavy = R"(module Heavy
{
    float last;
    float run(float x)
    {
        float s = x;
        for (int k = 0; k < 8; ++k)
        {
            s = sinf(s * 1.37f + 0.11f) + 0.25f * expf(-s * s);
            s = sinf(s * 0.93f - 0.07f) + 0.25f * expf(-0.5f * s * s);
            s = sinf(s * 1.11f + 0.05f) + 0.25f * expf(-0.7f * s * s);
            s = tanhf(s * 1.21f) + 0.1f * sinf(s * 2.3f) + 0.1f * sinf(s * 3.1f);
            s = sinf(s * 0.61f) + 0.1f * tanhf(s * 0.8f) + 0.05f * sinf(s * 5.3f);
        }
        last = s;
        return s;
    }
};
)";

    std::string heavyProgram()
    {
        if constexpr (kInstrument)
            return std::string(kHeavy) + R"(
module Voice
{
    float rate, ph, step;
    bool held;
    Heavy heavy;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; ph = 0; }
    void stop() { held = false; }
    float half(float x) { return 0.5f * x; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float x = sinf(6.28318531f * ph);
            ph += step;
            if (ph >= 1) ph -= 1;
            float y = heavy.run(x);
            left[i] = half(x) + 0.001f * y;
            right[i] = left[i];
        }
        return held;
    }
};
)";
        else
            return std::string(kHeavy) + R"(
module Effect
{
    Heavy heavy;
    float half(float x) { return 0.5f * x; }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float y = heavy.run(left[i]);
            left[i] = half(left[i]) + 0.001f * y;
            right[i] = half(right[i]) + 0.001f * y;
        }
    }
};
)";
    }

    // A program whose process calls FMinus's runtime: an FFT of 1024, forward and back, `rounds` times a call.
    std::string fftProgram(int rounds)
    {
        const std::string fields = R"(    void* plan;
    float buf[1024];
    float back[1024];
    float2 bins[513];
)";
        const std::string spin = "        for (int k = 0; k < " + std::to_string(rounds) + R"(; ++k)
        {
            buf[k] = left[0] + 0.0001f * back[k];
            fft_forward(plan, buf, bins);
            bins[3].x = 0.5f * bins[3].x;
            fft_inverse(plan, bins, back);
        }
)";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + fields + R"(    float rate, ph, step;
    bool held;
    void init(float r) { rate = r; plan = fft_create(1024); }
    void end() { fft_destroy(plan); }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = right[i] = 0.25f * sinf(6.28318531f * ph);
            ph += step;
            if (ph >= 1) ph -= 1;
        }
)" + spin + R"(        left[0] += 0.000001f * back[0];
        return held;
    }
};
)";
        else
            return "module Effect\n{\n" + fields + R"(    void init(float rate) { plan = fft_create(1024); }
    void end() { fft_destroy(plan); }
    void process(float* restrict left, float* restrict right, int n)
    {
)" + spin + R"(        left[0] += 0.000001f * back[0];
    }
};
)";
    }

    // A program whose process runs a parallel { } of two chains, one a channel.
    std::string parallelProgram()
    {
        const std::string chain = R"(module Chain
{
    float z;
    void run(float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float s = x[i];
            for (int k = 0; k < 16; ++k)
                s = sinf(1.1f * s + z);
            z = 0.001f * s;
            x[i] = x[i] + z;
        }
    }
};
)";
        if constexpr (kInstrument)
            return chain + R"(
module Voice
{
    Chain a, b;
    float rate, ph, step;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = right[i] = 0.25f * sinf(6.28318531f * ph);
            ph += step;
            if (ph >= 1) ph -= 1;
        }
        parallel
        {
            a.run(left, n);
            b.run(right, n);
        }
        return held;
    }
};
)";
        else
            return chain + R"(
module Effect
{
    Chain a, b;
    void process(float* restrict left, float* restrict right, int n)
    {
        parallel
        {
            a.run(left, n);
            b.run(right, n);
        }
    }
};
)";
    }

    // A light program, with a module Shared (its init named as the method it calls; an instrument's with its note
    // hooks too, Patch.h's "Shared on its own").
    std::string lightProgram()
    {
        const std::string shared = kInstrument ? R"(module Shared
{
    float gain;
    int held;
    void init(float rate) { gain = 0.5f; }
    void noteOn(float note, float velocity) { held += 1; }
    void noteOff(float note) { held -= 1; }
};
)"
                                               : R"(module Shared
{
    float gain;
    void init(float rate) { gain = 0.5f; }
};
)";
        if constexpr (kInstrument)
            return shared + R"(
module Voice
{
    Shared* shared;
    float rate, ph, step;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = right[i] = shared->gain * sinf(6.28318531f * ph);
            ph += step;
            if (ph >= 1) ph -= 1;
        }
        return held;
    }
};
)";
        else
            return shared + R"(
module Effect
{
    Shared* shared;
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = shared->gain * left[i];
            right[i] = shared->gain * right[i];
        }
    }
};
)";
    }

    // The report's profile line ("" when it has none).
    std::string profileLine(const std::string& report)
    {
        const std::size_t at = report.find("Profile");
        if (at == std::string::npos || (at > 0 && report[at - 1] != '\n'))
            return {};
        return report.substr(at, report.find('\n', at) - at);
    }

    const pg::profile::Sampler::Share* shareOf(const std::vector<pg::profile::Sampler::Share>& all, const std::string& f)
    {
        for (const auto& s : all)
            if (s.function == f)
                return &s;
        return nullptr;
    }

    bool lists(const std::vector<std::string>& names, const std::string& f)
    {
        return std::find(names.begin(), names.end(), f) != names.end();
    }
}

PROBE(dsp, profile)
{
    const pg::Sources heavy = pg::singleFile(heavyProgram());
    const std::string process = std::string(kMain) + "::process";
    const std::string half = std::string(kMain) + "::half";
    ms::Session session(heavy, 48000.0);
    P.eq("program.compiles", session.compiled() ? 1 : 0, 1);
    if (!session.compiled())
    {
        std::printf("does not compile:\n%s\n", session.errors().c_str());
        return P.finish();
    }
    ms::Request base;
    base.rate = 48000.0;
    base.holdSeconds = kInstrument ? 2.0f : 1.0f;
    base.tailSeconds = 0.2f;

    // ---- OFF ---------------------------------------------------------------------------------------------------------
    const uint64_t before = pg::profile::made();
    const ms::Result off = session.measure(base);
    const uint64_t afterOff = pg::profile::made();
    ms::Request withProfile = base;
    withProfile.profile = true;
    const ms::Result on = session.measure(withProfile);
    const uint64_t afterOn = pg::profile::made();
    std::printf("cpu_percent with the profile off %.3f %%, on %.3f %%\n", off.values.count("cpu_percent") != 0 ? off.values.at("cpu_percent") : -1.0,
                on.values.count("cpu_percent") != 0 ? on.values.at("cpu_percent") : -1.0);
    P.eq("off.no_sampler", static_cast<int64_t>(afterOff - before), 0);
    P.eq("off.no_line", profileLine(off.report).empty() && off.profile.empty() && off.profileSamples == 0 ? 1 : 0, 1);
    P.eq("off.one_sampler_on", static_cast<int64_t>(afterOn - afterOff), 1);
    P.eq("off.same_bits", !off.hash.empty() && off.hash == on.hash ? 1 : 0, 1);
    int differ = 0, compared = 0;
    for (const auto& [name, value] : off.values)
    {
        if (name == "cpu_percent" || name == "cpu16_percent")
            continue;
        ++compared;
        const auto it = on.values.find(name);
        differ += it == on.values.end() || !(it->second == value || (std::isnan(it->second) && std::isnan(value))) ? 1 : 0;
    }
    std::printf("metrics compared on and off: %d, differing %d\n", compared, differ);
    P.ge("off.metrics_compared", compared, 5);
    P.eq("off.same_metrics", differ, 0);

    // ---- HEAVY -------------------------------------------------------------------------------------------------------
    // Measured again, held longer, where the sampler took fewer than kFewSamples (its sleeps coarse, as a virtual
    // machine's are: a sample every millisecond or two of the thread's time rather than kIntervalMicros): what is checked
    // is that a render long enough has them, and then its words.
    ms::Result longer;
    const ms::Result* heavyRun = &on;
    if (pg::profile::available() && on.profileSamples > 0 && on.profileSamples < pg::profile::kFewSamples)
    {
        const double times = std::min(8.0, std::ceil(1.5 * static_cast<double>(pg::profile::kFewSamples)
                                                      / static_cast<double>(on.profileSamples)));
        ms::Request held = withProfile;
        held.holdSeconds = static_cast<float>(std::min(static_cast<double>(base.holdSeconds) * times, held.longest));
        longer = session.measure(held);
        std::printf("%zu samples held %.1f s (the sampler's sleeps coarse here): held %.1f s, %zu samples\n", on.profileSamples,
                    static_cast<double>(base.holdSeconds), static_cast<double>(held.holdSeconds), longer.profileSamples);
        heavyRun = &longer;
    }
    const ms::Result& heavyOn = *heavyRun;
    const std::string line = profileLine(heavyOn.report);
    std::printf("%s\n", line.c_str());
    for (const auto& s : heavyOn.profile)
    {
        std::string inl;
        for (const std::string& f : s.inlined)
            inl += (inl.empty() ? "" : ", ") + f;
        std::printf("  %-24s %6.2f %%  module %-8s inlined: %s\n", s.function.c_str(), s.share * 100.0, s.module.c_str(), inl.c_str());
    }
    std::printf("  outside %.2f %%, %zu samples\n", heavyOn.profileOutside * 100.0, heavyOn.profileSamples);
    if (!pg::profile::available())
    {
        P.eq("heavy.unavailable_said", has(line, "Profile: none (this platform cannot sample a thread)") ? 1 : 0, 1);
        return P.finish();
    }
    P.ge("heavy.samples", static_cast<double>(heavyOn.profileSamples), static_cast<double>(pg::profile::kFewSamples));
    P.eq("heavy.listed", heavyOn.profile.empty() ? 0 : 1, 1);
    if (heavyOn.profile.empty())
        return P.finish();
    const auto& top = heavyOn.profile.front();
    const bool ownCode = top.function == "Heavy::run";
    const bool inlinedTop = lists(top.inlined, "Heavy::run");
    std::printf("the heavy function on top: %s\n", ownCode ? "its own code" : inlinedTop ? ("inlined into " + top.function).c_str() : "no");
    P.eq("heavy.on_top", ownCode || inlinedTop ? 1 : 0, 1);
    P.ge("heavy.share", top.share, 0.8);
    double sum = heavyOn.profileOutside;
    for (const auto& s : heavyOn.profile)
        sum += s.share;
    P.near("heavy.shares_sum", sum, 1.0, 1e-9);
    // Its body too large to inline (above): its own code, its module named, and the modules' shares.
    P.eq("heavy.own_code", ownCode ? 1 : 0, 1);
    P.eq("heavy.module", top.module == "Heavy" ? 1 : 0, 1);
    // The modules' shares when more than one has 0.5 % at least: Heavy's and the main module's (process's).
    const auto* mainShare = shareOf(heavyOn.profile, process);
    const bool twoModules = mainShare != nullptr && mainShare->share >= 0.005;
    P.eq("heavy.by_module", has(line, ". By module: Heavy ") == twoModules ? 1 : 0, 1);

    // ---- INLINED -----------------------------------------------------------------------------------------------------
    // half() is folded into process's code (the plugin's appended call, named as the method it calls).
    std::string err;
    const std::vector<pg::Patch::CodeRange> code = session.patch()->codeRanges(err);
    // A function's code may be several ranges (under flush to zero an entry point and its body): counted by name.
    std::vector<std::string> holding;
    int plugins = 0;
    for (const auto& r : code)
    {
        if (lists(r.inlined, half) && !lists(holding, r.function))
            holding.push_back(r.function);
        plugins += has(r.function, "playground_") ? 1 : 0;
        for (const std::string& f : r.inlined)
            plugins += has(f, "playground_") ? 1 : 0;
    }
    P.eq("inlined.half_in_process", lists(holding, process) ? 1 : 0, 1);
    P.eq("inlined.half_nowhere_else", static_cast<int64_t>(holding.size()), 1);
    P.eq("inlined.no_plugin_names", plugins, 0);

    // ---- WORDS -------------------------------------------------------------------------------------------------------
    const std::string lead = kInstrument ? "Profile of the notes held (" : "Profile of the signals cpu_percent times (";
    P.eq("words.lead", line.rfind(lead, 0) == 0 && has(line, " samples of the rendering thread, about every ")
                           && has(line, " ms of its time; a function's share holds what the optimiser inlined into it): ") ? 1 : 0, 1);
    P.eq("words.top", has(line, "): " + top.function + " ") ? 1 : 0, 1);
    // Each function sampled is named (up to kListed), with what was inlined into it: process's half() (process takes a
    // sample or two of the effect's renders, or none).
    const auto* proc = shareOf(heavyOn.profile, process);
    P.eq("words.inlined", proc == nullptr || (has(line, ", " + process + " ") && has(line, "% (inlined into it: " + half + ")")) ? 1 : 0, 1);
    P.eq("words.outside", has(line, "; outside the program's code ") ? 1 : 0, 1);
    P.eq("words.steady", has(line, "Few samples") ? 0 : 1, 1);
    P.eq("words.no_region", has(line, "Only this thread is sampled") ? 0 : 1, 1);
    {
        ms::Request windowed = withProfile;
        windowed.want = { "cpu_percent" };
        if constexpr (kInstrument)
            windowed.window = { 0.5, 1.5 };
        const ms::Result w = session.measure(windowed);
        const std::string wl = profileLine(w.report);
        std::printf("%s\n", wl.c_str());
        P.eq("words.window", wl.rfind(kInstrument ? "Profile of the window (" : "Profile of the signals cpu_percent times (", 0) == 0 ? 1 : 0, 1);
    }
    {
        ms::Session light(pg::singleFile(lightProgram()), 48000.0);
        ms::Request brief = withProfile;
        brief.holdSeconds = 0.1f;
        brief.tailSeconds = 0.0f;
        brief.want = { "cpu_percent" };
        const ms::Result b = light.measure(brief);
        const std::string bl = profileLine(b.report);
        std::printf("%s\n", bl.c_str());
        P.eq("words.few", light.compiled() && (has(bl, "Few samples: hold the notes (or the signals) longer")
                                                || has(bl, "Profile: no samples (")) ? 1 : 0, 1);

        // ---- CODE ----------------------------------------------------------------------------------------------------
        std::string why;
        const std::vector<pg::Patch::CodeRange> lc = light.compiled() ? light.patch()->codeRanges(why) : std::vector<pg::Patch::CodeRange>{};
        bool sorted = !lc.empty(), named = true, sharedInit = false;
        for (std::size_t i = 0; i < lc.size(); ++i)
        {
            named = named && !lc[i].function.empty();
            sharedInit = sharedInit || (lc[i].function == "Shared::init" && lc[i].module == "Shared");
            if (i > 0)
                sorted = sorted && lc[i - 1].address + lc[i - 1].size <= lc[i].address;
        }
        std::printf("the light program's code: %zu ranges%s%s\n", lc.size(), why.empty() ? "" : ", ", why.c_str());
        P.eq("code.sorted_apart", sorted ? 1 : 0, 1);
        P.eq("code.named", named ? 1 : 0, 1);
        P.eq("code.shared_init", sharedInit ? 1 : 0, 1);
        // The appended call's code is named as the method it calls (PatchCode.cpp): Shared::init twice at least (the
        // method's own and the call's), and no name of the call's own.
        int sharedInits = 0, callNames = 0;
        for (const auto& r : lc)
        {
            sharedInits += r.function == "Shared::init" && r.module == "Shared" ? 1 : 0;
            callNames += has(r.function, "playground_") || r.function == std::string(kMain) + "::shared_init" ? 1 : 0;
        }
        std::printf("Shared::init ranges %d, ranges named for an appended call %d\n", sharedInits, callNames);
        P.ge("code.shared_init_call", sharedInits, 2);
        P.eq("code.no_call_names", callNames, 0);
        // An instrument's Shared hooks: their appended calls named as the methods they call, Shared::noteOn and
        // Shared::noteOff, never in the call's own words (Shared::note_on).
        if constexpr (kInstrument)
        {
            int hooks = 0, otherwise = 0;
            for (const auto& r : lc)
            {
                hooks += (r.function == "Shared::noteOn" || r.function == "Shared::noteOff") && r.module == "Shared" ? 1 : 0;
                otherwise += has(r.function, "note_on") || has(r.function, "note_off") ? 1 : 0;
            }
            std::printf("Shared's note hooks: %d ranges, %d named otherwise\n", hooks, otherwise);
            P.ge("code.shared_hooks", hooks, 2);
            P.eq("code.shared_hooks_named", otherwise, 0);
        }
    }

    // ---- CALLS -------------------------------------------------------------------------------------------------------
    {
        ms::Session fft(pg::singleFile(fftProgram(kInstrument ? 4 : 64)), 48000.0);
        P.eq("calls.compiles", fft.compiled() ? 1 : 0, 1);
        if (!fft.compiled())
            std::printf("does not compile:\n%s\n", fft.errors().c_str());
        else
        {
            const ms::Result c = fft.measure(withProfile);
            const std::string cl = profileLine(c.report);
            std::printf("%s\n", cl.c_str());
            const auto* called = shareOf(c.profile, process);
            std::printf("process %.2f %% (in its calls %.2f %%), outside %.2f %%, %zu samples\n", called != nullptr ? called->share * 100.0 : -1.0,
                        called != nullptr ? called->calls * 100.0 : -1.0, c.profileOutside * 100.0, c.profileSamples);
            if (pg::profile::chargesCalls())
            {
                P.ge("calls.process_share", called != nullptr ? called->share : 0.0, 0.8);
                P.ge("calls.in_its_calls", called != nullptr ? called->calls : 0.0, 0.5);
                P.le("calls.outside_small", c.profileOutside, 0.2);
                P.eq("calls.words", has(cl, "): " + process + " ") && has(cl, "in its calls out of the program's code ")
                                        && has(cl, "FMinus's runtime (an FFT, a called math function, the threads' pool) ") ? 1 : 0, 1);
            }
            else
            {
                P.ge("calls.outside", c.profileOutside, 0.5);
                P.eq("calls.words", has(cl, "in its calls out of the program's code") ? 0 : 1, 1);
            }
        }
    }

    // ---- PARALLEL ----------------------------------------------------------------------------------------------------
    {
        ms::Session par(pg::singleFile(parallelProgram()), 48000.0);
        P.eq("parallel.compiles", par.compiled() ? 1 : 0, 1);
        if (!par.compiled())
            std::printf("does not compile:\n%s\n", par.errors().c_str());
        else
        {
            std::string why;
            int regions = 0;
            for (const auto& r : par.patch()->codeRanges(why))
                regions += r.parallel ? 1 : 0;
            P.ge("parallel.listed", regions, 1);
            ms::Request held = withProfile;
            held.holdSeconds = 1.0f;
            held.want = { "cpu_percent" };
            ms::Result r = par.measure(held);
            // The rendering thread's own share of a parallel program is small: where the sampler took no sample of it
            // (its sleeps coarse), measured again, held the longest a render may be.
            if (pg::profile::available() && r.profileSamples == 0)
            {
                held.holdSeconds = static_cast<float>(held.longest);
                r = par.measure(held);
                std::printf("parallel: no samples held 1 s: held %.1f s, %zu samples\n", held.holdSeconds, r.profileSamples);
            }
            const std::string pl = profileLine(r.report);
            std::printf("%s\n", pl.c_str());
            P.eq("parallel.said", has(pl, ". Only this thread is sampled: a parallel region's work on the pool's other threads is not in it.") ? 1 : 0, 1);
        }
    }
    return P.finish();
}
