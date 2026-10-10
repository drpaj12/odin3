/*
 * vectors.h — the simulator's random-vector text format (odin3-sim-vectors, the `sim` pass).
 *
 * A header of '#' lines: "# <title> seed S cycles N", then one line per port in port order:
 * "# input NAME W" (W = its non-clock bits, omitted when it has none), "# clock NAME K" per clock
 * bit K of an input, "# output NAME W". Then one line per cycle, "cycle inputs outputs": the
 * cycle number from 0, the input bits driven (every non-clock bit, from the seeded PRNG) and the
 * output bits after the cycle, each field the bits of every port in port order, each port MSB
 * first, clock bits left out ("-" for an empty field). The text is a function of the netlist,
 * the title and the seed.
 */
#ifndef ODIN3_SIM_VECTORS_H
#define ODIN3_SIM_VECTORS_H

#include "odin3/odin3.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "util/str.h"

#include <stdint.h>

/* One vector run: the header's title (borrowed), the PRNG seed and the number of cycles. */
typedef struct odin3_sim_vectors {
    const char *title;
    uint64_t seed;
    uint32_t cycles;
} odin3_sim_vectors;

/* Appends the header lines to buf. ODIN3_ERR_NO_MEMORY (buf may hold part of it then). */
odin3_status odin3_sim_vectors_header(const odin3_sim *sim, const odin3_sim_vectors *run,
                                      odin3_strbuf *buf);

/*
 * Drives every non-clock input bit from prng (odin3_sim_drive_random), runs one cycle
 * (odin3_sim_cycle) and appends its line "cycle inputs outputs\n" to buf. ODIN3_ERR_NO_MEMORY
 * (buf may hold part of the line then; the cycle has run).
 */
odin3_status odin3_sim_vectors_step(odin3_sim *sim, odin3_prng *prng, uint32_t cycle,
                                    odin3_strbuf *buf);

#endif
