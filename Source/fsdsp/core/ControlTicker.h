#pragma once

// Control-rate ticks at ABSOLUTE base-rate sample-index multiples of kTickSamples (01 §7.1, §7.8, §11.4; F rec. 10;
// seeded from FCompressor: FC 01 §5.1; FC E §4.6), so every tick-rate decision lands on the same samples whatever
// the host block size (block-size invariance, 01 §10.7; FC C §5.6.3).
//
// Frozen at FZ0. Seeded with its body; F1 (S1) verifies it on FSynth's flags (dsp.units). advance() is
// FSDSP_NONBLOCKING (core/Rt.h) and inline in this header.

#include "fsdsp/core/Rt.h"
#include <cstdint>

namespace fsdsp {

struct ControlTicker {                      // true every kTickSamples at ABSOLUTE sample index multiples
    uint64_t next = 0;
    bool advance(uint64_t sampleIndex) noexcept FSDSP_NONBLOCKING;
};

inline constexpr int kTickSamples = 16;

// Body (seeded: FC's F1, S1). The first call ticks (a new or reset engine does its tick work on its first sample,
// next = 0);
// after that, ticks fall on the absolute multiples of kTickSamples, whatever the host block size. sampleIndex never
// decreases between resets (ControlTicker{} starts over); a skipped index still ticks at the next call.
inline bool ControlTicker::advance(uint64_t sampleIndex) noexcept FSDSP_NONBLOCKING
{
    constexpr uint64_t k = static_cast<uint64_t>(kTickSamples);
    if (sampleIndex < next)
        return false;
    next = sampleIndex - sampleIndex % k + k;
    return true;
}

} // namespace fsdsp
