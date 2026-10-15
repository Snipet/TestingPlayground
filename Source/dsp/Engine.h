// Source/dsp/Engine.h: the effect, JUCE-free. A gain and a one-pole low-pass (TONE), stereo in one SIMD vector
// {L, R, 0, 0}, both parameters smoothed per sample (fsdsp::Smoother4, 20 ms), so nothing steps and the output does
// not depend on the host's block size.
//
// The processor calls, on the audio thread: prepare() before playback (the only place that may allocate), setParams()
// once per block with the plain values of Params.h (finite, within the table's range), then process() in place.
// Hosts call tailSeconds() from any thread: return a constant, or read an atomic that setParams() stores. Replace the
// body with your algorithm and keep the contract: no allocation, no locks, no I/O in setParams() and process().
#pragma once

#include "dsp/Events.h"
#include "dsp/Params.h"

#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"

#include <span>

namespace testingplayground
{
    class Engine
    {
    public:
        void prepare(double sampleRate, int maxBlockSize) noexcept;
        void reset() noexcept;                                        // silence the state; the next setParams snaps
        void setParams(const ParamValues& values) noexcept;
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        double tailSeconds() const noexcept { return 0.0; }
        float  sampleRate() const noexcept { return fs_; }

    private:
        float fs_ = 48000.0f;
        fsdsp::Smoother4 smooth_{};          // lane 0: the linear gain; lane 1: the one-pole's G = g / (1 + g)
        fsdsp::simd::f32x4 state_{};         // the one-pole's integrator, {L, R, 0, 0}
        bool primed_ = false;                // false until the first setParams after reset(): it snaps
    };
}
