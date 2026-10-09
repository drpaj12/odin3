/*
 * prng.h — the simulator's fixed pseudo-random generator (xorshift64*), seedable, no libc rand.
 *
 * The sequence for a seed is part of the simulator's contract (vector files are reproducible from
 * a seed on every platform): the state is splitmix64(seed) (a fixed non-zero constant when that
 * is 0), and each draw is one xorshift64* step (shifts 12, 25, 27; multiplier
 * 0x2545F4914F6CDD1D).
 */
#ifndef ODIN3_SIM_PRNG_H
#define ODIN3_SIM_PRNG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_prng {
    uint64_t state; /* never 0 */
} odin3_prng;

/* Seeds prng; any seed (0 included) gives a valid, distinct-looking sequence. */
void odin3_prng_seed(odin3_prng *prng, uint64_t seed);

/* The next 64-bit draw. */
uint64_t odin3_prng_next(odin3_prng *prng);

/* One random bit: the top bit of the next draw (the best-mixed bit of xorshift64*). */
bool odin3_prng_bit(odin3_prng *prng);

#endif
