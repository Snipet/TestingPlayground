// Source/dsp/Hadamard.h: the FDN's feedback matrix, an orthonormal Hadamard matrix applied by a fast Walsh-Hadamard
// transform (JUCE-free).
//
// The legacy FDN declared a matrix order and no matrix (FDNCalculator.h: `matrix_order`); this is the family of
// HardwareReverb's FDN, which the legacy DelayLine came from: H_n (Sylvester order, n = 4, 8 or 16) scaled by
// 1 / sqrt(n), so it is orthogonal. An orthogonal mix keeps the energy of what circulates, so the loop decays exactly as
// the per-line gains say (DECAY), and every line feeds every other with equal weight.
//
// In place, n log2(n) additions: the butterflies of width 1 and 2 inside each group of four (scalar), those of width 4
// and 8 on whole vectors (fsdsp::simd), then the scaling. n = 4 and 16 scale by a power of two, exactly; n = 8 by the
// float nearest 1/sqrt(8), whose 3e-8 relative error changes the energy of a pass by under 1e-7.
#pragma once

#include "fsdsp/core/Simd.h"

namespace testingplayground
{
    // x[0 .. n) <- H_n x[0 .. n) / sqrt(n); n is 4, 8 or 16.
    inline void hadamard(float* x, int n) noexcept
    {
        namespace simd = fsdsp::simd;
        for (int i = 0; i < n; i += 4)
        {
            const float a = x[i], b = x[i + 1], c = x[i + 2], d = x[i + 3];
            const float ab = a + b, amb = a - b, cd = c + d, cmd = c - d;
            x[i] = ab + cd;
            x[i + 1] = amb + cmd;
            x[i + 2] = ab - cd;
            x[i + 3] = amb - cmd;
        }
        for (int h = 4; h < n; h <<= 1)
            for (int i = 0; i < n; i += 2 * h)
                for (int j = i; j < i + h; j += 4)
                {
                    const simd::f32x4 a = simd::load(x + j), b = simd::load(x + j + h);
                    simd::store(x + j, simd::add(a, b));
                    simd::store(x + j + h, simd::sub(a, b));
                }
        const simd::f32x4 scale = simd::set1(n == 16 ? 0.25f : (n == 8 ? 0.353553391f : 0.5f));
        for (int i = 0; i < n; i += 4)
            simd::store(x + i, simd::mul(simd::load(x + i), scale));
    }
}
