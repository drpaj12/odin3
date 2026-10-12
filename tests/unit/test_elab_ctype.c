/*
 * test_elab_ctype.c — 2D expression typing (spec §4.2, EL-13; EL-15/EL-16 types): the
 * self-determined and final (width, sign) of every node, on hand-built parsed trees
 * (elab_test_tree.h) and a test host declaring the names below. Covers the operator classes,
 * E8/E12/E22/E23, ruling I7 (reductions and logical operators take the context width), v4
 * EL-30's ~&, unsized literals and parameters (P/Q/R), memory words, the diagnostics with their
 * whole lines, the width cap, a 10,000-deep chain, nested use from a host callback, misuse and
 * OOM.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "elab_test_tree.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CAPTURE = 1024,
    MSG = 256,
    CHAIN_TERMS = 30000, /* the AST height cap is 32768 */
    OOM_LIMIT = 4096,
    LOWERED_WIDTH = 16,
};

static odin3_design *design;
static et_tree tree;
static odin3_ctype *ty;
static odin3_arena *values;
static char captured[CAPTURE]; /* the last line logged */
static int errors_logged;
static int warnings_logged;
static const char *last_what;    /* odin3_ctype_constant_what during the last constant call */
static odin3_ast_id nested_root; /* typed from inside the constant callback when set */
static odin3_status nested_status;

static const odin3_etype SELF_DET = {0, false, false};
static const odin3_etype CTX8 = {8, true, false};
static const odin3_etype CTX16 = {16, true, false};

/* --- the test host ------------------------------------------------------------------------- */

typedef struct host_decl {
    const char *name;
    odin3_etype type;
    bool is_const;
    int64_t value;
} host_decl;

/* a, b [7:0]; s signed [7:0]; t signed [3:0]; i integer; c 1 bit; P = 'hFFFFFFFF (unsized);
 * Q [31:0] (kept unsized, EL-16); R [35:0]; W, NEG, ZERO integer parameters; f returns [3:0];
 * g returns integer; zw is a broken leaf (width 0). Memories: m (signed [7:0] m [0:3]) and m2
 * ([5:0] m2 [0:3][0:1]) are handled in host_leaf. */
static const host_decl DECLS[] = {
    {"a", {8, false, false}, false, 0},          {"b", {8, false, false}, false, 0},
    {"s", {8, true, false}, false, 0},           {"t", {4, true, false}, false, 0},
    {"i", {32, true, false}, false, 0},          {"c", {1, false, false}, false, 0},
    {"P", {32, false, true}, true, 0xFFFFFFFF},  {"Q", {32, false, true}, true, 0xFFFFFFFF},
    {"R", {36, false, false}, true, 0xFFFFFFFF}, {"W", {32, true, false}, true, 4},
    {"NEG", {32, true, false}, true, -2},        {"ZERO", {32, true, false}, true, 0},
    {"f", {4, false, false}, false, 0},          {"g", {32, true, false}, false, 0},
    {"zw", {0, false, false}, false, 0},
};

static const host_decl *find_decl(const char *name) {
    for (size_t k = 0; k < sizeof DECLS / sizeof DECLS[0]; k++) {
        if (strcmp(DECLS[k].name, name) == 0) {
            return &DECLS[k];
        }
    }
    return NULL;
}

static odin3_status host_error(odin3_ast_id node, const char *fmt, const char *name) {
    char msg[MSG];
    (void)snprintf(msg, sizeof msg, fmt, name);
    odin3_diag(tree.design, ODIN3_LOG_ERROR, odin3_ast_loc(tree.ast, node), "%s", msg);
    return ODIN3_ERR_PARSE;
}

/* A memory word: m[x] (signed 8 bits) or m2[x][y] (6 bits); width 0 for any other select. */
static odin3_status host_select(odin3_ast_id node, odin3_etype *out) {
    uint32_t depth = 0;
    odin3_ast_id obj = node;
    while (odin3_ast_kind_of(tree.ast, obj) == ODIN3_AST_SELECT) {
        obj = odin3_ast_child(tree.ast, obj, 0);
        depth++;
    }
    const char *name = odin3_ast_name_str(tree.ast, obj);
    if (strcmp(name, "m") == 0 && depth == 1) {
        *out = (odin3_etype){8, true, false};
    } else if (strcmp(name, "m2") == 0 && depth == 2) {
        *out = (odin3_etype){6, false, false};
    } else {
        *out = (odin3_etype){0, false, false};
    }
    return ODIN3_OK;
}

static odin3_status host_leaf(void *user, odin3_ast_id node, odin3_etype *out) {
    (void)user;
    if (odin3_ast_kind_of(tree.ast, node) == ODIN3_AST_SELECT) {
        return host_select(node, out);
    }
    const char *name = odin3_ast_name_str(tree.ast, node);
    if (name[0] == '$') {
        return host_error(node, "system function '%s' is not supported", name);
    }
    if (strcmp(name, "m") == 0 || strcmp(name, "m2") == 0) {
        return host_error(node, "'%s' is a memory; select a word", name);
    }
    const host_decl *decl = find_decl(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(decl, name);
    *out = decl->type;
    return ODIN3_OK;
}

/* The node is typed (self and final) when asked for; a nested typing keeps the role. */
static void check_constant_ready(odin3_ast_id node) {
    last_what = odin3_ctype_constant_what(ty);
    TEST_ASSERT_NOT_EQUAL(0, odin3_ctype_self(ty, node).width);
    TEST_ASSERT_NOT_EQUAL(0, odin3_ctype_final(ty, node).width);
    if (nested_root.v != 0) {
        nested_status = odin3_ctype_expr(ty, nested_root, CTX8);
        TEST_ASSERT_EQUAL_STRING(last_what, odin3_ctype_constant_what(ty));
    }
}

static odin3_status literal_constant(odin3_ast_id node, int64_t *out) {
    odin3_ast_literal lit = {{0}, false, false};
    odin3_cval val = {0};
    odin3_status st = odin3_ast_number_value(tree.ast, node, values, &lit);
    st = st == ODIN3_OK ? odin3_cval_from_literal(values, &lit, &val) : st;
    if (st == ODIN3_OK) {
        TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, out));
    }
    return st;
}

static odin3_status host_constant(void *user, odin3_ast_id node, int64_t *out) {
    (void)user;
    check_constant_ready(node);
    if (odin3_ast_kind_of(tree.ast, node) == ODIN3_AST_NUMBER) {
        return literal_constant(node, out);
    }
    const char *name = odin3_ast_name_str(tree.ast, node);
    const host_decl *decl = find_decl(name);
    if (decl == NULL || !decl->is_const) {
        char fmt[MSG];
        (void)snprintf(fmt, sizeof fmt, "%s is not constant (it reads '%%s')", last_what);
        return host_error(node, fmt, name);
    }
    *out = decl->value;
    return ODIN3_OK;
}

static void host_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    (void)user;
    odin3_diag(tree.design, level, loc, "%s", msg);
}

static const odin3_ctype_host HOST = {host_leaf, host_constant, host_report, NULL};

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
    warnings_logged += level == ODIN3_LOG_WARN;
    (void)snprintf(captured, sizeof captured, "%s", msg);
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    captured[0] = '\0';
    errors_logged = 0;
    warnings_logged = 0;
    last_what = NULL;
    nested_root = (odin3_ast_id){0};
    nested_status = ODIN3_OK;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    et_init(&tree, design, "x.v");
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
    ty = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ctype_create(tree.ast, &HOST, &ty));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_ctype_destroy(ty);
    ty = NULL;
    odin3_arena_destroy(values);
    values = NULL;
    et_free(&tree);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_ast_id id(const char *name) {
    return et_ident(&tree, name);
}

static odin3_ast_id num(const char *text) {
    return et_num(&tree, text);
}

static odin3_ast_id bin(uint8_t sub, odin3_ast_id lhs, odin3_ast_id rhs) {
    return et_binary(&tree, sub, lhs, rhs);
}

static odin3_ast_id un(uint8_t sub, odin3_ast_id operand) {
    return et_unary(&tree, sub, operand);
}

static odin3_ast_id cat2(odin3_ast_id lhs, odin3_ast_id rhs) {
    const odin3_ast_id parts[] = {lhs, rhs};
    return et_concat(&tree, parts, 2);
}

static odin3_ast_id rep(odin3_ast_id count, odin3_ast_id part) {
    return et_replicate(&tree, count, et_concat(&tree, &part, 1));
}

static odin3_ast_id call1(const char *name, odin3_ast_id arg) {
    return et_call(&tree, name, &arg, 1);
}

static void type_ok(odin3_ast_id root, odin3_etype context) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ctype_expr(ty, root, context));
}

/*
 * got is (width, kind[0]) with kind "u" unsigned, "s" signed, "U" unsigned unsized, "S" signed
 * unsized.
 */
static void expect_type(odin3_etype got, uint32_t width, const char *kind, int line) {
    UNITY_TEST_ASSERT_EQUAL_UINT32(width, got.width, line, "width");
    UNITY_TEST_ASSERT(got.is_signed == (kind[0] == 's' || kind[0] == 'S'), line, "sign");
    UNITY_TEST_ASSERT(got.is_unsized == (kind[0] == 'U' || kind[0] == 'S'), line, "unsized");
}

#define KIND(kind) ((const char[]){(kind), '\0'})
#define EXPECT_FINAL(node, width, kind)                                                            \
    expect_type(odin3_ctype_final(ty, node), width, KIND(kind), __LINE__)
#define EXPECT_SELF(node, width, kind)                                                             \
    expect_type(odin3_ctype_self(ty, node), width, KIND(kind), __LINE__)

static void expect_line(const char *want) {
    TEST_ASSERT_EQUAL_STRING(want, captured);
}

/* --- the brief's table --------------------------------------------------------------------- */

static void test_add_in_context(void) {
    odin3_ast_id ref_a = id("a");
    odin3_ast_id ref_b = id("b");
    odin3_ast_id add = bin(ODIN3_AST_BINARY_ADD, ref_a, ref_b);
    type_ok(add, CTX8);
    EXPECT_FINAL(add, 8, 'u');
    type_ok(add, (odin3_etype){9, true, false});
    EXPECT_FINAL(add, 9, 'u');
    EXPECT_FINAL(ref_a, 9, 'u');
    EXPECT_FINAL(ref_b, 9, 'u');
    EXPECT_SELF(ref_a, 8, 'u');
    EXPECT_SELF(add, 8, 'u');
}

/* E22 (Chu p.45): (a + b) >> 1 drops the carry; (0 + a + b) >> 1 keeps it. */
static void test_e22_carry(void) {
    odin3_ast_id add = bin(ODIN3_AST_BINARY_ADD, id("a"), id("b"));
    odin3_ast_id shr = bin(ODIN3_AST_BINARY_SHR, add, num("1"));
    type_ok(shr, CTX8);
    EXPECT_FINAL(add, 8, 'u');
    EXPECT_FINAL(shr, 8, 'u');

    odin3_ast_id inner = bin(ODIN3_AST_BINARY_ADD, num("0"), id("a"));
    odin3_ast_id outer = bin(ODIN3_AST_BINARY_ADD, inner, id("b"));
    odin3_ast_id shr2 = bin(ODIN3_AST_BINARY_SHR, outer, num("1"));
    type_ok(shr2, CTX8);
    EXPECT_FINAL(inner, 32, 'u');
    EXPECT_FINAL(outer, 32, 'u');
    EXPECT_FINAL(shr2, 32, 'u');
}

/* E23: one unsigned operand makes the whole context unsigned. */
static void test_e23_signedness(void) {
    odin3_ast_id ref_s = id("s");
    odin3_ast_id sa = bin(ODIN3_AST_BINARY_ADD, ref_s, id("a"));
    type_ok(sa, SELF_DET);
    EXPECT_FINAL(sa, 8, 'u');
    EXPECT_FINAL(ref_s, 8, 'u');

    odin3_ast_id ref_t = id("t");
    odin3_ast_id st = bin(ODIN3_AST_BINARY_ADD, id("s"), ref_t);
    type_ok(st, SELF_DET);
    EXPECT_FINAL(st, 8, 's');
    EXPECT_FINAL(ref_t, 8, 's');
    EXPECT_SELF(ref_t, 4, 's');
}

/* E8: comparison operands are sized together, signed only when both are. */
static void test_e8_comparison(void) {
    odin3_ast_id ref_a = id("a");
    odin3_ast_id ref_t = id("t");
    odin3_ast_id lt = bin(ODIN3_AST_BINARY_LT, ref_a, ref_t);
    type_ok(lt, SELF_DET);
    EXPECT_FINAL(lt, 1, 'u');
    EXPECT_FINAL(ref_a, 8, 'u');
    EXPECT_FINAL(ref_t, 8, 'u');
    type_ok(lt, CTX8);
    EXPECT_FINAL(lt, 8, 'u');
    EXPECT_FINAL(ref_a, 8, 'u');
    EXPECT_FINAL(ref_t, 8, 'u');
    type_ok(lt, CTX16);
    EXPECT_FINAL(lt, 16, 'u');
    EXPECT_FINAL(ref_a, 8, 'u'); /* sized together, not to the context (Yosys $lt A_WIDTH 8) */
    EXPECT_FINAL(ref_t, 8, 'u');

    odin3_ast_id ref_s = id("s");
    odin3_ast_id t2 = id("t");
    type_ok(bin(ODIN3_AST_BINARY_LT, ref_s, t2), SELF_DET);
    EXPECT_FINAL(ref_s, 8, 's');
    EXPECT_FINAL(t2, 8, 's');
}

/* Shift amounts and exponents are self-determined. */
static void test_shift_and_power(void) {
    odin3_ast_id amount = id("a");
    odin3_ast_id shl = bin(ODIN3_AST_BINARY_SHL, id("t"), amount);
    type_ok(shl, SELF_DET);
    EXPECT_FINAL(shl, 4, 's');
    EXPECT_FINAL(amount, 8, 'u');

    odin3_ast_id ashr = bin(ODIN3_AST_BINARY_ASHR, id("s"), id("a"));
    type_ok(ashr, SELF_DET);
    EXPECT_FINAL(ashr, 8, 's');

    odin3_ast_id expo = id("a");
    odin3_ast_id pow = bin(ODIN3_AST_BINARY_POW, id("t"), expo);
    type_ok(pow, SELF_DET);
    EXPECT_FINAL(pow, 4, 's');
    EXPECT_FINAL(expo, 8, 'u');
}

static void test_ternary(void) {
    odin3_ast_id ref_c = id("c");
    odin3_ast_id ref_t = id("t");
    odin3_ast_id mux = et_ternary(&tree, ref_c, id("a"), ref_t);
    type_ok(mux, SELF_DET);
    EXPECT_FINAL(mux, 8, 'u');
    EXPECT_FINAL(ref_c, 1, 'u');
    EXPECT_FINAL(ref_t, 8, 'u');

    odin3_ast_id wide = id("i");
    odin3_ast_id t2 = id("t");
    odin3_ast_id smux = et_ternary(&tree, wide, id("s"), t2);
    type_ok(smux, SELF_DET);
    EXPECT_FINAL(smux, 8, 's');
    EXPECT_FINAL(wide, 32, 's');
    EXPECT_FINAL(t2, 8, 's');
}

static void test_concat_and_replicate(void) {
    odin3_ast_id ref_t = id("t");
    odin3_ast_id at = cat2(id("a"), ref_t);
    type_ok(at, SELF_DET);
    EXPECT_FINAL(at, 12, 'u');
    EXPECT_FINAL(ref_t, 4, 's');

    odin3_ast_id red = un(ODIN3_AST_UNARY_AND, id("a"));
    odin3_ast_id rb = cat2(red, id("b"));
    type_ok(rb, CTX8);
    EXPECT_FINAL(rb, 9, 'u');
    EXPECT_FINAL(red, 1, 'u');

    odin3_ast_id three = rep(num("3"), id("t"));
    type_ok(three, SELF_DET);
    EXPECT_FINAL(three, 12, 'u');
    TEST_ASSERT_EQUAL_STRING("replication count", last_what);
    TEST_ASSERT_NULL(odin3_ctype_constant_what(ty));
    TEST_ASSERT_EQUAL_INT(0, warnings_logged);
}

/* E12: an unsized constant in a concatenation is taken at its stored width, with a warning. */
static void test_concat_unsized_warning(void) {
    odin3_ast_id ref_a = id("a");
    et_col(&tree, 12);
    odin3_ast_id one = num("'h1");
    odin3_ast_id cat = cat2(ref_a, one);
    type_ok(cat, SELF_DET);
    EXPECT_FINAL(cat, 40, 'u');
    EXPECT_FINAL(one, 32, 'U');
    expect_line("x.v:1:12: warning: unsized constant in a concatenation is taken as 32 bits");
    TEST_ASSERT_EQUAL_INT(1, warnings_logged);
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

static void test_selects(void) {
    odin3_ast_id part =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, id("s"), num("3"), num("0")});
    type_ok(part, SELF_DET);
    EXPECT_FINAL(part, 4, 'u');
    TEST_ASSERT_EQUAL_STRING("range bound", last_what);

    odin3_ast_id bit = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, id("s"), num("2"), {0}});
    type_ok(bit, CTX8);
    EXPECT_FINAL(bit, 8, 'u');
    type_ok(bit, SELF_DET);
    EXPECT_FINAL(bit, 1, 'u');

    odin3_ast_id base = id("i");
    odin3_ast_id plus =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART_PLUS, id("a"), base, num("3")});
    type_ok(plus, SELF_DET);
    EXPECT_FINAL(plus, 3, 'u');
    EXPECT_FINAL(base, 32, 's');

    odin3_ast_id minus =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART_MINUS, id("a"), id("i"), id("W")});
    type_ok(minus, SELF_DET);
    EXPECT_FINAL(minus, 4, 'u');
}

static void test_signed_unsigned_calls(void) {
    odin3_ast_id ref_a = id("a");
    odin3_ast_id sa = call1("$signed", ref_a);
    type_ok(sa, SELF_DET);
    EXPECT_FINAL(sa, 8, 's');
    EXPECT_FINAL(ref_a, 8, 'u');

    odin3_ast_id us = call1("$unsigned", id("s"));
    type_ok(us, SELF_DET);
    EXPECT_FINAL(us, 8, 'u');

    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, call1("$signed", id("a")), id("t"));
    type_ok(sum, SELF_DET);
    EXPECT_FINAL(sum, 8, 's');

    odin3_ast_id clog = call1("$clog2", id("W"));
    type_ok(clog, SELF_DET);
    EXPECT_FINAL(clog, 32, 's');
}

/* Ruling I7: a reduction or logical operator takes the context width; its operands do not. */
static void test_reduction_logical_context(void) {
    const uint8_t unary[] = {ODIN3_AST_UNARY_AND, ODIN3_AST_UNARY_LNOT, ODIN3_AST_UNARY_XNOR};
    for (size_t k = 0; k < sizeof unary; k++) {
        odin3_ast_id ref_a = id("a");
        odin3_ast_id op = un(unary[k], ref_a);
        type_ok(op, CTX8);
        EXPECT_FINAL(op, 8, 'u');
        EXPECT_FINAL(ref_a, 8, 'u');
        EXPECT_SELF(op, 1, 'u');
        type_ok(op, (odin3_etype){16, true, false});
        EXPECT_FINAL(op, 16, 'u');
        EXPECT_FINAL(ref_a, 8, 'u');
    }
    odin3_ast_id ref_a = id("a");
    odin3_ast_id ref_b = id("s");
    odin3_ast_id land = bin(ODIN3_AST_BINARY_LAND, ref_a, ref_b);
    type_ok(land, (odin3_etype){16, true, false});
    EXPECT_FINAL(land, 16, 'u');
    EXPECT_FINAL(ref_a, 8, 'u');
    EXPECT_FINAL(ref_b, 8, 's');
}

/* v4 EL-30: ~&a is !(&a) — the outer operator takes the context, the reduction stays 1 bit. */
static void test_nand_reduction(void) {
    odin3_ast_id ref_a = id("a");
    odin3_ast_id nand = un(ODIN3_AST_UNARY_NAND, ref_a);
    type_ok(nand, CTX8);
    EXPECT_FINAL(nand, 8, 'u');
    EXPECT_FINAL(ref_a, 8, 'u');

    odin3_ast_id inner = un(ODIN3_AST_UNARY_AND, id("a"));
    odin3_ast_id outer = un(ODIN3_AST_UNARY_LNOT, inner);
    type_ok(outer, CTX8);
    EXPECT_FINAL(outer, 8, 'u');
    EXPECT_FINAL(inner, 1, 'u');
}

/* A function call's type is its return type; its arguments are typed by whoever binds them. */
static void test_function_calls(void) {
    odin3_ast_id arg = id("a");
    odin3_ast_id fa = call1("f", arg);
    type_ok(fa, SELF_DET);
    EXPECT_FINAL(fa, 4, 'u');
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ctype_final(ty, arg).width);
    odin3_ast_id ga = call1("g", id("a"));
    type_ok(ga, SELF_DET);
    EXPECT_FINAL(ga, 32, 's');
}

/* EL-15/EL-16 (review focus 2): P and Q stay unsized and extend by their top bit; R is sized. */
static void test_unsized_parameters(void) {
    const odin3_etype ctx64 = {64, true, false};
    odin3_ast_id ref_p = id("P");
    type_ok(ref_p, ctx64);
    EXPECT_FINAL(ref_p, 64, 'U');
    EXPECT_SELF(ref_p, 32, 'U');
    odin3_ast_id ref_q = id("Q");
    type_ok(ref_q, ctx64);
    EXPECT_FINAL(ref_q, 64, 'U');
    odin3_ast_id ref_r = id("R");
    type_ok(ref_r, ctx64);
    EXPECT_FINAL(ref_r, 64, 'u');

    odin3_ast_id lit = num("'hFFFFFFFF");
    odin3_ast_id plus = un(ODIN3_AST_UNARY_PLUS, lit);
    type_ok(plus, ctx64);
    EXPECT_FINAL(plus, 64, 'u');
    EXPECT_FINAL(lit, 64, 'U');
}

/* The width cap (§11): {1048577{1'b1}} is located at the replication. */
static void test_width_cap(void) {
    et_col(&tree, 2);
    odin3_ast_id count = num("1048577");
    et_col(&tree, 11);
    odin3_ast_id one = num("1'b1");
    et_col(&tree, 10);
    odin3_ast_id body = et_concat(&tree, &one, 1);
    et_col(&tree, 1);
    odin3_ast_id big = et_replicate(&tree, count, body);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, big, SELF_DET));
    expect_line("x.v:1:1: error: width 1048577 exceeds the limit of 1048576 bits");
    TEST_ASSERT_EQUAL_INT(1, errors_logged);
}

/* A 30,000-term left-deep + chain (brief: 10,000): every node typed, no recursion. */
static void test_deep_chain(void) {
    static odin3_ast_id nodes[2 * CHAIN_TERMS];
    uint32_t made = 0;
    odin3_ast_id acc = id("a");
    nodes[made++] = acc;
    for (uint32_t k = 1; k < CHAIN_TERMS; k++) {
        odin3_ast_id leaf = id(k % 2 != 0 ? "t" : "a");
        nodes[made++] = leaf;
        acc = bin(ODIN3_AST_BINARY_ADD, acc, leaf);
        nodes[made++] = acc;
    }
    type_ok(acc, (odin3_etype){16, true, false});
    for (uint32_t k = 0; k < made; k++) {
        odin3_etype got = odin3_ctype_final(ty, nodes[k]);
        TEST_ASSERT_EQUAL_UINT32(16, got.width);
        TEST_ASSERT_FALSE(got.is_signed);
    }
}

/* The bytes a typer holds grow with the nodes it types (the evaluator's byte budget counts every
 * level's typer: final review, finding 7). */
static void test_typer_bytes(void) {
    uint64_t before = odin3_ctype_bytes(ty);
    TEST_ASSERT_TRUE(before > 0);
    TEST_ASSERT_EQUAL_UINT64(0, odin3_ctype_bytes(NULL));
    odin3_ast_id acc = id("a");
    for (uint32_t k = 1; k < CHAIN_TERMS; k++) {
        acc = bin(ODIN3_AST_BINARY_ADD, acc, id("t"));
    }
    type_ok(acc, CTX16);
    TEST_ASSERT_TRUE(odin3_ctype_bytes(ty) >=
                     before + (uint64_t)CHAIN_TERMS * 2 * sizeof(uint64_t));
}

/* --- every operator class and role in a context wider than the operands -------------------- */

/* Expected finals in context 16 for s (8, signed) op t (4, signed), per operator class. */
typedef struct class_expect {
    const char *op; /* the operator's final kind (width 16) */
    uint32_t lhs_width;
    const char *lhs;
    uint32_t rhs_width;
    const char *rhs;
} class_expect;

enum { CLS_MAX, CLS_SHIFT, CLS_CMP, CLS_LOGIC };

static const class_expect CLASS_EXPECT[] = {
    [CLS_MAX] = {"s", 16, "s", 16, "s"},  /* both context-determined */
    [CLS_SHIFT] = {"s", 16, "s", 4, "s"}, /* the amount / exponent self-determined */
    [CLS_CMP] = {"u", 8, "s", 8, "s"},    /* sized together, not to the context */
    [CLS_LOGIC] = {"u", 8, "s", 4, "s"},  /* both self-determined */
};

static void test_binary_classes(void) {
    static const uint8_t cls[ODIN3_AST_BINARY_COUNT] = {
        [ODIN3_AST_BINARY_ADD] = CLS_MAX,     [ODIN3_AST_BINARY_SUB] = CLS_MAX,
        [ODIN3_AST_BINARY_MUL] = CLS_MAX,     [ODIN3_AST_BINARY_DIV] = CLS_MAX,
        [ODIN3_AST_BINARY_MOD] = CLS_MAX,     [ODIN3_AST_BINARY_POW] = CLS_SHIFT,
        [ODIN3_AST_BINARY_EQ] = CLS_CMP,      [ODIN3_AST_BINARY_NE] = CLS_CMP,
        [ODIN3_AST_BINARY_CASE_EQ] = CLS_CMP, [ODIN3_AST_BINARY_CASE_NE] = CLS_CMP,
        [ODIN3_AST_BINARY_LAND] = CLS_LOGIC,  [ODIN3_AST_BINARY_LOR] = CLS_LOGIC,
        [ODIN3_AST_BINARY_LT] = CLS_CMP,      [ODIN3_AST_BINARY_LE] = CLS_CMP,
        [ODIN3_AST_BINARY_GT] = CLS_CMP,      [ODIN3_AST_BINARY_GE] = CLS_CMP,
        [ODIN3_AST_BINARY_AND] = CLS_MAX,     [ODIN3_AST_BINARY_OR] = CLS_MAX,
        [ODIN3_AST_BINARY_XOR] = CLS_MAX,     [ODIN3_AST_BINARY_XNOR] = CLS_MAX,
        [ODIN3_AST_BINARY_SHL] = CLS_SHIFT,   [ODIN3_AST_BINARY_SHR] = CLS_SHIFT,
        [ODIN3_AST_BINARY_ASHL] = CLS_SHIFT,  [ODIN3_AST_BINARY_ASHR] = CLS_SHIFT,
    };
    for (uint32_t k = 0; k < ODIN3_AST_BINARY_COUNT; k++) {
        uint8_t sub = (uint8_t)k;
        const class_expect *want = &CLASS_EXPECT[cls[sub]];
        odin3_ast_id lhs = id("s");
        odin3_ast_id rhs = id("t");
        odin3_ast_id op = bin(sub, lhs, rhs);
        type_ok(op, CTX16);
        expect_type(odin3_ctype_final(ty, op), 16, want->op, sub); /* "line" = the sub-kind */
        expect_type(odin3_ctype_final(ty, lhs), want->lhs_width, want->lhs, sub);
        expect_type(odin3_ctype_final(ty, rhs), want->rhs_width, want->rhs, sub);
    }
}

/* Unary + - ~ are context-determined; ! and the reductions are 1 bit, operand self-determined. */
static void test_unary_classes(void) {
    for (uint32_t k = 0; k < ODIN3_AST_UNARY_COUNT; k++) {
        uint8_t sub = (uint8_t)k;
        bool same = sub == ODIN3_AST_UNARY_PLUS || sub == ODIN3_AST_UNARY_MINUS ||
                    sub == ODIN3_AST_UNARY_NOT;
        odin3_ast_id operand = id("t");
        odin3_ast_id op = un(sub, operand);
        type_ok(op, CTX16);
        expect_type(odin3_ctype_final(ty, op), 16, same ? "s" : "u", sub);
        expect_type(odin3_ctype_final(ty, operand), same ? 16 : 4, "s", sub);
    }
}

/* $clog2's argument, a bit-select index and a replication body keep their own types. */
static void test_self_determined_roles(void) {
    odin3_ast_id arg = id("s");
    odin3_ast_id clog_sum = bin(ODIN3_AST_BINARY_ADD, call1("$clog2", arg), id("a"));
    type_ok(clog_sum, CTX16);
    EXPECT_FINAL(clog_sum, 32, 'u');
    EXPECT_FINAL(arg, 8, 's');

    odin3_ast_id index = id("t");
    odin3_ast_id bit = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, id("s"), index, {0}});
    type_ok(bit, CTX16);
    EXPECT_FINAL(bit, 16, 'u');
    EXPECT_FINAL(index, 4, 's');

    odin3_ast_id part = id("t");
    odin3_ast_id body = et_concat(&tree, &part, 1);
    odin3_ast_id three = et_replicate(&tree, num("3"), body);
    odin3_ast_id plus = bin(ODIN3_AST_BINARY_ADD, three, id("i"));
    type_ok(plus, CTX16);
    EXPECT_FINAL(plus, 32, 'u');
    EXPECT_FINAL(three, 32, 'u');
    EXPECT_FINAL(body, 4, 'u'); /* the body is self-determined: {3{t}} replicates 4 bits */
    EXPECT_FINAL(part, 4, 's');
}

/* E12 also covers an unsized parameter (EL-16); an operator result such as -1 is sized (EL-15). */
static void test_concat_unsized_parameter(void) {
    et_col(&tree, 6);
    odin3_ast_id par = id("P");
    et_col(&tree, 1);
    odin3_ast_id cat = cat2(id("a"), par);
    type_ok(cat, SELF_DET);
    EXPECT_FINAL(cat, 40, 'u');
    expect_line("x.v:1:6: warning: unsized constant in a concatenation is taken as 32 bits");

    odin3_ast_id minus_one = cat2(id("a"), un(ODIN3_AST_UNARY_MINUS, num("1")));
    type_ok(minus_one, SELF_DET);
    EXPECT_FINAL(minus_one, 40, 'u');
    TEST_ASSERT_EQUAL_INT(1, warnings_logged);
}

/* --- leaves -------------------------------------------------------------------------------- */

/* Unsized literals take Yosys's stored width (2A §4.5): at least 32, +1 for a signed top 1. */
static void test_literal_types(void) {
    static const struct {
        const char *text;
        uint32_t width;
        char kind;
    } cases[] = {
        {"8'hFF", 8, 'u'},        {"4'sd5", 4, 's'},
        {"5", 32, 'S'},           {"'h1", 32, 'U'},
        {"'sh80000000", 33, 'S'}, {"'h80000000", 32, 'U'},
        {"2147483648", 33, 'S'},  {"'sb1", 32, 'S'},
        {"'hx", 32, 'U'},         {"'h1_0000_0000_0", 40, 'U'},
        {"3'b1", 3, 'u'},
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        odin3_ast_id lit = num(cases[k].text);
        type_ok(lit, SELF_DET);
        expect_type(odin3_ctype_final(ty, lit), cases[k].width, KIND(cases[k].kind), __LINE__);
        odin3_ast_literal value = {{0}, false, false};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(tree.ast, lit, values, &value));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(value.value.len, odin3_ctype_self(ty, lit).width,
                                         cases[k].text);
    }
}

/* L10: 8 bits per character, unsigned. ("" cannot be built: it is strtab ID 0 and a STRING's
 * name is required; the typer gives a name-0 STRING the 8 bits of 8'h00.) */
static void test_string_types(void) {
    odin3_ast_id ab = et_string(&tree, "AB");
    type_ok(ab, SELF_DET);
    EXPECT_FINAL(ab, 16, 'u');
    odin3_ast_id chr = et_string(&tree, "A");
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, chr, id("t"));
    type_ok(sum, (odin3_etype){12, true, false});
    EXPECT_FINAL(sum, 12, 'u');
    EXPECT_FINAL(chr, 12, 'u');
    EXPECT_SELF(chr, 8, 'u');
}

/* Memory words are typed by the host; only their indices are typed here. */
static void test_memory_words(void) {
    odin3_ast_id idx = id("i");
    odin3_ast_id word = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, id("m"), idx, {0}});
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, word, id("s"));
    type_ok(sum, SELF_DET);
    EXPECT_FINAL(sum, 8, 's');
    EXPECT_FINAL(word, 8, 's');
    EXPECT_FINAL(idx, 32, 's');

    odin3_ast_id inner = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, id("m"), id("c"), {0}});
    odin3_ast_id part =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, inner, num("3"), num("0")});
    type_ok(part, SELF_DET);
    EXPECT_FINAL(part, 4, 'u');
    EXPECT_FINAL(inner, 8, 's');

    odin3_ast_id row = id("i");
    odin3_ast_id col = id("c");
    odin3_ast_id m2 = id("m2");
    odin3_ast_id half = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, m2, row, {0}});
    odin3_ast_id word2 = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, half, col, {0}});
    type_ok(word2, CTX8);
    EXPECT_FINAL(word2, 8, 'u');
    EXPECT_FINAL(row, 32, 's');
    EXPECT_FINAL(col, 1, 'u');
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ctype_final(ty, half).width);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ctype_final(ty, m2).width);
}

/* Unary context operators and context through a comparison inside an addition. */
static void test_context_operators(void) {
    odin3_ast_id ref_t = id("t");
    odin3_ast_id neg = un(ODIN3_AST_UNARY_MINUS, ref_t);
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, neg, id("s"));
    type_ok(sum, (odin3_etype){10, true, false});
    EXPECT_FINAL(sum, 10, 's');
    EXPECT_FINAL(neg, 10, 's');
    EXPECT_FINAL(ref_t, 10, 's');
    type_ok(sum, (odin3_etype){10, false, false});
    EXPECT_FINAL(ref_t, 10, 'u');

    odin3_ast_id lt = bin(ODIN3_AST_BINARY_LT, id("a"), id("t"));
    odin3_ast_id plus = bin(ODIN3_AST_BINARY_ADD, lt, id("s"));
    type_ok(plus, SELF_DET);
    EXPECT_FINAL(plus, 8, 'u');
    EXPECT_FINAL(lt, 8, 'u');
    EXPECT_SELF(lt, 1, 'u');
}

/* --- diagnostics --------------------------------------------------------------------------- */

static void test_replication_errors(void) {
    et_col(&tree, 2);
    odin3_ast_id neg = id("NEG");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, rep(neg, id("a")), SELF_DET));
    expect_line("x.v:1:2: error: replication count -2 is negative");

    et_col(&tree, 3);
    odin3_ast_id sig = id("a");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, rep(sig, id("b")), SELF_DET));
    expect_line("x.v:1:3: error: replication count is not constant (it reads 'a')");
    TEST_ASSERT_EQUAL_INT(2, errors_logged);
}

/*
 * {0{a}} has width 0 (ruling, fix round 1): silent as a concatenation operand; anywhere else
 * accepted with one warning at the outermost zero-width node, as Yosys and Parmys accept it.
 */
static void test_zero_width(void) {
    odin3_ast_id zero = rep(id("ZERO"), id("a"));
    odin3_ast_id ok = cat2(id("b"), zero);
    type_ok(ok, SELF_DET);
    EXPECT_FINAL(ok, 8, 'u');
    EXPECT_FINAL(zero, 0, 'u');
    TEST_ASSERT_EQUAL_INT(0, warnings_logged);

    et_col(&tree, 5);
    odin3_ast_id alone = rep(id("ZERO"), id("a"));
    type_ok(alone, CTX8);
    expect_line("x.v:1:5: warning: zero-width concatenation");
    EXPECT_FINAL(alone, 8, 'u');
    EXPECT_SELF(alone, 0, 'u');
    type_ok(alone, SELF_DET);
    EXPECT_FINAL(alone, 0, 'u');
    TEST_ASSERT_EQUAL_INT(2, warnings_logged);

    et_col(&tree, 7);
    odin3_ast_id inside = rep(id("ZERO"), id("a"));
    et_col(&tree, 1);
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, inside, id("a"));
    type_ok(sum, CTX8);
    expect_line("x.v:1:7: warning: zero-width concatenation");
    EXPECT_FINAL(sum, 8, 'u');
    EXPECT_FINAL(inside, 8, 'u');
    TEST_ASSERT_EQUAL_INT(3, warnings_logged);
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

/* Nested zero-width nodes warn once, at the outermost. */
static void test_zero_width_nested(void) {
    odin3_ast_id inner = rep(id("ZERO"), id("a"));
    odin3_ast_id body = et_concat(&tree, &inner, 1);
    et_col(&tree, 3);
    odin3_ast_id twice = et_replicate(&tree, num("2"), body);
    type_ok(twice, SELF_DET);
    expect_line("x.v:1:3: warning: zero-width concatenation");
    TEST_ASSERT_EQUAL_INT(1, warnings_logged);
    EXPECT_FINAL(twice, 0, 'u');

    et_col(&tree, 9);
    odin3_ast_id neg = un(ODIN3_AST_UNARY_MINUS, rep(id("ZERO"), id("b")));
    odin3_ast_id red = un(ODIN3_AST_UNARY_AND, neg);
    type_ok(red, CTX8);
    expect_line("x.v:1:9: warning: zero-width concatenation");
    TEST_ASSERT_EQUAL_INT(2, warnings_logged);
    EXPECT_FINAL(red, 8, 'u');
    EXPECT_FINAL(neg, 0, 'u');
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

static void test_part_select_errors(void) {
    et_col(&tree, 9);
    odin3_ast_id width = id("ZERO");
    et_col(&tree, 1);
    odin3_ast_id sel =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART_PLUS, id("a"), id("i"), width});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, sel, SELF_DET));
    expect_line("x.v:1:9: error: part-select width 0 is not positive");

    et_col(&tree, 4);
    odin3_ast_id huge =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, id("a"), num("2147483647"), num("0")});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, huge, SELF_DET));
    expect_line("x.v:1:4: error: width 2147483648 exceeds the limit of 1048576 bits");

    et_col(&tree, 6);
    odin3_ast_id rev =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, id("a"), id("NEG"), num("1")});
    type_ok(rev, SELF_DET);
    EXPECT_FINAL(rev, 4, 'u');
}

static void test_real_and_host_errors(void) {
    et_col(&tree, 6);
    odin3_ast_id real = et_real(&tree, 1.5);
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, id("a"), real);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, sum, SELF_DET));
    expect_line("x.v:1:6: error: real number in an expression is not supported");

    et_col(&tree, 3);
    odin3_ast_id rnd = call1("$random", id("a"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, rnd, SELF_DET));
    expect_line("x.v:1:3: error: system function '$random' is not supported");

    et_col(&tree, 8);
    odin3_ast_id mem = id("m");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_ctype_expr(ty, bin(ODIN3_AST_BINARY_ADD, mem, id("a")), CTX8));
    expect_line("x.v:1:8: error: 'm' is a memory; select a word");
    TEST_ASSERT_EQUAL_INT(3, errors_logged);
}

/* A lowered cap applies to every node and to the context. */
static void test_lowered_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_width = LOWERED_WIDTH;
    odin3_elab_test_set_limits(&limits);
    const odin3_ast_id parts[] = {id("a"), id("b"), id("s")};
    et_col(&tree, 2);
    odin3_ast_id three = et_concat(&tree, parts, 3);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, three, SELF_DET));
    expect_line("x.v:1:2: error: width 24 exceeds the limit of 16 bits");

    et_col(&tree, 4);
    odin3_ast_id ref_a = id("a");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_ctype_expr(ty, ref_a, (odin3_etype){17, true, false}));
    expect_line("x.v:1:4: error: width 17 exceeds the limit of 16 bits");

    et_col(&tree, 5);
    odin3_ast_id wide = id("i");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ctype_expr(ty, wide, SELF_DET));
    expect_line("x.v:1:5: error: width 32 exceeds the limit of 16 bits");
    type_ok(id("a"), (odin3_etype){16, true, false});
}

/* --- reuse, nesting, misuse, OOM ----------------------------------------------------------- */

/* A host callback may type another expression with the same typer (a constant function body). */
static void test_nested_from_callback(void) {
    odin3_ast_id body_lhs = id("t");
    nested_root = bin(ODIN3_AST_BINARY_ADD, body_lhs, id("s"));
    odin3_ast_id ref_t = id("t");
    odin3_ast_id shl = bin(ODIN3_AST_BINARY_SHL, id("a"), rep(num("2"), ref_t));
    odin3_ast_id sel =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, id("b"), id("W"), num("1")});
    odin3_ast_id sum = bin(ODIN3_AST_BINARY_ADD, shl, sel);
    type_ok(sum, (odin3_etype){12, true, false});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, nested_status);
    EXPECT_FINAL(sum, 12, 'u');
    EXPECT_FINAL(shl, 12, 'u');
    EXPECT_FINAL(sel, 12, 'u');
    EXPECT_SELF(sel, 4, 'u');
    EXPECT_FINAL(ref_t, 4, 's');
    EXPECT_FINAL(body_lhs, 8, 's');
}

static odin3_status broken_constant(void *user, odin3_ast_id node, int64_t *out) {
    (void)user;
    (void)node;
    *out = 0;
    return ODIN3_ERR_NO_MEMORY;
}

static void test_misuse(void) {
    odin3_ctype *other = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_create(NULL, &HOST, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_create(tree.ast, NULL, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_create(tree.ast, &HOST, NULL));
    odin3_ctype_host partial = HOST;
    partial.report = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_create(tree.ast, &partial, &other));
    TEST_ASSERT_NULL(other);

    odin3_ast_id ref_a = id("a");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_expr(NULL, ref_a, SELF_DET));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_expr(ty, (odin3_ast_id){0}, SELF_DET));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ctype_expr(ty, (odin3_ast_id){ref_a.v + 100}, SELF_DET));
    odin3_ast_id range = et_node(&tree, (odin3_ast_spec){.kind = ODIN3_AST_RANGE},
                                 (const odin3_ast_id[]){num("1"), num("0")}, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ctype_expr(ty, range, SELF_DET));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ctype_expr(ty, bin(ODIN3_AST_BINARY_ADD, ref_a, id("zw")), CTX8));

    odin3_ctype_host failing = HOST;
    failing.constant = broken_constant;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ctype_create(tree.ast, &failing, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                          odin3_ctype_expr(other, rep(num("2"), id("a")), SELF_DET));
    odin3_ctype_destroy(other);

    TEST_ASSERT_EQUAL_UINT32(0, odin3_ctype_self(NULL, ref_a).width);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ctype_final(ty, (odin3_ast_id){0}).width);
    TEST_ASSERT_NULL(odin3_ctype_constant_what(NULL));
    odin3_ctype_destroy(NULL);
}

/* A tree touching every allocating path: many nodes, constants, a memory word, a call. */
static odin3_ast_id oom_tree(void) {
    odin3_ast_id acc = rep(num("2"), id("t"));
    for (int k = 0; k < 40; k++) {
        odin3_ast_id sel =
            et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, id("s"), num("5"), num("2")});
        odin3_ast_id word = et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, id("m"), id("i"), {0}});
        odin3_ast_id cmp = bin(ODIN3_AST_BINARY_LT, word, call1("f", id("a")));
        acc = bin(ODIN3_AST_BINARY_ADD, acc, cat2(sel, cmp));
    }
    return acc;
}

/* One OOM step: create and type with the fails-th allocation failing; true once it succeeds. */
static bool oom_step(odin3_ast_id root, long fails) {
    odin3_util_set_alloc_fail_after(fails);
    odin3_ctype *typer = NULL;
    odin3_status st = odin3_ctype_create(tree.ast, &HOST, &typer);
    if (st == ODIN3_OK) {
        ty = typer;
        st = odin3_ctype_expr(typer, root, (odin3_etype){10, true, false});
        ty = NULL;
    }
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    if (st == ODIN3_OK) {
        expect_type(odin3_ctype_final(typer, root), 10, "u", __LINE__);
    }
    odin3_ctype_destroy(typer);
    return st == ODIN3_OK;
}

static void test_oom(void) {
    odin3_ast_id root = oom_tree();
    odin3_ctype_destroy(ty);
    ty = NULL;
    bool done = false;
    for (long fails = 0; fails < OOM_LIMIT && !done; fails++) {
        done = oom_step(root, fails);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_in_context);
    RUN_TEST(test_e22_carry);
    RUN_TEST(test_e23_signedness);
    RUN_TEST(test_e8_comparison);
    RUN_TEST(test_shift_and_power);
    RUN_TEST(test_ternary);
    RUN_TEST(test_concat_and_replicate);
    RUN_TEST(test_concat_unsized_warning);
    RUN_TEST(test_selects);
    RUN_TEST(test_signed_unsigned_calls);
    RUN_TEST(test_reduction_logical_context);
    RUN_TEST(test_nand_reduction);
    RUN_TEST(test_function_calls);
    RUN_TEST(test_unsized_parameters);
    RUN_TEST(test_width_cap);
    RUN_TEST(test_deep_chain);
    RUN_TEST(test_typer_bytes);
    RUN_TEST(test_binary_classes);
    RUN_TEST(test_unary_classes);
    RUN_TEST(test_self_determined_roles);
    RUN_TEST(test_concat_unsized_parameter);
    RUN_TEST(test_literal_types);
    RUN_TEST(test_string_types);
    RUN_TEST(test_memory_words);
    RUN_TEST(test_context_operators);
    RUN_TEST(test_replication_errors);
    RUN_TEST(test_zero_width);
    RUN_TEST(test_zero_width_nested);
    RUN_TEST(test_part_select_errors);
    RUN_TEST(test_real_and_host_errors);
    RUN_TEST(test_lowered_cap);
    RUN_TEST(test_nested_from_callback);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom);
    return UNITY_END();
}
