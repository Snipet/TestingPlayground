// Source/dsp/FdnLayout.h: the FDN's delay lengths, from the room size, as distinct primes (JUCE-free).
//
// The legacy FDN (git show legacy-visage:Source/dsp/FDNCalculator.h) declared the inputs, calculate(sample_rate,
// room_size, ms_decay), and the outputs, `delayline_lengths` snapped to primes by findClosestPrime (Prime.h); its body
// stored the inputs and computed nothing. This is the computation it was set up for:
//
//   mean free path    l = 2 S / 3 m for a room of size S m (a cube of side S: 4 V / A)
//   line i of n       T_i = fs l / c 2^(kSpreadOctaves (i / (n - 1) - 1/2)) samples, c = 343 m/s: n lengths spread
//                     geometrically over one octave, centred (in log) on the mean free path's travel time
//   prime             d_i = primes::closest(round(T_i)), raised to the next prime above d_(i-1) when it is not greater,
//                     so the n lengths are distinct primes, pairwise coprime: no two lines share a period, and the
//                     network's echoes do not pile up on common multiples
//
// At 48 kHz and 12 m (the default) the 16 lengths run from 787 to 1583 samples (16 to 33 ms). The Engine and the
// editor's DELAYS readout both call makeLayout(), so the readout shows what plays.
#pragma once

#include <array>
#include <cstdint>

namespace testingplayground
{
    inline constexpr int kMaxLines = 16;                 // LINES: 4, 8 or 16 (Hadamard orders)
    inline constexpr float kSpeedOfSound = 343.0f;       // m/s, air at 20 degrees C
    inline constexpr float kSpreadOctaves = 1.0f;        // longest line / shortest line = 2
    inline constexpr float kMinSize = 1.0f, kMaxSize = 100.0f;   // SIZE's range, m (Params.h)

    struct FdnLayout
    {
        int lines = kMaxLines;
        std::array<uint32_t, kMaxLines> delay{};         // samples, ascending; [lines, kMaxLines) are 0
    };

    // The lines for a room of `sizeMeters` (clamped to [kMinSize, kMaxSize]) with `lines` lines (4, 8 or 16; anything
    // else is 16) at `sampleRate`.
    FdnLayout makeLayout(float sampleRate, float sizeMeters, int lines) noexcept;

    // The target length of line i of n before the prime snap (T_i above), in samples.
    float targetLength(float sampleRate, float sizeMeters, int i, int lines) noexcept;

    // An upper bound of every delay makeLayout() can return at `sampleRate` (the largest size's longest line, plus the
    // prime gaps above it), for sizing the delay lines.
    float maxLayoutDelay(float sampleRate) noexcept;

    // LINES' choice index (0, 1, 2) as a line count (4, 8, 16).
    constexpr int linesFromChoice(float index) noexcept
    {
        return index < 0.5f ? 4 : (index < 1.5f ? 8 : 16);
    }
}
