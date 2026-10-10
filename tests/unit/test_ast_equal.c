/*
 * test_ast_equal.c — unit tests for structural equality: the same tree built in two designs
 * with different locations, comments and warned flags is equal; a change in each compared field
 * (kind, sub, flags, name, NUMBER width/bits/nbits/has_question, REAL, TEXT, child count, an
 * absent slot, attribute count/name/value/flags) is not; child and attribute order count; REAL
 * -0.0 differs from 0.0; the depth-cap chain; abandoned roots; misuse; OOM.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { OOM_LIMIT = 64, FILE_LEN = 200 };

typedef enum mutation {
    M_NONE = 0,
    M_KIND,
    M_SUB,
    M_FLAGS,
    M_NAME,
    M_WIDTH,
    M_BITS,
    M_NBITS,
    M_QUESTION,
    M_REAL,
    M_TEXT,
    M_NCHILD,
    M_ABSENT,
    M_ATTR_COUNT,
    M_ATTR_NAME,
    M_ATTR_VALUE,
    M_ATTR_FLAG,
    M_COUNT
} mutation;

/* A store being built: every node gets loc; mut is the one change from the reference tree. */
typedef struct builder {
    odin3_ast *ast;
    odin3_loc loc;
    mutation mut;
} builder;

static odin3_design *design_a;
static odin3_design *design_b;
static odin3_ast *ast_a;
static odin3_ast *ast_b;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
}

static odin3_ast *new_store(odin3_design **design) {
    *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(*design);
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(*design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    return ast;
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    ast_a = new_store(&design_a);
    ast_b = new_store(&design_b);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast_a);
    odin3_ast_destroy(ast_b);
    odin3_design_destroy(design_a);
    odin3_design_destroy(design_b);
}

static const odin3_ast_id NO = {0};

/* --- the reference tree ---------------------------------------------------------------------- */

static uint32_t intern(const builder *bd, const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(bd->ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_spec spec(const builder *bd, odin3_ast_kind kind, const char *name) {
    return (odin3_ast_spec){
        .kind = (uint8_t)kind, .loc = bd->loc.v, .name = name != NULL ? intern(bd, name) : 0};
}

static odin3_ast_id mk(const builder *bd, odin3_ast_spec sp, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(bd->ast, &sp, ids, n, &id));
    return id;
}

static odin3_ast_id number(const builder *bd, const char *size, const char *base,
                           const char *digits) {
    odin3_number_text pieces = {odin3_bytes_cstr(size), odin3_bytes_cstr(base),
                                odin3_bytes_cstr(digits)};
    odin3_ast_spec sp = spec(bd, ODIN3_AST_NUMBER, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &sp.sub, &sp.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_parse(bd->ast, bd->loc, pieces, &sp.payload));
    return mk(bd, sp, NULL, 0);
}

/* 4'hff, or its width, bits or nbits mutation. */
static odin3_ast_id literal(const builder *bd) {
    switch (bd->mut) {
    case M_WIDTH:
        return number(bd, "5", "'h", "ff");
    case M_BITS:
        return number(bd, "4", "'h", "fe");
    case M_NBITS:
        return number(bd, "4", "'h", "0ff");
    default:
        return number(bd, "4", "'h", "ff");
    }
}

static odin3_ast_id sum(const builder *bd) {
    odin3_ast_spec real = spec(bd, ODIN3_AST_REAL, NULL);
    double value = bd->mut == M_REAL ? 2.5 : 1.5;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(bd->ast, value, &real.payload));
    odin3_ast_spec add = spec(bd, ODIN3_AST_BINARY, NULL);
    add.sub = bd->mut == M_SUB ? ODIN3_AST_BINARY_SUB : ODIN3_AST_BINARY_ADD;
    odin3_ast_id node = mk(bd, add, (odin3_ast_id[]){literal(bd), mk(bd, real, NULL, 0)}, 2);
    if (bd->mut == M_ATTR_COUNT) {
        return node;
    }
    odin3_ast_spec key = spec(bd, ODIN3_AST_ATTR, bd->mut == M_ATTR_NAME ? "dont_touch" : "keep");
    key.flags = bd->mut == M_ATTR_FLAG ? (uint16_t)ODIN3_AST_F_ATTR_METACOMMENT : 0;
    odin3_ast_id value_node = bd->mut == M_ATTR_VALUE ? number(bd, "", "", "1") : NO;
    odin3_ast_id attr = mk(bd, key, &value_node, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(bd->ast, node, &attr, 1));
    return node;
}

static odin3_ast_id cont_assign(const builder *bd) {
    odin3_ast_kind lhs_kind = bd->mut == M_KIND ? ODIN3_AST_STRING : ODIN3_AST_IDENT;
    odin3_ast_id lhs = mk(bd, spec(bd, lhs_kind, bd->mut == M_NAME ? "z" : "y"), NULL, 0);
    odin3_ast_id net =
        mk(bd, spec(bd, ODIN3_AST_NET_ASSIGN, NULL), (odin3_ast_id[]){lhs, sum(bd)}, 2);
    odin3_ast_id ids[4] = {NO, NO, net, net};
    if (bd->mut == M_NCHILD) {
        ids[3] = mk(bd, spec(bd, ODIN3_AST_NET_ASSIGN, NULL),
                    (odin3_ast_id[]){mk(bd, spec(bd, ODIN3_AST_IDENT, "q"), NULL, 0),
                                     number(bd, "4", "'b", "1")},
                    2);
    }
    return mk(bd, spec(bd, ODIN3_AST_CONT_ASSIGN, NULL), ids, bd->mut == M_NCHILD ? 4 : 3);
}

static odin3_ast_id ports(const builder *bd) {
    odin3_ast_spec port = spec(bd, ODIN3_AST_PORT_DECL, NULL);
    port.flags = ODIN3_AST_F_PORT_DECL_IN_HEADER;
    port.flags |= bd->mut == M_FLAGS ? (uint16_t)ODIN3_AST_F_PORT_DECL_SIGNED : 0;
    odin3_ast_id range = NO;
    if (bd->mut != M_ABSENT) {
        range = mk(bd, spec(bd, ODIN3_AST_RANGE, NULL),
                   (odin3_ast_id[]){number(bd, "", "", "3"), number(bd, "", "", "0")}, 2);
    }
    odin3_ast_id decl = mk(bd, spec(bd, ODIN3_AST_DECLARATOR, "a"), &NO, 1);
    odin3_ast_id question = number(bd, "4", "'b", bd->mut == M_QUESTION ? "z" : "?");
    odin3_ast_id init = mk(bd, spec(bd, ODIN3_AST_DECLARATOR, "b"), &question, 1);
    odin3_ast_id decl_port = mk(bd, port, (odin3_ast_id[]){range, decl, init}, 3);
    return mk(bd, spec(bd, ODIN3_AST_LIST, NULL), &decl_port, 1);
}

static odin3_ast_id build(odin3_ast *ast, odin3_loc loc, mutation mut) {
    builder bd = {ast, loc, mut};
    odin3_ast_spec text = spec(&bd, ODIN3_AST_SPECIFY_BLOCK, NULL);
    const char *body = mut == M_TEXT ? "specify  endspecify" : "specify endspecify";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_text_new(ast, odin3_bytes_cstr(body), &text.payload));
    odin3_ast_id items[4] = {NO, ports(&bd), cont_assign(&bd), mk(&bd, text, NULL, 0)};
    return mk(&bd, spec(&bd, ODIN3_AST_MODULE, "m"), items, 4);
}

/* A loc in a 200-byte file of design_a, with a comment recorded in ast_a. */
static odin3_loc file_loc(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design_a, &sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast_a, odin3_bytes_cstr("a.v"), &name));
    odin3_srcfile_spec spec_file = {.name = name, .len = FILE_LEN};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec_file, &buf));
    odin3_ast_comment_spec note = {.loc = odin3_srcman_loc(sm, buf, 3).v,
                                   .kind = ODIN3_AST_COMMENT_LINE,
                                   .text_ptr = "// note",
                                   .text_len = 7};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast_a, &note));
    return odin3_srcman_loc(sm, buf, 42);
}

static bool equal(const odin3_ast *lhs, odin3_ast_id ra, const odin3_ast *rhs, odin3_ast_id rb) {
    bool same = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_equal(lhs, ra, rhs, rb, &same));
    return same;
}

/* Sets the warned flag of the module's 4'hff (number_value truncates it) and returns it. */
static odin3_ast_id warn_once(odin3_ast_id module) {
    odin3_ast_id cont = odin3_ast_child(ast_a, module, 2);
    odin3_ast_id sum = odin3_ast_child(ast_a, odin3_ast_child(ast_a, cont, 2), 1);
    odin3_ast_id hex = odin3_ast_child(ast_a, sum, 0);
    odin3_arena *arena = odin3_arena_create(0);
    odin3_ast_literal lit;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast_a, hex, arena, &lit));
    odin3_arena_destroy(arena);
    TEST_ASSERT_TRUE(odin3_ast_number_of(ast_a, hex)->warned);
    return hex;
}

/* --- tests ----------------------------------------------------------------------------------- */

static void test_same_tree_in_two_designs(void) {
    odin3_ast_id ra = build(ast_a, file_loc(), M_NONE);
    odin3_ast_id rb = build(ast_b, (odin3_loc){0}, M_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast_a));
    odin3_ast_id hex = warn_once(ra); /* warned is set in ast_a only */
    bool results[4] = {equal(ast_a, ra, ast_b, rb), equal(ast_b, rb, ast_a, ra),
                       equal(ast_a, ra, ast_a, ra), equal(ast_a, ra, ast_a, hex)};
    const bool want[4] = {true, true, true, false}; /* the last: different subtrees */
    TEST_ASSERT_EQUAL_MEMORY(want, results, sizeof want);
}

static void test_each_field_change_is_unequal(void) {
    odin3_ast_id ra = build(ast_a, file_loc(), M_NONE);
    for (uint32_t mut = M_NONE + 1; mut < M_COUNT; mut++) {
        odin3_ast_id rb = build(ast_b, (odin3_loc){0}, (mutation)mut);
        bool forward = equal(ast_a, ra, ast_b, rb);
        bool backward = equal(ast_b, rb, ast_a, ra);
        if (forward || backward) {
            char why[64];
            (void)snprintf(why, sizeof why, "mutation %u compared equal", (unsigned)mut);
            TEST_FAIL_MESSAGE(why);
        }
    }
}

static odin3_ast_id chain(odin3_ast *ast, const char *leaf) {
    odin3_ast_spec unary = {.kind = ODIN3_AST_UNARY, .sub = ODIN3_AST_UNARY_NOT};
    odin3_ast_spec ident = {.kind = ODIN3_AST_IDENT};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(leaf), &ident.name));
    odin3_ast_id top = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &ident, NULL, 0, &top));
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &unary, &top, 1, &top));
    }
    return top;
}

static void test_chain_at_cap(void) {
    odin3_ast_id ra = chain(ast_a, "x");
    odin3_ast_id rb = chain(ast_b, "x");
    odin3_ast_id rc = chain(ast_b, "y");
    TEST_ASSERT_TRUE(equal(ast_a, ra, ast_b, rb));
    TEST_ASSERT_FALSE(equal(ast_a, ra, ast_b, rc));
}

/* Review Focus 5: absent slots compare by position; abandoned roots compare like any subtree. */
static void test_absent_slots_and_abandoned_roots(void) {
    builder ad = {ast_a, {0}, M_NONE};
    builder bd = {ast_b, {0}, M_NONE};
    odin3_ast_id lost_a = mk(&ad, spec(&ad, ODIN3_AST_IDENT, "lost"), NULL, 0);
    odin3_ast_id lost_b = mk(&bd, spec(&bd, ODIN3_AST_IDENT, "lost"), NULL, 0);
    odin3_ast_id empty_a = mk(&ad, spec(&ad, ODIN3_AST_DECLARATOR, "d"), &NO, 1);
    odin3_ast_id empty_b = mk(&bd, spec(&bd, ODIN3_AST_DECLARATOR, "d"), &NO, 1);
    odin3_ast_id init_b = mk(&bd, spec(&bd, ODIN3_AST_DECLARATOR, "d"), &lost_b, 1);
    bool results[4] = {equal(ast_a, lost_a, ast_b, lost_b), equal(ast_a, empty_a, ast_b, empty_b),
                       equal(ast_a, empty_a, ast_b, init_b), equal(ast_b, init_b, ast_a, empty_a)};
    const bool want[4] = {true, true, false, false};
    TEST_ASSERT_EQUAL_MEMORY(want, results, sizeof want);
}

/* Equality is positional: the same children or attributes in another order differ. */
static void test_order_matters(void) {
    builder ad = {ast_a, {0}, M_NONE};
    builder bd = {ast_b, {0}, M_NONE};
    odin3_ast_spec cat = spec(&ad, ODIN3_AST_CONCAT, NULL);
    odin3_ast_id ab = mk(&ad, cat,
                         (odin3_ast_id[]){mk(&ad, spec(&ad, ODIN3_AST_IDENT, "x"), NULL, 0),
                                          mk(&ad, spec(&ad, ODIN3_AST_IDENT, "y"), NULL, 0)},
                         2);
    odin3_ast_id ba = mk(&bd, cat,
                         (odin3_ast_id[]){mk(&bd, spec(&bd, ODIN3_AST_IDENT, "y"), NULL, 0),
                                          mk(&bd, spec(&bd, ODIN3_AST_IDENT, "x"), NULL, 0)},
                         2);
    odin3_ast_id node_a = mk(&ad, spec(&ad, ODIN3_AST_NULL_STMT, NULL), NULL, 0);
    odin3_ast_id node_b = mk(&bd, spec(&bd, ODIN3_AST_NULL_STMT, NULL), NULL, 0);
    odin3_ast_id attrs_a[2] = {mk(&ad, spec(&ad, ODIN3_AST_ATTR, "p"), &NO, 1),
                               mk(&ad, spec(&ad, ODIN3_AST_ATTR, "q"), &NO, 1)};
    odin3_ast_id attrs_b[2] = {mk(&bd, spec(&bd, ODIN3_AST_ATTR, "q"), &NO, 1),
                               mk(&bd, spec(&bd, ODIN3_AST_ATTR, "p"), &NO, 1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast_a, node_a, attrs_a, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast_b, node_b, attrs_b, 2));
    bool results[2] = {equal(ast_a, ab, ast_b, ba), equal(ast_a, node_a, ast_b, node_b)};
    const bool want[2] = {false, false};
    TEST_ASSERT_EQUAL_MEMORY(want, results, sizeof want);
}

static odin3_ast_id real_node(odin3_ast *ast, double value) {
    odin3_ast_spec sp = {.kind = ODIN3_AST_REAL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, value, &sp.payload));
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &sp, NULL, 0, &id));
    return id;
}

/* The printer writes -0.0 and 0.0 apart, so equality tells them apart too; NaN equals NaN. */
static void test_real_signed_zero_and_nan(void) {
    bool results[3] = {equal(ast_a, real_node(ast_a, 0.0), ast_b, real_node(ast_b, -0.0)),
                       equal(ast_a, real_node(ast_a, -0.0), ast_b, real_node(ast_b, -0.0)),
                       equal(ast_a, real_node(ast_a, NAN), ast_b, real_node(ast_b, NAN))};
    const bool want[3] = {false, true, true};
    TEST_ASSERT_EQUAL_MEMORY(want, results, sizeof want);
}

static void test_misuse(void) {
    builder ad = {ast_a, {0}, M_NONE};
    odin3_ast_id node = mk(&ad, spec(&ad, ODIN3_AST_IDENT, "x"), NULL, 0);
    bool same = true;
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_equal(NULL, node, ast_a, node, &same));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_equal(ast_a, node, NULL, node, &same));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_equal(ast_a, node, ast_a, node, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_equal(ast_a, NO, ast_a, node, &same));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_equal(ast_a, node, ast_b, (odin3_ast_id){1}, &same));
    TEST_ASSERT_EQUAL_size_t(errors + 5, errors_logged);
    TEST_ASSERT_TRUE(same); /* unchanged */
}

static void test_oom_sweep(void) {
    odin3_ast_id ra = build(ast_a, file_loc(), M_NONE);
    odin3_ast_id rb = build(ast_b, (odin3_loc){0}, M_NONE);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long k = 0; k < OOM_LIMIT && st != ODIN3_OK; k++) {
        bool same = false;
        odin3_util_set_alloc_fail_after(k);
        st = odin3_ast_equal(ast_a, ra, ast_b, rb, &same);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        TEST_ASSERT_EQUAL(st == ODIN3_OK, same); /* unchanged on failure */
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_same_tree_in_two_designs);
    RUN_TEST(test_each_field_change_is_unequal);
    RUN_TEST(test_chain_at_cap);
    RUN_TEST(test_absent_slots_and_abandoned_roots);
    RUN_TEST(test_order_matters);
    RUN_TEST(test_real_signed_zero_and_nan);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
