// Source/dsp/LfoShape.h: the modulation LFO's shape, a curve through points, and the table the audio thread reads
// (JUCE-free).
//
// Ported from the legacy graph editor's data model (git show legacy-visage:Source/gui/GraphPoint.h): points with x and
// y in [0, 1] and a `curve` that bends the segment to the next point, with the legacy's curve formula unchanged
// (applyCurve, sampleCurve, and the editor's inverse curveThrough). The graph editor (Source/editor/ShapeView.cpp)
// edits an LfoShape; the processor keeps it in the plugin state and hands its rendered table to the Engine, which reads
// one cycle of it per LFO period, y = 0.5 being no modulation.
//
// Rules a valid shape keeps (sanitize() restores them): 2 to kMaxPoints points, sorted by x, the first at x = 0 and the
// last at x = 1 with the same y (the cycle wraps there, and a step at the wrap would be a jump of the delay time),
// interior points within [1e-3, 1 - 1e-3] (two may share an x: a vertical step), every y in [0, 1] and every curve in
// [-kMaxCurve, kMaxCurve].
//
// Message thread (or any thread but the audio thread): renderTable() uses the standard library's math. The audio
// thread only reads a rendered LfoTable.
#pragma once

#include <array>
#include <cstddef>

namespace testingplayground
{
    struct ShapePoint
    {
        float x = 0.0f;
        float y = 0.5f;
        float curve = 0.0f;              // bends the segment to the next point: 0 straight, > 0 down, < 0 up
    };

    struct LfoShape
    {
        static constexpr int kMaxPoints = 32;
        static constexpr float kMaxCurve = 8.0f;       // curveThrough(0.02) = sqrt(2 / 0.02) - 2: a drag's limit

        std::array<ShapePoint, kMaxPoints> points{};
        int count = 0;

        bool operator==(const LfoShape& o) const noexcept;
    };

    inline constexpr int kLfoTableSize = 256;           // one cycle; the Engine interpolates linearly

    // One cycle of an LfoShape as bipolar values, 2 y - 1 in [-1, 1], at x = i / kLfoTableSize; v[kLfoTableSize] repeats
    // v[0], so a read at i + 1 never wraps.
    struct LfoTable
    {
        std::array<float, kLfoTableSize + 1> v{};
    };

    // The legacy curve mapping, f(t, a) = t / (a t - (1 + a))^2 for a = |curve| (monotonic on [0, 1], below the
    // diagonal), reflected as 1 - f(1 - t, a) for a negative curve; exactly t for |curve| < 1e-6.
    float applyCurve(float t, float curve) noexcept;

    // The shape's value at x in [0, 1] (the first point's y left of it, the last point's right of it).
    float sampleCurve(const LfoShape& s, float x) noexcept;

    // The curve that puts the segment's midpoint at the fraction m of its rise (the legacy midpoint drag's solution):
    // m is clamped to [0.02, 0.98]; 0.5 is straight.
    float curveThrough(float m) noexcept;

    // The default: a triangle, 0 at the start, up to +1 at a quarter, down to -1 at three quarters, back to 0.
    LfoShape defaultShape() noexcept;

    // Restores the rules above: sorts (stably), clamps, pins the edge points (their y becomes the first point's) and
    // falls back to defaultShape() for fewer than two points; it never drops a point. Returns whether it changed s.
    bool sanitize(LfoShape& s) noexcept;

    void renderTable(const LfoShape& s, LfoTable& out) noexcept;
}
