// Source/dsp/Adsr.h: the envelope of the voices, four at a time (one per SIMD lane). The legacy Voice had a linear
// attack up to a held level of 1 and a linear release; this is that envelope with a decay and a sustain level added,
// and a release that always lasts RELEASE (below). Every segment is a straight line:
//
//   attack   while the key is down: up to 1 at 1 / ATTACK per second, from wherever the level is (from silence it lasts
//            ATTACK; a key played again while its voice still sounds rises from the level it finds: the legacy rule)
//   decay    then down to SUSTAIN at (1 - SUSTAIN) / DECAY per second: from 1 it lasts DECAY
//   sustain  SUSTAIN while the key is down. The level follows SUSTAIN's smoothed value: a higher value is followed at
//            once, a lower one at the decay's slope or at the smoother's, whichever is slower, so it never steps
//   release  from the level at note-off down to 0 in RELEASE, whatever that level (a new RELEASE takes effect at once:
//            the slope is the note-off level / RELEASE). A change from the legacy rule, which fell at 1 / RELEASE per
//            second, so a note let go at level L faded in L x RELEASE: the two agree for a note let go at 1 (after its
//            attack, at SUSTAIN 100 %), but a note let go during its attack now fades for longer, up to 1 / L times
//            (ATTACK 200 ms, let go after 50 ms at 0.25: legacy 0.25 x RELEASE, now RELEASE). For the legacy rule, make
//            `fall` level - release in Quad::tick (releaseFrom is then unused) and adapt dsp.envelope's release rows.
//
// The times arrive once per block (Rates, per sample); changing one changes a slope, never the level. A released voice
// reaches exactly 0 and stays there, which is when the Engine counts it free.
//
// Bank holds every voice's state in arrays (voice v: lane v % 4 of quad v / 4); the Engine loads a Quad into registers
// for a run of samples, ticks it per sample and stores it back. Audio thread only; no allocation.
#pragma once

#include "fsdsp/core/Simd.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace testingplaygroundsynth::adsr
{
    namespace simd = fsdsp::simd;

    inline constexpr uint32_t kOn = 0xffffffffu;               // a lane mask: all ones

    // The slopes, per sample: 1 / (the segment's time in samples).
    struct Rates
    {
        float attack = 1.0f;
        float decay = 1.0f;
        float release = 1.0f;
    };

    // Four envelopes in registers.
    struct Quad
    {
        simd::f32x4 level;                                     // 0..1
        simd::m32x4 gate;                                      // the key is down
        simd::m32x4 rising;                                    // in the attack
        simd::f32x4 releaseFrom;                               // the level at note-off

        // One sample. `sustain` is SUSTAIN (0..1) at this sample, in every lane.
        simd::f32x4 tick(simd::f32x4 attack, simd::f32x4 decay, simd::f32x4 release, simd::f32x4 sustain) noexcept
        {
            const simd::f32x4 zero = simd::set1(0.0f), one = simd::set1(1.0f);
            const simd::f32x4 up = simd::min(simd::add(level, attack), one);
            const simd::f32x4 down = simd::max(simd::fms(level, simd::sub(one, sustain), decay), sustain);
            const simd::f32x4 fall = simd::max(simd::fms(level, releaseFrom, release), zero);
            level = simd::sel(gate, simd::sel(rising, up, down), fall);
            rising = simd::selu(simd::ge(up, one), simd::set1u(0), rising);   // the attack ends at 1
            return level;
        }
    };

    template <std::size_t N>
    struct Bank
    {
        static_assert(N % 4 == 0, "whole quads");

        alignas(16) std::array<float, N> level{};
        alignas(16) std::array<uint32_t, N> gate{};
        alignas(16) std::array<uint32_t, N> rising{};
        alignas(16) std::array<float, N> releaseFrom{};

        void clear() noexcept
        {
            level.fill(0.0f);
            gate.fill(0);
            rising.fill(0);
            releaseFrom.fill(0.0f);
        }

        // A new note from silence.
        void start(std::size_t v, bool keyDown) noexcept
        {
            level[v] = 0.0f;
            releaseFrom[v] = 0.0f;
            gate[v] = rising[v] = keyDown ? kOn : 0u;
        }

        // The key again: a new attack from the level the voice is at.
        void retrigger(std::size_t v) noexcept { gate[v] = rising[v] = kOn; }

        // The key let go: the release starts from the level now.
        void release(std::size_t v) noexcept
        {
            if (gate[v] == 0)
                return;
            gate[v] = rising[v] = 0;
            releaseFrom[v] = level[v];
        }

        bool held(std::size_t v) const noexcept { return gate[v] != 0; }
        bool sounding(std::size_t v) const noexcept { return gate[v] != 0 || level[v] > 0.0f; }

        Quad load(std::size_t q) const noexcept
        {
            const std::size_t o = 4 * q;
            Quad x;
            x.level = simd::load(level.data() + o);
            std::memcpy(&x.gate, gate.data() + o, sizeof x.gate);
            std::memcpy(&x.rising, rising.data() + o, sizeof x.rising);
            x.releaseFrom = simd::load(releaseFrom.data() + o);
            return x;
        }

        void store(std::size_t q, const Quad& x) noexcept
        {
            const std::size_t o = 4 * q;
            simd::store(level.data() + o, x.level);
            std::memcpy(gate.data() + o, &x.gate, sizeof x.gate);
            std::memcpy(rising.data() + o, &x.rising, sizeof x.rising);
            simd::store(releaseFrom.data() + o, x.releaseFrom);
        }
    };
}
