// PROBE name=dsp.primes timeout=180
//
// dsp.primes: the delay-length scheme (Primes.h, FdnLayout.h).
//   LEGACY  primes::closest(n) answers what the legacy FunkDSP::findClosestPrime answered (the nearest prime, a tie
//           to the lower, 2 below 3) for every n from 0 to 1,395,629, the legacy table's range; the reference here is a
//           sieve. The legacy test's own inputs are rows of their own.
//   LAYOUT  for 44.1, 48, 88.2, 96 and 192 kHz, eleven sizes from 1 to 100 m and 4, 8 and 16 lines: every delay is a
//           prime, the delays ascend strictly (so they are pairwise coprime), none exceeds maxLayoutDelay() (what the
//           delay lines are sized for), each is within 3 % of its target length from 3 m up (below that the targets
//           of 16 lines lie closer together than the primes, and the distinctness rule pushes them up), and their
//           geometric mean is the mean free path's travel time (2 S / 3 / 343 m/s) within 2 %.
//   DEFAULT 12 m, 16 lines, 48 kHz: 787 to 1583 samples (the numbers FdnLayout.h quotes).
#include "Probe.h"

#include "dsp/FdnLayout.h"
#include "dsp/Primes.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;

PROBE(dsp, primes)
{
    // LEGACY: the whole range of the legacy table.
    constexpr uint32_t kTop = 1395629;
    {
        const uint32_t limit = kTop + 200;
        std::vector<uint8_t> composite(limit + 1, 0);
        composite[0] = composite[1] = 1;
        for (uint32_t i = 2; static_cast<uint64_t>(i) * i <= limit; ++i)
            if (!composite[i])
                for (uint32_t j = i * i; j <= limit; j += i)
                    composite[j] = 1;
        std::vector<uint32_t> below(limit + 1, 0), above(limit + 1, 0);
        uint32_t last = 0;
        for (uint32_t i = 0; i <= limit; ++i)
        {
            if (!composite[i])
                last = i;
            below[i] = last;                             // the largest prime <= i (0 below 2)
        }
        last = 0;
        for (uint32_t i = limit + 1; i-- > 0;)
        {
            if (!composite[i])
                last = i;
            above[i] = last;                             // the smallest prime >= i
        }
        const auto want = [&](uint32_t n) {
            return n <= 2 ? 2u : (n - below[n] <= above[n] - n ? below[n] : above[n]);
        };
        uint32_t mismatches = 0, firstBad = 0;
        for (uint32_t n = 0; n <= kTop; ++n)
            if (tp::primes::closest(n) != want(n) && mismatches++ == 0)
                firstBad = n;
        if (mismatches > 0)
            std::printf("first mismatch at %u\n", firstBad);
        P.eq("legacy.closest.mismatches_0_to_1395629", static_cast<int64_t>(mismatches), 0);
        uint32_t primesFound = 0;
        for (uint32_t n = 0; n <= kTop; ++n)
            primesFound += tp::primes::isPrime(n) ? 1u : 0u;
        P.eq("legacy.prime_count_to_1395629", static_cast<int64_t>(primesFound), 106782);   // the table's NUM_PRIMES

        // The legacy test's inputs (tests/test_findClosestPrime.cpp), one row each.
        for (const uint32_t n : { 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 20u, 50u, 97u, 98u, 100u, 113u, 200u,
                                  1000u, 1009u, 1013u, 5000u, 10000u, 65537u, 100000u, 999983u, 1395629u })
            P.eq("legacy.closest_" + std::to_string(n), static_cast<int64_t>(tp::primes::closest(n)),
                 static_cast<int64_t>(want(n)));
    }

    // LAYOUT.
    {
        int bad = 0, badOrder = 0, badCap = 0;
        double worstDev = 0.0, worstMean = 0.0;
        for (const float fs : { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 192000.0f })
            for (const float size : { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 8.0f, 12.0f, 20.0f, 35.0f, 60.0f, 100.0f })
                for (const int lines : { 4, 8, 16 })
                {
                    const tp::FdnLayout l = tp::makeLayout(fs, size, lines);
                    double logSum = 0.0;
                    for (int i = 0; i < l.lines; ++i)
                    {
                        const uint32_t d = l.delay[static_cast<std::size_t>(i)];
                        bad += tp::primes::isPrime(d) ? 0 : 1;
                        badOrder += (i > 0 && d <= l.delay[static_cast<std::size_t>(i - 1)]) ? 1 : 0;
                        badCap += static_cast<float>(d) <= tp::maxLayoutDelay(fs) ? 0 : 1;
                        const double t = static_cast<double>(tp::targetLength(fs, size, i, lines));
                        if (size >= 3.0f)
                            worstDev = std::max(worstDev, std::fabs(static_cast<double>(d) / t - 1.0));
                        logSum += std::log2(static_cast<double>(d));
                    }
                    const double mfp = static_cast<double>(fs) * (2.0 * static_cast<double>(size) / 3.0) / 343.0;
                    if (size >= 3.0f)
                        worstMean = std::max(worstMean, std::fabs(std::exp2(logSum / l.lines) / mfp - 1.0));
                }
        P.eq("layout.not_prime", static_cast<int64_t>(bad), 0);
        P.eq("layout.not_ascending", static_cast<int64_t>(badOrder), 0);
        P.eq("layout.over_capacity", static_cast<int64_t>(badCap), 0);
        P.le("layout.worst_deviation_from_target_3m_up", worstDev, 0.03);
        P.le("layout.geometric_mean_vs_mean_free_path", worstMean, 0.02);
    }

    // DEFAULT.
    const tp::FdnLayout l = tp::makeLayout(48000.0f, 12.0f, 16);
    P.eq("default.lines", static_cast<int64_t>(l.lines), 16);
    P.eq("default.shortest", static_cast<int64_t>(l.delay[0]), 787);
    P.eq("default.longest", static_cast<int64_t>(l.delay[15]), 1583);
    return P.finish();
}
