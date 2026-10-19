// PROBE name=fminus.script timeout=120
//
// fminus.script: the FMinus script on the audio thread (Source/fminus/Script.h): the plugin's own script
// (Source/fminus/script.fm, compiled in as ScriptSource.h), run by the Engine on stereo noise at 48 kHz, after the
// reverb at its defaults. The rows hold for any script the file holds, as long as it compiles: edit it, rebuild, and
// this probe checks the new one.
//   SHIPPED  the plugin's script loads.
//   SILENT   at SCRIPT 0, the default, the output is the Engine's without a script, bit for bit: the script is not run.
//   PLAYS    at SCRIPT 100 %, every sample is finite, and the output is the same in blocks of 64 and of 333. It is
//            written to <build>/probe-out/fminus.script.wav, to listen to, beside fminus.script.dry.wav (no script).
//   SWAP     another script loaded while the first plays at 100 % (one that leaves the sound as it is) has taken over
//            within 50 ms, and from then on the output is the Engine's without a script, bit for bit; no sample moves
//            further from the one before it during the swap than the signals themselves do.
//   REFUSED  a script that does not compile, one whose process is not [[clang::nonblocking]], and one without destroy
//            are refused with their errors, and the running script runs on: the output is what it is without them.
//   LIVE     a ScriptWatcher on a file in probe-out/ loads it within 2 s of its appearing, and writes "loaded" in its
//            .log; the file rewritten with an error is not loaded, and the .log says why.
#include "Audio.h"
#include "Probe.h"

#include "ScriptSource.h"
#include "dsp/Engine.h"
#include "dsp/LfoShape.h"
#include "fminus/ScriptWatcher.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr std::size_t kSamples = 48000;   // 1 s

    // A script that leaves the sound as it is.
    constexpr const char* kIdentity = R"(
float* create(float rate) { return calloc(1, sizeof(float)); }
void process(float* restrict state, float* restrict left, float* restrict right, const float* restrict amount,
             int n) [[clang::nonblocking]] { }
void destroy(float* state) { free(state); }
)";

    probe::Stereo noise()
    {
        probe::Stereo s(kSamples);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (std::size_t i = 0; i < kSamples; ++i)
        {
            s.l[i] = u(rng);
            s.r[i] = u(rng);
        }
        return s;
    }

    // The Engine over `in` in blocks of `block`: `script` (none when null) loaded before prepare(), SCRIPT at `amount`
    // %. `hook` runs before each block, at its first sample, and may load another script; after each block, what the
    // audio thread handed back is freed, as the processor's ScriptWatcher frees it.
    probe::Stereo run(const probe::Stereo& in, int block, const char* script, float amount,
                      const std::function<void(std::size_t, tp::Engine&)>& hook = {})
    {
        tp::Engine engine;
        if (script != nullptr)
        {
            std::string errors;
            engine.script().load(script, "script.fm", errors);
        }
        engine.prepare(kFs, block);
        tp::LfoTable table;
        tp::renderTable(tp::defaultShape(), table);
        engine.setLfoTable(table);
        tp::ParamValues v = tp::defaultValues();
        v[tp::param::script] = amount;
        probe::Stereo out = in;
        const auto step = static_cast<std::size_t>(block);
        for (std::size_t pos = 0; pos < out.size(); pos += step)
        {
            const std::size_t len = std::min(step, out.size() - pos);
            if (hook)
                hook(pos, engine);
            engine.setParams(v);
            float* channels[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(channels, 2, static_cast<int>(len), {});
            engine.script().collect();
        }
        return out;
    }

    int64_t mismatches(const probe::Stereo& a, const probe::Stereo& b, std::size_t from = 0)
    {
        int64_t n = 0;
        for (std::size_t i = from; i < a.size(); ++i)
            n += (a.l[i] != b.l[i] ? 1 : 0) + (a.r[i] != b.r[i] ? 1 : 0);
        return n;
    }

    bool finite(const probe::Stereo& s)
    {
        return std::all_of(s.l.begin(), s.l.end(), [](float x) { return std::isfinite(x); })
               && std::all_of(s.r.begin(), s.r.end(), [](float x) { return std::isfinite(x); });
    }

    std::string slurp(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Waits up to 2 s for `path` to hold `text`; true when it does.
    bool waitFor(const std::filesystem::path& path, const std::string& text)
    {
        for (int i = 0; i < 40; ++i)
        {
            if (slurp(path).find(text) != std::string::npos)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    // The largest change from one sample to the next, on either channel, over [from, to).
    double largestStep(const probe::Stereo& s, std::size_t from, std::size_t to)
    {
        double m = 0.0;
        for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i)
            m = std::max({ m, std::fabs(double(s.l[i]) - s.l[i - 1]), std::fabs(double(s.r[i]) - s.r[i - 1]) });
        return m;
    }
} // namespace

PROBE(fminus, script)
{
    // SHIPPED
    std::string errors;
    {
        tp::Engine engine;
        P.eq("shipped.loads", engine.script().load(tp::kScriptSource, "script.fm", errors) ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("fminus.script: the plugin's script does not load:\n%s", errors.c_str());
    }

    const probe::Stereo in = noise();
    const probe::Stereo dry = run(in, 64, nullptr, 100.0f);   // no script: what the Engine gives alone

    // SILENT
    P.eq("silent.mismatches", mismatches(run(in, 64, tp::kScriptSource, 0.0f), dry), 0);

    // PLAYS
    const probe::Stereo wet = run(in, 64, tp::kScriptSource, 100.0f);
    P.eq("plays.finite", finite(wet) ? 1 : 0, 1);
    P.eq("plays.wav_written", probe::writeWav(probe::outPath("fminus.script.wav"), wet, 48000)
                                  && probe::writeWav(probe::outPath("fminus.script.dry.wav"), dry, 48000) ? 1 : 0, 1);
    P.eq("plays.blocks_mismatches", mismatches(run(in, 333, tp::kScriptSource, 100.0f), wet), 0);

    // SWAP: the identity script, loaded at 0.5 s.
    constexpr std::size_t at = kSamples / 2;
    const probe::Stereo swapped = run(in, 64, tp::kScriptSource, 100.0f, [&](std::size_t pos, tp::Engine& engine) {
        if (pos == at)
        {
            std::string e;
            engine.script().load(kIdentity, "identity.fm", e);
        }
    });
    std::size_t settled = kSamples;   // the first sample from which the output is the Engine's alone
    for (std::size_t i = kSamples; i > at; --i)
    {
        if (swapped.l[i - 1] != dry.l[i - 1] || swapped.r[i - 1] != dry.r[i - 1])
        {
            settled = i;
            break;
        }
    }
    const double settleMs = 1000.0 * static_cast<double>(settled - at) / kFs;
    P.le("swap.settle_ms", settleMs, 50.0);
    P.eq("swap.after_mismatches", mismatches(swapped, dry, at + static_cast<std::size_t>(0.05 * kFs)), 0);
    const double signals = std::max(largestStep(dry, 1, kSamples), largestStep(wet, 1, kSamples));
    P.le("swap.step_ratio", largestStep(swapped, at, settled + 1) / signals, 1.0);

    // REFUSED
    const char* broken = "float* create(float rate) { return 1 +; }\n";
    const char* blocking = "float* create(float rate) { return calloc(1, sizeof(float)); }\n"
                           "void process(float* state, float* left, float* right, const float* amount, int n) { }\n"
                           "void destroy(float* state) { free(state); }\n";
    const char* incomplete = "float* create(float rate) { return calloc(1, sizeof(float)); }\n"
                             "void process(float* state, float* left, float* right, const float* amount, int n)\n"
                             "    [[clang::nonblocking]] { }\n";
    std::string brokenErrors, blockingErrors, incompleteErrors;
    const probe::Stereo refused = run(in, 64, tp::kScriptSource, 100.0f, [&](std::size_t pos, tp::Engine& engine) {
        if (pos == at)
        {
            engine.script().load(broken, "broken.fm", brokenErrors);
            engine.script().load(blocking, "blocking.fm", blockingErrors);
            engine.script().load(incomplete, "incomplete.fm", incompleteErrors);
        }
    });
    P.eq("refused.syntax", brokenErrors.find("broken.fm:1:39: error:") != std::string::npos ? 1 : 0, 1);
    P.eq("refused.blocking", blockingErrors.find("mark it [[clang::nonblocking]]") != std::string::npos ? 1 : 0, 1);
    P.eq("refused.incomplete",
         incompleteErrors.find("the script needs a function destroy") != std::string::npos ? 1 : 0, 1);
    P.eq("refused.runs_on_mismatches", mismatches(refused, wet), 0);

    // LIVE
    const std::filesystem::path file = probe::outDir() / "fminus.script.live.fm";
    const std::filesystem::path log = file.string() + ".log";
    std::filesystem::remove(file);
    std::filesystem::remove(log);
    {
        tp::Script script;
        tp::ScriptWatcher watcher(script, file, tp::kScriptSource);
        std::ofstream(file, std::ios::binary) << kIdentity;
        P.eq("live.loads", waitFor(log, "loaded, compiled in") && script.running() ? 1 : 0, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a later file time than the first write's
        std::ofstream(file, std::ios::binary) << broken;
        P.eq("live.refused_logged", waitFor(log, "not loaded (the running script runs on):") ? 1 : 0, 1);
        P.eq("live.error_logged", slurp(log).find("fminus.script.live.fm:1:39: error:") != std::string::npos ? 1 : 0,
             1);
    }

    std::printf("fminus.script: the swap settled in %.1f ms; a script that does not compile says: %s", settleMs,
                brokenErrors.c_str());
    return P.finish();
}
