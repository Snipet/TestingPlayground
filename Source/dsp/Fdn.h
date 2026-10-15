// Source/dsp/Fdn.h: the feedback delay network (JUCE-free). The Engine (Engine.h) owns one, sets its targets once per
// block and runs tick() once per sample.
//
// Per sample, for the n = 4, 8 or 16 lines (LINES):
//
//   s_i = line_i.read(D_i + depth_i m_i)          the delay outputs: Thiran-interpolated reads (DelayLine.h)
//   f_i = b_i s_i + p_i f_i                       the absorption: a one-pole whose gain is g_i at 0 Hz and h_i at
//                                                 Nyquist, so the line loses 60 dB per DECAY seconds at low
//                                                 frequencies and per DECAY x HF DECAY seconds at the top
//   v = H f                                       the orthonormal Hadamard mix (Hadamard.h)
//   line_i.push(v_i + k u_i y_c(i))               the input: left into the even lines, right into the odd ones, with
//                                                 fixed signs u_i, scaled by the input gain k
//   y_c = x_c - x_c[-1] + R y_c[-1]               each input's DC blocker, before the network (R = e^(-2 pi 5 Hz / fs))
//   wet_L = sqrt(2/n) sum_even w_i s_i            the outputs: the even lines to the left, the odd to the right, with
//   wet_R = sqrt(2/n) sum_odd  w_i s_i            fixed signs w_i
//
//   D_i                the prime delay lengths of FdnLayout.h for SIZE; when SIZE moves they glide to the new primes,
//                      so a size change is a pitch glide, not a jump. The glide is a one-pole (tau 60 ms) on each line's
//                      OFFSET from its prime, landing exactly once under 1e-3 samples: a float one-pole on the position
//                      itself stalls short of the prime once its step drops below half an ulp (0.35 samples at 48 kHz
//                      for a 2,800-sample line; HardwareReverb measured it)
//   g_i = 10^(-3 D_i / (T fs)), h_i = 10^(-3 D_i / (r T fs)), T = DECAY in seconds, r = HF DECAY in 0.1 .. 1;
//                      p_i = (g_i - h_i) / (g_i + h_i), b_i = g_i (1 - p_i). r = 1 gives p_i = 0: no damping.
//   k = sqrt(1 - mean(g_i^2))   keeps the reverb's level for steady noise near the input's whatever SIZE and DECAY (the
//                      steady energy of the loop is k^2 / mean(1 - g^2) of the input's; HardwareReverb's choice, the
//                      family this FDN belongs to)
//   m_i                the LFO: the shape's table (LfoShape.h, bipolar) at the phase plus i / n of a cycle, so the
//                      lines move out of step; RATE in Hz. depth_i = min(DEPTH, 0.45 D_i) in samples
//   y_c                0 Hz is a mode of the network whatever its lengths (every line's comb peaks there and the
//                      modulation cannot detune it), so without the blocker a DC offset would charge the loop to
//                      about k / (1 - g) of itself: +12 dB at the defaults, +34 dB at 1 m, 4 lines and 20 s. The
//                      blocker's impulse response starts at exactly 1 (then (R - 1) R^(m - 1)), so a first arrival is
//                      still k u_i w_i sqrt(2/n); it takes 0.06 dB off the wet path at 40 Hz, 0.26 dB at 20 Hz, 1 dB
//                      at 10 Hz
//
// The coefficients (g, h, p, b, k and the LFO increment) are recomputed every fsdsp::kTickSamples samples, on absolute
// sample multiples (fsdsp::ControlTicker); the smoothed parameters (20 ms one-poles) and the delay glides run per
// sample. So the output does not depend on how the host splits the blocks.
//
// prepare() allocates; everything else is real-time safe: no allocation, no locks, no I/O. A new LFO table crossfades
// from the old over 20 ms (one table at a time; a table that arrives during a fade follows when it ends). setLines()
// clears the network (a memset of every line: 1 MB at 48 kHz, 4 MB at 192 kHz, once per LINES change); with fadeIn
// it also fades the input in over 20 ms, so the first echo of a signal already playing rises instead of stepping in.
#pragma once

#include "dsp/DelayLine.h"
#include "dsp/FdnLayout.h"
#include "dsp/Hadamard.h"
#include "dsp/LfoShape.h"

#include "fsdsp/core/ControlTicker.h"
#include "fsdsp/core/Ramp.h"
#include "fsdsp/core/Simd.h"
#include "fsdsp/core/Smoother.h"

#include <array>
#include <cstdint>

namespace testingplayground
{
    class Fdn
    {
    public:
        static constexpr float kMaxDepthMs = 2.0f;        // DEPTH's maximum (Params.h)
        static constexpr float kGlideMs = 60.0f;          // the delay glide's time constant
        static constexpr float kDepthLimit = 0.45f;       // depth_i <= 0.45 D_i: a read never comes near zero delay
        static constexpr float kDcHz = 5.0f;              // the inputs' DC blocker: its pole is e^(-2 pi kDcHz / fs)

        struct Targets
        {
            float decayMs = 2000.0f;                      // T60 at low frequencies
            float hfRatio = 0.5f;                         // the T60 at Nyquist over decayMs, 0.1 .. 1
            float rateHz = 0.5f;                          // the LFO
            float depthMs = 0.25f;                        // the LFO's peak delay change
        };

        void prepare(float sampleRate);                   // allocates the lines for every SIZE at this rate; silent
        void reset() noexcept;                            // silence, LFO at phase 0, the sample clock at 0

        // A new line count and layout (SIZE): the lines are cleared and the delays jump to the new primes; with fadeIn
        // the input fades in over 20 ms. The Engine calls it while the wet output is faded out (Engine.h).
        void setLines(int lines, float sizeMeters, bool fadeIn) noexcept;
        // SIZE for the current lines: the delays glide to the new primes.
        void setSize(float sizeMeters) noexcept;
        void setTargets(const Targets& t, bool snap) noexcept;
        void setTable(const LfoTable& table, bool snap) noexcept;

        void tick(float inL, float inR, float& wetL, float& wetR) noexcept;

        // ---- for the Engine, the probes and the tail length ------------------------------------------------------
        int lines() const noexcept { return lines_; }
        const FdnLayout& layout() const noexcept { return layout_; }          // the targets of the glides
        float delay(int i) const noexcept { return base_[static_cast<std::size_t>(i)]; }   // line i's D_i now
        // Where line i read on the last tick(), the modulation included: D_i + depth_i m_i.
        float readDelay(int i) const noexcept { return read_[static_cast<std::size_t>(i)]; }
        float loopGain(int i) const noexcept { return g_[static_cast<std::size_t>(i)]; }   // g_i
        float inputGain() const noexcept { return inGain_; }

    private:
        void updateCoefficients(fsdsp::simd::f32x4 smoothed) noexcept;

        float fs_ = 48000.0f;
        int lines_ = kMaxLines;
        float size_ = -1.0f;                              // SIZE of layout_
        FdnLayout layout_{};
        std::array<DelayLine, kMaxLines> line_{};

        fsdsp::Smoother4 params_{};                       // decayMs, hfRatio, rateHz, depthMs
        fsdsp::SmoothRamp inFade_{};                      // the input's weight: 1, or rising after a LINES switch
        float glideA_ = 0.0f;                             // the delay glide's one-pole coefficient
        fsdsp::ControlTicker ticker_{};
        uint64_t clock_ = 0;                              // samples since reset()

        alignas(16) std::array<float, kMaxLines> target_{};    // the primes D_i glides to
        alignas(16) std::array<float, kMaxLines> offset_{};    // D_i - target, decaying to 0
        alignas(16) std::array<float, kMaxLines> base_{};      // D_i = target + offset
        alignas(16) std::array<float, kMaxLines> g_{};         // the 0 Hz loop gain (for k and the probes)
        alignas(16) std::array<float, kMaxLines> b_{};         // the absorption filter
        alignas(16) std::array<float, kMaxLines> pole_{};
        alignas(16) std::array<float, kMaxLines> state_{};     // its state, f_i
        alignas(16) std::array<float, kMaxLines> read_{};      // the last tick's modulated read positions
        float inGain_ = 0.0f;
        float dcPole_ = 0.0f;                                  // R, the DC blockers' pole
        std::array<float, 2> dcIn_{}, dcOut_{};                // their last input and output, left and right
        float outScale_ = 0.25f;

        // The LFO: a 32-bit phase (one cycle = 2^32), line i offset by i / n of a cycle, the table crossfade.
        uint32_t phase_ = 0, inc_ = 0;
        std::array<uint32_t, kMaxLines> lfoOffset_{};
        LfoTable table_{}, next_{}, pending_{};
        bool fading_ = false, hasPending_ = false;
        fsdsp::SmoothRamp fade_{};                        // the weight of next_ while fading_
    };
}
