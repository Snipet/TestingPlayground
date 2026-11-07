// Source/dsp/Controls.h: the program's dials (JUCE-free). The running program's first kMaxControls parameters (its
// main module's, in declaration order; then its circuits' dials, Patch.h) are its dials, a slot each, each normalised
// 0..1. The first kHostControls slots are the plugin's host parameters, P1 to P32, which a host sees and automates;
// the rest, slots 32 to 63 (isPanelDial), are the panel's own dials: the panel, Claude, a session, a project's versions
// and presets set them as they set the others, and a host never sees them (no parameter, no automation). A Control says
// how slot i's normalised value maps onto the program's parameter:
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
#include <string>
#include <vector>

namespace playground
{
    inline constexpr int kHostControls = 32;           // the host's parameters, P1 to P32: slots 0 to 31
    inline constexpr int kPanelControls = 32;          // the panel's own dials after them: slots 32 to 63
    inline constexpr int kMaxControls = kHostControls + kPanelControls;
    inline constexpr int kFirstControls = 16;          // the parameters of the plugin's first version (P1 to P16)

    // Whether slot c is a panel dial (above): no host parameter, never automated.
    inline constexpr bool isPanelDial(int c) noexcept { return c >= kHostControls && c < kMaxControls; }
    // A slot's bit in a mask of slots (uint64_t: one a slot).
    inline constexpr uint64_t slotBit(int c) noexcept { return uint64_t{ 1 } << static_cast<unsigned>(c); }
    static_assert(kMaxControls <= 64, "a mask of slots holds a bit a slot");

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

    // A dial's plain value as a person reads it: a real one as formatValue writes it; an int or a bool by the name its
    // options give that value (the panel's options, from the range's low end), else as a number, or off and on.
    inline std::string dialText(const Control& c, float plain, const std::vector<std::string>& options = {})
    {
        if (!std::isfinite(plain))
            return "-";
        if (c.type != ControlType::real)
        {
            const long k = std::lround(plain) - std::lround(c.min);
            if (k >= 0 && static_cast<std::size_t>(k) < options.size() && !options[static_cast<std::size_t>(k)].empty())
                return options[static_cast<std::size_t>(k)];
            if (c.type == ControlType::toggle)
                return plain >= 0.5f ? "on" : "off";
            return std::to_string(std::lround(plain));
        }
        char buf[48];
        formatValue(c, plain, buf, sizeof buf);
        return buf;
    }

    inline bool sameName(const Control& a, const Control& b) noexcept
    {
        return std::strcmp(a.name, b.name) == 0;
    }

    // ---- a dial's value across a declaration change ----------------------------------------------------------------
    // A run that brings a parameter of the name the slot had keeps the dial's value in its units, never its normalised
    // position (which a new range or taper would move): the plain value, clamped to the new range (an int's rounded, a
    // bool's 0 or 1); and an int's or a bool's value that has a name (its switch's, buttons' or menu's text: the
    // panel's options) the value of that name now, wherever the new options put it (KEYS CAR PITCH to KEYS CAR TRUCK
    // PITCH keeps PITCH: 2, then 3). A name the new options lack keeps the number. A parameter new to the slot starts
    // at its default. Projects and presets keep a dial's value and its name the same way (ProjectStore.h's Dial).

    // `plain` within c's range: an int's rounded, a bool's 0 or 1; c's default when it is not finite.
    inline float clampedPlain(const Control& c, float plain) noexcept
    {
        if (!std::isfinite(plain))
            return c.def;
        switch (c.type)
        {
            case ControlType::toggle:  return plain >= 0.5f ? 1.0f : 0.0f;
            case ControlType::integer: return std::round(std::clamp(plain, c.min, c.max));
            case ControlType::real:    break;
        }
        return std::clamp(plain, c.min, c.max);
    }

    // The name an int's or a bool's plain value has among `options` (from the range's low end, as dialText reads
    // them); "" for a real one or none.
    inline std::string optionAt(const Control& c, float plain, const std::vector<std::string>& options)
    {
        if (c.type == ControlType::real || !std::isfinite(plain))
            return {};
        const long k = std::lround(plain) - std::lround(c.min);
        return k >= 0 && static_cast<std::size_t>(k) < options.size() ? options[static_cast<std::size_t>(k)] : std::string();
    }

    // The plain value of the option named `label` (none: NaN): c's low end plus its place in `options`.
    inline float optionValue(const Control& c, const std::vector<std::string>& options, const std::string& label)
    {
        if (c.type == ControlType::real || label.empty())
            return std::nanf("");
        for (std::size_t j = 0; j < options.size(); ++j)
            if (options[j] == label)
                return clampedPlain(c, static_cast<float>(std::lround(c.min) + static_cast<long>(j)));
        return std::nanf("");
    }

    // `plain`, the value of `from` (with its options), as the dial of `to` keeps it (above).
    inline float carryValue(const Control& from, const std::vector<std::string>& fromOptions, const Control& to,
                            const std::vector<std::string>& toOptions, float plain)
    {
        const float kept = clampedPlain(to, plain);
        const std::string label = optionAt(from, plain, fromOptions);
        if (label.empty() || optionAt(to, kept, toOptions) == label)
            return kept;                                         // no name, or the same value has it still
        const float v = optionValue(to, toOptions, label);
        return std::isfinite(v) ? v : kept;
    }

    // What changed in a dial's declaration that may change its value, in words ("range changed", "option moved",
    // "taper changed, clamped"), for run_program's report; "" when nothing did. `before` is the value under `from`,
    // `after` the value kept under `to`.
    inline std::string carryWhy(const Control& from, const std::vector<std::string>& fromOptions, const Control& to,
                                const std::vector<std::string>& toOptions, float before, float after)
    {
        std::string out;
        const auto add = [&](const char* what) { out += (out.empty() ? "" : ", ") + std::string(what); };
        if (from.type != to.type)
            add("type changed");
        const bool named = from.type != ControlType::real && to.type != ControlType::real && !fromOptions.empty()
                           && !toOptions.empty();
        if (named && fromOptions != toOptions)
        {
            // The names say what the range does.
            const std::string label = optionAt(from, before, fromOptions);
            if (label.empty() || !std::isfinite(optionValue(to, toOptions, label)))
                add(label.empty() ? "options changed" : "option removed");
            else
                add(std::lround(before) - std::lround(from.min) != std::lround(after) - std::lround(to.min) ? "option moved"
                                                                                                            : "options changed");
            return out;
        }
        if (from.min != to.min || from.max != to.max)
            add("range changed");
        const bool logFrom = from.taper == ControlTaper::log && logTaperFits(from);
        const bool logTo = to.taper == ControlTaper::log && logTaperFits(to);
        if (logFrom != logTo)
            add("taper changed");
        if (!out.empty() && std::isfinite(before) && before != after && clampedPlain(to, before) == after)
            add("clamped");
        return out;
    }

    // A program's dials as they come across from the program before it: the Compiler asks the processor for one just
    // before it offers a program (Compiler.h, "The dials' carry"). It travels in the Patch, for the audio thread, which
    // plays `plain` for a slot while the host's value there is still `fromNorm` (the processor has not written the
    // carried value yet: Engine.h, "Dials"), and in the status, for the processor, which writes the values. Plain data.
    struct DialCarry
    {
        uint64_t from = 0;                             // the program whose controls the host's values were in
        uint64_t ticket = 0;                           // the processor's count of carries made (only the newest writes)
        uint64_t slots = 0;                            // the slots it holds (slotBit(c)): the used ones
        std::array<float, kMaxControls> fromNorm{};    // each one's normalised value when it was made
        std::array<float, kMaxControls> plain{};       // and the plain value the new program plays for it
    };
}
