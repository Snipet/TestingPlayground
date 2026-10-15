// PROBE name=dsp.response timeout=120
//
// dsp.response: the effect against the formulas it implements, on both channels.
//   MODEL   each channel equals a double-precision model of the effect, sample by sample: y = GAIN x (the one-pole
//           low-pass of x), the one-pole in its topology-preserving form (g = tan(pi fc / fs), G = g / (1 + g); per
//           sample v = (x - s) G, y = v + s, s = y + v). Left and right carry different noises, so a channel swapped,
//           left out or turned upside down fails: the residual stays under -80 dB of the signal (float rounding and
//           the core's fast tan and dB conversions sit far below that). At GAIN +6 dB, TONE 2 kHz and at GAIN
//           -24 dB, TONE 200 Hz.
//   GAIN    a 1 kHz sine at -12 dBFS (left) and at -18 dBFS (right) comes out GAIN dB louder (TONE wide open: its own
//           -0.001 dB at 1 kHz is in the expected value), for GAIN -24, 0 and +6 dB; within 0.02 dB.
//   TONE    |H(f)| = 1 / sqrt(1 + (tan(pi f / fs) / tan(pi fc / fs))^2), so -3.01 dB at fc; checked at fc = 1 kHz for
//           1 kHz and 8 kHz, and at fc = 5 kHz for 1 kHz, on both channels; within 0.05 dB.
//   SMOOTH  a GAIN change from 0 to +12 dB between two blocks is smoothed (20 ms), not stepped: 10 ms after it the
//           gain is between the two.
// Levels are single-bin DFT amplitudes over a whole number of cycles, measured after 0.5 s (the smoothers settled).
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <utility>

namespace fpt = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr double kAmpL = 0.25118864315095801;             // -12 dBFS
    constexpr double kAmpR = 0.12589254117941673;             // -18 dBFS

    double onePoleDb(double f, double fc)
    {
        const double r = std::tan(std::numbers::pi * f / kFs) / std::tan(std::numbers::pi * fc / kFs);
        return -10.0 * std::log10(1.0 + r * r);
    }

    // The effect in double, at fixed parameters.
    std::vector<float> model(const std::vector<float>& x, double gainDb, double fc)
    {
        const double g = std::tan(std::numbers::pi * fc / kFs), G = g / (1.0 + g), gain = std::pow(10.0, gainDb / 20.0);
        std::vector<float> y(x.size());
        double s = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double v = (static_cast<double>(x[i]) - s) * G;
            const double lp = v + s;
            s = lp + v;
            y[i] = static_cast<float>(gain * lp);
        }
        return y;
    }

    // 20 log10(rms(out - ref) / rms(ref)), in dB.
    double residualDb(const std::vector<float>& out, const std::vector<float>& ref)
    {
        double e = 0.0, r = 0.0;
        for (std::size_t i = 0; i < ref.size(); ++i)
        {
            const double d = static_cast<double>(out[i]) - static_cast<double>(ref[i]);
            e += d * d;
            r += static_cast<double>(ref[i]) * static_cast<double>(ref[i]);
        }
        return 10.0 * std::log10(std::max(e, 1e-300) / std::max(r, 1e-300));
    }

    std::vector<float> noise(std::size_t n, unsigned seed, double amp)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(-amp, amp);
        std::vector<float> v(n);
        for (float& s : v)
            s = static_cast<float>(u(rng));
        return v;
    }

    // The levels (dBFS) of the `hz` component, left and right, of 1.5 s sines at -12 dBFS (left) and -18 dBFS (right,
    // a quarter cycle ahead) through the Engine, from 0.5 s to 1.5 s.
    std::pair<double, double> levelsDb(double hz, float gainDb, float toneHz)
    {
        const std::size_t n = static_cast<std::size_t>(1.5 * kFs), from = static_cast<std::size_t>(0.5 * kFs);
        fpt::ParamValues v = fpt::defaultValues();
        v[fpt::param::gain] = gainDb;
        v[fpt::param::tone] = toneHz;
        probe::Stereo in;
        in.l = probe::sine(hz, kAmpL, kFs, n);
        in.r.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            in.r[i] = static_cast<float>(kAmpR * std::cos(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kFs));
        const probe::Stereo out = probe::runEffect(in, kFs, 256, v);
        const auto len = static_cast<std::size_t>(kFs);
        return { probe::dbFromLin(probe::toneAmplitude(out.l, hz, kFs, from, len)),
                 probe::dbFromLin(probe::toneAmplitude(out.r, hz, kFs, from, len)) };
    }

    struct ModelCase { const char* key; float gain, tone; };
    struct ToneCase { const char* key; double hz; float fc; };
}

PROBE(dsp, response)
{
    // MODEL
    {
        const std::size_t n = static_cast<std::size_t>(kFs);
        probe::Stereo in;
        in.l = noise(n, 1u, 0.25);
        in.r = noise(n, 2u, 0.25);
        for (const ModelCase c : { ModelCase{ "gain6.tone2k", 6.0f, 2000.0f },
                                   ModelCase{ "gain-24.tone200", -24.0f, 200.0f } })
        {
            fpt::ParamValues v = fpt::defaultValues();
            v[fpt::param::gain] = c.gain;
            v[fpt::param::tone] = c.tone;
            const probe::Stereo out = probe::runEffect(in, kFs, 256, v);
            const std::string key = std::string("model.") + c.key;
            const double gain = static_cast<double>(c.gain), fc = static_cast<double>(c.tone);
            P.le(key + ".left.residual_db", residualDb(out.l, model(in.l, gain, fc)), -80.0);
            P.le(key + ".right.residual_db", residualDb(out.r, model(in.r, gain, fc)), -80.0);
        }
    }

    // GAIN
    const double open = onePoleDb(1000.0, 20000.0);           // TONE at its maximum, at 1 kHz
    const double inL = probe::dbFromLin(kAmpL), inR = probe::dbFromLin(kAmpR);
    for (const float g : { -24.0f, 0.0f, 6.0f })
    {
        const std::string key = "gain." + std::to_string(static_cast<int>(g)) + "db";
        const auto [l, r] = levelsDb(1000.0, g, 20000.0f);
        P.near(key + ".left.level_dbfs", l, inL + static_cast<double>(g) + open, 0.02);
        P.near(key + ".right.level_dbfs", r, inR + static_cast<double>(g) + open, 0.02);
    }

    // TONE
    for (const ToneCase t : { ToneCase{ "tone.fc1k.at1k", 1000.0, 1000.0f },
                              ToneCase{ "tone.fc1k.at8k", 8000.0, 1000.0f },
                              ToneCase{ "tone.fc5k.at1k", 1000.0, 5000.0f } })
    {
        const auto [l, r] = levelsDb(t.hz, 0.0f, t.fc);
        const double want = onePoleDb(t.hz, static_cast<double>(t.fc));
        P.near(std::string(t.key) + ".left.rel_db", l - inL, want, 0.05);
        P.near(std::string(t.key) + ".right.rel_db", r - inR, want, 0.05);
    }

    // SMOOTH: 0.5 s at 0 dB, then +12 dB from the next block on (blocks of 480 samples: the change lands at 0.5 s).
    {
        const std::size_t n = static_cast<std::size_t>(kFs), change = static_cast<std::size_t>(0.5 * kFs);
        const probe::Stereo in = probe::dual(probe::sine(1000.0, kAmpL, kFs, n));
        fpt::Engine engine;
        engine.prepare(kFs, 480);
        probe::Stereo out = in;
        for (std::size_t pos = 0; pos < n; pos += 480)
        {
            fpt::ParamValues v = fpt::defaultValues();
            v[fpt::param::gain] = pos < change ? 0.0f : 12.0f;
            engine.setParams(v);
            float* ch[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(ch, 2, 480, {});
        }
        // One 1 kHz cycle (48 samples) centred 10 ms after the change.
        const std::size_t at = change + static_cast<std::size_t>(0.010 * kFs) - 24;
        const double gainDb = probe::dbFromLin(probe::toneAmplitude(out.l, 1000.0, kFs, at, 48)) - inL;
        P.in("smooth.gain_10ms_after_step_db", gainDb, 1.0, 11.0);
        P.near("smooth.gain_settled_db",
               probe::dbFromLin(probe::toneAmplitude(out.l, 1000.0, kFs, n - 4800, 4800)) - inL, 12.0 + open, 0.02);
    }
    return P.finish();
}
