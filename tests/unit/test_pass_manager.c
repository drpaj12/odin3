/*
 * test_pass_manager.c — unit tests for the pass manager, the built-in passes and pass scripts.
 */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_test.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
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

static const odin3_pass_def COUNT_PASS = {"t_count", "t_count [args]: counts its calls", count_pass,
                                          &count_marker};

/* Breaks rule 4: a second driver on net 1 of module 1 (an input port's net). */
static odin3_status break_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                               void *user) {
    (void)user;
    (void)ctx;
    (void)args;
    odin3_module *module = odin3_module_get(des, (odin3_module_id){1});
    return odin3_ir_test_corrupt(module, (odin3_ir_test_target){ODIN3_IR_TEST_MULTI_DRIVER, 1});
}

static const odin3_pass_def BREAK_PASS = {"t_break", "t_break: breaks rule 4", break_pass, NULL};

static odin3_status fail_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                              void *user) {
    (void)user;
    (void)ctx;
    (void)des;
    (void)args;
    return ODIN3_ERR_IO;
}

static const odin3_pass_def FAIL_PASS = {"t_fail", "t_fail: always fails", fail_pass, NULL};

/* Breaks rule 4 like t_break, then fails: the check after it still runs. */
static odin3_status break_fail_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                                    void *user) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, break_pass(ctx, des, args, user));
    return ODIN3_ERR_IO;
}

static const odin3_pass_def BREAK_FAIL_PASS = {"t_break_fail", "t_break_fail: breaks, then fails",
                                               break_fail_pass, NULL};

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
    static const odin3_pass_def dup_builtin = {"check", "x", count_pass, NULL};
    static const odin3_pass_def blank = {"a b", "x", count_pass, NULL};
    static const odin3_pass_def semi = {"a;b", "x", count_pass, NULL};
    static const odin3_pass_def empty = {"", "x", count_pass, NULL};
    static const odin3_pass_def no_run = {"t_no_run", "x", NULL, NULL};
    static const odin3_pass_def no_help = {"t_no_help", NULL, count_pass, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&COUNT_PASS));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&dup_builtin));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&blank));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register_def(&semi));
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_builtins_registered);
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
    return UNITY_END();
}
