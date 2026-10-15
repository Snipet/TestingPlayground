// Tools/probes/dsp/Rig.h: runs the Engine as the processor does (the LFO table first, then setParams once per block,
// then process in place), over a whole stereo signal, and the reverb measurements the dsp probes share. JUCE-free: the
// dsp probes test Source/dsp alone.
#pragma once

#include "Audio.h"

#include "dsp/Engine.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace probe
{
    namespace tp = testingplayground;

    // Called before each block with the block's first sample index: may change the values or hand the engine a table.
    using BlockHook = std::function<void(std::size_t pos, tp::ParamValues& values, tp::Engine& engine)>;

    // `in` through a fresh Engine at `fs`, in blocks of `block` samples, with the plain parameter values `values` and
    // the default LFO shape (or `table`); `hook` may change either between blocks.
    inline Stereo runEffect(const Stereo& in, double fs, int block, const tp::ParamValues& values,
                            const tp::LfoTable* table = nullptr, const BlockHook& hook = {})
    {
        tp::Engine engine;
        engine.prepare(fs, block);
        tp::LfoTable initial;
        if (table == nullptr)
            tp::renderTable(tp::defaultShape(), initial);
        engine.setLfoTable(table != nullptr ? *table : initial);
        tp::ParamValues v = values;
        Stereo out = in;
        const std::size_t step = static_cast<std::size_t>(std::max(block, 1));
        for (std::size_t pos = 0; pos < out.size(); pos += step)
        {
            const std::size_t len = std::min(step, out.size() - pos);
            if (hook)
                hook(pos, v, engine);
            engine.setParams(v);
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

    // `n` samples of the response to a unit impulse at sample 0, on the left, the right or both inputs.
    inline Stereo impulseResponse(double fs, std::size_t n, const tp::ParamValues& values, bool left, bool right,
                                  int block = 256)
    {
        Stereo in(n);
        if (left)
            in.l[0] = 1.0f;
        if (right)
            in.r[0] = 1.0f;
        return runEffect(in, fs, block, values);
    }

    // Per-sample energy of both channels, l^2 + r^2, in double.
    inline std::vector<double> energy(const Stereo& s)
    {
        std::vector<double> e(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            e[i] = static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i];
        return e;
    }

    // Base values for a measurement of the network alone: MIX 100 %, GAIN 0 dB, no modulation, no damping.
    inline tp::ParamValues wetOnly()
    {
        tp::ParamValues v = tp::defaultValues();
        v[tp::param::mix] = 100.0f;
        v[tp::param::gain] = 0.0f;
        v[tp::param::moddepth] = 0.0f;
        v[tp::param::hfdecay] = 100.0f;
        return v;
    }
}
