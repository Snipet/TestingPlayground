// Source/dsp/FdnLayout.cpp: see FdnLayout.h.
#include "dsp/FdnLayout.h"

#include "dsp/Params.h"
#include "dsp/Primes.h"

#include "fsdsp/core/FastMath.h"

#include <algorithm>
#include <cmath>

namespace testingplayground
{
    float targetLength(float sampleRate, float sizeMeters, int i, int lines) noexcept
    {
        // A NaN would pass std::clamp and reach makeLayout's integer conversion (undefined): SIZE's default instead.
        const float size = std::isfinite(sizeMeters) ? std::clamp(sizeMeters, kMinSize, kMaxSize)
                                                     : kParams[param::size].def;
        const float meanFreePath = 2.0f * size / 3.0f;
        const float pos = lines > 1 ? static_cast<float>(i) / static_cast<float>(lines - 1) - 0.5f : 0.0f;
        return sampleRate * meanFreePath / kSpeedOfSound * fsdsp::exp2(kSpreadOctaves * pos);
    }

    FdnLayout makeLayout(float sampleRate, float sizeMeters, int lines) noexcept
    {
        FdnLayout out;
        out.lines = (lines == 4 || lines == 8) ? lines : kMaxLines;
        uint32_t last = 0;
        for (int i = 0; i < out.lines; ++i)
        {
            const float t = targetLength(sampleRate, sizeMeters, i, out.lines);
            const auto n = static_cast<uint32_t>(std::max(t, 2.0f) + 0.5f);
            uint32_t p = primes::closest(n);
            if (p <= last)
                p = primes::nextAbove(last);
            out.delay[static_cast<std::size_t>(i)] = p;
            last = p;
        }
        return out;
    }

    float maxLayoutDelay(float sampleRate) noexcept
    {
        // The longest line at the largest size, plus 512 samples: more than any prime gap at these lengths (the largest
        // gap below 10^6 is 114), and at that size the lines lie hundreds of samples apart, so none is bumped. Shorter
        // sizes can bump a line, but only by a few gaps, far below this bound. The dsp.primes probe checks it.
        return targetLength(sampleRate, kMaxSize, kMaxLines - 1, kMaxLines) + 512.0f;
    }
}
