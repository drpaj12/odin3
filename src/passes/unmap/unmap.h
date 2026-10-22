/*
 * unmap.h — the `unmap` pass (v1.0.4; spec docs/specs/2026-10-21-unmap-design.md): the inverse of
 * partial_map. The VTR hard cells (`adder`, `multiply`, `single_port_ram`, `dual_port_ram`) become
 * `$add`/`$sub`, `$mul` and `$mem` cells with provenance, exactly or not at all. Per module, in
 * node-ID order; every recovered group is one replacement transaction (um_replace, kit.h).
 */
#ifndef ODIN3_PASSES_UNMAP_UNMAP_H
#define ODIN3_PASSES_UNMAP_UNMAP_H

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/unmap/kit.h"

#include <stdbool.h>
#include <stdint.h>

/* The options (S-4). */
typedef struct um_opts {
    bool regroup; /* default true; --no-regroup clears */
} um_opts;

/* Counts for the log line (P-4): cells, not modules. */
typedef struct um_stats {
    uint64_t adder_cells, add, sub, multiply, mul, regrouped_mul, ram_cells, mem, declined, cands;
} um_stats;

/* The per-module context of the phases. */
struct um_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    const um_opts *opts;
    um_types types;
    um_stats stats;
    odin3_net_id konst[2]; /* cached constant nets of the module: gnd, vcc */
};

/*
 * Runs unmap on every module of the design (module-ID order). stats may be NULL. A design without
 * hard cells is left as it is. Errors: ODIN3_ERR_INVALID_ARG (E-2, E-3), ODIN3_ERR_NO_MEMORY.
 */
odin3_status um_run(odin3_pass_ctx *ctx, odin3_design *design, const um_opts *opts,
                    um_stats *stats);

/* One module: the phases of S-3. */
odin3_status um_module(um_ctx *uc);

#endif
