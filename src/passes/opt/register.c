/*
 * register.c — the eight opt pass definitions (2F spec §3, §16, OP-12, OP-16; plan Task 4): option
 * parsing (point (c)), the sub-pass call wrapper (hook, budget, snapshots, Debug check) and
 * --opt-stats (point (j)).
 */
#include "backends/rtlil/writer.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/opt/expr.h"
#include "passes/opt/fold.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- the passes ------------------------------------------------------------------------------ */

typedef enum opt_pass {
    P_OPT,
    P_EXPR,
    P_MERGE,
    P_MUXTREE,
    P_REDUCE,
    P_SHARE,
    P_DFF,
    P_CLEAN,
    P_COUNT
} opt_pass;

static const char *const k_pass_names[P_COUNT] = {"opt",         "opt_expr",   "opt_merge",
                                                  "opt_muxtree", "opt_reduce", "opt_share",
                                                  "opt_dff",     "opt_clean"};

/* The pass name of each sub-pass (they are the same words). */
static const char *const k_sub_names[ODIN3_OPT_N_SUBS] = {
    "opt_expr", "opt_merge", "opt_muxtree", "opt_reduce", "opt_share", "opt_dff", "opt_clean"};

/* --- options (point (c)) --------------------------------------------------------------------- */

#define ONLY(p) (1U << (p))
#define ALL_PASSES ((1U << P_COUNT) - 1U)

typedef struct opt_word {
    const char *name; /* without the dashes */
    size_t offset;    /* of the bool in odin3_opt_args */
    uint32_t passes;  /* the passes accepting it */
    bool long_only;   /* only the `--` spelling */
} opt_word;

#define OPTS_AT(field) offsetof(odin3_opt_args, opts.field)

static const opt_word k_words[] = {
    {"mux_undef", OPTS_AT(mux_undef), ONLY(P_OPT) | ONLY(P_EXPR), false},
    {"mux_bool", OPTS_AT(mux_bool), ONLY(P_OPT) | ONLY(P_EXPR), false},
    {"undriven", OPTS_AT(undriven), ONLY(P_OPT) | ONLY(P_EXPR), false},
    {"noclkinv", OPTS_AT(noclkinv), ONLY(P_OPT) | ONLY(P_EXPR), false},
    {"fine", OPTS_AT(fine), ONLY(P_OPT) | ONLY(P_EXPR) | ONLY(P_REDUCE), false},
    {"full", OPTS_AT(full), ONLY(P_OPT) | ONLY(P_EXPR) | ONLY(P_REDUCE), false},
    {"keepdc", OPTS_AT(keepdc), ONLY(P_OPT) | ONLY(P_EXPR) | ONLY(P_MERGE) | ONLY(P_DFF), false},
    {"nodffe", OPTS_AT(nodffe), ONLY(P_OPT) | ONLY(P_DFF), false},
    {"nosdff", OPTS_AT(nosdff), ONLY(P_OPT) | ONLY(P_DFF), false},
    {"purge", OPTS_AT(purge), ONLY(P_OPT) | ONLY(P_CLEAN), false},
    {"nomux", OPTS_AT(nomux), ONLY(P_MERGE), false},
    {"fast", OPTS_AT(fast), ONLY(P_OPT), false},
    {"noff", OPTS_AT(noff), ONLY(P_OPT), false},
    {"opt-stats", offsetof(odin3_opt_args, stats), ALL_PASSES, true},
    {"opt-full-rescan", offsetof(odin3_opt_args, full_rescan), ONLY(P_OPT) | ONLY(P_EXPR), true},
    {"opt-simcheck", offsetof(odin3_opt_args, simcheck), ALL_PASSES, true},
};

static bool find_pass(const char *pass, opt_pass *out) {
    for (int idx = 0; idx < P_COUNT; idx++) {
        if (strcmp(k_pass_names[idx], pass) == 0) {
            *out = (opt_pass)idx;
            return true;
        }
    }
    return false;
}

/* The word of args (not NUL-terminated) as table word: the dashes it starts with are 1 or 2. */
static bool word_is(odin3_bytes arg, const opt_word *word) {
    const char *text = (const char *)arg.ptr;
    size_t dashes = 0;
    while (dashes < arg.len && dashes < 2 && text[dashes] == '-') {
        dashes++;
    }
    if (dashes == 0 || (word->long_only && dashes != 2)) {
        return false;
    }
    size_t len = strlen(word->name);
    return arg.len - dashes == len && memcmp(text + dashes, word->name, len) == 0;
}

static const opt_word *find_word(odin3_bytes arg, opt_pass pass) {
    for (size_t i = 0; i < sizeof k_words / sizeof k_words[0]; i++) {
        if ((k_words[i].passes & ONLY(pass)) != 0 && word_is(arg, &k_words[i])) {
            return &k_words[i];
        }
    }
    return NULL;
}

/* -full on opt_expr or opt, and on opt_reduce (Yosys's implications). */
static void apply_full(opt_pass pass, odin3_opt_opts *opts) {
    if (!opts->full) {
        return;
    }
    if (pass == P_OPT || pass == P_EXPR) {
        opts->mux_undef = opts->mux_bool = opts->undriven = opts->fine = true;
    }
    if (pass == P_REDUCE) {
        opts->fine = true;
    }
}

odin3_status odin3_opt_parse_args(const char *pass, odin3_bytes args, odin3_opt_args *out) {
    opt_pass which = P_OPT;
    if (pass == NULL || out == NULL || !find_pass(pass, &which)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: invalid arguments to odin3_opt_parse_args");
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        const opt_word *found = find_word(word, which);
        if (found == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "%s: unknown argument '%.*s'", pass, (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        *((bool *)((char *)out + found->offset)) = true;
    }
    apply_full(which, &out->opts);
    return ODIN3_OK;
}

/* --- snapshots ------------------------------------------------------------------------------- */

static odin3_strbuf g_snap_dir;
static bool g_snap_on;
static uint32_t g_snap_seq;

ODIN3_EXPORT odin3_status odin3_pass_set_opt_snapshot(const char *dir) {
    odin3_strbuf copy;
    odin3_strbuf_init(&copy);
    if (dir != NULL && odin3_strbuf_append(&copy, odin3_bytes_cstr(dir)) != ODIN3_OK) {
        odin3_strbuf_free(&copy);
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_strbuf_free(&g_snap_dir);
    g_snap_dir = copy;
    g_snap_on = dir != NULL;
    g_snap_seq = 0;
    return ODIN3_OK;
}

/* <dir>/<seq>-<sub>-<which>.il holding text. */
static odin3_status snapshot_write(uint32_t seq, odin3_opt_sub sub, const char *which,
                                   const odin3_strbuf *text) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_strbuf_appendf(&path, "%s/%04u-%s-%s.il", g_snap_dir.data,
                                           (unsigned)seq, k_sub_names[sub], which);
    if (st == ODIN3_OK) {
        st = odin3_pass_write_text(path.data, text);
    }
    odin3_strbuf_free(&path);
    return st;
}

/* --- the call wrapper ------------------------------------------------------------------------ */

typedef odin3_status (*sub_run_fn)(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* The sub-pass bodies (the odin3_opt_<sub>_run of each task). */
static const sub_run_fn k_sub_run[ODIN3_OPT_N_SUBS] = {
    [ODIN3_OPT_SUB_EXPR] = odin3_opt_expr_design,    [ODIN3_OPT_SUB_MERGE] = odin3_opt_merge_run,
    [ODIN3_OPT_SUB_MUXTREE] = odin3_opt_muxtree_run, [ODIN3_OPT_SUB_REDUCE] = odin3_opt_reduce_run,
    [ODIN3_OPT_SUB_SHARE] = odin3_opt_share_run,     [ODIN3_OPT_SUB_DFF] = odin3_opt_dff_run,
    [ODIN3_OPT_SUB_CLEAN] = odin3_opt_clean_run,
};

/* How much the sub-pass changed so far: the sum of its counters (a change is growth). */
static uint64_t change_measure(const odin3_opt_counts *counts, odin3_opt_sub sub) {
    switch (sub) {
    case ODIN3_OPT_SUB_EXPR:
        return (uint64_t)counts->expr.rewritten + counts->expr.folded;
    case ODIN3_OPT_SUB_MERGE:
        return counts->merge_removed;
    case ODIN3_OPT_SUB_MUXTREE:
        return counts->muxtree_ports;
    case ODIN3_OPT_SUB_REDUCE:
        return counts->reduce_changes;
    case ODIN3_OPT_SUB_SHARE:
        return counts->share_merged;
    case ODIN3_OPT_SUB_DFF:
        return (uint64_t)counts->dff_changed + counts->dff_const_bits;
    case ODIN3_OPT_SUB_CLEAN:
    case ODIN3_OPT_N_SUBS:
        break;
    }
    return counts->clean_changes;
}

/* An upper bound of one call's side arrays: they are a few words per net, node and pin, a fraction
 * of what the design itself reserves for them. */
static uint64_t call_extra_bytes(const odin3_design *design) {
    return odin3_design_bytes_reserved(design) / 2U;
}

static odin3_status budget_gate(const odin3_opt_call_req *req) {
    if (odin3_budget_check(req->ctx, req->design, call_extra_bytes(req->design)) == ODIN3_OK) {
        return ODIN3_OK;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    if (odin3_budget_message(req->ctx, &msg) == ODIN3_OK && msg.data != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return ODIN3_ERR_PARSE;
}

static odin3_status debug_check(odin3_design *design) {
#ifdef NDEBUG
    (void)design;
    return ODIN3_OK;
#else
    odin3_check_opts opts = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    return odin3_check_design(design, opts);
#endif
}

/* --opt-simcheck runs in Debug builds only (spec 20.2, point (m)). */
static bool simcheck_on(const odin3_opt_call_req *req) {
#ifdef NDEBUG
    (void)req;
    return false;
#else
    return req->simcheck;
#endif
}

/* The "after" files, and the "before" text that was kept for them. */
static odin3_status snapshot_after(const odin3_opt_call_req *req, uint32_t seq,
                                   const odin3_strbuf *before) {
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    odin3_status st = snapshot_write(seq, req->sub, "before", before);
    if (st == ODIN3_OK) {
        st = odin3_rtlil_text(req->design, &after);
    }
    if (st == ODIN3_OK) {
        st = snapshot_write(seq, req->sub, "after", &after);
    }
    odin3_strbuf_free(&after);
    return st;
}

/* A snapshot that cannot be written never changes the call's result (R2): WARN, then go on. */
static bool snapshot_ok(odin3_status st) {
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_WARN, "opt: snapshot skipped: %s", odin3_status_string(st));
    }
    return st == ODIN3_OK;
}

static bool request_valid(const odin3_opt_call_req *req, const bool *changed) {
    bool ok = req != NULL && changed != NULL && req->ctx != NULL && req->design != NULL &&
              req->opts != NULL && req->counts != NULL && (unsigned)req->sub < ODIN3_OPT_N_SUBS;
    if (!ok) {
        odin3_log(ODIN3_LOG_ERROR, "opt: invalid arguments to odin3_opt_call");
    }
    return ok;
}

/* The sub-pass body, the after_run test hook, the Debug check and the --opt-simcheck comparison
 * (sim is NULL without the option). */
static odin3_status run_checked(const odin3_opt_call_req *req, const odin3_opt_test_hooks *hooks,
                                const odin3_opt_simcheck *sim) {
    const char *name = k_sub_names[req->sub];
    odin3_status st = k_sub_run[req->sub](req->ctx, req->design, req->opts, req->counts);
    if (st == ODIN3_OK && hooks->after_run != NULL) {
        hooks->after_run(hooks->user, name, req->design);
    }
    if (st == ODIN3_OK) {
        st = debug_check(req->design);
    }
    if (st == ODIN3_OK && sim != NULL) {
        st = odin3_opt_simcheck_after(req->design, sim, name);
    }
    return st;
}

odin3_status odin3_opt_call(const odin3_opt_call_req *req, bool *changed) {
    if (!request_valid(req, changed)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *changed = false;
    const odin3_opt_test_hooks *hooks = odin3_opt_test_hooks_now();
    if (hooks->on_call != NULL) {
        hooks->on_call(hooks->user, k_sub_names[req->sub]);
    }
    odin3_status st = budget_gate(req);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    const uint32_t seq = g_snap_on ? ++g_snap_seq : 0;
    bool snap = st == ODIN3_OK && g_snap_on;
    if (snap) {
        snap = snapshot_ok(odin3_rtlil_text(req->design, &before));
    }
    odin3_opt_simcheck *sim = NULL;
    if (st == ODIN3_OK && simcheck_on(req)) {
        st = odin3_opt_simcheck_before(req->design, &sim);
    }
    const uint64_t measure = change_measure(req->counts, req->sub);
    if (st == ODIN3_OK) {
        st = run_checked(req, hooks, sim);
    }
    odin3_opt_simcheck_free(sim);
    *changed = st == ODIN3_OK && change_measure(req->counts, req->sub) > measure;
    if (*changed && snap) {
        (void)snapshot_ok(snapshot_after(req, seq, &before));
    }
    odin3_strbuf_free(&before);
    return st;
}

/* --- --opt-stats (point (j)) ----------------------------------------------------------------- */

/*
 * The counter names: odin3_opt_rule's cover names (odin3_opt_rule_name), the folds' "const.$<type>"
 * (odin3_opt_fold_name) and odin3_opt_dff_action's (printed as "opt_dff.<name>"). A NULL name has
 * no output.
 */
static const char *const k_dff_names[ODIN3_OPT_DFF_MAX_ACTIONS] = {
    [ODIN3_OPT_DFF_NEVER_CLR] = "never_active_clr",
    [ODIN3_OPT_DFF_NEVER_SET] = "never_active_set",
    [ODIN3_OPT_DFF_ALWAYS_CLR] = "always_active_clr",
    [ODIN3_OPT_DFF_ALWAYS_SET] = "always_active_set",
    [ODIN3_OPT_DFF_CLR_SET_TO_ARST] = "clr_set_to_arst",
    [ODIN3_OPT_DFF_NEVER_ALOAD] = "never_active_aload",
    [ODIN3_OPT_DFF_ALWAYS_ALOAD] = "always_active_aload",
    [ODIN3_OPT_DFF_ALOAD_TO_ARST] = "aload_to_arst",
    [ODIN3_OPT_DFF_NEVER_ARST] = "never_active_arst",
    [ODIN3_OPT_DFF_ALWAYS_ARST] = "always_active_arst",
    [ODIN3_OPT_DFF_NEVER_SRST] = "never_active_srst",
    [ODIN3_OPT_DFF_ALWAYS_SRST] = "always_active_srst",
    [ODIN3_OPT_DFF_NEVER_EN] = "never_active_en",
    [ODIN3_OPT_DFF_ALWAYS_EN] = "always_active_en",
    [ODIN3_OPT_DFF_CONST_CLK] = "const_clk",
    [ODIN3_OPT_DFF_D_EQ_Q] = "d_eq_q",
    [ODIN3_OPT_DFF_AD_EQ_Q] = "ad_eq_q",
    [ODIN3_OPT_DFF_ADD_SRST] = "add_srst",
    [ODIN3_OPT_DFF_ADD_EN] = "add_en",
    [ODIN3_OPT_DFF_CONST_BIT_0] = "const_bit_0",
    [ODIN3_OPT_DFF_CONST_BIT_1] = "const_bit_1",
    [ODIN3_OPT_DFF_CONST_BIT_X] = "const_bit_x",
    [ODIN3_OPT_DFF_CONST_BIT_Z] = "const_bit_z",
};

enum { STAT_NAME_LEN = 96 };

typedef struct stat_line {
    char name[STAT_NAME_LEN];
    uint32_t count;
} stat_line;

enum {
    STAT_LINES_MAX = ODIN3_OPT_EXPR_MAX_RULES + ODIN3_OPT_FOLD_TYPES + ODIN3_OPT_DFF_MAX_ACTIONS
};

static uint32_t collect_stats(const odin3_opt_counts *counts, stat_line *lines) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < ODIN3_OPT_EXPR_MAX_RULES; i++) {
        const char *name = odin3_opt_rule_name((odin3_opt_rule)i);
        if (counts->expr.rule[i] != 0 && name != NULL) {
            (void)snprintf(lines[total].name, sizeof lines[total].name, "%s", name);
            lines[total++].count = counts->expr.rule[i];
        }
    }
    for (uint32_t i = 0; i < ODIN3_OPT_FOLD_TYPES; i++) {
        if (counts->fold_type[i] != 0 && odin3_opt_fold_name(i) != NULL) {
            (void)snprintf(lines[total].name, sizeof lines[total].name, "%s",
                           odin3_opt_fold_name(i));
            lines[total++].count = counts->fold_type[i];
        }
    }
    for (uint32_t i = 0; i < ODIN3_OPT_DFF_MAX_ACTIONS; i++) {
        if (counts->dff_action[i] != 0 && k_dff_names[i] != NULL) {
            (void)snprintf(lines[total].name, sizeof lines[total].name, "opt_dff.%s",
                           k_dff_names[i]);
            lines[total++].count = counts->dff_action[i];
        }
    }
    return total;
}

void odin3_opt_print_stats(const odin3_opt_counts *counts) {
    if (counts == NULL) {
        return;
    }
    stat_line lines[STAT_LINES_MAX];
    uint32_t total = collect_stats(counts, lines);
    for (uint32_t i = 1; i < total; i++) { /* insertion sort by name */
        stat_line cur = lines[i];
        uint32_t j = i;
        for (; j > 0 && strcmp(lines[j - 1].name, cur.name) > 0; j--) {
            lines[j] = lines[j - 1];
        }
        lines[j] = cur;
    }
    for (uint32_t i = 0; i < total; i++) {
        odin3_log(ODIN3_LOG_INFO, "opt: rule %s %u", lines[i].name, (unsigned)lines[i].count);
    }
}

/* --- the pass bodies ------------------------------------------------------------------------- */

typedef struct pass_desc {
    opt_pass pass;
    odin3_opt_sub sub; /* ODIN3_OPT_N_SUBS for opt itself (odin3_opt_run) */
} pass_desc;

static const pass_desc g_desc[P_COUNT] = {
    {P_OPT, ODIN3_OPT_N_SUBS},        {P_EXPR, ODIN3_OPT_SUB_EXPR},
    {P_MERGE, ODIN3_OPT_SUB_MERGE},   {P_MUXTREE, ODIN3_OPT_SUB_MUXTREE},
    {P_REDUCE, ODIN3_OPT_SUB_REDUCE}, {P_SHARE, ODIN3_OPT_SUB_SHARE},
    {P_DFF, ODIN3_OPT_SUB_DFF},       {P_CLEAN, ODIN3_OPT_SUB_CLEAN},
};

/* A standalone sub-pass: parse, zero the counts, one call, --opt-stats. */
static odin3_status opt_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                 void *user) {
    const pass_desc *desc = user;
    const char *name = k_pass_names[desc->pass];
    odin3_opt_args parsed;
    odin3_status st = odin3_opt_parse_args(name, args, &parsed);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    if (desc->sub == ODIN3_OPT_N_SUBS) { /* opt itself */
        st = odin3_opt_run(ctx, design, &parsed, &counts);
    } else {
        bool changed = false;
        odin3_opt_call_req req = {ctx, design, desc->sub, &parsed.opts, &counts, parsed.simcheck};
        st = odin3_opt_call(&req, &changed);
    }
    if (st == ODIN3_OK && parsed.stats) {
        odin3_opt_print_stats(&counts);
    }
    return st;
}

#define OPT_DEF(sym, idx, help_text)                                                               \
    const odin3_pass_def sym = {k_pass_names[idx],    help_text, opt_pass_run,                     \
                                (void *)&g_desc[idx], false,     ODIN3_PASS_PROCESSES_OK}

OPT_DEF(odin3_pass_opt, P_OPT,
        "opt [-fast] [-full] [-keepdc] [-mux_undef] [-mux_bool] [-undriven] [-noclkinv] [-fine] "
        "[-nodffe] [-nosdff] [-noff] [-purge] [--opt-full-rescan] [--opt-stats]: optimise the "
        "design to a fixed point (opt_expr, "
        "opt_merge, opt_muxtree, opt_reduce, opt_share, opt_dff, opt_clean)");
OPT_DEF(odin3_pass_opt_expr, P_EXPR,
        "opt_expr [-mux_undef] [-mux_bool] [-undriven] [-noclkinv] [-fine] [-full] [-keepdc] "
        "[--opt-full-rescan] [--opt-stats]: constant folding and simple rewrites of cells");
OPT_DEF(
    odin3_pass_opt_merge, P_MERGE,
    "opt_merge [-nomux] [-keepdc] [--opt-stats]: merge identical cells (common subexpressions)");
OPT_DEF(odin3_pass_opt_muxtree, P_MUXTREE,
        "opt_muxtree [--opt-stats]: remove dead ports of multiplexer trees");
OPT_DEF(odin3_pass_opt_reduce, P_REDUCE,
        "opt_reduce [-fine] [-full] [--opt-stats]: simplify large OR/AND/MUX cells with duplicate "
        "inputs");
OPT_DEF(odin3_pass_opt_share, P_SHARE,
        "opt_share [--opt-stats]: merge cells that share operands under a mux");
OPT_DEF(odin3_pass_opt_dff, P_DFF,
        "opt_dff [-nodffe] [-nosdff] [-keepdc] [--opt-stats]: simplify flip-flops (constant "
        "controls, enables, "
        "synchronous resets)");
OPT_DEF(odin3_pass_opt_clean, P_CLEAN,
        "opt_clean [-purge] [--opt-stats]: remove unused cells and wires");
