/*
 * test_construct_snapshot.c — unit tests for the parts of the construct snapshot a unit test can
 * reach without a read: the alias rows (constructs mode), the read mode a design carries, and the
 * refusals of odin3_construct_snapshot. The snapshot of real designs (arms, call instances,
 * generate clones, SystemVerilog) is tested by construct_golden and construct_bytes
 * (tools/construct-check).
 */
#include "ast/ast.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stddef.h>
#include <stdint.h>

static odin3_design *design;
static odin3_passrun_id run1;

static void quiet(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

void setUp(void) {
    odin3_log_set_sink(quiet, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(design, odin3_bytes_cstr("read_verilog"), &name));
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, name, &ctx));
    run1 = ctx.run;
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static uint32_t add_row(uint32_t ast, uint32_t kind, uint32_t parent) {
    odin3_construct_row row = {.run = run1.v,
                               .ast = ast,
                               .parent = parent,
                               .kind = (uint8_t)kind,
                               .level = (uint8_t)odin3_construct_kind_level(kind)};
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &index));
    return index;
}

static void test_read_mode_defaults_and_sets(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_DEFAULT, odin3_construct_read_mode(design));
    odin3_construct_set_read_mode(design, ODIN3_CONSTRUCT_MODE_FULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_FULL, odin3_construct_read_mode(design));
    odin3_construct_set_read_mode(NULL, ODIN3_CONSTRUCT_MODE_OFF); /* ignored */
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_DEFAULT, odin3_construct_read_mode(NULL));
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_DEFAULT, odin3_construct_read_mode(copy));
    odin3_design_destroy(copy);
}

static void test_alias_resolves_a_node_to_a_row(void) {
    uint32_t module = add_row(1, ODIN3_AST_MODULE, 0);
    uint32_t assign = add_row(2, ODIN3_AST_BLOCKING_ASSIGN, module);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_alias(design, run1, 3, assign));
    const odin3_construct_key key = {run1, 3, ODIN3_CONSTRUCT_PART_NODE, 0};
    TEST_ASSERT_EQUAL_UINT32(assign, odin3_construct_find(design, &key));
    TEST_ASSERT_TRUE(odin3_construct_has_node(design, run1, 3));
    /* the aliased node did not add a row, and the target still finds itself */
    TEST_ASSERT_EQUAL_UINT32(3, odin3_construct_end(design));
    const odin3_construct_key own = {run1, 2, ODIN3_CONSTRUCT_PART_NODE, 0};
    TEST_ASSERT_EQUAL_UINT32(assign, odin3_construct_find(design, &own));
}

static void test_alias_refusals(void) {
    uint32_t module = add_row(1, ODIN3_AST_MODULE, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 0, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 5, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 5, 99));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 1, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_alias(design, run1, 5, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 5, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(NULL, run1, 6, module));
}

static void test_alias_refuses_a_row_that_is_not_the_first_of_its_node(void) {
    uint32_t module = add_row(1, ODIN3_AST_MODULE, 0);
    uint32_t cond = add_row(2, ODIN3_AST_IF, module);
    odin3_construct_row arm = {.run = run1.v,
                               .ast = 2,
                               .parent = cond,
                               .part = ODIN3_CONSTRUCT_PART_THEN,
                               .kind = ODIN3_CONSTRUCT_IF_THEN,
                               .level = (uint8_t)ODIN3_LEVEL_BRANCH};
    uint32_t arm_row = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &arm, &arm_row));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_construct_alias(design, run1, 7, arm_row));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_alias(design, run1, 7, cond));
}

static void test_snapshot_refusals(void) {
    odin3_ast *store = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, run1, &store));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_snapshot(NULL, store, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_snapshot(design, NULL, ODIN3_CONSTRUCT_MODE_REFS));
    /* a parsed store is not the elaborated one */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_snapshot(design, store, ODIN3_CONSTRUCT_MODE_REFS));
    odin3_ast_destroy(store);
}

static void test_snapshot_of_an_empty_store_marks_the_run_and_off_does_not(void) {
    odin3_ast *store = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_create(design, ODIN3_AST_FORM_ELABORATED, run1, &store));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_snapshot(design, store, ODIN3_CONSTRUCT_MODE_OFF));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_OFF, odin3_construct_run_mode(design, run1));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_construct_end(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_snapshot(design, store, ODIN3_CONSTRUCT_MODE_FULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_FULL, odin3_construct_run_mode(design, run1));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_construct_end(design));
    /* a second snapshot of one run is refused */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_construct_snapshot(design, store, ODIN3_CONSTRUCT_MODE_REFS));
    odin3_ast_destroy(store);
}

/* --- a store that shares a subtree --------------------------------------------------------- */

static odin3_ast_id make(odin3_ast *store, const odin3_ast_spec *spec, const odin3_ast_id *kids,
                         uint32_t count) {
    odin3_ast_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(store, spec, kids, count, &out));
    return out;
}

static odin3_prov_id source_of(uint32_t ast) {
    odin3_pass_ctx ctx = {design, run1, 0};
    odin3_srcloc loc = {1, ast, 1, ast, 2, 0};
    odin3_prov_origin origin = {&loc, 1, ast, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

/* module m { always s1 { a }; always s2 { a } } with the assignment a reached through both blocks.
 * The first parent's chain is kept for the rows, but a record of a (or of a node under it) is a
 * recorded miss: its chain would be wrong from the second block. A record of s1 is not. */
static void test_shared_subtree_with_two_chains_is_a_miss(void) {
    odin3_ast *store = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_create(design, ODIN3_AST_FORM_ELABORATED, run1, &store));
    uint32_t xn = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(store, odin3_bytes_cstr("x"), &xn));
    odin3_ast_id xid = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_IDENT, .name = xn}, NULL, 0);
    odin3_ast_id lr[] = {xid, {0}, xid};
    odin3_ast_id assign = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_BLOCKING_ASSIGN}, lr, 3);
    odin3_ast_id body[] = {{0}, assign};
    odin3_ast_id blk1 = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_SEQ_BLOCK}, body, 2);
    odin3_ast_id blk2 = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_SEQ_BLOCK}, body, 2);
    odin3_ast_id al1 = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_ALWAYS}, &blk1, 1);
    odin3_ast_id al2 = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_ALWAYS}, &blk2, 1);
    uint32_t mn = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(store, odin3_bytes_cstr("m"), &mn));
    odin3_ast_id items[] = {{0}, {0}, al1, al2};
    odin3_ast_id mod =
        make(store, &(odin3_ast_spec){.kind = ODIN3_AST_MODULE, .name = mn}, items, 4);
    odin3_ast_id unit = make(store, &(odin3_ast_spec){.kind = ODIN3_AST_UNIT, .name = mn}, &mod, 1);
    (void)unit;
    odin3_prov_id rec_assign = source_of(assign.v);
    odin3_prov_id rec_blk = source_of(blk1.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_snapshot(design, store, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_TRUE(odin3_construct_is_miss(design, rec_assign));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_construct_of_record(design, rec_assign));
    TEST_ASSERT_FALSE(odin3_construct_is_miss(design, rec_blk));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_construct_of_record(design, rec_blk));
    odin3_ast_destroy(store);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_read_mode_defaults_and_sets);
    RUN_TEST(test_alias_resolves_a_node_to_a_row);
    RUN_TEST(test_alias_refusals);
    RUN_TEST(test_alias_refuses_a_row_that_is_not_the_first_of_its_node);
    RUN_TEST(test_snapshot_refusals);
    RUN_TEST(test_snapshot_of_an_empty_store_marks_the_run_and_off_does_not);
    RUN_TEST(test_shared_subtree_with_two_chains_is_a_miss);
    return UNITY_END();
}
