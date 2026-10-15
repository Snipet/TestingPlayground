#pragma once

// Transcendentals for the audio path (01 §2.3, §7.1, §7.3, §7.9; seeded from FCompressor: FC 01 §2.2 rule 4, §5.1;
// FC E §0.9, §3.2; FC K2 #14).
// The libm names are banned there because Apple's libm differs across arches and macOS releases; these are written
// with fma only, so arm64 and x86 produce the same bits and print.* hashes cannot move under an OS update.
//
// Frozen at FZ0 (names and signatures). The functions up to linFromDb are seeded with their bodies; F1 (S1) verifies
// them on FSynth's flags and writes the bodies of the additions declared after them (dsp.fastmath; 01 §7.9 has every
// target). The contract the seed gave its bodies:
//   - every scalar form is bit-identical to lane 0 of its vector form;
//   - log2: minimax polynomial, |err| <= 4e-6 log2 units (refit target 4e-7);
//   - exp2: input clamped to [-126, 126], relative error <= 9e-5 (refit target 1e-5);
//   - callers floor before log2: max(|x|, kLinFloor) and max(ms, kMsFloor) (FC 01 §5.7);
//   - every function is FSDSP_NONBLOCKING (core/Rt.h): an out-of-line definition repeats the macro.

#include "fsdsp/core/Rt.h"
#include "fsdsp/core/Simd.h"
#include <limits>

namespace fsdsp {

inline constexpr float kDbPerLog2 = 6.02059991f, kLog2PerDb = 0.166096404f;   // 20*log10(2) and its inverse
inline constexpr float kLinFloor = 1e-12f /*-240 dB*/, kMsFloor = 1e-24f;     // floors before log2 (FC 01 §5.7)

simd::f32x4 log2(simd::f32x4) noexcept FSDSP_NONBLOCKING;     // minimax poly; |err| <= 4e-6 (refit target 4e-7)
simd::f32x4 exp2(simd::f32x4) noexcept FSDSP_NONBLOCKING;     // input clamped to [-126, 126]; rel err <= 9e-5
float log2(float) noexcept FSDSP_NONBLOCKING;                 // == lane 0 of the vector form, bit-identical
float exp2(float) noexcept FSDSP_NONBLOCKING;

// libm replacements for shapers and per-tick filter design (targets fixed by FC's F1 spike; held by dsp.fastmath)
simd::f32x4 tanh(simd::f32x4) noexcept FSDSP_NONBLOCKING;     // 1 - 2/(exp2(2x*log2e) + 1); odd minimax for |x| < 0.125
simd::f32x4 logCosh(simd::f32x4) noexcept FSDSP_NONBLOCKING;  // |x| + ln2*log2(1 + exp2(-2|x|*log2e)) - ln 2 (ADAA-1
                                                              // antiderivative of tanh)
float tanh(float) noexcept FSDSP_NONBLOCKING;
float logCosh(float) noexcept FSDSP_NONBLOCKING;
float tanPi(float x) noexcept FSDSP_NONBLOCKING;              // tangent of pi*x, x in [0, 0.499]: SVF g = tanPi(fc/fs)
float sinPi(float x) noexcept FSDSP_NONBLOCKING;              // sine of pi*x, x in [-1, 1]
float cosPi(float x) noexcept FSDSP_NONBLOCKING;              // cosine of pi*x, x in [-1, 1]

inline simd::f32x4 dbFromLin(simd::f32x4 absx) noexcept FSDSP_NONBLOCKING;   // kDbPerLog2 * log2(max(|x|, kLinFloor))
inline simd::f32x4 dbFromMs (simd::f32x4 ms)   noexcept FSDSP_NONBLOCKING;   // 0.5*kDbPerLog2 * log2(max(ms, kMsFloor))
inline simd::f32x4 linFromDb(simd::f32x4 db)   noexcept FSDSP_NONBLOCKING;   // exp2(db * kLog2PerDb)

} // namespace fsdsp

// ==== Bodies of the seeded functions (FC's F1, S1) ===================================================================
//
// log2, exp2, tanh, logCosh and the three dB helpers are defined inline below: they run per sample, so non-LTO builds
// inline them too. The scalar tanPi, sinPi and cosPi run per control tick and are defined out of line in FastMath.cpp,
// as lane 0 of the vector forms at the end of this file.
// Every function is a fixed sequence of fsdsp::simd ops (fused fma, IEEE add/sub/mul/div, exact floor/abs/sel) plus
// exact integer exponent handling, so both backends produce the same bits; every scalar form is lane 0 of its vector
// form by construction (scalar(x) = lane<0>(vector(set1(x)))). Polynomials are minimax fits (Remez exchange, weighted
// for the error measure named at each one), coefficients rounded to float, evaluated by Horner with fused steps.
// Errors measured by FCompressor's dsp.simd over its sweeps (its spec rows hold them with ~20 % headroom) and again,
// over the same sweeps, by dsp.fastmath, which holds every result to FCompressor's bits (its seed.* rows) and the
// errors to 01 §7.9's figures:
//   log2      1.1e-7 log2 units for |x| in [1/2, 2) (refit target 4e-7); elsewhere at most half an ulp of the result
//             plus 7.7e-8, i.e. 2.0e-6 at exponent -40 = 1.2e-5 dB (FC E §0.9: 2.3e-5 dB). Exact on powers of two;
//             monotone over every float of exponents -40..40.
//   exp2      relative 1.71e-7 (1.709e-7 over the sweep, 1.7155e-7 over every float; refit target 1e-5; FC E §0.9:
//             8.5e-5); exact on integers, 2^n scaling exact.
//   tanh      absolute 1.6e-7; relative 6.7e-8 below |x| = 1/8, 1.1e-6 just above it (the 1 - 2/(e+1) form cancels).
//   logCosh   absolute 1.4e-7 beyond the result's rounding; relative 1.2e-7 below |x| = 1/8, 1.3e-5 just above it
//             (|x| - ln2 * |log2(...)| cancels there): an ADAA quotient over dx carries about 2.8e-7 / dx.
// Domains: log2 takes |x| of a positive normal float (callers floor first, FC 01 §5.7); zero and denormals give values
// in [-127, -126]; NaN and +-inf give NaN. exp2 clamps to [-126, 126] (results are normal) and returns NaN for NaN.
// tanh/logCosh give +-1 and +inf at +-inf and NaN for NaN. NaN in, NaN out everywhere, so poison is never laundered
// into a finite value before the poison check (01 §11.8; FC 01 §5.8).

namespace fsdsp {

namespace detail {

// x = 2^k * (1 + t) with 1 + t in [sqrt(1/2), sqrt(2)), from |x|'s bits (the sign bit is ignored). The biased
// exponent is taken relative to sqrt(1/2)'s bits (0x3f3504f3), so one integer subtract and an arithmetic shift give k
// and the mantissa lands on the centred interval where the polynomial is fitted; t = m - 1 is exact (Sterbenz).
struct Log2Split { simd::f32x4 k, t; };

inline Log2Split log2Split(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
#if defined(FSDSP_SIMD_NEON)
    const uint32x4_t ix = vsubq_u32(vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x7fffffffu)),
                                    vdupq_n_u32(0x3f3504f3u));
    const int32x4_t k = vshrq_n_s32(vreinterpretq_s32_u32(ix), 23);
    const float32x4_t m = vreinterpretq_f32_u32(vaddq_u32(vandq_u32(ix, vdupq_n_u32(0x007fffffu)),
                                                          vdupq_n_u32(0x3f3504f3u)));
    return { vcvtq_f32_s32(k), simd::sub(m, simd::set1(1.0f)) };
#else
    const __m128i ix = _mm_sub_epi32(_mm_and_si128(_mm_castps_si128(x), _mm_set1_epi32(0x7fffffff)),
                                     _mm_set1_epi32(0x3f3504f3));
    const __m128i k = _mm_srai_epi32(ix, 23);
    const __m128 m = _mm_castsi128_ps(_mm_add_epi32(_mm_and_si128(ix, _mm_set1_epi32(0x007fffff)),
                                                    _mm_set1_epi32(0x3f3504f3)));
    return { _mm_cvtepi32_ps(k), simd::sub(m, simd::set1(1.0f)) };
#endif
}

// p * 2^n for an integer-valued n with p * 2^n normal: n is added to p's exponent field (exact, no rounding).
inline simd::f32x4 scaleByPow2(simd::f32x4 p, simd::f32x4 n) noexcept FSDSP_NONBLOCKING
{
#if defined(FSDSP_SIMD_NEON)
    return vreinterpretq_f32_s32(vaddq_s32(vreinterpretq_s32_f32(p), vshlq_n_s32(vcvtq_s32_f32(n), 23)));
#else
    return _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(p), _mm_slli_epi32(_mm_cvttps_epi32(n), 23)));
#endif
}

// r with its sign bit flipped where x's is set: copysign(r, x) for r >= 0 (+-0, inf and NaN included).
inline simd::f32x4 xorSign(simd::f32x4 r, simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
#if defined(FSDSP_SIMD_NEON)
    return vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(r),
                                           vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u))));
#else
    return _mm_xor_ps(r, _mm_and_ps(x, _mm_set1_ps(-0.0f)));
#endif
}

} // namespace detail

// log2: k + t*q(t), q of degree 7 fitted to log2(1 + t)/t for the absolute error on [sqrt(1/2) - 1, sqrt(2) - 1]
// (4.8e-8). The final fused step k + t*q rounds once; adding x*0 (+-0 for every finite x, so exact) makes a NaN or an
// infinite input NaN instead of the finite value its bits would decode to, so poison reaches the poison check.
inline simd::f32x4 log2(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 poison = simd::mul(x, simd::set1(0.0f));
    const detail::Log2Split s = detail::log2Split(x);
    simd::f32x4 q = simd::set1(-0.145744577f);
    q = simd::fma(simd::set1(0.23689042f), q, s.t);
    q = simd::fma(simd::set1(-0.250069022f), q, s.t);
    q = simd::fma(simd::set1(0.286707461f), q, s.t);
    q = simd::fma(simd::set1(-0.360087216f), q, s.t);
    q = simd::fma(simd::set1(0.480939448f), q, s.t);
    q = simd::fma(simd::set1(-0.721357167f), q, s.t);
    q = simd::fma(simd::set1(1.44269478f), q, s.t);
    return simd::add(simd::fma(s.k, s.t, q), poison);
}

// exp2: x clamped to [-126, 126] (the constants are the FIRST operands, so a NaN stays NaN on x86 as on NEON),
// n = floor(x + 1/2), an integer, and f = x - n, exact, in [-1/2, 1/2] (a hair beyond where x + 1/2 rounds up to an
// integer); 2^f = 1 + f*r(f) with r of degree 4 fitted for the relative error (9.1e-8), then n added to the exponent
// field. n = -126 only with f >= 0, so the result is never denormal.
inline simd::f32x4 exp2(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    x = simd::min(simd::set1(126.0f), simd::max(simd::set1(-126.0f), x));
    const simd::f32x4 n = simd::floor(simd::add(x, simd::set1(0.5f)));
    const simd::f32x4 f = simd::sub(x, n);
    simd::f32x4 r = simd::set1(0.00132647273f);
    r = simd::fma(simd::set1(0.00967151299f), r, f);
    r = simd::fma(simd::set1(0.0555073358f), r, f);
    r = simd::fma(simd::set1(0.240222424f), r, f);
    r = simd::fma(simd::set1(0.693147004f), r, f);
    return detail::scaleByPow2(simd::fma(simd::set1(1.0f), f, r), n);
}

inline float log2(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::log2(simd::set1(x))); }
inline float exp2(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::exp2(simd::set1(x))); }

// tanh on |x|, then x's sign (so tanh(-x) == -tanh(x) exactly, zeros included): |x| < 1/8: |x| + |x|^3 * P(x^2),
// P = c3 + c5 x^2 fitted for the relative error (7.8e-9); otherwise 1 - 2/(exp2(2|x| log2 e) + 1).
inline simd::f32x4 tanh(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 ax = simd::abs(x), one = simd::set1(1.0f);
    const simd::f32x4 e = fsdsp::exp2(simd::mul(ax, simd::set1(2.88539008f)));             // e^(2|x|)
    const simd::f32x4 big = simd::sub(one, simd::div(simd::set1(2.0f), simd::add(e, one)));
    const simd::f32x4 x2 = simd::mul(ax, ax);
    const simd::f32x4 p = simd::fma(simd::set1(-0.333327711f), simd::set1(0.132167369f), x2);
    const simd::f32x4 small = simd::fma(ax, simd::mul(ax, x2), p);
    return detail::xorSign(simd::sel(simd::gt(simd::set1(0.125f), ax), small, big), x);
}

// logCosh: |x| < 1/8: u/2 + u^2 * S(u), u = x^2, S = s0 + s1 u fitted for the relative error (2.0e-9); even by
// construction. Otherwise |x| + ln2 * log2((1 + e^(-2|x|)) / 2), the design's |x| + ln2*log2(1 + e^(-2|x|)) - ln 2
// with the constant folded into the log's argument: (1 + e)/2 = 1/2 + e/2 rounds once and the separate - ln 2
// cancellation disappears. logCosh(-x) == logCosh(x) exactly.
inline simd::f32x4 logCosh(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 ax = simd::abs(x), half = simd::set1(0.5f);
    const simd::f32x4 e = fsdsp::exp2(simd::mul(ax, simd::set1(-2.88539008f)));            // e^(-2|x|)
    const simd::f32x4 big = simd::fma(ax, simd::set1(0.693147182f), fsdsp::log2(simd::fma(half, half, e)));
    const simd::f32x4 u = simd::mul(x, x);
    const simd::f32x4 s = simd::fma(simd::set1(-0.083332628f), simd::set1(0.022076292f), u);
    const simd::f32x4 small = simd::fma(simd::mul(half, u), simd::mul(u, u), s);
    return simd::sel(simd::gt(simd::set1(0.125f), ax), small, big);
}

inline float tanh(float x) noexcept FSDSP_NONBLOCKING    { return simd::lane<0>(fsdsp::tanh(simd::set1(x))); }
inline float logCosh(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::logCosh(simd::set1(x))); }

// The floors come FIRST in max(): a NaN input stays NaN on both backends (x86 maxps returns its second operand).
inline simd::f32x4 dbFromLin(simd::f32x4 absx) noexcept FSDSP_NONBLOCKING
{
    return simd::mul(simd::set1(kDbPerLog2), fsdsp::log2(simd::max(simd::set1(kLinFloor), simd::abs(absx))));
}

inline simd::f32x4 dbFromMs(simd::f32x4 ms) noexcept FSDSP_NONBLOCKING
{
    return simd::mul(simd::set1(0.5f * kDbPerLog2), fsdsp::log2(simd::max(simd::set1(kMsFloor), ms)));
}

inline simd::f32x4 linFromDb(simd::f32x4 db) noexcept FSDSP_NONBLOCKING
{
    return fsdsp::exp2(simd::mul(db, simd::set1(kLog2PerDb)));
}

// ---- FSynth additions, 01 §7.3 (extracted; F0 makes them compile) ----
// ---- FSynth additions (declared at FZ0; F1, S1, writes the bodies). Every scalar form is lane 0 of its vector form.
// sinPi, cosPi and tanPi keep the seeded scalar bits: the same reductions and polynomials (FastMath.cpp's
// sinPiPoly/cosPiPoly), moved inline into the header so a vector form exists (A §9; B rec. 2). Declared in namespace
// fsdsp; call them qualified (lint fsdsp.libm). ----
simd::f32x4 sinPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;           // sin(πx), x ∈ [−1, 1]
simd::f32x4 cosPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;           // cos(πx), x ∈ [−1, 1]
struct SinCos { simd::f32x4 s, c; };
SinCos      sincosPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;        // one reduction; s and c bit-identical to sinPi, cosPi
simd::f32x4 tanPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;           // tan(πx), x ∈ [0, 0.499]: per-sample cutoff prewarp
simd::f32x4 atan(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;            // every x, ±inf → ±π/2, NaN → NaN
simd::f32x4 atan2(simd::f32x4 y, simd::f32x4 x) noexcept FSDSP_NONBLOCKING;
float       atan(float x) noexcept FSDSP_NONBLOCKING;
float       atan2(float y, float x) noexcept FSDSP_NONBLOCKING;
// 2^x − 1 and log2(1 + x), accurate near 0 where exp2(x) − 1 and log2(1 + x) cancel (K2 #14): the envelope curves
// (§10.2) and the glide's EXP law (§9.5). |x| < 0.25: a short series; otherwise exp2(x) − 1 and log2(1 + x) exactly as
// FastMath forms them. exp2m1: x clamped to ±126. log2p1: x > −1 (−1 → −inf, below → NaN).
simd::f32x4 exp2m1(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;
simd::f32x4 log2p1(simd::f32x4 x) noexcept FSDSP_NONBLOCKING;
float       exp2m1(float x) noexcept FSDSP_NONBLOCKING;
float       log2p1(float x) noexcept FSDSP_NONBLOCKING;
// ---- end of FSynth additions ----

} // namespace fsdsp

// ==== Bodies of the FSynth additions (F1, S1; 01 §7.3, §7.9) =========================================================
//
// NEON only, as Simd.h's additions are (01 §7.2; Q4). Every vector form is a fixed sequence of fsdsp::simd ops; every
// scalar form is lane 0 of its vector form (scalar(x) = lane<0>(vector(set1(x)))), so the two cannot differ. The
// vector trig forms, atan, atan2, exp2m1 and log2p1 are inline here (they run per sample or per tick per lane); the
// seeded scalar tanPi, sinPi and cosPi stay out of line in FastMath.cpp and now return lane 0 of the vector forms,
// with the bits they had at the seed. Polynomials are minimax fits (Remez exchange for the error measure named at
// each), coefficients rounded to float, evaluated by Horner with fused steps. Errors measured by dsp.fastmath over
// its sweeps, against 01 §7.9's targets:
//   sinPi, cosPi   8.4e-8 absolute, 1.3e-7 relative (the seed's bounds; the vector forms are the seeded sequences)
//   sincosPi       s and c bit-identical to sinPi and cosPi: one fold, both polynomials, the same selects
//   tanPi          2.0e-7 relative over [0, 0.499] (2.04e-7 over the seed's sweep)
//   atan           1.5e-7 rad absolute (target 3e-7); odd exactly; a step back between consecutive floats of at
//                  most 1 ulp (target 2)
//   atan2          2.5e-7 rad absolute (target 4e-7); quadrants and signed zeros exact; atan2(y, 1) == atan(y) bit
//                  for bit
//   exp2m1         1.1e-7 relative for |x| < 1/4 (target 3e-7); elsewhere exp2(x) - 1, i.e. exp2's 1.7e-7 of 2^x plus
//                  the subtraction's rounding (exact while 2^x >= 1/2, at most half an ulp of the result below). At
//                  exactly +-1/4, the first floats of that branch, the error is 2.0e-7 and 3.0e-7 of the result.
//                  Steps back between consecutive floats stay within the bounds: 1.2e-7 of the result inside the
//                  series (one ulp), 8.4e-8 of 2^x elsewhere (exp2's own)
//   log2p1         1.2e-7 relative for |x| < 1/4 (target 3e-7); elsewhere log2(1 + x) as FastMath forms it: log2's
//                  bound at the float 1 + x rounds to (1.5e-7 and 3.3e-8 of the result at exactly +-1/4)
// Special values: atan(+-0) = +-0, atan(+-inf) = +-pi/2 (the float 0x3fc90fdb, 4.4e-8 above pi/2, as libm returns);
// atan2 follows IEEE 754 (atan2(+-0, +0) = +-0, atan2(+-0, -0) = +-pi, atan2(+-inf, +inf) = +-pi/4 within the error
// bound, and so on); exp2m1(+-0) = +-0 and log2p1(+-0) = +-0, and both carry the sign of x; exp2m1 clamps as exp2
// does (2^126 from 126 up; towards the negative end it is -1 from about -25 down, where 2^x is under half an ulp of
// 1); log2p1(-1) = -inf, log2p1(x < -1) = NaN, log2p1(+inf) = NaN (log2's rule for an infinite argument). NaN in, NaN
// out everywhere (01 §11.8), in either argument of atan2. Denormal arguments read as zero under FTZ, which fsdsp
// always runs under.

namespace fsdsp {

namespace detail {

// sin(pi y) = y * (s0 + s1 y^2 + s2 y^4 + s3 y^6) and cos(pi y) = 1 + y^2 * (c1 + c2 y^2 + c3 y^4 + c4 y^6) for
// |y| <= 1/4: FCompressor's fits (relative errors 3.2e-9 and 6.4e-11), the seed's sinPiPoly and cosPiPoly moved here
// from FastMath.cpp unchanged (01 §7.3), so that the vector forms and the scalar ones share them.
inline simd::f32x4 sinPiPoly(simd::f32x4 y) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 u = simd::mul(y, y);
    simd::f32x4 s = simd::set1(-0.589012206f);
    s = simd::fma(simd::set1(2.54976106f), s, u);
    s = simd::fma(simd::set1(-5.16770792f), s, u);
    s = simd::fma(simd::set1(3.14159274f), s, u);
    return simd::mul(y, s);
}

inline simd::f32x4 cosPiPoly(simd::f32x4 y) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 u = simd::mul(y, y);
    simd::f32x4 c = simd::set1(0.231364235f);
    c = simd::fma(simd::set1(-1.33505058f), c, u);
    c = simd::fma(simd::set1(4.05870771f), c, u);
    c = simd::fma(simd::set1(-4.93480206f), c, u);
    return simd::fma(simd::set1(1.0f), u, c);
}

// The fold sinPi and cosPi share, for x in [-1, 1]: a = |x|; b = a, or 1 - a where a > 1/2 (flip: cos changes sign
// there, sin does not); y = b, or 1/2 - b where b > 1/4 (high: the two polynomials trade places). Both reflections
// are exact (Sterbenz), so y in [0, 1/4] carries no error.
struct PiFold { simd::f32x4 y; simd::m32x4 flip, high; };

inline PiFold piFold(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 a = simd::abs(x);
    const simd::m32x4 flip = simd::gt(a, simd::set1(0.5f));
    const simd::f32x4 b = simd::sel(flip, simd::sub(simd::set1(1.0f), a), a);
    const simd::m32x4 high = simd::gt(b, simd::set1(0.25f));
    return { simd::sel(high, simd::sub(simd::set1(0.5f), b), b), flip, high };
}

// All-ones in every lane whose sign bit is set: negative values, -0 and negative NaNs.
inline simd::m32x4 signMask(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    return vcltzq_s32(vreinterpretq_s32_f32(x));
}

// atan(z)/z for z in [0, 1], from u = z^2: 1 + u*S(u) with S of degree 7 (the odd polynomial of degree 17), fitted for
// the absolute error of z + z^3 S(z^2) (7.4e-9; 1.5e-8 with the coefficients rounded to float). The leading 1 is exact,
// so z*P is z itself for a small z.
inline simd::f32x4 atanPoly(simd::f32x4 u) noexcept FSDSP_NONBLOCKING
{
    simd::f32x4 s = simd::set1(0.00262224465f);
    s = simd::fma(simd::set1(-0.0151325371f), s, u);
    s = simd::fma(simd::set1(0.0411218628f), s, u);
    s = simd::fma(simd::set1(-0.0736670643f), s, u);
    s = simd::fma(simd::set1(0.105739318f), s, u);
    s = simd::fma(simd::set1(-0.141859755f), s, u);
    s = simd::fma(simd::set1(0.199903965f), s, u);
    s = simd::fma(simd::set1(-0.333329856f), s, u);
    return simd::fma(simd::set1(1.0f), u, s);
}

// c +- z*atanPoly(z^2), fused: the one rounding of the result. c is 0, pi/2 or pi and z in [0, 1], so the result is
// never negative; negate chooses c - atan(z).
inline simd::f32x4 atanJoin(simd::f32x4 c, simd::f32x4 z, simd::m32x4 negate) noexcept FSDSP_NONBLOCKING
{
    return simd::fma(c, simd::sel(negate, simd::neg(z), z), atanPoly(simd::mul(z, z)));
}

inline constexpr float kHalfPiF = 1.57079637f, kPiF = 3.14159274f;     // the floats nearest pi/2 and pi

} // namespace detail

// sinPi, cosPi: the seeded reductions (see detail::piFold) and polynomials, as vectors. sin takes x's sign by a select
// on x < 0, cos the flip's, so sinPi(-x) == -sinPi(x) and cosPi(-x) == cosPi(x) exactly for x != 0.
inline simd::f32x4 sinPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const detail::PiFold f = detail::piFold(x);
    const simd::f32x4 r = simd::sel(f.high, detail::cosPiPoly(f.y), detail::sinPiPoly(f.y));
    return simd::sel(simd::gt(simd::set1(0.0f), x), simd::neg(r), r);
}

inline simd::f32x4 cosPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const detail::PiFold f = detail::piFold(x);
    const simd::f32x4 r = simd::sel(f.high, detail::sinPiPoly(f.y), detail::cosPiPoly(f.y));
    return simd::sel(f.flip, simd::neg(r), r);
}

// One fold and each polynomial once; the selects are sinPi's and cosPi's, so s and c have their bits.
inline SinCos sincosPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const detail::PiFold f = detail::piFold(x);
    const simd::f32x4 sp = detail::sinPiPoly(f.y), cp = detail::cosPiPoly(f.y);
    const simd::f32x4 rs = simd::sel(f.high, cp, sp), rc = simd::sel(f.high, sp, cp);
    return { simd::sel(simd::gt(simd::set1(0.0f), x), simd::neg(rs), rs), simd::sel(f.flip, simd::neg(rc), rc) };
}

// x in [0, 1/4]: sin/cos of pi x. x in (1/4, 1/2): tan(pi x) = cos(pi y)/sin(pi y) with y = 1/2 - x (exact). One IEEE
// division; the seeded sequence.
inline simd::f32x4 tanPi(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::m32x4 low = simd::ge(simd::set1(0.25f), x);
    const simd::f32x4 y = simd::sel(low, x, simd::sub(simd::set1(0.5f), x));
    const simd::f32x4 s = detail::sinPiPoly(y), c = detail::cosPiPoly(y);
    return simd::div(simd::sel(low, s, c), simd::sel(low, c, s));
}

// atan: a = |x|; z = a, or 1/a where a > 1 (IEEE division), in [0, 1]; the result is z*P(z^2), or pi/2 - z*P(z^2) where
// a > 1, formed by one fused step, then given x's sign (so atan is odd exactly, zeros included). 1/inf = 0 gives
// pi/2 at infinity; a NaN stays NaN through the division and the polynomial.
inline simd::f32x4 atan(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 a = simd::abs(x), one = simd::set1(1.0f);
    const simd::m32x4 big = simd::gt(a, one);
    const simd::f32x4 z = simd::sel(big, simd::div(one, a), a);
    const simd::f32x4 c = simd::sel(big, simd::set1(detail::kHalfPiF), simd::set1(0.0f));
    return detail::xorSign(detail::atanJoin(c, z, big), x);
}

// atan2: z = min(|x|, |y|) / max(|x|, |y|) in [0, 1] (NEON's min and max return NaN for a NaN operand, so a NaN in
// either argument reaches the result); 0/0 is replaced by 0 and inf/inf by 1, the angles IEEE 754 gives those pairs.
// The octant is resolved by selects: swap (|y| > |x|) measures from the y axis, a set sign bit of x (so -0 too) from
// the negative x axis; the result is c +- atan(z) with c in {0, pi/2, pi} in one fused step, then given y's sign bit.
inline simd::f32x4 atan2(simd::f32x4 y, simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 ax = simd::abs(x), ay = simd::abs(y), zero = simd::set1(0.0f);
    const simd::f32x4 num = simd::min(ax, ay), den = simd::max(ax, ay);
    simd::f32x4 z = simd::div(num, den);
    z = simd::sel(simd::ge(zero, den), zero, z);                                               // both zero
    z = simd::sel(simd::ge(num, simd::set1(std::numeric_limits<float>::infinity())), simd::set1(1.0f), z);  // both inf
    const simd::m32x4 swap = simd::gt(ay, ax), xneg = detail::signMask(x);
    const simd::f32x4 c = simd::sel(swap, simd::set1(detail::kHalfPiF),
                                    simd::sel(xneg, simd::set1(detail::kPiF), zero));
    return detail::xorSign(detail::atanJoin(c, z, simd::xoru(swap, xneg)), y);
}

inline float atan(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::atan(simd::set1(x))); }
inline float atan2(float y, float x) noexcept FSDSP_NONBLOCKING
{
    return simd::lane<0>(fsdsp::atan2(simd::set1(y), simd::set1(x)));
}

// exp2m1: |x| < 1/4: x*Q(x), Q of degree 5 fitted to (2^x - 1)/x for the relative error (1.7e-10; 2.9e-9 with float
// coefficients), so nothing cancels and the result has x's sign, +-0 included. Otherwise exp2(x) - 1 (exp2 clamps x to
// +-126). Both are evaluated and one is selected, per lane; a NaN fails the |x| < 1/4 test and comes back from exp2.
inline simd::f32x4 exp2m1(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 big = simd::sub(fsdsp::exp2(x), simd::set1(1.0f));
    simd::f32x4 q = simd::set1(0.000154035297f);
    q = simd::fma(simd::set1(0.0013347856f), q, x);
    q = simd::fma(simd::set1(0.0096181361f), q, x);
    q = simd::fma(simd::set1(0.0555040762f), q, x);
    q = simd::fma(simd::set1(0.240226507f), q, x);
    q = simd::fma(simd::set1(0.693147182f), q, x);
    return simd::sel(simd::gt(simd::set1(0.25f), simd::abs(x)), simd::mul(x, q), big);
}

// log2p1: |x| < 1/4: x*R(x), R of degree 8 fitted to log2(1 + x)/x for the relative error (1.7e-9; 2.0e-8 with float
// coefficients). Otherwise log2(1 + x), whose argument is positive and normal for every x > -1. log2 reads only the
// magnitude of its argument and floors a zero, so the two ends of the domain are selected: -inf at -1, NaN below.
inline simd::f32x4 log2p1(simd::f32x4 x) noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 minusOne = simd::set1(-1.0f);
    const simd::f32x4 big = fsdsp::log2(simd::add(simd::set1(1.0f), x));
    simd::f32x4 r = simd::set1(0.182493255f);
    r = simd::fma(simd::set1(-0.202359453f), r, x);
    r = simd::fma(simd::set1(0.204865217f), r, x);
    r = simd::fma(simd::set1(-0.239388779f), r, x);
    r = simd::fma(simd::set1(0.2885665f), r, x);
    r = simd::fma(simd::set1(-0.360692412f), r, x);
    r = simd::fma(simd::set1(0.480898142f), r, x);
    r = simd::fma(simd::set1(-0.721347451f), r, x);
    r = simd::fma(simd::set1(1.44269502f), r, x);
    simd::f32x4 y = simd::sel(simd::gt(simd::set1(0.25f), simd::abs(x)), simd::mul(x, r), big);
    y = simd::sel(simd::ge(minusOne, x), simd::set1(-std::numeric_limits<float>::infinity()), y);
    return simd::sel(simd::gt(minusOne, x), simd::set1(std::numeric_limits<float>::quiet_NaN()), y);
}

inline float exp2m1(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::exp2m1(simd::set1(x))); }
inline float log2p1(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::log2p1(simd::set1(x))); }

} // namespace fsdsp
