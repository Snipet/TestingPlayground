// Tools/probes/dsp/Rig.h: runs the Engine as the processor does (setParams once per block, then process in place),
// over a whole stereo signal. JUCE-free: the dsp probes test Source/dsp alone.
#pragma once

#include "Audio.h"

#include "dsp/Engine.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace probe
{
    // `in` through a fresh Engine at `fs`, in blocks of `block` samples, with the plain parameter values `values`.
    inline Stereo runEffect(const Stereo& in, double fs, int block, const testingplayground::ParamValues& values)
    {
        testingplayground::Engine engine;
        engine.prepare(fs, block);
        Stereo out = in;
        const std::size_t step = static_cast<std::size_t>(std::max(block, 1));
        for (std::size_t pos = 0; pos < out.size(); pos += step)
        {
            const std::size_t len = std::min(step, out.size() - pos);
            engine.setParams(values);
            float* channels[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(channels, 2, static_cast<int>(len), {});
        }
        return out;
    }

    // Both channels carrying the same signal.
    inline Stereo dual(const std::vector<float>& x)
    {
        Stereo s;
        s.l = x;
        s.r = x;
        return s;
    }
}
