// PROBE name=dsp.render timeout=120
//
// dsp.render: renders a test signal through the Engine to WAV files for listening and for other tools:
//   <build>/probe-out/dsp.render.in.wav    the input: an exponential sine sweep, 20 Hz to 20 kHz over 4 s, -12 dBFS,
//                                          then 1 s of silence (the tail)
//   <build>/probe-out/dsp.render.wav       the output with GAIN +6 dB and TONE 2 kHz
// and checks what must hold of any render: every sample finite, no clipping, a signal at all, and the same bits
// whatever the host's block size (64 and 333 samples).
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace fpt = testingplaygroundsynth;

PROBE(dsp, render)
{
    constexpr double fs = 48000.0;
    const std::size_t sweepN = static_cast<std::size_t>(4.0 * fs), n = sweepN + static_cast<std::size_t>(fs);

    // Exponential sweep: phase = 2 pi f0 T / ln(f1 / f0) (e^(t ln(f1 / f0) / T) - 1).
    std::vector<float> x(n, 0.0f);
    const double f0 = 20.0, f1 = 20000.0, T = 4.0, k = std::log(f1 / f0);
    const double amp = std::pow(10.0, -12.0 / 20.0);
    for (std::size_t i = 0; i < sweepN; ++i)
    {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * f0 * T / k * (std::exp(t * k / T) - 1.0)));
    }
    const probe::Stereo in = probe::dual(x);

    fpt::ParamValues values = fpt::defaultValues();
    values[fpt::param::gain] = 6.0f;
    values[fpt::param::tone] = 2000.0f;

    const probe::Stereo out = probe::runEffect(in, fs, 64, values);
    const probe::Stereo other = probe::runEffect(in, fs, 333, values);

    P.eq("render.finite", probe::allFinite(out.l) && probe::allFinite(out.r), 1);
    P.le("render.peak_dbfs", probe::dbFromLin(std::max(probe::peak(out.l), probe::peak(out.r))), 0.0);
    P.ge("render.rms_dbfs", probe::rmsDb(out.l, 0, sweepN), -40.0);
    P.eq("render.block_size_invariant", probe::sameBits(out.l, other.l) && probe::sameBits(out.r, other.r), 1);

    const std::string inPath = probe::outPath("dsp.render.in.wav"), outPath = probe::outPath("dsp.render.wav");
    P.eq("render.wav_written", probe::writeWav(inPath, in, 48000) && probe::writeWav(outPath, out, 48000), 1);
    std::printf("wrote %s\nwrote %s\n", inPath.c_str(), outPath.c_str());
    return P.finish();
}
