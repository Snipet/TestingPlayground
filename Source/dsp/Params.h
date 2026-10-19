// Source/dsp/Params.h: this plugin's parameters, one table row each (ParamSpec.h says what a row holds).
//
// GAIN, ATTACK and RELEASE are the legacy synth's (tag legacy-visage): the same IDs, ranges, defaults and skew (JUCE's
// 0.5, which `centre` states as the value at the middle of the travel). DECAY and SUSTAIN complete its envelope to an
// ADSR; at their defaults (SUSTAIN 100 %) a note sounds as the legacy synth's did. SCRIPT is new: how much of the FMinus
// script's output is heard (Source/fminus/script.fm, README.md, "FMinus"), 0 to 100 %; at 0, the default, the script
// does not run.
//
// Adding a parameter: add an index before `count`, add its row at the same position, then read
// values[param::<name>] in Engine::setParams. The processor, the host, the state and the panel follow from the table
// (a dial, a slider or a choice per its `control`). IDs are stored by hosts in sessions: never rename one.
#pragma once

#include "dsp/ParamSpec.h"

#include <array>
#include <cstddef>

namespace testingplaygroundsynth
{
    namespace param
    {
        enum Index : std::size_t
        {
            gain,
            attack,
            decay,
            sustain,
            release,
            script,
            count
        };
    }

    inline constexpr std::array<ParamSpec, param::count> kParams { {
        { .id = "gain", .name = "Gain", .label = "GAIN", .unit = Unit::db,
          .min = -24.0f, .max = 24.0f, .def = 0.0f, .centre = 0.0f, .control = Control::dial },
        { .id = "attack", .name = "Attack", .label = "ATTACK", .unit = Unit::ms,
          .min = 1.0f, .max = 200.0f, .def = 5.0f, .centre = 50.75f, .control = Control::dial },
        { .id = "decay", .name = "Decay", .label = "DECAY", .unit = Unit::ms,
          .min = 1.0f, .max = 2000.0f, .def = 200.0f, .centre = 500.75f, .control = Control::dial },
        { .id = "sustain", .name = "Sustain", .label = "SUSTAIN", .unit = Unit::percent,
          .min = 0.0f, .max = 100.0f, .def = 100.0f, .centre = 50.0f, .control = Control::dial },
        { .id = "release", .name = "Release", .label = "RELEASE", .unit = Unit::ms,
          .min = 1.0f, .max = 1000.0f, .def = 50.0f, .centre = 250.75f, .control = Control::dial },
        { .id = "script", .name = "Script", .label = "SCRIPT", .unit = Unit::percent,
          .min = 0.0f, .max = 100.0f, .def = 0.0f, .centre = 50.0f, .control = Control::dial },
    } };

    // Plain values in table order: what the processor hands Engine::setParams once per block. A choice's value is its
    // index (0, 1, ...).
    using ParamValues = std::array<float, param::count>;

    constexpr ParamValues defaultValues() noexcept
    {
        ParamValues v{};
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = kParams[i].def;
        return v;
    }
}
