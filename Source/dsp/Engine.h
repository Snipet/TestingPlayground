// Source/dsp/Engine.h: the audio side, JUCE-free. It plays the program the editor last ran (a Patch, Patch.h) and
// changes programs without a click:
//   an effect     the new program and the old run side by side for 20 ms while the output crossfades (smootherstep)
//                 from the old one's to the new one's; the first program fades in from the dry input
//   an instrument the old program's voices fade out over 20 ms while the new one plays from the next block, and the
//                 keys still held start their notes again on it
//
// Voices (an instrument). A note-on starts a voice on a free instance of the program (Patch::kInstances), a note-off
// stops it (Voice::stop), and a voice whose process returns false is free again. The voices run in calls cut at every
// absolute multiple of kGrid samples (and at each event), and a voice that has returned false, or faded to silence, is
// freed at the next such multiple: so which instance a later note gets, and the output, do not depend on how the
// host splits its blocks. At most Patch::kVoices sound at once:
// one more steals the oldest released voice, else the oldest: the Engine calls its stop() at once (a released voice had
// it), then fades it out over 2 ms (fsdsp::kStealFadeMs: the whole fade, whatever its process returns after that
// stop) on its own instance while the new note starts on another, so a key-up for it later finds no voice. A key pressed again while it still sounds stops its voice first. When every
// instance is still taken (fading voices too), the quietest voice is cut at once, its stop() first when it had none,
// and the new note starts on its instance. Each start() so gets exactly one stop() before that instance's next start():
// also a voice whose process returns false while its key is held (its stop() as it is freed), one dropped for going
// non-finite, and one still sounding when prepare() is called. A program swap is apart: the old program's voices fade
// out without a stop() (that program, its Shared with it, is going).
//
// The host (host.fm, Patch.h). setTransport() takes the host's transport once a block; the Engine keeps the song's
// position from there, sample by sample (its own clock at 120 BPM when the host says nothing), and fills the program's
// Host block before each call into it (start, stop, process, every part of a block), so a call reads the position of
// its own first sample. An instrument's controllers (the mod wheel, pitch bend, aftertouch, a racing wheel's: wheelcc)
// change at their events' offsets and stay until the next (restart() puts them at rest), and so does the sustain
// pedal, which the Engine applies itself: a key let go while it is down keeps its voice until the pedal comes up,
// which then stops it.
//
// Dials. setControls() takes the host's normalised values (P1 to P32) once a block. A real control glides to its new
// value over 20 ms, sample by sample, and while it moves the program reads it as a stream of those samples (a null
// stream, and the value, while it holds still: FMinus's flat copy of the method); an integer or a toggle is set once a
// block. A new program starts on the dials' values, without a glide.
//
// The dials' carry (Controls.h's DialCarry, Compiler.h). The host's values are normalised, and a new program's
// declarations (a range, a taper, an int's options) would give an old value another meaning; the processor writes the
// values the dials keep in their units only on the message thread, after the new program may already play. So
// setControls() also says whose controls the values are in (`meaning`, a program id; 0: the program's that plays), and
// a Patch carries how its dials come across: while the values are still in the controls the carry was made from
// (meaning == DialCarry::from), a slot it holds whose value is still the one it was made from (fromNorm, bit for bit)
// plays the carried plain value; a value that moved since (the processor's write, the host's automation, a hand on the
// dial) plays as it says, for good (until the next program); and once the values are in the program's own controls
// (meaning == its id) the carry is over. So no block plays an old normalised value through new declarations.
//
// Circuit dials (Patch.h, "Circuits"). A dial that drives a circuit's pot glides as a real dial does (20 ms), and a
// circuit's switch is an int dial; neither is an FMinus parameter: the Engine hands their values to the program through
// the shims compile() appended (Patch::setCircuits), each call every bound field's set() in one instance. It calls them
// on the absolute grid of kGrid samples (counted from prepare(), as the voices' parts are), with the dials' glided
// values at that sample: at every grid tick while a bound dial glides, through the first tick at or after it lands,
// and, after a bound switch changes, for ceil(0.02 rate / kGrid) + 1 ticks (its ramp: FMinus moves a switch a step a
// set() call over 20 ms). An instance that has not had the dials' values yet (a new program's, a voice's instance that
// missed calls while free) gets a call before its first process (an effect: at its block's first sample; a voice:
// before its start, or before its first part when its note came with the program); when a bound switch's dial is not
// where the instance's switch is (a fresh instance's at the netlist's default; one that missed a switch's change, or
// was let go during its ramp), as many calls in a row as the ramp takes, so it starts at the dial's position (it has
// played nothing yet: no step is heard). While the dials stand still, no call. The effect's process is cut at those
// grid ticks, and then only (a named exception to block-size invariance: while a circuit dial moves, the effect's calls
// end on the grid, so a program that keeps state per call may differ there, as it may where any dial moves); an
// instrument's voices already run in parts cut on the grid, and each voice of the program that plays gets its own
// calls. The old program of a swap gets none (its dials hold still). circuitCalls() counts the calls (the probes').
//
// The output guard. Whatever a program computes, what leaves the Engine is finite and within +-4 (+12 dBFS): a NaN or
// an infinity becomes 0, and a sample beyond is clipped. Either raises an alert (takeAlerts) the editor shows. A NaN in
// a program's state stays there: running the program again starts it from scratch.
//
// Meters. Each block the Engine reads the fields the program's displays show (Patch::tap: an effect's instance, an
// instrument's newest sounding voice, and the module Shared's one, whether a voice sounds or not; once a panel's edit
// gave the program others, setTaps' table, through Patch::tapOf) and publishes them (Meters) through a seqlock, which the editor reads at its frame
// rate. While someone reads them (readers(): an editor shown, which lets go while its window is minimised or hidden,
// Panel.h; a tool that listens; addReader), it also measures what came in (an effect's input) and what goes out (each
// side's peak and energy in lanes, Levels.h), and keeps the last kScopeLength samples of each for the scopes; with no
// reader the levels read 0 and the scopes stand still, which saves an effect about as much as a light program
// costs. A reader that comes finds the levels from the next block on (its first block's own) and the scopes silent
// before it. For a tool that listens (the MCP's listen) the meters also say, every block, what the host and the
// player do (the transport, the note-ons, the keys held, the voices and their steals, a voice dropped for going
// non-finite), and a ring of atomics keeps each block's cost and context (blocks()); a few scalar stores a block.
//
// Probes (the schematic's live probes; Patch.h's "Probes", Circuits.h). setProbes() (off the audio thread: the
// processor's, from the newest owner's set) leaves a ProbeSet in a seqlock and bumps a generation; at a block's start,
// when the generation or current_ moved (and each block for an instrument, whose target is its newest sounding voice,
// re-chosen as the taps' is), the audio thread applies it only when its program is current_'s: it writes the slots'
// nodes and prb_ into that one element of that one instance of current_ (Patch::armProbes), and prb_ = 0 into the one
// it armed before when the target moved within current_. It never writes into another Patch: a swap forgets the target
// (the old program keeps what it had while it fades), and so do prepare() and restart(). After each whole call (an
// effect's block of process calls, of current_ only during a crossfade) or each part of the followed voice (an
// instrument's), it reads how far prn_ moved (d, modulo 2^30), copies the newest min(d, kProbeRing) samples of each
// armed slot into its own rings of atomics (kScopeLength each; probe()), what is not finite as 0, and reads and zeroes
// the slots' peaks, sums and sums of squares, which publish() turns into Meters::probes (the peak falling 20 dB a
// second, the RMS over 50 ms, the mean over 200 ms; a block whose stat is not finite is not valid), with the set
// applied (probeSet, probeGen, probeProgram) and the instance followed. A call longer than kProbeRing (an effect's
// host block over 1,024 samples) leaves only its newest kProbeRing in the rings, so probe() counts the set's samples
// from there: a run of samples it gives is never two calls joined (the stats still take every sample). No call is
// ever cut for a probe: the output's bits are the same probed or not. With nothing armed the audio thread pays one
// generation load a block (and the set's few words in the meters), the module one branch a sample; armed, a delta
// read, at most 4 x kProbeRing ring copies and 12 stat reads and zeroes a call (an instrument's voices a look for the
// newest a block).
//
// A program that stops (Patch.h, "The loop budget", "Bounds checks"). A call that runs past its budget, or fails a
// bounds check, stops its Patch, which the Engine then calls no more, and it goes at once, fading, since nothing steps:
// an effect's output from the program's last good samples, held, to its input over 20 ms (smootherstep; the block's
// input is kept for it), then its input as it comes, as with no program; an instrument's voices of it, each from its
// last good samples, held, to silence over 20 ms (no stop(): the program plays no more), and no note starts on it.
// While the program it replaced is still there (an effect's 20 ms crossfade, an instrument's first block or its old
// voices fading) and the program was offered `revertible` (the compiler's RUN, until settle()), the Engine goes back to
// the one it replaced instead, which so plays on: an effect's crossfade turns back from where it is, the stopped side
// fading out under it; an instrument's held keys start again on it (its voices that were fading go on fading), with its
// dials going on from where they were for it: at a block's start as its glides were, mid-block from what its instances
// held since the swap (no snap, and no glide advanced twice in a block). stops() counts the stops of the program that
// plays, and lastStop() says the newest: the program, the call, the bounds check that failed (if one did), the sample
// (from prepare()) and whether the Engine went back. A stopped program plays no more, even across prepare() (restart()
// makes it as new: a render's).
//
// Flush-to-zero. process() runs in it (fsdsp::ScopedFtz), the mode the programs are compiled for, whoever calls it:
// the audio thread has it already; an offline render (Measure, the chat's tools) would otherwise make FMinus set and
// clear it around every call into the program.
//
// Frozen dials (Patch.h). While the dials hold still, the program's frozen copy may play: offerFrozen() leaves one in a
// slot, and the audio thread takes it at a block's start only when it is of the program that plays, no swap runs, and
// each dial stands exactly at the value the copy holds; it plays the same instances, so nothing changes but the
// cost. The block a dial moves (or another program comes) the program's own code plays again, and the copy goes back
// for collect() to destroy. dials() says what the freezer needs: the program, its dials' values, how long they have
// held still (a new program counts from its arrival), and whether the copy plays.
//
// Threads. prepare() while the audio thread is stopped (it allocates); setControls() and process() on the audio
// thread (no allocation, no lock, no I/O); offer() and collect() off it (Compiler.h), under a mutex prepare() shares.
// A program crosses by an atomic pointer each way: offer() leaves it in `pending_`, the audio thread takes it at the
// start of a block once the previous swap is over (and while holdPending does not hold it: the processor's A/B trim,
// for at most a 20 ms ramp), and leaves the old one in `retired_` for collect() to destroy.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Events.h"
#include "dsp/Meters.h"
#include "fminus/Patch.h"

#include "fsdsp/core/Ramp.h"
#include "fsdsp/core/Seqlock.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace playground
{
    // One block's cost and what went on in it (Engine::blocks): the share of its own length the audio thread took
    // (1 = a whole core), its length, the voices sounding and its Flag bits.
    struct BlockStat
    {
        enum Flag : uint8_t
        {
            swapping = 1u << 0,                       // a program swap's fade ran
            moving = 1u << 1,                         // a dial moved (glided)
            frozen = 1u << 2,                         // the frozen copy played
            notes = 1u << 3,                          // a note-on came
            alert = 1u << 4,                          // the output guard acted (or a voice was dropped)
            metering = 1u << 5,                       // the levels and the scopes were measured (a reader)
        };
        float share = 0.0f;
        uint16_t samples = 0;
        uint8_t voices = 0;
        uint8_t flags = 0;
    };

    // The dials as the freezer reads them (Engine::dials).
    struct DialState
    {
        uint64_t program = 0;                         // the program that plays (Patch::program), 0 for none
        double stillSeconds = 0.0;                    // since a dial last moved, or the program came
        bool frozen = false;                          // its frozen copy plays
        std::array<float, kMaxControls> plain{};      // each dial's value, plain (0 for an unused control)
    };

    class Engine
    {
    public:
        static constexpr int kScopeLength = 4096;     // samples the scopes keep (a power of two), of input and output
        enum Alert : uint32_t
        {
            nonFinite = 1u << 0,   // a NaN or an infinity, replaced by 0
            clipped = 1u << 1,     // a sample beyond +-4, clipped
        };

        Engine() = default;
        ~Engine();
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // While the audio thread is stopped. A program waiting is taken at once, without a fade, and the one playing
        // starts again at the new rate (its instances as new).
        void prepare(double sampleRate, int maxBlockSize);
        // Offline only (the audio thread stopped, as for prepare()): everything as new, the program that plays too
        // (Patch::reset: its instances and its module Shared's, end and init run), at the same rate and block. A
        // render after it is the same as one from a fresh Engine and a fresh compile (measure::Session renders one
        // compile many times so).
        void restart();

        // ---- the audio thread ------------------------------------------------------------------------------------
        // `meaning`: the program whose controls the values are in (above, "The dials' carry"); 0 for the one that plays.
        void setControls(const std::array<float, kMaxControls>& normalised, uint64_t meaning = 0) noexcept;
        // The host's transport at the next process call's first sample (once a host block, before its first call).
        void setTransport(const Transport& t) noexcept;
        // The Host block as the last call saw it (for the probes, while the audio thread is stopped).
        const HostBlock& host() const noexcept { return host_; }
        // The 7-bit values of a racing wheel's control changes (wheelcc::stateCc(i)'s, in that order) that bring a
        // fresh Engine where this one is (what a recording's replay starts from: plugin/CaptureReplay.cpp). The audio
        // thread's own.
        std::array<uint8_t, wheelcc::kStateCcs> wheelCcs() const noexcept;
        // At most prepare()'s maxBlockSize samples (the processor cuts a host's larger block); events sorted by offset.
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;
        // The program that plays (Patch::program; 0 for none), as of the last process call: the audio thread's own (the
        // processor's A/B trim reads it after each call), or offline.
        uint64_t playingProgram() const noexcept { return current_ != nullptr ? current_->program() : 0; }
        // A program going out still sounds (an effect's crossfade, an instrument's old voices fading). The audio thread's.
        bool outgoing() const noexcept { return old_ != nullptr || xfading_; }
        // The program offered and not taken yet (Patch::program; 0: none), any thread; and holding it there, the audio
        // thread's: while held, process() takes none (the processor's A/B trim turns the program that plays down to the
        // coming one's trim first, so that one never plays above it). prepare() takes one waiting, held or not.
        uint64_t pendingProgram() const noexcept { return pendingProgram_.load(std::memory_order_acquire); }
        void holdPending(bool hold) noexcept { holdPending_ = hold; }

        // ---- off the audio thread --------------------------------------------------------------------------------
        // Plays from the next block, as above. `revertible`: should it stop while the program it replaces is still
        // there, the Engine goes back to that one, until settle() (above, "A program that stops").
        void offer(std::unique_ptr<Patch> patch, bool revertible = false);
        // The compiler's: `program`, offered revertible, plays on, and the Engine never goes back from it now. False
        // when it went back from it already.
        bool settle(uint64_t program) noexcept;
        uint64_t revertible() const noexcept { return revertible_.load(std::memory_order_acquire); }
        void offerFrozen(std::unique_ptr<Patch::Frozen> frozen);   // taken at a block's start if it fits (above)
        void collect();                               // destroys the programs (and copies) the audio thread handed back
        // A panel's edit (Compiler.h, "A panel's edit"; above, "Meters"): the fields program `program`'s displays read
        // from now on, in place of the taps it was compiled with (Patch::taps), until another program plays; one
        // writer (the compiler's thread). Read at the end of the next block, as the probes' set is; a table of another
        // program than the one that plays reads nothing, and that program its own taps. Its count (from 1) is returned,
        // and tapsRead() says the newest the audio thread has read (after the block's meters are published); each
        // frame names the table it was read by (Meters::tapTable: this count, or 0 for the program's own taps).
        struct TapTable
        {
            uint64_t program = 0;
            uint32_t count = 0;                       // taps used, at most kMaxDisplays
            uint32_t spare = 0;
            std::array<Patch::Tap, kMaxDisplays> taps{};
        };
        uint64_t setTaps(const TapTable& table) noexcept;
        uint64_t tapsRead() const noexcept { return tapsRead_.load(std::memory_order_acquire); }

        // ---- any thread ------------------------------------------------------------------------------------------
        // A program's stop (above, "A program that stops").
        struct Stopped
        {
            uint64_t program = 0;                     // its id (Patch::program)
            Patch::Method call = Patch::Method::none;     // the call that ran past its budget, or failed a bounds check
            uint64_t at = 0;                          // the sample (from prepare()) its block or voice's part began at
            bool reverted = false;                    // the Engine went back to the program it replaced
            Patch::Bounds bounds;                     // the bounds check that failed (Patch::bounds), or none
        };
        uint64_t stops() const noexcept { return stops_.load(std::memory_order_acquire); }
        bool lastStop(Stopped& out) const noexcept { return stopped_.read(out); }   // false while published: again
        // The program that plays with no swap under way as of the last block (0: none, or a swap), when the last block
        // began and ended (the steady clock's nanoseconds: a start after the end is a block under way, which a call that
        // runs past its budget makes long) and how long it was (seconds): what the compiler waits on.
        uint64_t settledProgram() const noexcept { return settled_.load(std::memory_order_acquire); }
        int64_t lastBlockStartNs() const noexcept { return blockStartNs_.load(std::memory_order_acquire); }
        int64_t lastBlockNs() const noexcept { return blockNs_.load(std::memory_order_acquire); }
        double lastBlockSeconds() const noexcept { return blockSeconds_.load(std::memory_order_relaxed); }
        // prepare()'s maxBlockSize: the most samples of a block, so of an effect's call (its budget: Patch::budgetOf).
        int largestBlock() const noexcept { return largestBlock_.load(std::memory_order_relaxed); }

        float rate() const noexcept { return rate_.load(std::memory_order_relaxed); }
        double tailSeconds() const noexcept { return 5.0; }
        // The busiest recent block's processing time over the block's own length (1 = a whole core), decaying.
        float load() const noexcept { return load_.load(std::memory_order_relaxed); }
        uint32_t takeAlerts() noexcept { return alerts_.exchange(0, std::memory_order_relaxed); }
        // The dials as of the last block; false while the audio thread publishes (read again).
        bool dials(DialState& out) const noexcept { return dials_.read(out); }
        bool frozen() const noexcept { return frozenNow_.load(std::memory_order_relaxed); }   // the copy plays
        // While on (a recording armed, plugin/Capture.h), the program's own code plays: a frozen copy playing is
        // dropped at the next block, one offered goes back, and the Compiler's freezer waits; a replay of the
        // recording then gives its bits (the copy's Math::Fast would not).
        void setOwnCode(bool on) noexcept { ownCode_.store(on, std::memory_order_relaxed); }
        bool ownCode() const noexcept { return ownCode_.load(std::memory_order_relaxed); }
        // The output guard's calls that raised an alert, counted and never cleared (the low 32 bits: a NaN or an
        // infinity replaced; the high: a sample clipped). Reading it clears nothing, unlike takeAlerts.
        uint64_t alertTally() const noexcept { return alertTally_.load(std::memory_order_relaxed); }
        // The last block's meters; false while the audio thread publishes (keep the frame before).
        bool meters(Meters& out) const noexcept { return meters_.read(out); }
        // The probes (above, "Probes"): the set the audio thread applies from its next block (one writer: the
        // processor's message thread), and the last n samples (at most kScopeLength) of slot `slot`'s ring, oldest
        // first: how many of them are of the set that plays, in one run (the older are 0; since a call longer than
        // kProbeRing, at most its newest kProbeRing); 0 for a slot not armed.
        void setProbes(const ProbeSet& set) noexcept;
        int probe(int slot, float* out, int n) const noexcept;
        uint64_t probeGeneration() const noexcept { return probeGenIn_.load(std::memory_order_relaxed); }
        // The samples copied into the probes' rings so far (they stand still while nothing is armed): the probes'.
        uint32_t probeSamples() const noexcept { return static_cast<uint32_t>(probeAt_.load(std::memory_order_acquire)); }
        // The last n samples (at most kScopeLength) of the input or the output, mono, oldest first.
        void scope(bool input, float* out, int n) const noexcept;
        // Who reads the meters and the scopes (above): each reader adds itself while it reads, and removes itself after.
        void addReader() noexcept { readers_.fetch_add(1, std::memory_order_relaxed); }
        void removeReader() noexcept { readers_.fetch_sub(1, std::memory_order_relaxed); }
        int readers() const noexcept { return readers_.load(std::memory_order_relaxed); }
        // The blocks' costs (BlockStat), the last kBlockRing kept: those from block `from` on (blocks counted from the
        // Engine's making), oldest first, at most `max` into `out`; how many, and in `next` where to read from next.
        static constexpr int kBlockRing = 512;
        int blocks(uint64_t from, BlockStat* out, int max, uint64_t& next) const noexcept;

        // For the probes, while the audio thread is stopped.
        bool playing() const noexcept { return current_ != nullptr || pending_.load() != nullptr; }
        bool swapping() const noexcept { return old_ != nullptr || pending_.load() != nullptr; }
        int activeVoices() const noexcept;
        // Offline only, after restart() and before the first process(): the clock's start, so that the voices' grid
        // (kGrid) has a recording's phase (a replay of the capture, Measure.h's Request::clock).
        void setClock(uint64_t samples) noexcept { clock_ = samples; }
        // The circuit dials' calls made since the Engine was made (above, "Circuit dials"): each one an instance's set()
        // of every bound field.
        uint64_t circuitCalls() const noexcept { return circuitCalls_; }
        // The song's position at the next call's first sample (the audio thread's own: a recording keeps it where the
        // host gives no transport and the Engine's own runs at 120 BPM), and, offline only, where it starts (after
        // restart(), which sets it to 0: a replay of such a recording, Measure.h).
        double beat() const noexcept { return beat_; }
        void setBeat(double beat) noexcept { beat_ = beat; }

        // Offline only (a Session's Engine, Measure.h; never the one that plays), between process calls: for a tool
        // that watches the program as it plays (dsp/Debug.h). The program that plays, and the voices that sound (an
        // instrument's): each one's instance, key, when it started (a count of note-ons), and whether it is released,
        // fading (stolen) or ending (its process returned false: free at the next multiple of kGrid).
        const Patch* playingPatch() const noexcept { return current_; }
        struct VoiceInfo
        {
            int instance = 0;
            int key = -1;
            uint64_t age = 0;
            bool released = false, fading = false, ending = false;
        };
        void voices(std::vector<VoiceInfo>& out) const;

    private:
        // A value that moves to its target over a fixed time, linearly, landing on it exactly.
        struct Glide
        {
            float cur = 0.0f, tgt = 0.0f, inc = 0.0f;
            int left = 0;

            void set(float target, int length) noexcept;
            float tick() noexcept;
            bool moving() const noexcept { return left > 0; }
        };

        static constexpr int kGrid = 16;                  // the voices' calls end on multiples of it (absolute)
        static constexpr uint64_t kNever = ~uint64_t{ 0 };
        static constexpr uint64_t kReverted = uint64_t{ 1 } << 63;   // revertible_: the Engine went back from the id

        struct Voice
        {
            Patch* patch = nullptr;     // null: free
            int instance = 0;
            int key = -1;
            uint64_t age = 0;           // when it started (a count of note-ons)
            bool released = false;
            bool ending = false;        // its process returned false: free at the next multiple of kGrid
            bool fading = false;        // stolen (released: its stop() came first), or its program swapped out: gain falls
                                    // to 0, then the voice is free
            bool pedal = false;         // its key let go while the sustain pedal was down: stopped when it comes up
            bool exiting = false;       // its program stopped: its last samples, held, fade out (above)
            float gain = 1.0f, fall = 0.0f;   // per sample, while fading
            float lastL = 0.0f, lastR = 0.0f; // its last part's last samples (finite)
            uint64_t fed = 0;           // the feedGen_ its instance's dials were last written at
            uint64_t calledAt = kNever; // the absolute sample of its last circuit call
        };

        // An effect's side of a program that may stop (above): its last good samples, and its exit (the fade from them
        // to the input, samples left), current_'s and old_'s.
        struct Exit
        {
            float l = 0.0f, r = 0.0f;
            int left = 0;
            bool on = false;
        };

        void takePending() noexcept;
        void retire(Patch*& p) noexcept;              // hands p back (or keeps it until the slot is empty)
        void updateFrozen() noexcept;                 // takes, keeps or drops the frozen copy; publishes dials()
        bool fits(const Patch::Frozen& f) const noexcept;
        bool holdFrozen(Patch::Frozen* f) noexcept;   // to be handed back; false when every slot is taken
        void retireFrozen() noexcept;                 // the held copies into the slots collect() empties
        // The ramps over the block from its sample `from`: streamBuf_, streaming_, feedGen_.
        void glideControls(int n, int from = 0) noexcept;
        void feed(Patch& p, int offset, int instance) noexcept;   // the dials into p: streams from `offset`
        // The circuit dials (above): current_'s shims in `instance` with the dials' values at the block's sample `at`
        // (at < 0: their values after the last block, for a block of 0 samples).
        void callCircuits(int at, int instance) noexcept;
        // An instance's first calls (a new program's, one that missed calls): one, and as many as a switch's ramp takes
        // when a bound switch is not where the instance's last calls left it; the instance's values current.
        void firstCalls(int at, int instance) noexcept;
        // The effect's program over the block: its first circuit call, then its process cut at the grid ticks that call.
        void playEffect(float* left, float* right, int n) noexcept;
        // A program that stops (above): current_ over the block in place (playEffect), or its exit once it has stopped;
        // old_'s, on the copies of the input; an exit's fade from its held samples to the block's input (inL_, inR_).
        void runEffect(float* left, float* right, int n) noexcept;
        void runOldEffect(float* left, float* right, int n) noexcept;
        void exitEffect(Exit& e, float* left, float* right, int n) noexcept;
        // An instrument's voice of a stopped program over [from, from + len) of the block: its held samples, fading.
        void exitVoice(Voice& v, int from, int len, bool onGrid) noexcept;
        // current_ has stopped: back to old_ when allowed (an instrument's held keys start again on it, their calls
        // from the block's sample `resume`; -1: at the block's start, before its glides), else the stop published; at
        // the block's sample `at`. revert() is the going back alone; resumeGlides() an instrument's dials for the
        // program gone back to after the block's glides: from what its instances hold since the swap (heldDials_).
        void stopCheck(int at, int resume) noexcept;
        bool revert(int at) noexcept;
        void resumeGlides(int from) noexcept;
        void noteStop(int at, bool reverted) noexcept;
        // A voice of current_ let go at the absolute sample `at`: its instance missed calls if a window was open.
        void freeVoice(Voice& v, uint64_t at) noexcept;
        // A voice of current_ that has had no stop() since its start gets it now, and is released (so its key-up, the
        // pedal's going up or a second steal calls none): a steal, a cut, a drop, its end while held, prepare().
        void stopVoice(Voice& v) noexcept;
        static uint64_t gridCeil(uint64_t t) noexcept { return (t + kGrid - 1) / kGrid * kGrid; }
        void processEffect(float* const* channels, int numChannels, int n) noexcept;
        void syncHost(int offset) noexcept;           // the Host block at the call's sample `offset`, into the programs
        void controller(const Event& e) noexcept;     // a controller, pitch bend or pressure event
        void wheel(int cc, int value) noexcept;       // a racing wheel's control change (wheelcc), its 7-bit value
        void processInstrument(float* const* channels, int numChannels, int n, std::span<const Event> events) noexcept;
        void noteOn(int key, float velocity) noexcept;
        void noteOff(int key) noexcept;
        void allNotesOff() noexcept;
        void render(int from, int to) noexcept;       // every voice over [from, to) of the block, into acc
        void renderPart(int from, int to, bool onGrid) noexcept;   // one call a voice; to is a multiple of kGrid?
        int freeInstance(const Patch& p) const noexcept;
        void guard(float* x, int n) noexcept;
        // The block's levels and samples (before and after the program; while `metering`, `fresh` its first block),
        // the program's taps, published.
        void measureIn(float* const* channels, int numChannels, int n, bool fresh) noexcept;
        void publish(float* const* channels, int numChannels, int n, bool metering, bool fresh) noexcept;
        void heard(const Event& e) noexcept;          // a note-on of the player's, counted (Meters::noteOns)
        // The probes (above): the set applied at a block's start (the target armed, the one before disarmed), the target
        // forgotten (a swap; prepare(), which disarms it first while the audio thread is stopped), what the target's
        // call wrote taken in, and the block's stats into the frame.
        void applyProbes() noexcept;
        void forgetProbes() noexcept;
        void takeProbes() noexcept;
        void publishProbes(int n) noexcept;
        void blockDone(float share, int n) noexcept;  // the block's BlockStat, into the ring

        std::atomic<Patch::Frozen*> frozenPending_{ nullptr };
        std::array<std::atomic<Patch::Frozen*>, 4> frozenRetired_{};
        Patch::Frozen* frozen_ = nullptr;             // the copy of current_ that may play (the audio thread's)
        std::array<Patch::Frozen*, 4> frozenHold_{};  // copies done with, waiting for a retired slot
        std::atomic<bool> frozenNow_{ false };
        std::atomic<bool> ownCode_{ false };          // setOwnCode
        bool moved_ = false;                          // a dial moved in this block (glideControls)
        uint64_t samples_ = 0;                        // processed since prepare()
        uint64_t lastMove_ = 0;                       // samples_ when a dial last moved or the program came
        const Patch* stillOf_ = nullptr;              // the program lastMove_ counts for
        DialState dialState_{};
        fsdsp::Seqlock<DialState> dials_;

        std::mutex mutex_;                            // offer, collect, prepare; never the audio thread
        std::atomic<Patch*> pending_{ nullptr };
        std::atomic<uint64_t> pendingProgram_{ 0 };   // pending_'s program (offer() writes it first)
        bool holdPending_ = false;                    // the audio thread's (holdPending)
        std::atomic<Patch*> retired_{ nullptr };
        Patch* current_ = nullptr;                    // the audio thread's
        Patch* old_ = nullptr;                        // fading out, or waiting for retired_ to empty
        Patch* hold_ = nullptr;                       // done, waiting for retired_ to empty

        float fs_ = 48000.0f;
        std::atomic<float> rate_{ 48000.0f };
        int maxBlock_ = 0;
        fsdsp::SmoothRamp xfade_{};                   // an effect's new program's weight, 0 to 1
        bool xfading_ = false;

        std::array<float, kMaxControls> norm_{};
        bool normSet_ = false;                        // setControls has run since prepare()
        uint64_t meaning_ = 0;                        // setControls': whose controls norm_ is in (0: current_'s)
        uint32_t carryOver_ = 0;                      // the slots whose carry ended (bit c), for current_
        std::array<Glide, kMaxControls> glide_{};
        std::array<bool, kMaxControls> streaming_{};
        const Patch* fedPatch_ = nullptr;             // the program the glides were last aimed for
        std::vector<float> streamBuf_;                // kMaxControls x maxBlock_
        // What feed() writes changes only when a dial snaps or moves, or a stream starts or ends: glideControls bumps
        // feedGen_ then, and an instance fed at this generation (Voice::fed, effectFed_) is not fed again while no dial
        // streams. A stream's pointer moves every call, so while one streams every call feeds.
        uint64_t feedGen_ = 1;
        uint64_t effectFed_ = 0;                      // the effect's instance's
        bool streamingNow_ = false;                   // a dial streams in this block
        bool streamedLast_ = false;                   // one streamed in the block before

        // The circuit dials' calls (above): the ticks of a window after a switch changes, the window open now (calls at
        // absolute grid ticks below circuitUntil_), the bound values' generation (bumps when one changes), the
        // generation each instance had its values at (0: none yet, or calls missed), the effect's last call, the values a
        // call passes, the block's sample a note-on's call is at, the count.
        int circuitTicks_ = 61;
        uint64_t circuitUntil_ = 0;
        uint64_t circuitGen_ = 1;
        std::array<uint64_t, Patch::kInstances> instanceGen_{};
        // The bound switches' positions, so that only an instance whose switches are elsewhere gets a ramp's worth of
        // first calls: their generation (0 while every bound switch is at its default, where init leaves an instance;
        // else a new one at each change), the end of the window their ramp takes (absolute samples below switchUntil_),
        // and each instance's: the generation its last calls gave it (0 for a fresh program's; kSwitchesUnknown for an
        // instance kept across prepare(), or let go while a ramp's window was open).
        static constexpr uint64_t kSwitchesUnknown = ~uint64_t{ 0 };
        uint64_t switchGen_ = 0;
        uint64_t switchCount_ = 0;
        uint64_t switchUntil_ = 0;
        std::array<uint64_t, Patch::kInstances> instanceSwitch_{};
        uint64_t effectCalledAt_ = kNever;
        std::array<float, kMaxControls> circuitPlain_{};
        int callAt_ = 0;                              // -1: a block of 0 samples (the dials as the last block left them)
        uint64_t circuitCalls_ = 0;

        std::array<Voice, 2 * Patch::kInstances> voices_{};
        std::array<bool, 128> held_{};
        std::array<float, 128> heldVelocity_{};
        uint64_t noteCount_ = 0;
        uint64_t clock_ = 0;                          // samples processed since prepare(): the grid's origin
        HostBlock host_;                              // what the programs' Host block holds (host.fm)
        std::array<uint16_t, wheelcc::kAxes> wheel14_{ { wheelcc::kCentre, 0, 0, 0 } };   // a racing wheel's 14-bit
                                                      // values as its MSBs and LSBs left them (controller)
        double beat_ = 0.0;                           // the song's position at this call's first sample
        double bpm_ = 120.0;
        bool running_ = true;                         // the position moves (the host plays, or it said nothing)
        int stealFade_ = 96, swapFade_ = 960;         // samples (2 ms and 20 ms at the rate)

        std::vector<float> accL_, accR_, voiceL_, voiceR_, dryL_, dryR_, mono_;
        std::vector<float> inL_, inR_;                // an effect's block's input, kept (above, "A program that stops")

        // A program that stops (above): the effect's sides, whether current_'s stop is published, the block's length
        // (a revert's dials), the stops published, the program that may be gone back from (offer's `revertible`), and
        // what the compiler waits on.
        Exit exitCur_{}, exitOld_{};
        bool stopNoted_ = false;
        int blockN_ = 0;
        // An instrument's old program's dials as its instances hold them since the swap (each glide's target: the swap
        // took their streams), and its carry's ended slots, when the glides were aimed for it (heldFor_): what a revert
        // after the block's glides goes on from.
        std::array<float, kMaxControls> heldDials_{};
        uint32_t heldCarryOver_ = 0;
        const Patch* heldFor_ = nullptr;
        std::atomic<int> largestBlock_{ 0 };
        fsdsp::Seqlock<Stopped> stopped_;
        std::atomic<uint64_t> stops_{ 0 };
        std::atomic<uint64_t> revertible_{ 0 };
        std::atomic<uint64_t> settled_{ 0 };
        std::atomic<int64_t> blockStartNs_{ 0 }, blockNs_{ 0 };
        std::atomic<double> blockSeconds_{ 0.0 };

        std::atomic<float> load_{ 0.0f };
        std::atomic<uint32_t> alerts_{ 0 };
        std::atomic<uint64_t> alertTally_{ 0 };       // alertTally (the audio thread's, the only writer)

        fsdsp::Seqlock<Meters> meters_;
        Meters frame_{};                              // the audio thread's, built over a block
        std::array<std::atomic<float>, kScopeLength> scopeIn_{}, scopeOut_{};
        std::atomic<uint32_t> scopeAt_{ 0 };          // samples written so far
        std::atomic<int> readers_{ 0 };               // who reads the meters and the scopes (addReader)
        bool metering_ = false;                       // the audio thread measured the last block
        bool known_ = false;                          // the host's transport is known (setTransport)
        uint8_t blockFlags_ = 0;                      // the block's BlockStat flags so far
        // The blocks' costs, packed (the share's bits, the length, the voices, the flags), as the scopes' rings are.
        std::array<std::atomic<uint64_t>, kBlockRing> blockRing_{};
        std::atomic<uint64_t> blockAt_{ 0 };          // blocks written so far

        // The probes (above): the set (setProbes' seqlock and its generation), the set as the audio thread last read it
        // and the generation it read, the target armed (always current_: its field, element and instance, -1 for none;
        // the slots' nodes it was armed with and the slots armed), prn_ as last read, the block's stats so far (the
        // samples taken, each slot's peak, sum, sum of squares, all finite), the stats' first block, whether the meters'
        // slots hold numbers, the rings (the samples copied so far, and where the run of the set that plays began:
        // probe()'s), the slots armed for probe().
        fsdsp::Seqlock<ProbeSet> probeSet_;
        std::atomic<uint64_t> probeGenIn_{ 0 };
        ProbeSet probeWant_{};
        uint64_t probeSeen_ = 0;
        const Patch* probePatch_ = nullptr;
        int probeCircuit_ = -1, probeElement_ = 0, probeInstance_ = -1;
        std::array<int16_t, 4> probeNodes_{ -1, -1, -1, -1 };
        uint8_t probeSlots_ = 0;
        uint32_t probeLast_ = 0;
        uint32_t probeTaken_ = 0;
        std::array<float, 4> probePeak_{};
        std::array<double, 4> probeSum_{}, probeSquares_{};
        std::array<bool, 4> probeFinite_{};
        bool probeFresh_ = true;
        bool probeMetered_ = false;
        std::array<std::array<std::atomic<float>, kScopeLength>, 4> probeRings_{};
        std::atomic<uint64_t> probeAt_{ 0 }, probeFrom_{ 0 };
        std::atomic<uint8_t> probeMask_{ 0 };

        // A panel's taps (setTaps): the table and its count, the table as the audio thread last read it and the count
        // it read, published after the block's meters.
        fsdsp::Seqlock<TapTable> tapSet_;
        std::atomic<uint64_t> tapGenIn_{ 0 };
        TapTable tapWant_{};
        uint64_t tapSeen_ = 0;
        std::atomic<uint64_t> tapsRead_{ 0 };
    };
}
