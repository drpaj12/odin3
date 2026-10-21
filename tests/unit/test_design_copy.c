/*
 * test_design_copy.c — odin3_design_copy (Phase 7 Task 6, spec 3.2): a deep copy whose JSON
 * equals the original's, that a pass on the copy does not disturb, that keeps every ID, that
 * outlives the original and that leaves nothing behind when it runs out of memory.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
#endif

enum { PATH_CAP = 700, ARGS_CAP = 800, OOM_LIMIT = 200000 };

#define BLIF(name) ODIN3_SOURCE_DIR "/tests/golden/blif/" name ".blif"
#define VERILOG(name) ODIN3_SOURCE_DIR "/tests/micro/verilog/full/" name ".v"

static odin3_design *g_src;
static odin3_design *g_copy;

void setUp(void) {
    g_src = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_src);
    g_copy = NULL;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_design_destroy(g_copy);
    odin3_design_destroy(g_src);
    g_copy = NULL;
    g_src = NULL;
}

static void run(odin3_design *design, const char *pass, const char *args) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, pass, args));
}

static void load_blif(const char *path) {
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    run(g_src, "read_blif", args);
}

static void load_verilog(const char *path) {
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    run(g_src, "read_verilog", args);
    run(g_src, "proc", "");
}

/* The whole file at path, NUL-terminated; the caller frees it. */
static char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    long size = ftell(file);
    TEST_ASSERT_TRUE(size > 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *text = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_UINT((size_t)size, fread(text, 1, (size_t)size, file));
    text[size] = '\0';
    (void)fclose(file);
    return text;
}

/* The text write_json (with --odin3, which adds the provenance) gives; the caller frees it. */
static char *json_of(odin3_design *design) {
    char path[] = "/tmp/odin3_design_copy_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\" --odin3", path);
    run(design, "write_json", args);
    char *text = slurp(path);
    (void)remove(path);
    return text;
}

/* The text write_blif --vtr gives; the caller frees it. */
static char *blif_of(odin3_design *design) {
    char path[] = "/tmp/odin3_design_copy_blif_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "--vtr \"%s\"", path);
    run(design, "write_blif", args);
    char *text = slurp(path);
    (void)remove(path);
    return text;
}

static void assert_same_json(odin3_design *left, odin3_design *right) {
    char *lhs = json_of(left);
    char *rhs = json_of(right);
    TEST_ASSERT_EQUAL_STRING(lhs, rhs);
    free(lhs);
    free(rhs);
}

static void copy_ok(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_copy(g_src, &g_copy));
    TEST_ASSERT_NOT_NULL(g_copy);
}

static void test_copy_json_identical_blif(void) {
    load_blif(BLIF("hand_body"));
    copy_ok();
    assert_same_json(g_src, g_copy);
}

static void test_copy_json_identical_verilog(void) {
    load_verilog(VERILOG("ansiportlist_2"));
    copy_ok();
    assert_same_json(g_src, g_copy);
}

static void test_copy_json_identical_after_opt_left_dead_objects(void) {
    load_verilog(VERILOG("bm_DL_four_bit_adder_continuous_assign_using_vectors"));
    run(g_src, "opt", "");
    copy_ok();
    assert_same_json(g_src, g_copy);
}

static void test_copy_is_independent(void) {
    load_verilog(VERILOG("ansiportlist_2"));
    copy_ok();
    char *before = json_of(g_src);
    run(g_copy, "opt", "");
    char *after_src = json_of(g_src);
    char *after_copy = json_of(g_copy);
    TEST_ASSERT_EQUAL_STRING(before, after_src);
    TEST_ASSERT_TRUE(strcmp(before, after_copy) != 0);
    free(before);
    free(after_src);
    free(after_copy);
}

static void assert_same_ids(void) {
    uint32_t top = 0;
    uint32_t top_copy = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(g_src, &top));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(g_copy, &top_copy));
    TEST_ASSERT_EQUAL_UINT32(top, top_copy);
    uint32_t count = 0;
    uint32_t count_copy = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(g_src, &count));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(g_copy, &count_copy));
    TEST_ASSERT_EQUAL_UINT32(count, count_copy);
    TEST_ASSERT_TRUE(count > 0);
    for (uint32_t mod = 1; mod <= count; mod++) {
        uint32_t end = 0;
        uint32_t end_copy = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(g_src, mod, &end));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(g_copy, mod, &end_copy));
        TEST_ASSERT_EQUAL_UINT32(end, end_copy);
        for (uint32_t id = 1; id < end; id++) {
            const char *name = NULL;
            const char *name_copy = NULL;
            const char *type = NULL;
            const char *type_copy = NULL;
            odin3_ref ref = {mod, id};
            odin3_status st = odin3_node_get_name(g_src, ref, &name);
            if (st != ODIN3_OK) {
                continue; /* a dead node has no name; the copy must agree */
            }
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_name(g_copy, ref, &name_copy));
            TEST_ASSERT_EQUAL_STRING(name, name_copy);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_type_name(g_src, ref, &type));
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_type_name(g_copy, ref, &type_copy));
            TEST_ASSERT_EQUAL_STRING(type, type_copy);
        }
    }
}

static void test_copy_keeps_ids(void) {
    load_verilog(VERILOG("ansiportlist_2"));
    copy_ok();
    assert_same_ids();
}

static void test_copy_keeps_ids_blif(void) {
    load_blif(BLIF("hand_ports"));
    copy_ok();
    assert_same_ids();
}

/* Every string, name and value of the copy is its own: the original may go first. */
static void test_copy_outlives_original(void) {
    load_verilog(VERILOG("ansiportlist_2"));
    char *want = json_of(g_src);
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    char *have = json_of(g_copy);
    TEST_ASSERT_EQUAL_STRING(want, have);
    free(want);
    free(have);
}

static void test_copy_of_a_copy(void) {
    load_verilog(VERILOG("ansiportlist_2"));
    copy_ok();
    odin3_design *second = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_copy(g_copy, &second));
    assert_same_json(g_src, second);
    odin3_design_destroy(second);
}

/* The copy of an empty design is an empty design. */
static void test_copy_of_empty_design(void) {
    copy_ok();
    uint32_t count = 7;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(g_copy, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

static void test_copy_invalid_args(void) {
    odin3_design *out = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_copy(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_copy(g_src, NULL));
    TEST_ASSERT_NULL(out);
}

/* Copies with every allocation from the nth on failing; true when the copy succeeded. */
static bool copy_with_failures_from(long nth) {
    odin3_design *out = g_src; /* a non-NULL stand-in the call must overwrite */
    odin3_util_set_alloc_fail_from(nth);
    odin3_status st = odin3_design_copy(g_src, &out);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        TEST_ASSERT_NOT_NULL(out);
        assert_same_json(g_src, out);
        odin3_design_destroy(out);
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_NULL(out);
    return false;
}

/*
 * Every allocation from the n-th on fails, for each n up to the first copy that succeeds: each
 * failure returns NO_MEMORY with *out NULL, and the sanitizers show nothing left allocated.
 */
static void test_copy_oom_leaves_nothing(void) {
    load_verilog(VERILOG("bm_DL_four_bit_adder_continuous_assign_using_vectors"));
    run(g_src, "opt", ""); /* dead objects, tombstones and provenance in the store */
    long failures = 0;
    while (failures < OOM_LIMIT && !copy_with_failures_from(failures)) {
        failures++;
    }
    TEST_ASSERT_TRUE(failures > 10);
    TEST_ASSERT_TRUE(failures < OOM_LIMIT);
}

/* A local type with a width expression is copied: the copy keeps the (shared, immutable) rule. */
static bool wexpr_check(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                        odin3_width_why *why) {
    (void)wexpr;
    (void)def;
    (void)why;
    return true;
}

static odin3_status wexpr_eval(const odin3_width_expr *wexpr, const odin3_width_args *args,
                               uint32_t *width) {
    (void)wexpr;
    (void)args;
    *width = 1;
    return ODIN3_OK;
}

static void test_copy_takes_width_expression(void) {
    static const odin3_width_expr wexpr = {wexpr_check, wexpr_eval, NULL};
    const odin3_port_def ports[1] = {{"o", ODIN3_DIR_OUT, true, 1, NULL, NULL, &wexpr, false}};
    const odin3_celltype_def def = {
        "copy_wexpr_type", ODIN3_GRAN_BLACKBOX, 0, ports, 1, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(g_src, &def, &id));
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    const odin3_celltype_def *have = odin3_celltype_get(g_copy, id);
    TEST_ASSERT_NOT_NULL(have);
    TEST_ASSERT_EQUAL_STRING("copy_wexpr_type", have->name);
    TEST_ASSERT_EQUAL_PTR(&wexpr, have->ports[0].width_expr);
}

static void test_copy_takes_techlib_data(void) {
    static const int stand_in = 0;
    const odin3_celltype_def def = {
        "copy_lib_type", ODIN3_GRAN_BLACKBOX, 0, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(g_src, &def, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_celltype_set_lib(g_src, id, (const odin3_techlib_cell *)&stand_in));
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    TEST_ASSERT_EQUAL_PTR(&stand_in, odin3_celltype_lib(g_copy, id));
}

/* A design read with a tech library (a hard block through hardmap) is copied; the copy outlives
 * the source and writes the same BLIF and JSON as the source did before it was destroyed. */
static void test_copy_with_techlib_outlives_source(void) {
    run(g_src, "read_techlib", "\"" ODIN3_SOURCE_DIR "/lib/vtr.o3lib\"");
    run(g_src, "read_verilog", "--lib \"" ODIN3_SOURCE_DIR "/tests/hardmap/adder_lib.v\"");
    run(g_src, "read_verilog",
        "--nolatches --vtr-compat \"" ODIN3_SOURCE_DIR "/tests/hardmap/add_8x6.v\"");
    run(g_src, "proc", "");
    run(g_src, "opt", "");
    run(g_src, "flatten", "");
    run(g_src, "opt", "-full");
    run(g_src, "hardmap", "");
    char *json = json_of(g_src);
    char *blif = blif_of(g_src);
    TEST_ASSERT_NOT_NULL(strstr(blif, ".subckt"));
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    char *json_copy = json_of(g_copy);
    char *blif_copy = blif_of(g_copy);
    TEST_ASSERT_EQUAL_STRING(json, json_copy);
    TEST_ASSERT_EQUAL_STRING(blif, blif_copy);
    free(json);
    free(blif);
    free(json_copy);
    free(blif_copy);
}

/* hardmap on a copy of a design that has an architecture gives what it gives on the source. */
static void test_copy_keeps_arch_for_hardmap(void) {
    run(g_src, "read_arch", "\"" ODIN3_SOURCE_DIR "/tests/unit/data/arch/adder_chain.xml\"");
    run(g_src, "read_verilog", "--lib \"" ODIN3_SOURCE_DIR "/tests/hardmap/adder_lib.v\"");
    run(g_src, "read_verilog",
        "--nolatches --vtr-compat \"" ODIN3_SOURCE_DIR "/tests/hardmap/add_8x6.v\"");
    run(g_src, "proc", "");
    run(g_src, "opt", "");
    run(g_src, "flatten", "");
    run(g_src, "opt", "-full");
    copy_ok();
    TEST_ASSERT_NOT_NULL(odin3_design_arch(g_copy));
    odin3_design_destroy(g_src); /* the copy's arch outlives the source's design */
    g_src = NULL;
    run(g_copy, "hardmap", "");
    char *got = blif_of(g_copy);
    TEST_ASSERT_NOT_NULL(strstr(got, ".subckt adder"));
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_NOT_NULL(again);
    g_src = again;
    run(g_src, "read_arch", "\"" ODIN3_SOURCE_DIR "/tests/unit/data/arch/adder_chain.xml\"");
    run(g_src, "read_verilog", "--lib \"" ODIN3_SOURCE_DIR "/tests/hardmap/adder_lib.v\"");
    run(g_src, "read_verilog",
        "--nolatches --vtr-compat \"" ODIN3_SOURCE_DIR "/tests/hardmap/add_8x6.v\"");
    run(g_src, "proc", "");
    run(g_src, "opt", "");
    run(g_src, "flatten", "");
    run(g_src, "opt", "-full");
    run(g_src, "hardmap", "");
    char *want = blif_of(g_src);
    TEST_ASSERT_EQUAL_STRING(want, got);
    free(want);
    free(got);
}

/* Reads text (Verilog) into g_src through a temporary file. */
static void load_verilog_text(const char *text) {
    char path[] = "/tmp/odin3_design_copy_text_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    (void)fclose(file);
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    run(g_src, "read_verilog", args);
    (void)remove(path);
    run(g_src, "proc", "");
}

/* Attributes, STRING/BITS/PROC parameters, tombstones, a declared model and a black-box
 * definition: every one is the copy's own once the source is gone. */
static void test_copy_rich_design_outlives_source(void) {
    static const char text[] =
        "(* blackbox *) module bb #(parameter W = 1, parameter S = \"ab\", parameter B = 4'b10x1)"
        " (input [W-1:0] a, output [W-1:0] y);\nendmodule\n"
        "(* keep, note = \"hi\" *) module top(input [3:0] i, input [3:0] j, input k,"
        " output [3:0] o, output [3:0] p, output q);\n"
        "  (* inst_attr = 5 *) bb #(.W(4), .S(\"xyz\")) u1 (.a(i), .y(o));\n"
        "  bb #(.W(4), .B(4'b0101)) u2 (.a(j), .y(p));\n"
        "  reg r; always @(*) begin if (k) r = i[0]; else r = j[1]; end\n"
        "  assign q = r & (i[1] | 1'b0) & (i[2] | j[2]);\n"
        "endmodule\n";
    load_verilog_text(text);
    run(g_src, "opt", "-full");
    run(g_src, "compact", "");
    char *want = json_of(g_src);
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    char *have = json_of(g_copy);
    TEST_ASSERT_EQUAL_STRING(want, have);
    free(want);
    free(have);
}

/* An elaborated design with a parametric black box whose port widths differ per instance: the
 * width rule is in the shared cell-type arena, and the copy outlives the source. */
static void test_copy_parametric_blackbox_outlives_source(void) {
    static const char text[] =
        "(* blackbox *) module bb #(parameter W = 1) (input [W-1:0] a, output [W-1:0] y);\n"
        "endmodule\n"
        "module top(input [3:0] i, input [1:0] j, output [3:0] o, output [1:0] p);\n"
        "  bb #(.W(4)) u1 (.a(i), .y(o));\n"
        "  bb #(.W(2)) u2 (.a(j), .y(p));\n"
        "endmodule\n";
    load_verilog_text(text);
    copy_ok();
    odin3_design_destroy(g_src);
    g_src = NULL;
    run(g_copy, "opt", "");
    run(g_copy, "check", "");
    odin3_design *second = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_copy(g_copy, &second));
    odin3_design_destroy(g_copy);
    g_copy = second;
    run(g_copy, "check", "");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_copy_json_identical_blif);
    RUN_TEST(test_copy_json_identical_verilog);
    RUN_TEST(test_copy_json_identical_after_opt_left_dead_objects);
    RUN_TEST(test_copy_is_independent);
    RUN_TEST(test_copy_keeps_ids);
    RUN_TEST(test_copy_keeps_ids_blif);
    RUN_TEST(test_copy_outlives_original);
    RUN_TEST(test_copy_of_a_copy);
    RUN_TEST(test_copy_of_empty_design);
    RUN_TEST(test_copy_invalid_args);
    RUN_TEST(test_copy_oom_leaves_nothing);
    RUN_TEST(test_copy_takes_width_expression);
    RUN_TEST(test_copy_takes_techlib_data);
    RUN_TEST(test_copy_with_techlib_outlives_source);
    RUN_TEST(test_copy_keeps_arch_for_hardmap);
    RUN_TEST(test_copy_rich_design_outlives_source);
    RUN_TEST(test_copy_parametric_blackbox_outlives_source);
    return UNITY_END();
}
