/*
 * test_techlib_expr.c — unit tests for the .o3lib expression parser and integer evaluator.
 */
#include "techlib/expr.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEEP = 10000, MSG_MAX = 512, SWEEP_MAX = 5000 };

static odin3_arena *g_arena;
static odin3_strtab *g_tab;
static char g_msg[MSG_MAX];
static unsigned g_msgs;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
    g_msgs++;
}

void setUp(void) {
    g_arena = odin3_arena_create(0);
    g_tab = odin3_strtab_create();
    g_msg[0] = '\0';
    g_msgs = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_strtab_destroy(g_tab);
    odin3_arena_destroy(g_arena);
}

static uint32_t id_of(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(g_tab, odin3_bytes_cstr(name), &id));
    return id;
}

static odin3_status parse_bytes(odin3_bytes text, const odin3_expr **out) {
    const odin3_expr_parser parser = {g_arena, g_tab, "t.o3lib", 7};
    return odin3_expr_parse(&parser, text, out);
}

static const odin3_expr *parse_ok(const char *text) {
    const odin3_expr *expr = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_bytes(odin3_bytes_cstr(text), &expr), text);
    TEST_ASSERT_NOT_NULL(expr);
    return expr;
}

/* Parameters: a=12 b=5 c=3 d=0 w=8. */
static bool lookup(const void *user, uint32_t ident, int64_t *value) {
    (void)user;
    static const struct {
        const char *name;
        int64_t val;
    } params[] = {{"a", 12}, {"b", 5}, {"c", 3}, {"d", 0}, {"w", 8}};
    for (size_t i = 0; i < sizeof params / sizeof params[0]; i++) {
        if (strcmp(odin3_strtab_get(g_tab, ident), params[i].name) == 0) {
            *value = params[i].val;
            return true;
        }
    }
    return false;
}

static odin3_status eval_text(const char *text, int64_t *out) {
    const odin3_expr_env env = {lookup, NULL, g_tab};
    return odin3_expr_eval_int(parse_ok(text), &env, out);
}

static void expect_eval(const char *text, int64_t want) {
    int64_t got = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, eval_text(text, &got), text);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want, got, text);
}

typedef struct fail_case {
    const char *text;
    const char *where; /* expected message prefix ("" for none) */
    const char *fragment;
} fail_case;

static void expect_eval_fail(fail_case fc) {
    int64_t got = 0;
    g_msgs = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, eval_text(fc.text, &got), fc.text);
    TEST_ASSERT_TRUE_MESSAGE(g_msgs > 0, fc.text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_msg, fc.fragment), g_msg);
}

static void expect_parse_error(fail_case fc) {
    const odin3_expr *expr = NULL;
    g_msgs = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG,
                                  parse_bytes(odin3_bytes_cstr(fc.text), &expr), fc.text);
    TEST_ASSERT_NULL(expr);
    TEST_ASSERT_EQUAL_UINT(1, g_msgs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(g_msg, fc.where, strlen(fc.where)), g_msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_msg, fc.fragment), g_msg);
}

#define EVAL_FAIL(txt, frag) expect_eval_fail((fail_case){txt, "", frag})
#define PARSE_FAIL(txt, where, frag) expect_parse_error((fail_case){txt, where, frag})

static void test_primaries(void) {
    const odin3_expr *expr = parse_ok("  width_1  ");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_IDENT, expr->kind);
    TEST_ASSERT_EQUAL_UINT32(id_of("width_1"), expr->ident);
    TEST_ASSERT_EQUAL_UINT32(3, expr->col);
    expr = parse_ok("1_024");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_INT, expr->kind);
    TEST_ASSERT_EQUAL_INT64(1024, expr->ival);
    expect_eval("(((7)))", 7);
    expect_eval("a", 12);
}

static void test_sized_literals_four_state(void) {
    const odin3_expr *expr = parse_ok("4'b10x1");
    static const uint8_t want[] = {ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_0, ODIN3_BIT_1};
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_SIZED, expr->kind);
    TEST_ASSERT_EQUAL_UINT32(4, expr->nbits);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, expr->bits, 4);
    expr = parse_ok("4'bz");
    TEST_ASSERT_EQUAL_UINT32(4, expr->nbits);
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_Z, expr->bits[i]);
    }
    expr = parse_ok("6'b1x");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, expr->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, expr->bits[1]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, expr->bits[5]);
    expr = parse_ok("4'bx1");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, expr->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, expr->bits[3]);
    expr = parse_ok("8'hxF");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, expr->bits[3]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, expr->bits[4]);
    expr = parse_ok("2'b1111");
    TEST_ASSERT_EQUAL_UINT32(2, expr->nbits);
    expr = parse_ok("'d3");
    TEST_ASSERT_EQUAL_UINT32(32, expr->nbits);
    expr = parse_ok("4'b?");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_Z, expr->bits[0]);
}

static void test_sized_literals_eval(void) {
    expect_eval("8'hff", 255);
    expect_eval("'d3", 3);
    expect_eval("12'o777", 511);
    expect_eval("4'b1_0_1", 5);
    expect_eval("16'd1000", 1000);
    expect_eval("4'hA + 4'ha", 20);
    expect_eval("100'd7", 7);
    EVAL_FAIL("4'b10x1", "x or z");
    EVAL_FAIL("70'h8000000000000000", "overflow");
}

static void test_precedence_traps(void) {
    const odin3_expr *expr = parse_ok("a + b << 1");
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_SHL, expr->op);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_ADD, expr->a->op);
    expect_eval("a + b << 1", (12 + 5) << 1);
    expr = parse_ok("a == b & c");
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_AND, expr->op);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_EQ, expr->a->op);
    expect_eval("a == b & c", 0);
    expect_eval("2 == 2 & 2", 0);
    expr = parse_ok("~a[2]");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_UNARY, expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_NOT, expr->op);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BITSEL, expr->a->kind);
    expr = parse_ok("{2{a}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_REPL, expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_INT, expr->a->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_CONCAT, expr->b->kind);
    TEST_ASSERT_EQUAL_UINT32(1, expr->b->nitems);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_IDENT, expr->b->items[0]->kind);
}

static void test_precedence_levels(void) {
    expect_eval("1 + 2 * 3", 7);
    expect_eval("2 * 3 ** 2", 18);
    expect_eval("-2 ** 2", 4);
    expect_eval("10 - 4 - 3", 3);
    expect_eval("2 ** 3 ** 2", 64);
    expect_eval("100 / 5 / 2", 10);
    expect_eval("1 << 2 + 1", 8);
    expect_eval("1 + 2 < 4", 1);
    expect_eval("1 < 2 == 1", 1);
    expect_eval("6 & 3 ^ 1", 3);
    expect_eval("6 ^ 3 | 8", 13);
    expect_eval("1 | 0 && 0", 0);
    expect_eval("0 && 1 || 1", 1);
    expect_eval("1 || 0 && 0", 1);
    expect_eval("5 & 6 == 6", 1);
    expect_eval("!0 + 1", 2);
    expect_eval("-c + 10", 7);
    expect_eval("~0 & 255", 255);
    expect_eval("a % b * 2", 4);
}

static void test_every_operator(void) {
    static const struct {
        const char *text;
        int64_t want;
    } cases[] = {
        {"a & b", 4},
        {"a | b", 13},
        {"a ^ b", 9},
        {"a ~^ b", ~9},
        {"a ^~ b", ~9},
        {"a + b", 17},
        {"a - b", 7},
        {"a * b", 60},
        {"a / b", 2},
        {"a % b", 2},
        {"c ** 3", 27},
        {"a << 2", 48},
        {"a >> 2", 3},
        {"a == 12", 1},
        {"a != 12", 0},
        {"b < a", 1},
        {"a < b", 0},
        {"a <= 12", 1},
        {"a > b", 1},
        {"b >= a", 0},
        {"a && b", 1},
        {"a && d", 0},
        {"d || d", 0},
        {"d || b", 1},
        {"~d", -1},
        {"!a", 0},
        {"!d", 1},
        {"-a", -12},
        {"-4 >> 1", -2},
        {"-7 / 2", -3},
        {"-7 % 3", -1},
        {"2 ** 0", 1},
        {"0 ** 0", 1},
        {"(-1) ** 63", -1},
        {"1 << 62", 1LL << 62},
        {"a ? b : c", 5},
        {"d ? b : c", 3},
        {"w[3]", 1},
        {"a[3:2]", 3},
        {"a[0]", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        expect_eval(cases[i].text, cases[i].want);
    }
}

static void test_ternary(void) {
    expect_eval("1 ? 2 : 0 ? 3 : 4", 2);
    expect_eval("0 ? 1 : 0 ? 3 : 4", 4);
    expect_eval("1 ? 0 ? 5 : 6 : 7", 6);
    expect_eval("0 ? 0 ? 5 : 6 : 7", 7);
    expect_eval("1 ? 1 : 1 / 0", 1);
    expect_eval("0 ? 1 / 0 : 9", 9);
    expect_eval("0 && 1 / 0", 0);
    expect_eval("1 || 1 / 0", 1);
    expect_eval("a > b ? a - b : b - a", 7);
    const odin3_expr *expr = parse_ok("a ? b : c ? d : w");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_TERNARY, expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_TERNARY, expr->c->kind);
}

static void test_postfix_and_lists(void) {
    const odin3_expr *expr = parse_ok("a[w - 1 : 2 + 1]");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_SLICE, expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_SUB, expr->b->op);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_ADD, expr->c->op);
    expr = parse_ok("a[1][2]");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BITSEL, expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BITSEL, expr->a->kind);
    expr = parse_ok("{a, b[1], 2'b01, {c, d}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_CONCAT, expr->kind);
    TEST_ASSERT_EQUAL_UINT32(4, expr->nitems);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BITSEL, expr->items[1]->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_CONCAT, expr->items[3]->kind);
    expr = parse_ok("{w{a, b}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_REPL, expr->kind);
    TEST_ASSERT_EQUAL_UINT32(2, expr->b->nitems);
    expr = parse_ok("{ {2{a}}, b }");
    TEST_ASSERT_EQUAL_UINT32(2, expr->nitems);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_REPL, expr->items[0]->kind);
    expr = parse_ok("{a + 1}");
    TEST_ASSERT_EQUAL_UINT32(1, expr->nitems);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_ADD, expr->items[0]->op);
}

static void test_parse_errors_are_located(void) {
    PARSE_FAIL("a + * b", "t.o3lib:7:5:", "expected expression");
    PARSE_FAIL("a + ", "t.o3lib:7:5:", "expected expression");
    PARSE_FAIL("", "t.o3lib:7:1:", "expected expression");
    PARSE_FAIL("(a + b", "t.o3lib:7:1:", "'('");
    PARSE_FAIL("a + b)", "t.o3lib:7:6:", "')'");
    PARSE_FAIL("a b", "t.o3lib:7:3:", "unexpected");
    PARSE_FAIL("a # b", "t.o3lib:7:3:", "character");
    PARSE_FAIL("4'b102", "t.o3lib:7:6:", "digit");
    PARSE_FAIL("4'q1", "t.o3lib:7:3:", "base");
    PARSE_FAIL("4'b", "t.o3lib:7:3:", "digits");
    PARSE_FAIL("0'b1", "t.o3lib:7:1:", "size");
    PARSE_FAIL("99999999999999999999", "t.o3lib:7:1:", "too large");
    PARSE_FAIL("a ? b", "t.o3lib:7:3:", "'?'");
    PARSE_FAIL("a : b", "t.o3lib:7:3:", "':'");
    PARSE_FAIL("{}", "t.o3lib:7:2:", "expected expression");
    PARSE_FAIL("{a,}", "t.o3lib:7:4:", "expected expression");
    PARSE_FAIL("{a", "t.o3lib:7:1:", "'{'");
    PARSE_FAIL("a[1", "t.o3lib:7:2:", "'['");
    PARSE_FAIL("a[1:2:3]", "t.o3lib:7:6:", "':'");
    PARSE_FAIL("a, b", "t.o3lib:7:2:", "','");
    PARSE_FAIL("{2{a} b}", "t.o3lib:7:7:", "'}'");
    PARSE_FAIL("(a]", "t.o3lib:7:3:", "']'");
    PARSE_FAIL("a[1)", "t.o3lib:7:4:", "')'");
}

static void test_eval_errors(void) {
    EVAL_FAIL("9223372036854775807 + 1", "overflow");
    EVAL_FAIL("-9223372036854775807 - 2", "overflow");
    EVAL_FAIL("4611686018427387904 * 2", "overflow");
    EVAL_FAIL("2 ** 63", "overflow");
    EVAL_FAIL("3 ** 64", "overflow");
    EVAL_FAIL("1 << 63", "overflow");
    EVAL_FAIL("1 << 200", "overflow");
    EVAL_FAIL("1 << (0 - 1)", "negative");
    EVAL_FAIL("1 >> (0 - 1)", "negative");
    EVAL_FAIL("2 ** (0 - 1)", "negative");
    EVAL_FAIL("1 / 0", "zero");
    EVAL_FAIL("1 % 0", "zero");
    EVAL_FAIL("(0 - 9223372036854775807 - 1) / (0 - 1)", "overflow");
    EVAL_FAIL("-(0 - 9223372036854775807 - 1)", "overflow");
    EVAL_FAIL("zork + 1", "unknown identifier 'zork'");
    EVAL_FAIL("{a, b}", "concatenation");
    EVAL_FAIL("{2{a}}", "replication");
    EVAL_FAIL("a[64]", "bit");
    EVAL_FAIL("a[2:3]", "slice");
    expect_eval("1 >> 200", 0);
    expect_eval("(0 - 5) >> 200", -1);
    int64_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_expr_eval_int(parse_ok("a"), NULL, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_expr_eval_int(parse_ok("1 + 1"), NULL, &got));
    TEST_ASSERT_EQUAL_INT64(2, got);
}

static void test_bad_arguments(void) {
    const odin3_expr *expr = NULL;
    const odin3_expr_parser bad = {NULL, g_tab, "f", 1};
    int64_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_expr_parse(&bad, odin3_bytes_cstr("a"), &expr));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_expr_parse(NULL, odin3_bytes_cstr("a"), &expr));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse_bytes(odin3_bytes_cstr("a"), NULL));
    const odin3_bytes null_text = {NULL, 3};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse_bytes(null_text, &expr));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_expr_eval_int(NULL, NULL, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_expr_eval_int(parse_ok("1"), NULL, NULL));
    const odin3_expr_parser anon = {g_arena, g_tab, NULL, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_expr_parse(&anon, odin3_bytes_cstr("+"), &expr));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "<expr>:2:1:"));
    const odin3_bytes with_nul = {"a\0b", 3};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse_bytes(with_nul, &expr));
}

/* text = repeat(open, n) + body + repeat(close, n) */
static char *wrap(const char *open, const char *body, const char *close, size_t count) {
    const size_t olen = strlen(open);
    const size_t clen = strlen(close);
    const size_t blen = strlen(body);
    char *buf = malloc(count * (olen + clen) + blen + 1);
    TEST_ASSERT_NOT_NULL(buf);
    char *cursor = buf;
    for (size_t i = 0; i < count; i++) {
        memcpy(cursor, open, olen);
        cursor += olen;
    }
    memcpy(cursor, body, blen);
    cursor += blen;
    for (size_t i = 0; i < count; i++) {
        memcpy(cursor, close, clen);
        cursor += clen;
    }
    *cursor = '\0';
    return buf;
}

static void test_depth_parens(void) {
    char *text = wrap("(", "7", ")", DEEP);
    expect_eval(text, 7);
    free(text);
    text = wrap("-(", "7", ")", DEEP);
    expect_eval(text, 7); /* DEEP is even: the negations cancel */
    free(text);
    text = wrap("(", "1", " + 1)", DEEP);
    expect_eval(text, DEEP + 1);
    free(text);
}

static void test_depth_braces_and_unary(void) {
    char *text = wrap("{", "a", "}", DEEP);
    const odin3_expr *expr = parse_ok(text);
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_CONCAT, expr->kind);
    int64_t got = 0;
    const odin3_expr_env env = {lookup, NULL, g_tab};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_expr_eval_int(expr, &env, &got));
    text = wrap("{2{", "a", "}}", DEEP);
    expr = parse_ok(text);
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_REPL, expr->kind);
    text = wrap("~", "5", "", DEEP);
    expect_eval(text, 5);
    free(text);
    text = wrap("!", "5", "", DEEP);
    expect_eval(text, 1);
    free(text);
}

static void test_depth_chains(void) {
    char *text = wrap("", "1", " + 1", DEEP);
    expect_eval(text, DEEP + 1);
    free(text);
    text = wrap("", "1", " ** 1", DEEP);
    expect_eval(text, 1);
    free(text);
    text = wrap("1 ? ", "9", " : 0", DEEP);
    expect_eval(text, 9);
    free(text);
    text = wrap("0 ? 1 : ", "9", "", DEEP);
    expect_eval(text, 9);
    free(text);
    text = wrap("(1 ? ", "9", " : 0)", DEEP);
    expect_eval(text, 9);
    free(text);
    text = wrap("a[", "1", "]", DEEP);
    parse_ok(text);
    free(text);
}

/* One parse attempt with the nth allocation failing, in a fresh arena. */
static odin3_status parse_with_failure(const char *text, long nth, int64_t *value) {
    odin3_arena *arena = odin3_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);
    const odin3_expr_parser parser = {arena, g_tab, "t", 1};
    const odin3_expr *expr = NULL;
    odin3_util_set_alloc_fail_after(nth);
    const odin3_status st = odin3_expr_parse(&parser, odin3_bytes_cstr(text), &expr);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_expr_eval_int(expr, NULL, value));
    } else {
        TEST_ASSERT_NULL(expr);
    }
    odin3_arena_destroy(arena);
    return st;
}

static void test_oom_sweep_parse(void) {
    char *text = wrap("(", "1", " + 1)", 200);
    int64_t value = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long tries = 0; tries < SWEEP_MAX && st != ODIN3_OK; tries++) {
        st = parse_with_failure(text, tries, &value);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_INT64(201, value);
}

static void test_oom_sweep_eval(void) {
    char *text = wrap("(", "1", " + 1)", 200);
    const odin3_expr *expr = parse_ok(text);
    int64_t value = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long tries = 0; tries < SWEEP_MAX && st != ODIN3_OK; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        st = odin3_expr_eval_int(expr, NULL, &value);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_INT64(201, value);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_primaries);
    RUN_TEST(test_sized_literals_four_state);
    RUN_TEST(test_sized_literals_eval);
    RUN_TEST(test_precedence_traps);
    RUN_TEST(test_precedence_levels);
    RUN_TEST(test_every_operator);
    RUN_TEST(test_ternary);
    RUN_TEST(test_postfix_and_lists);
    RUN_TEST(test_parse_errors_are_located);
    RUN_TEST(test_eval_errors);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_depth_parens);
    RUN_TEST(test_depth_braces_and_unary);
    RUN_TEST(test_depth_chains);
    RUN_TEST(test_oom_sweep_parse);
    RUN_TEST(test_oom_sweep_eval);
    return UNITY_END();
}
