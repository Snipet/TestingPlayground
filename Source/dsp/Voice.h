#pragma once

#include <cmath>
#include "SineOscillator.h"

namespace testingplaygroundsynth
{
    // A single synth voice: one sine oscillator shaped by a simple linear
    // attack/release envelope so notes don't click on/off.
    //
    // Owned by the VoiceScheduler, which assigns it notes. Audio-thread only
    // once prepared: no allocation, no locks. MIDI note events arrive on the
    // audio thread (from the MidiBuffer in processBlock), so no atomics are
    // needed here.
    class Voice
    {
    public:
        void prepare(double sampleRate) noexcept
        {
            fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
            osc_.prepare(fs_);
            setAttackTime(0.005f);   // 5 ms
            setReleaseTime(0.050f);  // 50 ms
            env_   = 0.0f;
            stage_ = Stage::Idle;
            note_  = -1;
        }

        void noteOn(int midiNote, float velocity) noexcept
        {
            note_  = midiNote;
            level_ = velocity;
            osc_.setFrequency(midiNoteToHz(midiNote));
            stage_ = Stage::Attack;   // ramps up from the current env_ value
        }

        void noteOff() noexcept
        {
            if (stage_ != Stage::Idle)
                stage_ = Stage::Release;
        }

        bool isActive() const noexcept { return stage_ != Stage::Idle; }
        int  note()     const noexcept { return note_; }

        void setAttackTime(float seconds) noexcept
        {
            const float n = seconds * static_cast<float>(fs_);
            attackInc_ = n > 0.0f ? 1.0f / n : 1.0f;
        }

        void setReleaseTime(float seconds) noexcept
        {
            const float n = seconds * static_cast<float>(fs_);
            releaseInc_ = n > 0.0f ? 1.0f / n : 1.0f;
        }

        // Produces this voice's next output sample (0 when idle).
        float renderNextSample() noexcept
        {
            if (stage_ == Stage::Idle)
                return 0.0f;

            switch (stage_)
            {
                case Stage::Attack:
                    env_ += attackInc_;
                    if (env_ >= 1.0f) { env_ = 1.0f; stage_ = Stage::Sustain; }
                    break;
                case Stage::Release:
                    env_ -= releaseInc_;
                    if (env_ <= 0.0f) { env_ = 0.0f; stage_ = Stage::Idle; note_ = -1; }
                    break;
                case Stage::Sustain:
                case Stage::Idle:
                default:
                    break;
            }

            return osc_.nextSample() * env_ * level_;
        }

    private:
        enum class Stage { Idle, Attack, Sustain, Release };

        static float midiNoteToHz(int note) noexcept
        {
            return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
        }

        SineOscillator osc_;
        double fs_    = 48000.0;
        Stage  stage_ = Stage::Idle;
        int    note_  = -1;
        float  level_ = 0.0f;   // velocity, 0..1
        float  env_   = 0.0f;   // current envelope, 0..1
        float  attackInc_  = 0.0f;
        float  releaseInc_ = 0.0f;
    };
}
