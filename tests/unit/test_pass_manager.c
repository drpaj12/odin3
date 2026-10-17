/*
 * test_pass_manager.c — unit tests for the pass manager, the built-in passes and pass scripts.
 */
#include "ast/ast.h"
#include "ast/print.h"
#include "ast/symtab.h"
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_test.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(ODIN3_LIB_DIR) || !defined(ODIN3_TECHLIB_FIXTURES)
#error "ODIN3_LIB_DIR and ODIN3_TECHLIB_FIXTURES must name lib/ and tests/golden/techlib"
#endif
#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif

enum { LOG_CAP = 1 << 16, PATH_BUF = 512, OOM_SWEEP = 4000 };

static const char *const BLIF_PATH = "odin3_pm_test.blif";
static const char *const OUT_PATH = "odin3_pm_test_out.blif";
static const char *const SCRIPT_PATH = "odin3_pm_test.o3";
static const char *const SPACED_PATH = "odin3 pm test;#.blif";

/* leaf is the first model (the BLIF top) but root instantiates it: the auto top is root. */
static const char *const LEAF_ROOT = ".model leaf\n.inputs i\n.outputs o\n.names i o\n1 1\n.end\n"
                                     ".model root\n.inputs x\n.outputs y\n"
                                     ".subckt leaf i=x o=y\n.end\n";
static const char *const TWO_TOPS = ".model a\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n"
                                    ".model b\n.inputs i\n.outputs o\n.names i o\n1 1\n.end\n";
static const char *const CYCLE = ".model a\n.inputs x\n.outputs y\n.subckt b i=x o=y\n.end\n"
                                 ".model b\n.inputs i\n.outputs o\n.subckt a x=i y=o\n.end\n";

/* A black box whose output z is left open: check rule 11 warns, nothing fails. */
static const char *const OPEN_OUTPUT = ".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n"
                                       ".subckt bb x=a\n.end\n"
                                       ".model bb\n.inputs x\n.outputs z\n.blackbox\n.end\n";

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;
static odin3_pass_options saved_opts;

static const char *level_tag(odin3_log_level level) {
    switch (level) {
    case ODIN3_LOG_ERROR:
        return "E ";
    case ODIN3_LOG_WARN:
        return "W ";
    default:
        return "I ";
    }
}

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%s%s\n", level_tag(level), msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static void reset_log(void) {
    log_text[0] = '\0';
    log_len = 0;
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

/* Writes text to file (fopen's result) and closes it. */
static void put_text(FILE *file, const char *text) {
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, (int)fwrite(text, strlen(text), 1, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static odin3_status run(const char *name, const char *args) {
    return odin3_pass_run(design, name, odin3_bytes_cstr(args));
}

static odin3_status run_p(const char *text) {
    return odin3_pass_run_script(design, odin3_bytes_cstr(text),
                                 (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND});
}

/* The name of the design's top module, "" when none is set. */
static const char *top_name(void) {
    odin3_module_id top = odin3_design_top(design);
    if (!odin3_module_valid(top)) {
        return "";
    }
    return odin3_strtab_get(odin3_design_strtab(design),
                            odin3_module_name(odin3_module_get(design, top)));
}

static void read_text(const char *text) {
    put_text(fopen(BLIF_PATH, "w"), text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_blif", BLIF_PATH));
}

void setUp(void) {
    reset_log();
    odin3_log_set_sink(capture_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    saved_opts = odin3_pass_get_options();
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_pass_set_options(saved_opts);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    (void)remove(BLIF_PATH);
    (void)remove(OUT_PATH);
    (void)remove(SCRIPT_PATH);
    (void)remove(SPACED_PATH);
    /* last: after an ignored test, a Unity assertion ends tearDown at once */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
}

/* --- test passes ----------------------------------------------------------------------------- */

static int count_calls;
static odin3_pass_ctx count_ctx;
static char count_args[PATH_BUF];
static int count_marker; /* COUNT_PASS's user pointer */
static const void *count_user;

static odin3_status count_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                               void *user) {
    TEST_ASSERT_TRUE(des == ctx->design);
    count_calls++;
    count_ctx = *ctx;
    count_user = user;
    (void)snprintf(count_args, sizeof count_args, "%.*s", (int)args.len, (const char *)args.ptr);
    return ODIN3_OK;
}

static const odin3_pass_def COUNT_PASS = {"t_count",  "t_count [args]: counts its calls",
                                          count_pass, &count_marker,
                                          false,      ODIN3_PASS_PROCESSES_REFUSE};

/* Breaks rule 4: a second driver on net 1 of module 1 (an input port's net). */
static odin3_status break_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                               void *user) {
    (void)user;
    (void)ctx;
    (void)args;
    odin3_module *module = odin3_module_get(des, (odin3_module_id){1});
    return odin3_ir_test_corrupt(module, (odin3_ir_test_target){ODIN3_IR_TEST_MULTI_DRIVER, 1});
}

static const odin3_pass_def BREAK_PASS = {"t_break", "t_break: breaks rule 4",   break_pass, NULL,
                                          false,     ODIN3_PASS_PROCESSES_REFUSE};

static odin3_status fail_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                              void *user) {
    (void)user;
    (void)ctx;
    (void)des;
    (void)args;
    return ODIN3_ERR_IO;
}

static const odin3_pass_def FAIL_PASS = {"t_fail", "t_fail: always fails",     fail_pass, NULL,
                                         false,    ODIN3_PASS_PROCESSES_REFUSE};

/* Breaks rule 4 like t_break, then fails: the check after it still runs. */
static odin3_status break_fail_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                                    void *user) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, break_pass(ctx, des, args, user));
    return ODIN3_ERR_IO;
}

static const odin3_pass_def BREAK_FAIL_PASS = {"t_break_fail",  "t_break_fail: breaks, then fails",
                                               break_fail_pass, NULL,
                                               false,           ODIN3_PASS_PROCESSES_REFUSE};

static void register_test_passes(void) {
    static bool done = false;
    if (!done) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&COUNT_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&BREAK_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&FAIL_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&BREAK_FAIL_PASS));
        done = true;
    }
    count_calls = 0;
    count_args[0] = '\0';
}

/* --- registry -------------------------------------------------------------------------------- */

static void assert_builtin(const char *name) {
    const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr(name));
    TEST_ASSERT_NOT_NULL_MESSAGE(def, name);
    TEST_ASSERT_EQUAL_STRING(name, def->name);
    TEST_ASSERT_NOT_NULL(def->help);
}

static void test_builtins_registered(void) {
    static const char *const names[] = {"read_blif", "read_techlib", "write_blif", "check",
                                        "compact",   "stats",        "hierarchy"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        assert_builtin(names[i]);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sizeof names / sizeof names[0], odin3_pass_count());
    TEST_ASSERT_EQUAL_STRING("read_blif", odin3_pass_at(0)->name);
    TEST_ASSERT_NULL(odin3_pass_at(odin3_pass_count()));
    TEST_ASSERT_NULL(odin3_pass_find(odin3_bytes_cstr("nope")));
}

static void test_register_and_run(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_PTR(&COUNT_PASS, odin3_pass_find(odin3_bytes_cstr("t_count")));
    TEST_ASSERT_EQUAL_PTR(&COUNT_PASS, odin3_pass_at(odin3_pass_count() - 4));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_count", "a  b"));
    TEST_ASSERT_EQUAL_INT(1, count_calls);
    TEST_ASSERT_EQUAL_STRING("a  b", count_args);
    TEST_ASSERT_EQUAL_PTR(&count_marker, count_user); /* the definition's user pointer */
    TEST_ASSERT_TRUE(log_has("I pass t_count: "));
    TEST_ASSERT_TRUE(log_has(" ms\n"));
}

static void test_register_rejects(void) {
    register_test_passes();
    static const odin3_pass_def dup_builtin = {"check", "x",   count_pass,
                                               NULL,    false, ODIN3_PASS_PROCESSES_REFUSE};
    static const odin3_pass_def blank = {"a b", "x",   count_pass,
                                         NULL,  false, ODIN3_PASS_PROCESSES_REFUSE};
    static const odin3_pass_def semi = {"a;b", "x",   count_pass,
                                        NULL,  false, ODIN3_PASS_PROCESSES_REFUSE};
    static const odin3_pass_def quote = {
        "a\"b", "x", count_pass, NULL, false, ODIN3_PASS_PROCESSES_REFUSE}; /* opens a quote */
    static const odin3_pass_def empty = {"",   "x",   count_pass,
                                         NULL, false, ODIN3_PASS_PROCESSES_REFUSE};
    static const odin3_pass_def no_run = {"t_no_run", "x",   NULL,
                                          NULL,       false, ODIN3_PASS_PROCESSES_REFUSE};
    static const odin3_pass_def no_help = {"t_no_help", NULL,  count_pass,
                                           NULL,        false, ODIN3_PASS_PROCESSES_REFUSE};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&COUNT_PASS));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&dup_builtin));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&blank));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&semi));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&quote));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&empty));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&no_run));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&no_help));
    TEST_ASSERT_TRUE(log_has("already registered"));
}

static void test_unknown_pass(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("nope", ""));
    TEST_ASSERT_TRUE(log_has("E unknown pass 'nope'"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run(NULL, "check", (odin3_bytes){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run(design, NULL, (odin3_bytes){0}));
}

/* --- pass runs: provenance, check, failure ---------------------------------------------------- */

static void test_run_opens_named_provenance_run(void) {
    register_test_passes();
    uint32_t before = odin3_passrun_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_count", ""));
    TEST_ASSERT_EQUAL_UINT32(before + 1, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_UINT32(before, count_ctx.run.v);
    TEST_ASSERT_EQUAL_UINT32(0, count_ctx.op);
    TEST_ASSERT_TRUE(count_ctx.design == design);
    uint32_t name = odin3_passrun_name(design, count_ctx.run);
    TEST_ASSERT_EQUAL_STRING("t_count", odin3_strtab_get(odin3_design_strtab(design), name));
}

/* read_blif's records belong to the manager's run: one run named read_blif, not two. */
static void test_read_blif_uses_the_pass_run(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_blif", ODIN3_BLIF_FIXTURES "/hand_body.blif"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_passrun_end(design));
    odin3_module *top = odin3_module_get(design, (odin3_module_id){1});
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_module_prov(top));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT32(1, rec->run.v);
}

static void test_failing_post_check(void) {
    register_test_passes();
    odin3_pass_set_options((odin3_pass_options){.check = true});
    read_text(LEAF_ROOT);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run("t_break", ""));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E check: leaf: rule 4:"), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("E pass t_break: check after the pass failed"), log_text);
}

static void test_failing_pre_check_skips_the_pass(void) {
    register_test_passes();
    odin3_pass_set_options((odin3_pass_options){.check = true});
    read_text(LEAF_ROOT);
    odin3_module *module = odin3_module_get(design, (odin3_module_id){1});
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_ir_test_corrupt(module, (odin3_ir_test_target){ODIN3_IR_TEST_MULTI_DRIVER, 1}));
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run("t_count", ""));
    TEST_ASSERT_EQUAL_INT(0, count_calls);
    TEST_ASSERT_TRUE(log_has("E check: leaf: rule 4:"));
    TEST_ASSERT_TRUE(log_has("E pass t_count: check before the pass failed"));
}

/* Debug builds check around every pass even with the check option off (CLAUDE.md rule 3). */
static void test_debug_always_checks(void) {
#ifdef NDEBUG
    TEST_IGNORE_MESSAGE("Release build: checking follows the check option");
#else
    register_test_passes();
    odin3_pass_set_options((odin3_pass_options){.check = false});
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run("t_break", ""));
#endif
}

/* Count of needle in the captured log. */
static uint32_t log_count_of(const char *needle) {
    uint32_t count = 0;
    for (const char *at = strstr(log_text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* The pre-check reports only errors: a check warning is printed once per pass (by the check
 * after it), not twice; the log level is restored after the pre-check. */
static void test_check_warnings_once_per_pass(void) {
    read_text(OPEN_OUTPUT);
    odin3_pass_set_options((odin3_pass_options){.check = true});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    TEST_ASSERT_EQUAL_UINT32(1, log_count_of("W check: top: rule 11:"));
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_DEBUG, odin3_log_get_level());
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("stats; stats"));
    TEST_ASSERT_EQUAL_UINT32(2, log_count_of("W check: top: rule 11:"));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_log_set_level(ODIN3_LOG_ERROR)); /* a quieter caller level is kept, not raised */
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    TEST_ASSERT_EQUAL_UINT32(0, log_count_of("W check:"));
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_ERROR, odin3_log_get_level());
}

static void test_no_check_when_off_in_release(void) {
#ifndef NDEBUG
    TEST_IGNORE_MESSAGE("Debug build: always checks");
#else
    register_test_passes();
    odin3_pass_set_options((odin3_pass_options){.check = false});
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_break", ""));
#endif
}

static void test_failing_pass(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("t_fail", ""));
    TEST_ASSERT_TRUE(log_has("E pass t_fail: failed: ODIN3_ERR_IO"));
}

/* --- built-in passes ---------------------------------------------------------------------------
 */

static void test_read_blif_records_top(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_STRING("leaf", top_name());
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_top(design).v);
}

static void test_read_blif_bad_args(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("read_blif", ""));
    TEST_ASSERT_TRUE(log_has("E read_blif: expects one path"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("read_blif", "a.blif b.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("read_blif", "no_such_file.blif"));
}

static void test_write_blif_round_trip(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_blif", ODIN3_BLIF_FIXTURES "/hand_body.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_blif", OUT_PATH));
    odin3_design *back = odin3_design_create();
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(back, OUT_PATH));
    TEST_ASSERT_EQUAL_UINT32(odin3_design_module_end(design), odin3_design_module_end(back));
    odin3_design_destroy(back);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_blif", ""));
    TEST_ASSERT_TRUE(log_has("E write_blif: expects one path"));
}

static void test_check_pass(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("check", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("check", "--fast"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("check", "--slow"));
    TEST_ASSERT_TRUE(log_has("E check: unknown argument '--slow'"));
    odin3_module *module = odin3_module_get(design, (odin3_module_id){1});
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_ir_test_corrupt(module, (odin3_ir_test_target){ODIN3_IR_TEST_MULTI_DRIVER, 1}));
    /* the pass itself (Release) or the manager's pre-check (Debug) finds it */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run("check", "--fast"));
    TEST_ASSERT_TRUE(log_has("E check: leaf: rule 4:"));
}

static void test_compact_pass(void) {
    read_text(LEAF_ROOT);
    odin3_module *leaf = odin3_module_get(design, (odin3_module_id){1});
    uint32_t end = odin3_module_node_end(leaf);
    odin3_node_id last = {end - 1}; /* the $sop */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(leaf, last));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("compact", ""));
    TEST_ASSERT_EQUAL_UINT32(end - 1, odin3_module_node_end(leaf));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_tombstone_end(design) - 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("compact", "x"));
}

/* read_techlib registers the library's cells, so a BLIF read afterwards instantiates `multiply`
 * as the library's hard cell; bad arguments and a missing file fail. */
static void test_read_techlib(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("read_techlib", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("read_techlib", ODIN3_LIB_DIR "/none.o3lib"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_techlib " ODIN3_LIB_DIR
                                          "/vtr.o3lib; read_blif " ODIN3_TECHLIB_FIXTURES
                                          "/multiply.parmys.01.blif"));
    odin3_celltype_id type = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(design, odin3_bytes_cstr("multiply"), &name));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, name, &type));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, odin3_celltype_get(design, type)->gran);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, type));
}

static void test_stats_format(void) {
    read_text(LEAF_ROOT);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    const char *expected = "I stats: design: modules 2, top leaf\n"
                           "I stats: module leaf: ports 2, nodes 3, nets 2, wires 2\n"
                           "I stats: module leaf: cell $port_in 1\n"
                           "I stats: module leaf: cell $port_out 1\n"
                           "I stats: module leaf: cell $sop 1\n"
                           "I stats: module root: ports 2, nodes 3, nets 2, wires 2\n"
                           "I stats: module root: cell $port_in 1\n"
                           "I stats: module root: cell $port_out 1\n"
                           "I stats: module root: cell leaf 1\n";
    TEST_ASSERT_TRUE_MESSAGE(strncmp(log_text, expected, strlen(expected)) == 0, log_text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("stats", "x"));
}

static void test_stats_empty_design(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    TEST_ASSERT_TRUE(log_has("I stats: design: modules 0, top (none)\n"));
}

static void test_stats_counts_live_objects_only(void) {
    read_text(LEAF_ROOT);
    odin3_module *leaf = odin3_module_get(design, (odin3_module_id){1});
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_node_delete(leaf, (odin3_node_id){odin3_module_node_end(leaf) - 1}));
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    TEST_ASSERT_TRUE(log_has("I stats: module leaf: ports 2, nodes 2, nets 2, wires 2\n"));
    TEST_ASSERT_FALSE(log_has("cell $sop"));
}

/* --- hierarchy (top selection, PHASE1 #18) -----------------------------------------------------
 */

static void test_hierarchy_keeps_a_set_top(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", ""));
    TEST_ASSERT_EQUAL_STRING("leaf", top_name());
}

static void test_hierarchy_auto(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", "-auto"));
    TEST_ASSERT_EQUAL_STRING("root", top_name());
    TEST_ASSERT_TRUE(log_has("I hierarchy: top root"));
}

static void test_hierarchy_explicit_top(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", "-auto"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", "--top leaf"));
    TEST_ASSERT_EQUAL_STRING("leaf", top_name());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", "--top nope"));
    TEST_ASSERT_TRUE(log_has("E hierarchy: no module named 'nope'"));
    TEST_ASSERT_EQUAL_STRING("leaf", top_name());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", "--top"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", "--bogus"));
    TEST_ASSERT_TRUE(log_has("E hierarchy: unknown argument '--bogus'"));
}

static void test_hierarchy_two_candidates(void) {
    read_text(TWO_TOPS);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", "-auto"));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E hierarchy: 2 top candidates: a, b (choose one with --top)"),
                             log_text);
    TEST_ASSERT_EQUAL_STRING("a", top_name());
}

static void test_hierarchy_zero_candidates(void) {
    read_text(CYCLE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", "-auto"));
    TEST_ASSERT_TRUE_MESSAGE(
        log_has("E hierarchy: 0 top candidates: every module is instantiated by another"),
        log_text);
}

static void test_hierarchy_no_modules(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("hierarchy", ""));
    TEST_ASSERT_TRUE(log_has("E hierarchy: the design has no modules"));
}

/* A design built through the IR has no top until hierarchy picks one. */
static void test_hierarchy_selects_when_unset(void) {
    uint32_t n1 = 0;
    uint32_t n2 = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("m1"), &n1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("m2"), &n2));
    odin3_module_id m1 = {0};
    odin3_module_id m2 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, n2, (odin3_prov_id){0}, &m2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, n1, (odin3_prov_id){0}, &m1));
    odin3_module *mod1 = odin3_module_get(design, m1);
    odin3_node_spec spec = {.type = odin3_module_celltype(odin3_module_get(design, m2))};
    odin3_node_id inst = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod1, &spec, &inst));
    TEST_ASSERT_FALSE(odin3_module_valid(odin3_design_top(design)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", ""));
    TEST_ASSERT_EQUAL_STRING("m1", top_name());
}

/* --- design top (IR design record) -------------------------------------------------------------
 */

static void test_design_set_top(void) {
    TEST_ASSERT_FALSE(odin3_module_valid(odin3_design_top(design)));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_set_top(design, (odin3_module_id){1}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_set_top(design, (odin3_module_id){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_top(NULL, (odin3_module_id){1}));
    read_text(TWO_TOPS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, (odin3_module_id){2}));
    TEST_ASSERT_EQUAL_STRING("b", top_name());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_set_top(design, (odin3_module_id){3}));
    TEST_ASSERT_EQUAL_STRING("b", top_name());
}

/* The --top option wins over BLIF's first model (DESIGN §4.0). */
static void test_top_option_applies_to_read_blif(void) {
    odin3_pass_set_options((odin3_pass_options){.top = "root"});
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_STRING("root", top_name());
}

static void test_top_option_names_no_module(void) {
    odin3_pass_set_options((odin3_pass_options){.top = "nope"});
    put_text(fopen(BLIF_PATH, "w"), LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("read_blif", BLIF_PATH));
    TEST_ASSERT_TRUE(log_has("E read_blif: --top: no module named 'nope'"));
}

static void test_top_option_used_by_hierarchy(void) {
    read_text(TWO_TOPS);
    odin3_pass_set_options((odin3_pass_options){.top = "b"});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", ""));
    TEST_ASSERT_EQUAL_STRING("b", top_name());
}

/* odin3_pass_set_abc_script (3D AB-27) fills the option with a copy of the path, as given; the
 * other options are kept. */
static void test_abc_script_option_set(void) {
    odin3_pass_set_options((odin3_pass_options){.check = true, .top = "b"});
    char path[] = "abc/map.abc";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(path));
    path[0] = 'X';
    odin3_pass_options opts = odin3_pass_get_options();
    TEST_ASSERT_EQUAL_STRING("abc/map.abc", opts.abc_script);
    TEST_ASSERT_TRUE(opts.abc_script != path);
    TEST_ASSERT_TRUE(opts.check);
    TEST_ASSERT_EQUAL_STRING("b", opts.top);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script("other.abc"));
    TEST_ASSERT_EQUAL_STRING("other.abc", odin3_pass_get_options().abc_script);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(NULL));
    TEST_ASSERT_NULL(odin3_pass_get_options().abc_script);
}

/* Out of memory, the setter fails with NO_MEMORY and the path set before stays. */
static void test_abc_script_option_oom(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script("keep.abc"));
    odin3_util_set_alloc_fail_after(0);
    odin3_status st = odin3_pass_set_abc_script("new.abc");
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_STRING("keep.abc", odin3_pass_get_options().abc_script);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(NULL));
}

/* --- scripts -----------------------------------------------------------------------------------
 */

static void test_script_runs_in_order(void) {
    char text[PATH_BUF];
    (void)snprintf(text, sizeof text,
                   "# a comment line\nread_blif %s/hand_body.blif ; check --fast\n"
                   "\n  stats   # trailing comment\nwrite_blif %s;",
                   ODIN3_BLIF_FIXTURES, OUT_PATH);
    put_text(fopen(SCRIPT_PATH, "w"), text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_TRUE(log_has("I stats: design: modules 2, top top\n"));
    FILE *out = fopen(OUT_PATH, "r");
    TEST_ASSERT_NOT_NULL(out);
    (void)fclose(out);
    /* four runs: read_blif, check, stats, write_blif */
    TEST_ASSERT_EQUAL_UINT32(5, odin3_passrun_end(design));
}

static void test_script_args_reach_the_pass(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("  t_count   x  y ;t_count"));
    TEST_ASSERT_EQUAL_INT(2, count_calls);
    TEST_ASSERT_EQUAL_STRING("", count_args);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("t_count   x  y  # c"));
    TEST_ASSERT_EQUAL_STRING("x  y", count_args);
}

/* Review Focus 5: an unknown pass in a script file is located by file:line and nothing runs. */
static void test_script_unknown_pass_by_line(void) {
    register_test_passes();
    put_text(fopen(SCRIPT_PATH, "w"), "t_count\n# comment\ncheck; nope a b\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E odin3_pm_test.o3:3: unknown pass 'nope'"), log_text);
    TEST_ASSERT_EQUAL_INT(0, count_calls);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_passrun_end(design));
}

/* Review Focus 5: an unknown pass in -p is located by its command index. */
static void test_script_unknown_pass_by_command(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_p("t_count; ;t_count\nnope"));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E -p: command 3: unknown pass 'nope'"), log_text);
    TEST_ASSERT_EQUAL_INT(0, count_calls);
}

static void test_script_failing_pass_is_located(void) {
    register_test_passes();
    put_text(fopen(SCRIPT_PATH, "w"), "t_count\nt_fail x\nt_count\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_TRUE(log_has("E odin3_pm_test.o3:2: pass 't_fail' failed: ODIN3_ERR_IO"));
    TEST_ASSERT_EQUAL_INT(1, count_calls);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run_p("t_count;t_fail"));
    TEST_ASSERT_TRUE(log_has("E -p: command 2: pass 't_fail' failed: ODIN3_ERR_IO"));
}

static void test_script_empty_and_errors(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p(""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p(" ; ;\n# only a comment\n"));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_pass_run_script_file(design, "no_such.o3"));
    TEST_ASSERT_TRUE(log_has("E no_such.o3: cannot read"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_script_file(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run_script(design, (odin3_bytes){0},
                                                (odin3_script_src){NULL, ODIN3_SCRIPT_BY_LINE}));
}

/* One OOM try on a fresh design: the script with allocation fail_after failing. */
static int oom_failures;

static odin3_status script_try(long fail_after) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    count_calls = 0;
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = run_p("t_count a; t_count b\nt_count c");
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    oom_failures += st == ODIN3_ERR_NO_MEMORY ? 1 : 0;
    return st;
}

static void test_script_out_of_memory(void) {
    register_test_passes();
    bool succeeded = false;
    oom_failures = 0;
    for (long i = 0; i < OOM_SWEEP && !succeeded; i++) {
        succeeded = script_try(i) == ODIN3_OK;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_GREATER_THAN_INT(0, oom_failures); /* the sweep really hit allocations */
    TEST_ASSERT_EQUAL_INT(3, count_calls);
}

/* --- argument splitting ------------------------------------------------------------------------
 */

static void test_arg_is(void) {
    odin3_bytes word = {"ab", 2};
    TEST_ASSERT_TRUE(odin3_pass_arg_is(word, "ab"));
    TEST_ASSERT_FALSE(odin3_pass_arg_is(word, "a"));
    TEST_ASSERT_FALSE(odin3_pass_arg_is(word, "abc"));
}

/* The next word of *rest is want (NULL: no word is left). */
static void assert_next(odin3_bytes *rest, const char *want) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    TEST_ASSERT_EQUAL_INT(want == NULL, word.ptr == NULL);
    TEST_ASSERT_TRUE(want == NULL ? word.len == 0 : odin3_pass_arg_is(word, want));
}

static void test_arg_next(void) {
    odin3_bytes rest = odin3_bytes_cstr(" \tab  c\r\n d");
    assert_next(&rest, "ab");
    assert_next(&rest, "c");
    assert_next(&rest, "d");
    assert_next(&rest, NULL);
    rest = (odin3_bytes){0};
    assert_next(&rest, NULL);
}

/* --- follow-up: top written first, quoted arguments, failure post-check, resolve-only ----------
 */

/* The design read back from OUT_PATH has top `want` (BLIF: the first model). */
static void assert_written_top(const char *want) {
    odin3_design *back = odin3_design_create();
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(back, OUT_PATH));
    odin3_module_id top = odin3_design_top(back);
    TEST_ASSERT_EQUAL_UINT32(1, top.v);
    TEST_ASSERT_EQUAL_STRING(want,
                             odin3_strtab_get(odin3_design_strtab(back),
                                              odin3_module_name(odin3_module_get(back, top))));
    TEST_ASSERT_EQUAL_UINT32(odin3_design_module_end(design), odin3_design_module_end(back));
    odin3_design_destroy(back);
}

static void test_write_blif_auto_top_first(void) {
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("hierarchy", "-auto"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_blif", OUT_PATH));
    assert_written_top("root");
    TEST_ASSERT_FALSE(log_has("W write_blif"));
}

static void test_write_blif_option_top_first(void) {
    odin3_pass_set_options((odin3_pass_options){.top = "b"});
    read_text(TWO_TOPS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_blif", OUT_PATH));
    odin3_pass_set_options((odin3_pass_options){0});
    assert_written_top("b");
}

static void test_quoted_path(void) {
    put_text(fopen(SPACED_PATH, "w"), LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif \"odin3 pm test;#.blif\" # comment"));
    TEST_ASSERT_EQUAL_STRING("leaf", top_name());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_blif", "\"\""));
    TEST_ASSERT_TRUE(log_has("E write_blif: expects one path"));
}

static void test_quoted_args_reach_the_pass(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("t_count \"a;b #c\" d; t_count"));
    TEST_ASSERT_EQUAL_INT(2, count_calls);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("t_count \"a;b #c\" d"));
    TEST_ASSERT_EQUAL_STRING("\"a;b #c\" d", count_args);
}

/* An unterminated quote is a located error, and nothing runs. */
static void test_unterminated_quote(void) {
    register_test_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_p("t_count; ; t_count \"a b"));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E -p: command 2: unterminated quote"), log_text);
    put_text(fopen(SCRIPT_PATH, "w"), "t_count\nt_count \"x\n\"\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_TRUE_MESSAGE(log_has("E odin3_pm_test.o3:2: unterminated quote"), log_text);
    TEST_ASSERT_EQUAL_INT(0, count_calls);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("t_count", "\"a"));
    TEST_ASSERT_TRUE(log_has("E pass t_count: unterminated quote in the arguments"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(design, "t_count", (odin3_bytes){NULL, 3}));
    TEST_ASSERT_EQUAL_INT(0, count_calls);
}

static void test_arg_next_quoted(void) {
    odin3_bytes rest = odin3_bytes_cstr(" \"a b\" c\"d e\" \"\"");
    assert_next(&rest, "a b");
    assert_next(&rest, "c");
    assert_next(&rest, "d e");
    assert_next(&rest, ""); /* "" is a word: ptr set, len 0 */
    assert_next(&rest, NULL);
}

static void test_arg_next_unclosed(void) {
    odin3_bytes rest = odin3_bytes_cstr("\"ab c");
    assert_next(&rest, "ab c");
    assert_next(&rest, NULL);
}

/* A failing pass still gets the check after it (it must leave valid IR); its status wins. */
static void test_failed_pass_is_post_checked(void) {
    register_test_passes();
    odin3_pass_set_options((odin3_pass_options){.check = true});
    read_text(LEAF_ROOT);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("t_break_fail", ""));
    TEST_ASSERT_TRUE(log_has("E pass t_break_fail: failed: ODIN3_ERR_IO"));
    TEST_ASSERT_TRUE(log_has("E pass t_break_fail: check after the pass failed"));
    TEST_ASSERT_TRUE(log_has("I pass t_break_fail: "));
}

static void test_script_crlf(void) {
    register_test_passes();
    put_text(fopen(SCRIPT_PATH, "w"), "t_count a\r\n# c\r\n\r\nt_count b\r\nnope\r\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_TRUE(log_has("E odin3_pm_test.o3:5: unknown pass 'nope'\n"));
    put_text(fopen(SCRIPT_PATH, "w"), "t_count a\r\n# c\r\n\r\nt_count b\r\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_script_file(design, SCRIPT_PATH));
    TEST_ASSERT_EQUAL_INT(2, count_calls);
    TEST_ASSERT_EQUAL_STRING("b", count_args);
}

static void test_resolve_runs_nothing(void) {
    register_test_passes();
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_resolve_script(odin3_bytes_cstr("t_count; t_fail"), src));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_pass_resolve_script(odin3_bytes_cstr("t_count; nope"), src));
    TEST_ASSERT_TRUE(log_has("E -p: command 2: unknown pass 'nope'"));
    put_text(fopen(SCRIPT_PATH, "w"), "t_count\nt_fail\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_resolve_script_file(SCRIPT_PATH));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_pass_resolve_script_file("no_such.o3"));
    TEST_ASSERT_EQUAL_INT(0, count_calls);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_passrun_end(design));
}

/* --- AST keep level and write_verilog_ast (2A Task 8) ----------------------------------------- */

static odin3_keep_ast probe_level;

/* Records the design's keep level when it runs. */
static odin3_status keep_probe_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                                    void *user) {
    (void)ctx;
    (void)args;
    (void)user;
    probe_level = odin3_design_keep_ast(des);
    return ODIN3_OK;
}

static const odin3_pass_def KEEP_PROBE_PASS = {
    "t_keep_probe",  "t_keep_probe: records the keep level",
    keep_probe_pass, NULL,
    false,           ODIN3_PASS_PROCESSES_REFUSE};
static const odin3_pass_def WANTS_AST_PASS = {
    "t_wants_ast", "t_wants_ast: reads the AST", keep_probe_pass, NULL,
    true,          ODIN3_PASS_PROCESSES_REFUSE};

static void register_ast_passes(void) {
    static bool done = false;
    if (!done) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&KEEP_PROBE_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&WANTS_AST_PASS));
        done = true;
    }
}

static odin3_status script(const char *text) {
    return odin3_pass_run_script(design, odin3_bytes_cstr(text),
                                 (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND});
}

static void test_wants_ast_raises_keep_level_before_the_script(void) {
    register_ast_passes();
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, odin3_design_keep_ast(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script("t_keep_probe"));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, probe_level);
    /* the probe runs first, yet sees the level the later pass asked for */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script("t_keep_probe; t_wants_ast"));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ELABORATED, probe_level);
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ELABORATED, odin3_design_keep_ast(design));
    /* raised, never lowered */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ALL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script("t_wants_ast"));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ALL, odin3_design_keep_ast(design));
    /* a single pass run raises it too */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_NONE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_wants_ast", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ELABORATED, odin3_design_keep_ast(design));
    TEST_ASSERT_TRUE(odin3_pass_find(odin3_bytes_cstr("write_verilog_ast"))->wants_ast);
    TEST_ASSERT_FALSE(odin3_pass_find(odin3_bytes_cstr("write_blif"))->wants_ast);
}

/* A store of run `run` and form holding `module <name>; endmodule` in one UNIT. */
static odin3_ast *module_store(uint32_t run, odin3_ast_form form, const char *name) {
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, form, (odin3_passrun_id){run}, &ast));
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(name), &str));
    odin3_ast_spec spec = {.kind = ODIN3_AST_MODULE, .name = str};
    odin3_ast_id none[] = {{0}, {0}};
    odin3_ast_id module = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, none, 2, &module));
    spec = (odin3_ast_spec){.kind = ODIN3_AST_UNIT, .name = str};
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, &module, 1, &unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, ast));
    return ast;
}

/* The pass's text for a held store: its header, then its one UNIT (no location: unknown). */
static void append_printed(odin3_strbuf *want, const odin3_ast *ast, const char *header) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(want, odin3_bytes_cstr(header)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(want, "// unit %s at <unknown>:0:0\n",
                                               odin3_ast_name_str(ast, odin3_ast_root(ast, 0))));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, odin3_ast_root(ast, 0), want));
}

/* The first bytes of the file at path, NUL-terminated in buf; returns how many were read. */
static size_t read_back(const char *path, char *buf, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(buf, 1, cap - 1, file);
    buf[len] = '\0';
    (void)fclose(file);
    return len;
}

static void test_write_verilog_ast(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_verilog_ast", OUT_PATH));
    TEST_ASSERT_TRUE(log_has("E write_verilog_ast: the design holds no AST"));
    module_store(2, ODIN3_AST_FORM_ELABORATED, "old"); /* an earlier run: not printed */
    odin3_ast *elab = module_store(5, ODIN3_AST_FORM_ELABORATED, "m_elab");
    odin3_ast *parsed = module_store(5, ODIN3_AST_FORM_PARSED, "m");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog_ast", OUT_PATH));
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    append_printed(&want, parsed, "// odin3 write_verilog_ast: run 5, parsed AST\n");
    append_printed(&want, elab, "// odin3 write_verilog_ast: run 5, elaborated AST\n");
    char got[1024];
    TEST_ASSERT_EQUAL_size_t(want.len, read_back(OUT_PATH, got, sizeof got));
    TEST_ASSERT_EQUAL_STRING(want.data, got);
    TEST_ASSERT_NOT_NULL(strstr(got, "module m"));
    TEST_ASSERT_NULL(strstr(got, "old"));
    odin3_strbuf_free(&want);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_verilog_ast", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("write_verilog_ast", "/nonexistent/dir/x.v"));
}

/* --- processes (2D EL-33) -------------------------------------------------------------------- */

static int refuse_calls;

static odin3_status refuse_body(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                                void *user) {
    (void)ctx;
    (void)des;
    (void)args;
    (void)user;
    refuse_calls++;
    return ODIN3_OK;
}

static const odin3_pass_def REFUSE_PASS = {"t_refuse",  "t_refuse: counts its calls",
                                           refuse_body, NULL,
                                           false,       ODIN3_PASS_PROCESSES_REFUSE};
static const odin3_pass_def OK_PASS = {
    "t_proc_ok", "t_proc_ok: counts its calls", refuse_body, NULL, false, ODIN3_PASS_PROCESSES_OK};

static uint32_t plugin_calls;

static odin3_status plugin_body(odin3_design *des, const char *args, void *user) {
    (void)des;
    (void)args;
    (void)user;
    plugin_calls++;
    return ODIN3_OK;
}

static void register_process_passes(void) {
    static bool done = false;
    if (!done) {
        static const uint8_t one = ODIN3_PASS_PROCESSES_OK;
        odin3_plugin_pass refusing = {"t_plug_null", "h", plugin_body, NULL, NULL, {NULL}};
        odin3_plugin_pass running = {"t_plug_ok", "h", plugin_body, NULL, &one, {NULL}};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&REFUSE_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register_def(&OK_PASS));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&refusing));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&running));
        done = true;
    }
    refuse_calls = 0;
    plugin_calls = 0;
}

/* A SOURCE record in a pass run of its own, so the module and its objects have provenance. */
static odin3_prov_id source_prov(void) {
    uint32_t test = 0;
    uint32_t file = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("test"), &test));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("t.v"), &file));
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, test, &ctx));
    odin3_srcloc loc = {file, 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &prov));
    return prov;
}

static odin3_module *module_named(const char *name, odin3_prov_id prov) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, str, prov, &id));
    return odin3_module_get(design, id);
}

/* proc_fixture_design: module "m" holding Task 3's P1, after an empty module "e". */
static void design_with_process(void) {
    (void)proc_fixture_design(design);
    TEST_ASSERT_TRUE(odin3_design_has_processes(design));
    reset_log();
}

static void test_refuse_pass_on_processes(void) {
    register_process_passes();
    design_with_process();
    uint32_t runs = odin3_passrun_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("t_refuse", ""));
    TEST_ASSERT_EQUAL_STRING("E pass 't_refuse': module 'm' has processes; run proc first\n",
                             log_text);
    TEST_ASSERT_EQUAL_INT(0, refuse_calls);
    TEST_ASSERT_EQUAL_UINT32(runs, odin3_passrun_end(design)); /* no pass run was opened */
    /* in a script: the refusal is the failing pass's status, located */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_p("t_proc_ok; t_refuse"));
    TEST_ASSERT_EQUAL_INT(1, refuse_calls);
    TEST_ASSERT_TRUE(log_has("E -p: command 2: pass 't_refuse' failed"));
}

static void test_refuse_pass_without_processes_runs(void) {
    register_process_passes();
    read_text(LEAF_ROOT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_refuse", ""));
    TEST_ASSERT_EQUAL_INT(1, refuse_calls);
}

static void test_ok_passes_run_on_processes(void) {
    register_process_passes();
    design_with_process();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_proc_ok", ""));
    TEST_ASSERT_EQUAL_INT(1, refuse_calls);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("stats", ""));
    TEST_ASSERT_TRUE(log_has("$proc"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("check", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("compact", ""));
    TEST_ASSERT_FALSE(log_has("has processes"));
}

static void test_write_blif_pass_refused(void) {
    design_with_process();
    (void)remove(OUT_PATH);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_blif", OUT_PATH));
    TEST_ASSERT_EQUAL_STRING("E pass 'write_blif': module 'm' has processes; run proc first\n",
                             log_text);
    TEST_ASSERT_NULL(fopen(OUT_PATH, "r"));
}

/* A plugin pass with processes NULL is REFUSE; with &ODIN3_PASS_PROCESSES_OK it runs. */
static void test_plugin_pass_processes(void) {
    register_process_passes();
    design_with_process();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("t_plug_null", ""));
    TEST_ASSERT_TRUE(log_has("E pass 't_plug_null': module 'm' has processes; run proc first"));
    TEST_ASSERT_EQUAL_UINT32(0, plugin_calls);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_plug_ok", ""));
    TEST_ASSERT_EQUAL_UINT32(1, plugin_calls);
}

static void test_register_rejects_bad_processes(void) {
    static const odin3_pass_def two = {"t_two", "x", count_pass, NULL, false, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&two));
    TEST_ASSERT_TRUE(
        log_has("pass_register: pass 't_two': processes must be 0 (REFUSE) or 1 (OK)"));
    TEST_ASSERT_NULL(odin3_pass_find(odin3_bytes_cstr("t_two")));
}

/* Each built-in's value (2D EL-33 and plan point (x)). */
static void test_builtin_processes_values(void) {
    static const char *const ok[] = {"read_blif",
                                     "read_techlib",
                                     "check",
                                     "stats",
                                     "compact",
                                     "hierarchy",
                                     "write_verilog_ast",
                                     "read_project",
                                     "dump_project",
                                     "preprocess",
                                     "write_rtlil",
                                     "proc",
                                     "read_verilog",
                                     "opt",
                                     "opt_expr",
                                     "opt_merge",
                                     "opt_muxtree",
                                     "opt_reduce",
                                     "opt_share",
                                     "opt_dff",
                                     "opt_clean",
                                     "wreduce"};
    static const char *const refuse[] = {"write_blif", "write_json", "write_verilog", "write_dot",
                                         "flatten",    "lower",      "sim",           "abc"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr(ok[i]));
        TEST_ASSERT_NOT_NULL_MESSAGE(def, ok[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ODIN3_PASS_PROCESSES_OK, def->processes, ok[i]);
    }
    for (size_t i = 0; i < sizeof refuse / sizeof refuse[0]; i++) {
        const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr(refuse[i]));
        TEST_ASSERT_NOT_NULL_MESSAGE(def, refuse[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ODIN3_PASS_PROCESSES_REFUSE, def->processes, refuse[i]);
    }
    /* every built-in is listed: a new one must choose */
    TEST_ASSERT_EQUAL_UINT32(sizeof ok / sizeof ok[0] + sizeof refuse / sizeof refuse[0],
                             odin3_builtin_pass_count);
}

/* Wires w1 .. w5 of mod (IDs 1 .. 5), in wires[1 .. 5]. */
static void five_wires(odin3_module *mod, odin3_prov_id prov, odin3_wire_id wires[6]) {
    for (uint32_t i = 1; i <= 5; i++) {
        char name[8];
        (void)snprintf(name, sizeof name, "w%u", i);
        uint32_t str = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
        odin3_wire_spec spec = {str, 0, 0, false, prov};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, NULL, &wires[i]));
        TEST_ASSERT_EQUAL_UINT32(i, wires[i].v);
    }
}

/* An elaborated store with a design and a library scope, for the remap test's symbols. */
typedef struct remap_store {
    odin3_ast *ast;
    odin3_symtab *st;
    odin3_scope_id lib;
    odin3_ast_id decl;
} remap_store;

static uint32_t ast_name(odin3_ast *ast, const char *text) {
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(text), &name));
    return name;
}

static remap_store remap_store_new(void) {
    remap_store rs = {NULL, NULL, {0}, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_ELABORATED,
                                                     (odin3_passrun_id){1}, &rs.ast));
    odin3_ast_spec node_spec = {.kind = ODIN3_AST_IDENT, .name = ast_name(rs.ast, "x")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(rs.ast, &node_spec, NULL, 0, &rs.decl));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symtab_create(rs.ast, &rs.st));
    odin3_scope_id top = {0};
    odin3_scope_spec scope = {.kind = ODIN3_SCOPE_DESIGN};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(rs.st, &scope, &top));
    scope = (odin3_scope_spec){
        .kind = ODIN3_SCOPE_LIBRARY, .name = ast_name(rs.ast, "work"), .parent = top};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(rs.st, &scope, &rs.lib));
    return rs;
}

/* A module scope for mod in the library scope. */
static odin3_scope_id remap_scope(const remap_store *rs, const char *name) {
    odin3_scope_spec scope = {
        .kind = ODIN3_SCOPE_MODULE, .name = ast_name(rs->ast, name), .parent = rs->lib};
    odin3_scope_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(rs->st, &scope, &out));
    return out;
}

/* A net symbol named after wire `wire` of mod (w<wire>), in scope, bound to that wire. */
static odin3_sym_id remap_symbol(const remap_store *rs, odin3_scope_id scope, odin3_module *mod,
                                 uint32_t wire) {
    char name[8];
    (void)snprintf(name, sizeof name, "w%u", wire);
    odin3_symbol_spec spec = {
        .name = ast_name(rs->ast, name), .scope = scope, .kind = ODIN3_SYM_NET, .decl = rs->decl};
    odin3_sym_id sym = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(rs->st, &spec, &sym));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_symbol_set_ir(rs->st, sym, odin3_module_id_of(mod), (odin3_wire_id){wire}));
    return sym;
}

static void assert_bound(const remap_store *rs, odin3_sym_id sym, odin3_module *mod,
                         uint32_t wire) {
    TEST_ASSERT_EQUAL_UINT32(wire, odin3_symbol_get(rs->st, sym)->ir_wire.v);
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(mod).v, odin3_symbol_get(rs->st, sym)->ir_module.v);
}

/*
 * compact remaps every held store's symbol table (EL-2; PHASE2 #8 carry-over), each module by its
 * own map (Task 4 review minor 1): a loses w1 and b loses w2 and w3, so a's w5 -> 4, w2 -> 1 and
 * b's w5 -> 3, w1 -> 1, and b's symbol on the dead w3 is unbound (0).
 */
static void test_compact_remaps_symtabs(void) {
    odin3_prov_id prov = source_prov();
    odin3_module *mod_a = module_named("a", prov);
    odin3_module *mod_b = module_named("b", prov);
    odin3_wire_id wa[6] = {{0}};
    odin3_wire_id wb[6] = {{0}};
    five_wires(mod_a, prov, wa);
    five_wires(mod_b, prov, wb);
    remap_store rs = remap_store_new();
    odin3_scope_id sa = remap_scope(&rs, "a");
    odin3_scope_id sb = remap_scope(&rs, "b");
    odin3_sym_id a5 = remap_symbol(&rs, sa, mod_a, 5);
    odin3_sym_id a2 = remap_symbol(&rs, sa, mod_a, 2);
    odin3_sym_id b5 = remap_symbol(&rs, sb, mod_b, 5);
    odin3_sym_id b1 = remap_symbol(&rs, sb, mod_b, 1);
    odin3_sym_id b3 = remap_symbol(&rs, sb, mod_b, 3);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, rs.ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(mod_a, wa[1]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(mod_b, wb[2]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(mod_b, wb[3]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("compact", ""));
    assert_bound(&rs, a5, mod_a, 4);
    assert_bound(&rs, a2, mod_a, 1);
    assert_bound(&rs, b5, mod_b, 3);
    assert_bound(&rs, b1, mod_b, 1);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_get(rs.st, b3)->ir_wire.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("compact", "")); /* a second compact is the identity */
    assert_bound(&rs, a5, mod_a, 4);
    assert_bound(&rs, b5, mod_b, 3);
}

/*
 * The refusal names the first module in creation order that has a process (Task 4 review minor
 * 3): z_first, made before a_second, though a_second sorts first and is the later module.
 */
static void test_refusal_names_the_first_module(void) {
    register_process_passes();
    odin3_prov_id prov = source_prov();
    odin3_module *first = module_named("z_first", prov);
    odin3_module *second = module_named("a_second", prov);
    (void)proc_fixture_p1(second, prov);
    (void)proc_fixture_p1(first, prov);
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_get_proc_count(design, odin3_module_id_of(second).v, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("t_refuse", ""));
    TEST_ASSERT_EQUAL_STRING("E pass 't_refuse': module 'z_first' has processes; run proc first\n",
                             log_text);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_blif", OUT_PATH));
    TEST_ASSERT_EQUAL_STRING(
        "E pass 'write_blif': module 'z_first' has processes; run proc first\n", log_text);
}

/*
 * The plugin's processes byte is read once, at registration (test_abi_ext's claim; Task 4 review
 * minor 2): a pass registered OK still runs on a design with processes after its byte is set to
 * REFUSE.
 */
static void test_plugin_processes_read_once(void) {
    register_process_passes();
    uint8_t byte = ODIN3_PASS_PROCESSES_OK;
    odin3_plugin_pass once = {"t_plug_once", "h", plugin_body, NULL, &byte, {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&once));
    byte = ODIN3_PASS_PROCESSES_REFUSE;
    design_with_process();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("t_plug_once", ""));
    TEST_ASSERT_EQUAL_UINT32(1, plugin_calls);
}

/* 2D EL-33 and the compact remap (kept out of main for its size). */
static void run_process_tests(void) {
    RUN_TEST(test_refuse_pass_on_processes);
    RUN_TEST(test_refuse_pass_without_processes_runs);
    RUN_TEST(test_ok_passes_run_on_processes);
    RUN_TEST(test_write_blif_pass_refused);
    RUN_TEST(test_plugin_pass_processes);
    RUN_TEST(test_register_rejects_bad_processes);
    RUN_TEST(test_builtin_processes_values);
    RUN_TEST(test_compact_remaps_symtabs);
    RUN_TEST(test_refusal_names_the_first_module);
    RUN_TEST(test_plugin_processes_read_once);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_builtins_registered);
    RUN_TEST(test_wants_ast_raises_keep_level_before_the_script);
    RUN_TEST(test_write_verilog_ast);
    RUN_TEST(test_register_and_run);
    RUN_TEST(test_register_rejects);
    RUN_TEST(test_unknown_pass);
    RUN_TEST(test_run_opens_named_provenance_run);
    RUN_TEST(test_read_blif_uses_the_pass_run);
    RUN_TEST(test_failing_post_check);
    RUN_TEST(test_failing_pre_check_skips_the_pass);
    RUN_TEST(test_debug_always_checks);
    RUN_TEST(test_no_check_when_off_in_release);
    RUN_TEST(test_failing_pass);
    RUN_TEST(test_read_blif_records_top);
    RUN_TEST(test_read_blif_bad_args);
    RUN_TEST(test_write_blif_round_trip);
    RUN_TEST(test_check_pass);
    RUN_TEST(test_compact_pass);
    RUN_TEST(test_stats_format);
    RUN_TEST(test_stats_empty_design);
    RUN_TEST(test_stats_counts_live_objects_only);
    RUN_TEST(test_hierarchy_keeps_a_set_top);
    RUN_TEST(test_hierarchy_auto);
    RUN_TEST(test_hierarchy_explicit_top);
    RUN_TEST(test_hierarchy_two_candidates);
    RUN_TEST(test_hierarchy_zero_candidates);
    RUN_TEST(test_hierarchy_no_modules);
    RUN_TEST(test_hierarchy_selects_when_unset);
    RUN_TEST(test_design_set_top);
    RUN_TEST(test_top_option_applies_to_read_blif);
    RUN_TEST(test_top_option_names_no_module);
    RUN_TEST(test_top_option_used_by_hierarchy);
    RUN_TEST(test_abc_script_option_set);
    RUN_TEST(test_abc_script_option_oom);
    RUN_TEST(test_script_runs_in_order);
    RUN_TEST(test_script_args_reach_the_pass);
    RUN_TEST(test_script_unknown_pass_by_line);
    RUN_TEST(test_script_unknown_pass_by_command);
    RUN_TEST(test_script_failing_pass_is_located);
    RUN_TEST(test_script_empty_and_errors);
    RUN_TEST(test_script_out_of_memory);
    RUN_TEST(test_arg_is);
    RUN_TEST(test_arg_next);
    RUN_TEST(test_write_blif_auto_top_first);
    RUN_TEST(test_write_blif_option_top_first);
    RUN_TEST(test_quoted_path);
    RUN_TEST(test_quoted_args_reach_the_pass);
    RUN_TEST(test_unterminated_quote);
    RUN_TEST(test_arg_next_quoted);
    RUN_TEST(test_arg_next_unclosed);
    RUN_TEST(test_failed_pass_is_post_checked);
    RUN_TEST(test_script_crlf);
    RUN_TEST(test_resolve_runs_nothing);
    RUN_TEST(test_check_warnings_once_per_pass);
    RUN_TEST(test_read_techlib);
    run_process_tests();
    return UNITY_END();
}
