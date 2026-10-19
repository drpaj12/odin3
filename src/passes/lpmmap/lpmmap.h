/*
 * lpmmap.h — the `lpmmap` pass (5.1; spec docs/specs/2026-10-19-5.1-lpm-design.md, plan
 * docs/plans/2026-10-19-5.1-lpm-plan.md): binds word cells to Altera LPM megafunctions by fixed
 * rules: `$add`/`$sub` to `lpm_add_sub` (A-1..A-5), `$mul` to `lpm_mult` (M-1..M-5), the six
 * comparisons to `lpm_compare` (C-1..C-4), and an inferred `$mem` to `altsyncram` (R-1..R-9).
 * Internal: not part of the ABI.
 */
#ifndef ODIN3_PASSES_LPMMAP_LPMMAP_H
#define ODIN3_PASSES_LPMMAP_LPMMAP_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"

#include <stdint.h>

/* The default of `--family` and of `intended_device_family` (spec F-1; D10: Cyclone V). */
#define ODIN3_LPMMAP_DEFAULT_FAMILY "Cyclone V"

typedef struct odin3_lpmmap_opts {
    const char *family; /* intended_device_family of every altsyncram; NULL: the default */
} odin3_lpmmap_opts;

/* What a run did, for the INFO line (spec H-8). */
typedef struct odin3_lpmmap_stats {
    uint64_t add_sub; /* lpm_add_sub blocks made */
    uint64_t mult;    /* lpm_mult blocks made */
    uint64_t compare; /* lpm_compare blocks made */
    uint64_t ram;     /* altsyncram blocks made */
    uint64_t soft;    /* cells left soft with a warning (spec S rules) */
} odin3_lpmmap_stats;

/*
 * The pass: in every module, in node-ID order, `$add`/`$sub`, then `$mul`, then the six
 * comparisons, then `$mem` (spec P-4). Each source cell is one transaction: every new node is
 * built first on fresh nets, then the source is deleted and its output nets are merged into the
 * new ones, so ODIN3_ERR_NO_MEMORY before that leaves the IR valid and the source cell intact
 * (H-8). One provenance operation and one DERIVED record per source cell (H-6). opts NULL means
 * the defaults. Errors (logged, spec P-2): a needed LPM cell type that is missing or not a black
 * box: ODIN3_ERR_INVALID_ARG, the IR unchanged; ODIN3_ERR_PARSE when the design budget is
 * exceeded. *stats (may be NULL) gets the totals; the INFO line of H-8 is logged.
 */
odin3_status odin3_lpmmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                              const odin3_lpmmap_opts *opts, odin3_lpmmap_stats *stats);

#endif
