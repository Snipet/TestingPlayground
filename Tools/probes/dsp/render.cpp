// PROBE name=dsp.render timeout=120
//
// dsp.render: plays a short phrase through the Engine to a WAV file, for listening and for other tools:
//   <build>/probe-out/dsp.render.wav    5.5 s at 48 kHz, 32-bit float stereo: a melody over a pad of chords, with an
//                                       ADSR of ATTACK 8 ms, DECAY 300 ms, SUSTAIN 60 %, RELEASE 400 ms; the last bar
//                                       holds more notes than there are voices (sixteen), so it steals
// and checks what must hold of any render: every sample finite, no clipping, a signal at all, both channels the same,
// and the same bits whatever the host's block size (64 and 333 samples: the events land at other offsets of other
// blocks, and the ramps end inside other blocks).
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace tps = probe::tps;

    std::vector<probe::ScoreEvent> phrase(double fs)
    {
        const double beat = 0.25;                                   // 120 BPM, in eighth notes
        const auto at = [fs](double seconds) { return static_cast<std::size_t>(seconds * fs); };
        std::vector<probe::ScoreEvent> s;
        const auto note = [&](double startBeat, double beats, int n, float velocity) {
            s.push_back(probe::on(at(startBeat * beat), n, velocity));
            s.push_back(probe::off(at((startBeat + beats) * beat - 0.01), n));
        };

        // The pad: four chords of four notes, a bar each, then a last chord of twelve.
        const int chords[4][4] = { { 48, 55, 60, 64 }, { 45, 52, 57, 60 }, { 41, 48, 53, 57 }, { 43, 50, 55, 59 } };
        for (int c = 0; c < 4; ++c)
            for (const int n : chords[c])
                note(4.0 * c, 4.0, n, 0.35f);
        for (const int n : { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 })
            note(16.0, 3.0, n, 0.15f);

        // The melody, eighth notes, over the pad.
        const int melody[] = { 72, 74, 76, 79, 76, 74, 72, 67, 69, 72, 76, 74, 72, 71, 74, 79 };
        for (int k = 0; k < 16; ++k)
            note(static_cast<double>(k), 0.9, melody[k], 0.6f);
        note(16.0, 2.0, 86, 0.6f);                                 // with the chord and the releasing notes:
        note(16.5, 2.0, 88, 0.5f);                                 //   more notes than voices, so these steal
        note(17.0, 2.0, 91, 0.5f);

        std::stable_sort(s.begin(), s.end(), [](const probe::ScoreEvent& a, const probe::ScoreEvent& b) {
            return a.at < b.at;
        });
        return s;
    }

    tps::ParamValues settings()
    {
        tps::ParamValues v = tps::defaultValues();
        v[tps::param::attack] = 8.0f;
        v[tps::param::decay] = 300.0f;
        v[tps::param::sustain] = 60.0f;
        v[tps::param::release] = 400.0f;
        return v;
    }

    probe::Stereo render(double fs, int block)
    {
        return probe::runSynth(phrase(fs), static_cast<std::size_t>(5.5 * fs), fs, block, settings());
    }
}

PROBE(dsp, render)
{
    constexpr double fs = 48000.0;
    const probe::Stereo out = render(fs, 64);
    const probe::Stereo other = render(fs, 333);

    P.eq("render.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
    P.le("render.peak_dbfs", probe::dbFromLin(std::max(probe::peak(out.l), probe::peak(out.r))), 0.0);
    P.ge("render.rms_dbfs", probe::rmsDb(out.l, 0, out.size()), -40.0);
    P.eq("render.left_equals_right", probe::sameBits(out.l, out.r) ? 1 : 0, 1);
    P.eq("render.block_size_invariant", probe::sameBits(out.l, other.l) && probe::sameBits(out.r, other.r) ? 1 : 0, 1);

    const std::string path = probe::outPath("dsp.render.wav");
    P.eq("render.wav_written", probe::writeWav(path, out, 48000) ? 1 : 0, 1);
    std::printf("wrote %s\n", path.c_str());
    return P.finish();
}
