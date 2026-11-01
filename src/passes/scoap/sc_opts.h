/*
 * sc_opts.h — options of the scoap pass and the signal-probability helper (spec sections 4, 5).
 */
#ifndef ODIN3_PASSES_SCOAP_SC_OPTS_H
#define ODIN3_PASSES_SCOAP_SC_OPTS_H

#include "passes/graphlets/gl_int.h"
#include "passes/scoap/sc_int.h"

typedef struct sc_opts {
    bool seq;
    uint32_t top_n;
    uint64_t vectors; /* --sigprob */
    uint64_t seed;
    double threshold;
    odin3_strbuf out;
    gl_opts gl; /* only the focus fields are used (gl_groups_make) */
} sc_opts;

/* P(net = 1) in parts per million per net of module top (-1: not simulated), from opts->vectors
 * random vectors of the simulator. *ppm gets net_end entries, caller frees (odin3_util_free).
 * The simulator's refusals of a combinational loop or a multiply driven net are not fatal: the run
 * is skipped with a WARNING, every ppm stays -1 and *ran is false. Any other failure (budget,
 * unsupported cell, clock, invalid argument, memory) is returned as the status. */
odin3_status sc_sigprob(odin3_design *design, odin3_module_id top, const sc_opts *opts,
                        int32_t **ppm, bool *ran);

#endif
