/*
 * proc_pass.c — the proc pass (2E spec §2, PR-2 to PR-4): its definition and options, the
 * per-module schedule with its atomicity (PR-3), the statistics and info lines (§2.3), the caps
 * (§16) and the test hooks (proc_test.h, plan point (h)).
 */
#include "passes/proc/proc.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum { MS_PER_S = 1000, NS_PER_MS = 1000000 };

/* --- caps and test hooks (proc_test.h) ----------------------------------------------------- */

static const odin3_proc_limits k_default_limits = {
    ODIN3_PROC_MAX_ERRORS, ODIN3_PROC_MAX_PATTERN_BITS, ODIN3_PROC_MAX_ARST_ROUNDS,
    ODIN3_PROC_MAX_SIGNAL_WALK, ODIN3_PROC_BUDGET_PINS};
static odin3_proc_limits g_limits = {ODIN3_PROC_MAX_ERRORS, ODIN3_PROC_MAX_PATTERN_BITS,
                                     ODIN3_PROC_MAX_ARST_ROUNDS, ODIN3_PROC_MAX_SIGNAL_WALK,
                                     ODIN3_PROC_BUDGET_PINS};
static odin3_proc_test_hooks g_hooks;

static uint32_t clamp_cap(uint32_t value, uint32_t dflt) {
    if (value < 1) {
        return 1;
    }
    return value > dflt ? dflt : value;
}

void odin3_proc_test_set_limits(const odin3_proc_limits *limits) {
    if (limits == NULL) {
        g_limits = k_default_limits;
        return;
    }
    g_limits.max_errors = clamp_cap(limits->max_errors, k_default_limits.max_errors);
    g_limits.max_pattern_bits =
        clamp_cap(limits->max_pattern_bits, k_default_limits.max_pattern_bits);
    g_limits.max_arst_rounds = clamp_cap(limits->max_arst_rounds, k_default_limits.max_arst_rounds);
    g_limits.max_signal_walk = clamp_cap(limits->max_signal_walk, k_default_limits.max_signal_walk);
    g_limits.budget_pins = clamp_cap(limits->budget_pins, k_default_limits.budget_pins);
}

odin3_proc_limits odin3_proc_limits_now(void) {
    return g_limits;
}

void odin3_proc_test_set_hooks(const odin3_proc_test_hooks *hooks) {
    static const odin3_proc_test_hooks k_none = {NULL, NULL, NULL};
    g_hooks = hooks != NULL ? *hooks : k_none;
}

/* --- one run -------------------------------------------------------------------------------- */

typedef struct proc_run {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_proc_opts opts;
    odin3_proc_stats *stats;
    uint32_t reported; /* pending phases reported (bit per phase) */
    bool failed;       /* a module failed: the rest of the run only analyses */
    bool stopped;      /* E6 or E7: the run stops */
    uint32_t kept;     /* --stop-after: processes re-encoded */
    double start;      /* ms, for the info line */
} proc_run;

static double now_ms(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * MS_PER_S + (double)ts.tv_nsec / NS_PER_MS;
}

/* --- phases (point (h)) -------------------------------------------------------------------- */

/* The phase itself. Every phase is still pending (proc_pending.c); Tasks 4-9 dispatch theirs here
 * as their pending entries go. */
static odin3_status proc_real_phase(odin3_proc_mctx *mctx, odin3_proc_phase phase, odin3_pw *pw) {
    (void)pw;
    return odin3_proc_pending_run(mctx, phase);
}

/* One phase call: the test hook when it handles the call, else the real phase. */
static odin3_status proc_phase(odin3_proc_mctx *mctx, odin3_proc_phase phase, odin3_pw *pw) {
    if (g_hooks.phase != NULL) {
        odin3_status st = ODIN3_OK;
        odin3_proc_phase_call call = {phase, mctx, pw};
        if (g_hooks.phase(g_hooks.user, &call, &st)) {
            return st;
        }
    }
    return proc_real_phase(mctx, phase, pw);
}

/* --- step A: analysis (nothing in the IR changes) ------------------------------------------ */

static uint64_t vec_bytes(const odin3_vec *vec) {
    return (uint64_t)vec->cap * vec->elem_size;
}

/* The bytes a working form holds (the budget's extra, §16). */
static uint64_t pw_bytes(const odin3_pw *pw) {
    uint64_t bytes = sizeof *pw + vec_bytes(&pw->bits) + vec_bytes(&pw->cases) +
                     vec_bytes(&pw->switches) + vec_bytes(&pw->syncs) + vec_bytes(&pw->promoted) +
                     vec_bytes(&pw->in_nets) + vec_bytes(&pw->attrs);
    for (size_t i = 0; i < pw->cases.len; i++) {
        const odin3_pw_case *cs = odin3_vec_cat(&pw->cases, i);
        bytes += vec_bytes(&cs->compares) + vec_bytes(&cs->actions) + vec_bytes(&cs->switches);
    }
    for (size_t i = 0; i < pw->switches.len; i++) {
        bytes += vec_bytes(&((const odin3_pw_switch *)odin3_vec_cat(&pw->switches, i))->cases);
    }
    for (size_t i = 0; i < pw->syncs.len; i++) {
        bytes += vec_bytes(&((const odin3_pw_sync *)odin3_vec_cat(&pw->syncs, i))->actions);
    }
    return bytes;
}

/* The module's live $proc nodes in ID order. */
static odin3_status collect_procs(const odin3_proc_mctx *mctx, odin3_vec *nodes) {
    const odin3_module *module = mctx->module;
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(mctx->design, odin3_node_type(module, node));
        if (def != NULL && (def->flags & ODIN3_CT_PROCESS) != 0) {
            odin3_node_id *slot = odin3_vec_push(nodes);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = node;
        }
    }
    return ODIN3_OK;
}

/*
 * One process: the budget, decode, then the analysis phases up to last. ODIN3_ERR_PARSE when the
 * process has an error (a phase's, a pending phase's, E6 or E7), else ODIN3_OK or a failure.
 */
static odin3_status analyse_one(odin3_proc_mctx *mctx, odin3_node_id node, odin3_proc_phase last) {
    odin3_status st = odin3_proc_budget(mctx, odin3_node_prov(mctx->module, node));
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pw *pw = odin3_vec_push(&mctx->pws); /* reserved: earlier forms do not move */
    st = odin3_pw_decode(mctx, node, pw);
    if (st != ODIN3_OK) {
        odin3_vec_pop(&mctx->pws); /* decode left it empty */
        return st;
    }
    mctx->pw_bytes += pw_bytes(pw);
    for (uint32_t phase = ODIN3_PROC_PH_CLEAN; phase <= (uint32_t)last; phase++) {
        uint32_t before = mctx->errors;
        st = proc_phase(mctx, (odin3_proc_phase)phase, pw);
        if (st == ODIN3_ERR_PARSE || (st == ODIN3_OK && (mctx->errors > before || mctx->stopped))) {
            return ODIN3_ERR_PARSE;
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Step A for every process of the module; *failed when one of them has an error. */
static odin3_status proc_analyse(odin3_proc_mctx *mctx, odin3_proc_phase last, bool *failed) {
    odin3_vec nodes;
    odin3_vec_init(&nodes, sizeof(odin3_node_id));
    odin3_status st = collect_procs(mctx, &nodes);
    if (st == ODIN3_OK && odin3_vec_reserve(&mctx->pws, nodes.len) != ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; st == ODIN3_OK && !mctx->stopped && i < nodes.len; i++) {
        st = analyse_one(mctx, *(const odin3_node_id *)odin3_vec_cat(&nodes, i), last);
        if (st == ODIN3_ERR_PARSE) {
            *failed = true;
            st = ODIN3_OK;
        }
    }
    odin3_vec_free(&nodes);
    return st;
}

/* --- steps B-F ----------------------------------------------------------------------------- */

/* Records prune's root promotions of pw as connections, bit by bit (§5.3, §7). */
static odin3_status connect_promoted(odin3_proc_mctx *mctx, const odin3_pw *pw) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < pw->promoted.len; i++) {
        const odin3_pw_action *act = odin3_vec_cat(&pw->promoted, i);
        uint32_t len = act->lhs.len < act->rhs.len ? act->lhs.len : act->rhs.len;
        for (uint32_t k = 0; st == ODIN3_OK && k < len; k++) {
            st = odin3_proc_connect(mctx, odin3_pw_sig_bits(pw, act->lhs)[k],
                                    odin3_pw_sig_bits(pw, act->rhs)[k]);
        }
    }
    return st;
}

/* Step B for one process: its operation, the node's deletion, its mux trees and M1. */
static odin3_status lower_mux(odin3_proc_mctx *mctx, odin3_pw *pw) {
    odin3_status st = odin3_proc_budget(mctx, pw->prov);
    st = st == ODIN3_OK ? odin3_proc_begin_lowering(mctx, pw) : st;
    st = st == ODIN3_OK ? odin3_node_delete(mctx->module, pw->node) : st;
    if (st == ODIN3_OK) {
        pw->node = (odin3_node_id){0}; /* its O nets are undriven until the mux trees drive them */
    }
    st = st == ODIN3_OK ? connect_promoted(mctx, pw) : st;
    st = st == ODIN3_OK ? proc_phase(mctx, ODIN3_PROC_PH_MUX, pw) : st;
    return st == ODIN3_OK ? odin3_proc_apply_merges(mctx) : st;
}

/* Steps B-E (§2.2): mux per process, the module's latches (ending with M2), flip-flops per
 * process, orphan nets. Any failure stops at once (NO_MEMORY, E6), leaving valid IR. */
static odin3_status proc_lower(odin3_proc_mctx *mctx) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        st = lower_mux(mctx, odin3_vec_at(&mctx->pws, i));
    }
    st = st == ODIN3_OK ? proc_phase(mctx, ODIN3_PROC_PH_LATCH, NULL) : st;
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        odin3_pw *pw = odin3_vec_at(&mctx->pws, i);
        st = odin3_proc_budget(mctx, pw->prov);
        st = st == ODIN3_OK ? proc_phase(mctx, ODIN3_PROC_PH_FF, pw) : st;
    }
    return st == ODIN3_OK ? odin3_proc_orphans(mctx) : st;
}

/*
 * Step F: opt_expr -keepdc on the lowered module (§2.2; 2F §4.8), skipped under --noopt and
 * --stop-after. Until the pre-merge step of point (f) adds the call to odin3_opt_expr_run (2F-2),
 * only the test hook runs, stats->opt_expr stays zero and the info line has no opt_expr part.
 */
static void proc_step_f(const proc_run *run, const odin3_proc_mctx *mctx) {
    if (run->opts.noopt || run->opts.stop_after != ODIN3_PROC_STOP_NONE) {
        return;
    }
    if (g_hooks.on_step_f != NULL) {
        g_hooks.on_step_f(g_hooks.user, odin3_module_id_of(mctx->module));
    }
}

/* --stop-after: every process re-encoded as a new $proc node (taking pw->prov, point (i)), then
 * prune's promotions applied as merges. */
static odin3_status proc_reencode(odin3_proc_mctx *mctx) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        st = odin3_pw_encode(mctx, odin3_vec_at(&mctx->pws, i));
    }
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        st = connect_promoted(mctx, odin3_vec_cat(&mctx->pws, i));
    }
    return st == ODIN3_OK ? odin3_proc_apply_merges(mctx) : st;
}

/* The end of step A (point (h)): a pending lowering phase fails the module before any change. */
static bool lowering_pending(odin3_proc_mctx *mctx) {
    odin3_proc_phase first = odin3_proc_pending_first();
    if (first < ODIN3_PROC_PH_MUX || first >= ODIN3_PROC_N_PHASES) {
        return false;
    }
    (void)odin3_proc_pending_run(mctx, first);
    return true;
}

/* Steps B-F, or the re-encoding of --stop-after, on a module whose analysis succeeded. */
static odin3_status proc_change(proc_run *run, odin3_proc_mctx *mctx) {
    if (run->opts.stop_after != ODIN3_PROC_STOP_NONE) {
        odin3_status st = proc_reencode(mctx);
        run->kept += st == ODIN3_OK ? (uint32_t)mctx->pws.len : 0;
        return st;
    }
    odin3_status st = proc_lower(mctx);
    if (st == ODIN3_OK) {
        run->stats->processes_lowered += (uint32_t)mctx->pws.len;
        run->stats->modules_lowered++;
        proc_step_f(run, mctx);
    }
    return st;
}

/* --stop-after X runs the analysis phases up to X: stop values are phase values plus one. */
_Static_assert(ODIN3_PROC_STOP_CLEAN - 1 == ODIN3_PROC_PH_CLEAN &&
                   ODIN3_PROC_STOP_RMDEAD - 1 == ODIN3_PROC_PH_RMDEAD &&
                   ODIN3_PROC_STOP_PRUNE - 1 == ODIN3_PROC_PH_PRUNE &&
                   ODIN3_PROC_STOP_ARST - 1 == ODIN3_PROC_PH_ARST,
               "odin3_proc_stop and odin3_proc_phase are aligned");

/* One module (PR-3): step A; then, unless it or an earlier module failed, steps B-F. */
static odin3_status proc_module(proc_run *run, odin3_module *module) {
    odin3_proc_mctx mctx;
    odin3_status st = odin3_proc_mctx_init(&mctx, run->ctx, module);
    if (st != ODIN3_OK) {
        return st;
    }
    mctx.opts = &run->opts;
    mctx.stats = run->stats;
    mctx.reported = &run->reported;
    bool failed = false;
    odin3_proc_phase last = run->opts.stop_after == ODIN3_PROC_STOP_NONE
                                ? ODIN3_PROC_PH_CLOCKS
                                : (odin3_proc_phase)(run->opts.stop_after - 1);
    st = proc_analyse(&mctx, last, &failed);
    if (st == ODIN3_OK && !failed && !run->failed && run->opts.stop_after == ODIN3_PROC_STOP_NONE) {
        failed = lowering_pending(&mctx);
    }
    if (st == ODIN3_OK && !failed && !run->failed) {
        st = proc_change(run, &mctx);
        failed = st == ODIN3_ERR_PARSE; /* E6 while lowering */
        st = failed ? ODIN3_OK : st;
    }
    if (st == ODIN3_OK && (failed || run->failed)) {
        run->failed = true; /* this module keeps its processes, or was only analysed */
        run->stats->modules_failed++;
    }
    run->stopped = run->stopped || mctx.stopped;
    odin3_proc_mctx_free(&mctx);
    return st;
}

/* --- the run ------------------------------------------------------------------------------- */

static const char *const k_stop_names[] = {"", "clean", "rmdead", "prune", "arst"};

/* The info line of §2.3 for a run that ended with status st. */
static void log_summary(const proc_run *run, odin3_status st) {
    const odin3_proc_stats *stats = run->stats;
    uint32_t stop = (uint32_t)run->opts.stop_after;
    if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_INFO,
                  "proc: %" PRIu32 " processes in %" PRIu32 " modules lowered; %" PRIu32
                  " modules keep their processes (%" PRIu32 " errors)",
                  stats->processes_lowered, stats->modules_lowered, stats->modules_failed,
                  stats->errors);
    } else if (st == ODIN3_OK && stop != ODIN3_PROC_STOP_NONE) {
        odin3_log(ODIN3_LOG_INFO, "proc: stopped after %s; %" PRIu32 " processes kept",
                  stop <= ODIN3_PROC_STOP_ARST ? k_stop_names[stop] : "?", run->kept);
    } else if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "proc: %" PRIu32 " processes in %" PRIu32 " modules: %" PRIu32 " $dff, %" PRIu32
                  " $adff, %" PRIu32 " $aldff, %" PRIu32 " $dffsr, %" PRIu32 " $dlatch, %" PRIu32
                  " $mux, %" PRIu32 " $pmux; %.1f ms",
                  stats->processes_lowered, stats->modules_lowered, stats->cells[ODIN3_PROC_K_DFF],
                  stats->cells[ODIN3_PROC_K_ADFF], stats->cells[ODIN3_PROC_K_ALDFF],
                  stats->cells[ODIN3_PROC_K_DFFSR], stats->cells[ODIN3_PROC_K_DLATCH],
                  stats->cells[ODIN3_PROC_K_MUX], stats->cells[ODIN3_PROC_K_PMUX],
                  now_ms() - run->start);
    }
}

/* Visits every module with processes in creation order until one fails or the run stops; after a
 * stop the unvisited modules with processes count as failed too. */
static odin3_status proc_modules(proc_run *run) {
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_design_module_end(run->design);
    for (uint32_t i = 1; st == ODIN3_OK && i < end; i++) {
        odin3_module *module = odin3_module_get(run->design, (odin3_module_id){i});
        if (module == NULL || odin3_module_proc_count(module) == 0) {
            continue;
        }
        if (run->stopped) {
            run->stats->modules_failed++;
            continue;
        }
        st = proc_module(run, module);
    }
    return st;
}

odin3_status odin3_proc_run(odin3_pass_ctx *ctx, odin3_design *design, const odin3_proc_opts *opts,
                            odin3_proc_stats *stats) {
    if (ctx == NULL || design == NULL ||
        (opts != NULL && (uint32_t)opts->stop_after > ODIN3_PROC_STOP_ARST)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: no pass run, no design or a bad --stop-after");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_proc_stats local;
    proc_run run;
    memset(&run, 0, sizeof run);
    run.start = now_ms();
    run.ctx = ctx;
    run.design = design;
    run.stats = stats != NULL ? stats : &local;
    if (opts != NULL) {
        run.opts = *opts;
    }
    memset(run.stats, 0, sizeof *run.stats);
    if (!odin3_design_has_processes(design)) {
        odin3_log(ODIN3_LOG_INFO, "proc: no processes");
        return ODIN3_OK;
    }
    odin3_status st = proc_modules(&run);
    if (st == ODIN3_OK && run.failed) {
        st = ODIN3_ERR_PARSE;
    }
    log_summary(&run, st);
    return st;
}

/* --- the pass ------------------------------------------------------------------------------ */

/* The phase --stop-after names, NONE for anything else. */
static odin3_proc_stop stop_named(odin3_bytes word) {
    for (uint32_t i = ODIN3_PROC_STOP_CLEAN; i <= ODIN3_PROC_STOP_ARST; i++) {
        if (word.ptr != NULL && odin3_pass_arg_is(word, k_stop_names[i])) {
            return (odin3_proc_stop)i;
        }
    }
    return ODIN3_PROC_STOP_NONE;
}

static odin3_status parse_opts(odin3_bytes args, odin3_proc_opts *opts) {
    odin3_bytes rest = args;
    for (odin3_bytes word = odin3_pass_arg_next(&rest); word.ptr != NULL;
         word = odin3_pass_arg_next(&rest)) {
        if (odin3_pass_arg_is(word, "--noopt")) {
            opts->noopt = true;
        } else if (odin3_pass_arg_is(word, "--stop-after")) {
            opts->stop_after = stop_named(odin3_pass_arg_next(&rest));
            if (opts->stop_after == ODIN3_PROC_STOP_NONE) {
                odin3_log(ODIN3_LOG_ERROR,
                          "proc: --stop-after needs one of: clean, rmdead, prune, arst");
                return ODIN3_ERR_INVALID_ARG;
            }
        } else {
            odin3_log(ODIN3_LOG_ERROR,
                      "proc: unknown option '%.*s' (options: --noopt, --stop-after <phase>)",
                      (int)word.len, (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

static odin3_status proc_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)user;
    odin3_proc_opts opts = {false, ODIN3_PROC_STOP_NONE};
    odin3_status st = parse_opts(args, &opts);
    return st == ODIN3_OK ? odin3_proc_run(ctx, design, &opts, NULL) : st;
}

const odin3_pass_def odin3_pass_proc = {
    "proc",        "proc [--noopt] [--stop-after clean|rmdead|prune|arst]",
    proc_pass_run, NULL,
    false,         ODIN3_PASS_PROCESSES_OK};
