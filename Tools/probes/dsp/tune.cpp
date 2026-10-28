// PROBE name=dsp.tune timeout=180
//
// dsp.tune: the numbers the chat's checks and tune_dials read, and view_sound's analysis (Measure.h, Goals.h, Tune.h,
// SoundView.h), on programs whose answers are known. An instrument: a saw through a one-pole low-pass, its level in
// dB, a linear attack and a release that falls 60 dB in its own time; an effect: a gain in dB, a tanh drive and a
// one-pole low-pass, and an int that tune_dials must refuse.
//   METRICS  the named numbers: an instrument's pitch (the fundamental within 1 Hz, within 5 cents), its attack and
//            release as the dials set them, when its voice ends, a brighter low-pass a higher centroid, a high naive
//            saw's aliasing seen (inharmonic_db), the alerts and the CPU there (sixteen voices, measured, at least twice
//            the one note's); an effect's gain as its dial, its
//            THD+N rising with the drive, a darker tone a lower centroid, the impulse's peak and delay, silence silent.
//            Every metric of metrics() comes, with its unit and its words; valueText says "never" for infinity
//   SESSION  one compile measured again gives the same numbers; a render after another (Engine::restart) the same as a
//            fresh compile's; a report from `want` holds only what it asked for; run()'s report is the Session's
//   GOALS    met and penalty: bounds and targets, a relative tolerance and its floor, infinity ("never") and a value
//            not measured; a goal that cannot be is refused, saying why
//   CHECKS   evaluate: passing and failing checks, a check whose metric needs one note not measured, a program that
//            does not compile measured not at all
//   TUNE     tune_dials finds a level for an RMS, a release for a release time (an instrument), a gain for a gain and
//            a drive for a THD+N (an effect), each goal met within its budget; two dials at once; goals met already
//            take one render; an int, a name the program has not and an empty request are refused; progress can stop
//            it; the same request gives the same dials
//   VIEW     view_sound's analysis: a 440 Hz tone (an instrument's note 69; an effect's sine) is the spectrogram's
//            loudest row and the spectrum's loudest column, within a row; the close-up four of its cycles; the waveform
//            and the level as rendered; an effect's sweep rises through the rows; the note-off where it was
#include "Probe.h"

#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
#include "dsp/Tune.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kInf = std::numeric_limits<double>::infinity();

    // The instrument: level (dB), cutoff (Hz, a one-pole low-pass), attack (s, linear), release (s, 60 dB).
    const char* kVoice = R"(module Voice
{
    param float level [[fminus::range(-40, 0)]] [[fminus::default(-20)]];
    param float cutoff [[fminus::range(100, 10000)]] [[fminus::default(1000)]];
    param float attack [[fminus::range(0.001, 1)]] [[fminus::default(0.005)]];
    param float release [[fminus::range(0.01, 3)]] [[fminus::default(0.5)]];
    param int wave [[fminus::range(0, 1)]] [[fminus::default(0)]];
    float rate, ph, step, lp, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float g = expf(0.115129255f * level[0]);
        const float a = 1 - expf(-6.28318531f * cutoff[0] / rate);
        const float fall = expf(-6.90775528f / (release[0] * rate));
        const float up = 1 / (attack[0] * rate);
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 2 * ph - 1;
            if (wave[0] == 1) x = sinf(6.28318531f * ph);
            ph += step;
            if (ph >= 1) ph -= 1;
            lp += a * (x - lp);
            const float out = g * env * lp;
            left[i] += out;
            right[i] += out;
        }
        return held || env > 0.00001f;
    }
};
)";

    // The effect: gain (dB), drive (0: clean; tanh, its small-signal gain 1), tone (Hz, a one-pole low-pass), an int.
    const char* kEffect = R"(module Effect
{
    param float gain [[fminus::range(-24, 24)]] [[fminus::default(0)]];
    param float drive [[fminus::range(0, 1)]] [[fminus::default(0)]];
    param float tone [[fminus::range(200, 20000)]] [[fminus::default(20000)]];
    param int mode [[fminus::range(0, 1)]] [[fminus::default(0)]];
    float rate, lpl, lpr;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        const float g = expf(0.115129255f * gain[0]);
        const float d = 1 + 30 * drive[0];
        const float a = tone[0] >= 19999 ? 1.0f : 1 - expf(-6.28318531f * tone[0] / rate);
        for (int i = 0; i < n; ++i)
        {
            float x = left[i] * g, y = right[i] * g;
            if (drive[0] > 0)
            {
                x = tanhf(d * x) / d;
                y = tanhf(d * y) / d;
            }
            lpl += a * (x - lpl);
            lpr += a * (y - lpr);
            left[i] = lpl;
            right[i] = lpr;
        }
    }
};
)";

    pg::Sources files() { return pg::singleFile(kInstrument ? kVoice : kEffect); }

    double at(const ms::Metrics& m, const char* k)
    {
        const auto it = m.find(k);
        return it == m.end() ? std::nan("") : it->second;
    }

    ms::Goal target(const char* metric, double t, double tol = 0.0)
    {
        ms::Goal g;
        g.metric = metric;
        g.hasTarget = true;
        g.target = t;
        g.tolerance = tol;
        return g;
    }

    ms::Goal atMost(const char* metric, double v)
    {
        ms::Goal g;
        g.metric = metric;
        g.hasMax = true;
        g.max = v;
        return g;
    }

    ms::Goal atLeast(const char* metric, double v)
    {
        ms::Goal g;
        g.metric = metric;
        g.hasMin = true;
        g.min = v;
        return g;
    }

    // A report without its CPU line, which differs between renders.
    std::string withoutCpu(const std::string& r)
    {
        std::string out;
        std::size_t a = 0;
        while (a < r.size())
        {
            std::size_t b = r.find('\n', a);
            if (b == std::string::npos)
                b = r.size();
            const std::string line = r.substr(a, b - a);
            if (line.rfind("CPU:", 0) != 0)
                out += line + "\n";
            a = b + 1;
        }
        return out;
    }

    bool sameNumbers(const ms::Metrics& a, const ms::Metrics& b)
    {
        if (a.size() != b.size())
            return false;
        for (const auto& [k, v] : a)
        {
            if (k.rfind("cpu", 0) == 0)
                continue;
            const auto it = b.find(k);
            if (it == b.end() || !(it->second == v || (std::isnan(v) && std::isnan(it->second))))
                return false;
        }
        return true;
    }
}

PROBE(dsp, tune)
{
    // ---- METRICS -------------------------------------------------------------------------------------------------------
    ms::Session session(files());
    P.eq("metrics.compiles", session.compiled() ? 1 : 0, 1);
    if (!session.compiled())
        std::printf("%s\n", session.errors().c_str());
    ms::Request base;
    base.holdSeconds = 1.0f;
    base.tailSeconds = 1.5f;
    if constexpr (kInstrument)
    {
        base.notes = { 57 };                                 // A3, 220 Hz
        ms::Request r = base;
        r.dials = { { "attack", 0.2f }, { "release", 0.5f } };
        const ms::Result m = session.measure(r);
        std::printf("%s", m.report.c_str());
        P.near("metrics.fundamental_hz", at(m.values, "fundamental_hz"), 220.0, 1.0);
        P.near("metrics.pitch_cents", at(m.values, "pitch_cents"), 0.0, 5.0);
        // A linear attack of 0.2 s is within 3 dB of its top at 0.2 x 0.708 s.
        P.near("metrics.attack_s", at(m.values, "attack_s"), 0.2 * 0.70794578, 0.01);
        P.near("metrics.release_s", at(m.values, "release_s"), 0.5, 0.03);
        // The voice ends under 1e-5 (100 dB) in release x 100 / 60.
        P.near("metrics.voice_end_s", at(m.values, "voice_end_s"), 0.5 * 100.0 / 60.0, 0.03);
        // From its loudest moment (somewhere on the plateau, 0.2 s to 1 s) to 60 dB down (0.5 s after the note-off).
        P.in("metrics.decay_s", at(m.values, "decay_s"), 0.45, 1.35);
        P.eq("metrics.alerts", static_cast<int64_t>(at(m.values, "alerts")), 0);
        P.ge("metrics.cpu", at(m.values, "cpu_percent"), 0.0);
        // Sixteen voices measured at once cost more than the one note held (about sixteen times; twice is the floor).
        P.ge("metrics.cpu16_sixteen_voices", at(m.values, "cpu16_percent"), 2.0 * at(m.values, "cpu_percent"));
        ms::Request dark = base, bright = base;
        dark.dials = { { "cutoff", 300.0f } };
        bright.dials = { { "cutoff", 6000.0f } };
        const double cd = at(session.measure(dark).values, "centroid_hz"), cb = at(session.measure(bright).values, "centroid_hz");
        std::printf("centroid: cutoff 300 Hz %.0f Hz, 6000 Hz %.0f Hz\n", cd, cb);
        P.eq("metrics.brighter_higher_centroid", cb > 1.5 * cd ? 1 : 0, 1);
        ms::Request high = base;
        high.notes = { 100 };
        high.dials = { { "cutoff", 10000.0f } };
        const double alias = at(session.measure(high).values, "inharmonic_db");
        std::printf("a naive saw at note 100: inharmonic %.1f dB\n", alias);
        P.ge("metrics.aliasing_seen", alias, -60.0);
        ms::Request chord = base;
        chord.notes = { 57, 64 };
        const ms::Result c = session.measure(chord);
        P.eq("metrics.chord_no_inharmonic", c.values.count("inharmonic_db") == 0 ? 1 : 0, 1);
        ms::Request never = base;
        never.dials = { { "release", 3.0f } };
        never.tailSeconds = 0.5f;
        const ms::Result n = session.measure(never);
        P.eq("metrics.never_is_infinity", std::isinf(at(n.values, "release_s")) && std::isinf(at(n.values, "voice_end_s")) ? 1 : 0, 1);
    }
    else
    {
        ms::Request r = base;
        r.dials = { { "gain", 6.0f } };
        const ms::Result m = session.measure(r);
        std::printf("%s", m.report.c_str());
        P.near("metrics.sine_gain_db", at(m.values, "sine_gain_db"), 6.0, 0.05);
        P.near("metrics.noise_gain_db", at(m.values, "noise_gain_db"), 6.0, 0.1);
        P.near("metrics.impulse_peak_db", at(m.values, "impulse_peak_db"), -6.0206 + 6.0, 0.05);
        P.near("metrics.impulse_delay_ms", at(m.values, "impulse_delay_ms"), 0.0, 0.05);
        P.le("metrics.silence_db", at(m.values, "silence_db"), -200.0);
        P.le("metrics.clean_thd", at(m.values, "thd_db"), -80.0);
        ms::Request driven = base;
        driven.dials = { { "drive", 0.5f } };
        const double thd = at(session.measure(driven).values, "thd_db");
        std::printf("THD+N: clean %.1f dB, drive 0.5 %.1f dB\n", at(m.values, "thd_db"), thd);
        P.ge("metrics.driven_thd", thd, -40.0);
        ms::Request dark = base;
        dark.dials = { { "tone", 500.0f } };
        const ms::Result d = session.measure(dark);
        P.eq("metrics.darker_lower_centroid",
             at(d.values, "centroid_out_hz") < 0.5 * at(d.values, "centroid_in_hz") ? 1 : 0, 1);
        P.eq("metrics.alerts", static_cast<int64_t>(at(m.values, "alerts")), 0);
    }
    {
        const ms::Result all = session.measure(base);
        int missing = 0, worded = 0;
        for (const ms::MetricInfo& mi : ms::metrics())
        {
            missing += all.values.count(mi.name) == 0 ? 1 : 0;
            worded += std::string(mi.what).size() > 10 ? 1 : 0;
            if (all.values.count(mi.name) == 0)
                std::printf("metric not measured: %s\n", mi.name);
        }
        P.eq("metrics.every_one_measured", missing, 0);
        P.eq("metrics.every_one_worded", worded, static_cast<int64_t>(ms::metrics().size()));
        P.eq("metrics.value_text_never", ms::valueText(kInstrument ? "release_s" : "impulse_decay_s", kInf) == "never" ? 1 : 0, 1);
        P.eq("metrics.value_text_unit", ms::valueText(kInstrument ? "centroid_hz" : "centroid_out_hz", 1234.4) == "1234 Hz" ? 1 : 0, 1);
        P.eq("metrics.unknown_info", ms::metricInfo("no_such_metric") == nullptr ? 1 : 0, 1);
    }

    // ---- SESSION -------------------------------------------------------------------------------------------------------
    {
        ms::Request r = base;
        const ms::Result first = session.measure(r);
        const ms::Result again = session.measure(r);
        P.eq("session.same_again", sameNumbers(first.values, again.values) ? 1 : 0, 1);
        ms::Session fresh(files()), used(files());
        ms::Request other = base;
        other.dials = { { kInstrument ? "level" : "gain", -10.0f } };
        used.measure(other);
        const ms::Result a = fresh.measure(r), b = used.measure(r);
        P.eq("session.restart_as_fresh", sameNumbers(a.values, b.values) && withoutCpu(a.report) == withoutCpu(b.report) ? 1 : 0, 1);
        P.eq("session.run_is_session", withoutCpu(ms::run(files(), r)) == withoutCpu(a.report) ? 1 : 0, 1);
        ms::Request few = base;
        few.want = { kInstrument ? "rms_db" : "sine_gain_db" };
        const ms::Result part = session.measure(few);
        P.eq("session.want_measures_it", part.values.count(kInstrument ? "rms_db" : "sine_gain_db") == 1 ? 1 : 0, 1);
        P.eq("session.want_skips_the_rest",
             part.values.count(kInstrument ? "chord_peak_db" : "impulse_peak_db") == 0 && part.values.size() < first.values.size() ? 1 : 0, 1);
    }

    // ---- GOALS ---------------------------------------------------------------------------------------------------------
    {
        const char* hz = kInstrument ? "centroid_hz" : "centroid_out_hz";
        const char* sec = kInstrument ? "release_s" : "impulse_decay_s";
        const ms::Goal t = target(hz, 2000.0);               // 5 % of 2000 Hz: 100 Hz
        P.near("goals.relative_tolerance", ms::toleranceOf(t), 100.0, 1e-9);
        P.eq("goals.target_met", ms::met(t, 2090.0) && !ms::met(t, 2110.0) ? 1 : 0, 1);
        P.near("goals.penalty_target", ms::penalty(t, 2200.0), 4.0, 1e-9);
        const ms::Goal z = target(sec, 0.0);
        P.near("goals.tolerance_floor", ms::toleranceOf(z), 0.002, 1e-12);
        const ms::Goal most = atMost(sec, 2.0), least = atLeast(sec, 2.0);
        P.eq("goals.at_most", ms::met(most, 1.9) && !ms::met(most, kInf) ? 1 : 0, 1);
        P.eq("goals.at_least_never", ms::met(least, kInf) ? 1 : 0, 1);
        P.eq("goals.unmeasured", !ms::met(most, std::nan("")) && ms::penalty(most, std::nan("")) == 1.0e6 ? 1 : 0, 1);
        // Far off, still a slope: 30 tolerances costs more than 25.
        P.eq("goals.far_still_slopes", ms::penalty(t, 2000.0 + 30.0 * 100.0) > ms::penalty(t, 2000.0 + 25.0 * 100.0) ? 1 : 0, 1);
        P.eq("goals.within_bounds_zero", ms::penalty(most, 1.0) == 0.0 ? 1 : 0, 1);
        ms::Goal bad;
        bad.metric = "loudness";
        bad.hasMax = true;
        P.eq("goals.unknown_refused", ms::invalid(bad).find("no metric 'loudness'") != std::string::npos ? 1 : 0, 1);
        ms::Goal none;
        none.metric = hz;
        P.eq("goals.no_bound_refused", !ms::invalid(none).empty() ? 1 : 0, 1);
        ms::Goal both = t;
        both.hasMax = true;
        P.eq("goals.target_and_bound_refused", !ms::invalid(both).empty() ? 1 : 0, 1);
        std::printf("goals: %s; %s; %s\n", ms::goalText(t).c_str(), ms::goalText(most).c_str(),
                    ms::goalText(atLeast("dc", -0.01)).c_str());
    }

    // ---- CHECKS --------------------------------------------------------------------------------------------------------
    {
        std::vector<ms::Check> checks;
        const auto add = [&](const char* label, ms::Goal g, ms::Request r) {
            checks.push_back({ label, std::move(g), std::move(r) });
        };
        if constexpr (kInstrument)
        {
            ms::Goal level;
            level.metric = "rms_db";
            level.hasMin = level.hasMax = true;
            level.min = -40.0;
            level.max = -10.0;
            add("A sensible level", level, base);
            add("Releases within 0.2 s", atMost("release_s", 0.2), base);   // 0.5 s: fails
            ms::Request two = base;
            two.notes = { 57, 64 };
            add("No aliasing", atMost("inharmonic_db", -80.0), two);        // two notes: not measured
            add("In tune", target("pitch_cents", 0.0), base);
        }
        else
        {
            add("Unity gain", target("sine_gain_db", 0.0), base);
            add("Clean", atMost("thd_db", -60.0), base);
            ms::Request loud = base;
            loud.dials = { { "gain", 12.0f } };
            add("Quiet when turned up", atMost("sine_gain_db", 3.0), loud);  // fails
            add("Nothing from silence", atMost("silence_db", -90.0), base);
        }
        const std::vector<ms::CheckResult> res = ms::evaluate(files(), checks, {});
        for (std::size_t i = 0; i < res.size(); ++i)
            std::printf("check %-22s %s %s\n", checks[i].label.c_str(), res[i].measured ? (res[i].pass ? "PASS" : "FAIL") : "----",
                        res[i].text.c_str());
        P.eq("checks.count", static_cast<int64_t>(res.size()), 4);
        if (res.size() == 4)
        {
            P.eq("checks.pass", res[0].measured && res[0].pass ? 1 : 0, 1);
            P.eq("checks.fail", res[kInstrument ? 1 : 2].measured && !res[kInstrument ? 1 : 2].pass ? 1 : 0, 1);
            if constexpr (kInstrument)
                P.eq("checks.one_note_only", !res[2].measured && res[2].text.find("one note") != std::string::npos ? 1 : 0, 1);
            else
                P.eq("checks.clean", res[1].measured && res[1].pass ? 1 : 0, 1);
            P.eq("checks.last", res[3].measured && res[3].pass ? 1 : 0, 1);
            P.eq("checks.text", res[0].text.find(kInstrument ? "dBFS" : "dB") != std::string::npos ? 1 : 0, 1);
        }
        std::string errors;
        const std::vector<ms::CheckResult> broken = ms::evaluate(pg::singleFile("module Nothing {};\n"), checks, {}, &errors);
        P.eq("checks.no_compile", std::none_of(broken.begin(), broken.end(), [](const ms::CheckResult& r) { return r.measured; })
                                      && !errors.empty() ? 1 : 0, 1);
    }

    // ---- TUNE ----------------------------------------------------------------------------------------------------------
    {
        const auto report = [](const char* what, const ms::TuneResult& t) {
            std::printf("tune %s: %s%s\n", what, t.error.c_str(), t.text.c_str());
        };
        ms::TuneRequest q;
        q.render = base;
        if constexpr (kInstrument)
        {
            q.dials = { "level" };
            q.goals = { target("rms_db", -30.0, 0.25) };
            const ms::TuneResult a = ms::tune(files(), q);
            report("level", a);
            P.eq("tune.level_met", a.error.empty() && a.bestMet.size() == 1 && a.bestMet[0] ? 1 : 0, 1);
            P.le("tune.level_budget", a.renders, q.budget);
            P.near("tune.level_rms", at(a.bestValues, "rms_db"), -30.0, 0.25);
            // The level is a gain in dB: it moves as far as the RMS had to.
            P.near("tune.level_moved", a.best.count("level") != 0 ? static_cast<double>(a.best.at("level")) : 0.0,
                   static_cast<double>(a.start.at("level")) + (-30.0 - at(a.startValues, "rms_db")), 0.3);
            ms::TuneRequest r = q;
            r.dials = { "release" };
            r.goals = { target("release_s", 1.2, 0.04) };
            r.render.tailSeconds = 3.0f;
            const ms::TuneResult b = ms::tune(files(), r);
            report("release", b);
            P.eq("tune.release_met", b.bestMet.size() == 1 && b.bestMet[0] ? 1 : 0, 1);
            P.near("tune.release_dial", b.best.count("release") != 0 ? static_cast<double>(b.best.at("release")) : 0.0, 1.2, 0.06);
            ms::TuneRequest two = q;
            two.dials = { "level", "release" };
            two.goals = { target("rms_db", -26.0, 0.3), target("release_s", 0.8, 0.03) };
            const ms::TuneResult c = ms::tune(files(), two);
            report("two", c);
            P.eq("tune.two_met", c.bestMet.size() == 2 && c.bestMet[0] && c.bestMet[1] ? 1 : 0, 1);
            ms::TuneRequest again = two;
            const ms::TuneResult c2 = ms::tune(files(), again);
            P.eq("tune.deterministic", c2.best == c.best && c2.renders == c.renders ? 1 : 0, 1);
            ms::TuneRequest intDial = q;
            intDial.dials = { "wave" };
            P.eq("tune.int_refused", ms::tune(files(), intDial).error.find("float dials only") != std::string::npos ? 1 : 0, 1);
        }
        else
        {
            q.dials = { "gain" };
            q.goals = { target("sine_gain_db", 6.0, 0.1) };
            const ms::TuneResult a = ms::tune(files(), q);
            report("gain", a);
            P.eq("tune.gain_met", a.error.empty() && a.bestMet.size() == 1 && a.bestMet[0] ? 1 : 0, 1);
            P.le("tune.gain_budget", a.renders, q.budget);
            P.near("tune.gain_dial", a.best.count("gain") != 0 ? static_cast<double>(a.best.at("gain")) : 0.0, 6.0, 0.15);
            ms::TuneRequest r = q;
            r.dials = { "drive" };
            r.goals = { target("thd_db", -30.0, 1.0) };
            const ms::TuneResult b = ms::tune(files(), r);
            report("drive", b);
            P.eq("tune.drive_met", b.bestMet.size() == 1 && b.bestMet[0] ? 1 : 0, 1);
            ms::TuneRequest two = q;
            two.dials = { "gain", "tone" };
            two.goals = { target("sine_gain_db", -3.0, 0.2), target("centroid_out_hz", 7000.0) };
            const ms::TuneResult c = ms::tune(files(), two);
            report("two", c);
            P.eq("tune.two_met", c.bestMet.size() == 2 && c.bestMet[0] && c.bestMet[1] ? 1 : 0, 1);
            const ms::TuneResult c2 = ms::tune(files(), two);
            P.eq("tune.deterministic", c2.best == c.best && c2.renders == c.renders ? 1 : 0, 1);
            ms::TuneRequest intDial = q;
            intDial.dials = { "mode" };
            P.eq("tune.int_refused", ms::tune(files(), intDial).error.find("float dials only") != std::string::npos ? 1 : 0, 1);
        }
        ms::TuneRequest already = q;
        already.goals = { atMost("alerts", 0.5) };
        const ms::TuneResult d = ms::tune(files(), already);
        P.eq("tune.met_already_one_render", d.renders == 1 && d.bestMet.size() == 1 && d.bestMet[0] ? 1 : 0, 1);
        P.eq("tune.met_already_said", d.text.find("met already") != std::string::npos ? 1 : 0, 1);
        ms::TuneRequest unknown = q;
        unknown.dials = { "nothing" };
        P.eq("tune.unknown_dial", ms::tune(files(), unknown).error.find("no dial 'nothing'") != std::string::npos ? 1 : 0, 1);
        ms::TuneRequest empty = q;
        empty.goals.clear();
        P.eq("tune.no_goal", !ms::tune(files(), empty).error.empty() ? 1 : 0, 1);
        ms::TuneRequest stop = q;
        stop.goals = { target(kInstrument ? "rms_db" : "sine_gain_db", kInstrument ? -35.0 : 9.0, 0.01) };
        stop.progress = [](int done, int) { return done < 5; };
        const ms::TuneResult s = ms::tune(files(), stop);
        P.eq("tune.progress_stops", s.stopped && s.renders == 5 ? 1 : 0, 1);
    }

    // ---- VIEW ----------------------------------------------------------------------------------------------------------
    {
        ms::ViewRequest vr;
        vr.render = base;
        vr.render.holdSeconds = 1.0f;
        vr.render.tailSeconds = 0.5f;
        if constexpr (kInstrument)
        {
            vr.render.notes = { 69 };
            vr.render.dials = { { "wave", 1.0f }, { "cutoff", 10000.0f } };   // a sine
        }
        else
        {
            vr.input = ms::ViewInput::sine;
            vr.frequency = 440.0f;
        }
        const ms::View v = ms::view(files(), vr);
        P.eq("view.compiles", v.compiled ? 1 : 0, 1);
        const int mid = v.columns / 3;                           // in the held part
        int loudRow = 0;
        for (int r = 0; r < v.rows; ++r)
            if (v.cells[static_cast<std::size_t>(r * v.columns + mid)] > v.cells[static_cast<std::size_t>(loudRow * v.columns + mid)])
                loudRow = r;
        std::printf("view: %s; the loudest row %d (%.0f Hz), 440 Hz at row %.1f; close-up %zu samples\n", v.title.c_str(),
                    loudRow, ms::hzOfRow(v, loudRow + 0.5), ms::rowOf(v, 440.0), v.zoomTo - v.zoomFrom);
        P.near("view.spectrogram_row", static_cast<double>(loudRow) + 0.5, ms::rowOf(v, 440.0), 1.5);
        std::size_t loudCol = 0;
        for (std::size_t c = 0; c < v.spectrum.size(); ++c)
            if (v.spectrum[c] > v.spectrum[loudCol])
                loudCol = c;
        const double spectrumAt = static_cast<double>(v.spectrum.size()) * std::log(440.0 / v.loHz) / std::log(v.hiHz / v.loHz);
        P.near("view.spectrum_column", static_cast<double>(loudCol) + 0.5, spectrumAt, 2.0);
        P.near("view.close_up_four_cycles", static_cast<double>(v.zoomTo - v.zoomFrom), 4.0 * 48000.0 / 440.0, 2.0);
        P.eq("view.waveform_columns", v.minL.size() == static_cast<std::size_t>(v.columns) && v.levelDb.size() == v.minL.size() ? 1 : 0, 1);
        float top = 0.0f;
        for (std::size_t c = 0; c < v.maxL.size(); ++c)
            top = std::max(top, v.maxL[c]);
        P.near("view.waveform_peak_db", 20.0 * std::log10(std::max(1e-9, static_cast<double>(top))), v.peakDb, 0.05);
        P.near("view.held_until", v.heldUntil, 1.0, 1e-9);
        P.eq("view.not_stereo", v.stereo ? 0 : 1, 1);
        if constexpr (!kInstrument)
        {
            ms::ViewRequest sw = vr;
            sw.input = ms::ViewInput::sweep;
            const ms::View s = ms::view(files(), sw);
            const auto loudest = [&](int c) {
                int best = 0;
                for (int r = 0; r < s.rows; ++r)
                    if (s.cells[static_cast<std::size_t>(r * s.columns + c)] > s.cells[static_cast<std::size_t>(best * s.columns + c)])
                        best = r;
                return best;
            };
            const int early = loudest(s.columns / 6), late = loudest(s.columns / 2);
            std::printf("view: the sweep's loudest row %d early, %d later\n", early, late);
            P.eq("view.sweep_rises", late > early + 40 ? 1 : 0, 1);
        }
        ms::ViewRequest zoom = vr;
        zoom.from = 0.5f;
        zoom.to = 0.51f;
        const ms::View z = ms::view(files(), zoom);
        P.eq("view.close_up_asked", z.zoomFrom == 24000 && z.zoomTo == 24480 ? 1 : 0, 1);
    }
    return P.finish();
}
