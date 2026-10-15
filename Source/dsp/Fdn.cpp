// Source/dsp/Fdn.cpp: see Fdn.h.
#include "dsp/Fdn.h"

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Phase.h"
#include "fsdsp/core/Units.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace testingplayground
{
    namespace simd = fsdsp::simd;

    namespace
    {
        // The input and output signs (u_i, w_i): fixed and arbitrary, balanced within the even and the odd lines, so
        // a channel does not drive the network along one of the matrix's rows.
        constexpr std::array<float, kMaxLines> kTapIn{ +1.0f, +1.0f, -1.0f, +1.0f, +1.0f, -1.0f, -1.0f, -1.0f,
                                                       +1.0f, +1.0f, +1.0f, -1.0f, -1.0f, +1.0f, -1.0f, +1.0f };
        constexpr std::array<float, kMaxLines> kTapOut{ +1.0f, -1.0f, +1.0f, +1.0f, -1.0f, +1.0f, +1.0f, -1.0f,
                                                        -1.0f, +1.0f, -1.0f, -1.0f, +1.0f, +1.0f, +1.0f, -1.0f };

        constexpr float kThreeLog2Of10 = 9.96578428f;    // 10^(-3 x) = 2^(-3 log2(10) x)

        int validLines(int n) noexcept { return (n == 4 || n == 8) ? n : kMaxLines; }

        bool sameBits(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }

        // The layout's delays as floats, four per vector.
        void delaysOf(const FdnLayout& l, std::array<float, kMaxLines>& out) noexcept
        {
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] = static_cast<float>(l.delay[i]);
        }
    }

    void Fdn::prepare(float sampleRate)
    {
        fs_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
        const float capacity = maxLayoutDelay(fs_) + kMaxDepthMs * 0.001f * fs_ + 4.0f;
        for (DelayLine& l : line_)
            l.prepare(capacity);
        params_.prepare(fs_, fsdsp::kFadeMs);
        glideA_ = fsdsp::alphaFromTau(kGlideMs, fs_);
        fade_.prepare(fs_, fsdsp::kFadeMs);
        inFade_.prepare(fs_, fsdsp::kFadeMs);
        size_ = -1.0f;
        reset();
    }

    void Fdn::reset() noexcept
    {
        for (DelayLine& l : line_)
            l.clear();
        state_.fill(0.0f);
        phase_ = 0;
        clock_ = 0;
        ticker_ = fsdsp::ControlTicker{};
        inFade_.lin.cur = 1.0f;
        inFade_.setTarget(1.0f);
        if (fading_)                                     // a fade in progress ends where it was going
            table_ = hasPending_ ? pending_ : next_;
        fading_ = false;
        hasPending_ = false;
        fade_.lin.cur = 0.0f;
        fade_.setTarget(0.0f);
    }

    void Fdn::setLines(int lines, float sizeMeters, bool fadeIn) noexcept
    {
        lines_ = validLines(lines);
        size_ = sizeMeters;
        layout_ = makeLayout(fs_, sizeMeters, lines_);
        delaysOf(layout_, target_);
        offset_.fill(0.0f);
        base_ = target_;
        for (DelayLine& l : line_)
            l.clear();
        state_.fill(0.0f);
        inFade_.lin.cur = fadeIn ? 0.0f : 1.0f;
        inFade_.setTarget(1.0f);
        const uint32_t step = static_cast<uint32_t>((uint64_t{ 1 } << 32) / static_cast<uint64_t>(lines_));
        for (int i = 0; i < kMaxLines; ++i)
            lfoOffset_[static_cast<std::size_t>(i)] = i < lines_ ? step * static_cast<uint32_t>(i) : 0u;
        outScale_ = lines_ == 4 ? 0.707106781f : (lines_ == 8 ? 0.5f : 0.353553391f);   // sqrt(2 / n)
        ticker_ = fsdsp::ControlTicker{};                // the next sample recomputes the coefficients
    }

    void Fdn::setSize(float sizeMeters) noexcept
    {
        if (sameBits(sizeMeters, size_))
            return;
        size_ = sizeMeters;
        layout_ = makeLayout(fs_, sizeMeters, lines_);
        std::array<float, kMaxLines> target{};
        delaysOf(layout_, target);
        for (std::size_t i = 0; i < target.size(); ++i)
        {
            offset_[i] += target_[i] - target[i];       // the read stays where it is and glides from there
            target_[i] = target[i];
        }
    }

    void Fdn::setTargets(const Targets& t, bool snap) noexcept
    {
        const float decay = std::max(t.decayMs, 10.0f);
        const float hf = std::clamp(t.hfRatio, 0.1f, 1.0f);
        const float rate = std::clamp(t.rateHz, 0.0f, 0.25f * fs_);
        const float depth = std::clamp(t.depthMs, 0.0f, kMaxDepthMs);
        simd::f32x4 v = simd::set1(decay);
        v = simd::withLane<1>(v, hf);
        v = simd::withLane<2>(v, rate);
        v = simd::withLane<3>(v, depth);
        params_.setTarget(v);
        if (snap)
            params_.snap();
    }

    void Fdn::setTable(const LfoTable& table, bool snap) noexcept
    {
        if (snap)
        {
            table_ = table;
            fading_ = false;
            hasPending_ = false;
            fade_.lin.cur = 0.0f;
            fade_.setTarget(0.0f);
            return;
        }
        if (fading_)
        {
            pending_ = table;                            // after the fade in progress
            hasPending_ = true;
            return;
        }
        next_ = table;
        fading_ = true;
        fade_.lin.cur = 0.0f;
        fade_.setTarget(1.0f);
    }

    void Fdn::updateCoefficients(simd::f32x4 p) noexcept
    {
        const float decaySec = std::max(simd::lane<0>(p), 10.0f) * 0.001f;
        const float hf = std::clamp(simd::lane<1>(p), 0.1f, 1.0f);
        const simd::f32x4 perSample = simd::set1(-kThreeLog2Of10 / (decaySec * fs_));        // log2(g) per sample
        const simd::f32x4 perSampleHf = simd::set1(-kThreeLog2Of10 / (decaySec * hf * fs_));
        const simd::f32x4 one = simd::set1(1.0f);
        simd::f32x4 g2 = simd::set1(0.0f);
        for (int k = 0; k < lines_; k += 4)
        {
            const auto at = static_cast<std::size_t>(k);
            const simd::f32x4 len = simd::load(base_.data() + at);
            const simd::f32x4 g = fsdsp::exp2(simd::mul(len, perSample));
            const simd::f32x4 h = fsdsp::exp2(simd::mul(len, perSampleHf));
            const simd::f32x4 pole = simd::div(simd::sub(g, h), simd::add(g, h));
            simd::store(g_.data() + at, g);
            simd::store(pole_.data() + at, pole);
            simd::store(b_.data() + at, simd::mul(g, simd::sub(one, pole)));
            g2 = simd::add(g2, simd::mul(g, g));
        }
        const float meanG2 = (simd::lane<0>(g2) + simd::lane<1>(g2) + simd::lane<2>(g2) + simd::lane<3>(g2))
                           / static_cast<float>(lines_);
        inGain_ = std::sqrt(std::max(1.0f - meanG2, 0.0f));
        inc_ = simd::laneu<0>(fsdsp::phase::fromCycles(simd::set1(simd::lane<2>(p) / fs_)));
    }

    void Fdn::tick(float inL, float inR, float& wetL, float& wetR) noexcept
    {
        const int n = lines_;
        const simd::f32x4 p = params_.tick();
        const simd::f32x4 glide = simd::set1(glideA_), land = simd::set1(1e-3f), zero = simd::set1(0.0f);
        for (int k = 0; k < n; k += 4)
        {
            simd::f32x4 off = simd::mul(simd::load(offset_.data() + k), glide);
            off = simd::sel(simd::gt(land, simd::abs(off)), zero, off);
            simd::store(offset_.data() + k, off);
            simd::store(base_.data() + k, simd::add(simd::load(target_.data() + k), off));
        }
        if (ticker_.advance(clock_))
            updateCoefficients(p);
        ++clock_;

        // The LFO, and the table crossfade.
        float w = 0.0f;
        if (fading_)
        {
            w = fade_.tick();
            if (!fade_.moving())                         // landed: next_ is the table now
            {
                table_ = next_;
                fading_ = false;
                w = 0.0f;
                if (hasPending_)
                {
                    next_ = pending_;
                    hasPending_ = false;
                    fading_ = true;
                    fade_.lin.cur = 0.0f;
                    fade_.setTarget(1.0f);
                }
            }
        }
        const bool blend = fading_ && w > 0.0f;
        phase_ += inc_;

        // Reads at the modulated delays.
        alignas(16) std::array<float, kMaxLines> depth{};
        const simd::f32x4 depthSamples = simd::set1(simd::lane<3>(p) * fs_ * 0.001f);
        for (int k = 0; k < n; k += 4)
            simd::store(depth.data() + k,
                        simd::min(depthSamples, simd::mul(simd::load(base_.data() + k), simd::set1(kDepthLimit))));
        alignas(16) std::array<float, kMaxLines> s{};
        for (int i = 0; i < n; ++i)
        {
            const auto u = static_cast<std::size_t>(i);
            const uint32_t ph = phase_ + lfoOffset_[u];
            const uint32_t idx = ph >> 24;
            const float frac = static_cast<float>(ph & 0x00ffffffu) * 0x1p-24f;
            const float a0 = table_.v[idx], a1 = table_.v[idx + 1];
            float m = a0 + frac * (a1 - a0);
            if (blend)
            {
                const float b0 = next_.v[idx], b1 = next_.v[idx + 1];
                const float mb = b0 + frac * (b1 - b0);
                m = m + w * (mb - m);
            }
            s[u] = line_[u].read(base_[u] + depth[u] * m);
        }

        // Absorption, the mix, the input, the writes.
        alignas(16) std::array<float, kMaxLines> f{};
        for (int k = 0; k < n; k += 4)
        {
            const simd::f32x4 st = simd::add(simd::mul(simd::load(b_.data() + k), simd::load(s.data() + k)),
                                             simd::mul(simd::load(pole_.data() + k), simd::load(state_.data() + k)));
            simd::store(state_.data() + k, st);
            simd::store(f.data() + k, st);
        }
        hadamard(f.data(), n);
        const float in = inFade_.moving() ? inGain_ * inFade_.tick() : inGain_;
        const float kl = in * inL, kr = in * inR;
        for (int i = 0; i < n; ++i)
        {
            const auto u = static_cast<std::size_t>(i);
            line_[u].push(f[u] + kTapIn[u] * ((i & 1) != 0 ? kr : kl));
        }

        // The taps.
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < n; i += 2)
        {
            const auto u = static_cast<std::size_t>(i);
            l += kTapOut[u] * s[u];
            r += kTapOut[u + 1] * s[u + 1];
        }
        wetL = l * outScale_;
        wetR = r * outScale_;
    }
}
