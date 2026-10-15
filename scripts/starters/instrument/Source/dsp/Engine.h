// Source/dsp/Engine.h: the instrument, JUCE-free. Eight voices in two quads of four SIMD lanes (fsdsp::simd), each a
// phase accumulator (fsdsp::phase: a uint32 cycle, exact pitch, no drift) through a sine or a triangle, under an
// attack/release envelope (linear attack, exponential release to -80 dB) and the note's velocity. Notes play at their
// sample offsets. Nothing steps:
//   - a key played again while its voice still sounds retriggers that voice: its phase and envelope carry on, and its
//     level moves to the new velocity's over 2 ms (kRampMs);
//   - when all eight voices are busy, the oldest note is stolen: its voice fades out over 2 ms, then starts the new
//     note from silence (phase 0), so a note that steals sounds 2 ms late.
// The output gain is smoothed per sample (fsdsp::Smoother4, 20 ms). Everything advances per sample, so the output
// does not depend on the host's block size.
//
// The processor calls, on the audio thread: prepare() before playback (the only place that may allocate), setParams()
// once per block with the plain values of Params.h (finite, within the table's range), then process() with the
// block's note events; the output channels are written (not added to). Hosts call tailSeconds() from any thread, so
// it reads an atomic that setParams() stores. Replace the body with your algorithm and keep the contract: no
// allocation, no locks, no I/O in setParams() and process().
#pragma once

#include "dsp/Events.h"
#include "dsp/Params.h"

#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace testingplaygroundsynth
{
    class Engine
    {
    public:
        static constexpr int   kVoices = 8;
        static constexpr float kRampMs = 2.0f;                        // a retrigger's level change; a steal's fade

        void prepare(double sampleRate, int maxBlockSize) noexcept;
        void reset() noexcept;                                        // every voice off; the next setParams snaps
        void setParams(const ParamValues& values) noexcept;
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        double tailSeconds() const noexcept { return releaseSeconds_.load(std::memory_order_relaxed); }
        float  sampleRate() const noexcept { return fs_; }
        int    activeVoices() const noexcept;                         // held, releasing above -80 dB, or stolen and
                                                                      // fading out

    private:
        static constexpr int kQuads = kVoices / 4;

        void noteOn(int note, float velocity) noexcept;
        void noteOff(int note) noexcept;
        void allNotesOff() noexcept;
        bool sounding(int v) const noexcept;
        int  owner(int v) const noexcept;                             // the note a voice plays, or starts after a steal
        void rampTo(int v, float amp) noexcept;                       // the voice's level to `amp` over kRampMs
        void startPending(int v) noexcept;                            // a stolen voice, faded out: its new note
        void render(float* left, float* right, int n) noexcept;      // n samples with no event inside
        void renderSpan(float* left, float* right, int n) noexcept;  // ... and no ramp ending inside

        float fs_ = 48000.0f;
        float log2Inc0_ = 0.0f;              // fsdsp::phase::incFromPitch's offset: log2(440 / fs) - 69 / 12
        float attackStep_ = 1.0f;            // envelope rise per sample
        float releaseCoef_ = 0.0f;           // envelope factor per sample while released
        int   rampSamples_ = 96;             // kRampMs at fs_
        std::atomic<double> releaseSeconds_{ 0.3 };
        bool  triangle_ = false;
        fsdsp::Smoother4 smooth_{};          // lane 0: the linear output gain
        bool  primed_ = false;

        // Per voice v: quad v / 4, lane v % 4.
        alignas(16) std::array<uint32_t, kVoices> phase_{};
        alignas(16) std::array<uint32_t, kVoices> inc_{};
        alignas(16) std::array<uint32_t, kVoices> gate_{};   // all ones while the key is down (a lane mask)
        alignas(16) std::array<float, kVoices> env_{};
        alignas(16) std::array<float, kVoices> amp_{};       // the level, 0.25 x velocity, ramped
        alignas(16) std::array<float, kVoices> ampStep_{};   // its change per sample while a ramp runs, else 0
        std::array<float, kVoices> ampTarget_{};             // where the ramp lands, exactly
        std::array<int, kVoices> rampLeft_{};                // samples left in the ramp; 0: none
        std::array<int, kVoices> note_{};                    // the note sounding; -1: never used since reset()
        std::array<uint64_t, kVoices> started_{};            // note-on order, to steal the oldest

        // A stolen voice, while it fades out: the note it starts when the fade ends.
        std::array<bool, kVoices> pending_{};
        std::array<int, kVoices> pendingNote_{};
        std::array<uint32_t, kVoices> pendingInc_{};
        std::array<uint32_t, kVoices> pendingGate_{};
        std::array<float, kVoices> pendingAmp_{};
        uint64_t counter_ = 0;
    };
}
