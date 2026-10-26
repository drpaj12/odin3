/*
 * test_construct_check.c — check rule 12 (the construct table, IR §9): each clause fails on a
 * planted fault, a well-formed table passes, FAST does not look, and the completeness clause is
 * silent for a design without a snapshot (ast=none).
 */
#include "ir/check.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { BAD_STR = 100000 };

static odin3_design *design;
static odin3_passrun_id run1;
static char errors[1024];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && strlen(errors) + strlen(msg) + 2 < sizeof errors) {
        (void)strcat(errors, msg);
        (void)strcat(errors, "\n");
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

typedef struct row_args {
    uint32_t kind;
    uint32_t part;
    uint32_t ast;
    uint32_t parent;
} row_args;

/* ADD(kind, part, ast, parent): appends a row; level from the kind map; line = ast. */
#define ADD(...) add((row_args){__VA_ARGS__})

static uint32_t add(row_args args) {
    odin3_construct_row row = {0};
    row.run = run1.v;
    row.ast = args.ast;
    row.parent = args.parent;
    row.part = (uint8_t)args.part;
    row.kind = (uint8_t)args.kind;
    row.level = (uint8_t)odin3_construct_kind_level(args.kind);
    row.file = intern("a.v");
    row.line = args.ast;
    row.end_line = args.ast;
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &index));
    return index;
}

/* module 1, always 2, if 3, then-arm 4, else-arm 5, assignment 6 under the then-arm. */
static void build_table(void) {
    uint32_t mod = ADD(ODIN3_AST_MODULE, 0, 3, 0);
    uint32_t always = ADD(ODIN3_AST_ALWAYS, 0, 10, mod);
    uint32_t iff = ADD(ODIN3_AST_IF, 0, 12, always);
    uint32_t then_arm = ADD(ODIN3_CONSTRUCT_IF_THEN, ODIN3_CONSTRUCT_PART_THEN, 12, iff);
    (void)ADD(ODIN3_CONSTRUCT_IF_ELSE, ODIN3_CONSTRUCT_PART_ELSE, 12, iff);
    (void)ADD(ODIN3_AST_NONBLOCKING_ASSIGN, 0, 14, then_arm);
}

void setUp(void) {
    errors[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_verilog"), &ctx));
    run1 = ctx.run;
    build_table();
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_status check_level(odin3_check_level level) {
    return odin3_check_design(design, (odin3_check_opts){level, ODIN3_VIEW_NONE});
}

static void expect_rule12(const char *part) {
    errors[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_level(ODIN3_CHECK_FULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(errors, "rule 12"), errors);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(errors, part), errors);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FAST)); /* FAST skips rule 12 */
}

static void test_wellformed_table_passes(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_STRING("", errors);
}

static void test_parent_must_be_earlier(void) {
    odin3_construct_test_row(design, 3)->parent = 3;
    expect_rule12("not an earlier row");
}

static void test_parent_after_the_row(void) {
    odin3_construct_test_row(design, 2)->parent = 6;
    expect_rule12("not an earlier row");
}

static void test_run_must_be_a_run(void) {
    odin3_construct_test_row(design, 2)->run = 99;
    expect_rule12("not a run of the design");
}

static void test_level_must_match_kind(void) {
    odin3_construct_test_row(design, 2)->level = ODIN3_LEVEL_STMT;
    expect_rule12("kind map refuses");
}

static void test_kind_out_of_range(void) {
    odin3_construct_test_row(design, 2)->kind = 200;
    expect_rule12("kind map refuses");
}

static void test_transparent_kind_has_no_row(void) {
    odin3_construct_test_row(design, 2)->kind = ODIN3_AST_LIST;
    odin3_construct_test_row(design, 2)->level = ODIN3_LEVEL_BLOCK;
    expect_rule12("kind map refuses");
}

static void test_part_must_match_kind(void) {
    odin3_construct_test_row(design, 6)->part = ODIN3_CONSTRUCT_PART_THEN;
    expect_rule12("needs");
}

static void test_arm_must_hang_under_its_if(void) {
    odin3_construct_test_row(design, 4)->parent = 2; /* the always, not the IF */
    expect_rule12("IF row");
}

static void test_arm_with_another_node_than_its_if(void) {
    odin3_construct_test_row(design, 5)->ast = 99;
    expect_rule12("IF row");
}

static void test_strings_must_be_ids(void) {
    odin3_construct_test_row(design, 2)->file = BAD_STR;
    expect_rule12("not strings");
}

static void test_name_must_be_an_id(void) {
    odin3_construct_test_row(design, 2)->name = BAD_STR;
    expect_rule12("not strings");
}

static void test_keys_must_be_unique(void) {
    odin3_construct_test_row(design, 6)->ast = 10; /* the key of row 2 */
    expect_rule12("shares its key");
}

static void test_call_must_be_earlier(void) {
    odin3_construct_test_row(design, 2)->call = 9;
    expect_rule12("not an earlier row");
}

/* --- completeness ---------------------------------------------------------------------------- */

static odin3_prov_id source(uint32_t ast) {
    odin3_pass_ctx ctx = {design, run1, 0};
    odin3_srcloc loc = {intern("a.v"), ast, 1, ast, 2, 0};
    odin3_prov_origin origin = {&loc, 1, ast, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static void test_covered_source_records_pass(void) {
    (void)source(14);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL));
}

static void test_uncovered_source_record_fails(void) {
    (void)source(77);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_REFS));
    expect_rule12("has no construct");
}

static void test_recorded_call_site_miss_passes(void) {
    odin3_prov_id miss = source(77);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_FULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_note_miss(design, miss));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL));
}

static void test_no_snapshot_means_nothing_is_checked(void) {
    (void)source(77);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL)); /* ast=none: vacuous */
}

static void test_constructs_mode_does_not_demand_expression_rows(void) {
    (void)source(77);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_CONSTRUCTS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL));
}

static void test_records_without_ast_are_not_demanded(void) {
    (void)source(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_level(ODIN3_CHECK_FULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wellformed_table_passes);
    RUN_TEST(test_parent_must_be_earlier);
    RUN_TEST(test_parent_after_the_row);
    RUN_TEST(test_run_must_be_a_run);
    RUN_TEST(test_level_must_match_kind);
    RUN_TEST(test_kind_out_of_range);
    RUN_TEST(test_transparent_kind_has_no_row);
    RUN_TEST(test_part_must_match_kind);
    RUN_TEST(test_arm_must_hang_under_its_if);
    RUN_TEST(test_arm_with_another_node_than_its_if);
    RUN_TEST(test_strings_must_be_ids);
    RUN_TEST(test_name_must_be_an_id);
    RUN_TEST(test_keys_must_be_unique);
    RUN_TEST(test_call_must_be_earlier);
    RUN_TEST(test_covered_source_records_pass);
    RUN_TEST(test_uncovered_source_record_fails);
    RUN_TEST(test_recorded_call_site_miss_passes);
    RUN_TEST(test_no_snapshot_means_nothing_is_checked);
    RUN_TEST(test_constructs_mode_does_not_demand_expression_rows);
    RUN_TEST(test_records_without_ast_are_not_demanded);
    return UNITY_END();
}
