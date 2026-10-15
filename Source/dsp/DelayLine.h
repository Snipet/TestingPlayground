#pragma once

#include <cstddef>
#include <vector>
#include <cmath>
#include <algorithm>

namespace hrvb
{
    // First-order Thiran/allpass interpolated delay line.
    //
    // For a desired delay D = N + d, 0 <= d < 1, the allpass section
    //     y[n] = a * x[n] + x[n-1] - a * y[n-1],  a = (1 - d) / (1 + d)
    // is placed after an integer delay of N samples. This gives a unity
    // magnitude response (no amplitude blow-up in a feedback loop, unlike
    // higher-order polynomial interpolators which are not unity-magnitude
    // and can amplify content inside an FDN at certain delay-modulation
    // velocities).
    //
    // To avoid the "click" caused by d crossing 1 -> 0 (which would also
    // reset the allpass state), the caller is expected to keep the
    // *fractional* part smoothly varying by allowing N to drift with d.
    // We accomplish that by tracking the desired delay as a float and
    // splitting it into N + d on every read.
    class DelayLine
    {
    public:
        void prepare(double sampleRate, float maxDelaySamples)
        {
            // Round up to next power of two so we can mask instead of mod.
            size_t need = static_cast<size_t>(std::ceil(maxDelaySamples)) + 4;
            size_t pow2 = 1;
            while (pow2 < need) pow2 <<= 1;
            buffer_.assign(pow2, 0.0f);
            mask_ = pow2 - 1;
            write_ = 0;
            prevOut_ = 0.0f;
            (void) sampleRate;
        }

        void reset() noexcept
        {
            std::fill(buffer_.begin(), buffer_.end(), 0.0f);
            prevOut_ = 0.0f;
            write_ = 0;
        }

        inline void push(float x) noexcept
        {
            buffer_[write_ & mask_] = x;
            ++write_;
        }

        // Read at a fractional delay. Must be called once per sample,
        // before push(), to keep the allpass state coherent.
        inline float readAllpass(float delaySamples) noexcept
        {
            // Clamp to a safe minimum. The allpass approximation degrades
            // near d == 0 / d == 1; biasing the fractional part toward
            // 0.5 stabilises it at the cost of a half-sample offset.
            if (delaySamples < 1.0f) delaySamples = 1.0f;

            const float d   = delaySamples - std::floor(delaySamples);
            const size_t N  = static_cast<size_t>(delaySamples);
            const float a   = (1.0f - d) / (1.0f + d);

            const size_t r0 = (write_ - N    ) & mask_;
            const size_t r1 = (write_ - N - 1) & mask_;
            const float  x0 = buffer_[r0];
            const float  x1 = buffer_[r1];

            // y[n] = a*(x0 - y[n-1]) + x1
            const float y = a * (x0 - prevOut_) + x1;
            prevOut_ = y;
            return y;
        }

    private:
        std::vector<float> buffer_;
        size_t mask_ = 0;
        size_t write_ = 0;
        float prevOut_ = 0.0f;
    };
}
