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
// one more steals the oldest released voice, else the oldest, fading it out over 2 ms (fsdsp::kStealFadeMs) on its own
// instance while the new note starts on another. A key pressed again while it still sounds stops its voice first.
//
// The host (host.fm, Patch.h). setTransport() takes the host's transport once a block; the Engine keeps the song's
// position from there, sample by sample (its own clock at 120 BPM when the host says nothing), and fills the program's
// Host block before each call into it (start, stop, process, every part of a block), so a call reads the position of
// its own first sample. An instrument's controllers (the mod wheel, pitch bend, aftertouch) change at their events'
// offsets, and so does the sustain pedal, which the Engine applies itself: a key let go while it is down keeps its
// voice until the pedal comes up, which then stops it.
//
// Dials. setControls() takes the host's normalised values (P1 to P32) once a block. A real control glides to its new
// value over 20 ms, sample by sample, and while it moves the program reads it as a stream of those samples (a null
// stream, and the value, while it holds still: FMinus's flat copy of the method); an integer or a toggle is set once a
// block. A new program starts on the dials' values, without a glide.
//
// The output guard. Whatever a program computes, what leaves the Engine is finite and within +-4 (+12 dBFS): a NaN or
// an infinity becomes 0, and a sample beyond is clipped. Either raises an alert (takeAlerts) the editor shows. A NaN in
// a program's state stays there: running the program again starts it from scratch.
//
// Meters. Each block the Engine reads the fields the program's displays show (Patch::tap: an effect's instance, an
// instrument's newest sounding voice) and publishes them (Meters) through a seqlock, which the editor reads at its frame
// rate. While someone reads them (readers(): an editor open, a tool that listens; addReader), it also measures what
// came in (an effect's input) and what goes out, and keeps the last kScopeLength samples of each for the scopes; with
// no reader the levels read 0 and the scopes stand still, which saves an effect about as much as a light program
// costs. A reader that comes finds the levels from the next block on (its first block's own) and the scopes silent
// before it. For a tool that listens (the MCP's listen) the meters also say, every block, what the host and the
// player do (the transport, the note-ons, the keys held, the voices and their steals, a voice dropped for going
// non-finite), and a ring of atomics keeps each block's cost and context (blocks()); a few scalar stores a block.
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
// start of a block once the previous swap is over, and leaves the old one in `retired_` for collect() to destroy.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Events.h"
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
    // What the editor's displays show, as of the last block.
    struct Meters
    {
        uint32_t frame = 0;                           // bumps with each block
        float inPeak[2] = {}, inRms[2] = {};          // the input (an effect's), left and right, linear: the peak
                                                      // falls 20 dB a second, the RMS is smoothed over 50 ms (0
                                                      // while no one reads them: Engine::readers)
        float outPeak[2] = {}, outRms[2] = {};        // the output
        float taps[kMaxDisplays] = {};                // the fields the program's displays show (PanelDisplay::tap)
        uint64_t program = 0;                         // the program they are of (Patch::program), 0 while none plays
        float rate = 48000.0f;                        // the sample rate (a spectrum's frequencies)
        // The player and the host, as of the last block, for a tool that listens (whoever reads; the counts run on
        // from the Engine's making, so a reader takes differences):
        uint64_t samples = 0;                         // processed so far
        HostBlock host{};                             // the Host block: the tempo, the position at the block's first
                                                      // sample, the signature, playing; an instrument's wheels and pedal
        bool hostKnown = false;                       // the host says where it is (Transport::known), else our own clock
        uint8_t voices = 0;                           // an instrument's voices sounding (fading ones aside), of them:
        uint8_t released = 0, pedalHeld = 0;          // released, and let go while the pedal holds them
        uint8_t fading = 0;                           // voices fading out (stolen, or of a program swapped out)
        uint8_t held = 0;                             // keys down
        int8_t lastNote = -1;                         // the last note-on's key (-1: none yet)
        float lastVelocity = 0.0f;
        uint32_t heldKeys[4] = {};                    // the keys down, a bit each (key k: word k / 32, bit k % 32)
        uint32_t noteOns = 0;                         // the player's note-ons (not the held keys a new program starts)
        uint32_t steals = 0;                          // voices stolen for a note (with every voice taken)
        uint32_t dropped = 0;                         // voices dropped for going non-finite
        int32_t droppedKey = -1;                      // the last one's key
        uint64_t droppedAt = 0;                       // and its sample (as `samples` counts them)
    };

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
        void setControls(const std::array<float, kMaxControls>& normalised) noexcept;
        // The host's transport at the next process call's first sample (once a host block, before its first call).
        void setTransport(const Transport& t) noexcept;
        // The Host block as the last call saw it (for the probes, while the audio thread is stopped).
        const HostBlock& host() const noexcept { return host_; }
        // At most prepare()'s maxBlockSize samples (the processor cuts a host's larger block); events sorted by offset.
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        // ---- off the audio thread --------------------------------------------------------------------------------
        void offer(std::unique_ptr<Patch> patch);     // plays from the next block, as above
        void offerFrozen(std::unique_ptr<Patch::Frozen> frozen);   // taken at a block's start if it fits (above)
        void collect();                               // destroys the programs (and copies) the audio thread handed back

        // ---- any thread ------------------------------------------------------------------------------------------
        float rate() const noexcept { return rate_.load(std::memory_order_relaxed); }
        double tailSeconds() const noexcept { return 5.0; }
        // The busiest recent block's processing time over the block's own length (1 = a whole core), decaying.
        float load() const noexcept { return load_.load(std::memory_order_relaxed); }
        uint32_t takeAlerts() noexcept { return alerts_.exchange(0, std::memory_order_relaxed); }
        // The dials as of the last block; false while the audio thread publishes (read again).
        bool dials(DialState& out) const noexcept { return dials_.read(out); }
        bool frozen() const noexcept { return frozenNow_.load(std::memory_order_relaxed); }   // the copy plays
        // The last block's meters; false while the audio thread publishes (keep the frame before).
        bool meters(Meters& out) const noexcept { return meters_.read(out); }
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

        struct Voice
        {
            Patch* patch = nullptr;     // null: free
            int instance = 0;
            int key = -1;
            uint64_t age = 0;           // when it started (a count of note-ons)
            bool released = false;
            bool ending = false;        // its process returned false: free at the next multiple of kGrid
            bool fading = false;        // stolen, or its program swapped out: gain falls to 0, then the voice is free
            bool pedal = false;         // its key let go while the sustain pedal was down: stopped when it comes up
            float gain = 1.0f, fall = 0.0f;   // per sample, while fading
            uint64_t fed = 0;           // the feedGen_ its instance's dials were last written at
        };

        void takePending() noexcept;
        void retire(Patch*& p) noexcept;              // hands p back (or keeps it until the slot is empty)
        void updateFrozen() noexcept;                 // takes, keeps or drops the frozen copy; publishes dials()
        bool fits(const Patch::Frozen& f) const noexcept;
        bool holdFrozen(Patch::Frozen* f) noexcept;   // to be handed back; false when every slot is taken
        void retireFrozen() noexcept;                 // the held copies into the slots collect() empties
        void glideControls(int n) noexcept;           // the ramps over the block: streamBuf_, streaming_, feedGen_
        void feed(Patch& p, int offset, int instance) noexcept;   // the dials into p: streams from `offset`
        void processEffect(float* const* channels, int numChannels, int n) noexcept;
        void syncHost(int offset) noexcept;           // the Host block at the call's sample `offset`, into the programs
        void controller(const Event& e) noexcept;     // a controller, pitch bend or pressure event
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
        void blockDone(float share, int n) noexcept;  // the block's BlockStat, into the ring

        std::atomic<Patch::Frozen*> frozenPending_{ nullptr };
        std::array<std::atomic<Patch::Frozen*>, 4> frozenRetired_{};
        Patch::Frozen* frozen_ = nullptr;             // the copy of current_ that may play (the audio thread's)
        std::array<Patch::Frozen*, 4> frozenHold_{};  // copies done with, waiting for a retired slot
        std::atomic<bool> frozenNow_{ false };
        bool moved_ = false;                          // a dial moved in this block (glideControls)
        uint64_t samples_ = 0;                        // processed since prepare()
        uint64_t lastMove_ = 0;                       // samples_ when a dial last moved or the program came
        const Patch* stillOf_ = nullptr;              // the program lastMove_ counts for
        DialState dialState_{};
        fsdsp::Seqlock<DialState> dials_;

        std::mutex mutex_;                            // offer, collect, prepare; never the audio thread
        std::atomic<Patch*> pending_{ nullptr };
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

        std::array<Voice, 2 * Patch::kInstances> voices_{};
        std::array<bool, 128> held_{};
        std::array<float, 128> heldVelocity_{};
        uint64_t noteCount_ = 0;
        uint64_t clock_ = 0;                          // samples processed since prepare(): the grid's origin
        HostBlock host_;                              // what the programs' Host block holds (host.fm)
        double beat_ = 0.0;                           // the song's position at this call's first sample
        double bpm_ = 120.0;
        bool running_ = true;                         // the position moves (the host plays, or it said nothing)
        int stealFade_ = 96, swapFade_ = 960;         // samples (2 ms and 20 ms at the rate)

        std::vector<float> accL_, accR_, voiceL_, voiceR_, dryL_, dryR_, mono_;
        std::atomic<float> load_{ 0.0f };
        std::atomic<uint32_t> alerts_{ 0 };

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
    };
}
