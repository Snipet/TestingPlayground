// Source/dsp/Engine.cpp: see Engine.h.
//
// Per sample: the smoother (GAIN, SUSTAIN), then per quad the sine, the envelope and the level ramp (a smootherstep
// from `levelFrom` to `levelTo`, at rest when its position is 1), summed over the lanes and the quads, times GAIN.
// render() cuts its run at every ramp's end, where the ramp lands exactly on its target and a stolen voice, faded out,
// starts its new note; those cuts fall on the same samples whatever the host's block size.
#include "dsp/Engine.h"

#include "dsp/SineOscillator.h"

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Ramp.h"

#include <algorithm>
#include <cstring>

namespace testingplaygroundsynth
{
    namespace simd = fsdsp::simd;

    namespace
    {
        simd::u32x4 loadU(const uint32_t* p) noexcept
        {
            simd::u32x4 v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }

        void storeU(uint32_t* p, simd::u32x4 v) noexcept
        {
            std::memcpy(p, &v, sizeof v);
        }

        float perSample(float ms, float fs) noexcept           // 1 / (ms in samples), at most 1
        {
            return 1.0f / std::max(1.0f, ms * fs / 1000.0f);
        }
    }

    void Engine::prepare(double sampleRate, int maxBlockSize)
    {
        fs_ = sampleRate > 0.0 ? static_cast<float>(sampleRate) : 48000.0f;
        script_.prepare(fs_, maxBlockSize);
        pitchOffset_ = SineOscillator4::pitchOffset(static_cast<double>(fs_));
        rampSamples_ = std::max(1, static_cast<int>(fsdsp::kStealFadeMs * 0.001f * fs_ + 0.5f));
        smooth_.prepare(fs_, 20.0f);
        reset();
    }

    void Engine::reset() noexcept
    {
        phase_.fill(0);
        inc_.fill(0);
        env_.clear();
        levelFrom_.fill(0.0f);
        levelTo_.fill(0.0f);
        rampPos_.fill(1.0f);
        rampStep_.fill(0.0f);
        rampLeft_.fill(0);
        playing_.fill(-1);
        stolen_.fill(false);
        nextNote_.fill(-1);
        nextInc_.fill(0);
        nextLevel_.fill(0.0f);
        nextKeyDown_.fill(false);
        scheduler_.reset();
        primed_ = false;
    }

    void Engine::setParams(const ParamValues& values) noexcept
    {
        rates_.attack = perSample(values[param::attack], fs_);
        rates_.decay = perSample(values[param::decay], fs_);
        rates_.release = perSample(values[param::release], fs_);
        releaseSeconds_.store(static_cast<double>(values[param::release]) / 1000.0, std::memory_order_relaxed);

        const simd::f32x4 gain = fsdsp::linFromDb(simd::set1(values[param::gain]));
        smooth_.setTarget(simd::withLane<1>(gain, values[param::sustain] / 100.0f));
        script_.setAmount(values[param::script] * 0.01f, !primed_);
        if (!primed_)
        {
            smooth_.snap();
            primed_ = true;
        }
    }

    // ---- voices ---------------------------------------------------------------------------------------------------
    bool Engine::sounding(std::size_t v) const noexcept
    {
        return stolen_[v] || env_.sounding(v);
    }

    int Engine::activeVoices() const noexcept
    {
        int n = 0;
        for (std::size_t v = 0; v < kN; ++v)
            n += sounding(v) ? 1 : 0;
        return n;
    }

    Engine::VoiceState Engine::voice(int v) const noexcept
    {
        VoiceState s;
        if (v < 0 || v >= kVoices)
            return s;
        const auto i = static_cast<std::size_t>(v);
        s.sounding = sounding(i);
        s.note = playing_[i];
        s.held = env_.held(i);
        s.envelope = env_.level[i];
        s.level = levelNow(i);
        s.next = stolen_[i] ? nextNote_[i] : -1;
        return s;
    }

    float Engine::levelNow(std::size_t v) const noexcept
    {
        // The per-sample formula of renderSpan(), on one lane.
        const simd::f32x4 from = simd::set1(levelFrom_[v]);
        const simd::f32x4 delta = simd::sub(simd::set1(levelTo_[v]), from);
        return simd::lane<0>(simd::fma(from, delta, fsdsp::smootherstep(simd::set1(rampPos_[v]))));
    }

    void Engine::rampTo(std::size_t v, float level) noexcept
    {
        levelFrom_[v] = levelNow(v);
        levelTo_[v] = level;
        rampPos_[v] = 0.0f;
        rampStep_[v] = 1.0f / static_cast<float>(rampSamples_);
        rampLeft_[v] = rampSamples_;
    }

    void Engine::start(std::size_t v, int note, uint32_t inc, float level, bool keyDown) noexcept
    {
        phase_[v] = 0;                                          // from silence: phase 0, the envelope from 0
        inc_[v] = inc;
        env_.start(v, keyDown);
        levelFrom_[v] = levelTo_[v] = level;
        rampPos_[v] = 1.0f;
        rampStep_[v] = 0.0f;
        rampLeft_[v] = 0;
        playing_[v] = note;
        stolen_[v] = false;
    }

    void Engine::noteOn(int note, float velocity) noexcept
    {
        const float level = kVoiceLevel * std::clamp(velocity, 0.0f, 1.0f);
        const uint32_t inc = SineOscillator4::increment(note, pitchOffset_);

        std::array<bool, kN> busy{};
        for (std::size_t v = 0; v < kN; ++v)
            busy[v] = sounding(v);
        const Scheduler::Assignment a = scheduler_.noteOn(note, busy);
        const std::size_t v = a.voice;

        switch (a.action)
        {
            case Scheduler::Action::retrigger:
                if (stolen_[v])                                 // still fading out: it starts at this velocity
                {
                    nextLevel_[v] = level;
                    nextKeyDown_[v] = true;
                }
                else
                {
                    env_.retrigger(v);                          // phase and envelope carry on
                    if (level != levelTo_[v] || rampLeft_[v] > 0)
                        rampTo(v, level);
                }
                break;
            case Scheduler::Action::start:
                start(v, note, inc, level, true);
                break;
            case Scheduler::Action::steal:
                if (!stolen_[v])                                // fade it out; it starts the new note from silence
                    rampTo(v, 0.0f);
                stolen_[v] = true;
                nextNote_[v] = note;
                nextInc_[v] = inc;
                nextLevel_[v] = level;
                nextKeyDown_[v] = true;
                break;
        }
    }

    void Engine::noteOff(int note) noexcept
    {
        for (std::size_t v = 0; v < kN; ++v)
        {
            if (!scheduler_.plays(v, note))
                continue;
            if (stolen_[v])
                nextKeyDown_[v] = false;                        // released before it started: it starts silent
            else
                env_.release(v);
        }
    }

    void Engine::allNotesOff() noexcept
    {
        for (std::size_t v = 0; v < kN; ++v)
        {
            nextKeyDown_[v] = false;
            env_.release(v);
        }
    }

    // ---- rendering ------------------------------------------------------------------------------------------------
    void Engine::render(float* left, float* right, int n) noexcept
    {
        while (n > 0)
        {
            int span = n;
            for (const int remaining : rampLeft_)
                if (remaining > 0)
                    span = std::min(span, remaining);
            renderSpan(left, right, span);
            for (std::size_t v = 0; v < kN; ++v)
            {
                if (rampLeft_[v] == 0)
                    continue;
                rampLeft_[v] -= span;
                if (rampLeft_[v] > 0)
                    continue;
                levelFrom_[v] = levelTo_[v];                    // lands exactly
                rampPos_[v] = 1.0f;
                rampStep_[v] = 0.0f;
                if (stolen_[v])
                    start(v, nextNote_[v], nextInc_[v], nextLevel_[v], nextKeyDown_[v]);
            }
            left += span;
            if (right != nullptr)
                right += span;
            n -= span;
        }
    }

    void Engine::renderSpan(float* left, float* right, int n) noexcept
    {
        SineOscillator4 osc[kQuads];
        adsr::Quad env[kQuads];
        simd::f32x4 from[kQuads], delta[kQuads], pos[kQuads], step[kQuads];
        for (std::size_t q = 0; q < kQuads; ++q)
        {
            const std::size_t o = 4 * q;
            osc[q].phase = loadU(phase_.data() + o);
            osc[q].inc = loadU(inc_.data() + o);
            env[q] = env_.load(q);
            from[q] = simd::load(levelFrom_.data() + o);
            delta[q] = simd::sub(simd::load(levelTo_.data() + o), from[q]);
            pos[q] = simd::load(rampPos_.data() + o);
            step[q] = simd::load(rampStep_.data() + o);
        }
        const simd::f32x4 zero = simd::set1(0.0f), one = simd::set1(1.0f);
        const simd::f32x4 attack = simd::set1(rates_.attack), decay = simd::set1(rates_.decay);
        const simd::f32x4 release = simd::set1(rates_.release);

        for (int s = 0; s < n; ++s)
        {
            const simd::f32x4 smoothed = smooth_.tick();
            const float gain = simd::lane<0>(smoothed);
            const simd::f32x4 sustain = simd::set1(simd::lane<1>(smoothed));
            simd::f32x4 acc = zero;
            for (std::size_t q = 0; q < kQuads; ++q)
            {
                const simd::f32x4 wave = osc[q].next();
                const simd::f32x4 e = env[q].tick(attack, decay, release, sustain);
                pos[q] = simd::min(simd::add(pos[q], step[q]), one);
                const simd::f32x4 level = simd::fma(from[q], delta[q], fsdsp::smootherstep(pos[q]));
                acc = simd::fma(acc, wave, simd::mul(e, level));
            }
            const float sum = ((simd::lane<0>(acc) + simd::lane<1>(acc)) + (simd::lane<2>(acc) + simd::lane<3>(acc)))
                              * gain;
            left[s] = sum;
            if (right != nullptr)
                right[s] = sum;
        }

        for (std::size_t q = 0; q < kQuads; ++q)
        {
            const std::size_t o = 4 * q;
            storeU(phase_.data() + o, osc[q].phase);
            env_.store(q, env[q]);
            simd::store(rampPos_.data() + o, pos[q]);
        }
    }

    void Engine::process(float* const* channels, int numChannels, int numSamples,
                         std::span<const Event> events) noexcept
    {
        // A block with nothing to write (0 samples, which JUCE's VST3 wrapper passes on, or no channel) still applies
        // its events, all at once: a note-off is never lost. Only the rendering is skipped.
        const int len = numChannels > 0 ? std::max(numSamples, 0) : 0;
        float* left = len > 0 ? channels[0] : nullptr;
        float* right = len > 0 && numChannels > 1 ? channels[1] : nullptr;
        for (int ch = 2; ch < numChannels && len > 0; ++ch)
            std::fill(channels[ch], channels[ch] + len, 0.0f);

        int pos = 0;
        const auto renderTo = [&](int end) {
            if (end > pos)
                render(left + pos, right != nullptr ? right + pos : nullptr, end - pos);
            pos = std::max(pos, end);
        };
        for (const Event& e : events)
        {
            renderTo(std::clamp(static_cast<int>(e.offset), pos, len));
            switch (e.kind)
            {
                case EventKind::noteOn:      noteOn(e.note, e.velocity); break;
                case EventKind::noteOff:     noteOff(e.note); break;
                case EventKind::allNotesOff: allNotesOff(); break;
            }
        }
        renderTo(len);
        if (len > 0)
            script_.process(left, right, len);
    }
}
