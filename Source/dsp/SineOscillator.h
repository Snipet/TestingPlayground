// Source/dsp/SineOscillator.h: the legacy synth's oscillator (its SineOscillator: a float phase in radians through
// std::sin, one voice at a time), rebuilt on the DSP core for four voices at once, one per SIMD lane.
//
//   phase      a uint32 per voice: 2^32 is one cycle and the wrap is the integer overflow, so the phase never
//              accumulates an error and a held note never drifts (fsdsp::phase)
//   increment  a MIDI note's step per sample: fsdsp::phase::incFromPitch (within 0.002 cents of
//              440 * 2^((n - 69) / 12) Hz) rounded once to the uint32 grid, 2^-32 cycles per sample (the rounding is
//              largest for the lowest note at the highest rate: 0.0012 cents for MIDI 0 at 48 kHz, 0.0095 at 384 kHz)
//   wave       sin(2 pi phase) as fsdsp::sinPi of the signed phase: no libm on the audio thread
//
// To try another waveform, change wave(): it gets four phases (a cycle is 2^32) and returns four samples in [-1, 1].
// fsdsp::phase::toUnit gives the phase in [0, 1), toSigned in [-1, 1]; the voices, the envelope and the probes'
// pitch rows do not change. Mind aliasing: a waveform with harmonics (a saw, a square) needs band-limiting.
#pragma once

#include "fsdsp/core/FastMath.h"
#include "fsdsp/core/Phase.h"
#include "fsdsp/core/Simd.h"

#include <cmath>
#include <cstdint>

namespace testingplaygroundsynth
{
    struct SineOscillator4
    {
        fsdsp::simd::u32x4 phase = fsdsp::simd::set1u(0);   // per lane
        fsdsp::simd::u32x4 inc = fsdsp::simd::set1u(0);     // per lane and sample

        // The waveform, four lanes at once.
        static fsdsp::simd::f32x4 wave(fsdsp::simd::u32x4 p) noexcept
        {
            return fsdsp::sinPi(fsdsp::phase::toSigned(p));
        }

        // The next four samples; the phases advance by one sample.
        fsdsp::simd::f32x4 next() noexcept
        {
            const fsdsp::simd::f32x4 s = wave(phase);
            phase = fsdsp::simd::addu(phase, inc);
            return s;
        }

        // incFromPitch's offset for a sample rate: log2(440 / fs) - 69 / 12, in double, rounded once (prepare() only).
        static float pitchOffset(double fs) noexcept
        {
            return static_cast<float>(std::log2(440.0 / fs) - 69.0 / 12.0);
        }

        // A MIDI note's increment per sample (cycles x 2^32) at that offset. Allocation-free; called at note-on.
        static uint32_t increment(int note, float pitchOffset) noexcept
        {
            namespace simd = fsdsp::simd;
            const simd::f32x4 cycles = fsdsp::phase::incFromPitch(simd::set1(static_cast<float>(note)), pitchOffset);
            return simd::laneu<0>(fsdsp::phase::fromCycles(cycles));
        }
    };
}
