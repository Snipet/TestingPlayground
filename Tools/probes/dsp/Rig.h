// Tools/probes/dsp/Rig.h: runs the Engine as the processor does (setParams once per block, then process with the
// block's events at their offsets), over a whole score of note events, and the small measures the dsp probes share.
// JUCE-free: the dsp probes test Source/dsp alone.
#pragma once

#include "Audio.h"

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace probe
{
    namespace tps = testingplaygroundsynth;

    // A note event at an absolute sample index.
    struct ScoreEvent
    {
        std::size_t at = 0;
        tps::EventKind kind = tps::EventKind::noteOn;
        uint8_t note = 0;
        float velocity = 0.0f;
    };

    inline ScoreEvent on(std::size_t at, int note, float velocity = 1.0f)
    {
        return { at, tps::EventKind::noteOn, static_cast<uint8_t>(note), velocity };
    }

    inline ScoreEvent off(std::size_t at, int note)
    {
        return { at, tps::EventKind::noteOff, static_cast<uint8_t>(note), 0.0f };
    }

    // Parameter values from a sample index on: they reach setParams at the first block that starts there or later.
    using ParamChange = std::pair<std::size_t, tps::ParamValues>;

    // `n` samples of a fresh Engine at `fs` playing `score` (sorted by `at`), in blocks of `block` samples, with
    // `values` changed by `changes` (sorted by sample). `engineOut`, when given, is the engine used, as it is at the
    // end.
    inline Stereo runSynth(const std::vector<ScoreEvent>& score, std::size_t n, double fs, int block,
                           const tps::ParamValues& values, tps::Engine* engineOut = nullptr,
                           const std::vector<ParamChange>& changes = {})
    {
        tps::Engine local;
        tps::Engine& engine = engineOut != nullptr ? *engineOut : local;
        engine.prepare(fs, block);
        Stereo out(n);
        const std::size_t step = static_cast<std::size_t>(std::max(block, 1));
        std::size_t next = 0, nextChange = 0;
        tps::ParamValues current = values;
        std::vector<tps::Event> events;
        for (std::size_t pos = 0; pos < n; pos += step)
        {
            const std::size_t len = std::min(step, n - pos);
            while (nextChange < changes.size() && changes[nextChange].first <= pos)
                current = changes[nextChange++].second;
            events.clear();
            while (next < score.size() && score[next].at < pos + len)
            {
                const ScoreEvent& e = score[next++];
                tps::Event ev;
                ev.offset = static_cast<uint32_t>(e.at > pos ? e.at - pos : 0);
                ev.kind = e.kind;
                ev.note = e.note;
                ev.velocity = e.velocity;
                events.push_back(ev);
            }
            engine.setParams(current);
            float* channels[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(channels, 2, static_cast<int>(len), events);
        }
        return out;
    }

    inline double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

    inline double centsBetween(double hz, double reference) { return 1200.0 * std::log2(hz / reference); }

    // [from, from + count) of a channel, as its own vector.
    inline std::vector<float> slice(const std::vector<float>& x, std::size_t from, std::size_t count)
    {
        const std::size_t a = std::min(from, x.size()), b = std::min(x.size(), from + count);
        return { x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b) };
    }
}
