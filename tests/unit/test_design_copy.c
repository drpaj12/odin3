/*
 * test_design_copy.c — odin3_design_copy (Phase 7 Task 6, spec 3.2): a deep copy whose JSON
 * equals the original's, that a pass on the copy does not disturb, that keeps every ID, that
 * outlives the original and that leaves nothing behind when it runs out of memory.
 */
#include "ir/celltype.h"
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

/* The text write_json (with --odin3, which adds the provenance) gives; the caller frees it. */
static char *json_of(odin3_design *design) {
    char path[] = "/tmp/odin3_design_copy_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\" --odin3", path);
    run(design, "write_json", args);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    long size = ftell(file);
    TEST_ASSERT_TRUE(size > 0);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_UINT((size_t)size, fread(text, 1, (size_t)size, file));
    text[size] = '\0';
    (void)fclose(file);
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
    uint32_t top = 0, top_copy = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(g_src, &top));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(g_copy, &top_copy));
    TEST_ASSERT_EQUAL_UINT32(top, top_copy);
    uint32_t count = 0, count_copy = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(g_src, &count));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(g_copy, &count_copy));
    TEST_ASSERT_EQUAL_UINT32(count, count_copy);
    TEST_ASSERT_TRUE(count > 0);
    for (uint32_t mod = 1; mod <= count; mod++) {
        uint32_t end = 0, end_copy = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(g_src, mod, &end));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(g_copy, mod, &end_copy));
        TEST_ASSERT_EQUAL_UINT32(end, end_copy);
        for (uint32_t id = 1; id < end; id++) {
            const char *name = NULL, *name_copy = NULL, *type = NULL, *type_copy = NULL;
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

/*
 * Every allocation from the n-th on fails, for each n up to the first copy that succeeds: each
 * failure returns NO_MEMORY with *out NULL, and the sanitizers show nothing left allocated.
 */
static void test_copy_oom_leaves_nothing(void) {
    load_verilog(VERILOG("bm_DL_four_bit_adder_continuous_assign_using_vectors"));
    run(g_src, "opt", ""); /* dead objects, tombstones and provenance in the store */
    long failures = 0;
    for (long nth = 0; nth < OOM_LIMIT; nth++) {
        odin3_design *out = (odin3_design *)(uintptr_t)1; /* must be overwritten */
        odin3_util_set_alloc_fail_from(nth);
        odin3_status st = odin3_design_copy(g_src, &out);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_NOT_NULL(out);
            assert_same_json(g_src, out);
            odin3_design_destroy(out);
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(out);
        failures++;
    }
    TEST_ASSERT_TRUE(failures > 10);
    TEST_ASSERT_TRUE(failures < OOM_LIMIT);
}

/* A local type with a width expression is not copied by this function (it cannot be). */
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

static void test_copy_refuses_width_expression(void) {
    static const odin3_width_expr wexpr = {wexpr_check, wexpr_eval, NULL};
    const odin3_port_def ports[1] = {{"o", ODIN3_DIR_OUT, true, 1, NULL, NULL, &wexpr, false}};
    const odin3_celltype_def def = {"copy_wexpr_type", ODIN3_GRAN_BLACKBOX, 0, ports, 1, NULL, 0,
                                    NULL, NULL, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(g_src, &def, &id));
    odin3_design *out = (odin3_design *)(uintptr_t)1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_copy(g_src, &out));
    TEST_ASSERT_NULL(out);
}

static void test_copy_refuses_techlib_data(void) {
    static const int stand_in = 0;
    const odin3_celltype_def def = {"copy_lib_type", ODIN3_GRAN_BLACKBOX, 0, NULL, 0, NULL, 0,
                                    NULL, NULL, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(g_src, &def, &id));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_celltype_set_lib(g_src, id, (const odin3_techlib_cell *)&stand_in));
    odin3_design *out = (odin3_design *)(uintptr_t)1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_copy(g_src, &out));
    TEST_ASSERT_NULL(out);
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
    RUN_TEST(test_copy_refuses_width_expression);
    RUN_TEST(test_copy_refuses_techlib_data);
    return UNITY_END();
}
