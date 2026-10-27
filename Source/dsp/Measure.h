// Source/dsp/Measure.h: a program rendered offline and measured, in words and in numbers (JUCE-free). The chat's
// measure_output tool (ChatAgent.h) gives the words, because Claude cannot hear what it builds; its checks
// (set_checks, Goals.h) and tune_dials (Tune.h) read the numbers (metrics(): each a name, a unit and what it is);
// the probes check both (dsp.tune, plugin.chatexample).
//
// The files are compiled afresh (Patch::compile) and played in an Engine of their own, at `rate`, in blocks of 256,
// with the dials at the values asked for: by name from `dials`, else as they are now in the plugin (`current`, by
// name), else at the program's defaults. Nothing that plays in the plugin is touched. A Session compiles once and
// measures as often as asked, each render from the program as new (Engine::restart: the same as a fresh compile's).
//
//   an instrument  the notes (default one, C3) held for holdSeconds at `velocity`, released, then tailSeconds more:
//                  the held level (peak, RMS, DC, the stereo correlation and balance), the spectrum's strongest
//                  partials and its centroid over the held part (and how the lowest strong one compares with the
//                  lowest note's frequency), the largest step between two samples, the envelope (when it reaches its
//                  loudest, and when it falls 60 dB from there), when the sound falls 60 dB below its held level
//                  after the release and when the last voice ended (process returned false), the output guard's
//                  alerts, and the CPU time. With one note, the strongest component that is no harmonic of it
//                  (aliasing, or the sound's own: detuning, FM, noise), and a chord (the note, a third, a fifth and an
//                  octave above) is rendered too, for the summed level.
//   an effect      a sine of 220 Hz at -12 dBFS and noise at -18 dBFS RMS, each for holdSeconds, an impulse followed
//                  by tailSeconds of silence, and silence alone: the output's level and the gain for each, the sine's
//                  partials (what the effect adds), the noise's spectral centroid in and out, the impulse response's
//                  peak, delay and decay (60 dB), what comes out of silence, the alerts and the CPU time.
//
// Off the audio thread only (it compiles and allocates). Deterministic: the same files and request give the same
// report and numbers but for the CPU times.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <complex>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace testingplaygroundsynth::measure
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
        // The program's Host block (host.fm): the host plays at `bpm` from the song's start; an instrument's mod wheel
        // (0..1) and pitch bend (-1..1) from the first sample.
        float bpm = 120.0f;
        float modwheel = 0.0f;
        float pitchbend = 0.0f;
        // The metrics wanted (their names): only the renders and the analysis they need run, and the report is then
        // partial. Empty: everything (the report whole).
        std::set<std::string> want;

        // The same render (the notes, the levels, the times, the dials asked for and the host), `want` aside.
        bool sameRender(const Request& o) const;
    };

    // A number a measurement gives (Metrics, by name). A value that never came (a release that did not fall 60 dB
    // within the tail, a voice that never ended) is infinity; a silent level is -240 dB.
    struct MetricInfo
    {
        const char* name;                         // "release_s"
        const char* unit;                         // "dB", "dBFS", "Hz", "s", "ms", "cents", "%", "" (a ratio or a flag)
        const char* what;                         // a line of what it measures, for Claude's instructions
        double tolerance;                         // how near counts as met: in the unit, or a share of the target when
        bool relative;                            // relative (5 % of 2000 Hz)
        double floor;                             // a relative tolerance's least, in the unit (near a target of 0)
    };
    using Metrics = std::map<std::string, double>;

    // This plugin's kind's metrics, in the order the instructions list them.
    std::span<const MetricInfo> metrics();
    const MetricInfo* metricInfo(std::string_view name);    // null: no metric of this kind has that name
    // A value as text in its unit ("2.31 s", "-12.4 dBFS", "never", "1180 Hz").
    std::string valueText(std::string_view metric, double value);

    struct Result
    {
        bool compiled = false;
        std::string report;                       // the words (what run() returns)
        Metrics values;                           // the numbers measured (those `want` asked for, or all)
        std::string dials;                        // the dials rendered with, "cutoff 1200, resonance 0.3"
        std::vector<std::string> unknown;         // dials asked for by a name the program has not
    };

    // One compile, measured as often as asked.
    class Session
    {
    public:
        explicit Session(const Sources& files, double rate = 48000.0);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        bool compiled() const noexcept;
        const std::string& errors() const noexcept { return errors_; }   // the compiler's messages when it did not
        const Controls& controls() const noexcept;
        double rate() const noexcept { return rate_; }

        Result measure(const Request& request);

        // The output of one render: an instrument's notes held and released, or an effect's `input` (rendered from the
        // program as new; dials and host as `request` says). For view_sound (SoundView.h).
        struct Render
        {
            std::vector<float> l, r;
            std::string dials;
            uint32_t alerts = 0;
        };
        Render render(const Request& request, const std::vector<float>* inputL, const std::vector<float>* inputR,
                      std::size_t length);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::string errors_;
        double rate_;
    };

    // The report, a few lines of plain text; when the program does not compile, its messages.
    std::string run(const Sources& files, const Request& request);

    // ---- the analysis, for the probes and SoundView.h -------------------------------------------------------------
    // An FFT in place: a.size() a power of two, forward (e^-i).
    void fft(std::vector<std::complex<double>>& a);
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
