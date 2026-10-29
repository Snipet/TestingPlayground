// Source/dsp/Measure.h: a program rendered offline and measured, in words and in numbers (JUCE-free). The chat's
// measure_output tool (ChatAgent.h) gives the words, because Claude cannot hear what it builds; its checks
// (set_checks, Goals.h) and tune_dials (Tune.h) read the numbers (metrics(): each a name, a unit and what it is);
// the probes check both (dsp.tune, plugin.chatexample).
//
// The files are compiled afresh (Patch::compile) and played in an Engine of their own, at `rate`, in blocks of `block`
// (256 unless asked), with the dials at the values asked for: by name from `dials`, else as they are now in the plugin
// (`current`, by name), else at the program's defaults. Nothing that plays in the plugin is touched. A Session compiles once and
// measures as often as asked, each render from the program as new (Engine::restart: the same as a fresh compile's).
//
//   an instrument  the notes (default one, C3) held for holdSeconds at `velocity`, released, then tailSeconds more:
//                  the held level (peak, RMS, DC, the stereo correlation and balance), the spectrum's strongest
//                  partials and its centroid over the held part (and how the lowest strong one compares with the
//                  lowest note's frequency), the largest step between two samples, the envelope (when it reaches its
//                  loudest, and when it falls 60 dB from there), when the sound falls 60 dB below its held level
//                  after the release and when the last voice ended (process returned false), the output guard's
//                  alerts, and the CPU time (the notes while held, and sixteen voices at once: a render of sixteen
//                  notes held, the least of two). With one note, the strongest component that is no harmonic of it
//                  (aliasing, or the sound's own: detuning, FM, noise), and a chord (the note, a third, a fifth and an
//                  octave above) is rendered too, for the summed level.
//   an effect      a sine of 220 Hz at -12 dBFS and noise at -18 dBFS RMS, each for holdSeconds, an impulse followed
//                  by tailSeconds of silence, and silence alone: the output's level and the gain for each, the sine's
//                  partials (what the effect adds), the noise's spectral centroid in and out, the impulse response's
//                  peak, delay and decay (60 dB), what comes out of silence, the alerts and the CPU time.
//
// Off the audio thread only (it compiles and allocates). Deterministic: the same files and request give the same
// report and numbers but for the CPU times, which are the rendering thread's own time (not the wall clock's) in the
// Engine as the audio thread runs it with no editor open (flush-to-zero, no meters), of the program's own code with
// its dials held still: every call flat (Engine.h). A dial that moves costs more; once the dials have held still a
// while the plugin plays a frozen copy instead, usually cheaper (Patch.h).
//
// A scenario (the chat's `scenario`, plugin/ChatScenario.h) plays it as a musician would instead: a phrase of notes in
// time (in place of the notes held together: the held part then ends at its last note-off), dials moved, the wheels
// and the pedal, the host's transport stopped or moved, at the host's rate and block size. The render plays host
// blocks of `block` samples as the processor does: the dials (a moving one where it is at the block's start, as a
// host's automation) and the transport once a block, each block in one Engine call or in the parts a RenderHooks asks
// for, the events at their samples. A phrase and the wheels give the same bits at any block size (the voices run on
// the absolute grid, Engine.h); a dial that moves does not, as in a DAW. The report's step line then names each of the
// largest steps' nearest event ("0.41 at 0.300 s, 0.4 ms after note-on 52").
#pragma once

#include "dsp/Controls.h"
#include "dsp/Events.h"
#include "dsp/Sources.h"

#include <cstdint>
#include <utility>

#include <complex>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace playground
{
    class Engine;
}

namespace playground::measure
{
    // ---- a scenario (above) ------------------------------------------------------------------------------------------
    inline constexpr std::size_t kMaxPhrase = 256, kMaxAutomate = 32, kMaxWheels = 64;
    inline constexpr int kDefaultBlock = 256, kMaxBlock = 8192;
    inline constexpr double kMaxPlaySeconds = 10.0;   // where a phrase's notes, the moves and the edits end at most

    struct PhraseNote
    {
        int note = 48;
        double at = 0.0, length = 0.5;            // seconds: its note-on, and how long until its note-off
        float velocity = 0.8f;                    // 0..1
    };

    // A dial moved: a float from where it is at `at` to `to` over `over` seconds (0: at once; the Engine glides it over
    // 20 ms as it glides a host's), its value taken once a host block; an int or a bool steps to `to` at `at`.
    struct DialMove
    {
        std::string dial;                         // the parameter's name
        double at = 0.0;
        float to = 0.0f;                          // plain value, in the parameter's units
        double over = 0.0;
    };

    enum class Wheel : uint8_t
    {
        mod,                                      // the mod wheel, 0..1
        bend,                                     // the pitch wheel, -1..1
        pressure,                                 // channel pressure, 0..1
        sustain,                                  // the pedal: down from 0.5
    };

    struct WheelMove
    {
        Wheel wheel = Wheel::mod;
        double at = 0.0;
        float value = 0.0f;
    };

    // The host's transport edited (it plays from the song's start at the request's bpm otherwise): it stops at stopAt
    // seconds (-1: never), and its position jumps to beat jumpToBeat at jumpAt seconds (-1: no jump; 0: it starts
    // there). The host tells a block's start, so an edit lands on the first block at or after it.
    struct HostEdits
    {
        double stopAt = -1.0;
        double jumpAt = -1.0;
        double jumpToBeat = 0.0;
    };

    // An effect's own input, at the request's rate, in place of the test signals (a recording's: the capture lane).
    struct Input
    {
        std::vector<float> l, r;
    };

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

        // A scenario (above): each part optional. `phrase` replaces `notes` and `holdSeconds` (an instrument's).
        std::vector<PhraseNote> phrase;
        std::vector<DialMove> automate;
        std::vector<WheelMove> wheels;            // an instrument's, after modwheel and pitchbend at the start
        HostEdits hostEdits;
        int block = kDefaultBlock;                // the host's block size, 1..kMaxBlock
        std::shared_ptr<const Input> input;       // an effect's input in place of the test signals (none: those)
        int signal = -1;                          // an effect's input for view_sound and compare, numbered as SoundView's
                                                  // ViewInput (-1: the tool's own); measure() plays its test signals
        // A recording replayed (the capture lane's: plugin/ChatScenario.h), merged with the above: the MIDI at absolute
        // samples, the transport the host sent at a block's start, the host's blocks as they came (their sizes in
        // order; after the last, `block` each).
        std::vector<std::pair<std::size_t, Event>> events;
        std::vector<std::pair<std::size_t, Transport>> transport;
        std::vector<int> blocks;
        // What the plugin fills in before it renders (plugin/ChatScenario.h): the host's rate and block ("host"), the
        // user's last recording ("capture"). Measure renders what the fields above say and ignores these.
        bool hostRate = false, hostBlock = false, fromCapture = false;

        // The same render (the notes, the levels, the times, the dials asked for, the host and the scenario), `want`
        // aside.
        bool sameRender(const Request& o) const;
        // Anything of a scenario beyond the notes held together: a phrase, moves, wheels, host edits, a recording.
        bool scenario() const;
    };

    // What a request plays, by absolute sample (Session::timeline): the plan a render follows, and what the click
    // locator and the trace name a moment by.
    struct Mark
    {
        std::size_t at = 0;
        std::string what;                         // "note-on 52", "note-off 52", "the pedal down", "cutoff moves to
                                                  // 2400", "the host stops"
        int note = -1;                            // a note-on's or a note-off's key
        bool on = false;                          // a note-on
    };

    struct Timeline
    {
        std::size_t length = 0;                   // samples rendered
        std::size_t heldUntil = 0;                // the last note-off (an effect: its input's end)
        std::size_t firstNote = 0;                // the first note-on
        std::vector<std::pair<std::size_t, Event>> events;          // the MIDI, by sample (Event::offset unused)
        // The transport the host sends with the first block at or after its sample; `here`: at the position the
        // Engine's clock has reached (a stop), else at the transport's own beat (the start, a jump).
        struct Send
        {
            std::size_t at = 0;
            Transport transport;
            bool here = false;
        };
        std::vector<Send> transport;
        // A dial's moves (DialMove), by slot: from `from` (plain) at sample a to `to` at sample b (a == b: a step).
        struct Move
        {
            int slot = 0;
            std::size_t a = 0, b = 0;
            float from = 0.0f, to = 0.0f;
        };
        std::vector<Move> moves;                  // by slot, then by sample
        std::vector<int> blocks;                  // the host's blocks (Request::blocks), then `block` each
        int block = kDefaultBlock;
        std::vector<Mark> marks;                  // every event, move and edit, in words, by sample
        std::vector<std::string> unknown;         // dials moved by a name the program has not
    };

    // A render's calls into the Engine, for a tool that watches the program as it plays (the trace lane's). render()
    // asks callLength before each call and calls afterCall after it; without hooks a host block is one call. Offline
    // only: `engine` is the Session's own, never the one that plays.
    struct CallInfo
    {
        std::size_t at = 0;                       // the call's first sample (absolute)
        int length = 0;
        std::size_t hostBlock = 0;                // its host block's first sample
        Engine& engine;                           // as the call left it
        std::span<const Event> events;            // what the call played (offsets within it)
        const float* left = nullptr;              // the call's output, `length` samples
        const float* right = nullptr;
        uint32_t alerts = 0;                      // the output guard's, in this call (Engine::takeAlerts)
        const Timeline& timeline;
    };

    class RenderHooks
    {
    public:
        virtual ~RenderHooks() = default;
        // The next call's length at absolute sample `at`, 1 or more (render() cuts it at its host block's end); 0: the
        // rest of the host block.
        virtual int callLength(std::size_t /*at*/) { return 0; }
        // After each call; false stops the render there (the rest stays silent).
        virtual bool afterCall(const CallInfo& /*call*/) { return true; }
    };

    // A request's scenario in a line ("a phrase of 12 notes (C3 to G4, up to 3 at once) over 2.40 s; cutoff moves
    // twice; the pedal 2 times; the host stops at 1.50 s"), "" with none.
    std::string scenarioText(const Request& request);

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

        // The output of one render: an instrument's notes held and released (or its scenario), or an effect's `input`
        // (rendered from the program as new; dials and host as `request` says; Request::input, when set, in place of
        // `input`). For view_sound (SoundView.h), compare (Compare.h) and a tool's hooks (RenderHooks).
        struct Render
        {
            std::vector<float> l, r;
            std::string dials;
            std::vector<std::pair<std::string, float>> values;   // the dials rendered with, plain, by name
            uint32_t alerts = 0;
            std::size_t heldUntil = 0;            // the held part's end (the timeline's)
        };
        Render render(const Request& request, const std::vector<float>* inputL, const std::vector<float>* inputR,
                      std::size_t length, RenderHooks* hooks = nullptr);
        // What `request` plays over `length` samples (0: lengthOf), by absolute sample.
        Timeline timeline(const Request& request, std::size_t length = 0) const;
        // The samples a render of `request` takes: an instrument's held part (its phrase's, else holdSeconds) and the
        // tail; an effect's holdSeconds and the tail, or its own input's length.
        std::size_t lengthOf(const Request& request) const;

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

    // The click locator: the `count` largest steps between two neighbouring samples (of either channel), each the
    // largest within 10 ms of it, largest first: the largest, then those that stand out from the sound's own steps (more
    // than 1.5 times the median 10 ms window's largest); and a step's nearest moment of a timeline in words ("0.4 ms
    // after note-on 52 (the same key again 0.12 s after its release)"; "no event within 50 ms" when none is near).
    struct Step
    {
        double size = 0.0;
        std::size_t at = 0;                       // the sample after the step
    };
    std::vector<Step> largestSteps(const std::vector<float>& l, const std::vector<float>& r, std::size_t count, double rate);
    std::string nearestMark(const Timeline& timeline, std::size_t at, double rate);
}
