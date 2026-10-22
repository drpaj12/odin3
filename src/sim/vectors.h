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
 * Appends the line "cycle inputs outputs\n" for the current input and output values to buf (no
 * drive, no cycle). ODIN3_ERR_NO_MEMORY (buf may hold part of the line then).
 */
odin3_status odin3_sim_vectors_line(const odin3_sim *sim, uint32_t cycle, odin3_strbuf *buf);

/*
 * Drives every non-clock input bit from prng (odin3_sim_drive_random), runs one cycle
 * (odin3_sim_cycle) and appends its line (odin3_sim_vectors_line). ODIN3_ERR_NO_MEMORY (buf may
 * hold part of the line then; the cycle has run).
 */
odin3_status odin3_sim_vectors_step(odin3_sim *sim, odin3_prng *prng, uint32_t cycle,
                                    odin3_strbuf *buf);

/* Largest input vector file the reader accepts (SV-2). */
#define ODIN3_SIM_VECTORS_MAX_BYTES (1U << 28)

/* A parsed input vector file (odin3_sim_vectors_parse). */
typedef struct odin3_sim_vectors_in {
    uint8_t *bits;   /* cycles x n_bits bytes of 0/1, line by line (ports in order, MSB first) */
    uint32_t n_bits; /* the design's non-clock input bits */
    uint32_t cycles; /* vector lines */
    uint64_t seed;   /* the title line's seed; 0 without one */
} odin3_sim_vectors_in;

/*
 * Parses vector file text (SV-2) for sim's design: every error is logged as "<path>:<line>:
 * <message>" (SV-3) and is ODIN3_ERR_PARSE; ODIN3_ERR_NO_MEMORY. *out is zeroed on failure; free
 * a result with odin3_sim_vectors_in_free.
 */
odin3_status odin3_sim_vectors_parse(const odin3_sim *sim, const char *path, odin3_bytes text,
                                     odin3_sim_vectors_in *out);

/*
 * Reads the file at path (at most ODIN3_SIM_VECTORS_MAX_BYTES) and parses it. ODIN3_ERR_IO
 * ("sim: cannot read `<path>`: <reason>"), ODIN3_ERR_PARSE for a file over the limit or a bad
 * line, ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_sim_vectors_read(const odin3_sim *sim, const char *path,
                                    odin3_sim_vectors_in *out);

/* Frees in->bits and zeroes *in (safe on a zeroed or NULL in). */
void odin3_sim_vectors_in_free(odin3_sim_vectors_in *in);

/* Sets every non-clock input bit of sim from line `cycle` of in (the writer's order, inverse). */
void odin3_sim_vectors_drive(odin3_sim *sim, const odin3_sim_vectors_in *in, uint32_t cycle);

#endif
