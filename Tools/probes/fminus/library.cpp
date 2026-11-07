// PROBE name=fminus.library timeout=300
//
// fminus.library: the editor's LIBRARY (Source/fminus/library, compiled in as Library.h), each file #included by a
// harness of its own and run by FMinus's JIT at 48 kHz, its output measured here:
//   FILES     every file's first line names it and says what it is, a usage header follows, and its name is a tab's;
//             each compiles alone with no message, and all of them together (no names clash); a circuit netlist (.cir,
//             fminus/Circuits.h) the same in its own comments ('* name: ...', a '*' line, then its usage), #included
//             as the module it generates: each generates with no message and a configuration of its own (no
//             self-test waits on it), and compiles alone and with the others
//   SVF       low-pass: -3 dB at the cutoff with resonance 0.3 (Butterworth), below -38 dB a decade above; high-pass
//             the mirror; band-pass 0 dB at the cutoff; notch below -60 dB there; resonance 1 peaks 20 to 30 dB
//   G         svfG, ladderG and onePoleG are tan(pi fc / rate), clamped as set clamps the cutoff; each filter swept from
//             30 Hz to 20 kHz through setG (g in a loop of its own) plays what set (sin and cos in one call) plays,
//             within 1e-6 of the peak
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
//   WAVEGUIDE a pluck's fundamental within half a cent of 110, 440.5, 1234.5 and 3000 Hz (the allpass's fraction tuned
//             at the pitch), its 8th partial harmonic within half a cent without dispersion, sharp by 1 cent or more at
//             0.3 and 30 cents or more at 1; the fundamental's T60 within 5 % of decay at damping 0, the 8th partial's
//             shorter at damping 0.5; a tube's 2nd harmonic 40 dB under its fundamental (odd harmonics only), its 3rd
//             within 20 dB; a pluck in the middle takes the 2nd harmonic down 15 dB against one near the bridge; a glide
//             from 220 to 330 Hz (set every 16 samples) lands within a cent; noise in rings at a multiple of the pitch, a
//             DC input leaves no offset once the loop's decay has passed (without the input's blocker it would build to
//             0.5 / (1 - gain)); everything finite and under 1 for a pluck at 0.5; every call safe before allocate
//   OSC       at 440 Hz each wave has its RMS (saw and triangle 0.577, square 1, sine 0.707), no DC, 440 rising zero
//             crossings a second; a 25 % pulse has no DC; the morph keeps its fundamental between saw and square;
//             at 3.7 kHz the saw's strongest alias below 16 kHz is at least 10 dB under a naive saw's
//   WAVETABLE each shape at 440 Hz with its RMS (sine 0.707, triangle and saw 0.577, square 1), no DC, 440 rising zero
//             crossings; the saw at 1, 3.7, 7 and 11 kHz with nothing that is no harmonic above -80 dB below 20 kHz (a
//             naive saw's aliases are above -40 dB); at the end of a fade the 16th harmonic of 1061 Hz (17 kHz) at a
//             saw's level; a glide's copies move continuously and only up; a saw-to-square morph keeps its
//             fundamental (0.764 halfway); the copies' harmonics; every call safe before allocate and build
//   NOISE     white is uniform in -1..1 (RMS 0.577, no DC); pink and brown as loud (within 10 %); the low end
//             outweighs the top more from white to pink to brown; two seeds differ, uncorrelated; white is FMinus's
//             randf now, and against the version before it (Park-Miller's generator by Schrage's method, kept here)
//             the same statistics: the mean within 0.01 of the old one's, the RMS and the spectral flatness (Welch's
//             spectrum, 20 Hz to 20 kHz) within 0.5 dB of the old ones; fill() gives what as many calls of white()
//             give, bit for bit, in parts of any size; uniform() in (0, 1], as before randf: never 0, its least 2^-24
//             and its greatest 1 (the seeds whose first draw is randf's -1 and 1 - 2^-23, worked out through
//             lowbias32's inverse and checked through hash); each one's cost a sample printed
//   ADSR      the attack reaches 1 in its time (within 0.1 ms), the decay settles on the sustain, the release ends
//             within 1.4 times its time; a gate while it sounds rises from where it is, without a step
//   LFO       every shape stays in -1..1; the sine (a polynomial) within 0.0011 of sin at its phase; the square flips
//             twice a cycle; the random step changes once a cycle;
//             follow() takes its phase from the song's position (120 BPM from beat 3, a cycle a half note)
//   FOLLOWER  63 % of a step in the attack time, 37 % left after the release time; the compressor's gain curve: 0 under
//             the threshold, -(1 - 1/ratio) of the excess over it, the soft knee's middle
//   SHAPER    drive and fold keep -1..1 (drive maps 1 to 1); soft clipping stays under 1; the crusher's steps
//   UTIL      the conversions (440 Hz at note 69, a fifth, cents, dB), equal-power panning, the smoother's time
//   OVERSAMPLE each of Oversample2, Oversample4 (IIR), Oversample2Fir and Oversample4Fir: a 1 kHz sine up and then down
//             within 0.01 dB of its level, delayed by its header's figure (IIR 2.7 and 3.5 host samples, within 0.1;
//             FIR 40 and 47, within 0.5); tones from 15 to 20 kHz up, each stage's worst image (the first's at 48 kHz
//             - f, from 33 down to its stopband's edge at 28 kHz; at 4x the second's at 96 kHz - f, down to 76 kHz) at
//             or below the stopband its header quotes for that stage, plus 1 dB; a tanh driven by a 5 kHz sine through
//             Oversample4 aliases at least 20 dB less than at 1x
#include "Probe.h"

#include "Audio.h"
#include "Rig.h"

#include "Library.h"

#include "dsp/Measure.h"
#include "dsp/Sources.h"
#include "fminus/Circuits.h"

#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr float kRate = 48000.0f;

    // A harness that #includes library files, compiled with the library behind its #includes (a netlist: the module
    // generated from it, as the plugin reads it, its messages first).
    std::unique_ptr<fminus::Program> compileHarness(const std::string& source, std::string& errors)
    {
        pg::Sources library;
        for (const pg::LibraryFile& f : pg::kLibrary)
            library.push_back({ f.name, f.source });
        pg::circuits::Generator circuits(library);
        fminus::Program::Options o;
        o.fileName = "harness.fm";
        o.denormals = fminus::Program::Denormals::Flush;
        o.include = [&circuits](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            const pg::LibraryFile* f = pg::findLibraryFile(name);
            if (f == nullptr)
                return std::nullopt;
            if (pg::isCircuitFile(f->name))
            {
                const std::string* text = circuits.module(f->name);
                return text != nullptr ? std::optional(fminus::Program::IncludedFile{ std::string(f->name) + ".fm", *text })
                                       : std::nullopt;
            }
            return fminus::Program::IncludedFile{ f->name, f->source };
        };
        std::unique_ptr<fminus::Program> p = fminus::Program::compile(source, errors, o);
        errors = circuits.errors() + circuits.warnings() + circuits.notes() + circuits.recite(errors);
        return circuits.errors().empty() ? std::move(p) : nullptr;
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
        const std::vector<pg::measure::Peak> peaks = pg::measure::spectrumPeaks(x, kFs, 64);
        double worst = -200.0;
        for (const pg::measure::Peak& p : peaks)
        {
            const double k = std::round(p.hz / f0);
            if (p.hz < 16000.0 && std::fabs(p.hz - k * f0) > 30.0)
                worst = std::max(worst, p.db);
        }
        return worst;
    }

    // The phase of the component at `hz` in x over [from, from + count) (a whole number of cycles), radians.
    double phaseAt(const std::vector<float>& x, double hz, double fs, std::size_t from, std::size_t count)
    {
        double re = 0.0, im = 0.0;
        for (std::size_t i = from; i < from + count && i < x.size(); ++i)
        {
            const double ph = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs;
            re += static_cast<double>(x[i]) * std::cos(ph);
            im -= static_cast<double>(x[i]) * std::sin(ph);
        }
        return std::atan2(im, re);
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

// A cutoff swept from 30 Hz to 20 kHz over n samples through one filter (0 the Svf, 1 the Ladder, 2 the OnePole) twice:
// its coefficient in a loop of its own (svfG, ladderG, onePoleG), then setG, into y; set beside tick into z.
float gOf(float fc, int which)
{
    return which == 0 ? svfG(fc, 48000) : (which == 1 ? ladderG(fc, 48000) : onePoleG(fc, 48000));
}

void gRun(float* x, float* y, float* z, int n, int which)
{
    Svf sa;
    Svf sb;
    Ladder la;
    Ladder lb;
    OnePole oa;
    OnePole ob;
    float fc[16];
    float g[16];
    for (int at = 0; at < n; at += 16)
    {
        const int m = n - at < 16 ? n - at : 16;
        for (int i = 0; i < m; ++i)
            fc[i] = 30 * expf(6.5f * (float)(at + i) / n);
        for (int i = 0; i < m; ++i)
            g[i] = gOf(fc[i], which);
        for (int i = 0; i < m; ++i)
        {
            const int j = at + i;
            if (which == 0)
            {
                sa.setG(g[i], 0.5f);
                y[j] = sa.tick(x[j]);
                sb.set(fc[i], 0.5f, 48000);
                z[j] = sb.tick(x[j]);
            }
            else if (which == 1)
            {
                la.setG(g[i], 0.5f);
                y[j] = la.tick(x[j]);
                lb.set(fc[i], 0.5f, 48000);
                z[j] = lb.tick(x[j]);
            }
            else
            {
                oa.setG(g[i]);
                y[j] = oa.lowpass(x[j]);
                ob.set(fc[i], 48000);
                z[j] = ob.lowpass(x[j]);
            }
        }
    }
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

// White noise from fill(), in parts of `part` samples.
void fillRun(float* y, int n, int part, int seed)
{
    Noise noise;
    if (seed > 0)
        noise.seed(seed);
    for (int at = 0; at < n; at += part)
        noise.fill(y + at, n - at < part ? n - at : part);
}

// n draws of uniform(), unseeded.
void uniformRun(float* y, int n)
{
    Noise noise;
    for (int i = 0; i < n; ++i)
        y[i] = noise.uniform();
}

// uniform()'s first draw after seed(n): its white() is randf(hash(n)), which hashes hash(n) again.
float uniformAfter(int n)
{
    Noise noise;
    noise.seed(n);
    return noise.uniform();
}

int hashOf(int n)
{
    return hash(n);
}
)FM";

    const char* const kWaveguide = R"FM(#include "waveguide.fm"

// A pluck at hz into y (n samples): decay, damping, dispersion, a tube or a string, where it is plucked.
void wgPluck(float* y, int n, float hz, float decay, float damping, float dispersion, int tube, float position)
{
    Waveguide s;
    s.allocate(20, 48000);
    s.tube = tube != 0;
    s.set(hz, decay, damping, dispersion);
    s.pluck(position, 1, 0.5f);
    for (int i = 0; i < n; ++i)
        y[i] = s.tick(0);
    s.release();
}

// A pluck at `from` Hz gliding to `to` over the first half of n samples (set every 16), then held.
void wgGlide(float* y, int n, float from, float to)
{
    Waveguide s;
    s.allocate(20, 48000);
    s.set(from, 3, 0.3f, 0);
    s.pluck(0.3f, 1, 0.5f);
    for (int at = 0; at < n; at += 16)
    {
        const float t = fminf((float)at / (0.5f * (float)n), 1);
        s.set(from * expf(t * logf(to / from)), 3, 0.3f, 0);
        for (int i = at; i < at + 16 && i < n; ++i)
            y[i] = s.tick(0);
    }
    s.release();
}

// x through it all the time (an effect's resonator).
void wgResonate(float* x, float* y, int n, float hz, float decay)
{
    Waveguide s;
    s.allocate(20, 48000);
    s.set(hz, decay, 0.3f, 0);
    for (int i = 0; i < n; ++i)
        y[i] = s.tick(x[i]);
    s.release();
}

float wgUnallocated()
{
    Waveguide s;
    s.set(220, 1, 0.5f, 0.5f);
    s.pluck(0.3f, 0.5f, 1);
    s.clear();
    return s.tick(1) + s.tick(0);
}
)FM";

    // The frequency of the strongest component within `span` (a fraction) of `near` in x[from, from + count), to a
    // thousandth of a hertz: a Hann window's DFT scanned, then a ternary search on its peak.
    double toneHz(const std::vector<float>& x, double near, double span, std::size_t from, std::size_t count)
    {
        const auto mag = [&](double hz) { return probe::toneAmplitudeHann(x, hz, kFs, from, count); };
        double best = -1.0, at = near;
        for (int k = -150; k <= 150; ++k)
            if (const double f = near * (1.0 + span * k / 150.0); mag(f) > best)
            {
                best = mag(f);
                at = f;
            }
        double lo = at - near * span / 150.0, hi = at + near * span / 150.0;
        for (int it = 0; it < 40; ++it)
        {
            const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
            if (mag(a) < mag(b))
                lo = a;
            else
                hi = b;
        }
        return 0.5 * (lo + hi);
    }

    double cents(double hz, double want) { return 1200.0 * std::log2(hz / want); }

    // noise.fm's white noise before randf: Park-Miller's generator by Schrage's method (no 64-bit products), the
    // reference the new one's statistics are held to.
    const char* const kOldNoise = R"FM(module OldNoise
{
    int state;

    void seed(int n)
    {
        state = n > 0 ? n : 1;
    }

    float uniform()
    {
        if (state <= 0)
            state = 22222;
        const int hi = state / 127773;
        const int lo = state % 127773;
        state = 16807 * lo - 2836 * hi;
        if (state <= 0)
            state += 2147483647;
        return (float)state / 2147483647.0f;
    }

    float white()
    {
        return 2 * uniform() - 1;
    }
};

void oldRun(float* y, int n, int seed)
{
    OldNoise noise;
    if (seed > 0)
        noise.seed(seed);
    for (int i = 0; i < n; ++i)
        y[i] = noise.white();
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

// The saw of an LFO that follows the song from `from` quarter notes, `perSample` of them a sample, a cycle each `beats`.
void lfoFollow(float* y, int n, float from, float perSample, float beats)
{
    Lfo lfo;
    for (int i = 0; i < n; ++i)
    {
        lfo.follow(from + i * perSample, beats);
        y[i] = lfo.saw(0);
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

const char* const kOversample = R"FM(#include "oversample.fm"

// x (n host samples) up through one of the four (0 Oversample2, 1 Oversample4, 2 Oversample2Fir, 3 Oversample4Fir) into
// hi (n times its factor), and straight back down into y.
void osRun(float* x, float* hi, float* y, int n, int which)
{
    Oversample2 a;
    Oversample4 b;
    Oversample2Fir c;
    Oversample4Fir d;
    float v[4];
    for (int i = 0; i < n; ++i)
    {
        if (which == 0)
        {
            a.up(x[i], v);
            hi[2 * i] = v[0];
            hi[2 * i + 1] = v[1];
            y[i] = a.down(v);
        }
        else if (which == 1)
        {
            b.up(x[i], v);
            for (int k = 0; k < 4; ++k)
                hi[4 * i + k] = v[k];
            y[i] = b.down(v);
        }
        else if (which == 2)
        {
            c.up(x[i], v);
            hi[2 * i] = v[0];
            hi[2 * i + 1] = v[1];
            y[i] = c.down(v);
        }
        else
        {
            d.up(x[i], v);
            for (int k = 0; k < 4; ++k)
                hi[4 * i + k] = v[k];
            y[i] = d.down(v);
        }
    }
}

// x through tanh(g x), at 1x (which 0) or at 4x through an Oversample4 (1), into y.
void tanhRun(float* x, float* y, int n, float g, int which)
{
    Oversample4 os;
    float v[4];
    for (int i = 0; i < n; ++i)
    {
        if (which == 0)
            y[i] = tanhf(g * x[i]);
        else
        {
            os.up(x[i], v);
            for (int k = 0; k < 4; ++k)
                v[k] = tanhf(g * v[k]);
            y[i] = os.down(v);
        }
    }
}
)FM";

}

PROBE(fminus, library)
{
    const std::size_t n = static_cast<std::size_t>(kFs);         // one second
    const int N = static_cast<int>(n);

    // ---- FILES -----------------------------------------------------------------------------------------------------
    {
        int named = 0, headed = 0, alone = 0, netlists = 0, generated = 0;
        std::string all;
        for (const pg::LibraryFile& f : pg::kLibrary)
        {
            const std::string text = f.source;
            const bool netlist = pg::isCircuitFile(f.name);
            const std::string comment = netlist ? "*" : "//";   // a netlist's comments are SPICE's
            named += text.rfind(comment + " " + f.name + ": ", 0) == 0 && pg::validFileName(f.name) ? 1 : 0;
            headed += text.find("\n" + comment + "\n") != std::string::npos ? 1 : 0;
            if (netlist)
            {
                ++netlists;
                const fminus::circuit::Result r = fminus::circuit::generate(text, f.name);
                const bool clean = r.errors.empty() && r.warnings.empty() && !r.provisional && !r.source.empty();
                generated += clean ? 1 : 0;
                std::printf("%s: module %s; %s%s%s", f.name, r.module.c_str(), r.configuration.c_str(),
                            clean ? "\n" : "\n", (r.errors + r.warnings).c_str());
            }
            std::string errors;
            const auto p = compileHarness(std::string("#include \"") + f.name + "\"\nfloat one() { return 1; }\n", errors);
            alone += p != nullptr && errors.empty() ? 1 : 0;
            if (!errors.empty())
                std::printf("%s alone:\n%s", f.name, errors.c_str());
            all += std::string("#include \"") + f.name + "\"\n";
        }
        const auto count = static_cast<int64_t>(std::size(pg::kLibrary));
        std::printf("the library: %lld files\n", static_cast<long long>(count));
        P.ge("files.count", static_cast<double>(count), 12.0);
        P.eq("files.first_line_names_it", named, count);
        P.eq("files.usage_header", headed, count);
        P.eq("files.compile_alone", alone, count);
        P.ge("files.netlists", static_cast<double>(netlists), 3.0);
        P.eq("files.netlists_generate", generated, static_cast<int64_t>(netlists));
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
        auto* gOf = function<float(float, int)>(p.get(), "gOf");
        auto* gRun = function<void(float*, float*, float*, int, int)>(p.get(), "gRun");
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
        if (gOf != nullptr && gRun != nullptr)
        {
            const char* names[] = { "svf", "ladder", "onepole" };
            const double top[] = { 0.49, 0.45, 0.49 }, low[] = { 5.0, 5.0, 1.0 };
            std::vector<float> x = probe::noise(n, 0.25).l;
            std::vector<float> z(n);
            for (int which = 0; which < 3; ++which)
            {
                const std::string name = names[which];
                const auto tanOf = [](double fc) { return std::tan(std::numbers::pi * fc / kFs); };
                P.near(name + ".g_is_tan", static_cast<double>(gOf(1000.0f, which)) / tanOf(1000.0), 1.0, 1e-6);
                P.near(name + ".g_clamps_high", static_cast<double>(gOf(30000.0f, which)) / tanOf(top[which] * kFs), 1.0, 1e-5);
                P.near(name + ".g_clamps_low", static_cast<double>(gOf(0.1f, which)) / tanOf(low[which]), 1.0, 1e-5);
                gRun(x.data(), y.data(), z.data(), N, which);
                double worst = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                    worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - static_cast<double>(z[i])));
                std::printf("%s: setG against set, swept: the largest difference %.3g (peak %.3g)\n", names[which], worst,
                            static_cast<double>(probe::peak(z)));
                P.le(name + ".setg_plays_as_set", worst, 1e-6 * static_cast<double>(probe::peak(z)));
                P.ge(name + ".swept_audible", static_cast<double>(probe::peak(z)), 0.05);
            }
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

    // ---- WAVEGUIDE ---------------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kWaveguide, errors);
        P.eq("waveguide.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        auto* pluck = function<void(float*, int, float, float, float, float, int, float)>(p.get(), "wgPluck");
        auto* glide = function<void(float*, int, float, float)>(p.get(), "wgGlide");
        auto* resonate = function<void(float*, float*, int, float, float)>(p.get(), "wgResonate");
        auto* unallocated = function<float()>(p.get(), "wgUnallocated");
        const std::size_t len = static_cast<std::size_t>(0.6 * kFs), from = static_cast<std::size_t>(0.1 * kFs), count = len - from;
        std::vector<float> y(len);
        bool finite = true;
        float loudest = 0.0f;
        const auto play = [&](float hz, float decay, float damping, float dispersion, int tube, float position) {
            pluck(y.data(), static_cast<int>(len), hz, decay, damping, dispersion, tube, position);
            finite = finite && probe::allFinite(y);
            loudest = std::max(loudest, probe::peak(y));
        };
        if (pluck != nullptr)
        {
            for (const double hz : { 110.0, 440.5, 1234.5, 3000.0 })
            {
                play(static_cast<float>(hz), 3.0f, 0.3f, 0.0f, 0, 0.3f);
                const double c = cents(toneHz(y, hz, 0.03, from, count), hz);
                std::printf("waveguide at %g Hz: %+.3f cents\n", hz, c);
                char key[64];
                std::snprintf(key, sizeof key, "waveguide.pitch_%g_cents", hz);
                P.le(key, std::fabs(c), 0.5);
            }
            // The 8th partial against 8 times the fundamental: harmonic, then stretched by the dispersion.
            const auto eighth = [&](float dispersion) {
                play(220.0f, 3.0f, 0.3f, dispersion, 0, 0.3f);
                const double f0 = toneHz(y, 220.0, 0.03, from, count);
                return cents(toneHz(y, 8.0 * f0 * 1.02, 0.05, from, count), 8.0 * f0);
            };
            const double none = eighth(0.0f), some = eighth(0.3f), most = eighth(1.0f);
            std::printf("waveguide's 8th partial at 220 Hz: %+.2f cents (no dispersion), %+.2f (0.3), %+.2f (1)\n", none, some, most);
            P.le("waveguide.harmonic_8th_cents", std::fabs(none), 0.5);
            P.ge("waveguide.dispersion_0.3_8th_cents", some, 1.0);
            P.ge("waveguide.dispersion_1_8th_cents", most, 30.0);
            // The decay: a partial's level in two 0.1 s frames a second apart (0.4 s for the damped highs).
            const std::size_t longer = static_cast<std::size_t>(2.0 * kFs), frame = static_cast<std::size_t>(0.1 * kFs);
            std::vector<float> z(longer);
            const auto t60 = [&](float damping, double hz, double t1, double t2) {
                pluck(z.data(), static_cast<int>(longer), 220.0f, 1.5f, damping, 0.0f, 0, 0.3f);
                const double a = probe::dbFromLin(probe::toneAmplitudeHann(z, hz, kFs, static_cast<std::size_t>(t1 * kFs), frame));
                const double b = probe::dbFromLin(probe::toneAmplitudeHann(z, hz, kFs, static_cast<std::size_t>(t2 * kFs), frame));
                return 60.0 * (t2 - t1) / (a - b);
            };
            const double plain = t60(0.0f, 220.0, 0.2, 1.2), low = t60(0.5f, 220.0, 0.2, 0.6), high = t60(0.5f, 1760.0, 0.2, 0.6);
            std::printf("waveguide T60 at 220 Hz, decay 1.5 s: %.3f s (damping 0); damping 0.5: the fundamental %.3f s, the 8th "
                        "partial %.3f s\n", plain, low, high);
            P.near("waveguide.t60_s", plain, 1.5, 0.075);
            P.le("waveguide.damping_highs_shorter", high / low, 0.85);
            // A tube: odd harmonics.
            play(220.0f, 3.0f, 0.2f, 0.0f, 1, 0.3f);
            const double h1 = probe::toneAmplitudeHann(y, 220.0, kFs, from, count);
            const double h2 = probe::dbFromLin(probe::toneAmplitudeHann(y, 440.0, kFs, from, count) / h1);
            const double h3 = probe::dbFromLin(probe::toneAmplitudeHann(y, 660.0, kFs, from, count) / h1);
            std::printf("waveguide's tube at 220 Hz: 2nd harmonic %.1f dB, 3rd %.1f dB\n", h2, h3);
            P.le("waveguide.tube_no_2nd_db", h2, -40.0);
            P.ge("waveguide.tube_3rd_db", h3, -20.0);
            // Where it is plucked: the middle has a node of the 2nd harmonic.
            const auto second = [&](float position) {
                play(220.0f, 3.0f, 0.0f, 0.0f, 0, position);
                return probe::dbFromLin(probe::toneAmplitudeHann(y, 440.0, kFs, from, count)
                                        / probe::toneAmplitudeHann(y, 220.0, kFs, from, count));
            };
            const double middle = second(0.5f), bridge = second(0.13f);
            std::printf("waveguide's 2nd harmonic: %.1f dB plucked in the middle, %.1f dB near the bridge\n", middle, bridge);
            P.ge("waveguide.middle_takes_2nd_db", bridge - middle, 15.0);
            P.eq("waveguide.finite", finite ? 1 : 0, 1);
            P.le("waveguide.pluck_peak", loudest, 1.0);
        }
        if (glide != nullptr)
        {
            std::vector<float> g(static_cast<std::size_t>(kFs));
            glide(g.data(), N, 220.0f, 330.0f);
            const double c = cents(toneHz(g, 330.0, 0.03, static_cast<std::size_t>(0.6 * kFs), static_cast<std::size_t>(0.4 * kFs)), 330.0);
            std::printf("waveguide's glide to 330 Hz: %+.3f cents\n", c);
            P.le("waveguide.glide_cents", std::fabs(c), 1.0);
            P.eq("waveguide.glide_finite", probe::allFinite(g) ? 1 : 0, 1);
        }
        if (resonate != nullptr)
        {
            const probe::Stereo noise = probe::noise(n, 0.1);
            std::vector<float> in = noise.l, out(n);
            resonate(in.data(), out.data(), N, 220.0f, 0.5f);
            const std::vector<pg::measure::Peak> peaks = pg::measure::spectrumPeaks(out, kFs, 1);
            const double strongest = peaks.empty() ? 0.0 : peaks.front().hz;
            const double multiple = strongest / 220.0;
            std::printf("waveguide's resonance to noise: strongest at %.1f Hz\n", strongest);
            P.le("waveguide.resonates_on_pitch", std::fabs(multiple - std::round(multiple)) / std::max(1.0, std::round(multiple)), 0.01);
            std::vector<float> dc(n, 0.5f);
            resonate(dc.data(), out.data(), N, 220.0f, 0.5f);
            double tail = 0.0;
            for (std::size_t i = n / 2; i < n; ++i)
                tail += static_cast<double>(out[i]);
            tail /= static_cast<double>(n - n / 2);
            // Without the blocker it would build to 0.5 / (1 - gain), 8 here; with it, what the step's edge left dies
            // away with the loop's decay.
            std::printf("waveguide with a DC input of 0.5 (decay 0.5 s): %.5f after half a second\n", tail);
            P.le("waveguide.dc_blocked", std::fabs(tail), 0.01);
            P.eq("waveguide.resonator_finite", probe::allFinite(out) ? 1 : 0, 1);
        }
        if (unallocated != nullptr)
            P.eq("waveguide.safe_unallocated", unallocated() == 0.0f ? 1 : 0, 1);
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
            {
                std::vector<float> a(n), b(n);
                noise(a.data(), N, 0, 1);
                noise(b.data(), N, 0, 2);
                double ab = 0.0, aa = 0.0, bb = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    ab += static_cast<double>(a[i]) * b[i];
                    aa += static_cast<double>(a[i]) * a[i];
                    bb += static_cast<double>(b[i]) * b[i];
                }
                std::printf("seeds 1 and 2: correlation %.4f\n", ab / std::sqrt(aa * bb));
                P.le("noise.seeds_uncorrelated", std::fabs(ab / std::sqrt(aa * bb)), 0.02);
            }

            // Against the version before randf: the same statistics.
            std::string oldErrors;
            const auto pOld = compileHarness(kOldNoise, oldErrors);
            auto* old = function<void(float*, int, int)>(pOld.get(), "oldRun");
            auto* fill = function<void(float*, int, int, int)>(p.get(), "fillRun");
            P.eq("noise.old_compiles", old != nullptr && fill != nullptr && oldErrors.empty() ? 1 : 0, 1);
            if (old != nullptr && fill != nullptr)
            {
                std::vector<float> before(n);
                old(before.data(), N, 0);
                // Spectral flatness: the geometric mean of Welch's power over its arithmetic mean, 20 Hz to 20 kHz, in dB.
                const auto flatness = [](const std::vector<float>& x) {
                    const pg::measure::Spectrum sp = pg::measure::spectrum(x, kFs, { { 0, x.size() } });
                    double logs = 0.0, sum = 0.0;
                    int bins = 0;
                    for (std::size_t k = 0; k < sp.power.size(); ++k)
                    {
                        const double hz = static_cast<double>(k) * sp.hzPerBin;
                        if (hz < 20.0 || hz > 20000.0 || sp.power[k] <= 0.0)
                            continue;
                        logs += std::log(sp.power[k]);
                        sum += sp.power[k];
                        ++bins;
                    }
                    return bins > 0 ? 10.0 * std::log10(std::exp(logs / bins) / (sum / bins)) : -200.0;
                };
                const double meanNew = mean(white), meanOld = mean(before);
                const double rmsDb = 20.0 * std::log10(rms(white) / rms(before));
                const double flatNew = flatness(white), flatOld = flatness(before);
                std::printf("white against the old: mean %.5f (old %.5f), RMS %.4f (old %.4f, %+.3f dB), flatness %.3f dB "
                            "(old %.3f dB)\n", meanNew, meanOld, rms(white), rms(before), rmsDb, flatNew, flatOld);
                P.near("noise.mean_as_old", meanNew - meanOld, 0.0, 0.01);
                P.near("noise.rms_as_old_db", rmsDb, 0.0, 0.5);
                P.near("noise.flatness_as_old_db", flatNew - flatOld, 0.0, 0.5);
                // fill() in parts of any size: what as many calls of white() give
                std::vector<float> calls(n), parts(n), ones(n);
                noise(calls.data(), N, 0, 5);
                fill(parts.data(), N, 333, 5);
                fill(ones.data(), N, 1, 5);
                P.eq("noise.fill_as_white", parts == calls && ones == calls ? 1 : 0, 1);
                // uniform() in (0, 1], as before randf: never 0 (a log of it stays finite). Its extremes come one draw
                // in 2^24, so the seeds that give them first are worked out: randf(m) is -1 when hash(m) is
                // 0x80000000 (its top 24 bits, signed, -2^23) and 1 - 2^-23 when it is 0x7fffff00, and the first draw
                // after seed(n) is randf(hash(n)), so n = unhash(unhash(that)), with lowbias32's inverse (Wellons's).
                auto* uniform = function<void(float*, int)>(p.get(), "uniformRun");
                auto* after = function<float(int)>(p.get(), "uniformAfter");
                auto* hashOf = function<int(int)>(p.get(), "hashOf");
                P.eq("noise.uniform_runs", uniform != nullptr && after != nullptr && hashOf != nullptr ? 1 : 0, 1);
                if (uniform != nullptr && after != nullptr && hashOf != nullptr)
                {
                    std::vector<float> u(n);
                    uniform(u.data(), N);
                    const auto [lo, hi] = std::minmax_element(u.begin(), u.end());
                    const auto unhash = [](uint32_t x) {
                        x ^= x >> 16;
                        x *= 0x43021123u;
                        x ^= x >> 15 ^ x >> 30;
                        x *= 0x1d69e2a5u;
                        x ^= x >> 16;
                        return x;
                    };
                    const auto seedFor = [&](uint32_t h) { return static_cast<int>(unhash(unhash(h))); };
                    const int least = seedFor(0x80000000u), greatest = seedFor(0x7fffff00u);
                    const bool found = static_cast<uint32_t>(hashOf(hashOf(least))) == 0x80000000u
                                    && static_cast<uint32_t>(hashOf(hashOf(greatest))) == 0x7fffff00u;
                    const float a = after(least), b = after(greatest);
                    std::printf("uniform(): %d draws in [%.9g, %.9g]; seed %d's first draw %.9g, seed %d's %.9g\n", N,
                                static_cast<double>(*lo), static_cast<double>(*hi), least, static_cast<double>(a), greatest,
                                static_cast<double>(b));
                    P.eq("noise.uniform_in_range", *lo > 0.0f && *hi <= 1.0f ? 1 : 0, 1);
                    P.eq("noise.uniform_seeds_found", found ? 1 : 0, 1);
                    P.eq("noise.uniform_least", a == 0x1p-24f ? 1 : 0, 1);
                    P.eq("noise.uniform_greatest", b == 1.0f ? 1 : 0, 1);
                }
                // the cost of a sample of each (a bench: printed, never held to a number)
                const auto ns = [&](const auto& run) {
                    double best = 1e30;
                    for (int k = 0; k < 5; ++k)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        run();
                        best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
                    }
                    return best / static_cast<double>(n);
                };
                std::vector<float> scratch(n);
                const double tOld = ns([&] { old(scratch.data(), N, 3); });
                const double tWhite = ns([&] { noise(scratch.data(), N, 0, 3); });
                const double tFill = ns([&] { fill(scratch.data(), N, 64, 3); });
                std::printf("white noise a sample: the old %.2f ns, white() %.2f ns, fill() %.2f ns\n", tOld, tWhite, tFill);
            }
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
                const pg::measure::Peak odd = pg::measure::inharmonic(y, kFs, f0, 20000.0);
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
                const pg::measure::Peak odd = pg::measure::inharmonic(naive, kFs, f0, 20000.0);
                std::printf("a naive saw at %.0f Hz: an alias at %.1f dB (%.0f Hz)\n", f0, odd.db, odd.hz);
                P.ge("wavetable.naive_saw_aliases_db", odd.db, -40.0);
            }
            // The end of a fade (2 log2(2048 step) just under 11): copy 12 alone, sixteen harmonics.
            {
                const double f0 = kFs * std::pow(2.0, 5.5) / 2048.0 * 0.9999;
                run(y.data(), N, static_cast<float>(f0 / kFs), 0.0f, 2, 1);
                const std::vector<pg::measure::Peak> peaks = pg::measure::spectrumPeaks(y, kFs, 64);
                double h16 = -200.0;
                for (const pg::measure::Peak& pk : peaks)
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
        auto* lfoFollow = function<void(float*, int, float, float, float)>(p.get(), "lfoFollow");
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
            // The sine against sin at the phase the Lfo reads (its float phase, followed here as it advances).
            lfo(s.data(), N, 2.0f / kRate, 0, 0.25f);
            double worst = 0.0;
            float phase = 0.0f;
            for (std::size_t i = 0; i < n; ++i)
            {
                const float read = phase + 0.25f;
                const double at = static_cast<double>(read - std::floor(read));
                worst = std::max(worst, std::fabs(static_cast<double>(s[i]) - std::sin(2.0 * std::numbers::pi * at)));
                phase += 2.0f / kRate;
                if (phase >= 1.0f || phase < 0.0f)
                    phase -= std::floor(phase);
            }
            std::printf("lfo: the sine's largest difference from sin %.5f\n", worst);
            P.le("lfo.sine_within_0.0011", worst, 0.0011);
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
        if (lfoFollow != nullptr)
        {
            // 120 BPM from beat 3, a cycle a half note: the phase from the song's position, a new cycle at beat 4.
            std::vector<float> s(n);
            lfoFollow(s.data(), N, 3.0f, 2.0f / kRate, 2.0f);
            P.near("lfo.follow_from_position", static_cast<double>(s[0]), 0.0, 1e-5);
            P.near("lfo.follow_quarter_on", static_cast<double>(s[n / 4]), 0.5, 1e-3);
            int wraps = 0, at = -1;
            for (std::size_t i = 1; i < n; ++i)
                if (s[i] < s[i - 1])
                {
                    ++wraps;
                    at = static_cast<int>(i);
                }
            P.eq("lfo.follow_one_cycle", wraps, 1);
            P.near("lfo.follow_cycle_on_the_beat", static_cast<double>(at), kRate / 2.0, 1.0);
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

    // ---- OVERSAMPLE ----------------------------------------------------------------------------------------------------
    {
        std::string errors;
        const auto p = compileHarness(kOversample, errors);
        P.eq("oversample.compile", p != nullptr && errors.empty() ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        P.eq("oversample.listed", pg::findLibraryFile("oversample.fm") != nullptr ? 1 : 0, 1);
        auto* run = function<void(float*, float*, float*, int, int)>(p.get(), "osRun");
        auto* drive = function<void(float*, float*, int, float, int)>(p.get(), "tanhRun");
        const pg::LibraryFile* file = pg::findLibraryFile("oversample.fm");
        const std::string header = file != nullptr ? file->source : "";
        struct Kind
        {
            const char* name;
            const char* key;                              // its rows'
            int factor;
            double delay, within;                         // the header's round trip at 1 kHz, host samples
            double stage1, stage2;                        // the header's stopbands, dB: the first stage's, the second's (4x)
            const char* quoted1;                          // as the header says them
            const char* quoted2;
        };
        const Kind kinds[] = { { "Oversample2", "iir2", 2, 2.7, 0.1, -96.0, 0.0, "-96 dB IIR", nullptr },
                               { "Oversample4", "iir4", 4, 3.5, 0.1, -96.0, -101.0, "-96 dB IIR", "-101 dB IIR" },
                               { "Oversample2Fir", "fir2", 2, 40.0, 0.5, -94.0, 0.0, "-94 dB FIR", nullptr },
                               { "Oversample4Fir", "fir4", 4, 47.0, 0.5, -94.0, -93.0, "-94 dB FIR", "-93 dB FIR" } };
        if (run != nullptr)
            for (int which = 0; which < 4; ++which)
            {
                const Kind& k = kinds[which];
                const std::string key = std::string("oversample.") + k.key;
                std::vector<float> x = probe::sine(1000.0, 0.5, kFs, n), hi(n * static_cast<std::size_t>(k.factor)), y(n);
                run(x.data(), hi.data(), y.data(), N, which);
                const std::size_t from = n / 2, count = n / 2;
                const double level = probe::dbFromLin(probe::toneAmplitude(y, 1000.0, kFs, from, count) / probe::toneAmplitude(x, 1000.0, kFs, from, count));
                double dphi = phaseAt(x, 1000.0, kFs, from, count) - phaseAt(y, 1000.0, kFs, from, count);
                while (dphi < 0.0)
                    dphi += 2.0 * std::numbers::pi;
                while (dphi >= 2.0 * std::numbers::pi)
                    dphi -= 2.0 * std::numbers::pi;
                const double delay = dphi / (2.0 * std::numbers::pi) * (kFs / 1000.0);
                // Tones from 15 to 20 kHz up, every 500 Hz (whole cycles in the half second measured): each stage's
                // image against the tone, the worst of them, the first stage's at 48 kHz - f (33 down to 28 kHz, its
                // stopband's edge) and, at 4x, the second's at 96 kHz - f (81 down to 76 kHz).
                const double fsHi = kFs * k.factor;
                const std::size_t hiFrom = hi.size() / 2, hiCount = hi.size() / 2;
                double worst1 = 0.0, worst2 = 0.0;
                for (double f = 15000.0; f <= 20000.0; f += 500.0)
                {
                    std::vector<float> t = probe::sine(f, 0.5, kFs, n);
                    run(t.data(), hi.data(), y.data(), N, which);
                    const double tone = probe::toneAmplitude(hi, f, fsHi, hiFrom, hiCount);
                    worst1 = std::max(worst1, probe::toneAmplitude(hi, kFs - f, fsHi, hiFrom, hiCount) / tone);
                    if (k.factor == 4)
                        worst2 = std::max(worst2, probe::toneAmplitude(hi, 2.0 * kFs - f, fsHi, hiFrom, hiCount) / tone);
                }
                const double image1 = probe::dbFromLin(worst1), image2 = k.factor == 4 ? probe::dbFromLin(worst2) : 0.0;
                std::printf("%s: 1 kHz %+.5f dB, delayed %.3f host samples; 15 to 20 kHz tones' worst images: the first stage's "
                            "%.1f dB (the header: %.0f dB)", k.name, level, delay, image1, k.stage1);
                if (k.factor == 4)
                    std::printf(", the second's %.1f dB (the header: %.0f dB)", image2, k.stage2);
                std::printf("\n");
                P.near(key + ".level_1k_db", level, 0.0, 0.01);
                P.near(key + ".delay_1k_samples", delay, k.delay, k.within);
                P.le(key + ".image_stage1_db", image1, k.stage1 + 1.0);
                if (k.factor == 4)
                    P.le(key + ".image_stage2_db", image2, k.stage2 + 1.0);
                P.eq(key + ".header_quotes", header.find(k.quoted1) != std::string::npos
                                                 && (k.quoted2 == nullptr || header.find(k.quoted2) != std::string::npos) ? 1 : 0, 1);
            }
        if (drive != nullptr)
        {
            // tanh(4 x) of a 5 kHz sine at 0.5: its harmonics above 24 kHz fold back at 1x; what lands below 20 kHz on
            // no harmonic of 5 kHz is the alias (every fold lands on a multiple of 1 kHz).
            const std::vector<float> x = probe::sine(5000.0, 0.5, kFs, n);
            const auto aliasDb = [&](int which) {
                std::vector<float> y(n);
                drive(const_cast<float*>(x.data()), y.data(), N, 4.0f, which);
                double alias = 0.0;
                for (int f = 1000; f <= 20000; f += 1000)
                    if (f % 5000 != 0)
                    {
                        const double a = probe::toneAmplitude(y, f, kFs, n / 2, n / 2);
                        alias += a * a;
                    }
                return 10.0 * std::log10(std::max(alias, 1e-30)) - probe::dbFromLin(probe::toneAmplitude(y, 5000.0, kFs, n / 2, n / 2));
            };
            const double at1 = aliasDb(0), at4 = aliasDb(1);
            std::printf("tanh at 5 kHz: aliases %.1f dB at 1x, %.1f dB at 4x through Oversample4\n", at1, at4);
            P.ge("oversample.tanh_aliases_20db_less", at1 - at4, 20.0);
        }
    }
    return P.finish();
}
