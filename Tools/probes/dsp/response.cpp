// PROBE name=dsp.response timeout=180
//
// dsp.response: the reverb against what its structure implies, on both channels (48 kHz).
//   DRY      MIX 0 %: each channel is GAIN times its input, sample by sample against a double-precision model, with
//            different noises left and right (a channel swapped, dropped or inverted fails): residual under -120 dB, at
//            GAIN -24, 0 and +6 dB.
//   ROUTING  MIX 100 %, no modulation, 12 m, at 4, 8 and 16 lines: every line's first arrival, sample by sample. An
//            impulse on the left input enters the even lines (Fdn.h), each through the input's DC blocker (impulse
//            response h: 1, then (R - 1) R^(m - 1), R = e^(-2 pi 5 Hz / fs)), so until the matrix has carried it
//            round once (2 d_0 samples: at 12 m every line is shorter than that) the left output is exactly
//              sqrt(2/n) k sum_even u_i w_i h(t - d_i)
//            and the right output is silent; the right input is the mirror (the odd lines; until d_1 + d_0). The
//            signs u_i, w_i are the design's two tables (copied below from Fdn.cpp), so a line missing from a tap, a
//            sign flipped or a channel crossed misses by k sqrt(2/n), about 0.09. Within 1e-6, on both outputs.
//            k, the input gain, is checked against its law computed here in double from the layout's primes:
//            k = sqrt(1 - mean g_i^2), g_i = 10^(-3 d_i / (T fs)), within 0.1 % (the Engine's g_i come from
//            fsdsp::exp2, relative error up to 9e-5, which 1 - g^2 magnifies about tenfold at DECAY 2 s). The right
//            output's first sound after a left impulse comes when the mix has carried it into an odd line, exactly
//            d_0 + d_1 samples (787 + 829 at 16 lines); the mirror for the right input.
//   GAIN     the wet output is linear in GAIN: the impulse response at +6 dB is 10^(6/20) times the one at 0 dB
//            (within 1e-5 relative).
//   SMOOTH   a GAIN change from 0 to +12 dB between two blocks (MIX 0 %) is smoothed (20 ms), not stepped: 10 ms after
//            it the gain is between the two.
//   CLICKS   a 220 Hz sine through MIX 100 % (a reverb fed a sine rings at 220 Hz: nothing above 4 kHz), one change
//            at 1 s; a click is broadband, so the largest |output above 4 kHz| (a 4th-order high-pass) in the 0.3 s
//            after the change, relative to the largest |output| before it, tells one (a step of the signal's size
//            measures about -6 dB):
//              LINES  16 to 4 lines: the wet path fades out (20 ms), the network switches and clears, the input and
//                     the wet path fade back in (20 ms); under -60 dB (measured -84 dB), and 4 lines afterwards
//              SHAPE  a new LFO table (the triangle upside down, DEPTH 2 ms) crossfades over 20 ms; under -60 dB
//                     (measured -79 dB)
//              GLIDE  SIZE from 12 to 13 m (a knob nudge) and from 12 to 30 m: the delays glide, a pitch slide. While
//                     they move fast the Thiran reads (DelayLine.h) leave grain above 4 kHz: at the start of the 12 to
//                     30 m glide the reads move 0.6 samples per sample and the grain peaks at -39 dB; a nudge stays
//                     near -66 dB. Bounded at -30 and -55 dB, far from a click. Every line then lands exactly on
//                     its new prime (Fdn::delay(i) == the layout's) within 1 s
//   STILL    with DEPTH 0 the LFO changes nothing: the output is the unmodulated one, bit for bit.
//   MONO     a mono bus (one channel): at MIX 0 % the output is GAIN times the input (within 1e-6), at MIX 100 % the
//            wet signal is (wet L + wet R) / sqrt(2) of the stereo run with the input on both sides (within 1e-6).
//   SANITIZE NaN, +inf, -inf, a signalling NaN and +-1e30 among noise on both inputs (MIX 30 %): the output is finite
//            throughout (1.5 s, the tail included) and is bit for bit the output for the same input with those
//            samples zeroed and +-1e30 clamped to +-1e6 (fsdsp::sanitize, Engine.h: before any delay line; one NaN in
//            the loop would stay there for good).
//   DC       0 Hz is a mode of the network (Fdn.h), so the inputs' DC blockers keep a DC offset out of it: -40 dBFS DC
//            at the defaults (MIX 100 %, 6 s), the output's mean over the last second is under -60 dB of the input's
//            (measured -167 dB; without the blocker +12 dB); full-scale DC at the worst corner (1 m, 4 lines, DECAY
//            20 s, HF DECAY 100 %, 30 s) peaks under 0 dBFS (measured -4.7 dBFS: the step itself excites the
//            network; without the blocker the output settles at +34 dBFS) and its mean over the last 5 s is under
//            -40 dB (measured -86 dB).
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <string>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;

    // The design's input and output signs, u_i and w_i (Fdn.cpp's kTapIn and kTapOut; change both together).
    constexpr double kTapIn[16] = { +1, +1, -1, +1, +1, -1, -1, -1, +1, +1, +1, -1, -1, +1, -1, +1 };
    constexpr double kTapOut[16] = { +1, -1, +1, +1, -1, +1, +1, -1, -1, +1, -1, -1, +1, +1, +1, -1 };

    std::vector<float> noise(std::size_t n, unsigned seed, double amp)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(-amp, amp);
        std::vector<float> v(n);
        for (float& s : v)
            s = static_cast<float>(u(rng));
        return v;
    }

    // 20 log10(rms(out - ref) / rms(ref)), in dB.
    double residualDb(const std::vector<float>& out, const std::vector<double>& ref)
    {
        double e = 0.0, r = 0.0;
        for (std::size_t i = 0; i < ref.size(); ++i)
        {
            const double d = static_cast<double>(out[i]) - ref[i];
            e += d * d;
            r += ref[i] * ref[i];
        }
        return 10.0 * std::log10(std::max(e, 1e-300) / std::max(r, 1e-300));
    }

    std::size_t firstNonZero(const std::vector<float>& x)
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0.0f)
                return i;
        return x.size();
    }

    // The largest |x above 4 kHz| over [at, at + len) (two RBJ high-pass biquads, Q 1/sqrt(2), in double, run from the
    // start), in dB relative to the largest |x| over [at - len, at).
    double clickDb(const std::vector<float>& x, std::size_t at, std::size_t len)
    {
        const double w = 2.0 * std::numbers::pi * 4000.0 / kFs, alpha = std::sin(w) / (2.0 * 0.70710678), cw = std::cos(w);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        std::vector<double> y(x.begin(), x.end());
        for (int pass = 0; pass < 2; ++pass)
        {
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
            for (double& v : y)
            {
                const double in = v, out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1;
                x1 = in;
                y2 = y1;
                y1 = out;
                v = out;
            }
        }
        double hf = 0.0, ref = 0.0;
        for (std::size_t i = at; i < at + len && i < x.size(); ++i)
            hf = std::max(hf, std::fabs(y[i]));
        for (std::size_t i = at - len; i < at; ++i)
            ref = std::max(ref, std::fabs(static_cast<double>(x[i])));
        return probe::dbFromLin(hf / std::max(ref, 1e-30));
    }
}

PROBE(dsp, response)
{
    // DRY
    {
        const std::size_t n = static_cast<std::size_t>(0.5 * kFs);
        probe::Stereo in;
        in.l = noise(n, 1u, 0.25);
        in.r = noise(n, 2u, 0.25);
        for (const float g : { -24.0f, 0.0f, 6.0f })
        {
            tp::ParamValues v = tp::defaultValues();
            v[tp::param::mix] = 0.0f;
            v[tp::param::gain] = g;
            const probe::Stereo out = probe::runEffect(in, kFs, 256, v);
            const double gain = std::pow(10.0, static_cast<double>(g) / 20.0);
            std::vector<double> l(n), r(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                l[i] = gain * in.l[i];
                r[i] = gain * in.r[i];
            }
            const std::string key = "dry.gain" + std::to_string(static_cast<int>(g)) + "db";
            P.le(key + ".left.residual_db", residualDb(out.l, l), -120.0);
            P.le(key + ".right.residual_db", residualDb(out.r, r), -120.0);
        }
    }

    // ROUTING
    for (const int lines : { 4, 8, 16 })
    {
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::lines] = lines == 4 ? 0.0f : (lines == 8 ? 1.0f : 2.0f);
        const std::string key = "routing.n" + std::to_string(lines);
        const std::size_t n = 4096;
        tp::Engine e;                                           // one silent sample computes the coefficients
        e.prepare(kFs, 256);
        e.setParams(v);
        float silence[2] = { 0.0f, 0.0f };
        float* ch[2] = { &silence[0], &silence[1] };
        e.process(ch, 2, 1, {});
        const tp::FdnLayout l = tp::makeLayout(static_cast<float>(kFs), 12.0f, lines);

        // The input gain's law, in double.
        const double T = static_cast<double>(v[tp::param::decay]) * 0.001;
        double meanG2 = 0.0;
        for (int i = 0; i < lines; ++i)
        {
            const double d = static_cast<double>(l.delay[static_cast<std::size_t>(i)]);
            const double g = std::pow(10.0, -3.0 * d / (T * kFs));
            meanG2 += g * g / lines;
        }
        const double kRef = std::sqrt(1.0 - meanG2), k = e.fdn().inputGain();
        P.le(key + ".input_gain_rel_err", std::fabs(k / kRef - 1.0), 1e-3);

        // The first arrivals: the model above against both outputs, for an impulse on each input.
        const double R = std::exp(-2.0 * std::numbers::pi * static_cast<double>(tp::Fdn::kDcHz) / kFs);
        const auto h = [R](long m) {
            return m < 0 ? 0.0 : (m == 0 ? 1.0 : (R - 1.0) * std::pow(R, static_cast<double>(m - 1)));
        };
        const double scale = std::sqrt(2.0 / lines) * k;
        for (const int side : { 0, 1 })                         // 0: the left input, 1: the right
        {
            const probe::Stereo ir = probe::impulseResponse(kFs, n, v, side == 0, side == 1);
            const std::vector<float>& fed = side == 0 ? ir.l : ir.r;
            const std::vector<float>& other = side == 0 ? ir.r : ir.l;
            const std::size_t window = l.delay[static_cast<std::size_t>(side)] + l.delay[0];
            double err = 0.0;
            int linesHeard = 0;
            for (std::size_t t = 0; t < window; ++t)
            {
                double model = 0.0;
                for (int i = side; i < lines; i += 2)
                {
                    const auto u = static_cast<std::size_t>(i);
                    model += scale * kTapIn[u] * kTapOut[u] * h(static_cast<long>(t) - static_cast<long>(l.delay[u]));
                }
                err = std::max({ err, std::fabs(static_cast<double>(fed[t]) - model),
                                 std::fabs(static_cast<double>(other[t])) });
            }
            for (int i = side; i < lines; i += 2)               // for the log: the lines whose first arrival is right
            {
                const auto u = static_cast<std::size_t>(i);
                if (std::fabs(static_cast<double>(fed[l.delay[u]]) - scale * kTapIn[u] * kTapOut[u]) < 1e-3)
                    ++linesHeard;
                else
                    std::printf("%s: line %d's first arrival at %u is %.6f, want %.6f\n", key.c_str(), i,
                                l.delay[u], static_cast<double>(fed[l.delay[u]]), scale * kTapIn[u] * kTapOut[u]);
            }
            const std::string sk = key + (side == 0 ? ".left_in" : ".right_in");
            P.le(sk + ".first_arrivals.max_err", err, 1e-6);
            P.eq(sk + ".lines_heard", static_cast<int64_t>(linesHeard), static_cast<int64_t>(lines / 2));
            P.eq(sk + ".cross_channel.first_sample", static_cast<int64_t>(firstNonZero(other)),
                 static_cast<int64_t>(l.delay[0] + l.delay[1]));
        }
    }

    // GAIN
    {
        tp::ParamValues v = probe::wetOnly();
        const std::size_t n = static_cast<std::size_t>(0.5 * kFs);
        const probe::Stereo a = probe::impulseResponse(kFs, n, v, true, true);
        v[tp::param::gain] = 6.0f;
        const probe::Stereo b = probe::impulseResponse(kFs, n, v, true, true);
        const double g = std::pow(10.0, 6.0 / 20.0);
        double worst = 0.0, peak = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            worst = std::max({ worst, std::fabs(b.l[i] - g * a.l[i]), std::fabs(b.r[i] - g * a.r[i]) });
            peak = std::max({ peak, std::fabs(g * a.l[i]), std::fabs(g * a.r[i]) });
        }
        P.le("gain.wet_linear_rel_err", worst / peak, 1e-5);
    }

    // SMOOTH: 0.5 s at 0 dB, then +12 dB from the next block on (blocks of 480 samples: the change lands at 0.5 s).
    {
        const std::size_t n = static_cast<std::size_t>(kFs), change = static_cast<std::size_t>(0.5 * kFs);
        const double amp = 0.25;
        const probe::Stereo in = probe::dual(probe::sine(1000.0, amp, kFs, n));
        tp::ParamValues v = tp::defaultValues();
        v[tp::param::mix] = 0.0f;
        const probe::Stereo out = probe::runEffect(in, kFs, 480, v, nullptr,
                                                   [&](std::size_t pos, tp::ParamValues& p, tp::Engine&) {
                                                       p[tp::param::gain] = pos < change ? 0.0f : 12.0f;
                                                   });
        const std::size_t at = change + static_cast<std::size_t>(0.010 * kFs) - 24;   // one cycle centred at 10 ms
        const double gainDb = probe::dbFromLin(probe::toneAmplitude(out.l, 1000.0, kFs, at, 48) / amp);
        P.in("smooth.gain_10ms_after_step_db", gainDb, 1.0, 11.0);
        P.near("smooth.gain_settled_db", probe::dbFromLin(probe::toneAmplitude(out.l, 1000.0, kFs, n - 4800, 4800) / amp),
               12.0, 0.02);
    }

    // GLIDE, LINES and SHAPE: a 220 Hz sine through the wet path, one change at 1 s.
    const std::size_t n = static_cast<std::size_t>(2.5 * kFs), change = static_cast<std::size_t>(kFs);
    const std::size_t window = static_cast<std::size_t>(0.3 * kFs);
    const probe::Stereo sine = probe::dual(probe::sine(220.0, 0.25, kFs, n));
    struct Glide { const char* key; float to; double bound; };
    for (const Glide g : { Glide{ "size12to13", 13.0f, -55.0 }, Glide{ "size12to30", 30.0f, -30.0 } })
    {
        tp::ParamValues v = probe::wetOnly();
        bool landed = false;
        const probe::Stereo out = probe::runEffect(sine, kFs, 256, v, nullptr,
                                                   [&](std::size_t pos, tp::ParamValues& p, tp::Engine& e) {
                                                       p[tp::param::size] = pos < change ? 12.0f : g.to;
                                                       if (pos < change + static_cast<std::size_t>(kFs) || landed)
                                                           return;
                                                       landed = true;
                                                       const tp::FdnLayout& l = e.fdn().layout();
                                                       for (int i = 0; i < l.lines; ++i)
                                                           landed = landed && e.fdn().delay(i) == static_cast<float>(
                                                                                  l.delay[static_cast<std::size_t>(i)]);
                                                   });
        const double cl = clickDb(out.l, change, window), cr = clickDb(out.r, change, window);
        std::printf("glide %s: %.1f / %.1f dB\n", g.key, cl, cr);
        const std::string key = std::string("clicks.glide.") + g.key;
        P.le(key + ".left.hf_db", cl, g.bound);
        P.le(key + ".right.hf_db", cr, g.bound);
        P.eq(key + ".landed_on_primes_within_1s", landed ? 1 : 0, 1);
    }
    {
        tp::ParamValues v = probe::wetOnly();
        int linesAfter = 0;
        const probe::Stereo out = probe::runEffect(sine, kFs, 256, v, nullptr,
                                                   [&](std::size_t pos, tp::ParamValues& p, tp::Engine& e) {
                                                       p[tp::param::lines] = pos < change ? 2.0f : 0.0f;
                                                       linesAfter = e.fdn().lines();
                                                   });
        const double cl = clickDb(out.l, change, window), cr = clickDb(out.r, change, window);
        std::printf("lines: click %.1f / %.1f dB\n", cl, cr);
        P.le("clicks.lines.16to4.left.hf_db", cl, -60.0);
        P.le("clicks.lines.16to4.right.hf_db", cr, -60.0);
        P.eq("clicks.lines.16to4.lines_after", static_cast<int64_t>(linesAfter), 4);
    }
    {
        tp::LfoShape flipped = tp::defaultShape();
        for (int i = 0; i < flipped.count; ++i)
            flipped.points[static_cast<std::size_t>(i)].y = 1.0f - flipped.points[static_cast<std::size_t>(i)].y;
        tp::LfoTable table;
        tp::renderTable(flipped, table);
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::moddepth] = 2.0f;
        const auto hook = [&](std::size_t pos, tp::ParamValues&, tp::Engine& e) {
            if (pos == change)
                e.setLfoTable(table);
        };
        const probe::Stereo out = probe::runEffect(sine, kFs, 256, v, nullptr, hook);
        const double cl = clickDb(out.l, change, window), cr = clickDb(out.r, change, window);
        std::printf("shape: click %.1f / %.1f dB\n", cl, cr);
        P.le("clicks.shape.flip.left.hf_db", cl, -60.0);
        P.le("clicks.shape.flip.right.hf_db", cr, -60.0);

        v[tp::param::moddepth] = 0.0f;
        const probe::Stereo still = probe::runEffect(sine, kFs, 256, v, nullptr, hook);
        const probe::Stereo plain = probe::runEffect(sine, kFs, 256, v, &table);
        P.eq("still.depth0.unmodulated_bits", probe::sameBits(still.l, plain.l) && probe::sameBits(still.r, plain.r)
                                              ? 1 : 0, 1);
    }
    // MONO
    {
        const std::size_t m = static_cast<std::size_t>(0.5 * kFs);
        const std::vector<float> x = noise(m, 3u, 0.25);
        const auto mono = [&](const tp::ParamValues& v) {
            tp::Engine e;
            e.prepare(kFs, 256);
            tp::LfoTable t;
            tp::renderTable(tp::defaultShape(), t);
            e.setLfoTable(t);
            std::vector<float> y = x;
            for (std::size_t pos = 0; pos < m; pos += 256)
            {
                e.setParams(v);
                float* ch[1] = { y.data() + pos };
                e.process(ch, 1, static_cast<int>(std::min<std::size_t>(256, m - pos)), {});
            }
            return y;
        };
        tp::ParamValues dry = tp::defaultValues();
        dry[tp::param::mix] = 0.0f;
        dry[tp::param::gain] = -6.0f;
        const std::vector<float> yd = mono(dry);
        const double g = std::pow(10.0, -6.0 / 20.0);
        double errDry = 0.0;
        for (std::size_t i = 0; i < m; ++i)
            errDry = std::max(errDry, std::fabs(yd[i] - g * x[i]));
        P.le("mono.dry.max_err", errDry, 1e-6);

        const tp::ParamValues wet = probe::wetOnly();
        const std::vector<float> yw = mono(wet);
        const probe::Stereo st = probe::runEffect(probe::dual(x), kFs, 256, wet);
        double errWet = 0.0;
        for (std::size_t i = 0; i < m; ++i)
            errWet = std::max(errWet, std::fabs(yw[i] - (st.l[i] + st.r[i]) * 0.707106781));
        P.le("mono.wet.max_err", errWet, 1e-6);
    }

    // SANITIZE: poison among noise, against the same noise with the poison cleaned as fsdsp::sanitize cleans it.
    {
        const std::size_t m = static_cast<std::size_t>(1.5 * kFs);
        probe::Stereo dirty;
        dirty.l = noise(m, 4u, 0.25);
        dirty.r = noise(m, 5u, 0.25);
        std::fill(dirty.l.begin() + static_cast<std::ptrdiff_t>(kFs / 4), dirty.l.end(), 0.0f);   // then the tail
        std::fill(dirty.r.begin() + static_cast<std::ptrdiff_t>(kFs / 4), dirty.r.end(), 0.0f);
        probe::Stereo clean = dirty;
        const float nan = std::numeric_limits<float>::quiet_NaN(), snan = std::numeric_limits<float>::signaling_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        struct Poison { std::size_t at; float value, cleaned; bool left; };
        for (const Poison& x : { Poison{ 1000, nan, 0.0f, true }, Poison{ 1001, nan, 0.0f, false },
                                 Poison{ 2400, inf, 0.0f, true }, Poison{ 3100, -inf, 0.0f, false },
                                 Poison{ 4000, snan, 0.0f, true }, Poison{ 5200, 1e30f, 1e6f, false },
                                 Poison{ 6100, -1e30f, -1e6f, true }, Poison{ 9000, inf, 0.0f, true },
                                 Poison{ 9000, nan, 0.0f, false } })
        {
            (x.left ? dirty.l : dirty.r)[x.at] = x.value;
            (x.left ? clean.l : clean.r)[x.at] = x.cleaned;
        }
        const tp::ParamValues v = tp::defaultValues();
        const probe::Stereo outDirty = probe::runEffect(dirty, kFs, 256, v);
        const probe::Stereo outClean = probe::runEffect(clean, kFs, 256, v);
        P.eq("sanitize.output_finite", probe::allFinite(outDirty.l) && probe::allFinite(outDirty.r) ? 1 : 0, 1);
        P.eq("sanitize.same_bits_as_cleaned_input",
             probe::sameBits(outDirty.l, outClean.l) && probe::sameBits(outDirty.r, outClean.r) ? 1 : 0, 1);
    }

    // DC: the inputs' DC blockers.
    {
        const auto meanOf = [](const std::vector<float>& x, std::size_t from, std::size_t count) {
            double sum = 0.0;
            for (std::size_t i = from; i < from + count; ++i)
                sum += static_cast<double>(x[i]);
            return sum / static_cast<double>(count);
        };
        const auto peakOf = [](const std::vector<float>& x) {
            double peak = 0.0;
            for (const float s : x)
                peak = std::max(peak, std::fabs(static_cast<double>(s)));
            return peak;
        };
        const std::size_t sec = static_cast<std::size_t>(kFs);
        {
            tp::ParamValues v = tp::defaultValues();
            v[tp::param::mix] = 100.0f;
            probe::Stereo in(6 * sec);
            std::fill(in.l.begin(), in.l.end(), 0.01f);           // -40 dBFS
            std::fill(in.r.begin(), in.r.end(), 0.01f);
            const probe::Stereo out = probe::runEffect(in, kFs, 256, v);
            const double l = probe::dbFromLin(std::fabs(meanOf(out.l, 5 * sec, sec)) / 0.01);
            const double r = probe::dbFromLin(std::fabs(meanOf(out.r, 5 * sec, sec)) / 0.01);
            std::printf("dc at the defaults: mean out %.1f / %.1f dB of the DC in\n", l, r);
            P.le("dc.defaults.left.mean_db", l, -60.0);
            P.le("dc.defaults.right.mean_db", r, -60.0);
        }
        {
            tp::ParamValues v = probe::wetOnly();
            v[tp::param::size] = 1.0f;
            v[tp::param::lines] = 0.0f;
            v[tp::param::decay] = 20000.0f;
            probe::Stereo in(30 * sec);
            std::fill(in.l.begin(), in.l.end(), 1.0f);
            std::fill(in.r.begin(), in.r.end(), 1.0f);
            const probe::Stereo out = probe::runEffect(in, kFs, 256, v);
            const double peak = probe::dbFromLin(std::max(peakOf(out.l), peakOf(out.r)));
            const double mean = probe::dbFromLin(std::max(std::fabs(meanOf(out.l, 25 * sec, 5 * sec)),
                                                          std::fabs(meanOf(out.r, 25 * sec, 5 * sec))));
            std::printf("dc at 1 m, 4 lines, 20 s: peak %.1f dBFS, mean over 25-30 s %.1f dB\n", peak, mean);
            P.le("dc.corner.peak_dbfs", peak, 0.0);
            P.le("dc.corner.mean_25s_30s_db", mean, -40.0);
        }
    }
    return P.finish();
}
