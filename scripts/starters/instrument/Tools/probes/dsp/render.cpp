// PROBE name=dsp.render timeout=120
//
// dsp.render: plays a short score through the Engine to a WAV file for listening and for other tools:
//   <build>/probe-out/dsp.render.wav    an arpeggio (C4 E4 G4 C5), a chord (C4 E4 G4 B4), then the release tail;
//                                       sine for the first half, triangle for the second
// and checks what must hold of any render: every sample finite, no clipping, a signal at all, and the same bits
// whatever the host's block size (64 and 333 samples).
#include "Probe.h"
#include "Rig.h"

#include <cstdio>
#include <vector>

namespace tps = testingplaygroundsynth;

namespace
{
    std::vector<probe::ScoreEvent> score(double fs)
    {
        const auto at = [fs](double seconds) { return static_cast<std::size_t>(seconds * fs); };
        std::vector<probe::ScoreEvent> s;
        const int arp[4] = { 60, 64, 67, 72 };
        for (int k = 0; k < 4; ++k)
        {
            s.push_back(probe::on(at(0.25 * k), arp[k], 0.8f));
            s.push_back(probe::off(at(0.25 * k + 0.2), arp[k]));
        }
        for (const int note : { 60, 64, 67, 71 })
            s.push_back(probe::on(at(1.25), note, 0.6f));
        for (const int note : { 60, 64, 67, 71 })
            s.push_back(probe::off(at(2.25), note));
        return s;
    }

    probe::Stereo render(double fs, int block)
    {
        const std::size_t n = static_cast<std::size_t>(3.0 * fs);
        tps::ParamValues sine = tps::defaultValues(), triangle = tps::defaultValues();
        triangle[tps::param::wave] = 1.0f;
        probe::Stereo a = probe::runSynth(score(fs), n, fs, block, sine);
        const probe::Stereo b = probe::runSynth(score(fs), n, fs, block, triangle);
        for (std::size_t i = n / 2; i < n; ++i)                  // the second half from the triangle render
        {
            a.l[i] = b.l[i];
            a.r[i] = b.r[i];
        }
        return a;
    }
}

PROBE(dsp, render)
{
    constexpr double fs = 48000.0;
    const probe::Stereo out = render(fs, 64);
    const probe::Stereo other = render(fs, 333);

    P.eq("render.finite", probe::allFinite(out.l) && probe::allFinite(out.r), 1);
    P.le("render.peak_dbfs", probe::dbFromLin(std::max(probe::peak(out.l), probe::peak(out.r))), 0.0);
    P.ge("render.rms_dbfs", probe::rmsDb(out.l, 0, out.size()), -40.0);
    P.eq("render.block_size_invariant", probe::sameBits(out.l, other.l) && probe::sameBits(out.r, other.r), 1);

    const std::string path = probe::outPath("dsp.render.wav");
    P.eq("render.wav_written", probe::writeWav(path, out, 48000), 1);
    std::printf("wrote %s\n", path.c_str());
    return P.finish();
}
