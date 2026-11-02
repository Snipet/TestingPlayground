// PROBE name=dsp.metrics timeout=240
//
// dsp.metrics: the metrics' definitions (Measure.h), each measured over the whole held part and the same at every rate,
// on programs whose answers are known; each product's kind.
//   RATES    the same program at 48 and at 192 kHz: an instrument's (a voice of eight harmonics; Init, whose polyBLEP
//            saw is a little darker at 48 kHz, within 10 %) centroid_hz within 5 % and pitch_cents within 2 cents; an
//            effect's (two one-pole low-passes at 1.5 kHz) centroid_out_hz within 5 %; white noise's centroid (an effect's centroid_in_hz, an instrument's noise voice's centroid_hz)
//            within 10 % of a flat spectrum's power centroid over 20 Hz to 20 kHz (10,010 Hz), at both rates
//   PHRASE   an instrument's two-note phrase, a dark note (a sine) then a bright one (sixteen harmonics): its centroid
//            between theirs, each played alone, and more than 10 % from either; its RMS over the notes' held parts, the
//            gap between them left out (the phrase with a rest reads as without it)
//   PITCH    a note with a partial an octave below at -6 dB (a sub-order) reads the note's pitch (pitch_cents within a
//            cent, the partial its own); one without its fundamental (harmonics 2 to 6) reads it from its harmonics'
//            sum within 2 cents; a chord of a fifth reads its lowest note; low notes close together (a chord of C3 and
//            D3, a bass line E2, F#2, G2, A2) and a sine at C2 and at E2 read their lowest note's own partial
//   CLICKS   measure's click metric and trace's click finding agree: a sound with a step every 40 ms (an engine's
//            firing, longer than 1.5 cycles of its note) has none by either, a sine that jumps once has one by both; an
//            effect's: its report marks no step a click for the engine's, one for the jump, as trace finds, and none
//            for an effect that passes its input (the test impulse and its echoes are the response, not clicks)
//   TERMS    every metric's words name what metricTerms() defines or a span of its own; toleranceText is Goals'
//            toleranceOf
#include "Probe.h"

#include "dsp/Debug.h"
#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
#include "fminus/LibraryIncludes.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFlatCentroid = (20.0 + 20000.0) / 2.0;   // a flat spectrum's power centroid over the band

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    double at(const ms::Metrics& m, const char* k)
    {
        const auto it = m.find(k);
        return it == m.end() ? std::nan("") : it->second;
    }

    [[maybe_unused]] double noteHz(int n) { return 440.0 * std::exp2((n - 69) / 12.0); }

    // An instrument of eight harmonics, each 1/h: the same partials at every rate.
    [[maybe_unused]] const char* kHarmonicVoice = R"(module Voice
{
    float rate, ph, step, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.2f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= 8; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += 0.1f * env * x;
            right[i] += 0.1f * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // White noise (the library's), its level held.
    [[maybe_unused]] const char* kNoiseVoice = R"(#include "noise.fm"
module Voice
{
    Noise noise;
    bool held;
    float env, fall;
    void init(float r) { fall = expf(-6.90775528f / (0.05f * r)); }
    void start(float note, float v) { held = true; env = 1; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held) env *= fall;
            const float x = 0.2f * env * noise.white();
            left[i] += x;
            right[i] += x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // A sine below note 60, sixteen harmonics (1/h) from it up: a dark note and a bright one of the same RMS, about.
    [[maybe_unused]] const char* kTwoToneVoice = R"(module Voice
{
    float rate, ph, step, env;
    int bright;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0;
        bright = note >= 60 ? 16 : 1;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.02f * rate));
        const float g = bright > 1 ? 0.1f : 0.16f;
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= bright; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += g * env * x;
            right[i] += g * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // A note's partials: `first` to `last` (1 the fundamental), each 1/h, and a sub-order (half the note) at `sub`.
    [[maybe_unused]] std::string partialsVoice(int first, int last, float sub)
    {
        return std::string(R"(module Voice
{
    float rate, ph, step, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.05f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = )") + std::to_string(sub) + R"(f * sinf(6.28318531f * ph);
            for (int h = )" + std::to_string(first) + "; h <= " + std::to_string(last) + R"(; ++h)
                x += sinf(12.5663706f * h * ph) / h;
            ph += 0.5f * step;
            if (ph >= 1) ph -= 1;
            left[i] += 0.1f * env * x;
            right[i] += 0.1f * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // Whether the report's step line marks one of a signal's steps a click ("0.310 in the sine at 0.50 s, ... (a
    // click)").
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

    // Two one-pole low-passes at 1.5 kHz: a darker noise out, the same at every rate.
    [[maybe_unused]] const char* kDarkEffect = R"(module Effect
{
    float a, l1, l2, r1, r2;
    void init(float r) { a = 1 - expf(-6.28318531f * 1500 / r); }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            l1 += a * (left[i] - l1);
            l2 += a * (l1 - l2);
            r1 += a * (right[i] - r1);
            r2 += a * (r1 - r2);
            left[i] = l2;
            right[i] = r2;
        }
    }
};
)";

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

    // An effect that passes its input as it is.
    [[maybe_unused]] const char* kPassEffect = R"(module Effect
{
    void init(float r) {}
    void process(float* restrict left, float* restrict right, int n) {}
};
)";

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
    // ---- RATES ---------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        ms::Request r;
        r.notes = { 48 };
        r.holdSeconds = 1.0f;
        r.tailSeconds = 0.5f;
        r.want = { "centroid_hz", "fundamental_hz", "pitch_cents" };
        const pg::Sources init = pg::withLibraryIncludes(pg::singleFile(pg::kExamples[0].source));
        for (const auto& [name, files] : { std::pair<const char*, pg::Sources>{ "init", init },
                                           std::pair<const char*, pg::Sources>{ "harmonics", pg::singleFile(kHarmonicVoice) } })
        {
            const ms::Result a = measured(files, 48000.0, r), b = measured(files, 192000.0, r);
            const double ca = at(a.values, "centroid_hz"), cb = at(b.values, "centroid_hz");
            const double pa = at(a.values, "pitch_cents"), pb = at(b.values, "pitch_cents");
            std::printf("%s at C3: centroid %.1f Hz at 48 kHz, %.1f Hz at 192 kHz; pitch %+.3f and %+.3f cents\n", name, ca, cb, pa, pb);
            const std::string k = std::string("rates.") + name;
            // Init's polyBLEP saw is a little darker at 48 kHz (its top octave, a tenth of a saw's power centroid, falls
            // off near the Nyquist frequency): the harmonics' voice is the same sound at both rates.
            P.near(k + "_centroid", cb / ca, 1.0, std::string(name) == "init" ? 0.1 : 0.05);
            P.near(k + "_pitch_cents", pb - pa, 0.0, 2.0);
            P.near(k + "_in_tune", pa, 0.0, 2.0);
        }
        const pg::Sources noise = pg::withLibraryIncludes(pg::singleFile(kNoiseVoice));
        for (const double rate : { 48000.0, 192000.0 })
        {
            const double c = at(measured(noise, rate, r).values, "centroid_hz");
            std::printf("white noise at %.0f Hz: centroid %.0f Hz (a flat spectrum's %.0f Hz)\n", rate, c, kFlatCentroid);
            P.near("rates.noise_centroid_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k", c / kFlatCentroid, 1.0, 0.1);
        }
    }
    else
    {
        ms::Request r;
        r.holdSeconds = 1.0f;
        r.tailSeconds = 0.2f;
        r.want = { "centroid_in_hz", "centroid_out_hz" };
        const pg::Sources dark = pg::singleFile(kDarkEffect);
        const ms::Result a = measured(dark, 48000.0, r), b = measured(dark, 192000.0, r);
        const double oa = at(a.values, "centroid_out_hz"), ob = at(b.values, "centroid_out_hz");
        std::printf("two low-passes at 1.5 kHz: centroid out %.1f Hz at 48 kHz, %.1f Hz at 192 kHz; in %.0f and %.0f Hz (a flat "
                    "spectrum's %.0f Hz)\n", oa, ob, at(a.values, "centroid_in_hz"), at(b.values, "centroid_in_hz"), kFlatCentroid);
        P.near("rates.dark_centroid_out", ob / oa, 1.0, 0.05);
        P.near("rates.noise_centroid_in_48k", at(a.values, "centroid_in_hz") / kFlatCentroid, 1.0, 0.1);
        P.near("rates.noise_centroid_in_192k", at(b.values, "centroid_in_hz") / kFlatCentroid, 1.0, 0.1);
        P.le("rates.dark_darker", oa, 0.5 * at(a.values, "centroid_in_hz"));
    }

    // ---- PHRASE --------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        const pg::Sources two = pg::singleFile(kTwoToneVoice);
        const auto phrase = [&](std::vector<ms::PhraseNote> notes) {
            ms::Request r;
            r.phrase = std::move(notes);
            r.tailSeconds = 0.3f;
            return measured(two, 48000.0, r);
        };
        const ms::Result both = phrase({ { 48, 0.0, 0.5, 0.8f }, { 72, 0.5, 0.5, 0.8f } });
        const ms::Result dark = phrase({ { 48, 0.0, 0.5, 0.8f } }), bright = phrase({ { 72, 0.0, 0.5, 0.8f } });
        const double cb = at(both.values, "centroid_hz"), cd = at(dark.values, "centroid_hz"), cl = at(bright.values, "centroid_hz");
        std::printf("a dark note then a bright one: centroid %.0f Hz; the dark alone %.0f Hz, the bright alone %.0f Hz\n%s", cb, cd, cl,
                    both.report.c_str());
        P.eq("phrase.between", cd < cb && cb < cl ? 1 : 0, 1);
        P.ge("phrase.not_the_dark", cb / cd, 1.1);
        P.ge("phrase.not_the_bright", cl / cb, 1.1);
        // A rest between the notes is no held part: the RMS reads as without it.
        const ms::Result rest = phrase({ { 48, 0.0, 0.5, 0.8f }, { 72, 1.0, 0.5, 0.8f } });
        std::printf("with a rest of 0.5 s: RMS %.2f dBFS against %.2f dBFS; centroid %.0f Hz\n", at(rest.values, "rms_db"),
                    at(both.values, "rms_db"), at(rest.values, "centroid_hz"));
        P.near("phrase.rest_not_held_rms", at(rest.values, "rms_db"), at(both.values, "rms_db"), 0.2);
        P.near("phrase.rest_not_held_centroid", at(rest.values, "centroid_hz") / cb, 1.0, 0.02);
    }

    // ---- PITCH ---------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        ms::Request r;
        r.notes = { 60 };
        // Partials 1 to 4 and one an octave below at -6 dB (half the fundamental's amplitude).
        const ms::Result sub = measured(pg::singleFile(partialsVoice(1, 4, 0.5f)), 48000.0, r);
        std::printf("a sub-order 6 dB down: %s", sub.report.c_str());
        P.near("pitch.sub_order_cents", at(sub.values, "pitch_cents"), 0.0, 1.0);
        P.near("pitch.sub_order_hz", at(sub.values, "fundamental_hz") / noteHz(60), 1.0, 0.001);
        P.eq("pitch.sub_order_partial", has(sub.report, "its harmonics' sum") ? 0 : 1, 1);
        // No fundamental: harmonics 2 to 6 alone.
        const ms::Result missing = measured(pg::singleFile(partialsVoice(2, 6, 0.0f)), 48000.0, r);
        std::printf("no fundamental: %s", missing.report.c_str());
        P.near("pitch.missing_fundamental_cents", at(missing.values, "pitch_cents"), 0.0, 2.0);
        P.eq("pitch.missing_fundamental_sum", has(missing.report, "its harmonics' sum") ? 1 : 0, 1);
        // A fifth: the lowest note's.
        ms::Request fifth = r;
        fifth.notes = { 60, 67 };
        const ms::Result f = measured(pg::singleFile(kHarmonicVoice), 48000.0, fifth);
        P.near("pitch.fifth_lowest", at(f.values, "pitch_cents"), 0.0, 2.0);
        // Low notes close together: C3 and D3 are 16 Hz apart, a bass line's E2 and F#2 10 Hz; and a low sine, whose
        // range of 600 cents spans a few tens of Hz. Each reads its lowest note's own partial.
        const auto own = [&](const std::string& k, const ms::Result& m, double within) {
            std::printf("%s: pitch %+.2f cents%s\n", k.c_str(), at(m.values, "pitch_cents"),
                        has(m.report, "its harmonics' sum") ? " (its harmonics' sum)" : "");
            P.near("pitch." + k + "_cents", at(m.values, "pitch_cents"), 0.0, within);
            P.eq("pitch." + k + "_partial", has(m.report, "its harmonics' sum") ? 0 : 1, 1);
        };
        ms::Request close = r;
        close.notes = { 48, 50 };
        own("low_chord", measured(pg::singleFile(kHarmonicVoice), 48000.0, close), 2.0);
        ms::Request line;
        line.phrase = { { 40, 0.0, 0.5, 0.8f }, { 42, 0.5, 0.5, 0.8f }, { 43, 1.0, 0.5, 0.8f }, { 45, 1.5, 0.5, 0.8f } };
        line.tailSeconds = 0.3f;
        own("bass_line", measured(pg::singleFile(kHarmonicVoice), 48000.0, line), 2.0);
        for (const int n : { 36, 40 })
        {
            ms::Request low = r;
            low.notes = { n };
            own("sine_" + std::to_string(n), measured(pg::singleFile(partialsVoice(1, 1, 0.0f)), 48000.0, low), 1.0);
        }
    }

    // ---- CLICKS --------------------------------------------------------------------------------------------------------
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
                // The engine's step is large against the sound (more than 0.35 of its peak, every 40 ms, 1.5 cycles of
                // C3 being 11.5 ms): it is the sound's own because it comes back.
                P.ge("clicks.engine_step_large", at(m.values, "step"), 0.35 * std::pow(10.0, at(m.values, "peak_db") / 20.0));
                P.le("clicks.engine_period_long", 1.5 / noteHz(48), 0.04);
            }
    }

    if constexpr (!kInstrument)
    {
        // An effect that passes its input: the impulse's step is the test signal's own, no click.
        ms::Request r;
        r.holdSeconds = 0.5f;
        r.tailSeconds = 0.2f;
        const ms::Result m = measured(pg::singleFile(kPassEffect), 48000.0, r);
        std::printf("an effect that passes its input:\n%s", m.report.c_str());
        P.eq("clicks.pass_none_marked", has(m.report, "(a click)") ? 0 : 1, 1);
        P.eq("clicks.pass_impulse_listed", has(m.report, " in the impulse at ") ? 1 : 0, 1);
    }

    // ---- TERMS ---------------------------------------------------------------------------------------------------------
    {
        const std::string terms = ms::metricTerms();
        P.eq("terms.defined", has(terms, "Held: ") && has(terms, "The spectrum: ") && has(terms, "The band: 20 Hz to 20 kHz")
                                  && has(terms, "0.1 s Hann") ? 1 : 0, 1);
        int agree = 0, spans = 0;
        for (const ms::MetricInfo& mi : ms::metrics())
        {
            // The tolerance in words, against Goals' for a target of 1000 (and of 0, a relative one's least).
            ms::Goal g;
            g.metric = mi.name;
            g.hasTarget = true;
            g.target = 1000.0;
            const double far = ms::toleranceOf(g);
            g.target = 0.0;
            const double least = ms::toleranceOf(g);
            const std::string text = ms::toleranceText(mi);
            const double said = std::strtod(text.c_str() + 2, nullptr);
            const std::size_t floorAt = text.find("at least ");
            const double saidLeast = floorAt == std::string::npos ? said : std::strtod(text.c_str() + floorAt + 9, nullptr);
            const bool ok = mi.relative ? std::fabs(far - said / 100.0 * 1000.0) < 1e-9 && std::fabs(least - saidLeast) < 1e-12
                                        : std::fabs(far - said) < 1e-12 && std::fabs(least - said) < 1e-12;
            agree += ok ? 1 : 0;
            if (!ok)
                std::printf("tolerance of %s: %s, Goals %g and %g\n", mi.name, text.c_str(), far, least);
            // Its span named: the held part (the spectrum's), a note or a signal of its own, the render, a sample.
            const std::string w = mi.what;
            bool named = false;
            for (const char* word : { "held", "render", "note-o", "sample", "silence", "spectrum", "the same of", "expected pitch" })
                named = named || has(w, word);
            spans += named ? 1 : 0;
            if (!named)
                std::printf("no span in %s: %s\n", mi.name, mi.what);
        }
        P.eq("terms.tolerances_are_goals", agree, static_cast<int64_t>(ms::metrics().size()));
        P.eq("terms.every_span", spans, static_cast<int64_t>(ms::metrics().size()));
    }
    return P.finish();
}
