#pragma once

// Input sanitisation (01 §7.1, §7.8, §11.8; seeded from FCompressor: FC 01 §5.1, §5.8; FC K2 #13). FSynth applies
// it to every signal that enters the graph from outside it: the pm and side inputs where they enter a node, and any
// external audio, before ANY delay line or filter touches them (in FCompressor: main and key, so that the dry delay
// and the bypass path are finite by construction).
//
// Frozen at FZ0. Seeded with its body (Sanitize.cpp); F1 (S1) verifies it on FSynth's flags (dsp.units).
// FSDSP_NONBLOCKING (core/Rt.h): an out-of-line definition repeats the macro.

#include "fsdsp/core/Rt.h"

namespace fsdsp {

// Before ANY delay line or filter: NaN/inf -> 0 (bit test (bits & 0x7f800000) != 0x7f800000), then clamp |x| <= 1e6
// (+120 dBFS). Returns how many samples were replaced or clamped (the caller's diagnostic; 01 §11.8).
int sanitize(const float* in, float* out, int n) noexcept FSDSP_NONBLOCKING;

} // namespace fsdsp
