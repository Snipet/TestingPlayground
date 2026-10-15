// Smootherstep transitions (brief §4, §6, §11; FC 01 §5.1, S3 lead revision). Sprint-frozen (FZ0). A core header, so
// Mode kernels (blend-step ramps, unison copy fades) and the engine share one fade length.
#pragma once
#include "fsdsp/core/ControlTicker.h"
#include "fsdsp/core/Rt.h"
#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"
#include <cstdint>

namespace fsdsp {

inline constexpr float kFadeMs       = 20.0f;   // gains, crossfades, bypass, edge weights, Mode, kernel-role and preset
                                                //   fades, each half of a wet dip (§8.7), blend-step ramps, unison
                                                //   copy fades
inline constexpr float kStealFadeMs  = 2.0f;    // steal fades (F Table 2: −111 dB splatter above 8 kHz)
inline constexpr float kMinFadeGapMs = 50.0f;   // between Mode-crossfade and wet-dip starts, counted in samples

// Lengths in base-rate samples, whole ticks, so every state change sits on the tick grid [DECIDED]. Integer arithmetic:
// 960 at 48 kHz, 896 at 44.1 kHz (20.3 ms), 1,920 at 96 kHz; the gap 2,400 at 48 kHz. A voice-domain ramp lasts
// fadeSamples(fsBase)·osFactor run-rate samples. (graph/Transition.h uses these; §8.6.)
static_assert(kTickSamples == 16);
inline uint32_t fadeSamples(float fsBase) noexcept FSDSP_NONBLOCKING {
    const uint32_t hz = static_cast<uint32_t>(fsBase + 0.5f);
    const uint32_t n  = (hz * 20u + 999u) / 1000u;                  // ceil(20 ms)
    return (n + 15u) / 16u * 16u;                                    // whole ticks
}
inline uint32_t gapSamples(float fsBase) noexcept FSDSP_NONBLOCKING {
    const uint32_t hz = static_cast<uint32_t>(fsBase + 0.5f);
    return (hz * 50u + 999u) / 1000u;                                // ceil(50 ms), between crossfade and dip starts
}

// 6t⁵ − 15t⁴ + 10t³ with t clamped to [0, 1], evaluated as t³·(10 + t·(−15 + 6t)) with fused steps: exactly 0 and 1 at
// the ends. The exact sequence is the contract (it reaches every click row); the scalar form is lane 0. Over every float
// in [0, 1] [M]: absolute error 6.0e-7; rounding can step back by at most 5.4e-7 relative (no float form is strictly
// monotone), 126 dB below the value.
inline simd::f32x4 smootherstep(simd::f32x4 t) noexcept FSDSP_NONBLOCKING {
    t = simd::min(simd::set1(1.0f), simd::max(simd::set1(0.0f), t));
    const simd::f32x4 p = simd::fma(simd::set1(-15.0f), simd::set1(6.0f), t);   // −15 + 6t
    const simd::f32x4 q = simd::fma(simd::set1(10.0f), t, p);                    // 10 + t·p
    return simd::min(simd::set1(1.0f), simd::mul(simd::mul(simd::mul(t, t), t), q));
}
inline float smootherstep(float t) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(smootherstep(simd::set1(t))); }

// A LinearRamp read through smootherstep: every 20 ms transition that must not click. A linear fade measured +40/+48 dB
// on FC's click metric and smoothstep +3.3 dB; smootherstep passes (FC 01 §5.1).
struct SmoothRamp {
    LinearRamp lin{};
    void  prepare(float fs, float ms = kFadeMs) noexcept FSDSP_NONBLOCKING { lin.prepare(fs, ms); }
    void  setTarget(float t) noexcept FSDSP_NONBLOCKING                    { lin.setTarget(t); }
    float tick() noexcept FSDSP_NONBLOCKING                                { return smootherstep(lin.tick()); }
    bool  moving() const noexcept FSDSP_NONBLOCKING                        { return lin.moving(); }
};

// Per-lane 0 … 1 fades at one rate (steal fades, voice-bus fades): t moves by `step` per sample towards `tgt`, lands
// exactly, and tick() returns smootherstep(t).
struct LaneFade {
    simd::f32x4 t{}, tgt{}, step{};
    void prepare(float fs, float ms) noexcept FSDSP_NONBLOCKING;
    void setTarget(simd::m32x4 lanes, float target) noexcept FSDSP_NONBLOCKING;
    simd::f32x4 tick() noexcept FSDSP_NONBLOCKING;
};

// ---- Bodies (F1, S1; 01 §7.7, §7.9) ---------------------------------------------------------------------------------
// LaneFade is a linear ramp per lane, read through smootherstep. prepare() sets LinearRamp's step in every lane
// (1000 / (ms * fs), or 1, an instant fade, when the fade is shorter than a sample or ms * fs is <= 0 or NaN) and
// leaves t and tgt alone; setTarget() writes the target of the lanes whose mask is set and leaves the others; tick()
// moves each lane's t towards its tgt by step, lands it on tgt, and returns smootherstep(t), exactly 0 and 1 at the
// ends.
//
// Landing. A lane lands when a step would pass its target, or would leave less than half a step to go. The second
// rule makes the LENGTH exact, not only the end value: a fade over a distance of N steps lands on tick N as long as
// the float accumulation of the steps has drifted by less than half a step, where LinearRamp's clamp alone lands on
// tick N or N + 1. So a fade prepared for a whole number of samples N (ms * fs = 1000 N; prepare(N, 1000) says it
// exactly) ends on that sample: every 0 ... 1 fade of 1 to 4,096 samples does, up and down (dsp.units checks each),
// which is what puts a steal fade's end on its base-rate sample (01 §9.3: at most 768 run-rate samples) and covers a
// 20 ms fade at run rates up to 192 kHz. A float holds no more: the first length that lands a sample early is 5,859,
// so a 20 ms fade at 352.8 or 384 kHz (7,056 and 7,680 samples) may end one sample off, as LinearRamp's may anywhere.
// Until its last tick a lane's t is LinearRamp's, bit for bit; a landed lane stays (half a step is more than the 0
// left to go). Lanes are independent. The members are defined here, inline: tick() runs per sample.

inline void LaneFade::prepare(float fs, float ms) noexcept FSDSP_NONBLOCKING
{
    const float msFs = ms * fs;                             // the fade's length in samples, times 1000
    step = simd::set1(msFs > 1000.0f ? 1000.0f / msFs : 1.0f);
}

inline void LaneFade::setTarget(simd::m32x4 lanes, float target) noexcept FSDSP_NONBLOCKING
{
    tgt = simd::sel(lanes, simd::set1(target), tgt);
}

inline simd::f32x4 LaneFade::tick() noexcept FSDSP_NONBLOCKING
{
    const simd::f32x4 half = simd::mul(step, simd::set1(0.5f));
    const simd::f32x4 up = simd::add(t, step), down = simd::sub(t, step);
    const simd::f32x4 upLanded = simd::sel(simd::gt(half, simd::sub(tgt, up)), tgt, up);
    const simd::f32x4 downLanded = simd::sel(simd::gt(half, simd::sub(down, tgt)), tgt, down);
    t = simd::sel(simd::gt(tgt, t), upLanded, downLanded);
    return smootherstep(t);
}

} // namespace fsdsp
