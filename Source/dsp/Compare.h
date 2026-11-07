// Source/dsp/Compare.h: two programs rendered alike and compared, sample by sample (JUCE-free): the chat's compare
// (plugin/ChatCompare.cpp), an A/B and a null test. Each side is a program's files and the render it is played in (one
// request, its dials the side's own), or a recording (what played, as the capture kept it); each renders at its request's
// rate (one rate for both, unless B is asked at another), an effect with one input (Request::signal, its own
// Request::input, else noise; at two rates the sweep), each in a Session of its own, and as long as the recording (in
// seconds) when a side is one.
//
// At two rates (a rate check: does the program sound the same at 96 kHz?) no sample lines up with another, so the
// sides are compared by what is the same at every rate: the levels, the octave bands, the metrics (those the rate moves
// by their definition set aside: step, which a smooth wave makes smaller at a higher rate, and an effect's
// noise_gain_db, noise_lufs and impulse_peak_db, whose white noise and one-sample impulse spread over the rate's whole
// band; inharmonic_db under its -90 dB floor on both sides agrees) and, with `detail`, B's level against A's in 32
// parts; no null test, gain or lag. An effect's input is then the
// sweep (20 Hz to 20 kHz, the same at every rate) unless the request names one; the chat's compare refuses noise and
// the impulse there. Both rates are kMinRateAcross or more (else an error, nothing rendered): 20 Hz to 20 kHz exists
// on both sides, so the sweep, the metrics' bands (an effect's centroids) and the picture's axis are the same.
//
// What it finds, in a few lines: identical bits (over the shorter side when their lengths differ); or how far apart
// they are (the RMS of A - B against A's, in dB), the peak difference and when, the first and the last sample that
// differ by more than -120 dBFS; the gain and the lag that fit B to A best (b(t) ~ gain a(t - lag), from their
// cross-correlation through fft(), Measure.h, over the first 2^17 samples, lags within 0.5 s, each lag's normalised by
// the energies that overlap there) and what is left after them; B against A in ten octave bands (31.5 Hz to 16 kHz:
// Measure.h's bandLevels, both channels in kBandSeconds Hann frames, the same at every rate); and the metrics
// (Measure.h) that differ by more than their tolerance (MetricInfo), side by side (the CPU's aside, which no two renders
// share). `detail` adds the difference's level in 32 parts of the render.
//
// Off the audio thread only (it compiles and renders). Deterministic: the same sides give the same result.
#pragma once

#include "dsp/Measure.h"
#include "dsp/SoundView.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace playground::measure
{
    // Two rates' least: each side's Nyquist frequency above 20 kHz.
    inline constexpr double kMinRateAcross = 40000.0;

    struct CompareSide
    {
        Sources files;                            // what is rendered (unless `recorded`)
        Request request;                          // the render: one scenario for both, the dials the side's own
        const std::vector<float>* recordedL = nullptr;   // a recording in place of a render, at the request's rate
        const std::vector<float>* recordedR = nullptr;
    };

    struct Comparison
    {
        std::string error;                        // why nothing was compared ("" when it was): a side did not compile,
                                                  // or two rates and one under kMinRateAcross
        bool identical = false;
        double rate = 48000.0;                    // A's
        double rateB = 48000.0;                   // B's: A's, unless it was asked at another (then twoRates())
        std::size_t length = 0;                   // samples compared (the shorter side's; at two rates, A's of them)
        std::size_t lengthA = 0, lengthB = 0;
        double diffDb = -240.0;                   // RMS of A - B against A's (dB; against full scale when A is silent)
        double peakDiff = 0.0;
        std::size_t peakAt = 0;
        long firstDiff = -1, lastDiff = -1;       // the first and the last sample differing above -120 dBFS
        int lag = 0;                              // B is A delayed by `lag` samples (negative: ahead)
        double gain = 1.0;                        // and scaled by it
        double residualDb = -240.0;               // what is left of B after gain and lag, against B (dB)
        static constexpr std::array<double, 10> kBands = kOctaveBands;   // Measure.h's (Bands::octave), by name
        std::array<double, 10> bandsDb{};         // B against A in each octave band, dB (NaN: silent in both, or above
                                                  // the Nyquist frequency)
        struct Differs
        {
            std::string metric;
            double a = 0.0, b = 0.0;
        };
        std::vector<Differs> metrics;             // those beyond their tolerance (both sides rendered)
        bool metricsMeasured = false;
        std::vector<float> envelopeDb;            // detail: the difference's level in 32 parts, against A's (dB); at two
                                                  // rates B's level against A's in each part (dB)
        Sound a, b;                               // what was rendered (a recording's samples for its side), for a picture
        std::vector<std::string> lines;           // the result in words: at most seven lines

        bool twoRates() const { return rateB != rate; }
    };

    struct CompareOptions
    {
        bool detail = false;                      // the 32-part envelope
        bool metrics = true;                      // measure both and compare the metrics
        Cancel cancel;                            // both sides' renders cancelled as it says (Measure.h's Session::setCancel,
                                                  // its time limit over both): then an error that says how far it got
    };

    Comparison compare(const CompareSide& a, const CompareSide& b, const CompareOptions& options = {});
}
