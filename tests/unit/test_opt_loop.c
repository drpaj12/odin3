/*
 * test_opt_loop.c — the eight opt passes' registrations, option parsing, the stubs, the sub-pass
 * call wrapper (hook, budget, snapshots) and --opt-stats (2F spec OP-12, OP-16; plan Task 4).
 * Task 13 extends it with the loop.
 */
#include "backends/rtlil/writer.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/manager.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "proc_build.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { LINE_MAX_LEN = 256, LINES_MAX = 64, NETS_MANY = 10000, BUDGET_LIMIT = 4096, PATH_LEN = 128 };

typedef struct log_line {
    odin3_log_level level;
    char text[LINE_MAX_LEN];
} log_line;

static log_line lines[LINES_MAX];
static uint32_t n_lines;
static pb_result res;

/* dst = format with its one %s replaced by arg (asserted to fit). */
static void fmt_str(char *dst, size_t cap, const char *format, const char *arg) {
    int len = snprintf(dst, cap, format, arg);
    TEST_ASSERT_TRUE(len >= 0 && (size_t)len < cap);
}

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LINES_MAX) {
        fmt_str(lines[n_lines].text, LINE_MAX_LEN, "%s", msg);
        lines[n_lines++].level = level;
    }
}

static uint32_t count_lines(odin3_log_level level, const char *text) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level && strcmp(lines[i].text, text) == 0 ? 1U : 0U;
    }
    return hits;
}

static uint32_t count_prefix(const char *prefix) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += strncmp(lines[i].text, prefix, strlen(prefix)) == 0 ? 1U : 0U;
    }
    return hits;
}

void setUp(void) {
    memset(&res, 0, sizeof res);
    n_lines = 0;
    odin3_log_set_sink(sink, NULL);
    odin3_log_set_level(ODIN3_LOG_INFO);
}

void tearDown(void) {
    odin3_budget_test_set_limit(0);
    (void)odin3_pass_set_opt_snapshot(NULL);
    odin3_opt_test_set_hooks(NULL);
    pb_free(&res);
    odin3_log_set_sink(NULL, NULL);
}

static void build(const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
}

static odin3_status run_pass(const char *name, const char *args) {
    n_lines = 0;
    return odin3_design_run_pass(res.design, name, args);
}

static odin3_status parse(const char *pass, odin3_bytes args, odin3_opt_args *out) {
    n_lines = 0;
    return odin3_opt_parse_args(pass, args, out);
}

/* A module with one dead $_NOT_ (its output feeds nothing) beside a live one. */
#define DEAD_TEXT                                                                                  \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\t\n"                            \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"                         \
    "  cell $_NOT_ \\dead\n    connect \\A \\a\n    connect \\Y \\t\n  end\nend\n"

/* --- parse_opts ------------------------------------------------------------------------------ */

/* The flags of args as one mask, one bit per option (no branches). */
enum {
    M_MUX_UNDEF = 1U << 0U,
    M_MUX_BOOL = 1U << 1U,
    M_UNDRIVEN = 1U << 2U,
    M_NOCLKINV = 1U << 3U,
    M_FINE = 1U << 4U,
    M_FULL = 1U << 5U,
    M_KEEPDC = 1U << 6U,
    M_NODFFE = 1U << 7U,
    M_NOSDFF = 1U << 8U,
    M_PURGE = 1U << 9U,
    M_NOMUX = 1U << 10U,
    M_FAST = 1U << 11U,
    M_NOFF = 1U << 12U,
    M_STATS = 1U << 13U,
    M_RESCAN = 1U << 14U
};

static unsigned mask_of(const odin3_opt_args *args) {
    const odin3_opt_opts *opts = &args->opts;
    return (unsigned)opts->mux_undef * M_MUX_UNDEF | (unsigned)opts->mux_bool * M_MUX_BOOL |
           (unsigned)opts->undriven * M_UNDRIVEN | (unsigned)opts->noclkinv * M_NOCLKINV |
           (unsigned)opts->fine * M_FINE | (unsigned)opts->full * M_FULL |
           (unsigned)opts->keepdc * M_KEEPDC | (unsigned)opts->nodffe * M_NODFFE |
           (unsigned)opts->nosdff * M_NOSDFF | (unsigned)opts->purge * M_PURGE |
           (unsigned)opts->nomux * M_NOMUX | (unsigned)opts->fast * M_FAST |
           (unsigned)opts->noff * M_NOFF | (unsigned)args->stats * M_STATS |
           (unsigned)args->full_rescan * M_RESCAN;
}

typedef struct parse_case {
    const char *pass, *args;
    unsigned want;
} parse_case;

enum { M_FULL_X = M_FULL | M_MUX_UNDEF | M_MUX_BOOL | M_UNDRIVEN | M_FINE };

static const parse_case k_parse_ok[] = {
    {"opt", "-full -keepdc", M_FULL_X | M_KEEPDC},
    {"opt_expr", "--full", M_FULL_X},
    {"opt_expr", "-mux_undef --mux_bool -undriven -noclkinv -fine -keepdc",
     M_MUX_UNDEF | M_MUX_BOOL | M_UNDRIVEN | M_NOCLKINV | M_FINE | M_KEEPDC},
    {"opt_reduce", "-full", M_FULL | M_FINE},
    {"opt_reduce", "-fine", M_FINE},
    {"opt_clean", "-purge", M_PURGE},
    {"opt_merge", "-nomux -keepdc", M_NOMUX | M_KEEPDC},
    {"opt_dff", "-nodffe -nosdff -keepdc --opt-stats", M_NODFFE | M_NOSDFF | M_KEEPDC | M_STATS},
    {"opt", "-fast -noff --nodffe -nosdff -purge", M_FAST | M_NOFF | M_NODFFE | M_NOSDFF | M_PURGE},
    {"opt_expr", "--opt-full-rescan", M_RESCAN},
    {"opt", "--opt-full-rescan --opt-stats", M_RESCAN | M_STATS},
    {"opt_muxtree", "", 0},
    {"opt_share", "--opt-stats", M_STATS},
};

static void test_parse_opts(void) {
    for (size_t i = 0; i < sizeof k_parse_ok / sizeof k_parse_ok[0]; i++) {
        odin3_opt_args args;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK, parse(k_parse_ok[i].pass, odin3_bytes_cstr(k_parse_ok[i].args), &args),
            k_parse_ok[i].args);
        TEST_ASSERT_EQUAL_HEX_MESSAGE(k_parse_ok[i].want, mask_of(&args), k_parse_ok[i].args);
    }
}

static void expect_unknown(const char *pass, const char *arg) {
    odin3_opt_args args;
    char want[LINE_MAX_LEN];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want, "%s: unknown argument '%s'", pass, arg) <
                     (int)sizeof want);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, parse(pass, odin3_bytes_cstr(arg), &args),
                                  want);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_ERROR, want));
}

static void test_parse_unknown(void) {
    expect_unknown("opt_dff", "-sat");
    expect_unknown("opt_merge", "-share_all");
    expect_unknown("opt", "-nomux"); /* nomux is internal to opt's first opt_merge */
    expect_unknown("opt_muxtree", "-fine");
    expect_unknown("opt_reduce", "--opt-full-rescan");
    expect_unknown("opt_clean", "-opt-stats"); /* --opt-stats has only the long spelling */
    expect_unknown("opt_share", "-full");
    expect_unknown("opt_clean", "-fast");
    expect_unknown("opt_merge", "bare");
    odin3_opt_args args;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("opt_nosuch", odin3_bytes_cstr(""), &args));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("opt", odin3_bytes_cstr(""), NULL));
}

/* --- stubs ----------------------------------------------------------------------------------- */

/* One line per pass that is still a stub: each later task removes its own. */
static const char *const k_stub_passes[] = {
    "opt", "opt_expr", "opt_muxtree", "opt_reduce", "opt_share", "opt_dff",
};

static void test_stubs(void) {
    build(DEAD_TEXT);
    odin3_strbuf text_before;
    odin3_strbuf_init(&text_before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(res.design, &text_before));
    for (size_t i = 0; i < sizeof k_stub_passes / sizeof k_stub_passes[0]; i++) {
        char want[LINE_MAX_LEN];
        fmt_str(want, sizeof want, "%s: not available yet (2F)", k_stub_passes[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, run_pass(k_stub_passes[i], ""), want);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_lines(ODIN3_LOG_ERROR, want), want);
    }
    odin3_strbuf text_after;
    odin3_strbuf_init(&text_after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(res.design, &text_after));
    TEST_ASSERT_EQUAL_STRING(text_before.data, text_after.data);
    odin3_strbuf_free(&text_before);
    odin3_strbuf_free(&text_after);
    check_full();
}

static void test_call_stub(void) {
    build(DEAD_TEXT);
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_call_req req = {&res.ctx, res.design, ODIN3_OPT_SUB_EXPR, &opts, &counts};
    bool changed = true;
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_call(&req, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_ERROR, "opt_expr: not available yet (2F)"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_call(NULL, &changed));
    req.sub = ODIN3_OPT_N_SUBS;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_call(&req, &changed));
}

/* --- the wrapper ----------------------------------------------------------------------------- */

static uint32_t hook_calls;
static char hook_last[32];

static void on_call(void *user, const char *sub) {
    (void)user;
    hook_calls++;
    fmt_str(hook_last, sizeof hook_last, "%s", sub);
}

static void test_call_changed_and_hook(void) {
    build(DEAD_TEXT);
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_call_req req = {&res.ctx, res.design, ODIN3_OPT_SUB_CLEAN, &opts, &counts};
    odin3_opt_test_hooks hooks = {on_call, NULL};
    odin3_opt_test_set_hooks(&hooks);
    hook_calls = 0;
    bool changed = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(1, counts.clean_cells);
    TEST_ASSERT_EQUAL_UINT32(1, hook_calls);
    TEST_ASSERT_EQUAL_STRING("opt_clean", hook_last);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_FALSE(changed); /* a second call removes nothing */
    TEST_ASSERT_EQUAL_UINT32(2, hook_calls);
    check_full();
}

/* A clean call that only wires a buffer, or only deletes a wire, still reports `changed`. */
static void test_changed_measure(void) {
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    bool changed = false;
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\t\n"
          "  cell $_BUF_ \\b\n    connect \\A \\a\n    connect \\Y \\t\n  end\n"
          "  cell $_NOT_ \\n\n    connect \\A \\t\n    connect \\Y \\y\n  end\nend\n");
    odin3_opt_call_req req = {&res.ctx, res.design, ODIN3_OPT_SUB_CLEAN, &opts, &counts};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(0, counts.clean_cells); /* only the buffer wiring changed */
    pb_free(&res);
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\dead\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    req.ctx = &res.ctx;
    req.design = res.design;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(0, counts.clean_cells); /* only a wire went */
    TEST_ASSERT_EQUAL_UINT32(1, counts.clean_wires);
    check_full();
}

static char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *buf = malloc(1U << 20U);
    TEST_ASSERT_NOT_NULL(buf);
    size_t len = fread(buf, 1, (1U << 20U) - 1U, file);
    buf[len] = '\0';
    (void)fclose(file);
    return buf;
}

static bool exists(const char *path) {
    return access(path, F_OK) == 0;
}

typedef struct file_ref {
    const char *dir;
    const char *name;
} file_ref;

static void path_of(char *dst, const file_ref *ref) {
    int len = snprintf(dst, PATH_LEN, "%s/%s", ref->dir, ref->name);
    TEST_ASSERT_TRUE(len > 0 && len < PATH_LEN);
}

/* The file's text equals want. */
static void expect_file(const file_ref *ref, const odin3_strbuf *want) {
    char path[PATH_LEN];
    path_of(path, ref);
    char *got = slurp(path);
    TEST_ASSERT_EQUAL_STRING(want->data, got);
    free(got);
}

static void remove_file(const file_ref *ref) {
    char path[PATH_LEN];
    path_of(path, ref);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
}

static void expect_absent(const file_ref *ref) {
    char path[PATH_LEN];
    path_of(path, ref);
    TEST_ASSERT_FALSE(exists(path));
}

static void test_snapshot(void) {
    char dir[] = "/tmp/odin3-opt-loop-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    build(DEAD_TEXT);
    odin3_strbuf want_before;
    odin3_strbuf want_after;
    odin3_strbuf_init(&want_before);
    odin3_strbuf_init(&want_after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(res.design, &want_before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_opt_snapshot(dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(res.design, &want_after));
    TEST_ASSERT_TRUE(strcmp(want_before.data, want_after.data) != 0);
    const file_ref before1 = {dir, "0001-opt_clean-before.il"};
    const file_ref after1 = {dir, "0001-opt_clean-after.il"};
    const file_ref before2 = {dir, "0002-opt_clean-before.il"};
    const file_ref after2 = {dir, "0002-opt_clean-after.il"};
    expect_file(&before1, &want_before);
    expect_file(&after1, &want_after);
    /* the second call removes nothing: it writes no files (it takes number 0002) */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    expect_absent(&before2);
    expect_absent(&after2);
    /* setting the directory again restarts the sequence at 0001 (and replaces the old copy) */
    pb_free(&res);
    build(DEAD_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_opt_snapshot(dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    expect_file(&before1, &want_before);
    expect_file(&after1, &want_after);
    expect_absent(&before2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_opt_snapshot(NULL)); /* off: no more files */
    pb_free(&res);
    build(DEAD_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    expect_absent(&before2);
    odin3_strbuf_free(&want_before);
    odin3_strbuf_free(&want_after);
    remove_file(&before1);
    remove_file(&after1);
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir)); /* exactly those two files were there */
}

static void expect_snapshot_warned(void) {
    TEST_ASSERT_TRUE(count_prefix("opt: snapshot skipped: ") >= 1);
}

/* R2: a snapshot that cannot be written warns and the call goes on with its own result. A $_DFF_P_
 * has no RTLIL form: the "before" text cannot be made, the pass still cleans. */
static void test_snapshot_failure(void) {
    char dir[] = "/tmp/odin3-opt-loop-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\t\n  wire \\q\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"
          "  cell $_NOT_ \\dead\n    connect \\A \\a\n    connect \\Y \\t\n  end\n"
          "  cell $_DFF_P_ \\f\n    connect \\C \\a\n    connect \\D \\a\n    connect \\Q \\q\n"
          "  end\nend\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_opt_snapshot(dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    expect_snapshot_warned();
    uint32_t dead_name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res.design, odin3_bytes_cstr("dead"), &dead_name));
    odin3_node_id dead = odin3_module_find_node(res.module, dead_name);
    TEST_ASSERT_FALSE(odin3_node_valid(dead) && odin3_node_live(res.module, dead));
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir)); /* no file was written */
    check_full();
}

/* An unwritable directory: the "after" write fails, the status stays OK. */
static void test_snapshot_unwritable(void) {
    build(DEAD_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_opt_snapshot("/nonexistent-odin3-dir/x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    expect_snapshot_warned();
    check_full();
}

static void test_budget(void) {
    char text[256];
    int len = snprintf(text, sizeof text,
                       "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire width %d \\w\n"
                       "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n",
                       NETS_MANY);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    build(text);
    odin3_budget_test_set_limit(BUDGET_LIMIT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_pass("opt_clean", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_ERROR, "opt_clean needs more than 4096 bytes (the design's budget "
                                        "is 60% of physical memory)"));
    odin3_budget_test_set_limit(0);
    check_full();
}

/* --- processes, stats ------------------------------------------------------------------------- */

static void test_processes_flag(void) {
    static const char *const k_names[] = {"opt",        "opt_expr",  "opt_merge", "opt_muxtree",
                                          "opt_reduce", "opt_share", "opt_dff",   "opt_clean"};
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr(k_names[i]));
        TEST_ASSERT_NOT_NULL_MESSAGE(def, k_names[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ODIN3_PASS_PROCESSES_OK, def->processes, k_names[i]);
        TEST_ASSERT_TRUE(def->help != NULL &&
                         strncmp(def->help, k_names[i], strlen(k_names[i])) == 0);
    }
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\z\n  wire $tmp1\n"
          "  cell $or \\d\n    connect \\A \\a\n    connect \\B \\a\n    connect \\Y $tmp1\n  end\n"
          "  process $p\n    sync always\n      update \\z \\a\n  end\nend\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_clean: module 'm' has processes; skipped"));
    check_full();
}

static void test_stats(void) {
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    n_lines = 0;
    odin3_opt_print_stats(&counts);
    odin3_opt_print_stats(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix("opt: rule"));
    build(DEAD_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_clean", "--opt-stats"));
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix("opt: rule")); /* opt_clean has no rule counters */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("opt_clean", "-sat"));
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_ERROR, "opt_clean: unknown argument '-sat'"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_opts);
    RUN_TEST(test_parse_unknown);
    RUN_TEST(test_stubs);
    RUN_TEST(test_call_stub);
    RUN_TEST(test_call_changed_and_hook);
    RUN_TEST(test_snapshot);
    RUN_TEST(test_snapshot_failure);
    RUN_TEST(test_snapshot_unwritable);
    RUN_TEST(test_changed_measure);
    RUN_TEST(test_budget);
    RUN_TEST(test_processes_flag);
    RUN_TEST(test_stats);
    return UNITY_END();
}
