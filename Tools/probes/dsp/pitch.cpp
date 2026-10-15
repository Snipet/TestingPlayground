// PROBE name=dsp.pitch timeout=120
//
// dsp.pitch: every note at its equal-tempered pitch, 440 * 2^((n - 69) / 12) Hz.
//   A4       held for 1.6 s at 44.1, 48 and 96 kHz: its frequency over [0.5, 1.5) s, from the rising zero crossings
//            (each placed by linear interpolation), within 0.5 cent of 440 Hz
//   RANGE    the same for notes across the keyboard at 48 kHz: A0 (27.5 Hz) to C8 (4186 Hz)
//   WORST    the largest error of all of them, under 0.01 cent: the phase accumulator's increment is within 0.002 cents
//            of the pitch (fsdsp::phase::incFromPitch) and rounds to 2^-32 cycles, so nothing near the spec's 0.5 cent
//            should ever show; a larger error means the oscillator changed
//   STEADY   a held note does not drift: the frequency over the first and the second half of the window agree within
//            0.001 cent
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
    probe::Stereo held(int note, double fs)
    {
        const auto n = static_cast<std::size_t>(1.6 * fs);
        return probe::runSynth({ probe::on(0, note, 1.0f) }, n, fs, 256, probe::tps::defaultValues());
    }

    double centsOff(const probe::Stereo& out, int note, double fs, double fromSec, double lenSec)
    {
        const double hz = probe::frequencyHz(out.l, fs, static_cast<std::size_t>(fromSec * fs),
                                             static_cast<std::size_t>(lenSec * fs));
        return hz > 0.0 ? probe::centsBetween(hz, probe::noteHz(note)) : 1e9;
    }
}

PROBE(dsp, pitch)
{
    double worst = 0.0;

    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const probe::Stereo out = held(69, fs);
        const double c = centsOff(out, 69, fs, 0.5, 1.0);
        worst = std::max(worst, std::fabs(c));
        P.near("a4.fs" + std::to_string(static_cast<int>(fs)) + ".cents", c, 0.0, 0.5);
    }

    for (const int note : { 21, 33, 45, 57, 60, 64, 67, 81, 93, 105, 108 })
    {
        const probe::Stereo out = held(note, 48000.0);
        const double c = centsOff(out, note, 48000.0, 0.5, 1.0);
        worst = std::max(worst, std::fabs(c));
        P.near("range.note" + std::to_string(note) + ".cents", c, 0.0, 0.5);
    }

    P.le("worst_abs_cents", worst, 0.01);

    {
        const probe::Stereo out = held(57, 48000.0);
        const double first = centsOff(out, 57, 48000.0, 0.5, 0.5), second = centsOff(out, 57, 48000.0, 1.0, 0.5);
        P.near("steady.a3.second_half_minus_first_cents", second - first, 0.0, 0.001);
    }
    return P.finish();
}
