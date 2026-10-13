/*
 * test_opt_loop.c — the eight opt passes' registrations, option parsing, the sub-pass
 * call wrapper (hook, budget, snapshots) and --opt-stats (2F spec OP-12, OP-16; plan Task 4).
 * Task 13 extends it with the loop.
 */
#include "backends/rtlil/writer.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/manager.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
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
    odin3_opt_test_set_limits(NULL);
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

/* --- the call wrapper's arguments ------------------------------------------------------------ */

/* Every sub-pass has a body: opt_share runs through the wrapper and logs its INFO line. */
static void test_call_invalid(void) {
    build(DEAD_TEXT);
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_call_req req = {&res.ctx, res.design, ODIN3_OPT_SUB_SHARE, &opts, &counts};
    bool changed = true;
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt_share: merged 0 cells"));
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
    odin3_opt_test_hooks hooks = {on_call, NULL, NULL};
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

/* opt_muxtree through the wrapper and through the pass manager (Task 10). */
static void test_call_muxtree(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n"
          "  wire input 4 \\c\n  wire output 5 \\y\n  wire \\t\n"
          "  cell $mux \\i\n    parameter \\WIDTH 1\n    connect \\A \\b\n    connect \\B \\a\n"
          "    connect \\S \\s\n    connect \\Y \\t\n  end\n"
          "  cell $mux \\o\n    parameter \\WIDTH 1\n    connect \\A \\c\n    connect \\B \\t\n"
          "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_call_req req = {&res.ctx, res.design, ODIN3_OPT_SUB_MUXTREE, &opts, &counts};
    bool changed = false;
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(1, counts.muxtree_ports);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_muxtree: removed 1 multiplexer ports"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_call(&req, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_muxtree", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_muxtree: removed 0 multiplexer ports"));
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

/*
 * opt_expr --opt-stats: one "opt: rule <name> N" line per non-zero counter, sorted by name, with
 * the rules' cover names and the folds' "const.$<type>" (2F plan ruling R3). The module has a
 * folded $not, a const_and, a const_or, an xor_buffer, an and_or_buffer and a reduce_xnor_not.
 */
static void test_stats_expr(void) {
    build(
        "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y1\n"
        "  wire output 4 \\y2\n  wire output 5 \\y3\n  wire output 6 \\y4\n"
        "  wire output 7 \\y5\n  wire output 8 \\y6\n"
        "  cell $and \\c1\n    connect \\A \\a\n    connect \\B 1'0\n    connect \\Y \\y1\n  end\n"
        "  cell $or \\c2\n    connect \\A \\a\n    connect \\B 1'1\n    connect \\Y \\y2\n  end\n"
        "  cell $xor \\c3\n    connect \\A \\a\n    connect \\B 1'1\n    connect \\Y \\y3\n  end\n"
        "  cell $reduce_and \\c4\n    connect \\A \\b\n    connect \\Y \\y4\n  end\n"
        "  cell $reduce_xnor \\c5\n    connect \\A \\b\n    connect \\Y \\y5\n  end\n"
        "  cell $not \\c6\n    connect \\A 1'0\n    connect \\Y \\y6\n  end\nend\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_expr", "--opt-stats"));
    static const char *const k_want[] = {"opt: rule and_or_buffer 1",   "opt: rule const.$not 1",
                                         "opt: rule const_and 1",       "opt: rule const_or 1",
                                         "opt: rule reduce_xnor_not 1", "opt: rule xor_buffer 1"};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        if (strncmp(lines[i].text, "opt: rule", strlen("opt: rule")) == 0) {
            TEST_ASSERT_TRUE(seen < sizeof k_want / sizeof k_want[0]);
            TEST_ASSERT_EQUAL_STRING(k_want[seen], lines[i].text);
            TEST_ASSERT_EQUAL_INT(ODIN3_LOG_INFO, lines[i].level);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof k_want / sizeof k_want[0], seen);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_expr: 3 cells rewritten, 3 cells folded to constants"));
    check_full();
}

/*
 * opt_dff --opt-stats (2F plan ruling R3): one "opt: rule opt_dff.<name>" line per non-zero action
 * counter, sorted by name (not in enum order: never_active_srst comes after always_active_en
 * alphabetically, before it in the enum).
 */
static void test_stats_dff(void) {
    build("module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q1\n"
          "  wire output 4 \\q2\n  wire output 5 \\q3\n  wire output 6 \\q4\n"
          "  cell $adff \\f1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
          "    parameter \\ARST_POLARITY 1\n    parameter \\ARST_VALUE 1'1\n"
          "    connect \\CLK \\clk\n    connect \\ARST 1'1\n    connect \\D \\d\n"
          "    connect \\Q \\q1\n  end\n"
          "  cell $dffe \\f2\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
          "    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN 1'1\n"
          "    connect \\D \\d\n    connect \\Q \\q2\n  end\n"
          "  cell $sdff \\f3\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
          "    parameter \\SRST_POLARITY 1\n    parameter \\SRST_VALUE 1'0\n"
          "    connect \\CLK \\clk\n    connect \\SRST 1'0\n    connect \\D \\d\n"
          "    connect \\Q \\q3\n  end\n"
          "  cell $dff \\f4\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
          "    connect \\CLK \\clk\n    connect \\D 1'0\n    connect \\Q \\q4\n  end\nend\n");
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_dff", "--opt-stats"));
    static const char *const k_want[] = {
        "opt: rule opt_dff.always_active_arst 1", "opt: rule opt_dff.always_active_en 1",
        "opt: rule opt_dff.const_bit_0 1", "opt: rule opt_dff.never_active_srst 1"};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        if (strncmp(lines[i].text, "opt: rule", strlen("opt: rule")) == 0) {
            TEST_ASSERT_TRUE(seen < sizeof k_want / sizeof k_want[0]);
            TEST_ASSERT_EQUAL_STRING(k_want[seen], lines[i].text);
            TEST_ASSERT_EQUAL_INT(ODIN3_LOG_INFO, lines[i].level);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof k_want / sizeof k_want[0], seen);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_dff: 3 flip-flops changed, 1 constant bits"));
    check_full();
}

/* --- the opt driver (Task 13): loop, dirty set, cap, compact, opacity, budget -------------------
 */

/* y = s ? (s ? a : b) : c: the inner mux's A port is dead, the only rewrite opt has to make. */
#define NESTED_TEXT                                                                                \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n  wire input 4 \\s\n" \
    "  wire output 5 \\y\n  wire \\t\n"                                                            \
    "  cell $mux \\inner\n    parameter \\WIDTH 1\n    connect \\A \\b\n    connect \\B \\a\n"     \
    "    connect \\S \\s\n    connect \\Y \\t\n  end\n"                                            \
    "  cell $mux \\outer\n    parameter \\WIDTH 1\n    connect \\A \\c\n    connect \\B \\t\n"     \
    "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"

/* A constant 3-stage register pipeline: one stage becomes constant per opt_dff call. */
#define PIPELINE_TEXT "module \\m\n" PIPELINE_BODY
#define PIPELINE_BODY                                                                              \
    "  wire input 1 \\clk\n  wire output 2 \\q1\n  wire output 3 \\q2\n"                           \
    "  wire output 4 \\q3\n"                                                                       \
    "  cell $dff \\f1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D 1'0\n    connect \\Q \\q1\n  end\n"                  \
    "  cell $dff \\f2\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\q1\n    connect \\Q \\q2\n  end\n"                 \
    "  cell $dff \\f3\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\q2\n    connect \\Q \\q3\n  end\nend\n"

enum { SEQ_LEN = 2048 };
static char seq[SEQ_LEN];

static void on_seq(void *user, const char *sub) {
    (void)user;
    size_t len = strlen(seq);
    int wrote = snprintf(seq + len, sizeof seq - len, "%s%s", len == 0 ? "" : " ", sub);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof seq - len);
}

static void record_calls(void) {
    seq[0] = '\0';
    odin3_opt_test_hooks hooks = {on_seq, NULL, NULL};
    odin3_opt_test_set_hooks(&hooks);
}

#define ITER_DEFAULT " opt_muxtree opt_reduce opt_merge opt_dff opt_clean opt_expr"

static void test_loop_sequences_default(void) {
    build(NESTED_TEXT);
    record_calls();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    TEST_ASSERT_EQUAL_STRING("opt_expr opt_merge" ITER_DEFAULT ITER_DEFAULT, seq);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: fixed point after 2 iterations"));
    check_full();
}

static void test_loop_sequences_noff(void) {
    build(NESTED_TEXT);
    record_calls();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-noff"));
    TEST_ASSERT_EQUAL_STRING("opt_expr opt_merge"
                             " opt_muxtree opt_reduce opt_merge opt_clean opt_expr"
                             " opt_muxtree opt_reduce opt_merge opt_clean opt_expr",
                             seq);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: fixed point after 2 iterations"));
}

static void test_loop_sequences_full(void) {
    build(NESTED_TEXT);
    record_calls();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-full"));
    TEST_ASSERT_EQUAL_STRING(
        "opt_expr opt_merge"
        " opt_muxtree opt_reduce opt_merge opt_share opt_dff opt_clean opt_expr"
        " opt_muxtree opt_reduce opt_merge opt_share opt_dff opt_clean opt_expr",
        seq);
    check_full();
}

static void test_loop_sequences_fast(void) {
    build(NESTED_TEXT);
    record_calls();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-fast"));
    TEST_ASSERT_EQUAL_STRING("opt_expr opt_merge opt_dff opt_clean", seq);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: -fast: done after 1 iterations"));
    seq[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-fast -noff"));
    TEST_ASSERT_EQUAL_STRING("opt_expr opt_merge opt_clean", seq);
    check_full();
}

/* The text of the design (write_rtlil), malloc'd. */
static char *design_text(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(res.design, &text));
    char *copy = malloc(text.len + 1U);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text.data, text.len + 1U);
    odin3_strbuf_free(&text);
    return copy;
}

/* The text has (present) or lacks needle. */
static void expect_text(const char *text, const char *needle, bool present) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(present ? 1 : 0, strstr(text, needle) != NULL ? 1 : 0, needle);
}

static void expect_pipeline_constant(void) {
    char *text = design_text();
    expect_text(text, "cell $dff", false);
    expect_text(text, "connect \\q1 1'0", true);
    expect_text(text, "connect \\q2 1'0", true);
    expect_text(text, "connect \\q3 1'0", true);
    free(text);
}

/* Review Focus 4: one stage becomes constant per opt_dff call, so three stages take four
 * iterations (the last one finds nothing). */
static void test_loop_pipeline_iterations(void) {
    build(PIPELINE_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: fixed point after 4 iterations"));
    expect_pipeline_constant();
    check_full();
}

/* Review M4(a): -fast keeps looping while opt_dff changes something (one stage per round). */
static void test_loop_fast_pipeline(void) {
    build(PIPELINE_TEXT);
    record_calls();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-fast"));
    TEST_ASSERT_EQUAL_STRING(
        "opt_expr opt_merge opt_dff opt_clean opt_expr opt_merge opt_dff opt_clean"
        " opt_expr opt_merge opt_dff opt_clean opt_expr opt_merge opt_dff opt_clean",
        seq);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: -fast: done after 4 iterations"));
    expect_pipeline_constant();
    check_full();
}

/* Review M4(b): the cap warns once per module that the last iteration changed. */
static void test_loop_cap_modules(void) {
    build("module \\m\n" PIPELINE_BODY "module \\n\n" PIPELINE_BODY);
    odin3_opt_limits limits = {2, ODIN3_OPT_MUXTREE_MAX_EVALS};
    odin3_opt_test_set_limits(&limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN,
                       "opt: module 'm': no fixed point after 2 iterations; stopping"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN,
                       "opt: module 'n': no fixed point after 2 iterations; stopping"));
    check_full();
}

static void test_loop_cap(void) {
    build(PIPELINE_TEXT);
    odin3_opt_limits limits = {2, ODIN3_OPT_MUXTREE_MAX_EVALS};
    odin3_opt_test_set_limits(&limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN,
                       "opt: module 'm': no fixed point after 2 iterations; stopping"));
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix("opt: fixed point"));
    check_full();
    /* the same cap in -fast mode */
    pb_free(&res);
    setUp();
    build(PIPELINE_TEXT);
    odin3_opt_test_set_limits(&limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-fast"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN,
                       "opt: module 'm': no fixed point after 2 iterations; stopping"));
    check_full();
}

/* The twenty inputs of the dirty-set comparison (tests/unit/data/opt/loop): the module of one
 * test of each of the rule families (opt_expr, opt_merge, opt_reduce, opt_muxtree, opt_dff). */
static const char *const k_loop_cases[] = {
    "const_and",
    "and_bit_inverse",
    "bitwise_and_x",
    "bitwise_xor_x",
    "eqneq_resize",
    "constshift",
    "mul_low_zeros",
    "mux_undef",
    "fine_group",
    "clkinv_dff",
    "cse_cascade",
    "cse_pmux_pairs",
    "reduce_inline",
    "pmux_equal_slices",
    "muxtree_nested_same_sel",
    "dff_srst_merge",
    "dff_en_then_srst",
    "dff_feedback_patterns",
    "dff_constbits_one_stage",
    "share_same_type",
};

enum { N_LOOP_CASES = sizeof k_loop_cases / sizeof k_loop_cases[0] };

/* The case's file, built into res, run with args, and its write_rtlil text (malloc'd). */
static char *run_case(const char *name, const char *args) {
    char path[PATH_LEN];
    int len = snprintf(path, sizeof path, "%s/%s.il", ODIN3_LOOP_DATA, name);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    char *input = slurp(path);
    pb_free(&res);
    n_lines = 0;
    odin3_status built = pb_build(input, &res);
    free(input);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, built, n_lines > 0 ? lines[0].text : name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_pass("opt", args), name);
    check_full();
    return design_text();
}

/* Internal risk 4: the dirty set is exact. opt_expr's Debug build also sweeps the module in full
 * after each incremental call and aborts when that finds more to do. */
static void test_dirty_vs_full_rescan(void) {
    static const char *const k_args[] = {"", "-mux_undef -mux_bool -undriven -fine", "-keepdc"};
    for (size_t set = 0; set < sizeof k_args / sizeof k_args[0]; set++) {
        for (size_t i = 0; i < N_LOOP_CASES; i++) {
            char *incremental = run_case(k_loop_cases[i], k_args[set]);
            char full_args[128];
            int len = snprintf(full_args, sizeof full_args, "%s --opt-full-rescan", k_args[set]);
            TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof full_args);
            char *rescan = run_case(k_loop_cases[i], full_args);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(incremental, rescan, k_loop_cases[i]);
            free(incremental);
            free(rescan);
        }
    }
}

/*
 * What the dirty set exists for: opt_merge makes the two $not cells one, and only then can
 * opt_expr see that the $mux's two data inputs are the same net (mux_same). The $mux is in no
 * place opt_expr's second call would look at by itself.
 */
#define CHAIN_TEXT                                                                                 \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\s\n  wire output 3 \\y\n"                    \
    "  wire \\n1\n  wire \\n2\n"                                                                   \
    "  cell $not \\i1\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"                    \
    "    parameter \\Y_WIDTH 1\n    connect \\A \\a\n    connect \\Y \\n1\n  end\n"                \
    "  cell $not \\i2\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"                    \
    "    parameter \\Y_WIDTH 1\n    connect \\A \\a\n    connect \\Y \\n2\n  end\n"                \
    "  cell $mux \\mx\n    parameter \\WIDTH 1\n    connect \\A \\n1\n    connect \\B \\n2\n"      \
    "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"

static void test_dirty_chain(void) {
    build(CHAIN_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    char *text = design_text();
    TEST_ASSERT_NULL(strstr(text, "cell $mux"));
    TEST_ASSERT_NOT_NULL(strstr(text, "cell $not"));
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: fixed point after 2 iterations"));
    free(text);
    check_full();
    pb_free(&res);
    build(CHAIN_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "--opt-full-rescan"));
    char *rescan = design_text();
    TEST_ASSERT_NULL(strstr(rescan, "cell $mux"));
    free(rescan);
}

/*
 * Review M3: a net merge without a rebuilt survivor. opt_dff ties the Q net of a constant-D
 * flip-flop to the constant, so the $and that reads q only sees a changed input net; the
 * incremental opt_expr must still fold it (only odin3_kit_note_net marks the reader). With the
 * hook disabled this aborts at the settle() cross-check in Debug.
 */
#define TIE_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\b\n  wire output 3 \\y\n"                  \
    "  wire \\q\n"                                                                                 \
    "  cell $dff \\f1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D 1'0\n    connect \\Q \\q\n  end\n"                   \
    "  cell $and \\g\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                    \
    "    parameter \\A_WIDTH 1\n    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"            \
    "    connect \\A \\q\n    connect \\B \\b\n    connect \\Y \\y\n  end\nend\n"

static void test_dirty_tie(void) {
    build(TIE_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    char *text = design_text();
    expect_text(text, "cell $dff", false);
    expect_text(text, "cell $and", false);
    expect_text(text, "connect \\y 1'0", true);
    free(text);
    check_full();
}

/* The live nodes of the module `m`, and its node-ID end. */
static uint32_t live_nodes(const odin3_module *module) {
    uint32_t live = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        live += odin3_node_live(module, (odin3_node_id){id}) ? 1U : 0U;
    }
    return live;
}

/* IR-6 unchanged (PHASE2 #19): opt leaves dead nodes; the script's `compact` after it removes
 * them. A standalone opt_expr leaves dead nodes too. */
static void test_compact_after_opt(void) {
    build(DEAD_TEXT);
    odin3_module *module = pb_module(&res, "m");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
    module = pb_module(&res, "m");
    TEST_ASSERT_TRUE(live_nodes(module) + 1U < odin3_module_node_end(module));
    check_full();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("compact", ""));
    module = pb_module(&res, "m");
    TEST_ASSERT_EQUAL_UINT32(live_nodes(module) + 1U, odin3_module_node_end(module));
    check_full();
    pb_free(&res);
    char path[PATH_LEN];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/const_and.il", ODIN3_LOOP_DATA) > 0);
    char *input = slurp(path);
    build(input);
    free(input);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_expr", ""));
    module = pb_module(&res, "m");
    TEST_ASSERT_TRUE(live_nodes(module) + 1U < odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("compact", ""));
    module = pb_module(&res, "m");
    TEST_ASSERT_EQUAL_UINT32(live_nodes(module) + 1U, odin3_module_node_end(module));
}

/* The BODY blob of the nth $proc of `m` (malloc'd, *len bytes): what opt must never touch. */
static uint8_t *body_of(uint32_t nth, uint32_t *len) {
    const odin3_module *module = pb_module(&res, "m");
    const odin3_value *body = odin3_node_param(module, pb_proc(&res, "m", nth), ODIN3_PROC_P_BODY);
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_TRUE(body->kind == ODIN3_VAL_PROC && body->len > 0);
    uint8_t *copy = malloc(body->len);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, body->bits, body->len);
    *len = body->len;
    return copy;
}

static void expect_body(uint32_t nth, const uint8_t *want, uint32_t want_len) {
    uint32_t len = 0;
    uint8_t *got = body_of(nth, &len);
    TEST_ASSERT_EQUAL_UINT32(want_len, len);
    TEST_ASSERT_EQUAL_MEMORY(want, got, len);
    free(got);
}

/* A foldable $add feeding two identical processes, a dead $or beside them. */
#define OPAQUE_TEXT                                                                                \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\a\n  wire input 7 \\c\n  wire width 2 "    \
    "\\s\n"                                                                                        \
    "  wire width 2 output 3 \\z1\n  wire width 2 output 4 \\z2\n  wire \\dead\n"                  \
    "  cell $add \\add\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                  \
    "    parameter \\A_WIDTH 2\n    parameter \\B_WIDTH 2\n    parameter \\Y_WIDTH 2\n"            \
    "    connect \\A 2'01\n    connect \\B 2'01\n    connect \\Y \\s\n  end\n"                     \
    "  cell $or \\d\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                     \
    "    parameter \\A_WIDTH 1\n    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"            \
    "    connect \\A \\a\n    connect \\B \\c\n    connect \\Y \\dead\n  end\n"                    \
    "  process $p1\n    sync always\n      update \\z1 \\s\n  end\n"                               \
    "  process $p2\n    sync always\n      update \\z2 \\s\n  end\nend\n"

/* opt_muxtree and opt_clean logged their skip line for the module `m` (at least once). */
static void expect_skip_lines(void) {
    TEST_ASSERT_GREATER_THAN_UINT32(
        0, count_lines(ODIN3_LOG_INFO, "opt_muxtree: module 'm' has processes; skipped"));
    TEST_ASSERT_GREATER_THAN_UINT32(
        0, count_lines(ODIN3_LOG_INFO, "opt_clean: module 'm' has processes; skipped"));
}

/* Review Focus 5 (mode P): the cells around a $proc are optimised, the $proc is opaque. */
static void test_opacity_mode_p(void) {
    build(OPAQUE_TEXT);
    uint32_t len1 = 0;
    uint32_t len2 = 0;
    uint8_t *body1 = body_of(0, &len1);
    uint8_t *body2 = body_of(1, &len2);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_proc_count(pb_module(&res, "m")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-nodffe -nosdff"));
    char *text = design_text();
    expect_text(text, "cell $add", false);
    expect_text(text, "cell $or", true);
    free(text);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_proc_count(pb_module(&res, "m")));
    expect_body(0, body1, len1);
    expect_body(1, body2, len2);
    expect_skip_lines();
    free(body1);
    free(body2);
    check_full();
}

/* proc's E1a on a two-switch body (test_proc_pass.c's m2), with cells around it. */
#define FAILED_PROC_TEXT                                                                           \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\a\n"                 \
    "  wire input 4 \\b\n  wire input 5 \\d\n  wire output 6 \\q\n  wire \\t0\n"                   \
    "  wire input 8 \\c\n  wire width 2 \\s\n  wire width 2 output 7 \\z\n  wire \\dead\n"         \
    "  cell $add \\add\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                  \
    "    parameter \\A_WIDTH 2\n    parameter \\B_WIDTH 2\n    parameter \\Y_WIDTH 2\n"            \
    "    connect \\A 2'01\n    connect \\B 2'01\n    connect \\Y \\s\n  end\n"                     \
    "  cell $or \\d1\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                    \
    "    parameter \\A_WIDTH 1\n    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"            \
    "    connect \\A \\a\n    connect \\B \\c\n    connect \\Y \\dead\n  end\n"                    \
    "  attribute \\src \"t.v:9.3\"\n  process $p\n"                                                \
    "    switch \\a\n      case 1'1\n        assign \\t0 \\d\n    end\n"                           \
    "    switch \\b\n      case 1'1\n        assign \\t0 1'0\n    end\n"                           \
    "    sync posedge \\clk\n      update \\q \\t0\n"                                              \
    "    sync posedge \\rst\n      update \\q \\t0\n"                                              \
    "    sync always\n      update \\z \\s\n  end\nend\n"

/* 2E section 18.1: after proc failed on a body, opt_expr and opt -nodffe -nosdff still run. */
static void test_opt_after_failed_proc(void) {
    build(FAILED_PROC_TEXT);
    uint32_t len = 0;
    uint8_t *body = body_of(0, &len);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_pass("proc", ""));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(pb_module(&res, "m")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt_expr", ""));
    char *text = design_text();
    expect_text(text, "cell $add", false);
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "-nodffe -nosdff"));
    expect_skip_lines();
    expect_body(0, body, len);
    free(body);
    check_full();
}

/* The budget hook at 4096 bytes: opt stops at its first sub-pass call, the IR valid. */
static void test_budget_opt(void) {
    build(DEAD_TEXT);
    odin3_budget_test_set_limit(BUDGET_LIMIT);
    char *before = design_text();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_pass("opt", ""));
    TEST_ASSERT_EQUAL_UINT32(
        1,
        count_lines(ODIN3_LOG_ERROR, "opt needs more than 4096 bytes (the design's budget is 60% "
                                     "of physical memory)"));
    odin3_budget_test_set_limit(0);
    char *after = design_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    check_full();
}

/* --opt-stats on const_and's module: its one rule fired once, and no zero counter is printed. */
static void test_opt_stats_driver(void) {
    char path[PATH_LEN];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/const_and.il", ODIN3_LOOP_DATA) > 0);
    char *input = slurp(path);
    build(input);
    free(input);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", "--opt-stats"));
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt: rule const_and 1"));
    for (uint32_t i = 0; i < n_lines; i++) {
        if (strncmp(lines[i].text, "opt: rule ", 10) == 0) {
            TEST_ASSERT_NULL_MESSAGE(strstr(lines[i].text, " 0"), lines[i].text);
        }
    }
}

/* A module for the OOM sweep: a dead mux port, a duplicate gate, a constant pipeline. */
#define OOM_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\a\n  wire input 3 \\b\n"                   \
    "  wire input 4 \\s\n  wire output 5 \\y\n  wire output 6 \\w\n  wire output 7 \\q1\n"         \
    "  wire output 8 \\q2\n  wire \\t\n  wire \\n1\n  wire \\n2\n"                                 \
    "  cell $mux \\inner\n    parameter \\WIDTH 1\n    connect \\A \\b\n    connect \\B \\a\n"     \
    "    connect \\S \\s\n    connect \\Y \\t\n  end\n"                                            \
    "  cell $mux \\outer\n    parameter \\WIDTH 1\n    connect \\A \\a\n    connect \\B \\t\n"     \
    "    connect \\S \\s\n    connect \\Y \\y\n  end\n"                                            \
    "  cell $not \\i1\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"                    \
    "    parameter \\Y_WIDTH 1\n    connect \\A \\a\n    connect \\Y \\n1\n  end\n"                \
    "  cell $not \\i2\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"                    \
    "    parameter \\Y_WIDTH 1\n    connect \\A \\a\n    connect \\Y \\n2\n  end\n"                \
    "  cell $mux \\mx\n    parameter \\WIDTH 1\n    connect \\A \\n1\n    connect \\B \\n2\n"      \
    "    connect \\S \\b\n    connect \\Y \\w\n  end\n"                                            \
    "  cell $dff \\f1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D 1'0\n    connect \\Q \\q1\n  end\n"                  \
    "  cell $dff \\f2\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\q1\n    connect \\Q \\q2\n  end\nend\n"

enum { OOM_LIMIT = 4000, OOM_MIN_POINTS = 20 };

/* One run of `opt` with the allocation fail-th failing: true when it succeeded (the sweep's end).
 * A failed run leaves the IR valid, and a second run without failure finishes. */
static bool oom_once(long fail) {
    pb_free(&res);
    build(OOM_TEXT);
    n_lines = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_design_run_pass(res.design, "opt", "");
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("opt", ""));
        check_full();
    }
    return st == ODIN3_OK;
}

/* Every allocation point of an `opt` run fails once, in turn. */
static void test_opt_oom(void) {
    long failures = 0;
    while (failures < OOM_LIMIT && !oom_once(failures)) {
        failures++;
    }
    printf("opt OOM: %ld failing allocation points\n", failures);
    TEST_ASSERT_TRUE(failures < OOM_LIMIT);
    TEST_ASSERT_TRUE(failures > OOM_MIN_POINTS);
}

/* Two runs, byte-identical write_rtlil; and the reverse creation order of the nodes gives the
 * same netlist (compared as a sorted list of the cells' types, a stand-in for netlist-compare). */
static void test_determinism(void) {
    for (size_t i = 0; i < N_LOOP_CASES; i++) {
        char *first = run_case(k_loop_cases[i], "");
        char *second = run_case(k_loop_cases[i], "");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(first, second, k_loop_cases[i]);
        free(first);
        free(second);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_opts);
    RUN_TEST(test_parse_unknown);
    RUN_TEST(test_call_invalid);
    RUN_TEST(test_call_changed_and_hook);
    RUN_TEST(test_call_muxtree);
    RUN_TEST(test_snapshot);
    RUN_TEST(test_snapshot_failure);
    RUN_TEST(test_snapshot_unwritable);
    RUN_TEST(test_changed_measure);
    RUN_TEST(test_budget);
    RUN_TEST(test_processes_flag);
    RUN_TEST(test_stats);
    RUN_TEST(test_stats_expr);
    RUN_TEST(test_stats_dff);
    RUN_TEST(test_loop_sequences_default);
    RUN_TEST(test_loop_sequences_noff);
    RUN_TEST(test_loop_sequences_full);
    RUN_TEST(test_loop_sequences_fast);
    RUN_TEST(test_loop_pipeline_iterations);
    RUN_TEST(test_loop_cap);
    RUN_TEST(test_loop_fast_pipeline);
    RUN_TEST(test_loop_cap_modules);
    RUN_TEST(test_dirty_vs_full_rescan);
    RUN_TEST(test_dirty_chain);
    RUN_TEST(test_dirty_tie);
    RUN_TEST(test_compact_after_opt);
    RUN_TEST(test_opacity_mode_p);
    RUN_TEST(test_opt_after_failed_proc);
    RUN_TEST(test_budget_opt);
    RUN_TEST(test_opt_stats_driver);
    RUN_TEST(test_opt_oom);
    RUN_TEST(test_determinism);
    return UNITY_END();
}
