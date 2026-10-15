#pragma once

// fsdsp::simd: the 4-lane float vector every kernel runs on (01 §7.1, §7.2, §5.2; seeded from FCompressor: FC 01
// §5.1, FC E §3.2). It extends HardwareReverb's Simd.h:
// a set of thin inline forwarders over a type ALIAS (not a wrapper class), so on arm64 `f32x4` IS `float32x4_t` and
// each op is one intrinsic. In FSynth the lanes of a voice-domain vector are the four voices of a quad, and a
// global-domain vector is {L, R, 0, 0} (01 §5.2); lanes never read each other (D §7).
//
// Frozen at FZ0 (names and signatures). Seeded with the NEON and SSE4.1/AVX2 bodies, bit-identical across the two
// backends in FCompressor; FSynth builds and verifies NEON only (Q4), and the additions at the end of this file have
// NEON bodies only, so the SSE branches below are kept from the seed and cannot be built (01 §7.1, §7.2). F1 (S1)
// verifies the seeded ops on FSynth's flags (dsp.simd). Rules the bodies keep:
//   - fma(a, b, c) = a + b*c and fms(a, b, c) = a - b*c, fused (one rounding) on both backends: the accumulator is
//     the FIRST argument (NEON vfmaq_f32 order; x86 _mm_fmadd_ps(b, c, a)).
//   - rsqrte is NEON's FRSQRTE on both backends (x86 computes the table function; HR measured why rsqrtps is not
//     acceptable); rsqrts(p, q) = (3 - p*q) / 2 fused.
//   - min/max NaN semantics differ between NEON and x86. Input sanitisation (Sanitize.h), the log floors and the
//     poison check keep NaN out of the engine; dsp.simd pins the policy.
//   - Comparisons return a lane mask `m32x4` (all-ones or all-zeros per lane) that sel/band/bor consume.
//   - Every op is FSDSP_NONBLOCKING (core/Rt.h): the bodies are intrinsics only.
// The seed's x86-64 slice requires FMA: an unfused fallback would be a different arithmetic, not a slower one.

#include "fsdsp/core/Rt.h"
#include <cstdint>
#include <limits>

#if defined(__aarch64__) || defined(__ARM_NEON)
  #include <arm_neon.h>
  #define FSDSP_SIMD_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define FSDSP_SIMD_SSE 1
  #if !defined(__FMA__) && !(defined(_MSC_VER) && !defined(__clang__) && defined(__AVX2__))
    #error "fsdsp's x86-64 build needs FMA: fsyn_flags adds -mavx2 -mfma (03 §2.6)."
  #endif
#else
  #error "fsdsp: no SIMD backend for this architecture (arm64 NEON, or x86-64 SSE4.1/AVX2 with FMA)."
#endif

namespace fsdsp::simd {

#if defined(FSDSP_SIMD_NEON)
using f32x4 = float32x4_t;                  // four float lanes
using m32x4 = uint32x4_t;                   // per-lane mask: all-ones = true
#else
using f32x4 = __m128;
using m32x4 = __m128;                       // SSE compares produce float-typed masks
#endif

// Memory and construction (unaligned access on both backends).
inline f32x4 load (const float* p) noexcept FSDSP_NONBLOCKING;
inline void  store(float* p, f32x4 v) noexcept FSDSP_NONBLOCKING;
inline f32x4 set1 (float s) noexcept FSDSP_NONBLOCKING;

// Arithmetic. div and sqrt are IEEE correctly rounded, so bit-equal across arches.
inline f32x4 add(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 sub(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 mul(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 div(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING;     // a + b*c, one rounding
inline f32x4 fms(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING;     // a - b*c, one rounding
inline f32x4 min(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 max(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;
inline f32x4 abs(f32x4 a) noexcept FSDSP_NONBLOCKING;                       // exact (sign bit cleared)
inline f32x4 neg(f32x4 a) noexcept FSDSP_NONBLOCKING;                       // exact (sign bit flipped)
inline f32x4 sqrt(f32x4 a) noexcept FSDSP_NONBLOCKING;
inline f32x4 floor(f32x4 a) noexcept FSDSP_NONBLOCKING;                     // exact (round toward -inf)

// Newton-Raphson 1/sqrt halves, as the hardware exposes them: r0 = rsqrte(x); r1 = r0 * rsqrts(r0*x, r0).
inline f32x4 rsqrte(f32x4 x) noexcept FSDSP_NONBLOCKING;
inline f32x4 rsqrts(f32x4 p, f32x4 q) noexcept FSDSP_NONBLOCKING;

// Comparison, selection and mask logic.
inline m32x4 gt  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;             // a > b, per lane
inline m32x4 ge  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING;             // a >= b, per lane
inline f32x4 sel (m32x4 mask, f32x4 t, f32x4 f) noexcept FSDSP_NONBLOCKING; // mask ? t : f, per lane
inline m32x4 band(m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING;             // mask AND
inline m32x4 bor (m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING;             // mask OR

// Lane access; I in [0, 3].
template <int I> inline float lane(f32x4 v) noexcept FSDSP_NONBLOCKING;
template <int I> inline f32x4 withLane(f32x4 v, float s) noexcept FSDSP_NONBLOCKING;

} // namespace fsdsp::simd

// ==== Bodies (seeded: FC's F1, S1) ===================================================================================
//
// Every op is bit-identical on the two backends for every input, with four documented exceptions (dsp.simd pins the
// first two for NEON and, as FCompressor's does, leaves all four out of the seeded part of its hash):
//   - min/max with a NaN operand: NEON returns NaN; x86 returns the SECOND operand (minps/maxps). Put the operand that
//     may be NaN second (max(set1(floor), x)) and a NaN propagates on both.
//   - min/max of +0 and -0: NEON orders -0 < +0 (min -> -0, max -> +0); x86 returns the second operand.
//   - NaN payloads and signs are not specified anywhere (compilers commute operands; x86 FMA picks its NaN by
//     encoding), so dsp.simd hashes every NaN as one canonical NaN.
//   - Flush-to-zero decides tininess before rounding on arm64 (FZ, Arm ARM FPRound) and after rounding on x86 (FTZ):
//     a mul/div/fma/sqrt whose exact result lies just below FLT_MIN and rounds up to it is 0 on arm64, FLT_MIN on x86.
// Denormals follow the FP mode, which fsdsp always runs under (ScopedFtz: FPCR.FZ on arm64, MXCSR FTZ|DAZ on x86):
// arithmetic flushes denormal inputs and outputs on both; load/store/set1/abs/neg/sel/lane move bits untouched. The
// compiler folds constant expressions in the default mode (no flush), so never rely on FTZ for constant operands.
//
// rsqrte/rsqrts are AArch64 FRSQRTE/FRSQRTS on both backends. x86 computes FRSQRTE's table function per lane
// (detail::frsqrteBits, from the Arm ARM's FPRSqrtEstimate/RecipSqrtEstimate; HR measured why rsqrtps, a different
// 12-bit estimate, is not acceptable) and FRSQRTS as detail::rsqrtsPortable, a composition of the ops above that is
// exact against the instruction including its (0 x inf) -> 1.5 case. Both helpers compile on arm64 too, where
// dsp.simd checks them against the hardware instructions, so the x86 code is exercised on a Mac without Rosetta.

namespace fsdsp::simd {

#if defined(FSDSP_SIMD_NEON)

inline f32x4 load (const float* p) noexcept FSDSP_NONBLOCKING    { return vld1q_f32(p); }
inline void  store(float* p, f32x4 v) noexcept FSDSP_NONBLOCKING { vst1q_f32(p, v); }
inline f32x4 set1 (float s) noexcept FSDSP_NONBLOCKING           { return vdupq_n_f32(s); }

inline f32x4 add(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vaddq_f32(a, b); }
inline f32x4 sub(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vsubq_f32(a, b); }
inline f32x4 mul(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vmulq_f32(a, b); }
inline f32x4 div(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vdivq_f32(a, b); }
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING { return vfmaq_f32(a, b, c); }
inline f32x4 fms(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING { return vfmsq_f32(a, b, c); }
inline f32x4 min(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vminq_f32(a, b); }
inline f32x4 max(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vmaxq_f32(a, b); }
inline f32x4 abs(f32x4 a) noexcept FSDSP_NONBLOCKING                   { return vabsq_f32(a); }
inline f32x4 neg(f32x4 a) noexcept FSDSP_NONBLOCKING                   { return vnegq_f32(a); }
inline f32x4 sqrt(f32x4 a) noexcept FSDSP_NONBLOCKING                  { return vsqrtq_f32(a); }
inline f32x4 floor(f32x4 a) noexcept FSDSP_NONBLOCKING                 { return vrndmq_f32(a); }

inline f32x4 rsqrte(f32x4 x) noexcept FSDSP_NONBLOCKING          { return vrsqrteq_f32(x); }
inline f32x4 rsqrts(f32x4 p, f32x4 q) noexcept FSDSP_NONBLOCKING { return vrsqrtsq_f32(p, q); }

inline m32x4 gt  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vcgtq_f32(a, b); }
inline m32x4 ge  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING          { return vcgeq_f32(a, b); }
inline f32x4 sel (m32x4 mask, f32x4 t, f32x4 f) noexcept FSDSP_NONBLOCKING { return vbslq_f32(mask, t, f); }
inline m32x4 band(m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING          { return vandq_u32(a, b); }
inline m32x4 bor (m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING          { return vorrq_u32(a, b); }

template <int I> inline float lane(f32x4 v) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return vgetq_lane_f32(v, I);
}
template <int I> inline f32x4 withLane(f32x4 v, float s) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return vsetq_lane_f32(s, v, I);
}

#else // FSDSP_SIMD_SSE: SSE4.1 + FMA3 (the x86-64 slice builds with -mavx2 -mfma; VEX encodings throughout)

// Unaligned on purpose (HR Simd.h): with VEX encodings vmovups on aligned data costs what vmovaps does.
inline f32x4 load (const float* p) noexcept FSDSP_NONBLOCKING    { return _mm_loadu_ps(p); }
inline void  store(float* p, f32x4 v) noexcept FSDSP_NONBLOCKING { _mm_storeu_ps(p, v); }
inline f32x4 set1 (float s) noexcept FSDSP_NONBLOCKING           { return _mm_set1_ps(s); }

inline f32x4 add(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_add_ps(a, b); }
inline f32x4 sub(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_sub_ps(a, b); }
inline f32x4 mul(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_mul_ps(a, b); }
inline f32x4 div(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_div_ps(a, b); }
// The accumulator moves from NEON's first argument to x86's last: _mm_fmadd_ps(x, y, z) = x*y + z and
// _mm_fnmadd_ps(x, y, z) = -(x*y) + z, each with one rounding, so the product b*c is the same exact real on both.
inline f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING { return _mm_fmadd_ps(b, c, a); }
inline f32x4 fms(f32x4 a, f32x4 b, f32x4 c) noexcept FSDSP_NONBLOCKING { return _mm_fnmadd_ps(b, c, a); }
inline f32x4 min(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_min_ps(a, b); }     // NaN/+-0: see above
inline f32x4 max(f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING { return _mm_max_ps(a, b); }
inline f32x4 abs(f32x4 a) noexcept FSDSP_NONBLOCKING          { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
inline f32x4 neg(f32x4 a) noexcept FSDSP_NONBLOCKING          { return _mm_xor_ps(a, _mm_set1_ps(-0.0f)); }
inline f32x4 sqrt(f32x4 a) noexcept FSDSP_NONBLOCKING         { return _mm_sqrt_ps(a); }
inline f32x4 floor(f32x4 a) noexcept FSDSP_NONBLOCKING        { return _mm_floor_ps(a); }

inline m32x4 gt  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING             { return _mm_cmpgt_ps(a, b); }
inline m32x4 ge  (f32x4 a, f32x4 b) noexcept FSDSP_NONBLOCKING             { return _mm_cmpge_ps(a, b); }
// blendv reads each mask lane's sign bit; an m32x4 is all-ones or all-zeros per lane, so this is NEON's bsl. The empty
// asm hides the mask's origin from the optimiser (FC ADR-92, found by FCompressor's first x86 run): Clang lowers
// sel(gt(a, b), a, b) to maxps (and the like to minps), equal under the default FP mode, but under DAZ maxps returns a
// denormal operand flushed to zero where bsl, and blendv, return its bits. Every spelling of the blend (float or
// integer blendv, and/andnot/or) is matched; the fence emits no instruction.
inline f32x4 sel (m32x4 mask, f32x4 t, f32x4 f) noexcept FSDSP_NONBLOCKING
{
    __asm__("" : "+x"(mask));
    return _mm_blendv_ps(f, t, mask);
}
inline m32x4 band(m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING             { return _mm_and_ps(a, b); }
inline m32x4 bor (m32x4 a, m32x4 b) noexcept FSDSP_NONBLOCKING             { return _mm_or_ps(a, b); }

template <int I> inline float lane(f32x4 v) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(I, I, I, I)));
}
template <int I> inline f32x4 withLane(f32x4 v, float s) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return _mm_insert_ps(v, _mm_set_ss(s), I << 4);
}

#endif

// ---- portable FRSQRTE / FRSQRTS (the x86 bodies; checked against the instructions on arm64 by dsp.simd) -------------
namespace detail {

// Arm ARM RecipSqrtEstimate(a) without FEAT_RPRES, 128 <= a < 512: the largest b with a * b^2 < 2^28 (the pseudocode's
// b = 512; while a*(b+1)*(b+1) < 2^28 do b = b+1), then r = (b + 1) DIV 2, in [256, 511].
constexpr uint32_t isqrtFloor(uint32_t n) noexcept FSDSP_NONBLOCKING
{
    uint32_t lo = 0, hi = 65536;                    // invariant: lo*lo <= n < hi*hi
    while (hi - lo > 1)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (static_cast<uint64_t>(mid) * mid <= n)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

constexpr uint32_t recipSqrtEstimate(uint32_t a) noexcept FSDSP_NONBLOCKING
{
    if (a < 256)
        a = a * 2 + 1;                              // 0.25 .. 0.5, in units of 1/512 rounded to nearest
    else
        a = ((a >> 1) << 1) * 2 + 2;                // 0.5 .. 1.0: bottom bit discarded, units of 1/256
    const uint32_t b = isqrtFloor(((1u << 28) - 1) / a);    // the largest b with a * b^2 <= 2^28 - 1
    return ((b < 512 ? 512 : b) + 1) / 2;
}

// FRSQRTE's 8-bit estimate, indexed by (biased exponent odd ? 0 : 128) + fraction<22:16>.
struct FrsqrteTable { uint8_t e[256]; };
constexpr FrsqrteTable makeFrsqrteTable() noexcept
{
    FrsqrteTable t{};
    for (uint32_t i = 0; i < 128; ++i)
    {
        t.e[i]       = static_cast<uint8_t>(recipSqrtEstimate(128 + i) - 256);      // odd exponent: '01':frac<22:16>
        t.e[128 + i] = static_cast<uint8_t>(recipSqrtEstimate(256 + 2 * i) - 256);  // even: '1':frac<22:15>, 15 dropped
    }
    return t;
}
inline constexpr FrsqrteTable kFrsqrte = makeFrsqrteTable();
static_assert(kFrsqrte.e[0] == 255 && kFrsqrte.e[128] == 105,
              "FRSQRTE(1.0f) = 0x3f7f8000 and FRSQRTE(2.0f) = 0x3f348000");

// FPRSqrtEstimate(operand) for binary32 with FPCR.AH = 0. flushDenormals is FPCR.FZ (x86: MXCSR.DAZ, the input half
// of flush-to-zero): a denormal input is then a signed zero. NaN -> the quietened input (FPCR.DN = 0), +-0 -> +-inf,
// a negative non-zero -> the default NaN, +inf -> +0.
constexpr uint32_t frsqrteBits(uint32_t x, bool flushDenormals) noexcept FSDSP_NONBLOCKING
{
    const uint32_t sign = x & 0x80000000u;
    uint32_t exp = (x >> 23) & 0xffu;
    uint32_t frac = x & 0x007fffffu;
    if (exp == 0xffu && frac != 0)
        return x | 0x00400000u;                     // NaN: quietened
    if (exp == 0 && (frac == 0 || flushDenormals))
        return sign | 0x7f800000u;                  // +-0 (or a flushed denormal): +-inf
    if (sign != 0)
        return 0x7fc00000u;                         // negative: default NaN
    if (exp == 0xffu)
        return 0;                                   // +inf: +0
    int e = static_cast<int>(exp);
    if (exp == 0)                                   // denormal, not flushed: normalise (the pseudocode's while loop)
    {
        while ((frac & 0x00400000u) == 0)
        {
            frac <<= 1;
            --e;
        }
        frac = (frac << 1) & 0x007fffffu;
    }
    const uint32_t idx = ((e & 1) != 0 ? 0u : 128u) + (frac >> 16);
    const int resultExp = (380 - e) / 2;            // 380 - e > 0, so / is the pseudocode's DIV
    return (static_cast<uint32_t>(resultExp) << 23) | (static_cast<uint32_t>(kFrsqrte.e[idx]) << 15);
}

// FPRSqrtStepFused(p, q) = (3 - p*q) / 2 with ONE rounding, (+-0 x +-inf) in either order -> +1.5. 1.5 - (p/2)*q is
// that value whenever p/2 is exact (|p| >= 2^-125, or p zero, infinite or NaN), and it cannot overflow where the
// instruction does not. Below 2^-125, |p*q| < 8, so (3 - p*q) rounded and then halved is exact: halving a result of
// magnitude >= 2^-46 (the smallest non-zero 3 - p*q can be there) never rounds. The zero and infinity tests use the FP
// mode, as the instruction does (a denormal compares equal to 0 under FZ / DAZ).
inline f32x4 rsqrtsPortable(f32x4 p, f32x4 q) noexcept FSDSP_NONBLOCKING
{
    const f32x4 zero = set1(0.0f), inf = set1(std::numeric_limits<float>::infinity());
    const f32x4 halfP = fms(set1(1.5f), mul(set1(0.5f), p), q);
    const f32x4 tinyP = mul(set1(0.5f), fms(set1(3.0f), p, q));
    const f32x4 r = sel(gt(set1(0x1p-125f), abs(p)), tinyP, halfP);
    const m32x4 zeroP = band(ge(p, zero), ge(zero, p)), zeroQ = band(ge(q, zero), ge(zero, q));
    const m32x4 infP = ge(abs(p), inf), infQ = ge(abs(q), inf);
    return sel(bor(band(zeroP, infQ), band(infP, zeroQ)), set1(1.5f), r);
}

} // namespace detail

#if defined(FSDSP_SIMD_SSE)

inline f32x4 rsqrte(f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const bool daz = (_mm_getcsr() & 0x0040u) != 0u;
    const __m128i v = _mm_castps_si128(x);
    const int e0 = static_cast<int>(detail::frsqrteBits(static_cast<uint32_t>(_mm_extract_epi32(v, 0)), daz));
    const int e1 = static_cast<int>(detail::frsqrteBits(static_cast<uint32_t>(_mm_extract_epi32(v, 1)), daz));
    const int e2 = static_cast<int>(detail::frsqrteBits(static_cast<uint32_t>(_mm_extract_epi32(v, 2)), daz));
    const int e3 = static_cast<int>(detail::frsqrteBits(static_cast<uint32_t>(_mm_extract_epi32(v, 3)), daz));
    return _mm_castsi128_ps(_mm_setr_epi32(e0, e1, e2, e3));
}
inline f32x4 rsqrts(f32x4 p, f32x4 q) noexcept FSDSP_NONBLOCKING { return detail::rsqrtsPortable(p, q); }

#endif

} // namespace fsdsp::simd

// ---- FSynth additions, 01 §7.2 (extracted; F0 makes them compile) ----
// ---- FSynth additions (declared at FZ0; F1, S1, writes the NEON bodies), appended to core/Simd.h. NEON bodies
// only: v1 is arm64 (Q4). ----
namespace fsdsp::simd {
#if defined(FSDSP_SIMD_NEON)
using u32x4 = uint32x4_t;
#else
#error "fsdsp::simd::u32x4 has NEON bodies only; v1 is arm64 (Q4)"
#endif
inline u32x4 set1u(uint32_t s) noexcept FSDSP_NONBLOCKING;                  // vdupq_n_u32
inline u32x4 addu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;             // wrapping (vaddq_u32)
inline u32x4 subu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;             // wrapping
inline u32x4 mulu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;             // low 32 bits (vmulq_u32)
inline u32x4 xoru(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;
inline u32x4 oru (u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;
template <int N> inline u32x4 shlu(u32x4 a) noexcept FSDSP_NONBLOCKING;    // vshlq_n_u32
template <int N> inline u32x4 shru(u32x4 a) noexcept FSDSP_NONBLOCKING;    // logical (vshrq_n_u32)
inline m32x4 ltu (u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING;             // unsigned a < b (vcltq_u32)
inline u32x4 selu(m32x4 mask, u32x4 t, u32x4 f) noexcept FSDSP_NONBLOCKING; // vbslq_u32
inline u32x4 cvtRound(f32x4 a) noexcept FSDSP_NONBLOCKING;                  // to int32, nearest-even, saturating (FCVTNS,
                                                                            //   vcvtnq_s32_f32), reinterpreted as uint32
inline f32x4 cvtHi24(u32x4 a) noexcept FSDSP_NONBLOCKING;                   // (float)(a >> 8): exact
inline f32x4 cvtSigned(u32x4 a) noexcept FSDSP_NONBLOCKING;                 // (float)(int32_t)a, nearest
template <int I> inline uint32_t laneu(u32x4 v) noexcept FSDSP_NONBLOCKING;
template <int I> inline u32x4 withLaneu(u32x4 v, uint32_t s) noexcept FSDSP_NONBLOCKING;
} // namespace fsdsp::simd
// ---- end of FSynth additions ----

// ==== Bodies of the FSynth additions (F1, S1; 01 §7.2) ===============================================================
//
// NEON only (Q4): the declarations above stop an x86-64 build with #error, so nothing below needs a second backend.
// Each op is one instruction, or two for the conversions that shift or reinterpret first. The integer ops are exact
// and wrap modulo 2^32; none of them reads the FP mode. The three conversions:
//   - cvtRound  FCVTNS: to the nearest int32, ties to even, whatever FPCR's rounding mode says; saturating at
//               INT32_MIN and INT32_MAX (+-inf included); NaN gives 0. Under FPCR.FZ a denormal input is 0, as it
//               rounds anyway.
//   - cvtHi24   UCVTF of a >> 8: the value is below 2^24, so the float holds it exactly.
//   - cvtSigned SCVTF of the bits read as int32: exact below 2^24 in magnitude, else rounded to the nearest float
//               (ties to even: fsdsp never changes FPCR's rounding mode), so 0x7fffffff gives 2^31.
// shlu<N> takes N in [0, 31] and shru<N> N in [0, 31] (0 returns the argument: the instruction's immediate starts at 1).
// On NEON u32x4 and m32x4 are the same type, so ltu's result feeds selu, sel and the mask logic directly.

namespace fsdsp::simd {

inline u32x4 set1u(uint32_t s) noexcept FSDSP_NONBLOCKING         { return vdupq_n_u32(s); }
inline u32x4 addu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING    { return vaddq_u32(a, b); }
inline u32x4 subu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING    { return vsubq_u32(a, b); }
inline u32x4 mulu(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING    { return vmulq_u32(a, b); }
inline u32x4 xoru(u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING    { return veorq_u32(a, b); }
inline u32x4 oru (u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING    { return vorrq_u32(a, b); }

template <int N> inline u32x4 shlu(u32x4 a) noexcept FSDSP_NONBLOCKING
{
    static_assert(N >= 0 && N < 32, "shift count out of range");
    return vshlq_n_u32(a, N);
}
template <int N> inline u32x4 shru(u32x4 a) noexcept FSDSP_NONBLOCKING
{
    static_assert(N >= 0 && N < 32, "shift count out of range");
    if constexpr (N == 0)
        return a;
    else
        return vshrq_n_u32(a, N);
}

inline m32x4 ltu (u32x4 a, u32x4 b) noexcept FSDSP_NONBLOCKING             { return vcltq_u32(a, b); }
inline u32x4 selu(m32x4 mask, u32x4 t, u32x4 f) noexcept FSDSP_NONBLOCKING { return vbslq_u32(mask, t, f); }

inline u32x4 cvtRound(f32x4 a) noexcept FSDSP_NONBLOCKING  { return vreinterpretq_u32_s32(vcvtnq_s32_f32(a)); }
inline f32x4 cvtHi24(u32x4 a) noexcept FSDSP_NONBLOCKING   { return vcvtq_f32_u32(vshrq_n_u32(a, 8)); }
inline f32x4 cvtSigned(u32x4 a) noexcept FSDSP_NONBLOCKING { return vcvtq_f32_s32(vreinterpretq_s32_u32(a)); }

template <int I> inline uint32_t laneu(u32x4 v) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return vgetq_lane_u32(v, I);
}
template <int I> inline u32x4 withLaneu(u32x4 v, uint32_t s) noexcept FSDSP_NONBLOCKING
{
    static_assert(I >= 0 && I < 4, "lane index out of range");
    return vsetq_lane_u32(s, v, I);
}

} // namespace fsdsp::simd
