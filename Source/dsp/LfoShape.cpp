// Source/dsp/LfoShape.cpp: see LfoShape.h.
#include "dsp/LfoShape.h"

#include <algorithm>
#include <cmath>

namespace testingplayground
{
    namespace
    {
        constexpr float kMinGap = 1e-3f;                // the legacy kInteriorEps: interior points stay this far apart

        float clamp01(float v) noexcept
        {
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.5f;
        }
    }

    bool LfoShape::operator==(const LfoShape& o) const noexcept
    {
        if (count != o.count)
            return false;
        for (int i = 0; i < count; ++i)
        {
            const ShapePoint& a = points[static_cast<std::size_t>(i)];
            const ShapePoint& b = o.points[static_cast<std::size_t>(i)];
            if (a.x != b.x || a.y != b.y || a.curve != b.curve)
                return false;
        }
        return true;
    }

    float applyCurve(float t, float curve) noexcept
    {
        if (std::fabs(curve) < 1e-6f)
            return t;
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

    float sampleCurve(const LfoShape& s, float x) noexcept
    {
        if (s.count <= 0)
            return 0.5f;
        const ShapePoint* p = s.points.data();
        const int n = s.count;
        if (x <= p[0].x)
            return p[0].y;
        if (x >= p[n - 1].x)
            return p[n - 1].y;
        for (int i = 1; i < n; ++i)
        {
            const ShapePoint& a = p[i - 1];
            const ShapePoint& b = p[i];
            if (x <= b.x)
            {
                const float span = b.x - a.x;
                if (span <= 1e-9f)
                    return b.y;
                const float t = (x - a.x) / span;
                return a.y + (b.y - a.y) * applyCurve(t, a.curve);
            }
        }
        return p[n - 1].y;
    }

    // f(1/2, a) = (1/2) / (a/2 + 1)^2 = m gives a = sqrt(2 / m) - 2 for m <= 1/2 (bend down); the mirror,
    // 1 - f(1/2, a) = m, gives a = sqrt(2 / (1 - m)) - 2 for m >= 1/2, stored negative (bend up).
    float curveThrough(float m) noexcept
    {
        m = std::clamp(std::isfinite(m) ? m : 0.5f, 0.02f, 0.98f);
        if (std::fabs(m - 0.5f) < 1e-4f)
            return 0.0f;
        if (m < 0.5f)
            return std::sqrt(2.0f / m) - 2.0f;
        return -(std::sqrt(2.0f / (1.0f - m)) - 2.0f);
    }

    LfoShape defaultShape() noexcept
    {
        LfoShape s;
        s.points[0] = { 0.0f, 0.5f, 0.0f };
        s.points[1] = { 0.25f, 1.0f, 0.0f };
        s.points[2] = { 0.75f, 0.0f, 0.0f };
        s.points[3] = { 1.0f, 0.5f, 0.0f };
        s.count = 4;
        return s;
    }

    bool sanitize(LfoShape& s) noexcept
    {
        const LfoShape before = s;
        int n = std::clamp(s.count, 0, LfoShape::kMaxPoints);
        auto* p = s.points.data();
        for (int i = 0; i < n; ++i)
        {
            p[i].x = clamp01(p[i].x);
            p[i].y = clamp01(p[i].y);
            p[i].curve = std::isfinite(p[i].curve)
                ? std::clamp(p[i].curve, -LfoShape::kMaxCurve, LfoShape::kMaxCurve) : 0.0f;
        }
        std::stable_sort(p, p + n, [](const ShapePoint& a, const ShapePoint& b) { return a.x < b.x; });
        if (n < 2)
        {
            s = defaultShape();
            return !(s == before);
        }
        // The edges: the first point at x = 0, the last at x = 1, both at the first point's y.
        p[0].x = 0.0f;
        p[n - 1].x = 1.0f;
        p[n - 1].y = p[0].y;
        p[n - 1].curve = 0.0f;                           // the last point has no segment after it
        // Interior points: at least kMinGap inside the edges. Two interior points may share an x (a vertical step; the
        // sort keeps their order), so a drag never loses the point it carries past another.
        for (int i = 1; i < n - 1; ++i)
            p[i].x = std::clamp(p[i].x, kMinGap, 1.0f - kMinGap);
        for (int i = n; i < LfoShape::kMaxPoints; ++i)
            p[i] = ShapePoint{};
        s.count = n;
        return !(s == before);
    }

    void renderTable(const LfoShape& s, LfoTable& out) noexcept
    {
        for (int i = 0; i < kLfoTableSize; ++i)
        {
            const float x = static_cast<float>(i) / static_cast<float>(kLfoTableSize);
            out.v[static_cast<std::size_t>(i)] = 2.0f * sampleCurve(s, x) - 1.0f;
        }
        out.v[kLfoTableSize] = out.v[0];
    }
}
