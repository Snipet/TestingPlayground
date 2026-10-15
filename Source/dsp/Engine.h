// Source/dsp/Engine.h: the instrument, JUCE-free. The legacy sine synth (tag legacy-visage: SineOscillator, Voice,
// VoiceScheduler, Gain and the processor's MIDI loop), ported onto FSynth's DSP core:
//
//   voices     16, as the legacy synth had, in four quads of four SIMD lanes (fsdsp::simd). A voice is a sine
//              (SineOscillator.h) under an ADSR (Adsr.h) at its note's level: 0.25 x velocity, the legacy headroom, so
//              one note at full velocity peaks at -12.04 dBFS and four at 0 dBFS
//   notes      at their sample offsets within the block: process() renders up to each event, then applies it
//   policy     VoiceScheduler.h, the legacy one: the same note retriggers its voice, else a silent voice starts it,
//              else the oldest note's voice is stolen
//   output     the voices' sum, times GAIN, the same on both channels
//
// Nothing steps:
//   - a retriggered voice keeps its phase, its envelope rises from where it is, and its level moves to the new
//     velocity's along a smootherstep over 2 ms (fsdsp::kStealFadeMs);
//   - a stolen voice fades out along the same 2 ms curve, then starts its new note from silence (phase 0, envelope 0),
//     so a note that steals sounds 2 ms late;
//   - GAIN and SUSTAIN are smoothed per sample (fsdsp::Smoother4, 20 ms); ATTACK, DECAY and RELEASE change slopes only.
// The legacy synth stepped in the first two places: a stolen voice jumped to its new pitch and velocity mid-waveform, a
// retriggered one to its new velocity; and ATTACK and RELEASE reached only the notes started after a change. Its GAIN
// did not step (a 10 ms one-pole); SUSTAIN is new. Everything advances per sample and every ramp ends on a fixed
// sample, so the output does not depend on how the host splits its blocks.
//
// Other changes from the legacy synth: every note that starts from silence starts at phase 0 (the legacy Voice never
// reset its oscillator, and an idle voice did not advance it, so a reused voice resumed at its previous note's last
// phase: harmless under an envelope from 0, but not the same samples); and the release lasts RELEASE from any level
// (Adsr.h).
//
// The processor calls, on the audio thread: prepare() before playback (the only place that may allocate), setParams()
// once per block with the plain values of Params.h (finite, within the table's range), then process() with the
// block's note events; the output channels are written (not added to). A call with 0 samples (or no channel) still
// applies its events. Hosts call tailSeconds() from any thread, so it reads an atomic that setParams() stores.
// activeVoices() and voice() describe the voices, for the probes.
#pragma once

#include "dsp/Adsr.h"
#include "dsp/Events.h"
#include "dsp/Params.h"
#include "dsp/VoiceScheduler.h"

#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace testingplaygroundsynth
{
    class Engine
    {
    public:
        static constexpr int   kVoices = 16;
        static constexpr float kVoiceLevel = 0.25f;                   // a voice at full velocity, before GAIN

        // One voice, as the probes see it.
        struct VoiceState
        {
            bool  sounding = false;      // held, releasing, or fading out after a steal: not free
            int   note = -1;             // the note it sounds now (-1: none since reset)
            bool  held = false;          // its key is down (the envelope's gate)
            float envelope = 0.0f;       // the ADSR's level, 0..1
            float level = 0.0f;          // 0.25 x velocity, ramped
            int   next = -1;             // stolen: the note it starts when its fade ends; else -1
        };

        void prepare(double sampleRate, int maxBlockSize) noexcept;
        void reset() noexcept;                                        // every voice off; the next setParams snaps
        void setParams(const ParamValues& values) noexcept;
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        double tailSeconds() const noexcept { return releaseSeconds_.load(std::memory_order_relaxed); }
        float  sampleRate() const noexcept { return fs_; }
        int    rampSamples() const noexcept { return rampSamples_; }   // a retrigger's or a steal's ramp
        int    activeVoices() const noexcept;
        VoiceState voice(int v) const noexcept;

    private:
        static constexpr std::size_t kN = static_cast<std::size_t>(kVoices);
        static constexpr std::size_t kQuads = kN / 4;
        using Scheduler = VoiceScheduler<kN>;

        void noteOn(int note, float velocity) noexcept;
        void noteOff(int note) noexcept;
        void allNotesOff() noexcept;
        bool sounding(std::size_t v) const noexcept;
        float levelNow(std::size_t v) const noexcept;                 // the level ramp's value at this sample
        void rampTo(std::size_t v, float level) noexcept;             // the level to `level` over rampSamples_
        void start(std::size_t v, int note, uint32_t inc, float level, bool keyDown) noexcept;
        void render(float* left, float* right, int n) noexcept;      // n samples with no event inside
        void renderSpan(float* left, float* right, int n) noexcept;  // ... and no ramp ending inside

        float fs_ = 48000.0f;
        float pitchOffset_ = 0.0f;                    // SineOscillator4::pitchOffset(fs_)
        int   rampSamples_ = 96;                      // fsdsp::kStealFadeMs at fs_
        adsr::Rates rates_{};
        std::atomic<double> releaseSeconds_{ 0.05 };
        fsdsp::Smoother4 smooth_{};                   // lane 0: the linear output gain; lane 1: SUSTAIN, 0..1
        bool  primed_ = false;
        Scheduler scheduler_{};

        // Per voice v (quad v / 4, lane v % 4).
        alignas(16) std::array<uint32_t, kN> phase_{};
        alignas(16) std::array<uint32_t, kN> inc_{};
        adsr::Bank<kN> env_{};
        alignas(16) std::array<float, kN> levelFrom_{};    // the level ramp: from, to, its position 0..1 (1: at rest)
        alignas(16) std::array<float, kN> levelTo_{};
        alignas(16) std::array<float, kN> rampPos_{};
        alignas(16) std::array<float, kN> rampStep_{};     // 1 / rampSamples_ while a ramp runs, else 0
        std::array<int, kN> rampLeft_{};                    // samples left in the ramp; 0: none
        std::array<int, kN> playing_{};                     // the note sounding; -1: none since reset

        // A stolen voice, while it fades out: the note it starts when the fade ends.
        std::array<bool, kN> stolen_{};
        std::array<int, kN> nextNote_{};
        std::array<uint32_t, kN> nextInc_{};
        std::array<float, kN> nextLevel_{};
        std::array<bool, kN> nextKeyDown_{};
    };
}
