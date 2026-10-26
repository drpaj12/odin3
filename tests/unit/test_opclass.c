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
#include "passes/opt/kit.h"
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

/* --- helpers: one assertion site each, so a test reads as a list of facts ------------------- */

static void expect_default(odin3_passrun_id run, uint32_t op, const odin3_opclass *want) {
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_default(design->opclass, run, op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, want));
}

static void expect_no_default(odin3_passrun_id run, uint32_t op) {
    odin3_opclass got;
    TEST_ASSERT_FALSE(odin3_opclass_default(design->opclass, run, op, &got));
}

static void expect_class_in(const odin3_design *where, odin3_prov_id rec, const odin3_pass_ctx *ctx,
                            const odin3_opclass *want) {
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_of(where->opclass, rec, ctx->run, ctx->op, &got));
    TEST_ASSERT_TRUE(odin3_opclass_equal(&got, want));
}

static void expect_class_of(odin3_prov_id rec, const odin3_pass_ctx *ctx,
                            const odin3_opclass *want) {
    expect_class_in(design, rec, ctx, want);
}

static void expect_declare(odin3_passrun_id run, uint32_t op, const odin3_opclass *cls,
                           odin3_status want) {
    TEST_ASSERT_EQUAL_INT(want, odin3_opclass_declare(design->opclass, run, op, cls));
}

static void expect_rows(uint32_t want) {
    TEST_ASSERT_EQUAL_UINT32(want, odin3_opclass_row_count(design->opclass));
}

static void expect_overrides(uint32_t want) {
    TEST_ASSERT_EQUAL_UINT32(want, odin3_opclass_override_count(design->opclass));
}

static void expect_check(odin3_status want, const char *log_part) {
    TEST_ASSERT_EQUAL_INT(want, check_full());
    if (log_part != NULL) {
        TEST_ASSERT_NOT_NULL(strstr(last_error, log_part));
    }
}

static void expect_invalid(odin3_prov_dir dir, odin3_level from, odin3_level to, uint8_t tail) {
    odin3_opclass cls = {dir, from, to, false, tail};
    TEST_ASSERT_FALSE(odin3_opclass_valid(&cls));
}

/* --- tests --------------------------------------------------------------------------------- */

static void test_valid(void) {
    TEST_ASSERT_TRUE(odin3_opclass_valid(&REFINE));
    TEST_ASSERT_TRUE(odin3_opclass_valid(&CONTEXT1));
    TEST_ASSERT_FALSE(odin3_opclass_valid(NULL));
}

static void test_equal(void) {
    TEST_ASSERT_TRUE(odin3_opclass_equal(&REFINE, &REFINE));
    TEST_ASSERT_FALSE(odin3_opclass_equal(&REFINE, &REWRITE));
    TEST_ASSERT_FALSE(odin3_opclass_equal(&COARSEN, &BULK));
}

static void test_invalid_classes(void) {
    expect_invalid(ODIN3_DIR_ORIGIN, ODIN3_LEVEL_WORD, ODIN3_LEVEL_GATE, 0);
    expect_invalid(ODIN3_DIR_REFINE, ODIN3_LEVEL_NONE, ODIN3_LEVEL_GATE, 0);
    expect_invalid(ODIN3_DIR_REFINE, ODIN3_LEVEL_WORD, (odin3_level)(ODIN3_LEVEL_SITE + 1), 0);
    expect_invalid(ODIN3_DIR_REFINE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_GATE,
                   1); /* tail: context only */
    expect_invalid((odin3_prov_dir)(ODIN3_DIR_CONTEXT + 1), ODIN3_LEVEL_WORD, ODIN3_LEVEL_GATE, 0);
}

static void test_declare_default(void) {
    odin3_pass_ctx ctx = run_named("lower");
    odin3_prov_begin_op_class(&ctx, &REFINE);
    expect_default(ctx.run, ctx.op, &REFINE);
    expect_no_default(ctx.run, ctx.op + 1);
    expect_no_default((odin3_passrun_id){ctx.run.v + 1}, 1);
}

static void test_declare_rules(void) {
    odin3_pass_ctx ctx = run_named("lower");
    odin3_opclass tail = CONTEXT1;
    tail.context_tail = TAIL_BIG;
    expect_declare(ctx.run, 7, &tail, ODIN3_OK);
    expect_declare(ctx.run, 7, &tail, ODIN3_OK); /* the same class again */
    expect_declare(ctx.run, 7, &REWRITE, ODIN3_ERR_INVALID_ARG);
    expect_declare(ctx.run, 0, &REWRITE, ODIN3_ERR_INVALID_ARG);
    expect_declare((odin3_passrun_id){0}, 3, &REWRITE, ODIN3_ERR_INVALID_ARG);
    expect_default(ctx.run, 7, &tail);
}

/* Rows are per change of class, not per operation: a run of N rewrites is one row. */
static void test_run_length_rows(void) {
    odin3_pass_ctx ctx = run_named("opt");
    for (uint32_t i = 0; i < MANY_OPS; i++) {
        odin3_prov_begin_op_class(&ctx, &REWRITE);
    }
    expect_rows(1);
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    expect_rows(2);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    expect_rows(3);
}

static void test_run_length_lookup(void) {
    odin3_pass_ctx ctx = run_named("opt");
    for (uint32_t i = 0; i < MANY_OPS; i++) {
        odin3_prov_begin_op_class(&ctx, &REWRITE);
    }
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    odin3_prov_begin_op_class(&ctx, &COARSEN);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    expect_default(ctx.run, 1, &REWRITE);
    expect_default(ctx.run, MANY_OPS, &REWRITE);
    expect_default(ctx.run, MANY_OPS + 1, &COARSEN);
    expect_default(ctx.run, MANY_OPS + 2, &COARSEN);
    expect_default(ctx.run, MANY_OPS + 3, &REWRITE);
}

static void test_another_run_starts_its_own_row(void) {
    odin3_pass_ctx first = run_named("opt");
    odin3_prov_begin_op_class(&first, &REWRITE);
    odin3_pass_ctx other = run_named("lower");
    odin3_prov_begin_op_class(&other, &REWRITE);
    expect_rows(2);
}

/* An undeclared begin in the middle of a run breaks the range: the next declared op is a new row.
 */
static void test_undeclared_op_breaks_range(void) {
    odin3_pass_ctx ctx = run_named("mix");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_begin_op_lazy(&ctx);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    expect_rows(2);
    expect_no_default(ctx.run, 2);
    expect_default(ctx.run, 3, &REWRITE);
}

static void test_default_derives_need_no_override(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id plain = derive_class(&ctx, src, NULL);
    odin3_prov_id same = derive_class(&ctx, src, &REWRITE); /* the same record, same class */
    TEST_ASSERT_EQUAL_UINT32(plain.v, same.v);
    expect_overrides(0);
    expect_class_of(plain, &ctx, &REWRITE);
}

static void test_override_differs_from_default(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id plain = derive_class(&ctx, leaf(&ctx, 1), NULL);
    odin3_prov_id merged = derive_class(&ctx, leaf(&ctx, 2), &COARSEN);
    expect_overrides(1);
    expect_class_of(merged, &ctx, &COARSEN);
    expect_class_of(plain, &ctx, &REWRITE);
    odin3_opclass got;
    TEST_ASSERT_FALSE(odin3_opclass_override_of(design->opclass, plain, &got));
    expect_check(ODIN3_OK, NULL);
}

static void test_lazy_begin_first_derive_is_default(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_lazy(&ctx);
    expect_no_default(ctx.run, ctx.op);
    odin3_prov_id first = derive_class(&ctx, src, &REWRITE);
    expect_default(ctx.run, ctx.op, &REWRITE);
    expect_overrides(0);
    TEST_ASSERT_TRUE(odin3_prov_valid(first));
    expect_check(ODIN3_OK, NULL);
}

static void test_missing_declaration_fails_rule_13(void) {
    odin3_pass_ctx ctx = run_named("legacy");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op(&ctx); /* no class */
    odin3_prov_id rec = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){&src, 1}, &rec));
    expect_check(ODIN3_ERR_CHECK, "has no class");
    TEST_ASSERT_NOT_NULL(strstr(last_error, "rule 13"));
}

static void test_fast_check_skips_rule_13(void) {
    odin3_pass_ctx ctx = run_named("legacy");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op(&ctx);
    (void)derive_class(&ctx, src, NULL);
    odin3_check_opts fast = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, fast));
}

static void test_origin_only_ops_need_no_class(void) {
    odin3_pass_ctx ctx = run_named("read");
    odin3_prov_begin_op(&ctx);
    (void)leaf(&ctx, 1);
    expect_check(ODIN3_OK, NULL);
}

static void test_two_classes_for_one_record_fails_rule_13(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    (void)derive_class(&ctx, src, NULL);
    (void)derive_class(&ctx, src, &COARSEN); /* the same record, another class */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_conflicts(design->opclass, &(odin3_prov_id){0}));
    expect_check(ODIN3_ERR_CHECK, "second class");
}

static void test_context_tail_legal_shapes(void) {
    odin3_pass_ctx ctx = run_named("flatten");
    odin3_prov_id orig = leaf(&ctx, 1);
    odin3_prov_id inst = leaf(&ctx, 2);
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    odin3_prov_id both[2] = {orig, inst};
    odin3_prov_id rec = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive_class(&ctx, (odin3_prov_list){both, 2}, 0, NULL, &rec));
    (void)derive_class(&ctx, inst, NULL); /* {inst} alone: tail 1, no data parent, legal */
    expect_check(ODIN3_OK, NULL);
}

static void test_context_tail_past_parents_fails_rule_13(void) {
    odin3_pass_ctx ctx = run_named("flatten");
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    odin3_opclass long_tail = CONTEXT1;
    long_tail.context_tail = 3; /* more than its one parent */
    odin3_prov_id bad = derive_class(&ctx, leaf(&ctx, 3), &long_tail);
    TEST_ASSERT_TRUE(odin3_prov_valid(bad));
    expect_check(ODIN3_ERR_CHECK, "context tail");
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
    expect_check(ODIN3_ERR_CHECK, "invalid class");
}

static void test_bulk_flag_kept(void) {
    odin3_pass_ctx ctx = run_named("abc");
    odin3_prov_begin_op_class(&ctx, &BULK);
    odin3_prov_id rec = derive_class(&ctx, leaf(&ctx, 1), NULL);
    odin3_opclass got;
    TEST_ASSERT_TRUE(odin3_opclass_of(design->opclass, rec, ctx.run, ctx.op, &got));
    TEST_ASSERT_TRUE(got.bulk);
    expect_check(ODIN3_OK, NULL);
}

static void test_copy_with_design(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id plain = derive_class(&ctx, leaf(&ctx, 1), NULL);
    odin3_prov_id over = derive_class(&ctx, leaf(&ctx, 2), &COARSEN);
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    expect_class_in(copy, plain, &ctx, &REWRITE);
    expect_class_in(copy, over, &ctx, &COARSEN);
    TEST_ASSERT_EQUAL_UINT32(odin3_opclass_row_count(design->opclass),
                             odin3_opclass_row_count(copy->opclass));
    odin3_design_destroy(copy);
}

static void test_copy_is_independent(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    odin3_pass_ctx more = {copy, ctx.run, ctx.op};
    odin3_prov_begin_op_class(&more, &COARSEN);
    TEST_ASSERT_EQUAL_UINT32(odin3_opclass_row_count(design->opclass) + 1,
                             odin3_opclass_row_count(copy->opclass));
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(copy, opts));
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

/* A begin that runs out of memory cannot report it: the next derive returns NO_MEMORY. */
static void test_oom_in_begin_fails_the_next_derive(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_util_set_alloc_fail_after(OOM_AFTER);
    for (uint32_t i = 0; i < 64; i++) { /* alternate so each op needs a row */
        odin3_prov_begin_op_class(&ctx, i % 2 == 0 ? &COARSEN : &REWRITE);
        if (!odin3_opclass_take_pending(design->opclass)) {
            continue;
        }
        odin3_opclass_set_pending(design->opclass, true);
        odin3_prov_id out = {0};
        uint32_t end = odin3_prov_end(design);
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                              odin3_prov_derive(&ctx, (odin3_prov_list){&src, 1}, &out));
        TEST_ASSERT_EQUAL_UINT32(end, odin3_prov_end(design)); /* nothing was made */
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_FALSE(odin3_opclass_failed(design->opclass));
        return;
    }
    TEST_FAIL_MESSAGE("no begin ran out of memory");
}

/* An override that cannot be stored fails the derive before it makes a record. */
static void test_oom_in_override_fails_before_the_record(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_id src = leaf(&ctx, 1);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_util_set_alloc_fail_after(OOM_AFTER);
    odin3_prov_id out = {0};
    uint32_t end = odin3_prov_end(design);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_NO_MEMORY,
        odin3_prov_derive_class(&ctx, (odin3_prov_list){&src, 1}, 0, &COARSEN, &out));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT32(end, odin3_prov_end(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

/* The copy of a non-empty table keeps its rows, overrides, failed flag and conflict count. */
static void test_copy_keeps_failed_and_conflicts(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    odin3_prov_id over = derive_class(&ctx, leaf(&ctx, 1), &COARSEN);
    odin3_opclass_note_failure(design->opclass);
    odin3_opclass_note_conflict(design->opclass, over);
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    TEST_ASSERT_TRUE(odin3_opclass_failed(copy->opclass));
    odin3_prov_id first = {0};
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_conflicts(copy->opclass, &first));
    TEST_ASSERT_EQUAL_UINT32(over.v, first.v);
    TEST_ASSERT_EQUAL_UINT32(odin3_opclass_override_count(design->opclass),
                             odin3_opclass_override_count(copy->opclass));
    odin3_design_destroy(copy);
}

/* A begin's held out of memory survives a later successful begin and ends with its run. */
static void test_pending_survives_a_good_begin_and_dies_with_the_run(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_opclass_set_pending(design->opclass, true);
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    TEST_ASSERT_TRUE(odin3_opclass_take_pending(design->opclass));
    odin3_opclass_set_pending(design->opclass, true);
    odin3_pass_ctx next = run_named("lower");
    odin3_prov_begin_op_class(&next, &REFINE);
    TEST_ASSERT_FALSE(odin3_opclass_take_pending(design->opclass));
}

/* odin3_kit_derive_coarsen: one record repeated is the op default (rewrite, no override); two
 * distinct records are coarsen. */
static void test_kit_derive_coarsen_repeated_record_takes_the_default(void) {
    odin3_pass_ctx ctx = run_named("opt");
    odin3_opt_ctx oc = {0};
    oc.pass = &ctx;
    odin3_kit_begin_rewrite(&oc);
    odin3_prov_id one = leaf(&ctx, 1);
    odin3_prov_id two = leaf(&ctx, 2);
    odin3_prov_id same[3] = {one, one, one};
    odin3_prov_id pair[2] = {one, two};
    odin3_prov_id rep = {0};
    odin3_prov_id mix = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_kit_derive_coarsen(&oc, (odin3_prov_list){same, 3}, &rep));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_opclass_override_count(design->opclass));
    expect_class_in(design, rep, &ctx, &odin3_kit_class_rewrite);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_kit_derive_coarsen(&oc, (odin3_prov_list){pair, 2}, &mix));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opclass_override_count(design->opclass));
    expect_class_in(design, mix, &ctx, &odin3_kit_class_coarsen);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid);
    RUN_TEST(test_equal);
    RUN_TEST(test_invalid_classes);
    RUN_TEST(test_declare_default);
    RUN_TEST(test_declare_rules);
    RUN_TEST(test_run_length_rows);
    RUN_TEST(test_run_length_lookup);
    RUN_TEST(test_another_run_starts_its_own_row);
    RUN_TEST(test_undeclared_op_breaks_range);
    RUN_TEST(test_default_derives_need_no_override);
    RUN_TEST(test_override_differs_from_default);
    RUN_TEST(test_lazy_begin_first_derive_is_default);
    RUN_TEST(test_missing_declaration_fails_rule_13);
    RUN_TEST(test_fast_check_skips_rule_13);
    RUN_TEST(test_origin_only_ops_need_no_class);
    RUN_TEST(test_two_classes_for_one_record_fails_rule_13);
    RUN_TEST(test_context_tail_legal_shapes);
    RUN_TEST(test_context_tail_past_parents_fails_rule_13);
    RUN_TEST(test_invalid_class_is_rejected_and_noted);
    RUN_TEST(test_bulk_flag_kept);
    RUN_TEST(test_copy_with_design);
    RUN_TEST(test_copy_is_independent);
    RUN_TEST(test_bytes_reserved_counts_table);
    RUN_TEST(test_oom_in_begin_fails_the_next_derive);
    RUN_TEST(test_oom_in_override_fails_before_the_record);
    RUN_TEST(test_copy_keeps_failed_and_conflicts);
    RUN_TEST(test_pending_survives_a_good_begin_and_dies_with_the_run);
    RUN_TEST(test_kit_derive_coarsen_repeated_record_takes_the_default);
    return UNITY_END();
}
