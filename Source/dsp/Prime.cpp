#include "Prime.h"
#include <cstdlib>

namespace FunkDSP{

int findClosestPrime(int n){

    int lower_bound = 0;
    int upper_bound = NUM_PRIMES - 1;
    while(upper_bound - lower_bound > 1){
        int middle = (lower_bound + upper_bound) / 2;

        if(n < primes[middle]){
            // Lower bound remains the same
            // Upper bound becomes middle
            upper_bound = middle;
        }else{
            lower_bound = middle;
        }
    }
    int distLo = std::abs(n - primes[lower_bound]);
    int distHi = std::abs(n - primes[upper_bound]);
    return (distLo <= distHi) ? primes[lower_bound] : primes[upper_bound];
}

};