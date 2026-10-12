/*
 * test_proc_pass.c — the proc pass skeleton (2E spec §2, §12, §15, §16; plan Task 3): options,
 * the per-module schedule and its atomicity (Review Focus 4), statistics and info lines, step F,
 * --stop-after's re-encoding, the byte budget (E6), the error cap (E7), orphan nets, determinism,
 * the pending phases (point (h)) and out of memory. The phases are fakes installed through
 * proc_test.h: every analysis phase succeeds, except CLEAN on a process whose node carries the
 * attribute fake_error, which reports `fake error` at the process; mux, latch and ff do nothing
 * (step B itself deletes the node).
 *
 * Yosys re-derivation: none. These tests pin the schedule with fake phases; the real phases'
 * tests (Tasks 4-9) each record their yosys-phase command.
 */
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void install_fakes(void) {
    memset(&fakes, 0, sizeof fakes);
    odin3_proc_test_hooks hooks = {fake_phase, fake_step_f, &fakes};
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

/* Point (h): without hooks the first pending phase reports once and nothing changes. Task 4 changes
 * the phase to 'prune', Task 5 'arst', Task 6 'mux', Task 7 'latch', Task 8 'ff'; Task 9 deletes
 * this test. */
static void test_pending_stub(void) {
    odin3_proc_test_set_hooks(NULL);
    pb_result res;
    build(M1_TEXT, &res);
    saved_bodies before = save_bodies(&res, "m1");
    odin3_node_id first = pb_proc(&res, "m1", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pass_run(&res, "proc", ""));
    char errs[4][LOG_TEXT];
    TEST_ASSERT_TRUE(errors_seen(errs, 4) >= 1);
    TEST_ASSERT_EQUAL_STRING("odin3: error: proc: 'latch' is not available yet (2E)", errs[0]);
    size_t pending = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        pending += strstr(lines[i], "is not available yet") != NULL ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_size_t(1, pending);
    TEST_ASSERT_EQUAL_UINT32(first.v, pb_proc(&res, "m1", 0).v);
    saved_bodies after = save_bodies(&res, "m1");
    TEST_ASSERT_EQUAL_UINT32(2, after.count);
    for (uint32_t i = 0; i < after.count; i++) {
        TEST_ASSERT_TRUE(odin3_value_equal(&before.body[i], &after.body[i]));
    }
    TEST_ASSERT_EQUAL(ODIN3_PROC_PH_LATCH, odin3_proc_pending_first());
    pb_free(&res);
}

/* --- out of memory ------------------------------------------------------------------------- */

/* One injection: NO_MEMORY or OK, valid IR (ASan finds a leak at exit). True once it succeeds. */
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
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    check_full(&res);
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE(odin3_design_has_processes(res.design));
    }
    pb_free(&res);
    return st == ODIN3_OK;
}

static void test_oom(void) {
    bool done = false;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
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
    RUN_TEST(test_pending_stub);
    RUN_TEST(test_oom);
    RUN_TEST(test_docs);
    return UNITY_END();
}
