// PROBE name=fminus.library timeout=300
//
// fminus.library: the editor's LIBRARY (Source/fminus/library, compiled in as Library.h), each file #included by a
// harness of its own and run by FMinus's JIT at 48 kHz, its output measured here:
//   FILES     every file's first line names it and says what it is, a usage header follows, and its name is a tab's;
//             each compiles alone with no message, and all of them together (no names clash)
//   SVF       low-pass: -3 dB at the cutoff with resonance 0.3 (Butterworth), below -38 dB a decade above; high-pass
//             the mirror; band-pass 0 dB at the cutoff; notch below -60 dB there; resonance 1 peaks 20 to 30 dB
//   LADDER    -12 dB at the cutoff, below -80 dB a decade above, 0 dB a decade below; at resonance 1 it sings on its
//             own after an impulse, a sine at the cutoff (within 1 %), bounded (below 0.5); at 0.9 it dies away
//   ONEPOLE   low- and high-pass -3 dB at the cutoff; the DC blocker takes a DC offset out and passes 100 Hz
//   BIQUAD    low-pass -3 dB at fc (q 0.707) and below -38 dB a decade above; a bell of +6 and -12 dB at fc; shelves of
//             +6 dB below and -6 dB above, half of it at fc, 0 dB far on the other side; a notch below -60 dB
//   DELAY     an impulse comes out exactly d samples later through read, readCubic and tap (d = 1, 37, 500); a delay of
//             half a sample splits it; the four-point read loses less at 10 kHz than the linear one; every call is
//             safe before allocate
//   REVERB    an impulse's tail falls 60 dB in about 1.3 s at size 0.5 and over 6 s at size 1; the sides are
//             decorrelated at width 1 and alike at width 0; noise through it comes out within 3 dB of its level; it
//             stays finite
//   OSC       at 440 Hz each wave has its RMS (saw and triangle 0.577, square 1, sine 0.707), no DC, 440 rising zero
//             crossings a second; a 25 % pulse has no DC; the morph keeps its fundamental between saw and square;
//             at 3.7 kHz the saw's strongest alias below 16 kHz is at least 10 dB under a naive saw's
//   WAVETABLE each shape at 440 Hz with its RMS (sine 0.707, triangle and saw 0.577, square 1), no DC, 440 rising zero
//             crossings; the saw at 1, 3.7, 7 and 11 kHz with nothing that is no harmonic above -80 dB below 20 kHz (a
//             naive saw's aliases are above -40 dB); at the end of a fade the 16th harmonic of 1061 Hz (17 kHz) at a
//             saw's level; a glide's copies move continuously and only up; a saw-to-square morph keeps its
//             fundamental (0.764 halfway); the copies' harmonics; every call safe before allocate and build
//   NOISE     white is uniform in -1..1 (RMS 0.577, no DC); pink and brown as loud (within 10 %); the low end
//             outweighs the top more from white to pink to brown; two seeds differ
//   ADSR      the attack reaches 1 in its time (within 0.1 ms), the decay settles on the sustain, the release ends
//             within 1.4 times its time; a gate while it sounds rises from where it is, without a step
//   LFO       every shape stays in -1..1; the square flips twice a cycle; the random step changes once a cycle
//   FOLLOWER  63 % of a step in the attack time, 37 % left after the release time; the compressor's gain curve: 0 under
//             the threshold, -(1 - 1/ratio) of the excess over it, the soft knee's middle
//   SHAPER    drive and fold keep -1..1 (drive maps 1 to 1); soft clipping stays under 1; the crusher's steps
//   UTIL      the conversions (440 Hz at note 69, a fifth, cents, dB), equal-power panning, the smoother's time
#include "Probe.h"

#include "Audio.h"
#include "Rig.h"

#include "Library.h"

#include "dsp/Measure.h"
#include "dsp/Sources.h"

#include "fminus/FMinus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr float kRate = 48000.0f;

    // A harness that #includes library files, compiled with the library behind its #includes.
    std::unique_ptr<fminus::Program> compileHarness(const std::string& source, std::string& errors)
    {
        fminus::Program::Options o;
        o.fileName = "harness.fm";
        o.denormals = fminus::Program::Denormals::Flush;
        o.include = [](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            if (const tp::LibraryFile* f = tp::findLibraryFile(name))
                return fminus::Program::IncludedFile{ f->name, f->source };
            return std::nullopt;
        };
        return fminus::Program::compile(source, errors, o);
    }

    template <typename Signature>
    Signature* function(fminus::Program* p, const char* name)
    {
        if (p == nullptr)
            return nullptr;
        std::string errors;
        Signature* f = p->function<Signature>(name, errors);
        if (f == nullptr)
            std::printf("%s: %s\n", name, errors.c_str());
        return f;
    }

    // The RMS of `out` over that of `in`, in dB, over the second half (past the filters' settling).
    double gainDb(const std::vector<float>& in, const std::vector<float>& out)
    {
        const std::size_t h = in.size() / 2;
        return probe::rmsDb(out, h, in.size() - h) - probe::rmsDb(in, h, in.size() - h);
    }

    double rms(const std::vector<float>& x, std::size_t from = 0)
    {
        double s = 0.0;
        for (std::size_t i = from; i < x.size(); ++i)
            s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, x.size() - from)));
    }

    double mean(const std::vector<float>& x)
    {
        double s = 0.0;
        for (const float v : x)
            s += static_cast<double>(v);
        return s / static_cast<double>(std::max<std::size_t>(1, x.size()));
    }

    int risingCrossings(const std::vector<float>& x)
    {
        int n = 0;
        for (std::size_t i = 1; i < x.size(); ++i)
            n += x[i - 1] < 0.0f && x[i] >= 0.0f ? 1 : 0;
        return n;
    }

    // The ratio in dB of a signal's energy under a one-pole low-pass at 200 Hz to that over a high-pass at 5 kHz.
    double tilt(const std::vector<float>& x)
    {
        const double alo = 1.0 - std::exp(-2.0 * std::numbers::pi * 200.0 / kFs);
        const double ahi = 1.0 - std::exp(-2.0 * std::numbers::pi * 5000.0 / kFs);
        double lo = 0.0, hi = 0.0, elo = 0.0, ehi = 0.0;
        for (const float v : x)
        {
            lo += alo * (static_cast<double>(v) - lo);
            hi += ahi * (static_cast<double>(v) - hi);
            elo += lo * lo;
            ehi += (static_cast<double>(v) - hi) * (static_cast<double>(v) - hi);
        }
        return 10.0 * std::log10(elo / ehi);
    }

    // The strongest component of `x` below 16 kHz (where an alias is heard) that is not a harmonic of f0, in dB
    // relative to the strongest.
    double worstAlias(const std::vector<float>& x, double f0)
    {
        const std::vector<tp::measure::Peak> peaks = tp::measure::spectrumPeaks(x, kFs, 64);
        double worst = -200.0;
        for (const tp::measure::Peak& p : peaks)
        {
            const double k = std::round(p.hz / f0);
            if (p.hz < 16000.0 && std::fabs(p.hz - k * f0) > 30.0)
                worst = std::max(worst, p.db);
        }
        return worst;
    }

    // ---- the harnesses: each library file's modules run over buffers ------------------------------------------------
    const char* const kFilters = R"FM(#include "svf.fm"
#include "ladder.fm"
#include "onepole.fm"
#include "biquad.fm"

void svfRun(float* x, float* y, int n, float fc, float res, int mode)
{
    Svf f;
    f.set(fc, res, 48000);
    for (int i = 0; i < n; ++i)
    {
        f.tick(x[i]);
        y[i] = mode == 0 ? f.lp : (mode == 1 ? f.bp : (mode == 2 ? f.hp : f.notch));
    }
}

void ladderRun(float* x, float* y, int n, float fc, float res)
{
    Ladder f;
    f.set(fc, res, 48000);
    for (int i = 0; i < n; ++i)
        y[i] = f.tick(x[i]);
}

void onepoleRun(float* x, float* y, int n, float fc, int mode)
{
    OnePole f;
    DcBlock dc;
    f.set(fc, 48000);
    dc.set(48000);
    for (int i = 0; i < n; ++i)
        y[i] = mode == 0 ? f.lowpass(x[i]) : (mode == 1 ? f.highpass(x[i]) : dc.tick(x[i]));
}

void biquadRun(float* x, float* y, int n, float fc, float q, float db, int mode)
{
    Biquad f;
    if (mode == 0)
        f.setLowpass(fc, q, 48000);
    if (mode == 1)
        f.setHighpass(fc, q, 48000);
    if (mode == 2)
        f.setBandpass(fc, q, 48000);
    if (mode == 3)
        f.setNotch(fc, q, 48000);
    if (mode == 4)
        f.setPeak(fc, q, db, 48000);
    if (mode == 5)
        f.setLowShelf(fc, db, 48000);
    if (mode == 6)
        f.setHighShelf(fc, db, 48000);
    for (int i = 0; i < n; ++i)
        y[i] = f.tick(x[i]);
}
)FM";

    const char* const kDelays = R"FM(#include "delay.fm"
#include "reverb.fm"

void delayRun(float* x, float* y, int n, float d, int mode)
{
    Delay line;
    line.allocate(1000);
    for (int i = 0; i < n; ++i)
    {
        y[i] = mode == 0 ? line.read(d) : (mode == 1 ? line.readCubic(d) : line.tap((int)d));
        line.write(x[i]);
    }
    line.release();
}

float delayUnallocated()
{
    Delay line;
    line.write(1);
    line.clear();
    return line.read(5) + line.readCubic(5) + line.tap(3);
}

void reverbRun(float* inL, float* inR, float* outL, float* outR, int n, float size, float damping, float width)
{
    Reverb verb;
    verb.allocate(48000);
    verb.set(size, damping, width);
    for (int i = 0; i < n; ++i)
    {
        verb.tick(inL[i], inR[i]);
        outL[i] = verb.left;
        outR[i] = verb.right;
    }
    verb.release();
}
)FM";

    const char* const kSources = R"FM(#include "osc.fm"
#include "noise.fm"

void oscRun(float* y, int n, float step, int wave, float width)
{
    Osc o;
    for (int i = 0; i < n; ++i)
    {
        if (wave == 0)
            y[i] = o.saw(step);
        else if (wave == 1)
            y[i] = o.pulse(step, width);
        else if (wave == 2)
            y[i] = o.triangle(step);
        else if (wave == 3)
            y[i] = o.sine(step);
        else
            y[i] = o.morph(step, width);
    }
}

void noiseRun(float* y, int n, int kind, int seed)
{
    Noise noise;
    if (seed > 0)
        noise.seed(seed);
    for (int i = 0; i < n; ++i)
        y[i] = kind == 0 ? noise.white() : (kind == 1 ? noise.pink() : noise.brown());
}
)FM";

    const char* const kWavetable = R"FM(#include "wavetable.fm"

// A table of `frames` frames, the shapes from `first` on, read by one oscillator at `step` for n samples.
void wtRun(float* y, int n, float step, float position, int first, int frames)
{
    Wavetable t;
    t.allocate(frames);
    for (int f = 0; f < frames; ++f)
        t.shape(f, first + f);
    WaveOsc o;
    o.table = &t;
    for (int i = 0; i < n; ++i)
        y[i] = o.tick(step, position);
    t.release();
}

// A saw gliding from step s0 to s1, exponentially, over n samples, and the oscillator's level each sample.
void wtGlide(float* y, float* level, int n, float s0, float s1)
{
    Wavetable t;
    t.allocate(1);
    t.shape(0, 2);
    WaveOsc o;
    o.table = &t;
    const float k = expf(logf(s1 / s0) / n);
    float s = s0;
    for (int i = 0; i < n; ++i)
    {
        y[i] = o.tick(s, 0);
        level[i] = o.level;
        s *= k;
    }
    t.release();
}

// Every call before allocate and build: the sum of what they read (silence: 0).
float wtUnbuilt()
{
    Wavetable t;
    WaveOsc o;
    float sum = fabsf(o.tick(0.01f, 0));
    o.table = &t;
    sum += fabsf(o.tick(0.01f, 0.5f));
    float* c = t.cycle(3);
    c[0] = 1;
    t.build(0);
    t.shape(0, 2);
    sum += fabsf(t.at(0.3f, 5, 1));
    t.release();
    t.allocate(2);
    sum += fabsf(o.tick(0.02f, 0.5f));
    t.release();
    t.release();
    return sum;
}

int wtHarmonics(int j)
{
    Wavetable t;
    return t.harmonics(j);
}
)FM";

    const char* const kModulation = R"FM(#include "adsr.fm"
#include "lfo.fm"
#include "follower.fm"

// The envelope's level each sample, gated on at 0, off at `off`, on again at `again` (when above 0); -1 once inactive.
void adsrRun(float* y, int n, float a, float d, float s, float r, int off, int again)
{
    Adsr env;
    env.set(a, d, s, r, 48000);
    env.gate(true);
    for (int i = 0; i < n; ++i)
    {
        if (i == off)
            env.gate(false);
        if (again > 0 && i == again)
            env.gate(true);
        const float v = env.tick();
        y[i] = env.active() ? v : -1.0f;
    }
}

void lfoRun(float* y, int n, float step, int shape, float offset)
{
    Lfo lfo;
    for (int i = 0; i < n; ++i)
    {
        if (shape == 0)
            y[i] = lfo.sine(offset);
        else if (shape == 1)
            y[i] = lfo.triangle(offset);
        else if (shape == 2)
            y[i] = lfo.saw(offset);
        else if (shape == 3)
            y[i] = lfo.square(offset);
        else
            y[i] = lfo.random();
        lfo.advance(step);
    }
}

void followerRun(float* x, float* y, int n, float a, float r, int useRms)
{
    Follower f;
    f.set(a, r, 48000);
    for (int i = 0; i < n; ++i)
        y[i] = useRms == 1 ? f.rms(x[i]) : f.peak(x[i]);
}

float compressor(float db, float threshold, float ratio, float knee)
{
    return compressorGainDb(db, threshold, ratio, knee);
}
)FM";

    const char* const kShaping = R"FM(#include "shaper.fm"
#include "util.fm"

// 0 softClip, 1 hardClip(x, a), 2 drive(x, a), 3 fold(x, a), 4 asym(x, a)
float shape(int which, float x, float a)
{
    if (which == 0)
        return softClip(x);
    if (which == 1)
        return hardClip(x, a);
    if (which == 2)
        return drive(x, a);
    if (which == 3)
        return fold(x, a);
    return asym(x, a);
}

void crushRun(float* x, float* y, int n, float bits, float hold)
{
    Crusher c;
    for (int i = 0; i < n; ++i)
        y[i] = c.tick(x[i], bits, hold);
}

// 0 noteToHz(a), 1 semitones(a), 2 cents(a), 3 dbToGain(a), 4 gainToDb(a), 5 panLeft(a), 6 panRight(a),
// 7 crossfade(a, b, 0.5), 8 lerp(a, b, 0.25), 9 clampf(a, -1, 1)
float util(int which, float a, float b)
{
    if (which == 0)
        return noteToHz(a);
    if (which == 1)
        return semitones(a);
    if (which == 2)
        return cents(a);
    if (which == 3)
        return dbToGain(a);
    if (which == 4)
        return gainToDb(a);
    if (which == 5)
        return panLeft(a);
    if (which == 6)
        return panRight(a);
    if (which == 7)
        return crossfade(a, b, 0.5f);
    if (which == 8)
        return lerp(a, b, 0.25f);
    return clampf(a, -1, 1);
}

// The smoother after `samples` samples toward 1, from 0, at ms.
float smooth(float ms, int samples)
{
    Smooth s;
    s.set(ms, 48000);
    float v = 0;
    for (int i = 0; i < samples; ++i)
        v = s.tick(1);
    return v;
}
)FM";

}

PROBE(fminus, library)
{
    const std::size_t n = static_cast<std::size_t>(kFs);         // one second
    const int N = static_cast<int>(n);

    // ---- FILES -----------------------------------------------------------------------------------------------------
    {
        int named = 0, headed = 0, alone = 0;
        std::string all;
        for (const tp::LibraryFile& f : tp::kLibrary)
        {
            const std::string text = f.source;
            named += text.rfind(std::string("// ") + f.name + ": ", 0) == 0 && tp::validFileName(f.name) ? 1 : 0;
            headed += text.find("\n//\n") != std::string::npos ? 1 : 0;
            std::string errors;
            const auto p = compileHarness(std::string("#include \"") + f.name + "\"\nfloat one() { return 1; }\n", errors);
            alone += p != nullptr && errors.empty() ? 1 : 0;
            if (!errors.empty())
                std::printf("%s alone:\n%s", f.name, errors.c_str());
            all += std::string("#include \"") + f.name + "\"\n";
        }
        const auto count = static_cast<int64_t>(std::size(tp::kLibrary));
        std::printf("the library: %lld files\n", static_cast<long long>(count));
        P.ge("files.count", static_cast<double>(count), 12.0);
        P.eq("files.first_line_names_it", named, count);
        P.eq("files.usage_header", headed, count);
        P.eq("files.compile_alone", alone, count);
        std::string errors;
        const auto together = compileHarness(all + "float one() { return 1; }\n", errors);
        if (!errors.empty())
            std::printf("all together:\n%s", errors.c_str());
        P.eq("files.compile_together", together != nullptr && errors.empty() ? 1 : 0, 1);
    }

    // ---- SVF, LADDER, ONEPOLE, BIQUAD --------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kFilters, errors);
        P.eq("filters.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* svf = function<void(float*, float*, int, float, float, int)>(p.get(), "svfRun");
        auto* ladder = function<void(float*, float*, int, float, float)>(p.get(), "ladderRun");
        auto* onepole = function<void(float*, float*, int, float, int)>(p.get(), "onepoleRun");
        auto* biquad = function<void(float*, float*, int, float, float, float, int)>(p.get(), "biquadRun");
        std::vector<float> y(n);
        const auto tone = [&](double hz) { return probe::sine(hz, 0.25, kFs, n); };
        if (svf != nullptr)
        {
            const auto g = [&](double hz, float fc, float res, int mode) {
                std::vector<float> x = tone(hz);
                svf(x.data(), y.data(), N, fc, res, mode);
                return gainDb(x, y);
            };
            P.near("svf.lowpass_at_fc_res_0.3", g(1000, 1000, 0.3f, 0), -3.0, 0.15);
            P.le("svf.lowpass_decade_above", g(10000, 1000, 0.0f, 0), -38.0);
            P.near("svf.lowpass_decade_below", g(100, 1000, 0.0f, 0), 0.0, 0.2);
            P.le("svf.highpass_decade_below", g(100, 1000, 0.0f, 2), -38.0);
            P.near("svf.highpass_decade_above", g(10000, 1000, 0.0f, 2), 0.0, 0.2);
            P.near("svf.bandpass_at_fc", g(1000, 1000, 0.5f, 1), 0.0, 0.1);
            P.le("svf.notch_at_fc", g(1000, 1000, 0.5f, 3), -60.0);
            P.in("svf.resonance_peak", g(1000, 1000, 1.0f, 0), 20.0, 30.0);
        }
        if (ladder != nullptr)
        {
            const auto g = [&](double hz, float fc, float res) {
                std::vector<float> x = probe::sine(hz, 0.05, kFs, n);
                ladder(x.data(), y.data(), N, fc, res);
                return gainDb(x, y);
            };
            P.near("ladder.at_fc", g(1000, 1000, 0.0f), -12.0, 0.3);
            P.le("ladder.decade_above", g(10000, 1000, 0.0f), -80.0);
            P.near("ladder.decade_below", g(100, 1000, 0.0f), 0.0, 0.3);
            // An impulse, then silence: at resonance 1 it sings at the cutoff.
            std::vector<float> kick(n, 0.0f);
            kick[0] = 0.5f;
            ladder(kick.data(), y.data(), N, 1000.0f, 1.0f);
            const std::vector<float> tail(y.begin() + static_cast<std::ptrdiff_t>(n / 2), y.end());
            P.ge("ladder.self_oscillates", rms(tail), 0.1);
            P.near("ladder.sings_at_fc", static_cast<double>(risingCrossings(tail)) * 2.0, 1000.0, 10.0);
            P.le("ladder.bounded", static_cast<double>(probe::peak(y)), 0.5);
            ladder(kick.data(), y.data(), N, 1000.0f, 0.9f);
            P.le("ladder.dies_away_at_0.9", rms(std::vector<float>(y.begin() + static_cast<std::ptrdiff_t>(n / 2), y.end())), 1e-4);
        }
        if (onepole != nullptr)
        {
            std::vector<float> x = tone(1000);
            onepole(x.data(), y.data(), N, 1000.0f, 0);
            P.near("onepole.lowpass_at_fc", gainDb(x, y), -3.01, 0.05);
            onepole(x.data(), y.data(), N, 1000.0f, 1);
            P.near("onepole.highpass_at_fc", gainDb(x, y), -3.01, 0.05);
            std::vector<float> dc = probe::sine(100, 0.25, kFs, n);
            for (float& v : dc)
                v += 0.5f;
            onepole(dc.data(), y.data(), N, 0.0f, 2);
            const std::vector<float> late(y.begin() + static_cast<std::ptrdiff_t>(n / 2), y.end());
            P.le("onepole.dcblock_takes_dc", std::fabs(mean(late)), 0.002);
            P.near("onepole.dcblock_passes_100hz", probe::dbFromLin(rms(late)), probe::dbFromLin(0.25 / std::sqrt(2.0)), 0.1);
        }
        if (biquad != nullptr)
        {
            const auto g = [&](double hz, float fc, float q, float db, int mode) {
                std::vector<float> x = tone(hz);
                biquad(x.data(), y.data(), N, fc, q, db, mode);
                return gainDb(x, y);
            };
            P.near("biquad.lowpass_at_fc", g(1000, 1000, 0.707f, 0, 0), -3.0, 0.1);
            P.le("biquad.lowpass_decade_above", g(10000, 1000, 0.707f, 0, 0), -38.0);
            P.near("biquad.highpass_at_fc", g(1000, 1000, 0.707f, 0, 1), -3.0, 0.1);
            P.near("biquad.bandpass_at_fc", g(1000, 1000, 2.0f, 0, 2), 0.0, 0.05);
            P.le("biquad.notch_at_fc", g(1000, 1000, 2.0f, 0, 3), -60.0);
            P.near("biquad.peak_boost", g(1000, 1000, 1.0f, 6, 4), 6.0, 0.05);
            P.near("biquad.peak_cut", g(1000, 1000, 1.0f, -12, 4), -12.0, 0.05);
            P.near("biquad.low_shelf_below", g(20, 200, 0, 6, 5), 6.0, 0.1);
            P.near("biquad.low_shelf_at_fc", g(200, 200, 0, 6, 5), 3.0, 0.1);
            P.near("biquad.low_shelf_above", g(8000, 200, 0, 6, 5), 0.0, 0.05);
            P.near("biquad.high_shelf_above", g(20000, 5000, 0, -6, 6), -6.0, 0.1);
            P.near("biquad.high_shelf_below", g(100, 5000, 0, -6, 6), 0.0, 0.05);
        }
    }

    // ---- DELAY, REVERB -----------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kDelays, errors);
        P.eq("delays.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* delay = function<void(float*, float*, int, float, int)>(p.get(), "delayRun");
        auto* unallocated = function<float()>(p.get(), "delayUnallocated");
        auto* reverb = function<void(float*, float*, float*, float*, int, float, float, float)>(p.get(), "reverbRun");
        if (delay != nullptr)
        {
            constexpr int len = 2000;
            std::vector<float> x(len, 0.0f), y(len);
            x[10] = 1.0f;
            int exact = 0, tries = 0;
            for (const float d : { 1.0f, 37.0f, 500.0f })
                for (const int mode : { 0, 1, 2 })
                {
                    if (mode == 1 && d < 2.0f)
                        continue;                        // the four-point read needs two samples
                    delay(x.data(), y.data(), len, d, mode);
                    const auto at = std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); });
                    exact += at - y.begin() == 10 + static_cast<int>(d) && std::fabs(*at - 1.0f) < 1e-6f ? 1 : 0;
                    ++tries;
                }
            P.eq("delay.impulse_exact", exact, tries);
            delay(x.data(), y.data(), len, 100.5f, 0);
            P.eq("delay.half_sample_splits", std::fabs(y[110] - 0.5f) < 1e-6f && std::fabs(y[111] - 0.5f) < 1e-6f ? 1 : 0, 1);
            std::vector<float> s = probe::sine(10000, 0.25, kFs, n), out(n);
            delay(s.data(), out.data(), N, 100.5f, 0);
            const double linear = gainDb(s, out);
            delay(s.data(), out.data(), N, 100.5f, 1);
            const double cubic = gainDb(s, out);
            std::printf("a half-sample delay at 10 kHz: linear %.2f dB, four-point %.2f dB\n", linear, cubic);
            P.ge("delay.cubic_beats_linear_db", cubic - linear, 1.0);
        }
        if (unallocated != nullptr)
            P.eq("delay.safe_unallocated", unallocated() == 0.0f ? 1 : 0, 1);
        if (reverb != nullptr)
        {
            const std::size_t m = static_cast<std::size_t>(12.0 * kFs);
            std::vector<float> l(m, 0.0f), r(m, 0.0f), ol(m), orr(m);
            l[0] = r[0] = 1.0f;
            const auto t60 = [&](float size) {
                reverb(l.data(), r.data(), ol.data(), orr.data(), static_cast<int>(m), size, 0.5f, 1.0f);
                std::vector<double> e(m);
                for (std::size_t i = 0; i < m; ++i)
                    e[i] = static_cast<double>(ol[i]) * static_cast<double>(ol[i]) + static_cast<double>(orr[i]) * static_cast<double>(orr[i]);
                return probe::decayFit(e, kFs).t60;
            };
            const double half = t60(0.5f), full = t60(1.0f);
            std::printf("reverb T60: %.2f s at size 0.5, %.2f s at size 1\n", half, full);
            P.in("reverb.t60_size_0.5", half, 0.9, 1.8);
            P.ge("reverb.t60_size_1", full, 6.0);
            P.eq("reverb.finite", probe::allFinite(ol) && probe::allFinite(orr) ? 1 : 0, 1);
            const auto correlation = [&](float width) {
                reverb(l.data(), r.data(), ol.data(), orr.data(), static_cast<int>(m), 0.5f, 0.5f, width);
                double lr = 0.0, ll = 0.0, rr = 0.0;
                for (std::size_t i = 0; i < m; ++i)
                {
                    lr += static_cast<double>(ol[i]) * static_cast<double>(orr[i]);
                    ll += static_cast<double>(ol[i]) * static_cast<double>(ol[i]);
                    rr += static_cast<double>(orr[i]) * static_cast<double>(orr[i]);
                }
                return lr / std::sqrt(ll * rr);
            };
            P.le("reverb.wide_decorrelated", std::fabs(correlation(1.0f)), 0.2);
            P.ge("reverb.mono_alike", correlation(0.0f), 0.999);
            const probe::Stereo noise = probe::noise(m, 0.25);
            std::vector<float> nl = noise.l, nr = noise.l;
            reverb(nl.data(), nr.data(), ol.data(), orr.data(), static_cast<int>(m), 0.5f, 0.5f, 1.0f);
            P.near("reverb.noise_level_db", gainDb(nl, ol), 0.0, 3.0);
        }
    }

    // ---- OSC, NOISE ----------------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kSources, errors);
        P.eq("sources.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* osc = function<void(float*, int, float, int, float)>(p.get(), "oscRun");
        auto* noise = function<void(float*, int, int, int)>(p.get(), "noiseRun");
        std::vector<float> y(n);
        if (osc != nullptr)
        {
            const float step = 440.0f / kRate;
            const char* names[] = { "saw", "pulse", "triangle", "sine" };
            const double rmsWant[] = { 0.577, 1.0, 0.577, 0.7071 };
            for (int w = 0; w < 4; ++w)
            {
                osc(y.data(), N, step, w, 0.5f);
                const std::string k = std::string("osc.") + names[w];
                P.near(k + ".rms", rms(y), rmsWant[w], 0.02);
                P.le(k + ".no_dc", std::fabs(mean(y)), 0.002);
                P.near(k + ".crossings", static_cast<double>(risingCrossings(y)), 440.0, 1.0);
            }
            osc(y.data(), N, step, 1, 0.25f);
            P.le("osc.pulse_25_no_dc", std::fabs(mean(y)), 0.002);
            osc(y.data(), N, step, 4, 0.25f);
            const double between = rms(y);
            P.in("osc.morph_keeps_fundamental", between, 0.6, 1.0);
            // Aliasing at 3.7 kHz: the band-limited saw against a naive one.
            const double f0 = 3700.0;
            osc(y.data(), N, static_cast<float>(f0 / kFs), 0, 0.5f);
            std::vector<float> naive(n);
            double ph = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                naive[i] = static_cast<float>(2.0 * ph - 1.0);
                ph += f0 / kFs;
                ph -= std::floor(ph);
            }
            const double ours = worstAlias(y, f0), plain = worstAlias(naive, f0);
            std::printf("the strongest alias at %.0f Hz: %.1f dB (a naive saw's %.1f dB)\n", f0, ours, plain);
            P.ge("osc.saw_alias_under_naive_db", plain - ours, 10.0);
        }
        if (noise != nullptr)
        {
            noise(y.data(), N, 0, 0);
            const std::vector<float> white = y;
            P.near("noise.white_rms", rms(white), 0.5774, 0.01);
            P.le("noise.white_no_dc", std::fabs(mean(white)), 0.01);
            P.le("noise.white_in_range", static_cast<double>(probe::peak(white)), 1.0);
            std::vector<float> pink(n), brown(n), other(n);
            noise(pink.data(), N, 1, 0);
            noise(brown.data(), N, 2, 0);
            P.near("noise.pink_as_loud", rms(pink) / rms(white), 1.0, 0.1);
            P.near("noise.brown_as_loud", rms(brown) / rms(white), 1.0, 0.15);
            const double tw = tilt(white), tpk = tilt(pink), tb = tilt(brown);
            std::printf("low over high: white %.1f dB, pink %.1f dB, brown %.1f dB\n", tw, tpk, tb);
            P.eq("noise.colours_in_order", tw < tpk && tpk < tb ? 1 : 0, 1);
            noise(other.data(), N, 0, 7);
            P.eq("noise.seeds_differ", other != white ? 1 : 0, 1);
        }
    }

    // ---- WAVETABLE ---------------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kWavetable, errors);
        P.eq("wavetable.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* run = function<void(float*, int, float, float, int, int)>(p.get(), "wtRun");
        auto* glide = function<void(float*, float*, int, float, float)>(p.get(), "wtGlide");
        auto* unbuilt = function<float()>(p.get(), "wtUnbuilt");
        auto* harmonics = function<int(int)>(p.get(), "wtHarmonics");
        std::vector<float> y(n);
        if (run != nullptr)
        {
            const char* names[] = { "sine", "triangle", "saw", "square" };
            const double rmsWant[] = { 0.7071, 0.577, 0.577, 1.0 };
            for (int w = 0; w < 4; ++w)
            {
                run(y.data(), N, 440.0f / kRate, 0.0f, w, 1);
                const std::string k = std::string("wavetable.") + names[w];
                P.near(k + ".rms", rms(y), rmsWant[w], 0.02);
                P.le(k + ".no_dc", std::fabs(mean(y)), 0.002);
                P.near(k + ".crossings", static_cast<double>(risingCrossings(y)), 440.0, 1.0);
            }
            for (const double f0 : { 1000.0, 3700.0, 7000.0, 11000.0 })
            {
                run(y.data(), N, static_cast<float>(f0 / kFs), 0.0f, 2, 1);
                const tp::measure::Peak odd = tp::measure::inharmonic(y, kFs, f0, 20000.0);
                std::printf("the wavetable saw at %.0f Hz: nothing that is no harmonic above %.1f dB (at %.0f Hz)\n", f0,
                            odd.db, odd.hz);
                P.le("wavetable.saw_" + std::to_string(static_cast<int>(f0)) + "_hz_no_alias_db", odd.db, -80.0);
            }
            {
                const double f0 = 3700.0;
                std::vector<float> naive(n);
                double ph = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    naive[i] = static_cast<float>(2.0 * ph - 1.0);
                    ph += f0 / kFs;
                    ph -= std::floor(ph);
                }
                const tp::measure::Peak odd = tp::measure::inharmonic(naive, kFs, f0, 20000.0);
                std::printf("a naive saw at %.0f Hz: an alias at %.1f dB (%.0f Hz)\n", f0, odd.db, odd.hz);
                P.ge("wavetable.naive_saw_aliases_db", odd.db, -40.0);
            }
            // The end of a fade (2 log2(2048 step) just under 11): copy 12 alone, sixteen harmonics.
            {
                const double f0 = kFs * std::pow(2.0, 5.5) / 2048.0 * 0.9999;
                run(y.data(), N, static_cast<float>(f0 / kFs), 0.0f, 2, 1);
                const std::vector<tp::measure::Peak> peaks = tp::measure::spectrumPeaks(y, kFs, 64);
                double h16 = -200.0;
                for (const tp::measure::Peak& pk : peaks)
                    if (std::fabs(pk.hz - 16.0 * f0) < 20.0)
                        h16 = pk.db;
                std::printf("at %.1f Hz the 16th harmonic (%.0f Hz) is %.2f dB under the fundamental\n", f0, 16.0 * f0, -h16);
                P.near("wavetable.sixteenth_harmonic_kept_db", h16, 20.0 * std::log10(1.0 / 16.0), 0.5);
            }
            // A morph from a saw to a square: their fundamentals in phase.
            run(y.data(), N, 440.0f / kRate, 0.0f, 2, 2);
            P.near("wavetable.morph_start_saw", rms(y), 0.577, 0.02);
            run(y.data(), N, 440.0f / kRate, 1.0f, 2, 2);
            P.near("wavetable.morph_end_square", rms(y), 1.0, 0.02);
            run(y.data(), N, 440.0f / kRate, 0.5f, 2, 2);
            P.near("wavetable.morph_halfway", rms(y), 0.764, 0.02);
        }
        if (glide != nullptr)
        {
            std::vector<float> level(n);
            glide(y.data(), level.data(), N, 200.0f / kRate, 8000.0f / kRate);
            double jump = 0.0, back = 0.0;
            for (std::size_t i = 1; i < n; ++i)
            {
                jump = std::max(jump, static_cast<double>(std::fabs(level[i] - level[i - 1])));
                back = std::max(back, static_cast<double>(level[i - 1] - level[i]));
            }
            std::printf("a glide from 200 Hz to 8 kHz: its copies from %.2f to %.2f, the largest move a sample %.5f\n",
                        static_cast<double>(level.front()), static_cast<double>(level.back()), jump);
            P.le("wavetable.glide_continuous", jump, 0.001);
            P.le("wavetable.glide_only_up", back, 1e-6);
            P.eq("wavetable.glide_finite", std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }) ? 1 : 0, 1);
        }
        if (harmonics != nullptr)
        {
            P.eq("wavetable.harmonics", harmonics(0) == 1023 && harmonics(1) == 724 && harmonics(2) == 512
                                         && harmonics(12) == 16 && harmonics(20) == 1 ? 1 : 0, 1);
        }
        if (unbuilt != nullptr)
            P.eq("wavetable.safe_before_build", unbuilt() == 0.0f ? 1 : 0, 1);
    }

    // ---- ADSR, LFO, FOLLOWER -----------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kModulation, errors);
        P.eq("modulation.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* adsr = function<void(float*, int, float, float, float, float, int, int)>(p.get(), "adsrRun");
        auto* lfo = function<void(float*, int, float, int, float)>(p.get(), "lfoRun");
        auto* follower = function<void(float*, float*, int, float, float, int)>(p.get(), "followerRun");
        auto* compressor = function<float(float, float, float, float)>(p.get(), "compressor");
        std::vector<float> y(n * 3);
        if (adsr != nullptr)
        {
            adsr(y.data(), static_cast<int>(y.size()), 10.0f, 300.0f, 0.6f, 500.0f, 48000, 0);
            int top = -1, ended = -1;
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                if (top < 0 && y[i] >= 1.0f)
                    top = static_cast<int>(i);
                if (ended < 0 && y[i] < 0.0f)
                    ended = static_cast<int>(i);
            }
            P.near("adsr.attack_ms", static_cast<double>(top) / 48.0, 10.0, 0.1);
            P.near("adsr.sustain", static_cast<double>(y[47999]), 0.6, 0.001);
            P.le("adsr.release_ends_ms", static_cast<double>(ended - 48000) / 48.0, 1.4 * 500.0);
            // A gate while it releases: it rises from where it is.
            adsr(y.data(), static_cast<int>(y.size()), 50.0f, 300.0f, 0.6f, 500.0f, 48000, 60000);
            const double step = std::fabs(static_cast<double>(y[60000]) - static_cast<double>(y[59999]));
            P.le("adsr.retrigger_no_step", step, 0.01);
            P.ge("adsr.retrigger_rises", static_cast<double>(y[60480] - y[59999]), 0.05);
        }
        if (lfo != nullptr)
        {
            std::vector<float> s(n);
            bool inRange = true;
            for (int shape = 0; shape < 5; ++shape)
            {
                lfo(s.data(), N, 2.0f / kRate, shape, 0.25f);
                inRange = inRange && probe::peak(s) <= 1.0f;
            }
            P.eq("lfo.in_range", inRange ? 1 : 0, 1);
            lfo(s.data(), N, 2.0f / kRate, 3, 0.0f);
            int flips = 0, changes = 0;
            for (std::size_t i = 1; i < n; ++i)
                flips += s[i] != s[i - 1] ? 1 : 0;
            P.near("lfo.square_flips", static_cast<double>(flips), 3.0, 1.0);    // two cycles in a second
            lfo(s.data(), N, 2.0f / kRate, 4, 0.0f);
            for (std::size_t i = 1; i < n; ++i)
                changes += s[i] != s[i - 1] ? 1 : 0;
            P.near("lfo.random_steps_a_cycle", static_cast<double>(changes), 2.0, 1.0);
        }
        if (follower != nullptr)
        {
            std::vector<float> x(n, 0.0f), e(n);
            std::fill(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n / 2), 1.0f);
            follower(x.data(), e.data(), N, 10.0f, 100.0f, 0);
            P.near("follower.attack_63", static_cast<double>(e[480]), 0.632, 0.01);
            P.near("follower.release_37", static_cast<double>(e[n / 2 + 4800]), 0.368, 0.01);
        }
        if (compressor != nullptr)
        {
            P.near("compressor.under", static_cast<double>(compressor(-30.0f, -20.0f, 4.0f, 6.0f)), 0.0, 1e-6);
            P.near("compressor.over", static_cast<double>(compressor(-10.0f, -20.0f, 4.0f, 0.0f)), -7.5, 1e-4);
            P.near("compressor.knee_middle", static_cast<double>(compressor(-20.0f, -20.0f, 4.0f, 6.0f)), -0.5625, 1e-4);
        }
    }

    // ---- SHAPER, UTIL --------------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kShaping, errors);
        P.eq("shaping.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* shape = function<float(int, float, float)>(p.get(), "shape");
        auto* crush = function<void(float*, float*, int, float, float)>(p.get(), "crushRun");
        auto* util = function<float(int, float, float)>(p.get(), "util");
        auto* smooth = function<float(float, int)>(p.get(), "smooth");
        if (shape != nullptr)
        {
            bool bounded = true;
            for (float x = -3.0f; x <= 3.0f; x += 0.01f)
                for (const float a : { 0.0f, 0.5f, 1.0f })
                    bounded = bounded && std::fabs(shape(2, std::clamp(x, -1.0f, 1.0f), a)) <= 1.0001f
                           && std::fabs(shape(3, std::clamp(x, -1.0f, 1.0f), a)) <= 1.0001f && std::fabs(shape(0, x, 0)) < 1.0f;
            P.eq("shaper.bounded", bounded ? 1 : 0, 1);
            P.near("shaper.drive_keeps_full_scale", static_cast<double>(shape(2, 1.0f, 0.7f)), 1.0, 1e-5);
            P.near("shaper.hard_clip", static_cast<double>(shape(1, 2.0f, 0.5f)), 0.5, 1e-6);
            P.near("shaper.asym_zero", static_cast<double>(shape(4, 0.0f, 0.5f)), 0.0, 1e-6);
        }
        if (crush != nullptr)
        {
            std::vector<float> x{ 0.3f, 0.7f, 0.3f, 0.3f }, y(4);
            crush(x.data(), y.data(), 4, 1.0f, 1.0f);
            P.eq("crusher.one_bit", y[0] == 0.0f && y[1] == 1.0f ? 1 : 0, 1);
            crush(x.data(), y.data(), 4, 3.0f, 2.0f);
            P.eq("crusher.three_bits_held", y[1] == 0.75f && y[2] == 0.75f && y[3] == 0.25f ? 1 : 0, 1);
        }
        if (util != nullptr)
        {
            P.near("util.note_69", static_cast<double>(util(0, 69.0f, 0)), 440.0, 1e-3);
            P.near("util.fifth", static_cast<double>(util(1, 7.0f, 0)), 1.4983071, 1e-5);
            P.near("util.octave_in_cents", static_cast<double>(util(2, 1200.0f, 0)), 2.0, 1e-5);
            P.near("util.db_to_gain", static_cast<double>(util(3, -6.0206f, 0)), 0.5, 1e-4);
            P.near("util.gain_to_db", static_cast<double>(util(4, 0.1f, 0)), -20.0, 1e-4);
            const double l = util(5, 0.3f, 0), r = util(6, 0.3f, 0);
            P.near("util.pan_equal_power", l * l + r * r, 1.0, 1e-5);
            P.near("util.pan_hard_left", static_cast<double>(util(5, -1.0f, 0)), 1.0, 1e-6);
            P.near("util.crossfade_middle", static_cast<double>(util(7, 1.0f, 0.0f)), 0.70710678, 1e-5);
            P.near("util.lerp", static_cast<double>(util(8, 2.0f, 4.0f)), 2.5, 1e-6);
        }
        if (smooth != nullptr)
            P.near("util.smooth_63_at_its_time", static_cast<double>(smooth(10.0f, 480)), 0.632, 0.002);
    }
    return P.finish();
}
