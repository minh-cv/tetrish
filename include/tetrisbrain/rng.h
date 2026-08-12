#ifndef TETRISH_BRAIN_RNG_H
#define TETRISH_BRAIN_RNG_H

#include <stdint.h>

// splitmix64: a self-contained generator so a game is reproducible from its
// seed alone, independent of the process-wide rand() sequence.
typedef struct Rng {
    uint64_t state;
} Rng;

void rng_init(Rng* rng, uint64_t seed);
uint64_t rng_next(Rng* rng);
// Uniform in [0, bound), bound must be positive.
int rng_below(Rng* rng, int bound);

#endif
