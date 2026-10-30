// Source/plugin/Capture.h: the recorder (the capture tool, ChatCapture.cpp): what the host plays through the plugin,
// kept in the plugin's memory, for Claude to read as numbers and to replay offline. JUCE-free.
//
// What a recording holds, per host block (Processor::hostBlock and play call the audio thread's side):
//   - its size and its first sample, counted from the recording's start (a block of 0 samples too);
//   - the events the Engine got (an instrument's MIDI after toEvent, and the panel keyboard's, at their samples; a
//     note-on dropped while bypassed is not among them, since the Engine never got it);
//   - the dials' 32 normalised values as the Engine got them (setControls, once a block);
//   - the Engine's mod wheel, pitch wheel, pressure and pedal at its start (its Host block: a replay from there starts
//     on them);
//   - the host's transport as the Engine got it (setTransport), and the bypass;
//   - the program that played (Meters::program after the block) and whether its frozen copy did;
//   - the output guard's alerts raised in it (Engine::alertTally) and its processing time over its length;
// and the samples: an effect's input as it reached the Engine (before it processed in place) and the Engine's output
// (before the bypass's mix: what a replay gives back), left and right (a mono layout's channel twice). Its header: the
// rate, the processor's part size (prepareToPlay's block: a larger host block reaches the Engine in parts of it), the
// Engine's clock at the start (its samples since prepare: the phase of the instrument's 16-sample grid, Engine::kGrid),
// what started it, the voices sounding then and the output's peak just before. With the files of each program that
// played (noteProgram) and the rate, that is what an exact replay needs: Patch::compile(files, rate), the same parts,
// dials, transport and events, and the program's own code (below).
//
// Modes (Options):
//   last  (the default) records from the first block after arm, round and round; stop keeps the last `seconds`
//   next  waits for the first note-on the Engine gets, the host's transport starting to play, or (an effect) the
//         input's first sound above -60 dBFS; records from the start of that block, fills `seconds` once and ends
// restart (next only): the processor runs the files that play again first (a fresh compile: Processor::chatCapture)
// and the recording waits until that program plays alone (its swap's 20 ms fade over), so that it starts from the
// program's fresh state, as a replay does: what still sounds then is cut, so Claude asks the user first.
// stopOnAlert: the recording ends half a second after the output guard's first alert. It also ends when the host
// prepares the plugin again once it has started (or before, at another rate or block size: prepare()), and, in next,
// when it is full.
//
// The frozen copy (Engine.h): while a recording is armed the program's own code plays (Engine::setOwnCode, through the
// hook the processor gives: the copy playing is dropped at the next block, none is taken, the freezer waits), since the
// copy's Math::Fast is not the program's bits and a replay of it would not match. The cost: the plugin's CPU while armed
// is the program's own, flat calls (what it costs while its dials move, less the glides), usually a little more than
// the copy's; the freezer comes back when the recording ends (stop, forget, full, ten minutes without a call).
//
// Threads. act() and housekeep() and noteProgram(): any thread but the audio thread, under the recorder's mutex. The
// audio thread: prepare (while it is stopped), begin, input, output, event, end; no allocation, lock or syscall,
// relaxed and release stores into buffers the arming thread allocated (and touched). A recording crosses by atomic
// pointers, as the Engine's programs do: arm leaves it in `armed_` (announce), the audio thread takes it at a block's
// start (claim), and once asked to stop it hands it back through `retired_`, for housekeep (or the next call) to free.
// The audio thread never frees one and never waits: a full ring wraps (last) or ends the recording (next), with a
// flag. A reader copies what was published and leaves out what the audio thread may have overwritten while it copied
// (each ring announces what it is about to write), so stop and status never wait for the audio thread, and need no
// block to come.
//
// Privacy. Nothing here is written anywhere: a take lives in memory until forget, the next arm, or ten minutes without
// a call (housekeep), and leaves only as the numbers the tool reports.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Sources.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace playground::capture
{
    inline constexpr double kMaxSeconds = 20.0;
    inline constexpr double kMinSeconds = 1.0;
    inline constexpr double kDefaultSeconds = 10.0;
    inline constexpr double kIdleSeconds = 600.0;           // without a call: everything freed
    inline constexpr double kAlertTailSeconds = 0.5;        // recorded after the first alert (stopOnAlert)
    inline constexpr float kSoundThreshold = 0.001f;        // -60 dBFS: an effect's first sound (next)
    inline constexpr int kEventCapacity = 32768;            // an instrument's events a recording holds
    inline constexpr int kAverageBlock = 32;                // the host blocks a ring is sized for, on average

    enum class Mode : uint8_t { last, next };
    enum class Action : uint8_t { arm, stop, take, status, forget, replayed };

    struct Take;

    struct Options
    {
        Mode mode = Mode::last;
        double seconds = kDefaultSeconds;                   // kMinSeconds to kMaxSeconds
        bool stopOnAlert = false;
        bool restart = false;                               // next: from the program's fresh state (above)
        uint64_t freshFrom = 0;                             // restart: the compile request it waits for (the processor's)
        // replayed: what a replay of `take` found (ChatCaptured.cpp: its null test against what played, in a line),
        // kept with it while it is the take kept, for stop and status (Reply::replay).
        std::shared_ptr<const Take> take{};
        std::string replay{};
    };

    // What the recorder is doing (Reply::state).
    enum class State : uint8_t
    {
        off,          // nothing armed, nothing kept
        waiting,      // armed, not started yet: next's start, or no audio since arm
        recording,
        ended,        // it ended by itself (full, an alert, the host prepared again) and waits for stop
        kept,         // stopped: its take is kept (take)
    };

    // Why a recording ended (Take::end).
    enum class End : uint8_t
    {
        none,         // it still records (a snapshot)
        stopped,      // stop
        full,         // next: it filled its seconds (or its events or blocks: Take::trimmed)
        alert,        // stopOnAlert
        prepared,     // the host prepared the plugin again (another rate or block size)
    };

    // What started it (Take::start).
    enum class Start : uint8_t { arm, note, transport, sound };

    // What limited it (Take::trimmed, flags): a ring that filled before the samples' did.
    enum Trim : uint8_t
    {
        blocksLimit = 1u << 0,      // the host's blocks were small: more of them than the ring holds
        eventsLimit = 1u << 1,      // more events than kEventCapacity
    };

    // One host block, as recorded. Plain data (copied through atomic words).
    struct Block
    {
        uint64_t sample = 0;        // its first sample, from the take's start
        uint64_t event = 0;         // its first event (Take::events' index)
        uint64_t program = 0;       // Meters::program after it (Patch::program: the compile's request)
        double bpm = 120.0, beat = 0.0, barStart = 0.0;   // the host's transport at its first sample (Transport;
                                                          // without one, beat is the Engine's own: Engine::beat)
        float load = 0.0f;          // its processing time over its length (1 = a whole core; 0 for an empty block)
        uint32_t size = 0;          // samples
        uint32_t events = 0;
        uint16_t nonFinite = 0, clipped = 0;   // the output guard's calls that replaced a sample, that clipped one
        uint16_t steals = 0, dropped = 0;      // the Engine's steals and dropped voices in it (Meters::steals and
                                               // ::dropped, read at the block's start and end: Recorder::end)
        uint8_t beatsPerBar = 4, beatUnit = 4;
        uint8_t flags = 0;          // kKnown, kPlaying, kBypassed, kFrozen
        uint8_t reserved = 0;
        std::array<float, kMaxControls> dials{};   // normalised, as the Engine got them (Controls.h)
        std::array<float, 4> wheels{};             // the Engine's at its start (its Host block): the mod wheel, the pitch
                                                   // wheel, pressure, the pedal (1 down): what a replay from here starts on

        static constexpr uint8_t kKnown = 1u << 0;     // the host gave a transport (Transport::known)
        static constexpr uint8_t kPlaying = 1u << 1;
        static constexpr uint8_t kBypassed = 1u << 2;  // processBlockBypassed
        static constexpr uint8_t kFrozen = 1u << 3;    // the program's frozen copy played (Engine::frozen)
        bool has(uint8_t f) const noexcept { return (flags & f) != 0; }
    };

    // An event the Engine got, at its sample from the take's start.
    struct Note
    {
        uint64_t sample = 0;
        EventKind kind = EventKind::noteOn;
        uint8_t note = 0;
        uint16_t reserved = 0;
        float velocity = 0.0f;

        Event event(uint32_t offset) const noexcept
        {
            Event e;
            e.offset = offset;
            e.kind = kind;
            e.note = note;
            e.velocity = velocity;
            return e;
        }
    };

    // A recording taken: its own copy (stop), or one still recording as it stands (a snapshot: status).
    struct Take
    {
        Options options;
        double rate = 48000.0;
        int part = 0;               // the processor's largest Engine call (prepareToPlay's block)
        int channels = 2;           // the layout's (1: mono, its channel twice here)
        bool input = false;         // an effect's: `in` holds what came in
        Start start = Start::arm;
        End end = End::none;
        uint8_t trimmed = 0;        // Trim flags
        int voicesAtStart = 0;      // an instrument's voices sounding when it started
        float peakBefore = 0.0f;    // the output's peak in the block before it started (next: a tail still ringing)
        bool restarted = false;     // restart: it waited for the program's fresh compile
        bool restartPending = false;   // and that program does not play alone yet (nothing recorded)
        uint64_t clock = 0;         // the Engine's samples since prepare at the take's first sample
        double fromSeconds = 0.0;   // where the take starts in the recording (last: what it dropped before)
        double recordedSeconds = 0.0;   // how long it recorded in all
        double startedAgo = 0.0, endedAgo = 0.0;   // seconds before the take was made (steady clock)
        std::chrono::steady_clock::time_point madeAt{};   // when it was made
        bool claimed = false;       // the audio thread took it (audio came after arm)
        double armedAgo = 0.0;      // seconds since arm, when the take was made
        std::vector<Block> blocks;
        std::vector<Note> events;
        std::array<std::vector<float>, 2> in, out;   // left, right
        std::vector<std::pair<uint64_t, Sources>> programs;   // the files of each program that played, for a replay
        std::vector<std::pair<uint64_t, Controls>> programDials;   // and its dials (what its blocks' values are of)
        std::vector<std::pair<uint64_t, CircuitPins>> programPins;   // and its netlists' configurations (the replay's)
        std::size_t bytes = 0;      // what it holds

        uint64_t samples() const noexcept { return out[0].size(); }
        double seconds() const noexcept { return rate > 0.0 ? static_cast<double>(samples()) / rate : 0.0; }
    };

    // The take in numbers (summarize): what the tool reports.
    struct Levels
    {
        double peak = 0.0, rms = 0.0;   // linear, both channels
        double dc = 0.0;                // the larger channel mean, signed
        uint64_t nonFinite = 0;         // samples that were not numbers (an input's)
    };

    struct DialMove
    {
        int slot = 0;
        float low = 0.0f, high = 0.0f;  // normalised
        int moves = 0;                  // gestures: changes 50 ms or more apart start a new one
        double first = 0.0, last = 0.0; // seconds
    };

    struct Alert
    {
        double at = 0.0;
        bool nonFinite = false, clipped = false;
    };

    struct Summary
    {
        int blocks = 0, empty = 0;                              // host blocks, and those of 0 samples
        int smallest = 0, largest = 0, common = 0, commonCount = 0;   // the others' sizes (common: the most frequent)
        int notes = 0, lowest = -1, highest = -1, mostHeld = 0, mostSounding = 0;
        int softest = 127, hardest = 0;                         // velocities, MIDI's 1 to 127
        int pedalDowns = 0;
        int modEvents = 0, bendEvents = 0, pressureEvents = 0, allNotesOff = 0;
        float modLow = 1.0f, modHigh = 0.0f, bendLow = 1.0f, bendHigh = -1.0f, pressureHigh = 0.0f;
        std::vector<DialMove> dials;
        std::vector<std::pair<double, uint64_t>> programs;     // each program from its first block (the first at 0)
        double frozenSeconds = 0.0, frozenFrom = -1.0, frozenTo = -1.0;
        std::vector<Alert> alerts;                              // the first few blocks that raised one
        int alertBlocks = 0;
        double lastAlert = 0.0;
        float worstLoad = 0.0f, medianLoad = 0.0f;
        double worstAt = 0.0;
        int steals = 0, dropped = 0;
        bool transport = false;                                 // the host gave one
        int transportStarts = 0, transportStops = 0;
        double playingSeconds = 0.0;
        double bpmLow = 0.0, bpmHigh = 0.0, beatFrom = 0.0, beatTo = 0.0;
        int beatsPerBar = 4, beatUnit = 4;
        double bypassSeconds = 0.0, bypassFrom = -1.0, bypassTo = -1.0;
        Levels in, out;
    };

    Summary summarize(const Take& t);

    // What act() answers.
    struct Reply
    {
        State state = State::off;
        std::shared_ptr<const Take> take;   // stop, status, take: the take kept, or a snapshot of one recording
        std::string replay;                 // what a replay of the take kept found ("" until one is made: replayed)
        bool armedNow = false;              // arm: armed (false, with why, when it could not be)
        bool audio = false;                 // the plugin has been prepared (the host runs it)
        std::size_t bytes = 0;              // what the recorder holds now
        std::size_t freed = 0;              // forget: what it held before
        std::string why;
    };

    class Recorder
    {
    public:
        Recorder();
        ~Recorder();                         // frees everything (the audio thread has stopped)
        Recorder(const Recorder&) = delete;
        Recorder& operator=(const Recorder&) = delete;

        // The processor's: what plays the program's own code while a recording is armed (Engine::setOwnCode). Called
        // under the recorder's mutex, when that changes.
        void setOwnCode(std::function<void(bool)> hook);

        // ---- off the audio thread (any thread), under the recorder's mutex ---------------------------------------
        // arm (options), stop, take, status, forget: the tool's actions (Reply). stop and status are live: they copy
        // what is published and never wait for the audio thread (above).
        Reply act(Action action, const Options& options);
        // The processor's poll (the message thread, 20 Hz): frees what the audio thread handed back, notices a
        // recording that ended by itself (the program's own code then need not play), and after kIdleSeconds without
        // a call forgets everything. Skips a round while a call holds the mutex.
        void housekeep();
        // A program that plays now (a compile that landed: Processor::apply): its files, its dials and the configuration
        // each of its netlists was generated with (CompileStatus::circuits), for a replay.
        void noteProgram(uint64_t program, const Sources& files, const Controls& controls = {}, const CircuitPins& pins = {});
        bool audio() const noexcept { return rate_.load(std::memory_order_relaxed) > 0.0f; }   // prepared (the host runs it)
        std::size_t bytes() const;           // what it holds (probes: forget frees)
        int rings() const;                   // recordings allocated and not freed yet (probes)

        // ---- the audio thread ------------------------------------------------------------------------------------
        // prepareToPlay's (the audio thread stopped): the rate and the processor's part size; a recording under way
        // ends there (End::prepared).
        void prepare(float rate, int part) noexcept;
        // A host block: begin before the Engine's first call (the dials and the transport as it gets them), input and
        // output around each call (a part of the block: `channels` of `n` samples, in order), event for each event it
        // gets (`at`: its sample in the host block), end after the last call.
        void begin(int n, bool bypassed, const Transport& t, const std::array<float, kMaxControls>& dials,
                   const Engine& engine) noexcept;
        void input(const float* const* channels, int numChannels, int n) noexcept;
        void output(const float* const* channels, int numChannels, int n) noexcept;
        void event(int at, const Event& e) noexcept;
        void end(const Engine& engine) noexcept;

    private:
        struct Ring;                         // one recording's buffers and counters (Capture.cpp)

        // The reader's side (mutex_).
        Reply arm(const Options& o);
        Reply stop();
        Reply status(bool take);
        Reply forget();
        void collect();                      // frees what the audio thread handed back
        void release(Ring* r);               // r no longer wanted: freed now if the audio thread never took it,
                                             // else asked to stop (and handed back at its next block)
        void setOwn(bool on);
        void destroy(Ring* r);               // erased from owned_: freed
        std::shared_ptr<Take> snapshot(Ring& r) const;
        std::size_t heldBytes() const;
        State stateOf(const Ring& r) const;

        mutable std::mutex mutex_;
        std::vector<std::unique_ptr<Ring>> owned_;   // every ring not yet freed
        Ring* current_ = nullptr;                    // the one armed last, until stopped or forgotten
        std::shared_ptr<const Take> take_;           // the one stopped last
        std::string replay_;                         // what a replay of it found (replayed)
        std::vector<std::pair<uint64_t, Sources>> programs_;   // the last few programs' files (noteProgram)
        std::vector<std::pair<uint64_t, Controls>> programDials_;   // and their dials
        std::vector<std::pair<uint64_t, CircuitPins>> programPins_;   // and their netlists' configurations
        std::chrono::steady_clock::time_point lastUse_{};
        bool own_ = false;                           // what the hook was told last
        std::function<void(bool)> ownHook_;

        // Crossing: arm announces, the audio thread claims; it hands back, collect frees.
        std::atomic<Ring*> armed_{ nullptr };
        std::atomic<Ring*> retired_{ nullptr };
        std::atomic<float> rate_{ 0.0f };            // prepare's (0: never prepared)
        std::atomic<int> part_{ 0 };
        std::atomic<uint32_t> epoch_{ 0 };           // prepare's count: a recording of another epoch ends

        // The audio thread's.
        Ring* live_ = nullptr;
        bool writing_ = false;                       // this block is being written
        bool aborted_ = false;                       // this block did not fit (next: the recording ends before it)
        bool lastPlaying_ = false;                   // the host played in the block before
        bool startNow_ = false;                      // next: this block starts it
        bool hold_ = false;                          // restart: the fresh program does not play alone yet
        Start startWhy_ = Start::arm;
        uint32_t seenEpoch_ = 0;
        uint64_t clock_ = 0;                         // samples since prepare (the Engine's clock)
        uint64_t inAt_ = 0, outAt_ = 0;              // this block's next sample, per stream
        uint32_t blockEvents_ = 0;
        uint64_t tally_ = 0;                         // Engine::alertTally at the block's start
        uint32_t steals_ = 0, dropped_ = 0;          // Meters' steals and dropped voices at the block's start
        int voices_ = 0;                             // voices sounding at the block's start (next's start)
        float peak_ = 0.0f, lastPeak_ = 0.0f;        // the output's peak in this block, in the one before
        std::chrono::steady_clock::time_point t0_{};
        Block pending_{};
    };
}
