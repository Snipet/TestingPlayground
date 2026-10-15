// PROBE name=dsp.decay timeout=180
//
// dsp.decay: the decay time the network measures against DECAY, the parameter that asks for it (48 kHz).
//   T60     at DECAY 0.5, 2 and 8 s (SIZE 12 m, 16 LINES, HF DECAY 100 %: no damping, DEPTH 0: no modulation, MIX
//           100 %): T60 from the energy decay curve of the stereo impulse response (Schroeder integration; the line
//           through -5 to -35 dB, extrapolated to 60 dB), within 3 % of DECAY. The fit's correlation shows the decay is
//           a straight line in dB (|r| >= 0.999).
//   LINES   the same at 4 and 8 lines (DECAY 2 s), at SIZE 2 m and 60 m, and at 44.1 and 96 kHz: every line loses
//           60 dB per DECAY seconds, whatever its length and the rate, so neither the order, the size nor the sample
//           rate moves it (within 3 %).
//   HF      at DECAY 2 s and HF DECAY 50 %, the decay in an octave band at 250 Hz and a third-octave band at 12 kHz
//           against what the absorption filters give there (each line's one-pole: |H(w)| = b / |1 - p e^-jw|, a loss
//           of -20 log10 |H(w)| dB per pass of D_i samples, averaged over the lines), within 5 % (the bands hold a few
//           dozen modes, whose beating bends the curve: the fit runs from -5 to -25 dB); at 12 kHz the decay is
//           shorter than at 250 Hz by more than a quarter.
// The impulse responses run 1.5 T60 long: the curve's -35 dB point is far from the truncation (-90 dB).
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;

    probe::DecayFit measure(const tp::ParamValues& v, double t60, double fs = kFs)
    {
        const auto n = static_cast<std::size_t>((1.5 * t60 + 0.2) * fs);
        return probe::decayFit(probe::energy(probe::impulseResponse(fs, n, v, true, true)), fs);
    }

    // The decay time the absorption filters give at `hz`: per line, -20 log10 |H(w)| dB per D_i samples.
    double expectedT60(const tp::ParamValues& v, double hz)
    {
        const tp::FdnLayout l = tp::makeLayout(static_cast<float>(kFs), v[tp::param::size],
                                               tp::linesFromChoice(v[tp::param::lines]));
        const double t = static_cast<double>(v[tp::param::decay]) / 1000.0;
        const double r = static_cast<double>(v[tp::param::hfdecay]) / 100.0;
        const double w = 2.0 * std::numbers::pi * hz / kFs;
        double dbPerSec = 0.0;
        for (int i = 0; i < l.lines; ++i)
        {
            const double d = static_cast<double>(l.delay[static_cast<std::size_t>(i)]);
            const double g = std::pow(10.0, -3.0 * d / (t * kFs)), h = std::pow(10.0, -3.0 * d / (r * t * kFs));
            const double p = (g - h) / (g + h), b = g * (1.0 - p);
            const double mag = b / std::abs(1.0 - p * std::polar(1.0, -w));
            dbPerSec += -20.0 * std::log10(mag) * kFs / d;
        }
        dbPerSec /= static_cast<double>(l.lines);
        return 60.0 / dbPerSec;
    }

    probe::DecayFit measureBand(const tp::ParamValues& v, double hz, double q, double t60)
    {
        const auto n = static_cast<std::size_t>((1.5 * t60 + 0.2) * kFs);
        const probe::Stereo h = probe::impulseResponse(kFs, n, v, true, true);
        std::vector<double> l(n), r(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            l[i] = h.l[i];
            r[i] = h.r[i];
        }
        l = probe::bandPass(l, hz, q, kFs);
        r = probe::bandPass(r, hz, q, kFs);
        std::vector<double> e(n);
        for (std::size_t i = 0; i < n; ++i)
            e[i] = l[i] * l[i] + r[i] * r[i];
        return probe::decayFit(e, kFs, -5.0, -25.0);
    }
}

PROBE(dsp, decay)
{
    // T60 against DECAY.
    for (const float decayMs : { 500.0f, 2000.0f, 8000.0f })
    {
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::decay] = decayMs;
        const double want = static_cast<double>(decayMs) / 1000.0;
        const probe::DecayFit f = measure(v, want);
        const std::string key = "t60.decay" + std::to_string(static_cast<int>(decayMs)) + "ms";
        std::printf("%s: T60 %.4f s (fit %.3f..%.3f s, r %.6f)\n", key.c_str(), f.t60, f.fromSec, f.toSec, f.r);
        P.near(key + ".seconds", f.t60, want, 0.03 * want);
        P.le(key + ".fit_r", f.r, -0.999);
    }

    // Neither the line count, the size nor the rate moves it.
    struct Case { const char* key; float size; float linesChoice; double fs; };
    for (const Case c : { Case{ "lines4", 12.0f, 0.0f, kFs }, Case{ "lines8", 12.0f, 1.0f, kFs },
                          Case{ "size2m", 2.0f, 2.0f, kFs }, Case{ "size60m", 60.0f, 2.0f, kFs },
                          Case{ "fs44k1", 12.0f, 2.0f, 44100.0 }, Case{ "fs96k", 12.0f, 2.0f, 96000.0 } })
    {
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::decay] = 2000.0f;
        v[tp::param::size] = c.size;
        v[tp::param::lines] = c.linesChoice;
        const probe::DecayFit f = measure(v, 2.0, c.fs);
        std::printf("t60.%s: T60 %.4f s (r %.6f)\n", c.key, f.t60, f.r);
        P.near(std::string("t60.") + c.key + ".seconds", f.t60, 2.0, 0.06);
    }

    // HF DECAY: the decay by band against the absorption filters' design.
    {
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::decay] = 2000.0f;
        v[tp::param::hfdecay] = 50.0f;
        double measured[2] = {};
        int k = 0;
        for (const double hz : { 250.0, 12000.0 })
        {
            const double want = expectedT60(v, hz);
            const double q = hz < 1000.0 ? 1.414 : 4.32;           // an octave, a third of an octave
            const probe::DecayFit f = measureBand(v, hz, q, 2.0);
            measured[k++] = f.t60;
            const std::string key = "hf.band" + std::to_string(static_cast<int>(hz)) + "hz";
            std::printf("%s: T60 %.4f s, expected %.4f s (r %.6f)\n", key.c_str(), f.t60, want, f.r);
            P.near(key + ".seconds", f.t60, want, 0.05 * want);
        }
        P.le("hf.12k_over_250", measured[1] / measured[0], 0.75);
    }
    return P.finish();
}
