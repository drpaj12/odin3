/*
 * test_proc_pass.c — the proc pass (2E spec §2, §12, §15, §16; plan Tasks 3 and 9): options,
 * the per-module schedule and its atomicity (Review Focus 4), statistics and info lines, step F,
 * --stop-after's re-encoding, the byte budget (E6), the error cap (E7), orphan nets, determinism
 * and out of memory, first with fake phases, then with the real ones.
 *
 * The fakes are installed through proc_test.h: every analysis phase succeeds, except CLEAN on a
 * process whose node carries the attribute fake_error, which reports `fake error` at the process;
 * mux, latch and ff do nothing (step B itself deletes the node). The real-phase tests (Task 9,
 * the section "real phases") install no hook.
 *
 * Yosys re-derivation: none for the fake-phase tests. The real-phase tests reuse bodies whose
 * Yosys 0.55 runs are recorded in test_proc_ff.c (ff_dff, ff_adff), test_proc_arst.c
 * (clocks_e1a, clocks_e1b), test_proc_mux.c (mux_dead_eq) and test_proc_latch.c
 * (latch_verified_example); the whole-pass result is the sum of those, with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto dff <test>.il
 * as the command for any of them (E1 is Yosys's `Multiple edge sensitive events found for this
 * signal!`, §20 #11; E6 has no Yosys counterpart).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/budget.h"
#include "passes/manager.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_DOCS_DIR
#error "ODIN3_DOCS_DIR must name the docs directory"
#endif

enum {
    LOG_LINES = 64,
    LOG_TEXT = 512,
    TEXT_MAX = 8192,
    MODULES_MAX = 8,
    BODIES_MAX = 8,
    OOM_LIMIT = 4000,
    REAL_OOM_LIMIT = 40000,
    CAP_PROCS = 25,
    DOC_MAX = 1 << 20,
};

/* --- log capture --------------------------------------------------------------------------- */

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        levels[n_lines] = level;
        (void)snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
    }
    n_lines++;
}

static void clear_lines(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
}

/* The error lines, as the default sink prints them ("odin3: error: <msg>"), in order. */
static size_t errors_seen(char out[][LOG_TEXT], size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (levels[i] != ODIN3_LOG_ERROR) {
            continue;
        }
        if (count < max) {
            int len = snprintf(out[count], LOG_TEXT, "odin3: error: %s", lines[i]);
            TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
        }
        count++;
    }
    return count;
}

/* The last info line starting with "proc: ". */
static const char *proc_info(void) {
    const char *found = NULL;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (levels[i] == ODIN3_LOG_INFO && strncmp(lines[i], "proc: ", strlen("proc: ")) == 0) {
            found = lines[i];
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(found, "no proc info line");
    return found;
}

/* --- fake phases ----------------------------------------------------------------------------- */

typedef struct fake_state {
    uint32_t calls[MODULES_MAX][ODIN3_PROC_N_PHASES]; /* by module ID and phase */
    uint32_t step_f[MODULES_MAX];                     /* on_step_f calls by module ID */
    bool promote;                                     /* PRUNE promotes pure root actions */
    char trace[TEXT_MAX]; /* on_step: "A+ A- B+ ...", a budget check as "b" and "e" */
    int depth;            /* on_step: open steps */
} fake_state;

static fake_state fakes;

/* True when the working form's node carried attribute name. */
static bool pw_has_attr(const odin3_proc_mctx *mctx, const odin3_pw *pw, const char *name) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(mctx->design), odin3_bytes_cstr(name), &key)) {
        return false;
    }
    for (size_t i = 0; i < pw->attrs.len; i++) {
        if (((const odin3_pw_attr *)odin3_vec_cat(&pw->attrs, i))->key == key) {
            return true;
        }
    }
    return false;
}

/* prune's root promotion, faked: every root action moves to pw->promoted (§5.3). */
static odin3_status fake_promote(odin3_pw *pw) {
    odin3_pw_case *root = odin3_vec_at(&pw->cases, 0);
    while (root->actions.len > 0) {
        odin3_pw_action *slot = odin3_vec_push(&pw->promoted);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *(const odin3_pw_action *)odin3_vec_cat(&root->actions, 0);
        memmove(root->actions.data, (const char *)root->actions.data + sizeof *slot,
                (root->actions.len - 1) * sizeof *slot);
        odin3_vec_pop(&root->actions);
    }
    return ODIN3_OK;
}

static bool fake_phase(void *user, const odin3_proc_phase_call *call, odin3_status *st) {
    fake_state *state = user;
    uint32_t id = odin3_module_id_of(call->m->module).v;
    TEST_ASSERT_TRUE(id < MODULES_MAX);
    state->calls[id][call->phase]++;
    *st = ODIN3_OK;
    TEST_ASSERT_TRUE((call->pw == NULL) == (call->phase == ODIN3_PROC_PH_LATCH));
    if (call->phase == ODIN3_PROC_PH_CLEAN && pw_has_attr(call->m, call->pw, "fake_error")) {
        odin3_proc_error(call->m, call->pw->prov, "fake error");
        *st = ODIN3_ERR_PARSE;
    }
    if (call->phase == ODIN3_PROC_PH_PRUNE && state->promote) {
        *st = fake_promote(call->pw);
    }
    return true;
}

static void fake_step_f(void *user, odin3_module_id module) {
    fake_state *state = user;
    TEST_ASSERT_TRUE(module.v < MODULES_MAX);
    state->step_f[module.v]++;
}

static void on_step(void *user, odin3_proc_step step, bool begin, const odin3_proc_mctx *mctx) {
    static const char *const k_names[] = {"A", "B", "C", "D", "E", "F"};
    fake_state *state = user;
    char word[8];
    TEST_ASSERT_NOT_NULL(mctx);
    TEST_ASSERT_TRUE((unsigned)step < ODIN3_PROC_N_STEPS);
    if (step == ODIN3_PROC_STEP_BUDGET) {
        TEST_ASSERT_TRUE(state->depth > 0); /* a budget check nests in a step */
        (void)snprintf(word, sizeof word, "%s ", begin ? "b" : "e");
    } else {
        state->depth += begin ? 1 : -1;
        (void)snprintf(word, sizeof word, "%s%c ", k_names[step], begin ? '+' : '-');
    }
    TEST_ASSERT_TRUE(strlen(state->trace) + strlen(word) < sizeof state->trace);
    (void)strcat(state->trace, word);
}

static void install_fakes(void) {
    memset(&fakes, 0, sizeof fakes);
    odin3_proc_test_hooks hooks = {fake_phase, fake_step_f, NULL, &fakes, on_step};
    odin3_proc_test_set_hooks(&hooks);
}

void setUp(void) {
    clear_lines();
    odin3_log_set_sink(line_sink, NULL);
    install_fakes();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_hooks(NULL);
    odin3_proc_test_set_limits(NULL);
    odin3_budget_test_set_limit(0);
    odin3_log_set_sink(NULL, NULL);
}

/* --- designs ------------------------------------------------------------------------------- */

/* m1: two processes, a root action t0 <- d in the first. */
#define M1_TEXT                                                                                    \
    "module \\m1\n"                                                                                \
    "  wire input 1 \\clk\n"                                                                       \
    "  wire input 2 \\d\n"                                                                         \
    "  wire output 3 \\q\n"                                                                        \
    "  wire output 4 \\r\n"                                                                        \
    "  wire \\t0\n"                                                                                \
    "  process $p1\n"                                                                              \
    "    assign \\t0 \\d\n"                                                                        \
    "    sync posedge \\clk\n"                                                                     \
    "      update \\q \\t0\n"                                                                      \
    "  end\n"                                                                                      \
    "  process $p2\n"                                                                              \
    "    sync posedge \\clk\n"                                                                     \
    "      update \\r \\d\n"                                                                       \
    "  end\n"                                                                                      \
    "end\n"

/* A module named name with one process; its node carries fake_error when fail. */
#define ONE_PROC_TEXT(name, attrs)                                                                 \
    "module \\" name "\n"                                                                          \
    "  wire input 1 \\clk\n"                                                                       \
    "  wire input 2 \\d\n"                                                                         \
    "  wire output 3 \\q\n" attrs "  process $p\n"                                                 \
    "    sync posedge \\clk\n"                                                                     \
    "      update \\q \\d\n"                                                                       \
    "  end\n"                                                                                      \
    "end\n"

#define FAIL_ATTR "  attribute \\fake_error 1\n"

static const char *const k_atomicity_text =
    M1_TEXT ONE_PROC_TEXT("m2", FAIL_ATTR) ONE_PROC_TEXT("m3", "");

static const char *const k_ok_text = M1_TEXT ONE_PROC_TEXT("m3", "");

static const char *const k_oom_text = M1_TEXT ONE_PROC_TEXT("m3", "") ONE_PROC_TEXT("m4", "");

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

static void build(const char *text, pb_result *res) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, res));
    check_full(res);
}

/* odin3_proc_run in a new pass run named proc; check FULL before and after. */
static odin3_status run_proc(pb_result *res, const odin3_proc_opts *opts, odin3_proc_stats *stats) {
    check_full(res);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res->design, odin3_bytes_cstr("proc"), &name));
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(res->design, name, &ctx));
    clear_lines();
    odin3_status st = odin3_proc_run(&ctx, res->design, opts, stats);
    check_full(res);
    return st;
}

static odin3_status pass_run(pb_result *res, const char *pass, const char *args) {
    clear_lines();
    return odin3_pass_run(res->design, pass, odin3_bytes_cstr(args));
}

static uint32_t module_id(const pb_result *res, const char *name) {
    const odin3_module *mod = pb_module(res, name);
    TEST_ASSERT_NOT_NULL(mod);
    return odin3_module_id_of(mod).v;
}

static uint32_t proc_count(const pb_result *res, const char *name) {
    return odin3_module_proc_count(pb_module(res, name));
}

/* --- options ------------------------------------------------------------------------------- */

static void expect_first_error(const char *want) {
    char errs[1][LOG_TEXT];
    TEST_ASSERT_TRUE(errors_seen(errs, 1) >= 1);
    TEST_ASSERT_EQUAL_STRING(want, errs[0]);
}

static void test_options(void) {
    pb_result res;
    build("module \\m\n  wire \\a\nend\n", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_run(&res, "proc", "--bogus"));
    expect_first_error("odin3: error: proc: unknown option '--bogus' (options: --noopt, "
                       "--stop-after <phase>)");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_run(&res, "proc", "--stop-after"));
    expect_first_error("odin3: error: proc: --stop-after needs one of: clean, rmdead, prune, arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_run(&res, "proc", "--stop-after mux"));
    expect_first_error("odin3: error: proc: --stop-after needs one of: clean, rmdead, prune, arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass_run(&res, "proc", "--noopt --stop-after prune"));
    TEST_ASSERT_EQUAL_STRING("proc: no processes", proc_info());
    const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr("proc"));
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_EQUAL_STRING("proc [--noopt] [--stop-after clean|rmdead|prune|arst]", def->help);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PASS_PROCESSES_OK, def->processes);
    TEST_ASSERT_FALSE(def->wants_ast);
    pb_free(&res);
}

/* --- the schedule -------------------------------------------------------------------------- */

static void test_no_processes(void) {
    pb_result res;
    build("module \\a\n  wire \\x\nend\nmodule \\b\n  wire \\y\nend\n", &res);
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_STRING("proc: no processes", proc_info());
    TEST_ASSERT_EQUAL_UINT32(0, stats.processes_lowered);
    pb_free(&res);
    build(k_ok_text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, &stats));
    TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    const char *info = proc_info();
    const char *want = "proc: 3 processes in 2 modules: 0 $dff, 0 $adff, 0 $aldff, 0 $dffsr, "
                       "0 $dlatch, 0 $mux, 0 $pmux; ";
    TEST_ASSERT_EQUAL_INT(0, strncmp(want, info, strlen(want)));
    TEST_ASSERT_EQUAL_STRING(" ms", info + strlen(info) - strlen(" ms"));
    TEST_ASSERT_NOT_NULL(strchr(info + strlen(want), '.')); /* one decimal */
    TEST_ASSERT_EQUAL_UINT32(3, stats.processes_lowered);
    TEST_ASSERT_EQUAL_UINT32(2, stats.modules_lowered);
    TEST_ASSERT_EQUAL_UINT32(0, stats.modules_failed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass_run(&res, "proc", ""));
    TEST_ASSERT_EQUAL_STRING("proc: no processes", proc_info());
    pb_free(&res);
}

/* Review Focus 4: m1 lowered, m2 fails and keeps its process, m3 only analysed. */
static void test_schedule_atomicity(void) {
    pb_result res;
    build(k_atomicity_text, &res);
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, proc_count(&res, "m1"));
    TEST_ASSERT_EQUAL_UINT32(1, proc_count(&res, "m2"));
    TEST_ASSERT_EQUAL_UINT32(1, proc_count(&res, "m3"));
    uint32_t m3 = module_id(&res, "m3");
    TEST_ASSERT_EQUAL_UINT32(1, fakes.calls[m3][ODIN3_PROC_PH_CLEAN]);
    TEST_ASSERT_EQUAL_UINT32(1, fakes.calls[m3][ODIN3_PROC_PH_CLOCKS]);
    TEST_ASSERT_EQUAL_UINT32(0, fakes.calls[m3][ODIN3_PROC_PH_MUX]);
    uint32_t m1 = module_id(&res, "m1");
    TEST_ASSERT_EQUAL_UINT32(2, fakes.calls[m1][ODIN3_PROC_PH_MUX]);
    TEST_ASSERT_EQUAL_UINT32(1, fakes.calls[m1][ODIN3_PROC_PH_LATCH]);
    TEST_ASSERT_EQUAL_UINT32(2, fakes.calls[m1][ODIN3_PROC_PH_FF]);
    TEST_ASSERT_EQUAL_UINT32(2, stats.processes_lowered);
    TEST_ASSERT_EQUAL_UINT32(1, stats.modules_lowered);
    TEST_ASSERT_EQUAL_UINT32(2, stats.modules_failed);
    TEST_ASSERT_EQUAL_UINT32(1, stats.errors);
    char errs[4][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, errors_seen(errs, 4));
    TEST_ASSERT_EQUAL_STRING("odin3: error: proc: module 'm2': fake error", errs[0]);
    TEST_ASSERT_EQUAL_STRING(
        "proc: 2 processes in 1 modules lowered; 2 modules keep their processes (1 errors)",
        proc_info());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_run(&res, "write_blif", "x.blif"));
    expect_first_error(
        "odin3: error: pass 'write_blif': module 'm2' has processes; run proc first");
    pb_free(&res);
}

/* The failing process at `attribute \src "t.v:7.3"`: the line is located (point (k)). */
static void test_located_error(void) {
    pb_result res;
    build(ONE_PROC_TEXT("m2", FAIL_ATTR "  attribute \\src \"t.v:7.3\"\n"), &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, NULL));
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, errors_seen(errs, 2));
    TEST_ASSERT_EQUAL_STRING("odin3: error: t.v:7:3: error: fake error", errs[0]);
    pb_free(&res);
}

/* Step F runs for m1 only (m2 failed, m3 analysed); never under --noopt or --stop-after. */
static void test_step_f_schedule(void) {
    pb_result res;
    build(k_atomicity_text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, fakes.step_f[module_id(&res, "m1")]);
    TEST_ASSERT_EQUAL_UINT32(0, fakes.step_f[module_id(&res, "m2")]);
    TEST_ASSERT_EQUAL_UINT32(0, fakes.step_f[module_id(&res, "m3")]);
    pb_free(&res);
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    const odin3_proc_opts stop = {false, ODIN3_PROC_STOP_RMDEAD};
    const odin3_proc_opts *cases[] = {&noopt, &stop};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        install_fakes();
        build(k_ok_text, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, cases[i], NULL));
        for (uint32_t k = 0; k < MODULES_MAX; k++) {
            TEST_ASSERT_EQUAL_UINT32(0, fakes.step_f[k]);
        }
        pb_free(&res);
    }
}

/* --- the on_step hook (2E Task 13: bench_proc times the steps with it) ------------------- */

/* The events of a run of text under opts. */
static const char *trace_of(const char *text, const odin3_proc_opts *opts, odin3_status want) {
    pb_result res;
    build(text, &res);
    TEST_ASSERT_EQUAL_INT(want, run_proc(&res, opts, NULL));
    pb_free(&res);
    TEST_ASSERT_EQUAL_INT(0, fakes.depth);
    return fakes.trace;
}

static void test_on_step(void) {
    /* One module, one process: A, B, C, D, E and F, each opened and closed once, a budget check
     * before the process's decode (A), its lowering (B) and its flip-flops (D). */
    TEST_ASSERT_EQUAL_STRING("A+ b e A- B+ b e B- C+ C- D+ b e D- E+ E- F+ F- ",
                             trace_of(ONE_PROC_TEXT("m3", ""), NULL, ODIN3_OK));
    /* --noopt has no step F. */
    install_fakes();
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    TEST_ASSERT_EQUAL_STRING("A+ b e A- B+ b e B- C+ C- D+ b e D- E+ E- ",
                             trace_of(ONE_PROC_TEXT("m3", ""), &noopt, ODIN3_OK));
    /* An error in step A ends the module's run there (and the open step is closed). */
    install_fakes();
    TEST_ASSERT_EQUAL_STRING("A+ b e A- ",
                             trace_of(ONE_PROC_TEXT("m2", FAIL_ATTR), NULL, ODIN3_ERR_PARSE));
    /* --stop-after has A only. */
    install_fakes();
    const odin3_proc_opts stop = {false, ODIN3_PROC_STOP_PRUNE};
    TEST_ASSERT_EQUAL_STRING("A+ b e A- ", trace_of(ONE_PROC_TEXT("m3", ""), &stop, ODIN3_OK));
}

/* --- --stop-after -------------------------------------------------------------------------- */

typedef struct saved_bodies {
    odin3_value body[BODIES_MAX];
    odin3_prov_id prov[BODIES_MAX];
    uint32_t count;
} saved_bodies;

/* The BODY and record of every $proc of module name, in node-ID order (the values stay readable
 * in the module's arena until compact). */
static saved_bodies save_bodies(const pb_result *res, const char *name) {
    saved_bodies saved = {0};
    const odin3_module *mod = pb_module(res, name);
    for (odin3_node_id node = pb_proc(res, name, 0); odin3_node_valid(node);
         node = pb_proc(res, name, saved.count)) {
        TEST_ASSERT_TRUE(saved.count < BODIES_MAX);
        saved.body[saved.count] = *odin3_node_param(mod, node, ODIN3_PROC_P_BODY);
        saved.prov[saved.count] = odin3_node_prov(mod, node);
        saved.count++;
    }
    return saved;
}

/* The net on bit 0 of the wire named name in mod. */
static odin3_net_id wire_net0(const pb_result *res, const odin3_module *mod, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(res->design), odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(mod, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(mod, wire, 0);
}

/* Each new node of module name took the old node's record (point (i)). */
static void expect_same_records(const pb_result *res, const char *name, const saved_bodies *old) {
    saved_bodies now = save_bodies(res, name);
    TEST_ASSERT_EQUAL_UINT32(old->count, now.count);
    for (uint32_t i = 0; i < now.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(old->prov[i].v, now.prov[i].v);
    }
}

/* The promotion t0 <- d is applied as a merge after re-encoding; the bodies lose it. */
static void test_stop_after_reencodes(void) {
    pb_result res;
    build(k_ok_text, &res);
    saved_bodies m1_old = save_bodies(&res, "m1");
    saved_bodies m3_old = save_bodies(&res, "m3");
    odin3_node_id old_first = pb_proc(&res, "m1", 0);
    fakes.promote = true;
    const odin3_proc_opts opts = {false, ODIN3_PROC_STOP_PRUNE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, &opts, NULL));
    TEST_ASSERT_TRUE(odin3_design_has_processes(res.design));
    TEST_ASSERT_EQUAL_STRING("proc: stopped after prune; 3 processes kept", proc_info());
    TEST_ASSERT_EQUAL_UINT32(0, fakes.calls[module_id(&res, "m1")][ODIN3_PROC_PH_ARST]);
    odin3_module *m1 = pb_module(&res, "m1");
    TEST_ASSERT_FALSE(odin3_node_live(m1, old_first));
    expect_same_records(&res, "m1", &m1_old);
    expect_same_records(&res, "m3", &m3_old);
    TEST_ASSERT_EQUAL_UINT32(wire_net0(&res, m1, "d").v, wire_net0(&res, m1, "t0").v);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(m1, pb_proc(&res, "m1", 0), &text));
    TEST_ASSERT_EQUAL_STRING("  sync posedge \\clk\n    update \\q \\d\nend\n", text.data);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* Fakes that change nothing: --stop-after clean re-encodes every BODY byte for byte. */
static void test_stop_after_identity(void) {
    pb_result res;
    build(k_ok_text, &res);
    saved_bodies before = save_bodies(&res, "m1");
    const odin3_proc_opts opts = {true, ODIN3_PROC_STOP_CLEAN};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, &opts, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, fakes.calls[module_id(&res, "m1")][ODIN3_PROC_PH_RMDEAD]);
    saved_bodies after = save_bodies(&res, "m1");
    TEST_ASSERT_EQUAL_UINT32(before.count, after.count);
    for (uint32_t i = 0; i < after.count; i++) {
        TEST_ASSERT_TRUE(odin3_value_equal(&before.body[i], &after.body[i]));
        TEST_ASSERT_EQUAL_UINT32(before.prov[i].v, after.prov[i].v);
    }
    pb_free(&res);
}

/* --- caps ---------------------------------------------------------------------------------- */

/* E6: a 1-byte budget stops proc at its first process, located there, with valid IR. */
static void test_budget_e6(void) {
    pb_result res;
    build(k_ok_text, &res);
    odin3_budget_test_set_limit(1);
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, &stats));
    char errs[4][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, errors_seen(errs, 4));
    TEST_ASSERT_EQUAL_STRING("odin3: error: proc: module 'm1': proc needs more than 1 bytes (the "
                             "design's budget is 60% of physical memory)",
                             errs[0]);
    TEST_ASSERT_EQUAL_UINT32(2, proc_count(&res, "m1"));
    TEST_ASSERT_EQUAL_UINT32(1, proc_count(&res, "m3"));
    TEST_ASSERT_EQUAL_UINT32(2, stats.modules_failed);
    TEST_ASSERT_EQUAL_UINT32(0, fakes.calls[module_id(&res, "m1")][ODIN3_PROC_PH_CLEAN]);
    pb_free(&res);
}

/* One module with n processes, each failing (fake_error) and each on its own bit of q. */
/* Appends piece at *at in out (of size TEXT_MAX), checking it fits. */
static void put_text(char *out, size_t *at, const char *piece) {
    size_t len = strlen(piece);
    TEST_ASSERT_TRUE(*at + len < TEXT_MAX);
    memcpy(out + *at, piece, len + 1);
    *at += len;
}

static void build_failing(uint32_t n, pb_result *res) {
    static char out[TEXT_MAX];
    char piece[LOG_TEXT];
    size_t at = 0;
    int len = snprintf(piece, sizeof piece,
                       "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                       "  wire width %u output 3 \\q\n",
                       n);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof piece);
    put_text(out, &at, piece);
    for (uint32_t i = 0; i < n; i++) {
        len = snprintf(piece, sizeof piece,
                       FAIL_ATTR "  process $p%u\n    sync posedge \\clk\n      update \\q [%u] "
                                 "\\d\n  end\n",
                       i, i);
        TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof piece);
        put_text(out, &at, piece);
    }
    put_text(out, &at, "end\n");
    build(out, res);
}

/* E7: the 20th error is the cap's line; a lowered cap counts the same way. */
static void test_error_cap(void) {
    pb_result res;
    build_failing(CAP_PROCS, &res);
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, &stats));
    char errs[LOG_LINES][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(20, errors_seen(errs, LOG_LINES));
    for (size_t i = 0; i < 19; i++) {
        TEST_ASSERT_EQUAL_STRING("odin3: error: proc: module 'm': fake error", errs[i]);
    }
    TEST_ASSERT_EQUAL_STRING("odin3: error: too many errors; proc stopped after 20", errs[19]);
    TEST_ASSERT_EQUAL_UINT32(20, stats.errors);
    TEST_ASSERT_EQUAL_UINT32(CAP_PROCS, proc_count(&res, "m"));
    pb_free(&res);
    install_fakes();
    build_failing(CAP_PROCS, &res);
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_errors = 3;
    odin3_proc_test_set_limits(&limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(3, errors_seen(errs, LOG_LINES));
    TEST_ASSERT_EQUAL_STRING("odin3: error: proc: module 'm': fake error", errs[1]);
    TEST_ASSERT_EQUAL_STRING("odin3: error: too many errors; proc stopped after 3", errs[2]);
    pb_free(&res);
}

/* The test hook clamps each cap to [1, its default]; NULL restores the defaults. */
static void test_limits_clamped(void) {
    odin3_proc_limits big = {1000, 1U << 30, 1000, 1U << 20, 1U << 30};
    odin3_proc_test_set_limits(&big);
    odin3_proc_limits now = odin3_proc_limits_now();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_MAX_ERRORS, now.max_errors);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_MAX_PATTERN_BITS, now.max_pattern_bits);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_MAX_ARST_ROUNDS, now.max_arst_rounds);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_MAX_SIGNAL_WALK, now.max_signal_walk);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_BUDGET_PINS, now.budget_pins);
    odin3_proc_limits zero = {0, 0, 0, 0, 0};
    odin3_proc_test_set_limits(&zero);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_proc_limits_now().budget_pins);
    odin3_proc_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_MAX_ERRORS, odin3_proc_limits_now().max_errors);
}

/* --- step E, determinism, pending phases --------------------------------------------------- */

/* The unnamed $tmp1 (only on the deleted node's pins) is deleted; t0's net (a wire) stays. */
/* The last net of mod spelled as a net of its own (no named wire bit): pb's $tmp1. */
static odin3_net_id last_tmp_net(const odin3_module *mod) {
    odin3_net_id tmp = {0};
    for (uint32_t i = 1; i < odin3_module_net_end(mod); i++) {
        if (odin3_proc_sig_is_net(mod, (odin3_net_id){i})) {
            tmp = (odin3_net_id){i};
        }
    }
    TEST_ASSERT_TRUE(odin3_net_valid(tmp));
    return tmp;
}

static void test_orphans(void) {
    pb_result res;
    build("module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n"
          "  wire \\t0\n  wire $tmp1\n  process $p\n    assign $tmp1 \\d\n"
          "    assign \\t0 $tmp1\n    sync posedge \\clk\n      update \\q \\t0\n  end\nend\n",
          &res);
    odin3_module *mod = res.module;
    odin3_net_id tmp = last_tmp_net(mod);
    odin3_net_id t0 = wire_net0(&res, mod, "t0");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, NULL));
    TEST_ASSERT_FALSE(odin3_net_live(mod, tmp));
    TEST_ASSERT_TRUE(odin3_net_live(mod, t0));
    pb_free(&res);
}

static void rtlil_after_proc(const char *text, odin3_strbuf *out) {
    pb_result res;
    install_fakes();
    build(text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, out));
    pb_free(&res);
}

static void test_determinism(void) {
    odin3_strbuf first;
    odin3_strbuf second;
    odin3_strbuf_init(&first);
    odin3_strbuf_init(&second);
    rtlil_after_proc(k_ok_text, &first);
    rtlil_after_proc(k_ok_text, &second);
    TEST_ASSERT_TRUE(first.len > 0);
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

/* --- out of memory ------------------------------------------------------------------------- */

/*
 * Whether the injected failure fired: probes up to fail + 1 allocations, one of which fails when it
 * did not (fewer than fail + 1 allocations were made). Leaves the hook disabled. A sweep that ends
 * only when the failure did not fire is not cut short by a failure the code absorbs.
 */
static bool injection_fired(long fail) {
    for (long i = 0; i <= fail; i++) {
        void *probe = odin3_util_malloc(1);
        if (probe == NULL) {
            return false;
        }
        odin3_util_free(probe);
    }
    odin3_util_set_alloc_fail_after(-1);
    return true;
}

/* One injection: NO_MEMORY or OK, valid IR (ASan finds a leak at exit). True once the run makes
 * no more allocations than fail (review M2 of 2E Task 8: an absorbed failure no longer ends the
 * sweep). */
static bool oom_step(long fail) {
    pb_result res;
    install_fakes();
    build(k_oom_text, &res);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res.design, odin3_bytes_cstr("proc"), &name));
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(res.design, name, &ctx));
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_proc_run(&ctx, res.design, NULL, NULL);
    bool fired = injection_fired(fail);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    check_full(&res);
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    }
    pb_free(&res);
    return !fired;
}

static void test_oom(void) {
    bool done = false;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
}

/* --- real phases (Task 9): no hooks -------------------------------------------------------- */

/* The raw error lines (the sink's text, no prefix), in order. */
static size_t errors_raw(char out[][LOG_TEXT], size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (levels[i] != ODIN3_LOG_ERROR) {
            continue;
        }
        if (count < max) {
            int len = snprintf(out[count], LOG_TEXT, "%s", lines[i]);
            TEST_ASSERT_TRUE(len >= 0 && len < LOG_TEXT);
        }
        count++;
    }
    return count;
}

/* m1: ff_dff's body (test_proc_ff.c). */
#define DFF_MODULE(name)                                                                           \
    "module \\" name "\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n"           \
    "  wire \\t0\n  process $p\n    assign \\t0 \\d\n    sync posedge \\clk\n"                     \
    "      update \\q \\t0\n  end\nend\n"

/* m2: clocks_e1a's body (test_proc_arst.c), at t.v:9.3: two root switches, so arst finds no
 * reset and q keeps two edge events (E1a). */
#define E1A_MODULE                                                                                 \
    "module \\m2\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\a\n"                \
    "  wire input 4 \\b\n  wire input 5 \\d\n  wire output 6 \\q\n  wire \\t0\n"                   \
    "  attribute \\src \"t.v:9.3\"\n  process $p\n"                                                \
    "    switch \\a\n      case 1'1\n        assign \\t0 \\d\n    end\n"                           \
    "    switch \\b\n      case 1'1\n        assign \\t0 1'0\n    end\n"                           \
    "    sync posedge \\clk\n      update \\q \\t0\n"                                              \
    "    sync posedge \\rst\n      update \\q \\t0\n  end\nend\n"

/* m3: clocks_e1b's body (test_proc_arst.c), at t.v:20.3: `@(posedge clk or negedge clk)` (E1b). */
#define E1B_MODULE                                                                                 \
    "module \\m3\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n  wire \\t0\n"    \
    "  attribute \\src \"t.v:20.3\"\n  process $p\n    assign \\t0 \\d\n"                          \
    "    sync posedge \\clk\n      update \\q \\t0\n"                                              \
    "    sync negedge \\clk\n      update \\q \\t0\n  end\nend\n"

/* Review Focus 4 with the real phases (finding 16): m1 is lowered, m2 fails with E1a located at
 * its always and keeps its node, m3 is analysed after the failure (its own E1b shows it) and keeps
 * its node too. */
static void test_atomicity_real_e1a(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build(DFF_MODULE("m1") E1A_MODULE E1B_MODULE, &res);
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(&res, NULL, &stats));
    char errs[4][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(2, errors_raw(errs, 4));
    TEST_ASSERT_EQUAL_STRING("t.v:9:3: error: 'q' is clocked by both 'clk' and 'rst'; a second "
                             "edge event must be an asynchronous reset tested by the block's only "
                             "top-level 'if'",
                             errs[0]);
    TEST_ASSERT_EQUAL_STRING("t.v:20:3: error: 'q' is clocked by both edges of 'clk'; dual-edge "
                             "registers are not supported",
                             errs[1]);
    TEST_ASSERT_EQUAL_STRING(
        "proc: 1 processes in 1 modules lowered; 2 modules keep their processes (2 errors)",
        proc_info());
    TEST_ASSERT_EQUAL_UINT32(0, proc_count(&res, "m1"));
    TEST_ASSERT_EQUAL_UINT32(1, proc_count(&res, "m2"));
    TEST_ASSERT_EQUAL_UINT32(1, proc_count(&res, "m3"));
    TEST_ASSERT_EQUAL_UINT32(1, stats.cells[ODIN3_PROC_K_DFF]);
    TEST_ASSERT_EQUAL_UINT32(2, stats.errors);
    TEST_ASSERT_EQUAL_UINT32(2, stats.modules_failed);
    pb_free(&res);
}

enum { BUDGET_TEST_PINS = 64, BUDGET_SEARCH = 1 << 24 };

/* A 15-item case on a 4-bit select, 8 bits wide, at t.v:5.3: the mux step makes 15 $eq (9 pins
 * each) and a $pmux of 151 pins before the register, so with budget_pins 64 it checks the budget
 * at least twice. */
static const char *const k_budget_text =
    "module \\m\n  wire input 1 \\clk\n  wire width 4 input 2 \\s\n  wire width 8 input 3 \\a\n"
    "  wire width 8 input 4 \\b\n  wire width 8 output 5 \\q\n  wire width 8 \\t0\n"
    "  attribute \\src \"t.v:5.3\"\n  process $p\n    switch \\s\n"
    "      case 4'0001\n        assign \\t0 \\a\n      case 4'0010\n        assign \\t0 \\b\n"
    "      case 4'0011\n        assign \\t0 \\a\n      case 4'0100\n        assign \\t0 \\b\n"
    "      case 4'0101\n        assign \\t0 \\a\n      case 4'0110\n        assign \\t0 \\b\n"
    "      case 4'0111\n        assign \\t0 \\a\n      case 4'1000\n        assign \\t0 \\b\n"
    "      case 4'1001\n        assign \\t0 \\a\n      case 4'1010\n        assign \\t0 \\b\n"
    "      case 4'1011\n        assign \\t0 \\a\n      case 4'1100\n        assign \\t0 \\b\n"
    "      case 4'1101\n        assign \\t0 \\a\n      case 4'1110\n        assign \\t0 \\b\n"
    "      case 4'1111\n        assign \\t0 \\a\n      case\n        assign \\t0 8'00000000\n"
    "    end\n    sync posedge \\clk\n      update \\q \\t0\n  end\nend\n";

/* A module and the pins between in-cell budget checks for a budget test. */
typedef struct budget_case {
    const char *text;
    uint32_t pins;
} budget_case;

/* proc --noopt on a new build of the case's text with its budget_pins and the budget's limit at
 * limit, check FULL before and after; true when the module changed (its $proc nodes are gone). */
static bool budget_probe(budget_case bc, uint64_t limit, odin3_status *st) {
    pb_result res;
    build(bc.text, &res);
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.budget_pins = bc.pins;
    odin3_proc_test_set_limits(&limits);
    odin3_budget_test_set_limit(limit);
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    *st = run_proc(&res, &noopt, NULL);
    bool changed = proc_count(&res, "m") == 0;
    pb_free(&res);
    return changed;
}

/* The count of live nodes of type `type` in mod. */
static uint32_t count_nodes(const pb_result *res, const odin3_module *mod, const char *type) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(mod); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(mod, node) &&
            strcmp(odin3_celltype_get(res->design, odin3_node_type(mod, node))->name, type) == 0) {
            count++;
        }
    }
    return count;
}

/* The smallest limit at which proc changes the module, i.e. what the budget measures once step A is
 * done (the value odin3_design_bytes_reserved reports then, plus proc's working forms, which the
 * check counts too): below it the boundary check before step B fails; the design's own bytes leave
 * the module unchanged, 2^24 bytes more lower it completely. */
static uint64_t budget_threshold(budget_case bc) {
    pb_result res;
    build(bc.text, &res);
    uint64_t lo = odin3_design_bytes_reserved(res.design);
    pb_free(&res);
    uint64_t hi = lo + BUDGET_SEARCH;
    odin3_status st = ODIN3_OK;
    TEST_ASSERT_FALSE(budget_probe(bc, lo, &st));
    TEST_ASSERT_TRUE(budget_probe(bc, hi, &st));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (budget_probe(bc, mid, &st)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

/* proc --noopt on a new build of the case at its threshold: ODIN3_ERR_PARSE with exactly the
 * located E6 at `at` (file:line:col); the design is left in *res, checked FULL. */
static void budget_stop(budget_case bc, const char *at, pb_result *res) {
    uint64_t limit = budget_threshold(bc);
    build(bc.text, res);
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.budget_pins = bc.pins;
    odin3_proc_test_set_limits(&limits);
    odin3_budget_test_set_limit(limit);
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_proc(res, &noopt, NULL));
    char want[LOG_TEXT];
    int len = snprintf(want, sizeof want,
                       "%s: error: proc needs more than %llu bytes (the design's budget is 60%% of "
                       "physical memory)",
                       at, (unsigned long long)limit);
    TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, errors_raw(errs, 2));
    TEST_ASSERT_EQUAL_STRING(want, errs[0]);
    TEST_ASSERT_EQUAL_UINT32(0, proc_count(res, "m"));
}

/*
 * Review Focus 5 with the real phases (finding 15): at the threshold every process-boundary check
 * passes, and the first check inside the lowering, when the mux step's 8th $eq brings its pins to
 * 72, stops proc with the located E6 at the process being lowered. The module is left incomplete
 * (seven $eq, no $pmux, no register) and valid.
 */
static void test_budget_e6_mid_lowering(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    budget_stop((budget_case){k_budget_text, BUDGET_TEST_PINS}, "t.v:5:3", &res);
    /* the 8th $eq brings the pins to 72: its check, the first inside the mux step, stops proc */
    TEST_ASSERT_EQUAL_UINT32(7, count_nodes(&res, res.module, "$eq"));
    TEST_ASSERT_EQUAL_UINT32(0, count_nodes(&res, res.module, "$pmux"));
    TEST_ASSERT_EQUAL_UINT32(0, count_nodes(&res, res.module, "$dff"));
    pb_free(&res);
}

/* latch_verified_example's body at t.v:7.3. */
static const char *const k_budget_latch_text =
    "module \\m\n  wire input 1 \\en\n  wire input 2 \\a\n  wire output 3 \\y\n  wire \\t0\n"
    "  wire \\t1\n  attribute \\src \"t.v:7.3\"\n  process $p\n    assign \\t0 \\t1\n"
    "    switch \\en\n      case 1'1\n        assign \\t1 \\a\n      case\n"
    "        assign \\t1 \\y\n    end\n    sync always\n      update \\y \\t0\n  end\nend\n";

/* `if (en) q <= d;` (2D's body: the root default t0 <- q) at t.v:3.3. */
static const char *const k_budget_ff_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\en\n  wire input 3 \\d\n"
    "  wire output 4 \\q\n  wire \\t0\n  attribute \\src \"t.v:3.3\"\n  process $p\n"
    "    assign \\t0 \\q\n    switch \\en\n      case 1'1\n        assign \\t0 \\d\n    end\n"
    "    sync posedge \\clk\n      update \\q \\t0\n  end\nend\n";

/* The latch step's own budget check (2E-3 final review M5): with no check inside a cell (the
 * default 2^16 pins), the first check after step B is the latch step's, at its process, before the
 * holds and the $dlatch: the mux exists, no latch does. */
static void test_budget_e6_at_latch(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    budget_stop((budget_case){k_budget_latch_text, ODIN3_PROC_BUDGET_PINS}, "t.v:7:3", &res);
    TEST_ASSERT_EQUAL_UINT32(1, count_nodes(&res, res.module, "$mux"));
    TEST_ASSERT_EQUAL_UINT32(0, count_nodes(&res, res.module, "$dlatch"));
    TEST_ASSERT_EQUAL_UINT32(0, count_nodes(&res, res.module, "$not"));
    pb_free(&res);
}

/* The flip-flop step's budget check (M5): a clocked block has no ALWAYS action, so after step B the
 * next check is the one before its flip-flops: the mux exists, the $dff does not. */
static void test_budget_e6_at_ff(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    budget_stop((budget_case){k_budget_ff_text, ODIN3_PROC_BUDGET_PINS}, "t.v:3:3", &res);
    TEST_ASSERT_EQUAL_UINT32(1, count_nodes(&res, res.module, "$mux"));
    TEST_ASSERT_EQUAL_UINT32(0, count_nodes(&res, res.module, "$dff"));
    pb_free(&res);
}

/* mux_dead_eq's body (test_proc_mux.c) as a real process, with a case rmdead removes (2'10, after
 * 2'-- covers every value) that assigned and read the unnamed $tmp1: after proc $tmp1's net, on
 * no pin of anything left, is deleted (step E, finding 16). */
static void test_real_orphans(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build("module \\m\n  wire width 2 input 1 \\s\n  wire input 2 \\a\n  wire output 3 \\y\n"
          "  wire \\t1\n  wire $tmp1\n  process $p\n    switch \\s\n"
          "      case 2'01, 2'--\n        assign \\t1 \\a\n"
          "      case 2'10\n        assign $tmp1 \\a\n        assign \\t1 $tmp1\n    end\n"
          "    sync always\n      update \\y \\t1\n  end\nend\n",
          &res);
    odin3_module *mod = res.module;
    odin3_net_id tmp = last_tmp_net(mod);
    TEST_ASSERT_TRUE(odin3_net_live(mod, tmp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, NULL));
    TEST_ASSERT_FALSE(odin3_net_live(mod, tmp));
    TEST_ASSERT_EQUAL_UINT32(wire_net0(&res, mod, "a").v, wire_net0(&res, mod, "y").v);
    pb_free(&res);
}

/* ff_adff's process and latch_verified_example's in one module (their temporaries renamed). */
static const char *const k_adff_latch_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
    "  wire output 4 \\q\n  wire \\t0\n  wire input 5 \\en\n  wire input 6 \\a\n"
    "  wire output 7 \\y\n  wire \\l0\n  wire \\l1\n"
    "  process $p1\n    switch \\rst\n      case 1'1\n        assign \\t0 1'0\n"
    "      case\n        assign \\t0 \\d\n    end\n"
    "    sync posedge \\clk\n      update \\q \\t0\n"
    "    sync posedge \\rst\n      update \\q \\t0\n  end\n"
    "  process $p2\n    assign \\l0 \\l1\n    switch \\en\n      case 1'1\n"
    "        assign \\l1 \\a\n      case\n        assign \\l1 \\y\n    end\n"
    "    sync always\n      update \\y \\l0\n  end\nend\n";

/* True when text is `<digits>.<digit> ms`. */
static bool is_ms(const char *text) {
    size_t digits = strspn(text, "0123456789");
    return digits > 0 && text[digits] == '.' && text[digits + 1] >= '0' &&
           text[digits + 1] <= '9' && strcmp(text + digits + 2, " ms") == 0;
}

/* The success info line under --noopt, matching ^proc: 2 processes in 1 modules: 0 \$dff, 1
 * \$adff, 0 \$aldff, 0 \$dffsr, 1 \$dlatch, 1 \$mux, 0 \$pmux; [0-9]+\.[0-9] ms$, and the
 * statistics: 6 helpers (the latch's two $not, two $reduce_or and $and, the $adff's $reduce_or)
 * and one W1. */
static void test_info_line_success(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build(k_adff_latch_text, &res);
    odin3_proc_stats stats;
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, &noopt, &stats));
    const char *info = proc_info();
    const char *want = "proc: 2 processes in 1 modules: 0 $dff, 1 $adff, 0 $aldff, 0 $dffsr, "
                       "1 $dlatch, 1 $mux, 0 $pmux; ";
    TEST_ASSERT_EQUAL_INT(0, strncmp(want, info, strlen(want)));
    TEST_ASSERT_TRUE_MESSAGE(is_ms(info + strlen(want)), info);
    TEST_ASSERT_EQUAL_UINT32(6, stats.cells[ODIN3_PROC_K_HELPER]);
    TEST_ASSERT_EQUAL_UINT32(1, stats.warnings[ODIN3_PROC_W1]);
    TEST_ASSERT_EQUAL_UINT32(2, stats.processes_lowered);
    TEST_ASSERT_EQUAL_UINT32(0, stats.opt_expr.folded); /* --noopt: no step F */
    pb_free(&res);
}

/* The live $reduce_or nodes of the last module whose A is the constant 1 (the latch's enable). */
static uint32_t reduce_or_of_one(const pb_result *res) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(res->module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(res->design, odin3_node_type(res->module, node));
        odin3_pinslice pins = odin3_node_port(res->module, node, 0);
        if (strcmp(def->name, "$reduce_or") == 0 && pins.count == 1) {
            odin3_net_id net = odin3_pin_net(res->module, pins.first);
            hits += odin3_net_const_value(res->module, net) == ODIN3_CONST_1 ? 1U : 0U;
        }
    }
    return hits;
}

/*
 * Step F (point (f); the pre-merge step of 2F-2): proc without --noopt ends with opt_expr -keepdc
 * on each lowered module. k_adff_latch_text's latch enable is a $reduce_or of the constant 1 (the
 * rmdead'ed condition of a full case), which opt_expr folds and then takes the $and and $not chain
 * above it apart (the $dlatch's EN becomes `en`); under --noopt the cell stays and
 * stats.opt_expr is zero. Yosys 0.55's proc builds the latch another way, so the oracle runs on
 * the cells that Odin III's `proc --noopt` writes (write_rtlil, the post-lowering dump):
 *   yosys -p "read_rtlil stepf.il; opt_expr -keepdc; write_rtlil"
 * leaves the $not, the $dlatch with EN = en, and connects the $reduce_or's Y to 1'1.
 */
static void test_step_f_folds(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build(k_adff_latch_text, &res);
    const odin3_proc_opts noopt = {true, ODIN3_PROC_STOP_NONE};
    odin3_proc_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, &noopt, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, reduce_or_of_one(&res));
    TEST_ASSERT_EQUAL_UINT32(0, stats.opt_expr.folded + stats.opt_expr.rewritten);
    pb_free(&res);
    build(k_adff_latch_text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, &stats));
    TEST_ASSERT_TRUE(stats.opt_expr.folded >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, reduce_or_of_one(&res));
    uint32_t changes = stats.opt_expr.rewritten + stats.opt_expr.folded;
    TEST_ASSERT_TRUE(changes >= 1);
    char want[256];
    (void)snprintf(want, sizeof want,
                   "proc: 2 processes in 1 modules: 0 $dff, 1 $adff, 0 $aldff, 0 $dffsr, "
                   "1 $dlatch, 1 $mux, 0 $pmux; opt_expr: %" PRIu32 " changes; ",
                   changes);
    const char *info = proc_info();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(want, info, strlen(want)), info);
    TEST_ASSERT_TRUE_MESSAGE(is_ms(info + strlen(want)), info);
    pb_free(&res);
}

/*
 * Every construct of Tasks 4-9 in one module: a switch on a net tied to a constant (clean folds
 * it), a case listing every value (rmdead drops its default), a casez whose second item is dead,
 * prune's promotions, the reset forms of arst ($adff, the self-reset mux, $dffsr from two resets
 * with W4, $aldff with W3), mux trees ($mux, $pmux, a priority chain), a latch (W1) and a
 * combinational output whose init goes (W2), and $dff on both edges.
 */
static const char *const k_every_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\r2\n"
    "  wire input 4 \\d\n  wire input 5 \\e\n  wire input 6 \\a\n  wire input 7 \\en\n"
    "  wire width 2 input 8 \\s\n  wire width 8 input 9 \\dw\n  wire output 10 \\q\n"
    "  wire output 11 \\u\n  wire output 12 \\sr\n  wire output 13 \\l\n"
    "  wire width 8 output 14 \\w\n  wire output 15 \\y\n  attribute \\init 2'01\n"
    "  wire width 2 output 16 \\z\n  wire output 17 \\v\n  wire \\k\n  wire \\nr2\n"
    "  wire \\t0\n  wire \\u0\n  wire \\s0\n  wire \\l0\n  wire width 8 \\w0\n  wire \\y0\n"
    "  wire \\y1\n  wire width 2 \\z0\n  wire \\v0\n  connect \\k 1'0\n"
    "  cell $logic_not $c1\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
    "    parameter \\Y_WIDTH 1\n    connect \\A \\r2\n    connect \\Y \\nr2\n  end\n"
    "  process $p1\n    assign \\u0 \\u\n    switch \\rst\n"
    "      case 1'1\n        assign \\t0 1'0\n"
    "      case\n        assign \\t0 \\d\n        assign \\u0 \\e\n    end\n"
    "    sync posedge \\clk\n      update \\q \\t0\n      update \\u \\u0\n"
    "    sync posedge \\rst\n      update \\q \\t0\n      update \\u \\u0\n  end\n"
    "  process $p2\n    switch \\rst\n      case 1'1\n        assign \\s0 1'0\n      case\n"
    "        switch \\nr2\n          case 1'1\n            assign \\s0 1'1\n"
    "          case\n            assign \\s0 \\d\n        end\n    end\n"
    "    sync posedge \\clk\n      update \\sr \\s0\n"
    "    sync posedge \\rst\n      update \\sr \\s0\n"
    "    sync negedge \\r2\n      update \\sr \\s0\n  end\n"
    "  process $p3\n    assign \\l0 \\l\n    switch \\rst\n      case 1'1\n"
    "        assign \\l0 \\a\n    end\n"
    "    sync posedge \\clk\n      update \\l \\l0\n"
    "    sync posedge \\rst\n      update \\l \\l0\n  end\n"
    "  process $p4\n    assign \\w0 \\w\n    switch \\k\n      case 1'1\n"
    "        assign \\w0 8'00000000\n    end\n    switch \\en\n      case 1'1\n"
    "        assign \\w0 \\dw\n    end\n    sync negedge \\clk\n      update \\w \\w0\n  end\n"
    "  process $p5\n    assign \\y0 \\y1\n    switch \\en\n      case 1'1\n"
    "        assign \\y1 \\a\n      case\n        assign \\y1 \\y\n    end\n"
    "    sync always\n      update \\y \\y0\n  end\n"
    "  process $p6\n    switch \\s\n      case 2'00\n        assign \\z0 2'01\n"
    "      case 2'01\n        assign \\z0 2'10\n      case 2'10\n        assign \\z0 2'11\n"
    "      case 2'11\n        assign \\z0 2'00\n      case\n        assign \\z0 2'xx\n"
    "    end\n    sync always\n      update \\z \\z0\n  end\n"
    "  process $p7\n    switch \\s\n      case 2'1-\n        assign \\v0 \\a\n"
    "      case 2'11\n        assign \\v0 \\d\n      case 2'-1\n        assign \\v0 \\e\n"
    "      case\n        assign \\v0 1'0\n    end\n"
    "    sync posedge \\clk\n      update \\v \\v0\n  end\nend\n";

/* One run of every construct: its write_rtlil text (a fresh build each time). */
static void rtlil_real(odin3_strbuf *out, odin3_proc_stats *stats) {
    pb_result res;
    build(k_every_text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_proc(&res, NULL, stats));
    TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, out));
    pb_free(&res);
}

/* Two runs of every construct give byte-identical write_rtlil text; the run uses each kind. */
static void test_real_determinism(void) {
    odin3_proc_test_set_hooks(NULL);
    odin3_strbuf first;
    odin3_strbuf second;
    odin3_strbuf_init(&first);
    odin3_strbuf_init(&second);
    odin3_proc_stats stats;
    rtlil_real(&first, &stats);
    rtlil_real(&second, NULL);
    TEST_ASSERT_TRUE(first.len > 0);
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    static const odin3_proc_kind k_kinds[] = {
        ODIN3_PROC_K_DFF,   ODIN3_PROC_K_ADFF,   ODIN3_PROC_K_ALDFF,
        ODIN3_PROC_K_DFFSR, ODIN3_PROC_K_DLATCH, ODIN3_PROC_K_MUX,
        ODIN3_PROC_K_PMUX,  ODIN3_PROC_K_EQ,     ODIN3_PROC_K_HELPER};
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        TEST_ASSERT_TRUE(stats.cells[k_kinds[i]] > 0);
    }
    static const odin3_proc_warning k_warns[] = {ODIN3_PROC_W1, ODIN3_PROC_W2, ODIN3_PROC_W3,
                                                 ODIN3_PROC_W4};
    for (size_t i = 0; i < sizeof k_warns / sizeof k_warns[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(1, stats.warnings[k_warns[i]]);
    }
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

/* §2.3's postcondition: after a successful run the design has no processes, write_blif runs (it
 * refused the design before), and a second proc logs `proc: no processes`. */
/* A new private directory in dir (beside the test's working directory, so parallel runs never
 * share it) and the path of out.blif in it. */
static void temp_blif(char *dir, char *path) {
    int len = snprintf(dir, LOG_TEXT, "odin3_proc_XXXXXX");
    TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    len = snprintf(path, LOG_TEXT, "%s/out.blif", dir);
    TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
}

static void test_postcondition(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build(DFF_MODULE("m"), &res);
    char dir[LOG_TEXT];
    char path[LOG_TEXT];
    temp_blif(dir, path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_run(&res, "write_blif", path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass_run(&res, "proc", ""));
    TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass_run(&res, "write_blif", path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass_run(&res, "proc", ""));
    TEST_ASSERT_EQUAL_STRING("proc: no processes", proc_info());
    TEST_ASSERT_EQUAL_INT(0, remove(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
    pb_free(&res);
}

/* One injection over the whole pass on every construct: NO_MEMORY or OK, valid IR, no leak (ASan
 * at exit). True once the run makes no more allocations than fail. */
static bool real_oom_step(long fail) {
    pb_result res;
    build(k_every_text, &res);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res.design, odin3_bytes_cstr("proc"), &name));
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(res.design, name, &ctx));
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_proc_run(&ctx, res.design, NULL, NULL);
    bool fired = injection_fired(fail);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    check_full(&res);
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    }
    pb_free(&res);
    return !fired;
}

static void test_real_oom(void) {
    odin3_proc_test_set_hooks(NULL);
    bool done = false;
    long fail = 0;
    for (; fail < REAL_OOM_LIMIT && !done; fail++) {
        done = real_oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(fail > 1);
}

/* --- docs ---------------------------------------------------------------------------------- */

static char doc_buf[DOC_MAX];

static void test_docs(void) {
    FILE *file = fopen(ODIN3_DOCS_DIR "/PASSES.md", "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(doc_buf, 1, DOC_MAX - 1, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE(len < DOC_MAX - 1);
    doc_buf[len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(
        doc_buf, "\n| `proc [--noopt] [--stop-after clean\\|rmdead\\|prune\\|arst]` | 2 (2E) |"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_options);
    RUN_TEST(test_no_processes);
    RUN_TEST(test_schedule_atomicity);
    RUN_TEST(test_located_error);
    RUN_TEST(test_step_f_schedule);
    RUN_TEST(test_stop_after_reencodes);
    RUN_TEST(test_stop_after_identity);
    RUN_TEST(test_budget_e6);
    RUN_TEST(test_error_cap);
    RUN_TEST(test_limits_clamped);
    RUN_TEST(test_orphans);
    RUN_TEST(test_determinism);
    RUN_TEST(test_atomicity_real_e1a);
    RUN_TEST(test_budget_e6_mid_lowering);
    RUN_TEST(test_budget_e6_at_latch);
    RUN_TEST(test_budget_e6_at_ff);
    RUN_TEST(test_real_orphans);
    RUN_TEST(test_info_line_success);
    RUN_TEST(test_step_f_folds);
    RUN_TEST(test_on_step);
    RUN_TEST(test_real_determinism);
    RUN_TEST(test_postcondition);
    RUN_TEST(test_real_oom);
    RUN_TEST(test_oom);
    RUN_TEST(test_docs);
    return UNITY_END();
}
