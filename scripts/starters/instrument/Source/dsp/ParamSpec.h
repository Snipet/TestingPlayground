// Source/dsp/ParamSpec.h: what a parameter is, for the table in Params.h. JUCE-free, like everything in Source/dsp:
// the processor builds its JUCE parameters from the table, the editor builds its controls from it, and the Engine
// reads plain values in table order.
//
// The mapping between a plain value and the host's normalised 0..1 is JUCE's NormalisableRange with a skew
// (setSkewForCentre): `centre` is the value at the middle of the control's travel, so (min + max) / 2 is linear and a
// lower centre gives the low end more travel (frequencies, times). toNormalised/fromNormalised are that mapping, here
// so the editor and the probes can use it without JUCE; the probe plugin.state holds them to JUCE's own.
#pragma once

#include <cmath>
#include <cstdint>

namespace testingplaygroundsynth
{
    enum class Unit : uint8_t { none, db, hz, ms, percent };        // percent: the plain value is 0..100

    enum class Control : uint8_t
    {
        dial,       // funkgui::Dial
        slider,     // funkgui::RuleSlider
        choice,     // funkgui::SegmentedSelector, one cell per choice (choices / numChoices)
        hidden,     // a host parameter without a control on the panel
    };

    struct ParamSpec
    {
        const char* id = "";              // the parameter ID: hosts store it in sessions, so never change it later
        const char* name = "";            // the host's name
        const char* label = "";           // the panel's label: upper case, short
        Unit  unit = Unit::none;
        float min = 0.0f;
        float max = 1.0f;
        float def = 0.0f;                 // the default (double-click on a control returns to it)
        float centre = 0.5f;              // the value at the middle of the travel; (min + max) / 2 = linear
        Control control = Control::dial;
        const char* const* choices = nullptr;   // Control::choice: the names (upper case); min 0, max count - 1
        int numChoices = 0;
    };

    // JUCE's NormalisableRange::setSkewForCentre: 1 (linear) when the centre is the midpoint or outside the range.
    inline float skewOf(const ParamSpec& s) noexcept
    {
        const float span = s.max - s.min;
        const float c = span > 0.0f ? (s.centre - s.min) / span : 0.5f;
        if (!(c > 0.0f && c < 1.0f) || c == 0.5f)
            return 1.0f;
        return std::log(0.5f) / std::log(c);
    }

    // Plain value -> host-normalised 0..1, as NormalisableRange::convertTo0to1 (clamped).
    inline float toNormalised(const ParamSpec& s, float plain) noexcept
    {
        const float span = s.max - s.min;
        float p = span > 0.0f ? (plain - s.min) / span : 0.0f;
        p = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
        const float skew = skewOf(s);
        return skew == 1.0f ? p : std::pow(p, skew);
    }

    // Host-normalised 0..1 -> plain value, as NormalisableRange::convertFrom0to1 (clamped).
    inline float fromNormalised(const ParamSpec& s, float norm) noexcept
    {
        float p = norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
        const float skew = skewOf(s);
        if (skew != 1.0f && p > 0.0f)
            p = std::exp(std::log(p) / skew);
        return s.min + (s.max - s.min) * p;
    }

    // A dial draws its arc from 12:00 when 0 sits there (a gain in dB from -24 to +24).
    inline bool isBipolar(const ParamSpec& s) noexcept
    {
        return s.min < 0.0f && s.max > 0.0f && std::fabs(toNormalised(s, 0.0f) - 0.5f) < 1e-6f;
    }
}
