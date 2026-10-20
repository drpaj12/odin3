/*
 * hardmap.h — the `hardmap` pass (3C; spec docs/specs/2026-10-14-3C-hardmap-design.md, plan
 * docs/plans/2026-10-14-3C-hardmap-plan.md): binds word cells to the VTR arch's hard blocks where
 * Parmys's defaults do. Internal: not part of the ABI. PR 3C-1 has the `$mul` rule (M-1..M-7), 3C-2
 * the `$add`/`$sub` rule (A-1..A-9), 3C-3 the explicit RAM instances (R-1..R-7); the inferred
 * `$mem` follows.
 */
#ifndef ODIN3_PASSES_HARDMAP_HARDMAP_H
#define ODIN3_PASSES_HARDMAP_HARDMAP_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/partial_map/rules.h"

#include <stdbool.h>
#include <stdint.h>

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
 * The pass: maps, in every module and in node-ID order, the `$mul` cells to `multiply` blocks, then
 * the `$add` and `$sub` cells to `adder` chains of max(A_WIDTH, B_WIDTH) + 2 cells, a signed cell's
 * operands sign-extended; then each `single_port_ram` / `dual_port_ram` instance that is not 15
 * address bits by 1 data bit into slices of that shape (depth split with registered selects, width
 * split, address padding; an instance of more than 31 address bits is the located error; spec H-8:
 * `$mul`, then `$add`/`$sub`, then memories). Each source cell is
 * one transaction: every new node is built first, then the source is deleted and its output nets
 * are merged into the new ones, so ODIN3_ERR_NO_MEMORY before that leaves the IR valid and the
 * source cell intact (H-9); a `$mul` whose Y_WIDTH exceeds A_WIDTH + B_WIDTH (unsigned), and an
 * `$add`/`$sub` whose Y_WIDTH exceeds max(A_WIDTH, B_WIDTH) + 1, stay soft so that no merge after
 * the delete allocates. An explicit `multiply` instance is treated like the equivalent `$mul`
 * (WIDTH 2 soft as a `$mul`, else padded to the arch's multiplier width W), with a warning (M-6 as
 * amended 2026-10-17); one above W is the equivalent `$mul`. An unsigned `$mul` with an operand
 * above W is split into smaller `$mul` and `$add` cells (4.3 X-4, mul_split.c). One
 * provenance operation and one DERIVED record per source cell (H-7). Errors (logged, spec H-3,
 * M-6): a needed hard cell type that is missing, not `hard` or of another shape;
 * ODIN3_ERR_INVALID_ARG, the IR unchanged. ODIN3_ERR_PARSE when the
 * design budget is exceeded, possibly after earlier modules were mapped. *stats (may be NULL) gets
 * the totals; the INFO line of H-10 is logged.
 */
odin3_status odin3_partial_map_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   odin3_hardmap_stats *stats);

/* The options of the pass (all off by default; S-5). */
typedef struct odin3_partial_map_opts {
    bool allow_unknown_blocks; /* `--allow-unknown-blocks` (B-6): an unknown black box is a warning
                                */
    bool mem_width_arch;   /* `--mem-width arch`: slice memories to the arch's widest mode (R-4) */
    uint32_t min_add;      /* `--min-add N` (4.2 C-4): an `$add`/`$sub` of fewer than N bits is
                              left to lower; 0 is "not given" (1) */
    bool parmys_split_ext; /* `--parmys-split-ext` (4.3 D19): the B split of a multiplier
                              sign-extends the low product as Parmys does; for hardmap-diff's retry,
                              not for use */
    uint32_t min_mult;     /* `--min-mult N` (4.2 X-3): a `$mul` whose operands are both at least 2
                              bits and whose wider operand has fewer than N bits is left to lower;
                              0 is "not given" (3) */
} odin3_partial_map_opts;

/* odin3_partial_map_run with options (NULL: the defaults). `--mem-width arch` on a design without
 * an arch record is a WARNING and changes nothing. */
odin3_status odin3_partial_map_run_opts(odin3_pass_ctx *ctx, odin3_design *design,
                                        const odin3_partial_map_opts *opts,
                                        odin3_hardmap_stats *stats);

/* The 3C name of odin3_partial_map_run (4.2 S-1), kept for the existing tests. */
odin3_status odin3_hardmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                               odin3_hardmap_stats *stats);

#endif
