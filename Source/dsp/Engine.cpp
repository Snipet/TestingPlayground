// Source/dsp/Engine.cpp: see Engine.h.
//
// Per sample and quad: the envelope (attack: +attackStep up to 1 while the gate is on; release: x releaseCoef, and
// exactly 0 once under -80 dB), the level (plus its ramp step, 0 when no ramp runs), the waveform at the phase (sinPi
// of the signed phase, or the triangle 1 - 2|phase|), the sum of the lanes, the smoothed gain. Pitch:
// phase::incFromPitch (within 0.002 cents of 440 * 2^((n - 69) / 12)) rounded once to a uint32 increment, so a held
// note never drifts.
//
// Ramps: a level ramp runs kRampMs (rampSamples_ samples) and lands exactly on its target. render() cuts its span at
// every ramp's end, where a stolen voice, now silent, starts its new note; the cuts fall on the same samples whatever
// the host's block size, so the output does not depend on it.
#include "dsp/Engine.h"

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Phase.h"
#include "fsdsp/core/Units.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace testingplaygroundsynth
{
    namespace simd = fsdsp::simd;

    namespace
    {
        constexpr float kVoiceAmp = 0.25f;                 // a voice at full velocity: -12 dBFS peak
        constexpr float kSilent = 1e-4f;                   // -80 dB: a released voice under it is off
        constexpr float kLog2Of1e4 = 13.2877124f;          // a release falls 80 dB in its time
        constexpr uint32_t kGateOn = 0xffffffffu;

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
    }

    void Engine::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
    {
        fs_ = sampleRate > 0.0 ? static_cast<float>(sampleRate) : 48000.0f;
        log2Inc0_ = static_cast<float>(std::log2(440.0 / static_cast<double>(fs_)) - 69.0 / 12.0);
        rampSamples_ = std::max(1, static_cast<int>(kRampMs * 0.001f * fs_ + 0.5f));
        smooth_.prepare(fs_, 20.0f);
        reset();
    }

    void Engine::reset() noexcept
    {
        phase_.fill(0);
        inc_.fill(0);
        gate_.fill(0);
        env_.fill(0.0f);
        amp_.fill(0.0f);
        ampStep_.fill(0.0f);
        ampTarget_.fill(0.0f);
        rampLeft_.fill(0);
        note_.fill(-1);
        started_.fill(0);
        pending_.fill(false);
        pendingNote_.fill(-1);
        pendingInc_.fill(0);
        pendingGate_.fill(0);
        pendingAmp_.fill(0.0f);
        counter_ = 0;
        primed_ = false;
    }

    void Engine::setParams(const ParamValues& values) noexcept
    {
        const float attackSamples = std::max(1.0f, values[param::attack] * fs_ / 1000.0f);
        const float releaseSamples = std::max(1.0f, values[param::release] * fs_ / 1000.0f);
        attackStep_ = 1.0f / attackSamples;
        releaseCoef_ = fsdsp::exp2(-kLog2Of1e4 / releaseSamples);
        releaseSeconds_.store(static_cast<double>(values[param::release]) / 1000.0, std::memory_order_relaxed);
        triangle_ = values[param::wave] >= 0.5f;

        smooth_.setTarget(fsdsp::linFromDb(simd::set1(values[param::gain])));
        if (!primed_)
        {
            smooth_.snap();
            primed_ = true;
        }
    }

    bool Engine::sounding(int v) const noexcept
    {
        const auto i = static_cast<std::size_t>(v);
        return pending_[i] || (note_[i] >= 0 && (gate_[i] != 0 || env_[i] > 0.0f));
    }

    int Engine::owner(int v) const noexcept
    {
        const auto i = static_cast<std::size_t>(v);
        return pending_[i] ? pendingNote_[i] : note_[i];
    }

    int Engine::activeVoices() const noexcept
    {
        int n = 0;
        for (int v = 0; v < kVoices; ++v)
            n += sounding(v) ? 1 : 0;
        return n;
    }

    void Engine::rampTo(int v, float amp) noexcept
    {
        const auto i = static_cast<std::size_t>(v);
        ampTarget_[i] = amp;
        rampLeft_[i] = rampSamples_;
        ampStep_[i] = (amp - amp_[i]) / static_cast<float>(rampSamples_);
    }

    void Engine::startPending(int v) noexcept
    {
        const auto i = static_cast<std::size_t>(v);
        phase_[i] = 0;                                          // from silence: phase 0, the envelope from 0
        env_[i] = 0.0f;
        inc_[i] = pendingInc_[i];
        amp_[i] = pendingAmp_[i];
        ampTarget_[i] = pendingAmp_[i];
        ampStep_[i] = 0.0f;
        rampLeft_[i] = 0;
        gate_[i] = pendingGate_[i];                             // released during the fade: it starts silent
        note_[i] = pendingNote_[i];
        pending_[i] = false;
    }

    void Engine::noteOn(int note, float velocity) noexcept
    {
        const float amp = kVoiceAmp * std::clamp(velocity, 0.0f, 1.0f);
        const simd::f32x4 cycles = fsdsp::phase::incFromPitch(simd::set1(static_cast<float>(note)), log2Inc0_);
        const uint32_t inc = simd::laneu<0>(fsdsp::phase::fromCycles(cycles));

        for (int v = 0; v < kVoices; ++v)                       // the same key again: retrigger its voice
        {
            const auto i = static_cast<std::size_t>(v);
            if (owner(v) != note || !sounding(v))
                continue;
            if (pending_[i])                                    // still fading out: it starts at this velocity
            {
                pendingAmp_[i] = amp;
                pendingGate_[i] = kGateOn;
            }
            else
            {
                gate_[i] = kGateOn;                             // phase and envelope carry on
                if (amp != amp_[i] || rampLeft_[i] > 0)
                    rampTo(v, amp);
            }
            started_[i] = ++counter_;
            return;
        }

        for (int v = 0; v < kVoices; ++v)                       // a silent voice: it starts at once
        {
            const auto i = static_cast<std::size_t>(v);
            if (sounding(v))
                continue;
            pendingGate_[i] = kGateOn;
            pendingNote_[i] = note;
            pendingInc_[i] = inc;
            pendingAmp_[i] = amp;
            startPending(v);
            started_[i] = ++counter_;
            return;
        }

        int voice = 0;                                          // all busy: steal the oldest note
        for (int v = 1; v < kVoices; ++v)
            if (started_[static_cast<std::size_t>(v)] < started_[static_cast<std::size_t>(voice)])
                voice = v;
        const auto i = static_cast<std::size_t>(voice);
        if (!pending_[i])                                       // fade it out; it starts the new note at silence
            rampTo(voice, 0.0f);
        pending_[i] = true;
        pendingNote_[i] = note;
        pendingInc_[i] = inc;
        pendingGate_[i] = kGateOn;
        pendingAmp_[i] = amp;
        started_[i] = ++counter_;
    }

    void Engine::noteOff(int note) noexcept
    {
        for (std::size_t i = 0; i < note_.size(); ++i)
        {
            if (note_[i] == note)
                gate_[i] = 0;
            if (pending_[i] && pendingNote_[i] == note)
                pendingGate_[i] = 0;
        }
    }

    void Engine::allNotesOff() noexcept
    {
        gate_.fill(0);
        pendingGate_.fill(0);
    }

    void Engine::render(float* left, float* right, int n) noexcept
    {
        while (n > 0)
        {
            int span = n;
            for (const int remaining : rampLeft_)
                if (remaining > 0)
                    span = std::min(span, remaining);
            renderSpan(left, right, span);
            for (int v = 0; v < kVoices; ++v)
            {
                const auto i = static_cast<std::size_t>(v);
                if (rampLeft_[i] == 0)
                    continue;
                rampLeft_[i] -= span;
                if (rampLeft_[i] == 0)
                {
                    amp_[i] = ampTarget_[i];                    // exactly
                    ampStep_[i] = 0.0f;
                    if (pending_[i])
                        startPending(v);
                }
            }
            left += span;
            if (right != nullptr)
                right += span;
            n -= span;
        }
    }

    void Engine::renderSpan(float* left, float* right, int n) noexcept
    {
        simd::u32x4 phase[kQuads], inc[kQuads];
        simd::m32x4 gate[kQuads];
        simd::f32x4 env[kQuads], amp[kQuads], ampStep[kQuads];
        for (int q = 0; q < kQuads; ++q)
        {
            const auto o = static_cast<std::size_t>(4 * q);
            phase[q] = loadU(phase_.data() + o);
            inc[q] = loadU(inc_.data() + o);
            gate[q] = loadU(gate_.data() + o);
            env[q] = simd::load(env_.data() + o);
            amp[q] = simd::load(amp_.data() + o);
            ampStep[q] = simd::load(ampStep_.data() + o);
        }
        const simd::f32x4 zero = simd::set1(0.0f), one = simd::set1(1.0f), two = simd::set1(2.0f);
        const simd::f32x4 attackStep = simd::set1(attackStep_), releaseCoef = simd::set1(releaseCoef_);
        const simd::f32x4 silent = simd::set1(kSilent);

        for (int s = 0; s < n; ++s)
        {
            const float gain = simd::lane<0>(smooth_.tick());
            simd::f32x4 acc = zero;
            for (int q = 0; q < kQuads; ++q)
            {
                const simd::f32x4 up = simd::min(simd::add(env[q], attackStep), one);
                const simd::f32x4 down = simd::mul(env[q], releaseCoef);
                env[q] = simd::sel(gate[q], up, simd::sel(simd::gt(silent, down), zero, down));
                amp[q] = simd::add(amp[q], ampStep[q]);
                const simd::f32x4 x = fsdsp::phase::toSigned(phase[q]);
                const simd::f32x4 wave = triangle_ ? simd::fms(one, two, simd::abs(x)) : fsdsp::sinPi(x);
                acc = simd::fma(acc, wave, simd::mul(env[q], amp[q]));
                phase[q] = simd::addu(phase[q], inc[q]);
            }
            const float sum = ((simd::lane<0>(acc) + simd::lane<1>(acc)) + (simd::lane<2>(acc) + simd::lane<3>(acc)))
                              * gain;
            left[s] = sum;
            if (right != nullptr)
                right[s] = sum;
        }

        for (int q = 0; q < kQuads; ++q)
        {
            const auto o = static_cast<std::size_t>(4 * q);
            storeU(phase_.data() + o, phase[q]);
            simd::store(env_.data() + o, env[q]);
            simd::store(amp_.data() + o, amp[q]);
        }
    }

    void Engine::process(float* const* channels, int numChannels, int numSamples,
                         std::span<const Event> events) noexcept
    {
        if (numChannels <= 0 || numSamples <= 0)
            return;
        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : nullptr;
        for (int ch = 2; ch < numChannels; ++ch)
            std::fill(channels[ch], channels[ch] + numSamples, 0.0f);

        int pos = 0;
        const auto renderTo = [&](int end) {
            if (end > pos)
                render(left + pos, right != nullptr ? right + pos : nullptr, end - pos);
            pos = std::max(pos, end);
        };
        for (const Event& e : events)
        {
            renderTo(std::clamp(static_cast<int>(e.offset), pos, numSamples));
            switch (e.kind)
            {
                case EventKind::noteOn:      noteOn(e.note, e.velocity); break;
                case EventKind::noteOff:     noteOff(e.note); break;
                case EventKind::allNotesOff: allNotesOff(); break;
            }
        }
        renderTo(numSamples);
    }
}
