#include "../Source/dsp/Prime.h"

#include <cstdio>
#include <cstdlib>

// Reference implementation: linear scan of the sorted primes[] table,
// returns the prime with the smallest absolute distance to n. Ties go to
// the lower prime.
static int referenceClosestPrime(int n) {
    using namespace FunkDSP;
    for (size_t i = 0; i < NUM_PRIMES; ++i) {
        if (primes[i] >= n) {
            if (i == 0) return primes[0];
            int distLo = n - primes[i - 1];
            int distHi = primes[i] - n;
            return (distLo <= distHi) ? primes[i - 1] : primes[i];
        }
    }
    return primes[NUM_PRIMES - 1];
}

int main() {
    using namespace FunkDSP;

    int failures = 0;
    int checks = 0;

    int testInputs[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        20, 50, 97, 98, 100, 113, 200,
        1000, 1009, 1013, 5000, 10000,
        65537, 100000, 999983, 1395629,
    };

    for (int n : testInputs) {
        int got = findClosestPrime(n);
        int expected = referenceClosestPrime(n);
        ++checks;
        const char* status = (got == expected) ? "PASS" : "FAIL";
        std::printf("[%s] findClosestPrime(%d) = %d  (expected %d)\n",
                    status, n, got, expected);
        if (got != expected) ++failures;
    }

    std::printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
