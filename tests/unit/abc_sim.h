/*
 * abc_sim.h — "simulation equal before and after" for the abc pass tests (3D plan Tasks 17 to 19;
 * static inline: a test uses what it needs): the top module's outputs on ASIM_CYCLES cycles of
 * random inputs for each of the seeds 1 .. ASIM_SEEDS, recorded into a trace and compared.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_SIM_H
#define ODIN3_TESTS_UNIT_ABC_SIM_H

#include "ir/design.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { ASIM_CYCLES = 256, ASIM_SEEDS = 3 };

typedef struct asim_trace {
    uint8_t *bits;
    size_t len;
} asim_trace;

static inline uint32_t asim_output_bits(const odin3_sim *sim) {
    uint32_t total = 0;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        total += odin3_sim_output_width(sim, port);
    }
    return total;
}

static inline void asim_seed_run(odin3_sim *sim, uint32_t seed, uint8_t *row) {
    odin3_prng prng;
    odin3_prng_seed(&prng, seed);
    size_t at = 0;
    for (uint32_t cycle = 0; cycle < ASIM_CYCLES; cycle++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
            for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
                bool value = false;
                (void)odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &value);
                row[at++] = value ? 1U : 0U;
            }
        }
    }
}

/* Records the top module's trace into *out (free with asim_free). Fails the test when the design
 * cannot be simulated. */
static inline void asim_record(odin3_design *design, asim_trace *out) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_sim_build(design, odin3_design_top(design), &sim),
                                  "the design cannot be simulated");
    size_t per_seed = (size_t)asim_output_bits(sim) * ASIM_CYCLES;
    out->len = per_seed * ASIM_SEEDS;
    out->bits = calloc(out->len == 0 ? 1 : out->len, 1);
    TEST_ASSERT_NOT_NULL(out->bits);
    for (uint32_t seed = 1; seed <= ASIM_SEEDS; seed++) {
        asim_seed_run(sim, seed, out->bits + per_seed * (seed - 1U));
    }
    odin3_sim_destroy(sim);
}

static inline void asim_free(asim_trace *trace) {
    free(trace->bits);
    *trace = (asim_trace){0};
}

/* The trace of design equals want (the same outputs, the same values on every cycle). */
static inline void asim_expect_equal(odin3_design *design, const asim_trace *want) {
    asim_trace got = {0};
    asim_record(design, &got);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want->len, got.len, "the outputs changed");
    size_t first = want->len;
    for (size_t at = 0; at < want->len && first == want->len; at++) {
        first = want->bits[at] != got.bits[at] ? at : first;
    }
    asim_free(&got);
    TEST_ASSERT_TRUE_MESSAGE(first == want->len, "simulation differs after the pass");
}

#endif
