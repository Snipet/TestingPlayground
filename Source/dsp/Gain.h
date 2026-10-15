#pragma once

#include <atomic>
#include <cmath>

namespace testingplaygroundsynth
{
    // Minimal example DSP block. Replace with your own — the contract
    // the processor expects is:
    //   - prepare(sampleRate, maxBlockSize) called from prepareToPlay
    //   - setX(...) parameter setters callable from the message thread
    //   - processBlock(...) called on the audio thread, no allocation,
    //     no locks, no denormals
    class Gain
    {
    public:
        void prepare(double sampleRate, int /*maxBlockSize*/) noexcept
        {
            fs_ = sampleRate;
            current_ = target_.load(std::memory_order_relaxed);
        }

        void setGainDb(float db) noexcept
        {
            target_.store(std::pow(10.0f, db * 0.05f), std::memory_order_relaxed);
        }

        void processBlock(float* const* channels, int numChannels, int numSamples) noexcept
        {
            const float tgt = target_.load(std::memory_order_relaxed);
            const float coeff = 1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(fs_)));
            float g = current_;

            for (int i = 0; i < numSamples; ++i)
            {
                g += coeff * (tgt - g);
                for (int ch = 0; ch < numChannels; ++ch)
                    channels[ch][i] *= g;
            }

            current_ = g;
        }

    private:
        double fs_ = 48000.0;
        std::atomic<float> target_{ 1.0f };
        float current_ = 1.0f;
    };
}
