#pragma once

#include <cmath>

namespace testingplaygroundsynth
{
    // A bare-bones sine-wave oscillator.
    //
    // Self-contained (no JUCE) and audio-thread safe: no allocation, no locks.
    // Usage: prepare() once with the sample rate, setFrequency() whenever the
    // pitch changes, then pull one sample at a time with nextSample().
    class SineOscillator
    {
    public:
        void prepare(double sampleRate) noexcept
        {
            fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
            updateIncrement();
        }

        void setFrequency(float hz) noexcept
        {
            freq_ = hz;
            updateIncrement();
        }

        void reset() noexcept { phase_ = 0.0f; }

        float nextSample() noexcept
        {
            const float s = std::sin(phase_);
            phase_ += inc_;
            if (phase_ >= kTwoPi)
                phase_ -= kTwoPi;
            return s;
        }

    private:
        static constexpr float kTwoPi = 6.28318530717958647692f;

        void updateIncrement() noexcept
        {
            inc_ = kTwoPi * freq_ / static_cast<float>(fs_);
        }

        double fs_    = 48000.0;
        float  freq_  = 440.0f;
        float  phase_ = 0.0f;
        float  inc_   = 0.0f;
    };
}
