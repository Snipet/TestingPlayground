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
//                  the held level (peak, RMS, DC, the stereo correlation and balance), its loudness (BS.1770's
//                  integrated, and the RMS A-weighted), the spectrum's strongest partials and its centroid, what is off
//                  the orders of a fixed base (base_hz, else the expected pitch: the share of the power, the strongest
//                  lines off them, the half- and third-orders), how the spectrum's shape moves from frame to frame
//                  (wobble), the pitch (the strongest partial near the expected note, stretch by stretch: stretches(),
//                  each against the lowest key held there, bent by the pitch wheel then; the report names
//                  the sound unpitched instead when the lowest note and a whole tone up, each alone for 0.6 s with none
//                  of the scenario's moves, do not measure a whole tone apart, and then gives no Inharmonic words), the
//                  largest step between two samples and the clicks (clicks(), trace's rule), the
//                  envelope (when it reaches its loudest, and when it falls 60 dB from there), when the sound falls
//                  60 dB below its held level after the release and when the last voice ended (process returned
//                  false), the output guard's alerts, and the CPU time (the notes while held, and sixteen voices at
//                  once: a render of sixteen notes held, the least of two; in words only when more than one voice
//                  sounds there). With one note, the strongest component (a peak) that is no harmonic of it
//                  (aliasing, or the sound's own: detuning, FM, noise), and a chord (the note, a third, a fifth and an
//                  octave above) is rendered too, for the summed level.
//   an effect      a sine of 220 Hz at -12 dBFS and noise at -18 dBFS RMS, each for holdSeconds, an impulse followed
//                  by tailSeconds of silence, and silence alone: the output's level and the gain for each, the sine's
//                  partials (what the effect adds), its THD+N and what is off its orders (220 Hz's multiples), the
//                  noise's spectral centroid in and out and its loudness out and in, the impulse response's peak, delay
//                  and decay (60 dB), what comes out of silence, the alerts and the CPU time. For wobble_db alone (its
//                  steps, alerts and CPU time are not counted), a steady tone (steadyTone: partials every 10 Hz, as flat
//                  as the noise, repeating every 0.1 s) at -18 dBFS RMS for holdSeconds: how its output's shape moves.
//
// Every metric is measured over a span the metric's words (MetricInfo::what) name, and the same at every sample rate.
// The held part (heldSpans) is an instrument's notes from kSettleSeconds after each note-on (half the note when it is
// shorter) to its note-off, a phrase's notes joined where they overlap; an effect's test signal from kSettleSeconds
// after it starts to its end. Its levels are over all of it, and its spectra Welch's average over all of it (Spectrum:
// frames fixed in seconds, so a frame holds the same stretch of sound at 48 and at 192 kHz; a span shorter than a frame
// is one frame of it, so a note held under kSettleSeconds + kPitchSeconds reads its pitch in a shorter frame, and one
// whose frame cannot part what a metric reads leaves it unmeasured, saying how long a note it takes: the pitch, and
// inharmonic_db).
//
// A request's window (Window: the render settings' `window`, from and to in seconds) replaces the held part, for a
// part of the sound that no note holds (a key that is a control, a sound that goes on after its note-off, the stretch
// after a dial moves); at least kMinWindowSeconds long (the tools refuse a shorter one). An instrument's is in seconds
// of its render, which then lasts at least until the window's end: every metric "while held" and peak_db measure it
// (the CPU's too: the calls that render it), and the others keep their own spans as they are without it (attack_s from
// the first note-on over the notes, release_s from the last note-off under the notes' held RMS, the whole render's
// over the length it has without the window (Timeline::plain), the chord's and the sixteen voices' own renders). An
// effect's is in seconds of the sine's, the noise's and the steady tone's renders (each from the program as new, the
// signal from 0), cut at their end (hold_seconds): their metrics measure it, sine_peak_db too; the impulse's,
// silence's and the whole renders' keep theirs. A window that holds too little of what a metric needs (an effect's
// that starts after hold_seconds, or less than kMinWindowSeconds before it) leaves it unmeasured, saying why
// (Result::unmeasured), as a held part (or a window) leaves wobble_db with fewer than three of its frames, and
// off_order_db with no frame or a base under twice an order's zone.
// view_sound's average spectrum and RMS are over it (SoundView.h).
//
// How long a render may be (Request::longest): its times (holdSeconds, a phrase's notes, the moves, the wheels, the
// host's edits) end by kMaxPlaySeconds when it makes a picture (view_sound, view_panel's moment, compare's picture) and
// by kMaxLongSeconds when the call returns numbers alone (measure_output, a check, tune_dials, compare without its
// picture, trace), the tail at most kMaxTailSeconds after them (a window ends by both: Request::endSeconds); and a
// render holds kMaxRenderSamples a channel at most (310 s up to 96 kHz, 155 s at 192 kHz: about 240 MB of stereo
// floats a buffer), so at a rate above 96 kHz its times end by what that holds less a whole tail (longestAt: 145 s at
// 192 kHz) and the render, its tail and its window by kMaxRenderSamples. The plugin's tools refuse what is past either,
// in words (plugin/ChatScenario.h's fits), so a request they take is never re-timed; here a request is clamped to the
// same (a Session's times to longestAt), so that every span a render's analysis reads lies inside it.
//
// A render can be cancelled as it goes (Session::setCancel: the chat's stop, an MCP cancel, a time limit): it asks
// every kCancelSamples samples and stops there. measure() then measures nothing and says how far it got ("Cancelled
// 84.21 s into the render of the notes held (301.50 s long), 12.3 s after it began, about 6.8 s of sound a second.
// Nothing was measured."), as render() and what renders through it (SoundView.h, Compare.h) do; a cancelled render is
// never measured. The time limit is the call's (Cancel::start, shared by the Sessions a call renders in), and its words
// name it whole, and the render that spent it when the limit lands in a short one after it. The render
// runs on its caller's thread (the chat's worker, the MCP link's one worker): the call's answer is its numbers, and
// what a call reads and writes is the chat's one at a time (ChatAgent's busy_), so nothing else could take the worker
// meanwhile; a cancel reaches it in milliseconds, and the live tools answer on the link's poll thread while it runs.
//
// A render whose program stops (a call past its loop budget, or one that fails a bounds check: Patch.h, "The loop
// budget", "Bounds checks") plays as the plugin's Engine plays it, faded out from there; the report then begins with
// the stop's message and when it came (Session::stopWords, the first render's), and a render says so (Render::stopCall,
// stopAt), as view_sound and trace do.
//
// The report ends with the render hash (Result::hash: "Render hash: 9f86d081884c7d65 (SHA-256 of the float32 samples, L R
// interleaved, of the render of the note held).", Export.h; two reports with the same hash rendered the same bits) and the
// values measured, name=value in the metrics' order ("Metrics: peak_db=-9.5 rms_db=-17.2 ..."), in the names checks use. A request's `spectrum` adds band levels as data (bandsOf: an octave's ten, a third's
// thirty-one or a semitone's 120, the averaged spectrum's points), of both channels' power over the held part (or the
// window) in Hann frames (bandSpectrum: kBandSeconds, a semitone's kNoteBandSeconds, each at most the part's length):
// an instrument's levels (dBFS), an effect's noise's (and its own input's) gain in each, out against in. The line names
// its frames and the band below which a tone spreads over the bands beside it (sharpFrom).
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
// largest steps' nearest event ("0.41 at 0.300 s, 0.4 ms after note-on 52"). A render lasts until the scenario's last
// move or edit and the tail after it; an effect's test signal that ends before it is followed by silence (the report
// says so).
#pragma once

#include "dsp/Controls.h"
#include "dsp/Events.h"
#include "dsp/Profile.h"
#include "dsp/Sources.h"
#include "fminus/Patch.h"

#include <cstdint>
#include <utility>

#include <array>
#include <chrono>
#include <complex>
#include <functional>
#include <limits>
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
    class Patch;
}

namespace playground::measure
{
    // ---- a scenario (above) ------------------------------------------------------------------------------------------
    inline constexpr std::size_t kMaxPhrase = 256, kMaxAutomate = 32, kMaxWheels = 64;
    inline constexpr int kDefaultBlock = 256, kMaxBlock = 8192;
    // How long a render may be (above): a picture's times end by kMaxPlaySeconds, the numbers' by kMaxLongSeconds
    // (Request::longest), the tail kMaxTailSeconds after them at most; and kMaxRenderSamples a channel at any rate.
    inline constexpr double kMaxPlaySeconds = 10.0;   // where a phrase's notes, the moves and the edits end at most
    inline constexpr double kMaxLongSeconds = 300.0;  // and in a render that returns numbers alone
    inline constexpr double kMaxTailSeconds = 10.0;
    inline constexpr double kMaxRenderSamples = (kMaxLongSeconds + kMaxTailSeconds) * 96000.0;

    // A render cancelled as it goes (Session::setCancel, above): asked every kCancelSamples samples. kRenderSeconds: the
    // time limit the plugin's tools give their renders, wall-clock, under an MCP call's deadline (mcp/Main.cpp: 200 s),
    // as tune_dials' (Tune.h's kMaxSeconds).
    inline constexpr std::size_t kCancelSamples = 8192;
    inline constexpr double kRenderSeconds = 170.0;
    struct Cancel
    {
        std::function<bool()> asked;              // true: stop (the chat's stop, an MCP cancel)
        double seconds = 0.0;                     // the call's time limit, wall-clock from `start` (0: none)
        std::function<void(double done, double length)> progress;   // a render's seconds so far, of its length
        // When the call's time began (unset, the clock's epoch: at Session::setCancel). A call that renders in several
        // Sessions (compare's sides, the checks' rates) gives each the same (started()), so the limit is the call's.
        std::chrono::steady_clock::time_point start{};
        std::string who;                          // whose renders, in the words ("A": "A's render"); empty: no one's
        // What the renders spent, over the Sessions that share it: the words name the render that used the time when
        // the limit lands in a short one after it.
        struct Spent
        {
            double sound = 0.0;                   // seconds of sound rendered
            int renders = 0;                      // renders that ran to their end
            std::string longest;                  // the one that took the most of the clock: what it rendered, its
            double longestLength = 0.0, longestSeconds = 0.0;   // seconds of sound and of the clock
        };
        std::shared_ptr<Spent> spent;
        // A copy with `start` (now) and `spent` set where they are not: what a call gives each Session it renders in.
        Cancel started() const;
    };

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
        steering,                                 // a racing wheel's (wheelcc, as host.fm reads them): -1..1, 14 bits
        throttle,                                 // its pedals, 0..1, 14 bits
        brake,
        clutch,
        gear,                                     // -1 reverse, 0 neutral, 1..7 (rounded)
        buttons,                                  // its buttons (host->buttons): the mask, a bit a button, 0..kMaxButtons,
                                                  // sent as the wheel sends it (wheelcc 21 to 24: a part's control change
                                                  // when its 7 bits change; the start key's note while bit 17 is down)
    };
    inline constexpr int kWheelKinds = static_cast<int>(Wheel::buttons) + 1;
    inline constexpr int kMaxButtons = (1 << 24) - 1;     // bits 0 to 23, each mask a float holds exactly

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

    // An effect's own input, at the request's rate, in place of the test signals (a recording's: the capture's).
    struct Input
    {
        std::vector<float> l, r;
    };

    // Band levels as data in the report (Request::spectrum; bandsOf, below): an octave's, a third's, a semitone's.
    enum class Bands : uint8_t { none, octave, third, semitone };

    // The seconds a request's metrics measure, from the render's start (above): none while `to` is not after `from`;
    // they end by the request's endSeconds() (kMaxWindowSeconds for a picture's).
    inline constexpr double kMaxWindowSeconds = kMaxPlaySeconds + kMaxTailSeconds;   // the moves' end and the longest tail
    inline constexpr double kMinWindowSeconds = 0.01;                    // the least a window holds (16 samples or more
                                                                         // at any rate: a spectrum's least frame)
    struct Window
    {
        double from = 0.0, to = 0.0;
        bool set() const { return to > from; }
        bool operator==(const Window&) const = default;
    };
    // "1.50 to 2.50 s".
    std::string windowText(const Window& window);

    struct Request
    {
        std::vector<int> notes;                   // an instrument's notes, played together; empty: one, 48 (C3)
        float velocity = 0.8f;                    // 0..1
        float holdSeconds = 1.0f;                 // 0.05..longest
        float tailSeconds = 1.5f;                 // 0..kMaxTailSeconds
        // Where its times end at most (above): kMaxPlaySeconds for a picture's render, kMaxLongSeconds for a call that
        // returns numbers alone (the plugin's tools set it before they read the rest: plugin/ChatScenario.h). It only
        // bounds the times (a request within both renders alike), so sameRender leaves it out.
        double longest = kMaxPlaySeconds;
        double endSeconds() const { return longest + kMaxTailSeconds; }   // a render's (and a window's) end at most
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
        Window window;                            // the metrics' seconds in place of the held part (above)
        // off_order_db's base in Hz (analysis only, as `want`): an instrument's orders are its multiples (0: the expected
        // pitch); an effect's are the sine's, 220 Hz, always.
        double baseHz = 0.0;
        // The report's band levels (analysis only, as `want`): an instrument's while held (bandLevels of bandSpectrum),
        // an effect's noise's (and its own input's) out against in, a band's gain; none: no such line.
        Bands spectrum = Bands::none;
        // The report's profile (dsp/Profile.h; analysis only, as `want`, its bits the same): where the CPU time of the
        // calls cpu_percent times goes, by the program's function and module, sampled; false: no sampler at all.
        bool profile = false;
        // A recording replayed (the capture's: plugin/ChatScenario.h), merged with the above: the MIDI at absolute
        // samples, the transport the host sent at a block's start, the host's blocks as they came (their sizes in
        // order; after the last, `block` each).
        std::vector<std::pair<std::size_t, Event>> events;
        std::vector<std::pair<std::size_t, Transport>> transport;
        std::vector<int> blocks;
        // A recording's dials, clock and parts (the capture's), for a replay that gives what played bit for bit:
        // `frames`, the 32 normalised values the processor sent with the host block at each sample (where they
        // changed), of a program whose dials were `frameControls`: as they are for a program with the same dials, else
        // by name through their plain values; they set every dial but those `dials` names (and `automate` moves).
        // `clock`: the Engine's samples at the start (the phase of the voices' grid, Engine::kGrid). `part`: the
        // largest Engine call, a host block played in parts of it as the processor plays one (0: in one call).
        // `recording`: the recording in words, for the scenario's line.
        std::vector<std::pair<std::size_t, std::array<float, kMaxControls>>> frames;
        std::shared_ptr<const Controls> frameControls;
        uint64_t clock = 0;
        int part = 0;
        std::string recording;
        // What the plugin fills in before it renders (plugin/ChatScenario.h): the host's rate and block ("host"), the
        // user's last recording ("capture"; captureFrom and captureTo, in seconds of it, the part replayed, -1: to its
        // end). Measure renders what the fields above say and ignores these.
        bool hostRate = false, hostBlock = false, fromCapture = false;
        double captureFrom = 0.0, captureTo = -1.0;

        // The same render (the notes, the levels, the times, the dials asked for, the host and the scenario) measured
        // over the same window with the same base, `want` and `spectrum` aside.
        bool sameRender(const Request& o) const;
        // Anything of a scenario beyond the notes held together: a phrase, moves, wheels, host edits, a recording.
        bool scenario() const;
        // The wheels press the start key (the buttons' bit wheelcc::kStartBit): the wheel's note plays beside the notes,
        // so a report or a view takes what plays as a phrase.
        bool startKey() const;
    };

    // The seconds the longest render of a request lasts, as Session's plans make it, without a compile (a move of a dial
    // the program lacks counts too): an instrument's notes held (or its phrase), its moves, wheels and host edits, then
    // the tail; an effect's test signals (or its own input), its moves and edits, then the tail; at least until its
    // window's end. The plugin holds it to kMaxRenderSamples at the request's rate (plugin/ChatScenario.h's fits).
    double renderSeconds(const Request& request);
    // The latest of a request's times as its plan reads them, its longest at most: holdSeconds (an instrument's while it
    // plays no phrase and no recording; an effect's always, its test signals' length), a phrase's note-offs, the moves'
    // ends, the wheels, the host's edits. A Session cuts them to longestAt (above); the plugin refuses a request past it.
    double latestTime(const Request& request);
    // Where a request's times end at most at `rate` (above): `longest`, or what kMaxRenderSamples holds there less a
    // whole tail (145 s at 192 kHz; 300 s and more up to 96 kHz).
    double longestAt(double longest, double rate);

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
        std::size_t plain = 0;                    // the samples it renders without the request's window, when the window
                                                  // makes it longer (0: length); a call ends there (render())
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
        // A recording's (Request::frames, clock, part): the normalised dials from each sample on, for this program's
        // slots; the Engine's clock at the start; the largest call.
        std::vector<std::pair<std::size_t, std::array<float, kMaxControls>>> frames;
        uint64_t clock = 0;
        int part = 0;
    };

    // What `request` plays for a program with these dials, as Session::timeline gives it, without a compile (the
    // capture's report names its steps' nearest events so).
    Timeline timeline(const Request& request, const Controls& controls, double rate, std::size_t length = 0);

    // A render's calls into the Engine, for a tool that watches the program as it plays (trace's). render()
    // calls started once the Engine is restarted, before its first call, then asks callLength before each call and
    // calls afterCall after it; without hooks a host block is one call. Offline only: `engine` is the Session's own,
    // never the one that plays.
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
        // Before the first call, the Engine restarted (a meter reader added here measures from the first sample).
        virtual void started(Engine& /*engine*/) {}
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
        const char* unit;                         // "dB", "dBFS", "LUFS", "Hz", "s", "ms", "cents", "%", "" (a ratio or a flag)
        const char* what;                         // exactly what it measures (its span, window, weighting and band, in
                                                  // the words metricTerms() defines), for Claude's instructions
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
    // The tolerance a target on it takes when a goal gives none (Goals.h's toleranceOf), as the instructions write it
    // after its unit: "+-1" (in the unit; a level's in dB), "+-5 %, at least 1" (a share of the target, never under 1).
    std::string toleranceText(const MetricInfo& metric);
    // The words the metrics' lines use, for the instructions before them: the held part, the spectrum, the band.
    const char* metricTerms();

    struct Result
    {
        bool compiled = false;
        std::string report;                       // the words (what run() returns)
        Metrics values;                           // the numbers measured (those `want` asked for, or all)
        std::map<std::string, std::string> unmeasured;   // a metric the part (or the window) holds too little for, and
                                                         // why ("the window starts after the sine and the noise end, at
                                                         // 1.00 s"): no value in `values`
        std::string dials;                        // the dials rendered with, "cutoff 1200, resonance 0.3"
        std::vector<std::string> unknown;         // dials asked for by a name the program has not
        Patch::Method stopped = Patch::Method::none;  // a render stopped (Patch.h, "The loop budget", "Bounds
                                                      // checks"): the report begins so,
        std::string stopWords;                        // with these words (Session::stopWords)
        bool cancelled = false;                   // a render was cancelled (Session::setCancel): the report says how far
                                                  // it got, and nothing is measured (`values` empty)
        // The render hash of a whole report (`want` empty; a partial one, a check's or tune_dials', has none: Export.h:
        // SHA-256 of the float32 samples, L R interleaved, 64 hex digits; the report's line shows its first 16): an
        // instrument's render of the notes held (or the scenario), not the pitch's, the chord's or the sixteen voices',
        // over its own length (Timeline::plain: a window past its end renders on, which the hash leaves out, so the same
        // sound hashes the same whatever the window); an effect's test signals' renders in turn (the sine's, the
        // noise's, the impulse's and silence's; the steady tone's aside), its own input's after them, which a window
        // never lengthens. `hashed`: those renders, in words.
        std::string hash, hashed;
        // The profile (Request::profile; dsp/Profile.h), as its line gives it: each function's share of the time sampled
        // (the most first), the share outside the program's code, and the samples; empty without one.
        std::vector<profile::Sampler::Share> profile;
        double profileOutside = 0.0;
        std::size_t profileSamples = 0;
    };

    // One compile, measured as often as asked. `pins`: its netlists generated with these configurations (Sources.h's
    // CircuitPins, a recording's: what played), not the self-test's store.
    class Session
    {
    public:
        explicit Session(const Sources& files, double rate = 48000.0, const CircuitPins* pins = nullptr);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        bool compiled() const noexcept;
        const std::string& errors() const noexcept { return errors_; }   // the compiler's messages when it did not
        // An init (an instance's or Shared's) that ran past its budget or failed a bounds check (Patch.h): the program
        // did not compile, and errors() says why; none when it compiled or its text did not.
        Patch::Method initStopped() const noexcept { return initStopped_; }
        const Controls& controls() const noexcept;
        double rate() const noexcept { return rate_; }
        // The program as its Engine plays it (its fields, between a render's calls: dsp/Debug.h); null when it did not
        // compile.
        const Patch* patch() const noexcept;

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
            Patch::Method stopCall = Patch::Method::none;   // a call that ran past its budget (Patch.h, "The loop budget"),
            std::size_t stopAt = 0;               // and the sample its block (or its voice's part) began at
            Patch::Bounds stopBounds;             // or the bounds check it failed (Patch.h, "Bounds checks")
            bool cancelled = false;               // cancelled (setCancel) at `reached` samples: the rest is silent
            std::size_t reached = 0;
        };
        Render render(const Request& request, const std::vector<float>* inputL, const std::vector<float>* inputR,
                      std::size_t length, RenderHooks* hooks = nullptr);
        // What `request` plays over `length` samples (0: lengthOf), by absolute sample.
        Timeline timeline(const Request& request, std::size_t length = 0) const;
        // The samples a render of `request` takes: an instrument's held part (its phrase's, else holdSeconds) and the
        // tail; an effect's holdSeconds and the tail, or its own input's length; at least until its window's end.
        std::size_t lengthOf(const Request& request) const;
        // A render that stopped (Render::stopCall, stopBounds) in words: the stop's message (Patch::stopMessage), and
        // where.
        std::string stopWords(Patch::Method call, double seconds, const Patch::Bounds* bounds = nullptr) const;

        // From now on each render asks `cancel` every kCancelSamples samples (above), its time limit counted from its
        // start (here when it has none: Cancel::started), and stops when it says so; so does every render after it, at
        // once, until the next setCancel. cancelled(): one did; cancelWords(): what was rendering, how far it got,
        // after how long and how fast ("Cancelled 84.21 s into the render of the notes held (301.50 s long), 12.3 s
        // after it began, about 6.8 s of sound a second."; at the time limit "Stopped at the time limit (170 s): the
        // render of the notes held had reached 150.87 s of its 301.50 s, about 0.89 s of sound a second; a shorter
        // render fits.", or, when the limit lands in a short render after a long one, "Stopped at the time limit (170
        // s) 0.00 s into a short render after it (0.60 s long): the render before it, the render of the notes held
        // (301.50 s long), took 169.9 s; a shorter render fits."). measure() adds " Nothing was measured.".
        void setCancel(Cancel cancel);
        bool cancelled() const noexcept;
        std::string cancelWords() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::string errors_;
        Patch::Method initStopped_ = Patch::Method::none;
        double rate_;
    };

    // The report, a few lines of plain text; when the program does not compile, its messages.
    std::string run(const Sources& files, const Request& request);
    // The same, cancelled as `cancel` says (Session::setCancel); `cancelled` (when given) says whether it was.
    std::string run(const Sources& files, const Request& request, const Cancel& cancel, bool* cancelled = nullptr);

    // ---- the analysis the metrics read, for the probes, SoundView.h and trace (Debug.h) -----------------------------
    // An FFT in place: a.size() a power of two, forward (e^-i).
    void fft(std::vector<std::complex<double>>& a);
    struct Peak
    {
        double hz = 0.0;
        double db = 0.0;                          // relative to the strongest
    };

    using Span = std::pair<std::size_t, std::size_t>;        // samples [first, second)
    inline constexpr double kSettleSeconds = 0.05;           // a held part starts this long after its note-on
    inline constexpr double kSpectrumSeconds = 0.1;          // the Hann frames: the centroid, the partials
    inline constexpr double kPitchSeconds = 0.4;             // the Hann frames of the pitch (low close notes part)
    inline constexpr double kPuritySeconds = 0.4;            // the Blackman-Harris frames: inharmonic_db, thd_db
    inline constexpr double kBandSeconds = 0.4;              // the Hann frames of the band levels (bandLevels): a
                                                             // main lobe of 5 Hz each side, under a third-octave band
                                                             // from 25 Hz up (kSharpBins)
    inline constexpr double kNoteBandSeconds = 2.0;          // a semitone's in the report (bandSpectrum): its band is
                                                             // 2.4 Hz wide at E1, 1.2 Hz at the lowest note, 16 (E0)
    inline constexpr double kSharpBins = 2.25;               // a band at least this many of its frame's bins wide (5.6 Hz
                                                             // for 0.4 s, 2.25 Hz for 1 s) holds a tone at its centre
                                                             // within 0.3 dB, the bands beside it 13 dB under; a narrower
                                                             // one holds less and the tone spreads over those beside it
    inline constexpr double kBandLowHz = 20.0, kBandHighHz = 20000.0;   // the band (the Nyquist frequency when lower)
    inline constexpr double kPitchCents = 600.0;             // how far from the expected note the pitch looks
    inline constexpr double kSameBendCents = 1.0;            // pitch wheel moves within this of each other: one stretch

    // The held part of a timeline's notes (an instrument's): from kSettleSeconds after each note-on (half the note when
    // it is shorter) to its note-off (a key pressed again while held ends it there and starts the next; a key still down
    // ends at `heldEnd`), the spans joined where they overlap, by sample. Notes held together give one span.
    std::vector<Span> heldSpans(const Timeline& timeline, std::size_t heldEnd, double rate);

    // The expected pitch's stretches: `held` (the held part, or a window) cut where the expected pitch changes: the lowest
    // key held there (from its held span's start, as heldSpans; where none is, in a window, the lowest sounding, from its
    // note-on to its note-off; where none sounds, the lowest of those released last; before any note-on, `key`) bent by
    // the pitch wheel at 2 semitones (host.fm's), as the timeline's events set them; the wheel's moves within
    // kSameBendCents of each other make one stretch, bent by their mean over it. `next`: the nearest key above it held
    // (or sounding, as `key`) anywhere in the stretch (-1: none). The metrics read the pitch of each stretch alone
    // (Session::measure).
    struct Stretch
    {
        Span span;
        int key = 48, next = -1;
        float bend = 0.0f;                        // the pitch wheel, -1..1
        double hz() const;                        // its expected pitch
    };
    std::vector<Stretch> stretches(const Timeline& timeline, const std::vector<Span>& held, std::size_t heldEnd, double rate, int key);

    enum class Taper : uint8_t
    {
        hann,                                     // sidelobes -31 dB, a main lobe of 2 bins each side
        blackmanHarris,                           // four terms: sidelobes under -92 dB, a main lobe of 4 bins each side
    };

    // Welch's average power spectrum of `x` over `spans`: in each span, frames of `seconds` (one frame of the span's
    // length when it is shorter) spread evenly from its start to its end, overlapped by half or more, each tapered and
    // zero-padded to the power of two at or above the frame's length, its power over the taper's (so noise gives its
    // mean square whatever the frame); each span counts as its length. Frames fixed in seconds: at any rate the same
    // stretch of sound, the same resolution in Hz.
    struct Spectrum
    {
        double rate = 0.0;
        double hzPerBin = 0.0;                    // rate over the FFT's size
        double resolutionHz = 0.0;                // 1 / the shortest frame's length in seconds (a bin of the frame's own)
        double frameSeconds = 0.0;                // the longest frame's length: `seconds`, or the longest span's
        std::vector<double> power;                // bins 0 to the Nyquist frequency's
        std::size_t frames = 0;                   // 0: no span long enough (16 samples)

        // The power-weighted mean frequency over [lo, min(hi, the Nyquist frequency)]; 0 with no power there.
        double centroid(double lo = kBandLowHz, double hi = kBandHighHz) const;
        // Its strongest partials: local maxima (over two bins each side) within `withinDb` of the strongest bin, at most
        // `count`, by frequency, each placed and sized by a parabola through its log power and its neighbours'.
        std::vector<Peak> peaks(int count, double withinDb = 40.0) const;
        // The strongest bin's power (0: silent), and a bin's place and log power refined by that parabola.
        double strongest() const;
        void refine(std::size_t bin, double& hz, double& logPower) const;
        // The mean square in [lo, hi), cut at the Nyquist frequency: each bin's power by the share of its width there (DC
        // none), so that noise reads its power in the band whatever the frame, and a sine of amplitude a at the band's
        // centre a^2 / 2 when the band holds the frame's main lobe (within 0.3 dB from kSharpBins of the frame's bins
        // wide). NaN when lo is at or above the Nyquist frequency.
        double bandPower(double lo, double hi) const;
    };
    Spectrum spectrum(const std::vector<float>& x, double rate, const std::vector<Span>& spans, Taper taper = Taper::hann,
                      double seconds = kSpectrumSeconds);
    // The same of both channels: the mean of the left's and the right's power (as rms_db's RMS of both), in one FFT a
    // frame (the channels its real and imaginary parts); in kBandSeconds frames unless asked.
    Spectrum stereoSpectrum(const std::vector<float>& l, const std::vector<float>& r, double rate, const std::vector<Span>& spans,
                            Taper taper = Taper::hann, double seconds = kBandSeconds);

    // Fractional-octave bands, each from its centre over 2^(1/2b) to its centre times it (b bands an octave): an
    // octave's ten and a third's thirty-one, IEC 61260's base-2 bands (1 kHz x 2^(k/b)) from 31.5 Hz to 16 kHz and from
    // 20 Hz to 20 kHz, each named by its nominal centre; a semitone's 120, centred on the MIDI notes 16 to 135 (A 440:
    // 20.6 Hz to 19.9 kHz), the averaged spectrum's points.
    struct Band
    {
        double hz = 0.0;                          // its centre: a nominal one for an octave's and a third's (31.5, 63, 125)
        double lo = 0.0, hi = 0.0;                // its edges
        int note = -1;                            // a semitone's MIDI note
    };
    inline constexpr std::array<double, 10> kOctaveBands{ 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    std::vector<Band> bandsOf(Bands kind);
    // Each band's level in a spectrum (Spectrum::bandPower), in dBFS (-240: none there; NaN: above the Nyquist frequency):
    // an instrument's from bandSpectrum over the held part, compare's and the capture's from stereoSpectrum over what
    // they analyse.
    std::vector<double> bandLevels(const Spectrum& spectrum, Bands kind);
    // The spectrum a report's bands line reads (Request::spectrum): stereoSpectrum over `spans` in Hann frames of
    // kBandSeconds (a semitone's kNoteBandSeconds; a shorter span's frame is the span).
    Spectrum bandSpectrum(Bands kind, const std::vector<float>& l, const std::vector<float>& r, double rate,
                          const std::vector<Span>& spans);
    // The first of bandsOf(kind) from which each band is at least kSharpBins of the spectrum's shortest frame's bins
    // wide (a band's width grows with its centre): 0 every band; bandsOf(kind).size() none.
    std::size_t sharpFrom(Bands kind, const Spectrum& spectrum);
    // A band's centre in the bands' lines: "31.5", "250", "1k", "1.25k", "16k".
    std::string bandName(double hz);

    // The pitch near `expectedHz`, in (loHz, hiHz): the strongest partial there (a local maximum within 60 dB of the
    // spectrum's strongest and 10 dB above the median of the range); without one, the f0 there (in steps of a cent)
    // whose harmonics up to the 20th, in the band, sum the most magnitude, placed by the strongest of them. 0: silent.
    // The metrics read it from kPitchSeconds Hann frames: a main lobe of 5 Hz each side, so that a low chord's notes
    // (C3 and D3 are 16 Hz apart, E2 and F#2 10 Hz) and a low note's range of 600 cents hold distinct bins.
    struct Pitch
    {
        double hz = 0.0;
        bool partial = false;                     // a partial of its own (false: its harmonics' sum)
    };
    Pitch pitchOf(const Spectrum& spectrum, double expectedHz, double loHz, double hiHz);

    // The strongest component of a spectrum between 20 Hz and maxHz (and the Nyquist frequency) that is no harmonic of
    // f0 (farther than six of its resolution bins from every multiple of it: 15 Hz with kPuritySeconds frames): an
    // alias, or the sound's own. A component is a peak (a local maximum over two bins each side): a slow attack widens
    // each harmonic's lobe past the six bins within a frame, and that skirt, falling away from its harmonic, has none. A
    // decay through the held part can still make peaks of its own (frames whose level falls across them: 60 dB in 0.1 s
    // reads about -29 dB, in 1 s about -87), so a decaying sound's aliasing is judged on a held, steady note. db relative
    // to the strongest component, -200 when nothing is found. The second form reads all of `x` through kPuritySeconds
    // Blackman-Harris frames.
    Peak inharmonic(const Spectrum& spectrum, double f0, double maxHz);
    Peak inharmonic(const std::vector<float>& x, double rate, double f0, double maxHz);
    // The energy in the band farther than six resolution bins from `hz`, against all of it in the band, in dB: a sine's
    // THD+N (its harmonics, aliases and noise); -240 when nothing is away from it.
    double awayDb(const Spectrum& spectrum, double hz);
    // The strongest partials of all of `x` (kSpectrumSeconds Hann frames; Spectrum::peaks), at most `count`, by
    // frequency; and its centroid (Spectrum::centroid).
    std::vector<Peak> spectrumPeaks(const std::vector<float>& x, double rate, int count, double* centroidHz = nullptr);

    // ---- loudness and time variation: lufs (noise_lufs), rms_a_db, off_order_db, wobble_db -----------------------------
    inline constexpr double kLoudnessBlockSeconds = 0.4;     // BS.1770's gating blocks, one every kLoudnessStepSeconds
    inline constexpr double kLoudnessStepSeconds = 0.1;
    inline constexpr double kOrderSeconds = 2.0;             // off_order_db's Blackman-Harris frames: as long as the part,
                                                             // up to this (an order's zone 3 Hz at 2 s, 6.3 Hz at 0.95 s)
    inline constexpr double kOrderBins = 6.0;                // an order's zone: within this many of the shortest frame's
                                                             // bins of a multiple of the base (its main lobe: 4)
    inline constexpr double kWobbleRangeDb = 30.0;           // wobble_db's bands: within this of the loudest
    inline constexpr double kWobbleFloorDb = 60.0;           // a band's share in a frame: at least this far under the whole
    inline constexpr double kSteadyToneDb = -18.0;           // an effect's steady tone (wobble_db's): its RMS, as the noise's
    inline constexpr double kSteadyFadeSeconds = 0.01;       // and its fade-in

    // BS.1770-4's integrated loudness of both channels over `spans`, in LUFS: each channel K-weighted (its two filters made
    // for `rate` from the analog shelf and high-pass its 48 kHz coefficients come from: the same at 48 kHz, within 0.03 dB
    // at 192 kHz), the channels' mean squares summed over blocks of kLoudnessBlockSeconds every kLoudnessStepSeconds inside
    // each span (a span shorter than a block: one block of it; under 16 samples: none), gated at -70 LUFS, then 10 LU under
    // the mean of the blocks left; -240 when no block is over -70.
    double loudness(const std::vector<float>& l, const std::vector<float>& r, double rate, const std::vector<Span>& spans);

    // IEC 61672's A-weighting of a frequency as a power ratio, 1 at 1 kHz; and a spectrum's power A-weighted in the band
    // (kBandLowHz to kBandHighHz, cut at the Nyquist frequency), its mean square (rms_a_db's, of bandSpectrum's semitone
    // frames: a tone's lobe narrow enough that the weight across it holds from 20 Hz).
    double aWeight(double hz);
    double aWeightedPower(const Spectrum& spectrum);

    // The orders of a base (its multiples, from the base up) in a spectrum: off_order_db, the power in the band farther
    // than kOrderBins of the spectrum's shortest frame's bins from every multiple (an order's zone), against all of it in
    // the band; -240 when none is off them (or the band is silent). With kOrderSeconds Blackman-Harris frames: a pure
    // harmonic sound's lobes stay inside the zones. With no frame, or a base under twice the zone (every frequency near
    // a multiple), off_order_db is not measured (Result::unmeasured, saying why).
    struct Orders
    {
        double offDb = -240.0;
        double zoneHz = 0.0;                      // how far from a multiple counts as on it
        // The power near the half-orders (the odd multiples of half the base) and the third-orders (the multiples of a
        // third of it that are not the base's), off the orders, against all of it: a period of two or three of the base's
        // cycles. NaN where those zones would overlap (half or a third of the base under two zones).
        double halfDb = std::numeric_limits<double>::quiet_NaN();
        double thirdDb = std::numeric_limits<double>::quiet_NaN();
        std::vector<Peak> lines;                  // the strongest local maxima off the orders (within 60 dB of the
                                                  // strongest component), at most 3, strongest first, db under the strongest
    };
    Orders orders(const Spectrum& spectrum, double baseHz);

    // How the spectrum's shape moves (wobble_db): over `spans` in kSpectrumSeconds Hann frames (wobbleFrame's samples;
    // a span shorter than a frame has none), each third-octave band's share of the frame's power in the thirds, in dB (at
    // least -kWobbleFloorDb), and its standard deviation over the frames; the mean of those, weighted by each band's
    // power, over the bands within kWobbleRangeDb of the loudest. An instrument's frames are spread evenly over each span,
    // at most half a frame apart (a span over 1.5 frames has three). With `onPeriod` (an effect's, of steadyTone's
    // output) they lie end to end, each from a multiple of a frame from the render's start: on the tone's period, so that
    // an effect that holds still gives every frame the same samples once its memory has settled (a delay, an echo, an
    // allpass or a filter 0, a reverb whose tail is still building a little), and only what moves reads. A silent frame
    // has no shape and is left out; NaN with fewer than three frames that sound.
    struct Wobble
    {
        double db = std::numeric_limits<double>::quiet_NaN();
        double bandHz = 0.0;                      // the band of those weighed that moves most (its nominal centre)
        double bandDb = 0.0;                      // its standard deviation
        int frames = 0;                           // the frames that sound
        int silent = 0;                           // and those left out as silent
    };
    Wobble wobble(const std::vector<float>& l, const std::vector<float>& r, double rate, const std::vector<Span>& spans,
                  bool onPeriod = false);
    // wobble_db's frame in samples at `rate` (kSpectrumSeconds, at least 16); and an effect's steady tone for it, `n`
    // samples: a partial every rate / wobbleFrame(rate) (10 Hz) in the band (kBandLowHz to kBandHighHz, under the Nyquist
    // frequency), each of the same amplitude (a flat spectrum, as the test noise's) and a fixed random phase, kSteadyToneDb
    // RMS, repeating every wobbleFrame(rate) samples exactly, faded in over kSteadyFadeSeconds.
    std::size_t wobbleFrame(double rate);
    std::vector<float> steadyTone(std::size_t n, double rate);

    // The click rule, the metrics' and trace's (Debug.h): in 10 ms windows from the first sample, each window's largest
    // step between two neighbouring samples (of either channel); a window's step is a click when it is over 0.05, over
    // three times the second largest of the ten windows each side (a step that comes back, as a low saw's wrap or an
    // engine's firing does, is the sound's own) and over 0.35 of the largest sample within five windows each side.
    // Every click, the one that stands out most (its step over that second largest) first.
    struct Click
    {
        std::size_t at = 0;                       // the sample after the step
        double size = 0.0;
        double usual = 0.0;                       // the second largest step around it
    };
    std::vector<Click> clicks(const std::vector<float>& l, const std::vector<float>& r, double rate);

    // The click locator: the largest step between two neighbouring samples (of either channel), then the clicks (by the
    // rule above) farther than 10 ms from it, largest first, `count` at most; and a step's nearest moment of a timeline
    // in words ("0.4 ms after note-on 52 (the same key again 0.12 s after its release)"; "no event within 50 ms" when
    // none is near).
    struct Step
    {
        double size = 0.0;
        std::size_t at = 0;                       // the sample after the step
        bool click = false;                       // a click by the rule
    };
    std::vector<Step> largestSteps(const std::vector<float>& l, const std::vector<float>& r, std::size_t count, double rate);
    std::string nearestMark(const Timeline& timeline, std::size_t at, double rate);
}
