// PROBE name=dsp.render timeout=120
//
// dsp.render: renders the reverb to WAV files for listening and for other tools (48 kHz, 32-bit float, stereo):
//   <build>/probe-out/dsp.render.ir.wav    the impulse response at the defaults (SIZE 12 m, DECAY 2 s, HF DECAY 50 %,
//                                          16 LINES, the LFO at 0.5 Hz and 0.25 ms) with MIX 100 %: a unit impulse on
//                                          both inputs, 4 s
//   <build>/probe-out/dsp.render.in.wav    a test signal: three short noise bursts and two clicks, -12 dBFS, in the
//                                          first 3.05 s of 6 s
//   <build>/probe-out/dsp.render.wav       that signal through the reverb at the defaults (MIX 30 %)
// and checks what must hold of any render: every sample finite, no clipping, the impulse response's energy and its
// first arrival (the shortest line: 787 samples at 12 m, or one sample sooner, where the modulated Thiran read's
// x[n - N] term first sees the impulse: N = floor(D - 1/2), DelayLine.h), a tail that rings on after the input stops
// and falls as DECAY says (2 s: -45 dB in 1.5 s, within 10 dB: a burst's tail is short of modes to average), and the
// same bits whatever the host's block size (64 and 333 samples).
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

namespace tp = testingplayground;

PROBE(dsp, render)
{
    constexpr double fs = 48000.0;

    // The impulse response.
    tp::ParamValues ir = tp::defaultValues();
    ir[tp::param::mix] = 100.0f;
    const std::size_t irN = static_cast<std::size_t>(4.0 * fs);
    const probe::Stereo h = probe::impulseResponse(fs, irN, ir, true, true, 64);
    const probe::Stereo h333 = probe::impulseResponse(fs, irN, ir, true, true, 333);
    double e = 0.0;
    std::size_t first = irN;
    for (std::size_t i = 0; i < irN; ++i)
    {
        e += static_cast<double>(h.l[i]) * h.l[i] + static_cast<double>(h.r[i]) * h.r[i];
        if (first == irN && (h.l[i] != 0.0f || h.r[i] != 0.0f))
            first = i;
    }
    P.eq("ir.finite", probe::allFinite(h.l) && probe::allFinite(h.r) ? 1 : 0, 1);
    P.le("ir.peak_dbfs", probe::dbFromLin(std::max(probe::peak(h.l), probe::peak(h.r))), 0.0);
    P.in("ir.first_arrival_samples", static_cast<double>(first), 786.0, 787.0);   // line 0 at 12 m, 48 kHz
    P.in("ir.energy_db", 10.0 * std::log10(e), -12.0, 6.0);                 // ~0 dB: the input gain's normalisation
    P.eq("ir.block_size_invariant", probe::sameBits(h.l, h333.l) && probe::sameBits(h.r, h333.r) ? 1 : 0, 1);

    // The test signal: noise bursts (50 ms, Hann-shaped) at 0, 1.5 and 3 s and clicks at 0.75 and 2.25 s.
    const std::size_t n = static_cast<std::size_t>(6.0 * fs);
    std::vector<float> x(n, 0.0f);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const double amp = std::pow(10.0, -12.0 / 20.0);
    for (const double at : { 0.0, 1.5, 3.0 })
    {
        const auto start = static_cast<std::size_t>(at * fs), len = static_cast<std::size_t>(0.05 * fs);
        for (std::size_t i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i)
                                                  / static_cast<double>(len));
            x[start + i] = static_cast<float>(amp * w * u(rng));
        }
    }
    for (const double at : { 0.75, 2.25 })
        x[static_cast<std::size_t>(at * fs)] = static_cast<float>(amp);
    probe::Stereo in;
    in.l = x;
    in.r.assign(n, 0.0f);
    for (std::size_t i = 0; i + 240 < n; ++i)                   // the right input: the same, 5 ms later
        in.r[i + 240] = x[i];

    const tp::ParamValues values = tp::defaultValues();
    const probe::Stereo out = probe::runEffect(in, fs, 64, values);
    const probe::Stereo other = probe::runEffect(in, fs, 333, values);

    P.eq("render.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.le("render.peak_dbfs", probe::dbFromLin(std::max(probe::peak(out.l), probe::peak(out.r))), 0.0);
    const auto rmsAt = [&](double from, double len) {
        return probe::rmsDb(out.l, static_cast<std::size_t>(from * fs), static_cast<std::size_t>(len * fs));
    };
    std::printf("tail: %.1f dBFS at 3.5 s, %.1f dBFS at 5 s\n", rmsAt(3.5, 0.5), rmsAt(5.0, 0.5));
    P.ge("render.tail_3s5_to_4s_rms_dbfs", rmsAt(3.5, 0.5), -70.0);     // the reverb rings on after the input stops
    P.near("render.tail_fall_1s5_db", rmsAt(5.0, 0.5) - rmsAt(3.5, 0.5), -45.0, 10.0);   // and falls 60 dB per 2 s
    P.eq("render.block_size_invariant", probe::sameBits(out.l, other.l) && probe::sameBits(out.r, other.r) ? 1 : 0, 1);

    const std::string irPath = probe::outPath("dsp.render.ir.wav");
    const std::string inPath = probe::outPath("dsp.render.in.wav"), outPath = probe::outPath("dsp.render.wav");
    P.eq("render.wav_written", probe::writeWav(irPath, h, 48000) && probe::writeWav(inPath, in, 48000)
                               && probe::writeWav(outPath, out, 48000) ? 1 : 0, 1);
    std::printf("wrote %s\nwrote %s\nwrote %s\n", irPath.c_str(), inPath.c_str(), outPath.c_str());
    return P.finish();
}
