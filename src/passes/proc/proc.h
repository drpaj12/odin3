/*
 * proc.h — the proc pass (2E spec §2, §16, §17; PR-3, PR-4, PR-22): lowers every $proc node to
 * ordinary cells as Yosys 0.55 `proc -norom` does, one module at a time and each module completely
 * or not at all. The pass definition (odin3_pass_proc) is declared in passes/builtin.h.
 */
#ifndef ODIN3_PASSES_PROC_PROC_H
#define ODIN3_PASSES_PROC_PROC_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"

#include <stdbool.h>
#include <stdint.h>

/* Caps (§16), checked before the work they bound; tests lower them (proc_test.h). */
enum {
    ODIN3_PROC_MAX_ERRORS = 20,
    ODIN3_PROC_MAX_ARST_ROUNDS = 100,
    ODIN3_PROC_MAX_SIGNAL_WALK = 1024
};
#define ODIN3_PROC_MAX_PATTERN_BITS (1U << 24) /* per pattern pool (W5a, W5b) */
#define ODIN3_PROC_BUDGET_PINS (1U << 16)      /* a budget check every 2^16 created pins */

/* --stop-after: the last analysis phase run before the processes are re-encoded. */
typedef enum odin3_proc_stop {
    ODIN3_PROC_STOP_NONE,
    ODIN3_PROC_STOP_CLEAN,
    ODIN3_PROC_STOP_RMDEAD,
    ODIN3_PROC_STOP_PRUNE,
    ODIN3_PROC_STOP_ARST
} odin3_proc_stop;

/* The warnings of §15, counted in odin3_proc_stats.warnings. */
typedef enum odin3_proc_warning {
    ODIN3_PROC_W1,
    ODIN3_PROC_W2,
    ODIN3_PROC_W3,
    ODIN3_PROC_W4,
    ODIN3_PROC_W5A,
    ODIN3_PROC_W5B,
    ODIN3_PROC_N_WARNINGS
} odin3_proc_warning;

typedef struct odin3_proc_opts {
    bool noopt;                 /* --noopt: skip step F */
    odin3_proc_stop stop_after; /* --stop-after; implies noopt */
    bool share_selects;         /* --share-selects: one select cell per case, not per signal */
} odin3_proc_opts;

/* Cells counted in odin3_proc_stats.cells. */
typedef enum odin3_proc_kind {
    ODIN3_PROC_K_DFF,
    ODIN3_PROC_K_ADFF,
    ODIN3_PROC_K_ALDFF,
    ODIN3_PROC_K_DFFSR,
    ODIN3_PROC_K_DLATCH,
    ODIN3_PROC_K_MUX,
    ODIN3_PROC_K_PMUX,
    ODIN3_PROC_K_EQ,
    ODIN3_PROC_K_HELPER, /* $not $and $reduce_or of selects, holds and triggers */
    ODIN3_PROC_N_KINDS
} odin3_proc_kind;

/* The counts of a run, filled on success and on failure. */
typedef struct odin3_proc_stats {
    uint32_t processes_lowered, modules_lowered, modules_failed, errors;
    uint32_t warnings[ODIN3_PROC_N_WARNINGS]; /* W1 … W5b */
    uint32_t cells[ODIN3_PROC_N_KINDS];
    odin3_opt_expr_counts opt_expr; /* step F, 2F §18; zero under --noopt */
} odin3_proc_stats;

/*
 * Runs proc on design inside pass run ctx (its records and messages are ctx's): per module in
 * creation order, skipping those without processes, step A (analysis: decode, clean, rmdead,
 * prune, arst, the clock pre-check; nothing in the IR changes), then, unless a process of the
 * module has an error, steps B-F (mux, latch, flip-flops, orphan nets, opt_expr). A module with
 * an error keeps every $proc node, and later modules are only analysed. opts NULL means no option;
 * *stats (may be NULL) is zeroed, then filled, also on failure. Logs the info line of §2.3.
 * ODIN3_OK; ODIN3_ERR_PARSE after a user error (located, §15), at the error cap (E7) or past the
 * byte budget (E6); ODIN3_ERR_NO_MEMORY (the module being lowered is left valid but incomplete);
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL ctx or design; ODIN3_ERR_CHECK is a 2E bug.
 */
odin3_status odin3_proc_run(odin3_pass_ctx *ctx, odin3_design *design, const odin3_proc_opts *opts,
                            odin3_proc_stats *stats);

#endif
