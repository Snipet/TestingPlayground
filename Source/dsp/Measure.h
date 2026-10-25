// Source/dsp/Measure.h: a program rendered offline and measured, in words (JUCE-free). The chat's measure_output tool
// (ChatAgent.h) runs it, because Claude cannot hear what it builds; the probes check its numbers (dsp.measure).
//
// The files are compiled afresh (Patch::compile) and played in an Engine of their own, at `rate`, in blocks of 256,
// with the dials at the values asked for: by name from `dials`, else as they are now in the plugin (`current`, by
// name), else at the program's defaults. Nothing that plays in the plugin is touched.
//
//   an instrument  the notes (default one, C3) held for holdSeconds at `velocity`, released, then tailSeconds more:
//                  the held level (peak, RMS, DC, the stereo correlation and balance), the spectrum's strongest
//                  partials and its centroid over the held part (and how the lowest strong one compares with the
//                  lowest note's frequency), the largest step between two samples, when the sound falls 60 dB below
//                  its held level after the release and when the last voice ended (process returned false), the
//                  output guard's alerts, and the CPU time. With one note, the strongest component that is no
//                  harmonic of it (aliasing, or the sound's own: detuning, FM, noise), and a chord (the note, a
//                  third, a fifth and an octave above) is rendered too, for the summed level.
//   an effect      a sine of 220 Hz at -12 dBFS and noise at -18 dBFS RMS, each for holdSeconds, an impulse followed
//                  by tailSeconds of silence, and silence alone: the output's level and the gain for each, the sine's
//                  partials (what the effect adds), the noise's spectral centroid in and out, the impulse response's
//                  peak, delay and decay (60 dB), what comes out of silence, the alerts and the CPU time.
//
// Off the audio thread only (it compiles and allocates). Deterministic: the same files and request give the same
// report but for the CPU times.
#pragma once

#include "dsp/Sources.h"

#include <map>
#include <string>
#include <vector>

namespace testingplayground::measure
{
    struct Request
    {
        std::vector<int> notes;                   // an instrument's notes, played together; empty: one, 48 (C3)
        float velocity = 0.8f;                    // 0..1
        float holdSeconds = 1.0f;                 // 0.05..10
        float tailSeconds = 1.5f;                 // 0..10
        std::map<std::string, float> dials;       // plain values, by parameter name: what to render with
        std::map<std::string, float> current;     // the plugin's dials as they are now, by name
        double rate = 48000.0;
    };

    // The report, a few lines of plain text; when the program does not compile, its messages.
    std::string run(const Sources& files, const Request& request);

    // ---- the analysis, for the probes ------------------------------------------------------------------------------
    struct Peak
    {
        double hz = 0.0;
        double db = 0.0;                          // relative to the strongest
    };
    // The strongest local maxima of `x`'s spectrum (a Hann window over the largest power of two that fits, up to
    // 16384 samples), at most `count`, by frequency; and the spectrum's centroid in Hz.
    std::vector<Peak> spectrumPeaks(const std::vector<float>& x, double rate, int count, double* centroidHz = nullptr);
    // The strongest component of `x`'s spectrum between 20 Hz and maxHz that is no harmonic of f0 (farther than six
    // bins from every multiple of it): an alias, or the sound's own. A four-term Blackman-Harris window (sidelobes
    // under -92 dB) over the largest power of two that fits, up to 32768 samples; db relative to the strongest
    // component, -200 when nothing is found.
    Peak inharmonic(const std::vector<float>& x, double rate, double f0, double maxHz);
}
