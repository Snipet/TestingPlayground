// Source/dsp/Engine.h: the effect, JUCE-free. An FDN reverb (Fdn.h) with a dry/wet MIX and an output GAIN.
//
//   out = GAIN ((1 - MIX) x + MIX dip wet)          per channel; wet is the Fdn's (left: its even lines, right: odd)
//   then the FMinus script (Source/fminus/script.fm, Script.h), as much of it as SCRIPT says: none, by default
//
// MIX and GAIN are smoothed per sample (fsdsp::Smoother4, 20 ms); the Fdn smooths its own parameters and glides its
// delays. LINES is the one parameter that cannot move smoothly (it changes the network): a change fades the wet path
// out (`dip`, 20 ms smootherstep), switches the network and clears it while it is silent, and fades back in, so it never
// clicks; the tail in flight is lost. A mono bus processes left only, its wet signal the two outputs' sum over sqrt(2).
//
// The processor calls, on the audio thread: prepare() before playback (the only place that allocates), setParams()
// once per block with the plain values of Params.h (finite, within the table's range), setLfoTable() when the LFO shape
// changed (the editor's graph; Processor.h), then process() in place, with at most prepare()'s maxBlockSize samples per
// call (the processor cuts a larger host block into parts; a part may have 0 samples). It keeps calling them while the
// host bypasses the plugin (the processor crossfades to the input; Processor.h). Before the first prepare() there are
// no delay lines: process() then only sanitises its input and passes it through (the processor does not call it then,
// either). Hosts call tailSeconds() from any thread: an atomic, stored by setParams(). Everything runs per sample on
// the absolute sample clock, so the output does not depend on the host's block size or on how the blocks are split
// (dsp.render checks 64 against 333 samples).
#pragma once

#include "dsp/Events.h"
#include "dsp/Fdn.h"
#include "fminus/Script.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include "fsdsp/core/Ramp.h"
#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"

#include <atomic>
#include <span>

namespace testingplayground
{
    class Engine
    {
    public:
        void prepare(double sampleRate, int maxBlockSize);           // allocates the delay lines
        void reset() noexcept;                                        // silence the state; the next setParams snaps
        void setParams(const ParamValues& values) noexcept;
        void setLfoTable(const LfoTable& table) noexcept;             // crossfades to it (snaps before the first block)
        void process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept;

        double tailSeconds() const noexcept { return tail_.load(std::memory_order_relaxed); }
        float  sampleRate() const noexcept { return fs_; }
        const Fdn& fdn() const noexcept { return fdn_; }               // for the probes
        Script& script() noexcept { return script_; }                  // the FMinus script on the output (Script.h)

    private:
        float fs_ = 48000.0f;
        Fdn fdn_;
        fsdsp::Smoother4 smooth_{};          // lane 0: MIX (0..1); lane 1: GAIN (linear)
        fsdsp::SmoothRamp dip_{};            // the wet path's weight: 0 while LINES switches, else 1
        int wantLines_ = 0;                  // a LINES change waiting for the dip to reach 0
        float size_ = 12.0f;
        bool primed_ = false;                // false until the first setParams after reset(): it snaps
        bool prepared_ = false;              // prepare() has allocated the delay lines
        std::atomic<double> tail_{ 3.0 };
        Script script_;                      // after the output GAIN: its share is SCRIPT
    };
}
