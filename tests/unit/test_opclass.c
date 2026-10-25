/*
 * test_opclass.c — unit tests for the op-class table (IR-12a): declare, run-length rows, lazy
 * begins, overrides, copy with the design, and check rule 13.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MANY_OPS = 1000, TAIL_BIG = 9, OOM_AFTER = 0 };

static odin3_design *design;
static size_t errors_logged;
static char last_error[256];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
        (void)strncpy(last_error, msg, sizeof last_error - 1);
    }
}

void setUp(void) {
    errors_logged = 0;
    last_error[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static const odin3_opclass REFINE = {ODIN3_DIR_REFINE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_GATE, false,
                                     0};
static const odin3_opclass REWRITE = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                      0};
static const odin3_opclass COARSEN = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, false,
                                      0};
static const odin3_opclass BULK = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, true, 0};
static const odin3_opclass CONTEXT1 = {ODIN3_DIR_CONTEXT, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                       1};

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_pass_ctx run_named(const char *name) {
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern(name), &ctx));
    return ctx;
}

/* A SOURCE record; n makes distinct ones. */
static odin3_prov_id leaf(odin3_pass_ctx *ctx, uint32_t line) {
    odin3_srcloc loc = {intern("a.v"), line, 1, line, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(ctx, &origin, &id));
    return id;
}

static odin3_prov_id derive_class(odin3_pass_ctx *ctx, odin3_prov_id parent,
                                  const odin3_opclass *cls) {
    odin3_prov_id out = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_derive_class(ctx, (odin3_prov_list){&parent, 1}, 0, cls, &out));
    return out;
}

static odin3_status check_full(void) {
    return odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE});
}

static void test_valid_and_equal(void) {
    TEST_ASSERT_TRUE(odin3_opclass_valid(&REFINE));
    TEST_ASSERT_TRUE(odin3_opclass_valid(&CONTEXT1));
    TEST_ASSERT_FALSE(odin3_opclass_valid(NULL));
    odin3_opclass bad = REFINE;
    bad.dir = ODIN3_DIR_ORIGIN;
    TEST_ASSERT_FALSE(odin3_opclass_valid(&bad));
    bad = REFINE;
    bad.from_level = ODIN3_LEVEL_NONE;
    TEST_ASSERT_FALSE(odin3_opclass_valid(&bad));
    bad = REFINE;
    bad.to_level = (odin3_level)(ODIN3_LEVEL_SITE + 1);
    TEST_ASSERT_FALSE(odin3_opclass_valid(&bad));
    bad = REFINE;
    bad.context_tail = 1; /* a tail only on a context class */
    TEST_ASSERT_FALSE(odin3_opclass_valid(&bad));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&REFINE, &REFINE));
    TEST_ASSERT_FALSE(odin3_opclass_equal(&REFINE, &REWRITE));
    TEST_ASSERT_FALSE(odin3_opclass_equal(&COARSEN, &BULK));
}

static void test_declare_default(void) {
    odin3_pass_ctx ctx = run_named("lower");
    odin3_prov_begin_op_class(&ctx, &REFINE);
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REFINE));
    TEST_ASSERT_FALSE(odin3_opclass_default(design->opclass, ctx.run, ctx.op + 1, &got));
    TEST_ASSERT_FALSE(
        odin3_opclass_default(design->opclass, (odin3_passrun_id){ctx.run.v + 1}, 1, &got));
    odin3_opclass tail = CONTEXT1;
    tail.context_tail = TAIL_BIG;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opclass_declare(design->opclass, ctx.run, 7, &tail));
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, 7, &got));
    TEST_ASSERT_EQUAL_UINT8(TAIL_BIG, got.context_tail);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opclass_declare(design->opclass, ctx.run, 7, &tail));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opclass_declare(design->opclass, ctx.run, 7, &REWRITE));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opclass_declare(design->opclass, ctx.run, 0, &REWRITE));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_opclass_declare(design->opclass, (odin3_passrun_id){0}, 3, &REWRITE));
}

/* Rows are per change of class, not per operation: a run of N rewrites is one row. */
static void test_run_length_rows(void) {
    odin3_pass_ctx ctx = run_named("opt");
    for (uint32_t i = 0; i < MANY_OPS; i++) {
        odin3_prov_begin_op_class(&ctx, &REWRITE);
    }
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_row_count(design->opclass));
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_opclass_row_count(design->opclass));
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_opclass_row_count(design->opclass));
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, 1, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, MANY_OPS, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, MANY_OPS + 1, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &COARSEN));
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, MANY_OPS + 3, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    /* another run starts its own row */
    odin3_pass_ctx other = run_named("lower");
    odin3_prov_begin_op_class(&other, &REWRITE);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_opclass_row_count(design->opclass));
}

/* An undeclared begin in the middle of a run breaks the range: the next declared op is a new row.
 */
static void test_undeclared_op_breaks_range(void) {
    odin3_pass_ctx ctx = run_named("mix");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_begin_op_lazy(&ctx);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_opclass_row_count(design->opclass));
    odin3_opclass got;
    TEST_ASSERT_FALSE(odin3_opclass_default(design->opclass, ctx.run, 2, &got));
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, 3, &got));
}

static void test_default_and_override(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id plain = derive_class(&ctx, src, NULL);
    odin3_prov_id same = derive_class(&ctx, src, &REWRITE); /* the same record, same class */
    TEST_ASSERT_EQUAL_UINT32(plain.v, same.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_opclass_override_count(design->opclass));
    odin3_prov_id src2 = leaf(&ctx, 2);
    odin3_prov_id merged = derive_class(&ctx, src2, &COARSEN);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_override_count(design->opclass));
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_of(design->opclass, merged, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &COARSEN));
    TEST_ASSERT_TRUE(odin3_opclass_of(design->opclass, plain, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    TEST_ASSERT_FALSE(odin3_opclass_override_of(design->opclass, plain, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

static void test_lazy_begin_first_derive_is_default(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_lazy(&ctx);
    odin3_opclass got;
    TEST_ASSERT_FALSE(odin3_opclass_default(design->opclass, ctx.run, ctx.op, &got));
    odin3_prov_id first = derive_class(&ctx, src, &REWRITE);
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_opclass_override_count(design->opclass));
    TEST_ASSERT_TRUE(odin3_prov_valid(first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

static void test_missing_declaration_fails_rule_13(void) {
    odin3_pass_ctx ctx = run_named("legacy");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op(&ctx); /* no class */
    odin3_prov_id rec = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){&src, 1}, &rec));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_full());
    TEST_ASSERT_NOT_NULL(strstr(last_error, "rule 13"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "has no class"));
    /* FAST skips rule 13 */
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FAST, ODIN3_VIEW_NONE}));
}

static void test_origin_only_ops_need_no_class(void) {
    odin3_pass_ctx ctx = run_named("read");
    odin3_prov_begin_op(&ctx);
    (void)leaf(&ctx, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

static void test_two_classes_for_one_record_fails_rule_13(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    (void)derive_class(&ctx, src, NULL);
    (void)derive_class(&ctx, src, &COARSEN); /* the same record, another class */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_conflicts(design->opclass, &(odin3_prov_id){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_full());
    TEST_ASSERT_NOT_NULL(strstr(last_error, "second class"));
}

static void test_context_tail_rules(void) {
    odin3_pass_ctx ctx = run_named("flatten");
    odin3_prov_id orig = leaf(&ctx, 1);
    odin3_prov_id inst = leaf(&ctx, 2);
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    odin3_prov_id both[2] = {orig, inst};
    odin3_prov_id rec = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive_class(&ctx, (odin3_prov_list){both, 2}, 0, NULL, &rec));
    (void)derive_class(&ctx, inst, NULL); /* {inst} alone: tail 1, no data parent, legal */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
    odin3_opclass long_tail = CONTEXT1;
    long_tail.context_tail = 3; /* more than its two parents */
    odin3_prov_id bad = derive_class(&ctx, leaf(&ctx, 3), &long_tail);
    TEST_ASSERT_TRUE(odin3_prov_valid(bad));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_full());
    TEST_ASSERT_NOT_NULL(strstr(last_error, "context tail"));
}

static void test_invalid_class_is_rejected_and_noted(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_opclass bad = REWRITE;
    bad.from_level = ODIN3_LEVEL_NONE;
    odin3_prov_begin_op_class(&ctx, &bad); /* cannot fail: noted for check */
    TEST_ASSERT_TRUE(odin3_opclass_failed(design->opclass));
    odin3_prov_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_derive_class(&ctx, (odin3_prov_list){&src, 1}, 0, &bad, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_full());
}

static void test_bulk_flag_kept(void) {
    odin3_pass_ctx ctx = run_named("abc");
    odin3_prov_begin_op_class(&ctx, &BULK);
    odin3_prov_id rec = derive_class(&ctx, leaf(&ctx, 1), NULL);
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_of(design->opclass, rec, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(got.bulk);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

static void test_copy_with_design(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id plain = derive_class(&ctx, leaf(&ctx, 1), NULL);
    odin3_prov_id over = derive_class(&ctx, leaf(&ctx, 2), &COARSEN);
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    TEST_ASSERT_NOT_NULL(copy->opclass);
    TEST_ASSERT_TRUE(copy->opclass != design->opclass);
    TEST_ASSERT_EQUAL_UINT32(odin3_opclass_row_count(design->opclass),
                             odin3_opclass_row_count(copy->opclass));
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_of(copy->opclass, plain, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &REWRITE));
    TEST_ASSERT_TRUE(odin3_opclass_of(copy->opclass, over, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, &COARSEN));
    /* the copy is independent */
    odin3_pass_ctx more = {copy, ctx.run, ctx.op};
    odin3_prov_begin_op_class(&more, &COARSEN);
    TEST_ASSERT_EQUAL_UINT32(odin3_opclass_row_count(design->opclass) + 1,
                             odin3_opclass_row_count(copy->opclass));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_check_design(copy, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    odin3_design_destroy(copy);
}

static void test_bytes_reserved_counts_table(void) {
    odin3_pass_ctx ctx = run_named("opt");
    uint64_t before = odin3_design_bytes_reserved(design);
    for (uint32_t i = 0; i < 4; i++) { /* four rows: classes alternate */
        odin3_prov_begin_op_class(&ctx, i % 2 == 0 ? &REWRITE : &COARSEN);
    }
    TEST_ASSERT_TRUE(odin3_design_bytes_reserved(design) > before);
    TEST_ASSERT_TRUE(odin3_opclass_bytes_reserved(design->opclass) > 0);
    TEST_ASSERT_EQUAL_size_t(0, odin3_opclass_bytes_reserved(NULL));
}

static void test_oom_in_begin_is_noted(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_util_set_alloc_fail_after(OOM_AFTER);
    for (uint32_t i = 0; i < 64; i++) { /* alternate so each op needs a row */
        odin3_prov_begin_op_class(&ctx, i % 2 == 0 ? &REWRITE : &COARSEN);
    }
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(odin3_opclass_failed(design->opclass));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, check_full());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_and_equal);
    RUN_TEST(test_declare_default);
    RUN_TEST(test_run_length_rows);
    RUN_TEST(test_undeclared_op_breaks_range);
    RUN_TEST(test_default_and_override);
    RUN_TEST(test_lazy_begin_first_derive_is_default);
    RUN_TEST(test_missing_declaration_fails_rule_13);
    RUN_TEST(test_origin_only_ops_need_no_class);
    RUN_TEST(test_two_classes_for_one_record_fails_rule_13);
    RUN_TEST(test_context_tail_rules);
    RUN_TEST(test_invalid_class_is_rejected_and_noted);
    RUN_TEST(test_bulk_flag_kept);
    RUN_TEST(test_copy_with_design);
    RUN_TEST(test_bytes_reserved_counts_table);
    RUN_TEST(test_oom_in_begin_is_noted);
    return UNITY_END();
}
