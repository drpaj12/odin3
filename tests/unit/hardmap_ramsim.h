/*
 * hardmap_ramsim.h — a small test-only cycle simulator over the IR for the RAM tests of 3C (plan
 * Task 5). The 1E simulator has no model of a hard RAM, so this one evaluates the cells a RAM
 * mapping leaves behind: constants, `$and`, `$not`, `$mux`, `$dff`, `single_port_ram` and
 * `dual_port_ram`, plus (3C Task 6) `$dff`, `$dffe` of any width and `$mem` (rising edge only,
 * OFFSET 0, unwritten words 0). A RAM registers its read of the old word, then writes (the
 * primitives' "old data read-first behaviour"); the pad (`$_CONSTX_`) reads 0. Both ports of a
 * `dual_port_ram` read the old word; a same-address dual write is outside the model (R-6). Nets
 * that nothing drives and the test does not randomise stay 0.
 */
#ifndef ODIN3_TESTS_UNIT_HARDMAP_RAMSIM_H
#define ODIN3_TESTS_UNIT_HARDMAP_RAMSIM_H

#include "ir/ids.h"
#include "ir/module.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdint.h>

typedef struct rs_io {
    const odin3_net_id *in; /* nets set to random bits each cycle */
    uint32_t n_in;
    const odin3_net_id *out; /* nets traced each cycle, after the combinational settle */
    uint32_t n_out;
} rs_io;

typedef struct rs_run {
    uint32_t cycles;
    uint64_t seed;
} rs_run;

/* Appends one line of '0'/'1' per cycle for `cycles` cycles from the seeded random inputs. The
 * trace of two designs is equal when they behave alike at those nets. Asserts (Unity) on any
 * unsupported cell or on out of memory. */
void rs_trace(const odin3_module *module, const rs_io *io, rs_run run, odin3_strbuf *out);

#endif
