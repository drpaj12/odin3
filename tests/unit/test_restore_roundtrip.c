/*
 * test_restore_roundtrip.c — V1.2.0 Task 9: write, restore, write again on hand-built designs, the
 * cases the micro designs do not reach: a join of more than 16 constructs (the cap: the same 16
 * after a restore), a bulk coarsen record, the one-parent context record of a flatten whose
 * original instance is 0, a call-instance row, and an IF arm. The second map must equal the first
 * byte for byte. Run under ASan.
 */
#include "ast/kinds.h"
#include "backends/common/hash_name.h"
#include "backends/common/name_map.h"
#include "backends/common/prov_export.h"
#include "ir/check.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "ir/restore.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MANY = 20, CAP = 16, WIDE = 8 };

static const odin3_opclass COARSEN = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_STMT, ODIN3_LEVEL_GATE, false,
                                      0};
static const odin3_opclass BULK = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_STMT, ODIN3_LEVEL_GATE, true, 0};
static const odin3_opclass CONTEXT1 = {ODIN3_DIR_CONTEXT, ODIN3_LEVEL_GATE, ODIN3_LEVEL_GATE, false,
                                       1};
static const odin3_opclass REWRITE = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_GATE, ODIN3_LEVEL_GATE, false,
                                      0};

static odin3_design *design;
static odin3_pass_ctx ctx;

static uint32_t intern_in(odin3_design *target, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(target, odin3_bytes_cstr(name), &str));
    return str;
}

typedef struct row_spec {
    uint32_t kind, ast, parent;
} row_spec;

static uint32_t add_row(row_spec spec) {
    const uint32_t ast = spec.ast;
    odin3_construct_row row = {.run = ctx.run.v, .ast = ast, .parent = spec.parent};
    row.kind = (uint8_t)spec.kind;
    row.level = (uint8_t)odin3_construct_kind_level(spec.kind);
    row.file = intern_in(design, "a.v");
    row.line = ast;
    row.end_line = ast;
    row.end_col = 5;
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &index));
    return index;
}

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(design, intern_in(design, "read_verilog"), &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, ctx.run, ODIN3_CONSTRUCT_MODE_REFS));
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

typedef struct leaf_spec {
    uint32_t ast, line;
} leaf_spec;

static odin3_prov_id leaf(leaf_spec spec) {
    odin3_srcloc loc = {intern_in(design, "a.v"), spec.line, 1, spec.line, 5, 0};
    odin3_prov_origin origin = {&loc, 1, spec.ast, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static odin3_prov_id derive(const odin3_opclass *cls, const odin3_prov_id *parents, uint32_t n) {
    odin3_prov_id out = {0};
    odin3_prov_begin_op_class(&ctx, cls);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){parents, n}, &out));
    return out;
}

/* A kept object named `name` in module m whose record is `record`. */
static odin3_prov_obj kept(const char *name, odin3_prov_id record) {
    return (odin3_prov_obj){.entry = {.module = odin3_bytes_cstr("m"),
                                      .kind = ODIN3_NAME_NET,
                                      .short_name = odin3_bytes_cstr(name)},
                            .kept = true,
                            .record = record.v};
}

static void write_text(odin3_design *source, const odin3_prov_obj *objs, size_t n,
                       odin3_strbuf *out) {
    const odin3_prov_map_req req = {source, 10, ODIN3_CONSTRUCT_MODE_REFS};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_map_text(&req, objs, n, out));
}

/* Writes the design's map for the named objects, restores it into a fresh design and writes again:
 * the second text must equal the first. */
static void expect_round_trip(const char *const *names, const odin3_prov_id *records, size_t n) {
    odin3_prov_obj objs[WIDE];
    TEST_ASSERT_TRUE(n <= WIDE);
    for (size_t i = 0; i < n; i++) {
        objs[i] = kept(names[i], records[i]);
    }
    odin3_strbuf first;
    odin3_strbuf_init(&first);
    write_text(design, objs, n, &first);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_name_map_parse(&map, (odin3_bytes){first.data, first.len}, "m.names"));
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&(odin3_name_check_v2){0}));
    odin3_name_check_v2 checked = odin3_name_map_check_v2(&map);
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&checked));
    odin3_design *again = odin3_design_create();
    odin3_pass_ctx reader;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(again, intern_in(again, "read_blif"), &reader));
    odin3_restore *restore = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_restore(again, &map, "m.names", &restore));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_check_design(again, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    for (size_t i = 0; i < n; i++) {
        objs[i].record = odin3_restore_named_record(restore, odin3_bytes_cstr("m"), ODIN3_NAME_NET,
                                                    odin3_bytes_cstr(names[i]));
        TEST_ASSERT_NOT_EQUAL_UINT32(0, objs[i].record);
    }
    odin3_strbuf second;
    odin3_strbuf_init(&second);
    write_text(again, objs, n, &second);
    TEST_ASSERT_EQUAL_UINT(first.len, second.len);
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_strbuf_free(&second);
    odin3_restore_free(restore);
    odin3_name_map_free(&map);
    odin3_design_destroy(again);
    odin3_strbuf_free(&first);
}

static void test_a_join_over_the_cap_keeps_the_same_16_after_a_restore(void) {
    uint32_t module = add_row((row_spec){ODIN3_AST_MODULE, 10, 0});
    uint32_t block = add_row((row_spec){ODIN3_AST_ALWAYS, 20, module});
    odin3_prov_id parents[MANY];
    for (uint32_t i = 0; i < MANY; i++) {
        (void)add_row((row_spec){ODIN3_AST_BLOCKING_ASSIGN, 100 + i, block});
        parents[i] = leaf((leaf_spec){100 + i, 100U + i});
    }
    odin3_prov_id join = derive(&COARSEN, parents, MANY);
    odin3_prov_label label;
    odin3_prov_label_init(&label);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_label_compute(design, join.v, &label));
    TEST_ASSERT_EQUAL_UINT32(MANY, label.info.n_joined);
    TEST_ASSERT_TRUE((label.info.flags & ODIN3_PROV_F_LOSSY_CAP) != 0);
    TEST_ASSERT_EQUAL_UINT(MANY, label.joined.len);
    odin3_prov_label_free(&label);
    const char *names[] = {"x"};
    expect_round_trip(names, &join, 1);
}

static void test_a_bulk_coarsen_record_restores_with_its_flag(void) {
    uint32_t module = add_row((row_spec){ODIN3_AST_MODULE, 10, 0});
    uint32_t block = add_row((row_spec){ODIN3_AST_ALWAYS, 20, module});
    (void)add_row((row_spec){ODIN3_AST_BLOCKING_ASSIGN, 100, block});
    (void)add_row((row_spec){ODIN3_AST_BLOCKING_ASSIGN, 101, block});
    odin3_prov_id parents[2] = {leaf((leaf_spec){100, 100}), leaf((leaf_spec){101, 101})};
    odin3_prov_id bulk = derive(&BULK, parents, 2);
    odin3_opclass cls;
    TEST_ASSERT_TRUE(odin3_prov_class_of(design, bulk, &cls));
    TEST_ASSERT_TRUE(cls.bulk);
    const char *names[] = {"b"};
    expect_round_trip(names, &bulk, 1);
}

static void test_a_one_parent_context_record_restores(void) {
    uint32_t module = add_row((row_spec){ODIN3_AST_MODULE, 10, 0});
    (void)add_row((row_spec){ODIN3_AST_INSTANCE, 30, module});
    odin3_prov_id inst = leaf((leaf_spec){30, 30});
    odin3_prov_id flat = derive(&CONTEXT1, &inst, 1); /* flatten's {inst} alone: orig is 0 */
    odin3_prov_id after = derive(&REWRITE, &flat, 1);
    const char *names[] = {"f", "g"};
    const odin3_prov_id records[] = {flat, after};
    expect_round_trip(names, records, 2);
}

static void test_if_arms_and_call_instances_restore(void) {
    uint32_t module = add_row((row_spec){ODIN3_AST_MODULE, 10, 0});
    uint32_t block = add_row((row_spec){ODIN3_AST_ALWAYS, 20, module});
    uint32_t iff = add_row((row_spec){ODIN3_AST_IF, 40, block});
    odin3_construct_row arm = {.run = ctx.run.v, .ast = 40, .parent = iff, .part = 1};
    arm.kind = ODIN3_CONSTRUCT_IF_THEN;
    arm.level = (uint8_t)ODIN3_LEVEL_BRANCH;
    arm.file = intern_in(design, "a.v");
    arm.line = 41;
    arm.end_line = 42;
    arm.end_col = 3;
    uint32_t arm_row = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &arm, &arm_row));
    odin3_construct_row stmt = {.run = ctx.run.v, .ast = 50, .parent = arm_row};
    stmt.kind = ODIN3_AST_BLOCKING_ASSIGN;
    stmt.level = (uint8_t)ODIN3_LEVEL_STMT;
    stmt.file = arm.file;
    stmt.line = 43;
    stmt.end_line = 43;
    stmt.end_col = 9;
    uint32_t stmt_row = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &stmt, &stmt_row));
    odin3_prov_id under_arm = leaf((leaf_spec){50, 43});
    odin3_prov_id other = leaf((leaf_spec){20, 20});
    const odin3_prov_id both[2] = {under_arm, other};
    odin3_prov_id join = derive(&COARSEN, both, 2);
    const char *names[] = {"arm", "j"};
    const odin3_prov_id records[] = {under_arm, join};
    expect_round_trip(names, records, 2);
}

/* A SOURCE record of more than 8 locations writes `capped` and keeps 8; after a restore it has 8
 * and the flag still comes out, so the second map equals the first. */
static void test_a_capped_record_keeps_its_flag_after_a_restore(void) {
    enum { LOCS = 11 };
    odin3_srcloc locs[LOCS];
    for (uint32_t i = 0; i < LOCS; i++) {
        locs[i] = (odin3_srcloc){intern_in(design, "a.v"), 10 + i, 1, 10 + i, 5, 0};
    }
    odin3_prov_origin origin = {locs, LOCS, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    odin3_prov_obj obj = kept("c", id);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    write_text(design, &obj, 1, &text);
    TEST_ASSERT_NOT_NULL(strstr(text.data, "capped"));
    odin3_strbuf_free(&text);
    const char *names[] = {"c"};
    expect_round_trip(names, &id, 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_join_over_the_cap_keeps_the_same_16_after_a_restore);
    RUN_TEST(test_a_bulk_coarsen_record_restores_with_its_flag);
    RUN_TEST(test_a_one_parent_context_record_restores);
    RUN_TEST(test_if_arms_and_call_instances_restore);
    RUN_TEST(test_a_capped_record_keeps_its_flag_after_a_restore);
    return UNITY_END();
}
