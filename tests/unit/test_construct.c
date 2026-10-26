/*
 * test_construct.c — unit tests for the construct table (IR-12a, V1.2.0 Task 3): the total kind
 * map, add and find, arm and call-instance keys, record resolution, runs and misses, design copy,
 * compact, the byte budget and out of memory. Run under ASan.
 */
#include "ast/kinds.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MANY_ROWS = 500, KIND_STMT_COUNT = 26 };

static odin3_design *design;
static odin3_passrun_id run1;
static size_t errors_logged;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    (void)msg;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_verilog"), &ctx));
    run1 = ctx.run;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

typedef struct row_args {
    uint32_t kind;
    uint32_t ast;
    uint32_t parent;
    uint32_t line;
} row_args;

/* ROW(kind, ast, parent, line): a row of that kind with a 1-line span; level from the kind map. */
#define ROW(...) make_row((row_args){__VA_ARGS__})

static odin3_construct_row make_row(row_args args) {
    odin3_construct_row row = {0};
    row.run = run1.v;
    row.ast = args.ast;
    row.parent = args.parent;
    row.kind = (uint8_t)args.kind;
    row.level = (uint8_t)odin3_construct_kind_level(args.kind);
    row.file = intern("a.v");
    row.line = args.line;
    row.col = 1;
    row.end_line = args.line;
    row.end_col = 9;
    return row;
}

/* FIND(design, run, ast, part, call): the row with that key, 0 when none. */
#define FIND(where, run, ...) find_key(where, (odin3_construct_key){run, __VA_ARGS__})

static uint32_t find_key(const odin3_design *where, odin3_construct_key key) {
    return odin3_construct_find(where, &key);
}

static uint32_t add(odin3_construct_row row) {
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &index));
    return index;
}

static void expect_refused(odin3_construct_row row) {
    uint32_t index = 77;
    size_t before = errors_logged;
    uint32_t end = odin3_construct_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_add(design, &row, &index));
    TEST_ASSERT_EQUAL_UINT32(77, index);
    TEST_ASSERT_EQUAL_UINT32(end, odin3_construct_end(design));
    TEST_ASSERT_EQUAL_size_t(before + 1, errors_logged);
}

/* --- the kind map (V1.2.0 2.1) ---------------------------------------------------------------- */

typedef struct kind_counts {
    uint32_t per_level[ODIN3_LEVEL_SITE + 1];
    uint32_t transparent;
} kind_counts;

/* Counts one kind in its class (the level's slot, or the transparent count). */
static void count_kind(uint32_t kind, kind_counts *counts) {
    const char *name = odin3_ast_kind_name(kind);
    TEST_ASSERT_TRUE_MESSAGE(name != NULL && name[0] != '\0', "kind has a name");
    odin3_construct_kind_class cls = odin3_construct_kind_class_of(kind);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(ODIN3_KCLASS_UNKNOWN, cls, name);
    if (cls == ODIN3_KCLASS_TRANSPARENT) {
        counts->transparent++;
        TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_NONE, odin3_construct_kind_level(kind));
    } else {
        counts->per_level[odin3_construct_kind_level(kind)]++;
    }
}

static void test_kind_map_is_total(void) {
    kind_counts counts = {{0}, 0};
    for (uint32_t kind = 1; kind < ODIN3_AST_KIND_COUNT; kind++) {
        count_kind(kind, &counts);
    }
    TEST_ASSERT_EQUAL_UINT32(4, counts.transparent); /* UNIT, DIRECTIVE, LIST, ATTR */
    TEST_ASSERT_EQUAL_UINT32(3, counts.per_level[ODIN3_LEVEL_MODULE]);
    TEST_ASSERT_EQUAL_UINT32(3, counts.per_level[ODIN3_LEVEL_INSTANCE]);
    TEST_ASSERT_EQUAL_UINT32(12, counts.per_level[ODIN3_LEVEL_BLOCK]);
    TEST_ASSERT_EQUAL_UINT32(1,
                             counts.per_level[ODIN3_LEVEL_BRANCH]); /* CASE_ITEM; arms synthetic */
    TEST_ASSERT_EQUAL_UINT32(KIND_STMT_COUNT, counts.per_level[ODIN3_LEVEL_STMT]);
    TEST_ASSERT_EQUAL_UINT32(19, counts.per_level[ODIN3_LEVEL_EXPR]);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_KIND_COUNT - 1U,
                             counts.transparent + 3 + 3 + 12 + 1 + 26 + 19);
}

static void test_kind_map_specific_levels(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_MODULE, odin3_construct_kind_level(ODIN3_AST_MODULE));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_INSTANCE, odin3_construct_kind_level(ODIN3_AST_INSTANCE));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_BLOCK, odin3_construct_kind_level(ODIN3_AST_ALWAYS));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_BRANCH, odin3_construct_kind_level(ODIN3_AST_CASE_ITEM));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_STMT, odin3_construct_kind_level(ODIN3_AST_IF));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_STMT, odin3_construct_kind_level(ODIN3_AST_TIMING_STMT));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_EXPR, odin3_construct_kind_level(ODIN3_AST_BINARY));
}

static void test_synthesized_kinds_are_classed(void) {
    TEST_ASSERT_EQUAL_UINT32(0x80, ODIN3_CONSTRUCT_IF_THEN);
    TEST_ASSERT_EQUAL_UINT32(0x81, ODIN3_CONSTRUCT_IF_ELSE);
    TEST_ASSERT_EQUAL_UINT32(0x82, ODIN3_CONSTRUCT_CALL_SITE);
    TEST_ASSERT_EQUAL_INT(ODIN3_KCLASS_UNKNOWN,
                          odin3_construct_kind_class_of(ODIN3_AST_KIND_COUNT));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_BRANCH, odin3_construct_kind_level(ODIN3_CONSTRUCT_IF_THEN));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_BRANCH, odin3_construct_kind_level(ODIN3_CONSTRUCT_IF_ELSE));
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_EXPR, odin3_construct_kind_level(ODIN3_CONSTRUCT_CALL_SITE));
    TEST_ASSERT_EQUAL_INT(ODIN3_KCLASS_UNKNOWN, odin3_construct_kind_class_of(ODIN3_AST_NONE));
    TEST_ASSERT_EQUAL_INT(ODIN3_KCLASS_UNKNOWN,
                          odin3_construct_kind_class_of(ODIN3_CONSTRUCT_CALL_SITE + 1U));
    TEST_ASSERT_EQUAL_INT(ODIN3_KCLASS_UNKNOWN, odin3_construct_kind_class_of(0xFFFFFFFFU));
}

/* --- add, find, get -------------------------------------------------------------------------- */

static void test_empty_table(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_construct_end(design));
    TEST_ASSERT_NULL(odin3_construct_get(design, 0));
    TEST_ASSERT_NULL(odin3_construct_get(design, 1));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, 5, 0, 0));
    TEST_ASSERT_FALSE(odin3_construct_has_node(design, run1, 5));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_construct_count(design));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_construct_count(NULL));
}

static void test_add_find_get(void) {
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    uint32_t always = add(ROW(ODIN3_AST_ALWAYS, 10, mod, 2));
    uint32_t iff = add(ROW(ODIN3_AST_IF, 12, always, 3));
    TEST_ASSERT_EQUAL_UINT32(1, mod);
    TEST_ASSERT_EQUAL_UINT32(3, iff);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_construct_end(design));
    TEST_ASSERT_EQUAL_UINT32(iff, FIND(design, run1, 12, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(mod, FIND(design, run1, 3, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, 13, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, (odin3_passrun_id){99}, 12, 0, 0));
    const odin3_construct_row *row = odin3_construct_get(design, always);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_UINT32(mod, row->parent);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_LEVEL_BLOCK, row->level);
    TEST_ASSERT_EQUAL_UINT32(10, row->ast);
    TEST_ASSERT_NULL(odin3_construct_get(design, 4));
    TEST_ASSERT_TRUE(odin3_construct_has_node(design, run1, 12));
}

static void test_arm_rows_have_their_own_key(void) {
    uint32_t iff = add(ROW(ODIN3_AST_IF, 12, 0 + 0, 3));
    odin3_construct_row then_arm = ROW(ODIN3_CONSTRUCT_IF_THEN, 12, iff, 4);
    then_arm.part = ODIN3_CONSTRUCT_PART_THEN;
    odin3_construct_row else_arm = ROW(ODIN3_CONSTRUCT_IF_ELSE, 12, iff, 6);
    else_arm.part = ODIN3_CONSTRUCT_PART_ELSE;
    uint32_t then_row = add(then_arm);
    uint32_t else_row = add(else_arm);
    TEST_ASSERT_EQUAL_UINT32(iff, FIND(design, run1, 12, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(then_row, FIND(design, run1, 12, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(else_row, FIND(design, run1, 12, 2, 0));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, 12, 3, 0));
}

static void test_call_instances_share_a_node(void) {
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    uint32_t body = add(ROW(ODIN3_AST_FUNCTION_DECL, 20, mod, 2));
    uint32_t site_a = add(ROW(ODIN3_CONSTRUCT_CALL_SITE, 31, mod, 30));
    uint32_t site_b = add(ROW(ODIN3_CONSTRUCT_CALL_SITE, 41, mod, 40));
    odin3_construct_row copy_a = ROW(ODIN3_AST_FUNCTION_DECL, 20, site_a, 2);
    copy_a.call = site_a;
    odin3_construct_row copy_b = ROW(ODIN3_AST_FUNCTION_DECL, 20, site_b, 2);
    copy_b.call = site_b;
    uint32_t row_a = add(copy_a);
    uint32_t row_b = add(copy_b);
    TEST_ASSERT_EQUAL_UINT32(body, FIND(design, run1, 20, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(row_a, FIND(design, run1, 20, 0, site_a));
    TEST_ASSERT_EQUAL_UINT32(row_b, FIND(design, run1, 20, 0, site_b));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, 20, 0, mod));
}

static void test_add_refusals(void) {
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    expect_refused(ROW(ODIN3_AST_MODULE, 3, 0, 1));        /* the key exists */
    expect_refused(ROW(ODIN3_AST_ALWAYS, 10, 2, 2));       /* parent is not an earlier row */
    expect_refused(ROW(ODIN3_AST_ALWAYS, 10, mod + 1, 2)); /* parent is the row itself */
    expect_refused(ROW(ODIN3_AST_LIST, 10, mod, 2));       /* transparent: no row */
    expect_refused(ROW(ODIN3_AST_NONE, 10, mod, 2));
    odin3_construct_row bad_level = ROW(ODIN3_AST_ALWAYS, 10, mod, 2);
    bad_level.level = ODIN3_LEVEL_STMT;
    expect_refused(bad_level);
    odin3_construct_row bad_part = ROW(ODIN3_AST_ALWAYS, 10, mod, 2);
    bad_part.part = 1;
    expect_refused(bad_part);
    odin3_construct_row arm_without_part = ROW(ODIN3_CONSTRUCT_IF_THEN, 10, mod, 2);
    expect_refused(arm_without_part);
    odin3_construct_row bad_call = ROW(ODIN3_AST_ALWAYS, 10, mod, 2);
    bad_call.call = 9;
    expect_refused(bad_call);
    expect_refused(ROW(ODIN3_AST_ALWAYS, 0, mod, 2)); /* ast 0 */
    odin3_construct_row no_run = ROW(ODIN3_AST_ALWAYS, 10, mod, 2);
    no_run.run = 0;
    expect_refused(no_run);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_construct_end(design));
}

static void test_add_null_arguments(void) {
    odin3_construct_row row = ROW(ODIN3_AST_MODULE, 3, 0, 1);
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_add(NULL, &row, &index));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_add(design, NULL, &index));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_add(design, &row, NULL));
}

/* --- records resolve to rows ----------------------------------------------------------------- */

typedef struct source_args {
    uint32_t ast;
    uint32_t n_locs;
    uint32_t line;
} source_args;

/* SRC(ctx, ast, n_locs, line): a SOURCE record of node ast; a second location is the call site. */
#define SRC(ctx, ...) source((ctx), (source_args){__VA_ARGS__})

static odin3_prov_id source(odin3_pass_ctx *ctx, source_args args) {
    odin3_srcloc locs[2] = {{intern("a.v"), args.line, 1, args.line, 9, 0},
                            {intern("a.v"), 30, 1, 30, 9, 0}};
    odin3_prov_origin origin = {locs, args.n_locs, args.ast, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(ctx, &origin, &id));
    return id;
}

static void test_of_record_plain_and_call_site(void) {
    odin3_pass_ctx ctx = {design, run1, 0};
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    uint32_t body_stmt = add(ROW(ODIN3_AST_BLOCKING_ASSIGN, 21, mod, 2));
    odin3_construct_row site = ROW(ODIN3_AST_CALL, 31, mod, 30);
    uint32_t site_row = add(site);
    odin3_construct_row copy = ROW(ODIN3_AST_BLOCKING_ASSIGN, 21, site_row, 2);
    copy.call = site_row;
    uint32_t copy_row = add(copy);
    odin3_prov_id plain = SRC(&ctx, 3, 1, 1);
    odin3_prov_id none = SRC(&ctx, 0, 1, 7);
    odin3_prov_id call_site = SRC(&ctx, 21, 2, 2);
    odin3_prov_id miss = SRC(&ctx, 22, 1, 8);
    TEST_ASSERT_EQUAL_UINT32(mod, odin3_construct_of_record(design, plain));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_construct_of_record(design, none));
    TEST_ASSERT_EQUAL_UINT32(copy_row, odin3_construct_of_record(design, call_site));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_construct_of_record(design, miss));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_construct_of_record(design, (odin3_prov_id){0}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_construct_of_record(design, (odin3_prov_id){999}));
    TEST_ASSERT_TRUE(body_stmt != copy_row);
}

static void test_runs_and_misses(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_OFF, odin3_construct_run_mode(design, run1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_FULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS, odin3_construct_run_mode(design, run1));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_construct_mark_run(design, (odin3_passrun_id){0}, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_OFF));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_note_miss(design, (odin3_prov_id){4}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_note_miss(design, (odin3_prov_id){9}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_note_miss(design, (odin3_prov_id){9}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_note_miss(design, (odin3_prov_id){2}));
    TEST_ASSERT_TRUE(odin3_construct_is_miss(design, (odin3_prov_id){4}));
    TEST_ASSERT_TRUE(odin3_construct_is_miss(design, (odin3_prov_id){9}));
    TEST_ASSERT_FALSE(odin3_construct_is_miss(design, (odin3_prov_id){5}));
}

/* --- copy, compact, budget, out of memory ---------------------------------------------------- */

static void test_design_clone_copies_the_table(void) {
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    (void)add(ROW(ODIN3_AST_ALWAYS, 10, mod, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, run1, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_note_miss(design, (odin3_prov_id){3}));
    odin3_design *dup = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &dup));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_construct_end(dup));
    TEST_ASSERT_EQUAL_UINT32(2, FIND(dup, run1, 10, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(mod, odin3_construct_get(dup, 2)->parent);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS, odin3_construct_run_mode(dup, run1));
    TEST_ASSERT_TRUE(odin3_construct_is_miss(dup, (odin3_prov_id){3}));
    /* independent: a row added to the copy is not in the original */
    odin3_construct_row more = ROW(ODIN3_AST_INITIAL, 11, mod, 5);
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(dup, &more, &index));
    TEST_ASSERT_EQUAL_UINT32(3, index);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_construct_end(design));
    TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, 11, 0, 0));
    odin3_design_destroy(dup);
}

static void test_compact_leaves_the_table_alone(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("top"), (odin3_prov_id){0}, &mid));
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    (void)add(ROW(ODIN3_AST_ALWAYS, 10, mod, 2));
    size_t bytes = odin3_construct_bytes_reserved(design);
    odin3_compact_map map = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(odin3_module_get(design, mid), &map));
    odin3_compact_map_free(&map);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_construct_end(design));
    TEST_ASSERT_EQUAL_UINT32(2, FIND(design, run1, 10, 0, 0));
    TEST_ASSERT_EQUAL_size_t(bytes, odin3_construct_bytes_reserved(design));
}

static void test_budget_counts_the_table(void) {
    uint64_t empty = odin3_design_bytes_reserved(design);
    size_t table_empty = odin3_construct_bytes_reserved(design);
    TEST_ASSERT_TRUE(table_empty > 0);
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    for (uint32_t i = 0; i < MANY_ROWS; i++) {
        (void)add(ROW(ODIN3_AST_BLOCKING_ASSIGN, 100 + i, mod, 2 + i));
    }
    size_t table_full = odin3_construct_bytes_reserved(design);
    TEST_ASSERT_TRUE(table_full >= table_empty + ((size_t)MANY_ROWS * sizeof(odin3_construct_row)));
    uint64_t full = odin3_design_bytes_reserved(design);
    TEST_ASSERT_TRUE(full - empty >= table_full - table_empty);
    TEST_ASSERT_EQUAL_size_t(0, odin3_construct_bytes_reserved(NULL));
    TEST_ASSERT_EQUAL_size_t(48, sizeof(odin3_construct_row));
}

static void test_add_out_of_memory_changes_nothing(void) {
    uint32_t mod = add(ROW(ODIN3_AST_MODULE, 3, 0, 1));
    for (long after = 0; after < 6; after++) {
        odin3_construct_row row = ROW(ODIN3_AST_ALWAYS, 10 + (uint32_t)after, mod, 2);
        uint32_t end = odin3_construct_end(design);
        uint32_t index = 0;
        odin3_util_set_alloc_fail_after(after);
        odin3_status st = odin3_construct_add(design, &row, &index);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_UINT32(end, index);
            continue;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(end, odin3_construct_end(design));
        TEST_ASSERT_EQUAL_UINT32(0, FIND(design, run1, row.ast, 0, 0));
        TEST_ASSERT_EQUAL_UINT32(end, add(row)); /* and it works afterwards */
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kind_map_is_total);
    RUN_TEST(test_kind_map_specific_levels);
    RUN_TEST(test_synthesized_kinds_are_classed);
    RUN_TEST(test_empty_table);
    RUN_TEST(test_add_find_get);
    RUN_TEST(test_arm_rows_have_their_own_key);
    RUN_TEST(test_call_instances_share_a_node);
    RUN_TEST(test_add_refusals);
    RUN_TEST(test_add_null_arguments);
    RUN_TEST(test_of_record_plain_and_call_site);
    RUN_TEST(test_runs_and_misses);
    RUN_TEST(test_design_clone_copies_the_table);
    RUN_TEST(test_compact_leaves_the_table_alone);
    RUN_TEST(test_budget_counts_the_table);
    RUN_TEST(test_add_out_of_memory_changes_nothing);
    return UNITY_END();
}
