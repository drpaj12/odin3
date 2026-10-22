/*
 * chain.h — carry chains of the hard `adder` cell (spec C-1 to C-8; plan Tasks 4 and 5).
 */
#ifndef ODIN3_PASSES_UNMAP_CHAIN_H
#define ODIN3_PASSES_UNMAP_CHAIN_H

#include "odin3/odin3.h"
#include "passes/unmap/unmap.h"
#include "util/vec.h"

/* The ports of the `adder` cell (partial_map's k_shapes order). */
enum { UM_ADDER_A, UM_ADDER_B, UM_ADDER_CIN, UM_ADDER_COUT, UM_ADDER_SUMOUT };

/* One walk: cells in carry order, and why it cannot be recovered (NULL when it can). */
typedef struct um_walk {
    odin3_vec cells; /* odin3_node_id */
    const char *decline;
} um_walk;

/*
 * C-1: the walks of the module, in node-ID order of their first cell. `walks` becomes an
 * odin3_vec of um_walk; free it with um_walks_free. Linear, no recursion.
 */
odin3_status um_chain_walks(um_ctx *uc, odin3_vec *walks);
void um_walks_free(odin3_vec *walks);

/* Phase A: walks, then the conversion of each recoverable walk. */
odin3_status um_adder_phase(um_ctx *uc);

#endif
