// Deterministic randomness (F §10; D §7 rule 3). Sprint-frozen (FZ0).
#pragma once
#include "fsdsp/core/Rt.h"
#include "fsdsp/core/Simd.h"
#include <cstdint>

namespace fsdsp {

// SplitMix64 (Steele, Lea, Flood 2014): advances `state` and returns the mixed value.
constexpr uint64_t splitMix64(uint64_t& state) noexcept FSDSP_NONBLOCKING {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
// A voice's seed: SplitMix64(instance seed ⊕ noteId); never the lane or the quad. The note counter behind noteId
// restarts at 1 on reset(), and on the transport start edge (F rec. 13) only when no note is held, keys down or held by
// a pedal; otherwise it continues (§9.7; ADR-69).
constexpr uint64_t voiceSeed(uint64_t instanceSeed, uint64_t noteId) noexcept FSDSP_NONBLOCKING {
    uint64_t s = instanceSeed ^ noteId;
    return splitMix64(s);
}

// PCG32, minimal form (pcg-random.org, pcg_basic.c): 64-bit LCG, multiplier 6364136223846793005, XSH-RR output.
// Scalar (NEON has no 64-bit lane multiply); 0.98 ns per call (F §9). Used at note-on, per S&H step, per step seed.
struct Pcg32 {
    uint64_t state = 0x853c49e6748fea9bull;          // PCG32_INITIALIZER
    uint64_t inc   = 0xda3e39cb94b95bdbull;
    constexpr void seed(uint64_t initState, uint64_t sequence) noexcept FSDSP_NONBLOCKING {
        state = 0; inc = (sequence << 1u) | 1u; (void)next(); state += initState; (void)next();
    }
    constexpr uint32_t next() noexcept FSDSP_NONBLOCKING {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    constexpr float unit() noexcept FSDSP_NONBLOCKING    { return static_cast<float>(next() >> 8) * 0x1p-24f; }  // [0, 1)
    constexpr float bipolar() noexcept FSDSP_NONBLOCKING { return unit() * 2.0f - 1.0f; }                     // [−1, 1)
};

// Audio-rate white noise: one xorshift32 (13, 17, 5) per lane (A rec. 9), each lane seeded from the voice's Pcg32.
struct Xorshift32x4 {
    simd::u32x4 s{};
    void seed(uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3) noexcept FSDSP_NONBLOCKING;   // each ORed with 1
    simd::f32x4 next() noexcept FSDSP_NONBLOCKING;                                   // (s >> 8)·2^−23 − 1 in [−1, 1)
};

// ---- Bodies (F1, S1; 01 §7.5, §7.9) ---------------------------------------------------------------------------------
// seed() ORs 1 into every lane, so no lane starts at 0, the one state xorshift32 never leaves. next() advances each
// lane by Marsaglia's xorshift32 with the triple (13, 17, 5): s ^= s << 13; s ^= s >> 17; s ^= s << 5, then returns
// the NEW state's top 24 bits as (s >> 8)*2^-23 - 1: an exact conversion, an exact scaling and an exact fused
// subtraction, on a 2^-23 grid in [-1, 1 - 2^-23]. A lane is the scalar generator bit for bit and never reads its
// neighbours (dsp.units).

inline void Xorshift32x4::seed(uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3) noexcept FSDSP_NONBLOCKING
{
    simd::u32x4 v = simd::set1u(l0);
    v = simd::withLaneu<1>(v, l1);
    v = simd::withLaneu<2>(v, l2);
    v = simd::withLaneu<3>(v, l3);
    s = simd::oru(v, simd::set1u(1u));
}

inline simd::f32x4 Xorshift32x4::next() noexcept FSDSP_NONBLOCKING
{
    simd::u32x4 x = s;
    x = simd::xoru(x, simd::shlu<13>(x));
    x = simd::xoru(x, simd::shru<17>(x));
    x = simd::xoru(x, simd::shlu<5>(x));
    s = x;
    return simd::fma(simd::set1(-1.0f), simd::cvtHi24(x), simd::set1(0x1p-23f));
}

} // namespace fsdsp
