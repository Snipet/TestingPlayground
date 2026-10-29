// Source/dsp/Compare.h: two programs rendered alike and compared, sample by sample (JUCE-free): the chat's compare
// (plugin/ChatCompare.cpp), an A/B and a null test. Each side is a program's files and the render it is played in (one
// request, its dials the side's own), or a recording (what played, the capture lane's); both render at one rate, an
// effect with one input (Request::signal, its own Request::input, else noise), each in a Session of its own, and as
// long as the recording when a side is one.
//
// What it finds, in a few lines: identical bits (over the shorter side when their lengths differ); or how far apart
// they are (the RMS of A - B against A's, in dB), the peak difference and when, the first and the last sample that
// differ by more than -120 dBFS; the gain and the lag that fit B to A best (b(t) ~ gain a(t - lag), from their
// cross-correlation through fft(), Measure.h, over the first 2^17 samples, lags within 0.5 s, each lag's normalised by
// the energies that overlap there) and what is left after them; B against A in ten octave bands (31.5 Hz to 16 kHz,
// Welch power spectra); and the metrics (Measure.h) that differ by more than their tolerance (MetricInfo), side by
// side (the CPU's aside, which no two renders share). `detail` adds the difference's level in 32 parts of the render.
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
    struct CompareSide
    {
        Sources files;                            // what is rendered (unless `recorded`)
        Request request;                          // the render: one scenario for both, the dials the side's own
        const std::vector<float>* recordedL = nullptr;   // a recording in place of a render, at the request's rate
        const std::vector<float>* recordedR = nullptr;
    };

    struct Comparison
    {
        std::string error;                        // why nothing was compared ("" when it was): a side did not compile
        bool identical = false;
        double rate = 48000.0;
        std::size_t length = 0;                   // samples compared (the shorter side's)
        std::size_t lengthA = 0, lengthB = 0;
        double diffDb = -240.0;                   // RMS of A - B against A's (dB; against full scale when A is silent)
        double peakDiff = 0.0;
        std::size_t peakAt = 0;
        long firstDiff = -1, lastDiff = -1;       // the first and the last sample differing above -120 dBFS
        int lag = 0;                              // B is A delayed by `lag` samples (negative: ahead)
        double gain = 1.0;                        // and scaled by it
        double residualDb = -240.0;               // what is left of B after gain and lag, against B (dB)
        static constexpr std::array<double, 10> kBands{ 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
        std::array<double, 10> bandsDb{};         // B against A in each octave band, dB (NaN: silent in both, or above
                                                  // the Nyquist frequency)
        struct Differs
        {
            std::string metric;
            double a = 0.0, b = 0.0;
        };
        std::vector<Differs> metrics;             // those beyond their tolerance (both sides rendered)
        bool metricsMeasured = false;
        std::vector<float> envelopeDb;            // detail: the difference's level in 32 parts, against A's (dB)
        Sound a, b;                               // what was rendered (a recording's samples for its side), for a picture
        std::vector<std::string> lines;           // the result in words: at most seven lines
    };

    struct CompareOptions
    {
        bool detail = false;                      // the 32-part envelope
        bool metrics = true;                      // measure both and compare the metrics
    };

    Comparison compare(const CompareSide& a, const CompareSide& b, const CompareOptions& options = {});
}
