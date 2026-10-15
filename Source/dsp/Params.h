// Source/dsp/Params.h: this plugin's parameters, one table row each (ParamSpec.h says what a row holds).
//
// Adding a parameter: add an index before `count`, add its row at the same position, then read
// values[param::<name>] in Engine::setParams. The processor, the host, the state and the panel follow from the table
// (a dial, a slider or a choice per its `control`). IDs are stored by hosts in sessions: never rename one.
//
// Where they come from (the legacy is the tag legacy-visage):
//   size, decay       the legacy FDNConfig::calculate(sample_rate, room_size, ms_decay): the room size (here in metres,
//                     the delay lengths' scale, FdnLayout.h) and the time for the tail to fall 60 dB (ms)
//   hfdecay           the legacy FDNConfig::differential_decay, declared and never set: here the decay time at the top
//                     of the spectrum, as a percentage of DECAY (100 % = no damping)
//   lines             the legacy FDNConfig::matrix_order: the Hadamard matrix's order, 4, 8 or 16
//   modrate, moddepth the legacy graph editor's RATE (0.01 to 20 Hz, its range) and AMPLITUDE, now driving the delay
//                     lines' modulation: DEPTH is the peak change of a line's delay, 0 to 2 ms
//   mix               new: the reverb needs a dry/wet balance (the legacy processor ran only a gain)
//   gain              the legacy processor's one parameter: -24 to +24 dB, default 0, ID "gain"
// The ranges of size, decay, hfdecay, mix and depth are this port's: the legacy gave none.
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
            size,
            decay,
            hfdecay,
            mix,
            gain,
            lines,
            modrate,
            moddepth,
            count
        };
    }

    inline constexpr const char* kLineChoices[] = { "4", "8", "16" };

    inline constexpr std::array<ParamSpec, param::count> kParams { {
        { .id = "size", .name = "Size", .label = "SIZE", .unit = Unit::m,
          .min = 1.0f, .max = 100.0f, .def = 12.0f, .centre = 12.0f, .control = Control::dial },
        { .id = "decay", .name = "Decay", .label = "DECAY", .unit = Unit::ms,
          .min = 100.0f, .max = 20000.0f, .def = 2000.0f, .centre = 2000.0f, .control = Control::dial },
        { .id = "hfdecay", .name = "HF Decay", .label = "HF DECAY", .unit = Unit::percent,
          .min = 10.0f, .max = 100.0f, .def = 50.0f, .centre = 55.0f, .control = Control::dial },
        { .id = "mix", .name = "Mix", .label = "MIX", .unit = Unit::percent,
          .min = 0.0f, .max = 100.0f, .def = 30.0f, .centre = 50.0f, .control = Control::dial },
        { .id = "gain", .name = "Gain", .label = "GAIN", .unit = Unit::db,
          .min = -24.0f, .max = 24.0f, .def = 0.0f, .centre = 0.0f, .control = Control::dial },
        { .id = "lines", .name = "Lines", .label = "LINES", .unit = Unit::none,
          .min = 0.0f, .max = 2.0f, .def = 2.0f, .centre = 1.0f, .control = Control::choice,
          .choices = kLineChoices, .numChoices = 3 },
        { .id = "modrate", .name = "Mod Rate", .label = "RATE", .unit = Unit::hz,
          .min = 0.01f, .max = 20.0f, .def = 0.5f, .centre = 1.0f, .control = Control::dial },
        { .id = "moddepth", .name = "Mod Depth", .label = "DEPTH", .unit = Unit::ms,
          .min = 0.0f, .max = 2.0f, .def = 0.25f, .centre = 0.5f, .control = Control::dial },
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
