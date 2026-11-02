// PROBE name=dsp.metrics timeout=240
//
// dsp.metrics: the metrics' definitions (Measure.h), on programs whose answers are known; each product's kind.
//   CLICKS   measure's click metric and trace's click finding agree: a sound with a step every 40 ms (an engine's
//            firing, longer than 1.5 cycles of its note) has none by either, a sine that jumps once has one by both; an
//            effect's: its report marks no step a click for the engine's, one for the jump, as trace finds
#include "Probe.h"

#include "dsp/Debug.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    [[maybe_unused]] double at(const ms::Metrics& m, const char* k)
    {
        const auto it = m.find(k);
        return it == m.end() ? std::nan("") : it->second;
    }

    [[maybe_unused]] double noteHz(int n) { return 440.0 * std::exp2((n - 69) / 12.0); }

    // Whether the report's step line marks one of a signal's steps a click ("0.310 in the sine at 0.50 s, ... (a click)").
    [[maybe_unused]] bool markedIn(const std::string& report, const std::string& signal)
    {
        const std::size_t from = report.find("Largest step");
        if (from == std::string::npos)
            return false;
        const std::string line = report.substr(from, report.find('\n', from) - from);
        for (std::size_t a = 0; a < line.size();)
        {
            std::size_t b = line.find("; ", a);
            if (b == std::string::npos)
                b = line.size();
            const std::string entry = line.substr(a, b - a);
            if (has(entry, " in " + signal + " at ") && has(entry, "(a click)"))
                return true;
            a = b + 2;
        }
        return false;
    }

    // A sine with a firing every 40 ms (a step of 0.25 that decays over 4 ms: an engine's), or with one jump of half a
    // cycle at 0.5 s.
    [[maybe_unused]] std::string clickVoice(bool engine)
    {
        return std::string(R"(module Voice
{
    float rate, ph, step, env, fire;
    int count, period, done;
    bool held;
    void init(float r) { rate = r; period = (int)(0.04f * r); }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.05f * rate));
        const float decay = expf(-1 / (0.004f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            count += 1;
)") + (engine ? R"(            if (count >= period) { count = 0; fire = 0.25f; }
)"
                  : R"(            if (count == (int)(0.5f * rate)) ph += 0.5f;
)")
             + R"(            const float out = env * (0.1f * sinf(6.28318531f * ph) + fire);
            fire *= decay;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += out;
            right[i] += out;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // The input with a firing every 40 ms added (a step of 0.25 decaying over 4 ms), or with 0.3 added from 0.5 s on.
    [[maybe_unused]] std::string clickEffect(bool engine)
    {
        return std::string(R"(module Effect
{
    float rate, fire;
    int count, period;
    void init(float r) { rate = r; period = (int)(0.04f * r); }
    void process(float* restrict left, float* restrict right, int n)
    {
        const float decay = expf(-1 / (0.004f * rate));
        for (int i = 0; i < n; ++i)
        {
            count += 1;
)") + (engine ? R"(            if (count >= period) { count = 0; fire = 0.25f; }
            const float add = fire;
            fire *= decay;
)"
                  : R"(            const float add = count >= (int)(0.5f * rate) ? 0.3f : 0.0f;
)")
             + R"(            left[i] += add;
            right[i] += add;
        }
    }
};
)";
    }

    ms::Result measured(const pg::Sources& files, double rate, ms::Request r)
    {
        r.rate = rate;
        ms::Session s(files, rate);
        if (!s.compiled())
            std::printf("does not compile:\n%s\n", s.errors().c_str());
        return s.measure(r);
    }
}

PROBE(dsp, metrics)
{
    // ---- CLICKS ----------------------------------------------------------------------------------------------------------
    for (const bool engine : { true, false })
    {
        const char* what = engine ? "engine" : "jump";
        const pg::Sources files = pg::singleFile(kInstrument ? clickVoice(engine) : clickEffect(engine));
        ms::TraceRequest q;
        q.find = ms::TraceFind::click;
        if constexpr (kInstrument)
        {
            q.render.notes = { 48 };
            q.render.holdSeconds = 1.0f;
            q.render.tailSeconds = 0.3f;
        }
        else
        {
            q.render.holdSeconds = 1.0f;
            q.render.tailSeconds = 0.1f;
            q.render.signal = static_cast<int>(ms::ViewInput::sine);
        }
        const ms::TraceResult t = ms::trace(files, q);
        const bool traced = std::any_of(t.problems.begin(), t.problems.end(),
                                        [](const ms::TraceProblem& p) { return p.kind == ms::TraceFind::click; });
        ms::Request asked = q.render;
        asked.signal = -1;                                   // measure plays its own signals (the sine among them)
        const ms::Result m = measured(files, 48000.0, asked);
        bool marked = false;
        if constexpr (kInstrument)
            marked = at(m.values, "click") > 0.5;
        else
            marked = markedIn(m.report, "the sine");             // the signal trace plays
        std::printf("%s: measure says %s, trace says %s\n%s", what, marked ? "a click" : "none", traced ? "a click" : "none", m.report.c_str());
        for (const std::string& line : t.lines)
            std::printf("  trace: %s\n", line.c_str());
        P.eq(std::string("clicks.") + what + "_agree", marked == traced ? 1 : 0, 1);
        P.eq(std::string("clicks.") + what + "_verdict", marked ? 1 : 0, engine ? 0 : 1);
        if constexpr (kInstrument)
            if (engine)
            {
                // The engine's step is large against the sound (more than 0.35 of its peak, every 40 ms, 1.5 cycles of C3
                // being 11.5 ms): it is the sound's own because it comes back.
                P.ge("clicks.engine_step_large", at(m.values, "step"), 0.35 * std::pow(10.0, at(m.values, "peak_db") / 20.0));
                P.le("clicks.engine_period_long", 1.5 / noteHz(48), 0.04);
            }
    }

    return P.finish();
}
