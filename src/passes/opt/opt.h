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
#include "util/vec.h"

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
    /*
     * 2F Task 3: opt_clean's did_something events (buffers wired, non-constant nodes removed, wires
     * deleted, init bits removed). Beyond the briefed fields: odin3_opt_clean_run has no changed
     * output, and those events are not all in clean_cells/clean_wires (Yosys counts neither the
     * buffers nor the inits, and constants have no Yosys counterpart); the loop reads this one.
     */
    uint32_t clean_changes;
} odin3_opt_counts;

/* --- 2F Task 3: opt_clean (OP-10) ----------------------------------------------------------- */

/*
 * opt_clean (2F spec §10, Yosys rmunused_module) over every module of design, in creation order,
 * except those with live $proc nodes (INFO "opt_clean: module 'm' has processes; skipped"). Per
 * module: `$_BUF_` nodes without `keep` become wiring; liveness spreads back from the module's
 * ports, `keep` wires and `keep` nodes (instances of `keep` modules included); dead nodes are
 * deleted in ID order (a dead FF's Q init bits go with it); an `init` bit goes only where its net
 * is constant-driven with the opposite value (WARN), every other init stays; wires whose nets have
 * no pins are deleted (also, with opts->purge, public wires that are not the primary name of a net
 * with pins; ports, `keep` and initialised wires stay); nets left with no pins, wire or aliases are
 * deleted. Dead `$_CONST*_` nodes have no Yosys counterpart: they are removed silently, uncounted
 * and without a change. It adds into counts (clean_cells, clean_nets, clean_wires, clean_changes)
 * and logs the INFO line "opt_clean: removed N unused cells, M unused nets and W unused wires"
 * (this call's totals). ODIN3_ERR_INVALID_ARG (logged) for a NULL argument; ODIN3_ERR_NO_MEMORY,
 * possibly after some modules were cleaned (the IR stays valid).
 */
odin3_status odin3_opt_clean_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts);

/*
 * Yosys's keep_cache: keep is cleared and receives one bool per module ID (index 0 unused, so
 * odin3_design_module_end entries; the vec's element size must be sizeof(bool), else INVALID_ARG):
 * true when the module has `keep`, contains a node or wire with `keep`, or (iteratively) contains
 * an instance of a module for which it is true. NO_MEMORY (keep then empty).
 */
odin3_status odin3_opt_keep_modules(odin3_design *design, odin3_vec *keep);

/* --- 2F Task 4: registrations, options, the sub-pass call wrapper (OP-12, OP-16) ------------ */

/* A pass's parsed arguments (plan point (c)). */
typedef struct odin3_opt_args {
    odin3_opt_opts opts;
    bool stats, full_rescan; /* --opt-stats, --opt-full-rescan */
} odin3_opt_args;

/*
 * Parses args (the words after the pass name) for pass ("opt", "opt_expr", ..., "opt_clean"):
 * Yosys's spellings and a `--` spelling of each; -full implies -mux_undef -mux_bool -undriven -fine
 * (opt_expr, opt) and -fine (opt_reduce); --opt-stats everywhere, --opt-full-rescan on opt and
 * opt_expr. *out is zeroed first. An unknown word logs "<pass>: unknown argument '<arg>'" and gives
 * ODIN3_ERR_INVALID_ARG, as does an unknown pass or a NULL out.
 */
odin3_status odin3_opt_parse_args(const char *pass, odin3_bytes args, odin3_opt_args *out);

/* The sub-passes odin3_opt_call runs, in the order of opt's script. */
typedef enum odin3_opt_sub {
    ODIN3_OPT_SUB_EXPR,
    ODIN3_OPT_SUB_MERGE,
    ODIN3_OPT_SUB_MUXTREE,
    ODIN3_OPT_SUB_REDUCE,
    ODIN3_OPT_SUB_SHARE,
    ODIN3_OPT_SUB_DFF,
    ODIN3_OPT_SUB_CLEAN,
    ODIN3_OPT_N_SUBS
} odin3_opt_sub;

typedef struct odin3_opt_call_req {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_opt_sub sub;
    const odin3_opt_opts *opts;
    odin3_opt_counts *counts;
} odin3_opt_call_req;

/*
 * One sub-pass call: the on_call test hook, the budget check (over budget logs "<pass> needs more
 * than N bytes (...)", the pass being ctx's, and gives ODIN3_ERR_PARSE), the "before" snapshot when
 * a snapshot directory is set, the sub-pass, in Debug odin3_check_design FAST, and the snapshot
 * files when the call changed anything. *changed is set to whether it did (the growth of the
 * sub-pass's counters in counts). A sub-pass that is still a stub logs "<sub>: not available yet
 * (2F)" and gives ODIN3_ERR_INVALID_ARG. ODIN3_ERR_INVALID_ARG (logged) for a NULL argument.
 */
odin3_status odin3_opt_call(const odin3_opt_call_req *req, bool *changed);

/* odin3_opt_set_snapshot_dir is public (odin3.h); register.c defines it. */

/* --opt-stats: INFO "opt: rule <name> N" per non-zero rule or opt_dff action counter, by name. */
void odin3_opt_print_stats(const odin3_opt_counts *counts);

#endif
