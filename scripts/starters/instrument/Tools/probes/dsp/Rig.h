// Tools/probes/dsp/Rig.h: runs the Engine as the processor does (setParams once per block, then process with the
// block's events at their offsets), over a whole score of note events. JUCE-free: the dsp probes test Source/dsp alone.
#pragma once

#include "Audio.h"

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace probe
{
    // A note event at an absolute sample index.
    struct ScoreEvent
    {
        std::size_t at = 0;
        testingplayground::EventKind kind = testingplayground::EventKind::noteOn;
        uint8_t note = 0;
        float velocity = 0.0f;
    };

    inline ScoreEvent on(std::size_t at, int note, float velocity = 1.0f)
    {
        return { at, testingplayground::EventKind::noteOn, static_cast<uint8_t>(note), velocity };
    }

    inline ScoreEvent off(std::size_t at, int note)
    {
        return { at, testingplayground::EventKind::noteOff, static_cast<uint8_t>(note), 0.0f };
    }

    // `n` samples of a fresh Engine at `fs` playing `score` (sorted by `at`), in blocks of `block` samples.
    // `engineOut`, when given, receives the engine as it is at the end (for activeVoices()).
    inline Stereo runSynth(const std::vector<ScoreEvent>& score, std::size_t n, double fs, int block,
                           const testingplayground::ParamValues& values,
                           testingplayground::Engine* engineOut = nullptr)
    {
        testingplayground::Engine local;
        testingplayground::Engine& engine = engineOut != nullptr ? *engineOut : local;
        engine.prepare(fs, block);
        Stereo out(n);
        const std::size_t step = static_cast<std::size_t>(std::max(block, 1));
        std::size_t next = 0;
        std::vector<testingplayground::Event> events;
        for (std::size_t pos = 0; pos < n; pos += step)
        {
            const std::size_t len = std::min(step, n - pos);
            events.clear();
            while (next < score.size() && score[next].at < pos + len)
            {
                const ScoreEvent& e = score[next++];
                testingplayground::Event ev;
                ev.offset = static_cast<uint32_t>(e.at > pos ? e.at - pos : 0);
                ev.kind = e.kind;
                ev.note = e.note;
                ev.velocity = e.velocity;
                events.push_back(ev);
            }
            engine.setParams(values);
            float* channels[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(channels, 2, static_cast<int>(len), events);
        }
        return out;
    }
}
