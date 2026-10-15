// uint32 phase accumulators (brief §9; A rec. 4). One cycle = 2^32; wrapping is the integer overflow, so no error
// accumulates. Sprint-frozen (FZ0).
#pragma once
#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Rt.h"
#include "fsdsp/core/Simd.h"
#include <cstdint>

namespace fsdsp::phase {
using simd::f32x4;
using simd::m32x4;
using simd::u32x4;

// Cycles per sample from pitch in semitones (MIDI scale): exp2(semis/12 + log2Inc0), log2Inc0 = PrepareInfo::log2Inc0.
inline f32x4 incFromPitch(f32x4 semis, float log2Inc0) noexcept FSDSP_NONBLOCKING;
// Increment: round-to-nearest(c · 2^32) with c clamped to ±0.49999997 (below Nyquist); a negative c runs backwards
// (through-zero FM) as the two's complement.
inline u32x4 fromCycles(f32x4 cyclesPerSample) noexcept FSDSP_NONBLOCKING;
inline u32x4 fromUnit(f32x4 phase01) noexcept FSDSP_NONBLOCKING;            // a start phase in [0, 1)
inline f32x4 toUnit(u32x4 p) noexcept FSDSP_NONBLOCKING;                    // (p >> 8)·2^−24 in [0, 1): exact
inline f32x4 toSigned(u32x4 p) noexcept FSDSP_NONBLOCKING;                  // (int32)p · 2^−31, nearest, in [−1, 1]:
                                                                            //   1.0 for the top 64 phases;
                                                                            //   sin(2π·p/2^32) = sinPi(toSigned(p))
inline m32x4 wrapped(u32x4 before, u32x4 after) noexcept FSDSP_NONBLOCKING; // a forward wrap: after < before (unsigned)
inline f32x4 fraction(u32x4 after, u32x4 inc) noexcept FSDSP_NONBLOCKING;   // where in the sample a forward wrap fell,
                                                                            //   after/inc in [0, 1) (BLEP timing)
// FREE start phase (§5.4): phase0 + inc·index mod 2^32, exact (64-bit product, truncated).
constexpr uint32_t freeStart(uint32_t phase0, uint32_t inc, uint64_t index) noexcept FSDSP_NONBLOCKING {
    return phase0 + static_cast<uint32_t>(static_cast<uint64_t>(inc) * index);
}
} // namespace fsdsp::phase

// ==== Bodies (F1, S1; 01 §7.4, §7.9) =================================================================================
//
// Every function is a fixed sequence of fsdsp::simd ops, per lane, so a lane never depends on its neighbours.
//   - incFromPitch: one fused step forms semis/12 + log2Inc0 (the constant is the float nearest 1/12), then
//     fsdsp::exp2. Over MIDI 0-127 the result is within 0.0017 cents of 440*2^((semis - 69)/12)/fsRun when log2Inc0
//     is the float nearest log2(440/fsRun) - 69/12 (01 §7.9; dsp.fastmath measures 0.0014 ct at 176.4 and 352.8 kHz,
//     0.0012 at 88.2 kHz, 0.0008 at 96 kHz, and 0.0016 ct at worst over every 100 Hz from 44.1 to 384 kHz). The error
//     is the float argument's: log2Inc0's own rounding and the fused step's (at most 4.8e-7 octaves each while the
//     magnitude is 8 or more), the constant's 3.0e-8 of semis/12, and exp2's 1.7e-7. The argument's steps are far
//     coarser than the result's ulps, so a wanted increment is in general NOT reachable exactly from a float pitch:
//     fromCycles is the exact entry (dsp.units lists 03 §3.5.1's six).
//   - fromCycles: c is clamped to +-0.49999997 (2^-1 - 2^-25), scaled by 2^32 (exact) and rounded to the nearest
//     integer, ties to even, by FCVTNS; the largest magnitude is 2^31 - 128, so the conversion never saturates. A
//     negative c gives the two's complement, which runs the accumulator backwards. A NaN gives 0: a phase that
//     stands still.
//   - fromUnit: phase * 2^32 modulo 2^32, to the nearest unit. The conversion is signed, so the phase is taken as
//     phase - 1 from 1/2 up (exact for phase in [1/2, 1]); the result is exact modulo 2^32 for every phase in
//     [-1/2, 3/2), so 1.0 is phase 0 and a small negative phase wraps. Outside that range it saturates.
//   - toUnit: the top 24 bits, exact, in [0, 1 - 2^-24].
//   - toSigned: the phase read as int32, converted to the NEAREST float and scaled by 2^-31 (exact): within 2^-25
//     of the signed phase, in [-1, 1]. The upper end is closed: the top 64 phases (from 0x7fffffc0) round to exactly
//     1.0, where sinPi is exactly 0, so sinPi(toSigned(p)) stays inside sinPi's domain and continuous.
//   - wrapped: unsigned after < before; with an increment below 2^31 that is a forward wrap and nothing else.
//   - fraction: after and inc converted to float (both are below 2^31 after a forward wrap, so the signed conversion
//     is right; each rounds to 24 bits) and divided (IEEE). The integers satisfy after < inc, but their floats can
//     be equal, so the quotient is clamped to 1 - 2^-24: the result is in [0, 1) as declared. Lanes that did not
//     wrap hold an unused value; an increment of 0 divides by 1 there, so no lane produces a NaN.

namespace fsdsp::phase {

inline f32x4 incFromPitch(f32x4 semis, float log2Inc0) noexcept FSDSP_NONBLOCKING
{
    return fsdsp::exp2(simd::fma(simd::set1(log2Inc0), semis, simd::set1(0.0833333358f)));
}

inline u32x4 fromCycles(f32x4 cyclesPerSample) noexcept FSDSP_NONBLOCKING
{
    constexpr float kMax = 0x1.fffffep-2f;                                      // 0.49999997
    const f32x4 c = simd::min(simd::set1(kMax), simd::max(simd::set1(-kMax), cyclesPerSample));
    return simd::cvtRound(simd::mul(c, simd::set1(0x1p32f)));
}

inline u32x4 fromUnit(f32x4 phase01) noexcept FSDSP_NONBLOCKING
{
    const f32x4 centred = simd::sel(simd::ge(phase01, simd::set1(0.5f)), simd::sub(phase01, simd::set1(1.0f)), phase01);
    return simd::cvtRound(simd::mul(centred, simd::set1(0x1p32f)));
}

inline f32x4 toUnit(u32x4 p) noexcept FSDSP_NONBLOCKING
{
    return simd::mul(simd::cvtHi24(p), simd::set1(0x1p-24f));
}

inline f32x4 toSigned(u32x4 p) noexcept FSDSP_NONBLOCKING
{
    return simd::mul(simd::cvtSigned(p), simd::set1(0x1p-31f));
}

inline m32x4 wrapped(u32x4 before, u32x4 after) noexcept FSDSP_NONBLOCKING { return simd::ltu(after, before); }

inline f32x4 fraction(u32x4 after, u32x4 inc) noexcept FSDSP_NONBLOCKING
{
    const f32x4 q = simd::div(simd::cvtSigned(after), simd::max(simd::set1(1.0f), simd::cvtSigned(inc)));
    return simd::min(simd::set1(0x1.fffffep-1f), q);
}

} // namespace fsdsp::phase
