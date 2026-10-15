// The exact voice sum (brief §4; D §7, rec. 10). Sprint-frozen (FZ0). Bodies inline in this header (F1).
#pragma once
#include "fsdsp/core/Rt.h"
#include "fsdsp/core/Simd.h"
#include <cstdint>

namespace fsdsp {

inline constexpr double kBusScale = 0x1p32;      // one accumulator unit = 2^−32 of full scale (−193 dBFS)

// Adds one quad's stereo span to a bus: each lane's sample (ENV 1, voice.pan and the bus input already applied by the
// caller) is widened to double (exact), scaled by 2^32 (exact) and rounded to int64 by FCVTNS (vcvtnq_s64_f64: nearest,
// ties to even) — an instruction, never std::llrint or std::lround (lint fsdsp.rint). Lanes outside `mask` are selected
// to 0 before the conversion, so nothing from an idle lane, NaN included, can leak. Inputs are finite and |x| ≤ 1e6
// (sanitised upstream), so 72 lanes cannot overflow the int64.
inline void busAdd(int64_t* accL, int64_t* accR, const simd::f32x4* l, const simd::f32x4* r, simd::m32x4 mask, int n)
    noexcept FSDSP_NONBLOCKING;
// acc·2^−32 → float: int64 → double (nearest), exact scaling, double → float (nearest). Deterministic.
inline void busRead(const int64_t* acc, float* out, int n) noexcept FSDSP_NONBLOCKING;
// One conversion as busAdd performs it, for probes.
inline int64_t toBusUnits(float x) noexcept FSDSP_NONBLOCKING;

// ---- Bodies (F1, S1; 01 §7.6, §7.9) ---------------------------------------------------------------------------------
// NEON only, as the u32x4 lanes are (01 §7.2; Q4): the double and int64 lanes are used directly, because fsdsp::simd
// has no 64-bit type. Per lane: FCVT float -> double (exact), FMUL by 2^32 (exact: a power of two, far from overflow
// and underflow), FCVTNS double -> int64 (nearest, ties to even, in any FPCR rounding mode; a NaN would give 0 and an
// infinity would saturate, but the caller sanitises). The four int64 lanes of a sample are added to its accumulator
// with integer adds, which commute, so neither the lane a voice sits in nor the order of the busAdd calls can change
// a bus. Each voice is off by at most 2^-33 of full scale; a magnitude below 2^-33 (a denormal float among them,
// with or without FTZ) rounds to 0.
//
// busRead: SCVTF int64 -> double (exact below 2^53 units, i.e. below 2^21 = +126 dBFS; nearest above), FMUL by 2^-32
// (exact), FCVT double -> float (nearest). Plain conversions and one multiply: the compiler may vectorise the loop,
// the values cannot change.

#if !defined(FSDSP_SIMD_NEON)
  #error "fsdsp::busAdd has a NEON body only; v1 is arm64 (Q4)"
#endif

namespace detail {
// a + b modulo 2^64. Within busAdd's contract the sum never leaves the int64, so this is the plain sum; outside it
// (an unsanitised infinity saturates the conversion) it wraps as the vector adds do, instead of being undefined.
inline int64_t busWrapAdd(int64_t a, int64_t b) noexcept FSDSP_NONBLOCKING
{
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
} // namespace detail

inline void busAdd(int64_t* accL, int64_t* accR, const simd::f32x4* l, const simd::f32x4* r, simd::m32x4 mask, int n)
    noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 zero = simd::set1(0.0f);
    const float64x2_t scale = vdupq_n_f64(kBusScale);
    for (int i = 0; i < n; ++i)
    {
        const simd::f32x4 xl = simd::sel(mask, l[i], zero), xr = simd::sel(mask, r[i], zero);
        const int64x2_t l01 = vcvtnq_s64_f64(vmulq_f64(vcvt_f64_f32(vget_low_f32(xl)), scale));
        const int64x2_t l23 = vcvtnq_s64_f64(vmulq_f64(vcvt_high_f64_f32(xl), scale));
        const int64x2_t r01 = vcvtnq_s64_f64(vmulq_f64(vcvt_f64_f32(vget_low_f32(xr)), scale));
        const int64x2_t r23 = vcvtnq_s64_f64(vmulq_f64(vcvt_high_f64_f32(xr), scale));
        accL[i] = detail::busWrapAdd(accL[i], vaddvq_s64(vaddq_s64(l01, l23)));
        accR[i] = detail::busWrapAdd(accR[i], vaddvq_s64(vaddq_s64(r01, r23)));
    }
}

inline void busRead(const int64_t* acc, float* out, int n) noexcept FSDSP_NONBLOCKING
{
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<float>(static_cast<double>(acc[i]) * (1.0 / kBusScale));
}

inline int64_t toBusUnits(float x) noexcept FSDSP_NONBLOCKING
{
    return vcvtnd_s64_f64(static_cast<double>(x) * kBusScale);
}

} // namespace fsdsp
