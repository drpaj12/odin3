/*
 * test_ast_attr.c — unit tests for the attribute map: attach once, ATTR-only entries, value
 * expressions, METACOMMENT attributes, empty views, a view of the child table as input, and
 * OOM at every allocation point (child table, map creation and map growth).
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { OOM_LIMIT = 64, OOM_NODES = 300 };

static odin3_design *design;
static odin3_ast *ast;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_id make(odin3_ast_spec spec, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, ids, n, &id));
    return id;
}

static odin3_ast_id ident(const char *name) {
    return make((odin3_ast_spec){.kind = ODIN3_AST_IDENT, .name = intern(name)}, NULL, 0);
}

/* An ATTR (* key *) or (* key = value *); value 0 for none. */
static odin3_ast_id attr(const char *key, odin3_ast_id value, uint16_t flags) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_ATTR, .flags = flags, .name = intern(key)};
    return make(spec, &value, 1);
}

static void expect_misuse(odin3_ast_id node, const odin3_ast_id *attrs, uint32_t n) {
    size_t children = ast->children.len;
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_attach(ast, node, attrs, n));
    TEST_ASSERT_EQUAL_size_t(children, ast->children.len);
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
}

static void test_attach_once(void) {
    odin3_ast_id node = ident("x");
    odin3_ast_id keep = attr("keep", (odin3_ast_id){0}, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, node, &keep, 1));
    odin3_ast_span span = odin3_ast_attrs(ast, node);
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_UINT32(keep.v, span.ids[0].v);
    TEST_ASSERT_EQUAL_STRING("keep", odin3_ast_name_str(ast, span.ids[0]));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(ast, keep, 0).v); /* no value */
    odin3_ast_id again = attr("dont_touch", (odin3_ast_id){0}, 0);
    expect_misuse(node, &again, 1); /* a second attach */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_attrs(ast, node).n);
    /* the attributes are not children */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(ast, node));
}

static void test_rejections(void) {
    odin3_ast_id node = ident("x");
    odin3_ast_id other = ident("y");
    odin3_ast_id keep = attr("keep", (odin3_ast_id){0}, 0);
    odin3_ast_id mixed[2] = {keep, other};
    expect_misuse(node, mixed, 2);                          /* a non-ATTR entry */
    expect_misuse(node, (odin3_ast_id[]){{0}}, 1);          /* ID 0 */
    expect_misuse(node, (odin3_ast_id[]){{keep.v + 1}}, 1); /* not made */
    expect_misuse(node, NULL, 1);
    expect_misuse(node, &keep, 0);               /* nothing to attach */
    expect_misuse((odin3_ast_id){0}, &keep, 1);  /* no node */
    expect_misuse((odin3_ast_id){99}, &keep, 1); /* not made */
    odin3_ast_id inner = attr("inner", (odin3_ast_id){0}, 0);
    expect_misuse(keep, &inner, 1); /* an ATTR is not annotated */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_attach(NULL, node, &keep, 1));
    TEST_ASSERT_NULL(ast->attrs); /* nothing was recorded */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect_misuse(node, &keep, 1); /* sealed */
}

static void test_value_and_metacomment(void) {
    odin3_ast_spec num = {.kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_DEC};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_ast_number_parse(
            ast, (odin3_loc){0},
            (odin3_number_text){odin3_bytes_cstr(""), odin3_bytes_cstr(""), odin3_bytes_cstr("3")},
            &num.payload));
    num.flags = ODIN3_AST_F_NUMBER_SIGNED;
    odin3_ast_id value = make(num, NULL, 0);
    odin3_ast_id init = attr("init", value, 0);
    odin3_ast_id full = attr("full_case", (odin3_ast_id){0}, ODIN3_AST_F_ATTR_METACOMMENT);
    odin3_ast_id par = attr("parallel_case", (odin3_ast_id){0}, ODIN3_AST_F_ATTR_METACOMMENT);
    odin3_ast_id sel = ident("s");
    odin3_ast_id null_stmt = make((odin3_ast_spec){.kind = ODIN3_AST_NULL_STMT}, NULL, 0);
    odin3_ast_spec item = {.kind = ODIN3_AST_CASE_ITEM, .flags = ODIN3_AST_F_CASE_ITEM_DEFAULT};
    odin3_ast_id default_item = make(item, &null_stmt, 1);
    odin3_ast_id case_kids[2] = {sel, default_item};
    odin3_ast_id case_stmt = make((odin3_ast_spec){.kind = ODIN3_AST_CASE}, case_kids, 2);
    /* (* init = 3 *) merged with the metacomments after the header: one attach */
    odin3_ast_id all[3] = {init, full, par};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, case_stmt, all, 3));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast)); /* reads work after finish */
    odin3_ast_span span = odin3_ast_attrs(ast, case_stmt);
    TEST_ASSERT_EQUAL_UINT32(3, span.n);
    TEST_ASSERT_EQUAL_UINT32(value.v, odin3_ast_child(ast, span.ids[0], 0).v);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_ast_number_of(ast, value)->bits[0] +
                                    2 * odin3_ast_number_of(ast, value)->bits[1]);
    TEST_ASSERT_EQUAL_UINT16(0, odin3_ast_flags(ast, span.ids[0]));
    TEST_ASSERT_EQUAL_UINT16(ODIN3_AST_F_ATTR_METACOMMENT, odin3_ast_flags(ast, span.ids[1]));
    TEST_ASSERT_EQUAL_STRING("parallel_case", odin3_ast_name_str(ast, span.ids[2]));
    /* unannotated nodes: an empty view */
    for (uint32_t id = 1; id < odin3_ast_node_end(ast).v; id++) {
        if (id != case_stmt.v) {
            TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(ast, (odin3_ast_id){id}).n);
            TEST_ASSERT_NULL(odin3_ast_attrs(ast, (odin3_ast_id){id}).ids);
        }
    }
}

/* Attaching from a view of the child table survives the table's growth (ASan). */
static void test_attach_from_child_view(void) {
    odin3_ast_id attrs[2] = {attr("a", (odin3_ast_id){0}, 0), attr("b", (odin3_ast_id){0}, 0)};
    odin3_ast_id list = make((odin3_ast_spec){.kind = ODIN3_AST_LIST}, attrs, 2);
    odin3_ast_id node = ident("x");
    odin3_ast_id leaf = ident("y");
    while (ast->children.len < ast->children.cap) { /* fill the table: attach must grow it */
        make((odin3_ast_spec){.kind = ODIN3_AST_LIST}, &leaf, 1);
    }
    odin3_ast_span view = odin3_ast_children(ast, list);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, node, view.ids, view.n));
    odin3_ast_span got = odin3_ast_attrs(ast, node);
    TEST_ASSERT_EQUAL_UINT32(2, got.n);
    TEST_ASSERT_EQUAL_UINT32(attrs[0].v, got.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(attrs[1].v, got.ids[1].v);
}

/* --- OOM ----------------------------------------------------------------------------------- */

static odin3_ast_id oom_attr;

/* OOM_NODES IDENTs and one ATTR, with the child table full. */
static void prepare(void) {
    for (uint32_t i = 0; i < OOM_NODES; i++) {
        ident("n");
    }
    oom_attr = attr("keep", (odin3_ast_id){0}, 0);
    odin3_ast_id leaf = {1};
    while (ast->children.len < ast->children.cap || ast->children.cap == 0) {
        make((odin3_ast_spec){.kind = ODIN3_AST_LIST}, &leaf, 1);
    }
}

/*
 * Attaches the ATTR to IDENTs 1..OOM_NODES in turn (the map grows on the way) with failure fail_at
 * injected; the call that fails must change nothing. Returns true when every attach succeeded.
 */
static bool attach_all(long fail_at) {
    odin3_util_set_alloc_fail_after(fail_at);
    for (uint32_t id = 1; id <= OOM_NODES; id++) {
        size_t children = ast->children.len;
        size_t count = ast->attrs != NULL ? odin3_u64map_count(ast->attrs) : 0;
        odin3_status st = odin3_ast_attach(ast, (odin3_ast_id){id}, &oom_attr, 1);
        if (st != ODIN3_OK) {
            odin3_util_set_alloc_fail_after(-1);
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_size_t(children, ast->children.len);
            TEST_ASSERT_EQUAL_size_t(count,
                                     ast->attrs != NULL ? odin3_u64map_count(ast->attrs) : 0);
            TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(ast, (odin3_ast_id){id}).n);
            /* and the store still works */
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_ast_attach(ast, (odin3_ast_id){id}, &oom_attr, 1));
            return false;
        }
    }
    odin3_util_set_alloc_fail_after(-1);
    return true;
}

static void test_oom(void) {
    long k = 0;
    for (; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        prepare();
        if (attach_all(k)) {
            break;
        }
    }
    TEST_ASSERT_LESS_THAN_INT(OOM_LIMIT, k);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, k); /* child table, map creation, map growth */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_attach_once);
    RUN_TEST(test_rejections);
    RUN_TEST(test_value_and_metacomment);
    RUN_TEST(test_attach_from_child_view);
    RUN_TEST(test_oom);
    return UNITY_END();
}
