// Source/dsp/Engine.cpp: see Engine.h.
#include "dsp/Engine.h"

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Sanitize.h"

#include <algorithm>

namespace testingplayground
{
    namespace simd = fsdsp::simd;

    void Engine::prepare(double sampleRate, int maxBlockSize)
    {
        fs_ = sampleRate > 0.0 ? static_cast<float>(sampleRate) : 48000.0f;
        fdn_.prepare(fs_);
        script_.prepare(fs_, maxBlockSize);
        smooth_.prepare(fs_, fsdsp::kFadeMs);
        dip_.prepare(fs_, fsdsp::kFadeMs);
        prepared_ = true;
        reset();
    }

    void Engine::reset() noexcept
    {
        fdn_.reset();
        dip_.lin.cur = 1.0f;
        dip_.setTarget(1.0f);
        wantLines_ = 0;
        primed_ = false;
    }

    void Engine::setParams(const ParamValues& values) noexcept
    {
        const int lines = linesFromChoice(values[param::lines]);
        size_ = values[param::size];
        Fdn::Targets t;
        t.decayMs = values[param::decay];
        t.hfRatio = values[param::hfdecay] * 0.01f;
        t.rateHz = values[param::modrate];
        t.depthMs = values[param::moddepth];
        simd::f32x4 target = simd::set1(0.0f);
        target = simd::withLane<0>(target, std::clamp(values[param::mix] * 0.01f, 0.0f, 1.0f));
        target = simd::withLane<1>(target, simd::lane<0>(fsdsp::linFromDb(simd::set1(values[param::gain]))));
        smooth_.setTarget(target);
        script_.setAmount(values[param::script] * 0.01f, !primed_);

        if (!primed_)
        {
            fdn_.setLines(lines, size_, false);
            fdn_.setTargets(t, true);
            smooth_.snap();                  // the first block starts at its values, not 20 ms away from them
            primed_ = true;
        }
        else
        {
            if (lines != fdn_.lines())
            {
                wantLines_ = lines;          // process() switches when the wet path has faded out
                dip_.setTarget(0.0f);
            }
            else
            {
                wantLines_ = 0;
                dip_.setTarget(1.0f);
                fdn_.setSize(size_);
            }
            fdn_.setTargets(t, false);
        }

        // Hosts' tail: 1.5 DECAY (-90 dB) after the longest line.
        const FdnLayout& l = fdn_.layout();
        const float longest = static_cast<float>(l.delay[static_cast<std::size_t>(l.lines - 1)]) / fs_;
        tail_.store(1.5 * static_cast<double>(t.decayMs) * 0.001 + static_cast<double>(longest),
                    std::memory_order_relaxed);
    }

    void Engine::setLfoTable(const LfoTable& table) noexcept
    {
        fdn_.setTable(table, !primed_);
    }

    void Engine::process(float* const* channels, int numChannels, int numSamples,
                         std::span<const Event> /*events*/) noexcept
    {
        if (numChannels <= 0 || numSamples <= 0)
            return;
        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : nullptr;
        fsdsp::sanitize(left, left, numSamples);                      // NaN and inf in -> 0, before any delay line
        if (right != nullptr)
            fsdsp::sanitize(right, right, numSamples);
        if (!prepared_)
            return;                                                   // no delay lines yet: the dry signal, as it is

        for (int i = 0; i < numSamples; ++i)
        {
            if (wantLines_ != 0 && dip_.lin.cur == 0.0f && dip_.lin.tgt == 0.0f)
            {
                fdn_.setLines(wantLines_, size_, true);               // silent now: switch, clear, fade back in
                wantLines_ = 0;
                dip_.setTarget(1.0f);
            }
            const float xl = left[i];
            const float xr = right != nullptr ? right[i] : xl;
            float wl = 0.0f, wr = 0.0f;
            fdn_.tick(xl, xr, wl, wr);
            const simd::f32x4 c = smooth_.tick();
            const float mix = simd::lane<0>(c), gain = simd::lane<1>(c);
            const float wet = mix * dip_.tick(), dry = 1.0f - mix;
            if (right != nullptr)
            {
                left[i] = gain * (dry * xl + wet * wl);
                right[i] = gain * (dry * xr + wet * wr);
            }
            else
            {
                left[i] = gain * (dry * xl + wet * ((wl + wr) * 0.707106781f));
            }
        }
        script_.process(left, right, numSamples);
    }
}
