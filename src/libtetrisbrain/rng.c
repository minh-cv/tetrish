#include "tetrisbrain/rng.h"

#include <assert.h>

void rng_init(Rng* rng, uint64_t seed) {
    rng->state = seed;
}

uint64_t rng_next(Rng* rng) {
    uint64_t z = (rng->state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

int rng_below(Rng* rng, int bound) {
    assert(bound > 0);

    uint64_t limit = (uint64_t)bound;
    // Reject the leading partial block so every value is equally likely.
    uint64_t reject_below = (UINT64_MAX % limit + 1) % limit;
    for (;;) {
        uint64_t r = rng_next(rng);
        if (r >= reject_below) return (int)(r % limit);
    }
}
