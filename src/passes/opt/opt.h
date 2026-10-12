/*
 * opt.h — the opt family (2F). Created by 2E Task 3 (plan point (c), ruling I2) with the two
 * types proc's statistics need and the declaration of odin3_opt_expr_run, proc's step F
 * (2E spec §2.2; 2F spec §4.8). Those three are frozen: 2F Task 1 appends below them and 2F Task 6
 * defines odin3_opt_expr_run in expr.c without changing the declaration. Nothing in 2E calls it
 * until point (f)'s pre-merge step, so 2E links without 2F.
 */
#ifndef ODIN3_PASSES_OPT_OPT_H
#define ODIN3_PASSES_OPT_OPT_H

#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* One flag per option of the opt passes (2F spec §3), spelled as Yosys's. */
typedef struct odin3_opt_opts {
    bool mux_undef, mux_bool, undriven, noclkinv, fine, full, keepdc; /* opt_expr */
    bool nodffe, nosdff;                                              /* opt_dff */
    bool purge;                                                       /* opt_clean */
    bool nomux;                                                       /* opt_merge */
    bool fast, noff;                                                  /* opt */
} odin3_opt_opts;

enum { ODIN3_OPT_EXPR_MAX_RULES = 128 };

/* opt_expr's counts (2F spec §16); callers zero them, odin3_opt_expr_run adds into them. */
typedef struct odin3_opt_expr_counts {
    uint32_t rewritten, folded;
    uint32_t rule[ODIN3_OPT_EXPR_MAX_RULES]; /* indexed by 2F's odin3_opt_rule */
} odin3_opt_expr_counts;

/* opt_expr on one module (2F §4.8), defined by 2F Task 6; adds into *counts. */
odin3_status odin3_opt_expr_run(odin3_pass_ctx *ctx, odin3_module *module,
                                const odin3_opt_opts *opts, odin3_opt_expr_counts *counts);

/* --- 2F Task 1 (appended below 2E-1's frozen types) ---------------------------------------- */

/* opt_dff's per-action counters (plan point (k)): at most this many odin3_opt_dff_action values. */
enum { ODIN3_OPT_DFF_MAX_ACTIONS = 32 };

/*
 * The caps (2F plan, Global Constraints): opt's iterations per invocation (OP-12), opt_muxtree's
 * eval_mux calls per module (point (r)), and a budget check every 2^16 pins the kit creates
 * (odin3_kit_new_cell; the budget itself is 2E-1's odin3_budget_check). The first two are lowered
 * by tests through odin3_opt_test_set_limits (opt_test.h).
 */
enum {
    ODIN3_OPT_MAX_ITERATIONS = 10000,
    ODIN3_OPT_MUXTREE_MAX_EVALS = 100000,
    ODIN3_OPT_BUDGET_PINS = 1 << 16
};

/*
 * Every sub-pass's counters in one aggregate (plan point (b)): opt_expr's frozen counts, then one
 * counter per §16 info line and opt_dff's per-action counters (indexed by odin3_opt_dff_action).
 * Callers zero it; each sub-pass call adds into it, and --opt-stats prints from it.
 */
typedef struct odin3_opt_counts {
    odin3_opt_expr_counts expr;
    uint32_t merge_removed, muxtree_ports, reduce_changes, share_merged, dff_changed,
        dff_const_bits;
    uint32_t dff_action[ODIN3_OPT_DFF_MAX_ACTIONS];
    uint32_t clean_cells, clean_nets, clean_wires, iterations;
} odin3_opt_counts;

#endif
