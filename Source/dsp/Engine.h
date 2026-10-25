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
// Dials. setControls() takes the host's normalised values (P1 to P16) once a block. A real control glides to its new
// value over 20 ms, sample by sample, and while it moves the program reads it as a stream of those samples (a null
// stream, and the value, while it holds still: FMinus's flat copy of the method); an integer or a toggle is set once a
// block. A new program starts on the dials' values, without a glide.
//
// The output guard. Whatever a program computes, what leaves the Engine is finite and within +-4 (+12 dBFS): a NaN or
// an infinity becomes 0, and a sample beyond is clipped. Either raises an alert (takeAlerts) the editor shows. A NaN in
// a program's state stays there: running the program again starts it from scratch.
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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace testingplayground
{
    class Engine
    {
    public:
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

        // ---- the audio thread ------------------------------------------------------------------------------------
        void setControls(const std::array<float, kMaxControls>& normalised) noexcept;
        // At most prepare()'s maxBlockSize samples (the processor cuts a host's larger block); events sorted by offset.
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        // ---- off the audio thread --------------------------------------------------------------------------------
        void offer(std::unique_ptr<Patch> patch);     // plays from the next block, as above
        void collect();                               // destroys the programs the audio thread handed back

        // ---- any thread ------------------------------------------------------------------------------------------
        float rate() const noexcept { return rate_.load(std::memory_order_relaxed); }
        double tailSeconds() const noexcept { return 5.0; }
        // The busiest recent block's processing time over the block's own length (1 = a whole core), decaying.
        float load() const noexcept { return load_.load(std::memory_order_relaxed); }
        uint32_t takeAlerts() noexcept { return alerts_.exchange(0, std::memory_order_relaxed); }

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
            float gain = 1.0f, fall = 0.0f;   // per sample, while fading
        };

        void takePending() noexcept;
        void retire(Patch*& p) noexcept;              // hands p back (or keeps it until the slot is empty)
        void glideControls(int n) noexcept;           // the ramps over the block: streamBuf_, streaming_
        void feed(Patch& p, int offset, int instance) noexcept;   // the dials into p: streams from `offset`
        void processEffect(float* const* channels, int numChannels, int n) noexcept;
        void processInstrument(float* const* channels, int numChannels, int n, std::span<const Event> events) noexcept;
        void noteOn(int key, float velocity) noexcept;
        void noteOff(int key) noexcept;
        void allNotesOff() noexcept;
        void render(int from, int to) noexcept;       // every voice over [from, to) of the block, into acc
        void renderPart(int from, int to, bool onGrid) noexcept;   // one call a voice; to is a multiple of kGrid?
        int freeInstance(const Patch& p) const noexcept;
        void guard(float* x, int n) noexcept;

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

        std::array<Voice, 2 * Patch::kInstances> voices_{};
        std::array<bool, 128> held_{};
        std::array<float, 128> heldVelocity_{};
        uint64_t noteCount_ = 0;
        uint64_t clock_ = 0;                          // samples processed since prepare(): the grid's origin
        int stealFade_ = 96, swapFade_ = 960;         // samples (2 ms and 20 ms at the rate)

        std::vector<float> accL_, accR_, voiceL_, voiceR_, dryL_, dryR_, mono_;
        std::atomic<float> load_{ 0.0f };
        std::atomic<uint32_t> alerts_{ 0 };
    };
}
