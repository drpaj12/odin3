/*
 * opt_loop.c — the `opt` driver (2F spec §3, §12, OP-12; plan Task 13): Yosys 0.55 opt.cc's loop,
 * mirrored, as a table of sub-pass steps per mode, each run through odin3_opt_call (hook, budget,
 * snapshot, Debug check). The session (dirty.h) makes opt_expr incremental after its first call;
 * the cap (ODIN3_OPT_MAX_ITERATIONS) ends a loop that does not reach its fixed point; the last
 * step is `compact` (IR-6; point (h)).
 */
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/opt/dirty.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* When a step of a loop runs. */
typedef enum step_when {
    WHEN_ALWAYS,
    WHEN_FULL,     /* only with -full */
    WHEN_NOT_NOFF, /* unless -noff */
} step_when;

typedef struct step {
    odin3_opt_sub sub;
    step_when when;
} step;

/* The default loop body, in Yosys's order. */
static const step k_default_loop[] = {
    {ODIN3_OPT_SUB_MUXTREE, WHEN_ALWAYS}, {ODIN3_OPT_SUB_REDUCE, WHEN_ALWAYS},
    {ODIN3_OPT_SUB_MERGE, WHEN_ALWAYS},   {ODIN3_OPT_SUB_SHARE, WHEN_FULL},
    {ODIN3_OPT_SUB_DFF, WHEN_NOT_NOFF},   {ODIN3_OPT_SUB_CLEAN, WHEN_ALWAYS},
    {ODIN3_OPT_SUB_EXPR, WHEN_ALWAYS},
};

/* The -fast loop body; opt_clean follows it unless the loop breaks first. */
static const step k_fast_loop[] = {
    {ODIN3_OPT_SUB_EXPR, WHEN_ALWAYS},
    {ODIN3_OPT_SUB_MERGE, WHEN_ALWAYS},
    {ODIN3_OPT_SUB_DFF, WHEN_NOT_NOFF},
};

typedef struct loop {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    const odin3_opt_args *args;
    odin3_opt_counts *counts;
    bool dff_changed; /* the last opt_dff call changed something (-fast's test) */
} loop;

static bool step_applies(const loop *lp, step_when when) {
    switch (when) {
    case WHEN_FULL:
        return lp->args->opts.full;
    case WHEN_NOT_NOFF:
        return !lp->args->opts.noff;
    case WHEN_ALWAYS:
        break;
    }
    return true;
}

/* One sub-pass call; *any is set when it changed anything. */
static odin3_status call_sub(loop *lp, odin3_opt_sub sub, const odin3_opt_opts *opts, bool *any) {
    odin3_opt_call_req req = {lp->ctx, lp->design, sub, opts, lp->counts};
    bool changed = false;
    odin3_status st = odin3_opt_call(&req, &changed);
    *any = *any || changed;
    if (sub == ODIN3_OPT_SUB_DFF) {
        lp->dff_changed = changed;
    }
    return st;
}

/* The steps of a table that apply, in order; *any is set when one changed anything. */
static odin3_status run_steps(loop *lp, const step *steps, size_t n, bool *any) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < n; i++) {
        if (step_applies(lp, steps[i].when)) {
            st = call_sub(lp, steps[i].sub, &lp->args->opts, any);
        }
    }
    return st;
}

/* One cap WARN line for a module. */
static void warn_one(const loop *lp, const odin3_module *module, uint32_t iterations) {
    odin3_log(
        ODIN3_LOG_WARN, "opt: module '%s': no fixed point after %" PRIu32 " iterations; stopping",
        odin3_strtab_get(odin3_design_strtab(lp->design), odin3_module_name(module)), iterations);
}

/* WARN "opt: module 'm': no fixed point after K iterations; stopping", for each module that the
 * last iteration changed (the top module when the session noticed none, else the first). */
static void warn_cap(const loop *lp, uint32_t iterations) {
    uint32_t end = odin3_design_module_end(lp->design);
    uint32_t warned = 0;
    for (uint32_t id = 1; id < end; id++) {
        const odin3_module *module = odin3_module_get(lp->design, (odin3_module_id){id});
        if (module != NULL && odin3_opt_session_module_changed((odin3_module_id){id})) {
            warn_one(lp, module, iterations);
            warned++;
        }
    }
    if (warned == 0) {
        odin3_module_id top = odin3_design_top(lp->design);
        for (uint32_t id = top.v != 0 ? top.v : 1U; id < end && warned == 0; id++) {
            const odin3_module *module = odin3_module_get(lp->design, (odin3_module_id){id});
            if (module != NULL) {
                warn_one(lp, module, iterations);
                warned++;
            }
        }
    }
}

/* opt_expr X; opt_merge -nomux M; then do { body } while (changed). */
static odin3_status run_default(loop *lp, uint32_t cap, uint32_t *iterations, bool *capped) {
    bool ignored = false;
    odin3_opt_opts first_merge = lp->args->opts;
    first_merge.nomux = true;
    odin3_status st = call_sub(lp, ODIN3_OPT_SUB_EXPR, &lp->args->opts, &ignored);
    st = st == ODIN3_OK ? call_sub(lp, ODIN3_OPT_SUB_MERGE, &first_merge, &ignored) : st;
    bool changed = true;
    while (st == ODIN3_OK && changed) {
        changed = false;
        odin3_opt_session_new_iteration();
        st = run_steps(lp, k_default_loop, sizeof k_default_loop / sizeof k_default_loop[0],
                       &changed);
        (*iterations)++;
        if (st == ODIN3_OK && changed && *iterations >= cap) {
            *capped = true;
            break;
        }
    }
    return st;
}

/* do { opt_expr; opt_merge; [opt_dff]; if (!dff changed) break; opt_clean } ; opt_clean */
static odin3_status run_fast(loop *lp, uint32_t cap, uint32_t *iterations, bool *capped) {
    bool ignored = false;
    odin3_status st = ODIN3_OK;
    for (;;) {
        lp->dff_changed = false;
        odin3_opt_session_new_iteration();
        st = run_steps(lp, k_fast_loop, sizeof k_fast_loop / sizeof k_fast_loop[0], &ignored);
        (*iterations)++;
        if (st != ODIN3_OK || !lp->dff_changed) {
            break;
        }
        st = call_sub(lp, ODIN3_OPT_SUB_CLEAN, &lp->args->opts, &ignored);
        if (st != ODIN3_OK) {
            break;
        }
        if (*iterations >= cap) {
            *capped = true;
            break;
        }
    }
    return st == ODIN3_OK ? call_sub(lp, ODIN3_OPT_SUB_CLEAN, &lp->args->opts, &ignored) : st;
}

odin3_status odin3_opt_run(odin3_pass_ctx *ctx, odin3_design *design, const odin3_opt_args *args,
                           odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || args == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: no pass run, design, arguments or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_opt_session_begin(design, !args->full_rescan);
    if (st != ODIN3_OK) {
        return st;
    }
    loop lp = {ctx, design, args, counts, false};
    const uint32_t cap = odin3_opt_limits_now().max_iterations;
    uint32_t iterations = 0;
    bool capped = false;
    st = args->opts.fast ? run_fast(&lp, cap, &iterations, &capped)
                         : run_default(&lp, cap, &iterations, &capped);
    counts->iterations += iterations;
    if (st == ODIN3_OK && capped) {
        warn_cap(&lp, iterations);
    }
    odin3_opt_session_end();
    if (st != ODIN3_OK) {
        return st;
    }
    if (!capped) {
        odin3_log(ODIN3_LOG_INFO,
                  args->opts.fast ? "opt: -fast: done after %" PRIu32 " iterations"
                                  : "opt: fixed point after %" PRIu32 " iterations",
                  iterations);
    }
    return odin3_design_compact(ctx, design);
}
