// Source/dsp/Controls.h: the program's dials (JUCE-free). The plugin has kMaxControls host parameters, P1 to P32, each
// normalised 0..1; the running program's first kMaxControls parameters (its main module's, in declaration order) take
// them, and a Control says how slot i's normalised value maps onto the program's parameter:
//
//   real     a float parameter: min + v (max - min), smoothed by the Engine and read sample by sample (a stream);
//            with a log taper (a .gui file's taper=log, for a frequency or a time) min (max / min)^v instead, so
//            each octave takes the same turn of the knob
//   integer  an int: the same, rounded to the nearest integer, read once a call (a knob)
//   toggle   a bool: 1 from v = 0.5 up, else 0 (a knob)
//
// The range is the parameter's fminus::range, or [0, 1] for a float without one, [0, 10] for an int; its default its
// fminus::default, or the range's low end. How a real value reads (the panel's and the host's text): times `scale`,
// with `decimals` (by its size when -1) and `unit` after it, a frequency in Hz from 1000 up in kHz (formatValue). Plain
// data, trivially copyable, so it crosses threads by value.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace testingplayground
{
    inline constexpr int kMaxControls = 32;
    inline constexpr int kFirstControls = 16;          // the parameters of the plugin's first version (P1 to P16)

    enum class ControlType : uint8_t { real, integer, toggle };
    enum class ControlTaper : uint8_t { linear, log };   // log: a real one whose min is above 0

    struct Control
    {
        char         name[32] = {};          // the parameter's name, as the program spells it; "" for an unused slot
        ControlType  type = ControlType::real;
        float        min = 0.0f, max = 1.0f, def = 0.0f;
        bool         used = false;
        ControlTaper taper = ControlTaper::linear;
        char         unit[8] = {};           // after the value ("Hz", "dB", "ms", "%"), "" for none
        int8_t       decimals = -1;          // a real value's, -1: by its size (2 under 10, 1 under 100, then 0)
        float        scale = 1.0f;           // the value shown is the plain one times it (0..1 as 0..100 %)
    };

    // Whether a log taper fits c: a real control whose range is above 0.
    inline bool logTaperFits(const Control& c) noexcept
    {
        return c.type == ControlType::real && c.min > 0.0f && c.max > c.min;
    }

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
        if (c.taper == ControlTaper::log && logTaperFits(c))
            return std::clamp(c.min * std::exp(t * std::log(c.max / c.min)), c.min, c.max);
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
        if (c.type == ControlType::real && c.taper == ControlTaper::log && logTaperFits(c))
            return plain > c.min ? std::clamp(std::log(plain / c.min) / std::log(c.max / c.min), 0.0f, 1.0f) : 0.0f;
        return std::clamp((plain - c.min) / span, 0.0f, 1.0f);
    }

    // A real control's plain value as the panel and the host show it (formatValue's rules, above).
    inline void formatValue(const Control& c, float plain, char* out, std::size_t n) noexcept
    {
        double v = static_cast<double>(plain) * static_cast<double>(c.scale);
        const char* unit = c.unit;
        if (std::strcmp(unit, "Hz") == 0 && std::fabs(v) >= 1000.0)
        {
            v /= 1000.0;
            unit = "kHz";
        }
        const double a = std::fabs(v);
        const int d = c.decimals >= 0 ? std::min<int>(c.decimals, 4) : (a >= 100.0 ? 0 : (a >= 10.0 ? 1 : 2));
        std::snprintf(out, n, "%.*f%s%s", d, v, unit[0] != '\0' ? " " : "", unit);
    }

    // The plain value of a text a host or a user typed for a real control: its number, divided by the scale, in Hz
    // times 1000 when it says k ("1.2k", "1.2 kHz"). NaN when there is no number.
    inline float parseValue(const Control& c, const char* text) noexcept
    {
        char* end = nullptr;
        double v = std::strtod(text, &end);
        if (end == text || !std::isfinite(v))
            return std::nanf("");
        while (*end == ' ')
            ++end;
        if (*end == 'k' || *end == 'K')
            v *= 1000.0;
        if (c.scale != 0.0f)
            v /= static_cast<double>(c.scale);
        return static_cast<float>(v);
    }

    inline bool sameName(const Control& a, const Control& b) noexcept
    {
        return std::strcmp(a.name, b.name) == 0;
    }
}
