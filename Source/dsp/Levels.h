// Source/dsp/Levels.h: a block's peak and energy, for the meters (Engine.cpp's levels: each side of the input and the
// output, every block while someone reads the meters).
//
// blockLevels(x, n) is the largest |x[i]| and the sum of x[i] squared (in double), a sample that is not finite counted
// as 0. It keeps kLevelLanes maxima and partial sums side by side, sample i in lane i % kLevelLanes, and adds the lanes
// at the end, so the compiler makes vector code of it: one chain of double additions, a sample after the other, was
// its cost before, which the compiler may not reorder itself (no fast math). The peak is a serial pass's to the bit;
// the energy differs from a serial sum in its rounding only (dsp.meters LANES).
//
// JUCE-free, header-only; the audio thread's: no allocation.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace playground
{
    inline constexpr int kLevelLanes = 8;

    struct BlockLevels
    {
        float peak = 0.0f;                               // the largest magnitude
        double energy = 0.0;                             // the sum of the squares
    };

    inline BlockLevels blockLevels(const float* x, int n) noexcept
    {
        float peak[kLevelLanes] = {};
        double energy[kLevelLanes] = {};
        const auto add = [&](int lane, float s) {
            const float a = std::fabs(s);
            const float v = a <= std::numeric_limits<float>::max() ? a : 0.0f;   // NaN, an infinity: 0
            peak[lane] = std::max(peak[lane], v);
            energy[lane] += static_cast<double>(v) * static_cast<double>(v);
        };
        int i = 0;
        for (; i + kLevelLanes <= n; i += kLevelLanes)
            for (int k = 0; k < kLevelLanes; ++k)
                add(k, x[i + k]);
        for (int k = 0; i < n; ++i, ++k)
            add(k, x[i]);
        BlockLevels out;
        for (int k = 0; k < kLevelLanes; ++k)
            out.peak = std::max(out.peak, peak[k]);
        for (int w = kLevelLanes / 2; w > 0; w /= 2)    // the lanes added in pairs
            for (int k = 0; k < w; ++k)
                energy[k] += energy[k + w];
        out.energy = energy[0];
        return out;
    }
}
