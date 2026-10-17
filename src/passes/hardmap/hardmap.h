/*
 * hardmap.h — the `hardmap` pass (3C; spec docs/specs/2026-10-14-3C-hardmap-design.md, plan
 * docs/plans/2026-10-14-3C-hardmap-plan.md): binds word cells to the VTR arch's hard blocks where
 * Parmys's defaults do. Internal: not part of the ABI. PR 3C-1 has the `$mul` rule (M-1..M-7);
 * `$add`/`$sub` and the memories arrive with 3C-2 and 3C-3.
 */
#ifndef ODIN3_PASSES_HARDMAP_HARDMAP_H
#define ODIN3_PASSES_HARDMAP_HARDMAP_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"

#include <stdint.h>

/* The width of the arch's multiplier (spec H-1: Parmys's fixed_hard_multiplier size). */
#define ODIN3_HARDMAP_MUL_WIDTH 36U

/* What a run did, for the INFO line of H-10. */
typedef struct odin3_hardmap_stats {
    uint64_t multiply;  /* `multiply` blocks made */
    uint64_t adder;     /* `adder` blocks made */
    uint64_t chains;    /* adder chains made */
    uint64_t sp_slices; /* single_port_ram slices made */
    uint64_t dp_slices; /* dual_port_ram slices made */
    uint64_t soft;      /* cells left soft with a warning (S-2, S-3) */
} odin3_hardmap_stats;

/*
 * The pass: maps, in every module and in node-ID order, the `$mul` cells to `multiply` blocks
 * (spec H-8: `$mul`, then `$add`/`$sub`, then memories). Each source cell is one transaction: every
 * new node is built first, then the source is deleted and its output nets are merged into the new
 * ones, so ODIN3_ERR_NO_MEMORY before that leaves the IR valid and the source cell intact (H-9);
 * a `$mul` whose Y_WIDTH exceeds A_WIDTH + B_WIDTH (unsigned) stays soft so that no merge after the
 * delete allocates. An explicit `multiply` instance is treated like the equivalent `$mul` (WIDTH 2
 * soft as a `$mul`, else padded to 36), with a warning (M-6 as amended 2026-10-17).
 * One provenance operation and one DERIVED record per source cell (H-7). Errors (logged, spec
 * H-3, M-6): a needed hard cell type that is missing, not `hard` or of another shape, an
 * explicit `multiply` wider than 36; ODIN3_ERR_INVALID_ARG, the IR unchanged. ODIN3_ERR_PARSE when
 * the design budget is exceeded, possibly after earlier modules were mapped. *stats (may be NULL)
 * gets the totals; the INFO line of H-10 is logged.
 */
odin3_status odin3_hardmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                               odin3_hardmap_stats *stats);

#endif
