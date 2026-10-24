// Source/dsp/Controls.h: the program's dials (JUCE-free). The plugin has kMaxControls host parameters, P1 to P8, each
// normalised 0..1; the running program's first kMaxControls parameters (its main module's, in declaration order) take
// them, and a Control says how slot i's normalised value maps onto the program's parameter:
//
//   real     a float parameter: min + v (max - min), smoothed by the Engine and read sample by sample (a stream)
//   integer  an int: the same, rounded to the nearest integer, read once a call (a knob)
//   toggle   a bool: 1 from v = 0.5 up, else 0 (a knob)
//
// The range is the parameter's fminus::range, or [0, 1] for a float without one, [0, 10] for an int; its default its
// fminus::default, or the range's low end. Plain data, trivially copyable, so it crosses threads by value.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace testingplayground
{
    inline constexpr int kMaxControls = 8;

    enum class ControlType : uint8_t { real, integer, toggle };

    struct Control
    {
        char        name[32] = {};          // the parameter's name, as the program spells it; "" for an unused slot
        ControlType type = ControlType::real;
        float       min = 0.0f, max = 1.0f, def = 0.0f;
        bool        used = false;
    };

    using Controls = std::array<Control, kMaxControls>;

    // The plain value of a normalised one (v outside 0..1 is clamped first).
    inline float plainOf(const Control& c, float v) noexcept
    {
        const float t = std::clamp(v, 0.0f, 1.0f);
        switch (c.type)
        {
            case ControlType::toggle:  return t >= 0.5f ? 1.0f : 0.0f;
            case ControlType::integer: return std::round(c.min + t * (c.max - c.min));
            case ControlType::real:    break;
        }
        return c.min + t * (c.max - c.min);
    }

    // The normalised value of a plain one, clamped to the range.
    inline float normOf(const Control& c, float plain) noexcept
    {
        if (c.type == ControlType::toggle)
            return plain >= 0.5f ? 1.0f : 0.0f;
        const float span = c.max - c.min;
        if (!(span > 0.0f))
            return 0.0f;
        return std::clamp((plain - c.min) / span, 0.0f, 1.0f);
    }

    inline bool sameName(const Control& a, const Control& b) noexcept
    {
        return std::strcmp(a.name, b.name) == 0;
    }
}
