// Source/dsp/DelayLine.h: one FDN delay line, read through a first-order Thiran allpass (JUCE-free).
//
// Ported from the legacy hrvb::DelayLine (git show legacy-visage:Source/dsp/DelayLine.h, which came from
// HardwareReverb): a power-of-two ring buffer, written once per sample by push() and read once per sample, before the
// push, at a fractional delay D = N + d through the allpass
//
//     y[n] = a (x[n - N] - y[n - 1]) + x[n - N - 1],   a = (1 - d) / (1 + d)
//
// whose magnitude is exactly 1 at every frequency (an interpolator in a feedback loop must not gain or lose level), so
// the delay can glide (SIZE) and be modulated (the LFO) without changing the decay.
//
// One fix, which the legacy comment asked for and its code did not do: the legacy split D as floor(D) + d with d in
// [0, 1), so an integer delay gave d = 0, a = 1 and a pole ON the unit circle at z = -1 (a loop that never forgets an
// error at Nyquist). Here the split is N = floor(D - 1/2), d = D - N in [1/2, 3/2): a lies in (-1/5, 1/3], the pole
// is at least 2/3 inside the circle, and an integer delay has d = 1, a = 0: a plain delay of exactly D samples.
//
// prepare() allocates (call it from Engine::prepare only); clear(), push() and read() are real-time safe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace testingplayground
{
    class DelayLine
    {
    public:
        static constexpr float kMinDelay = 2.0f;         // read() clamps up to this; N >= 1 from 1.5 on

        // Room for delays up to maxDelaySamples (rounded up to a power of two, plus slack); silent.
        void prepare(float maxDelaySamples)
        {
            const auto need = static_cast<std::size_t>(std::max(maxDelaySamples, kMinDelay)) + 4u;
            std::size_t size = 1;
            while (size < need)
                size <<= 1;
            buffer_.assign(size, 0.0f);
            mask_ = size - 1;
            clear();
        }

        void clear() noexcept
        {
            std::fill(buffer_.begin(), buffer_.end(), 0.0f);
            write_ = 0;
            prev_ = 0.0f;
        }

        // The largest delay read() honours.
        float capacity() const noexcept { return static_cast<float>(mask_) - 2.0f; }

        void push(float x) noexcept
        {
            buffer_[write_ & mask_] = x;
            ++write_;
        }

        // The sample `delay` samples back (fractional; read before the push of this sample). Clamped to
        // [kMinDelay, capacity()].
        float read(float delay) noexcept
        {
            delay = std::clamp(delay, kMinDelay, capacity());
            const auto n = static_cast<std::size_t>(delay - 0.5f);    // floor: the argument is >= 1.5
            const float d = delay - static_cast<float>(n);           // [0.5, 1.5)
            const float a = (1.0f - d) / (1.0f + d);
            const float x0 = buffer_[(write_ - n) & mask_];
            const float x1 = buffer_[(write_ - n - 1u) & mask_];
            const float y = a * (x0 - prev_) + x1;
            prev_ = y;
            return y;
        }

    private:
        std::vector<float> buffer_;
        std::size_t mask_ = 0;
        std::size_t write_ = 0;
        float prev_ = 0.0f;
    };
}
