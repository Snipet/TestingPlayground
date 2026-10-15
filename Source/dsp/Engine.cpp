// Source/dsp/Engine.cpp: see Engine.h.
//
// TONE is a topology-preserving one-pole low-pass: g = tan(pi fc / fs) (fsdsp::tanPi: no libm on the audio path),
// G = g / (1 + g); per sample v = (x - s) G, y = v + s, s = y + v. Its magnitude is exactly -3.01 dB at fc (the
// bilinear prewarp), and it is stable for every G in [0, 1), so G can be smoothed per sample.
#include "dsp/Engine.h"

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Sanitize.h"

#include <algorithm>

namespace testingplayground
{
    namespace simd = fsdsp::simd;

    void Engine::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
    {
        fs_ = sampleRate > 0.0 ? static_cast<float>(sampleRate) : 48000.0f;
        smooth_.prepare(fs_, 20.0f);
        reset();
    }

    void Engine::reset() noexcept
    {
        state_ = simd::set1(0.0f);
        primed_ = false;
    }

    void Engine::setParams(const ParamValues& values) noexcept
    {
        const float gain = simd::lane<0>(fsdsp::linFromDb(simd::set1(values[param::gain])));
        const float fc = std::clamp(values[param::tone], 10.0f, 0.49f * fs_);
        const float g = fsdsp::tanPi(fc / fs_);
        simd::f32x4 target = simd::set1(0.0f);
        target = simd::withLane<0>(target, gain);
        target = simd::withLane<1>(target, g / (1.0f + g));
        smooth_.setTarget(target);
        if (!primed_)
        {
            smooth_.snap();                  // the first block starts at its values, not 20 ms away from zero
            primed_ = true;
        }
    }

    void Engine::process(float* const* channels, int numChannels, int numSamples,
                         std::span<const Event> /*events*/) noexcept
    {
        if (numChannels <= 0 || numSamples <= 0)
            return;
        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : nullptr;
        fsdsp::sanitize(left, left, numSamples);                      // NaN and inf in -> 0, before the filter state
        if (right != nullptr)
            fsdsp::sanitize(right, right, numSamples);

        simd::f32x4 s = state_;
        for (int i = 0; i < numSamples; ++i)
        {
            const simd::f32x4 c = smooth_.tick();
            const simd::f32x4 gain = simd::set1(simd::lane<0>(c));
            const simd::f32x4 coef = simd::set1(simd::lane<1>(c));
            simd::f32x4 x = simd::withLane<0>(simd::set1(0.0f), left[i]);
            if (right != nullptr)
                x = simd::withLane<1>(x, right[i]);
            const simd::f32x4 v = simd::mul(simd::sub(x, s), coef);
            const simd::f32x4 y = simd::add(v, s);
            s = simd::add(y, v);
            const simd::f32x4 out = simd::mul(y, gain);
            left[i] = simd::lane<0>(out);
            if (right != nullptr)
                right[i] = simd::lane<1>(out);
        }
        state_ = s;
    }
}
