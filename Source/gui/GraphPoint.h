#pragma once

#include <cmath>
#include <cstddef>

namespace graph
{
    // A control point on the LFO graph.
    //
    // x, y are normalised to [0, 1] (screen-independent). curve shapes the
    // line segment between this point and the next point in x-order:
    //   curve == 0     → straight line
    //   curve  > 0     → ease-in  (slow start, fast end)
    //   curve  < 0     → ease-out (fast start, slow end)
    struct Point
    {
        float x = 0.0f;
        float y = 0.5f;
        float curve = 0.0f;
    };

    // Curve mapping. The underlying formula is
    //
    //     f(t, a) = t / (a*t - (1+a))^2          (a >= 0)
    //
    // which is monotonic on [0, 1] for any non-negative `a` and always
    // produces a midpoint at or below the linear midpoint (= "bend down").
    // For "bend up" we don't feed a negative `a` to the formula (that
    // breaks monotonicity and overshoots); we reflect instead:
    //
    //     f_up(t, a) = 1 - f(1 - t, a)            (a >= 0)
    //
    // The sign of `curve` selects which branch:
    //   curve == 0   →  linear
    //   curve  > 0   →  bend DOWN, using f(t, |curve|)
    //   curve  < 0   →  bend UP,   using 1 - f(1 - t, |curve|)
    //
    // The formula itself only ever sees a non-negative argument, so the
    // renderer always operates in the well-behaved region.
    inline float applyCurve(float t, float curve) noexcept
    {
        if (std::fabs(curve) < 1e-6f) return t;
        const float a = std::fabs(curve);
        if (curve > 0.0f)
        {
            const float d = a * t - (1.0f + a);
            return t / (d * d);
        }
        const float u = 1.0f - t;
        const float d = a * u - (1.0f + a);
        return 1.0f - u / (d * d);
    }

    // Sample the piecewise curve defined by a sorted (by x) point list at
    // normalised x. Returns 0 if there are no points.
    template <typename PointsT>
    inline float sampleCurve(const PointsT& pts, float x) noexcept
    {
        if (pts.empty()) return 0.0f;
        if (x <= pts.front().x) return pts.front().y;
        if (x >= pts.back().x)  return pts.back().y;

        // Linear scan is fine — typical LFOs have <32 points.
        for (size_t i = 1; i < pts.size(); ++i)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];
            if (x <= b.x)
            {
                const float span = b.x - a.x;
                if (span <= 1e-9f) return b.y;
                const float t      = (x - a.x) / span;
                const float curved = applyCurve(t, a.curve);
                return a.y + (b.y - a.y) * curved;
            }
        }
        return pts.back().y;
    }
}
