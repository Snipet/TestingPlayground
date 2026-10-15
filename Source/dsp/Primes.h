// Source/dsp/Primes.h: the primes the FDN's delay lengths are snapped to (JUCE-free).
//
// Ported from the legacy FunkDSP::findClosestPrime (git show legacy-visage:Source/dsp/Prime.cpp): a binary search over
// a 966 KB table of the first 106,782 primes (2 .. 1,395,629), returning the prime nearest n with a tie going to the
// lower prime. closest() gives the same answer for every n in the table's range without the table (trial division;
// the dsp.primes probe compares the two over the whole range). Beyond the range the legacy answered its last prime;
// closest() answers the true nearest prime. The FDN never asks for more than about 53,000 (100 m at 192 kHz).
//
// Cost: isPrime is at most sqrt(n) / 3 divisions (about 80 for the FDN's lengths); closest() and nextAbove() stop at
// the first prime, a few candidates away. The Engine calls them when SIZE or LINES changes, at most 16 times a block.
#pragma once

#include <cstdint>

namespace testingplayground::primes
{
    // n is prime: trial division by 2, 3 and 6k +- 1 up to sqrt(n). Exact for every uint32_t.
    constexpr bool isPrime(uint32_t n) noexcept
    {
        if (n < 2u)
            return false;
        if (n < 4u)
            return true;
        if (n % 2u == 0u || n % 3u == 0u)
            return false;
        for (uint32_t k = 5u; static_cast<uint64_t>(k) * k <= n; k += 6u)
            if (n % k == 0u || n % (k + 2u) == 0u)
                return false;
        return true;
    }

    // The prime nearest n, a tie going to the lower prime; 2 for n <= 2 (the legacy findClosestPrime's rule).
    constexpr uint32_t closest(uint32_t n) noexcept
    {
        if (n <= 2u)
            return 2u;
        for (uint32_t d = 0u;; ++d)
        {
            if (d + 2u <= n && isPrime(n - d))              // the lower candidate first: ties go down
                return n - d;
            if (isPrime(n + d))
                return n + d;
        }
    }

    // The smallest prime greater than n.
    constexpr uint32_t nextAbove(uint32_t n) noexcept
    {
        uint32_t p = n + 1u;
        while (!isPrime(p))
            ++p;
        return p;
    }
}
