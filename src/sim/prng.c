/* prng.c — xorshift64* seeded through splitmix64 (prng.h has the fixed contract). */
#include "sim/prng.h"

#include <stdbool.h>
#include <stdint.h>

/* splitmix64 (Steele, Lea, Flood 2014): spreads small seeds over the 64-bit state. */
static const uint64_t k_golden_gamma = 0x9E3779B97F4A7C15ULL;
static const uint64_t k_mix_one = 0xBF58476D1CE4E5B9ULL;
static const uint64_t k_mix_two = 0x94D049BB133111EBULL;
enum { MIX_SHIFT_ONE = 30, MIX_SHIFT_TWO = 27, MIX_SHIFT_THREE = 31 };

/* xorshift64* (Vigna 2016). */
static const uint64_t k_star = 0x2545F4914F6CDD1DULL;
enum { XS_SHIFT_ONE = 12, XS_SHIFT_TWO = 25, XS_SHIFT_THREE = 27, TOP_BIT = 63 };

/* Any non-zero value: splitmix64 maps exactly one seed to 0, and xorshift needs a non-zero state.
 */
static const uint64_t k_nonzero = k_golden_gamma;

void odin3_prng_seed(odin3_prng *prng, uint64_t seed) {
    uint64_t mix = seed + k_golden_gamma;
    mix = (mix ^ (mix >> MIX_SHIFT_ONE)) * k_mix_one;
    mix = (mix ^ (mix >> MIX_SHIFT_TWO)) * k_mix_two;
    mix ^= mix >> MIX_SHIFT_THREE;
    prng->state = mix != 0 ? mix : k_nonzero;
}

uint64_t odin3_prng_next(odin3_prng *prng) {
    uint64_t state = prng->state;
    state ^= state >> XS_SHIFT_ONE;
    state ^= state << XS_SHIFT_TWO;
    state ^= state >> XS_SHIFT_THREE;
    prng->state = state;
    return state * k_star;
}

bool odin3_prng_bit(odin3_prng *prng) {
    return (odin3_prng_next(prng) >> TOP_BIT) != 0;
}
