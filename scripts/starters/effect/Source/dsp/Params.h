// Source/dsp/Params.h: this plugin's parameters, one table row each (ParamSpec.h says what a row holds).
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
            tone,
            count
        };
    }

    inline constexpr std::array<ParamSpec, param::count> kParams { {
        { .id = "gain", .name = "Gain", .label = "GAIN", .unit = Unit::db,
          .min = -24.0f, .max = 24.0f, .def = 0.0f, .centre = 0.0f, .control = Control::dial },
        { .id = "tone", .name = "Tone", .label = "TONE", .unit = Unit::hz,
          .min = 200.0f, .max = 20000.0f, .def = 20000.0f, .centre = 2000.0f, .control = Control::slider },
    } };

    // Plain values in table order: what the processor hands Engine::setParams once per block.
    using ParamValues = std::array<float, param::count>;

    constexpr ParamValues defaultValues() noexcept
    {
        ParamValues v{};
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = kParams[i].def;
        return v;
    }
}
