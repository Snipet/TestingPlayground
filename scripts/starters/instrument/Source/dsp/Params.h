// Source/dsp/Params.h: this plugin's parameters, one table row each (ParamSpec.h says what a row holds).
//
// Adding a parameter: add an index before `count`, add its row at the same position, then read
// values[param::<name>] in Engine::setParams. The processor, the host, the state and the panel follow from the table
// (a dial, a slider or a choice per its `control`). IDs are stored by hosts in sessions: never rename one.
#pragma once

#include "dsp/ParamSpec.h"

#include <array>
#include <cstddef>

namespace testingplayground
{
    namespace param
    {
        enum Index : std::size_t
        {
            gain,
            attack,
            release,
            wave,
            count
        };
    }

    inline constexpr std::array<const char*, 2> kWaves { "SINE", "TRIANGLE" };

    inline constexpr std::array<ParamSpec, param::count> kParams { {
        { .id = "gain", .name = "Gain", .label = "GAIN", .unit = Unit::db,
          .min = -36.0f, .max = 12.0f, .def = 0.0f, .centre = -12.0f, .control = Control::dial },
        { .id = "attack", .name = "Attack", .label = "ATTACK", .unit = Unit::ms,
          .min = 1.0f, .max = 2000.0f, .def = 5.0f, .centre = 100.0f, .control = Control::dial },
        { .id = "release", .name = "Release", .label = "RELEASE", .unit = Unit::ms,
          .min = 5.0f, .max = 5000.0f, .def = 300.0f, .centre = 300.0f, .control = Control::slider },
        { .id = "wave", .name = "Wave", .label = "WAVE", .unit = Unit::none,
          .min = 0.0f, .max = 1.0f, .def = 0.0f, .centre = 0.5f, .control = Control::choice,
          .choices = kWaves.data(), .numChoices = static_cast<int>(kWaves.size()) },
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
