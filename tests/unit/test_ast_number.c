/*
 * test_ast_number.c — unit tests for NUMBER payloads: literal pieces → bits as written (coverage
 * L4–L8), the node's sub-kind and SIGNED flag, ready payloads, the caps, §3.5.1 sizing in
 * number_value with its single warning, a reference-model sweep, and OOM.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CAPTURE = 4096,
    BITS_TEXT = 128,
    OOM_LIMIT = 16,
    BIG_DIGITS = 1 << 17, /* larger than an arena chunk: its own allocation */
    RANDOM_LITERALS = 2000,
};

static odin3_design *design;
static odin3_ast *ast;
static odin3_arena *values;
static odin3_loc at;
static char captured[CAPTURE];
static size_t errors_logged, warnings_logged;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
    warnings_logged += level == ODIN3_LOG_WARN;
    strncpy(captured, msg, sizeof captured - 1);
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    captured[0] = '\0';
    errors_logged = 0;
    warnings_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr("f.v"), &name));
    odin3_srcfile_spec spec = {.name = name, .len = 64};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    at = odin3_srcman_loc(sm, buf, 0);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_arena_destroy(values);
    values = NULL;
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_number_text text3(const char *size, const char *base, const char *digits) {
    return (odin3_number_text){odin3_bytes_cstr(size), odin3_bytes_cstr(base),
                               odin3_bytes_cstr(digits)};
}

/* A NUMBER node for the pieces, with the sub-kind and flags number_kind gives. */
static odin3_ast_id number_node(odin3_number_text pieces) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .loc = at};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_parse(ast, at, pieces, &spec.payload));
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &id));
    return id;
}

static odin3_ast_id number(const char *size, const char *base, const char *digits) {
    return number_node(text3(size, base, digits));
}

/* Bits as text, LSB first ("x1000000" for 8'b1x). */
static const char *bits_text(const uint8_t *bits, uint32_t n) {
    static char text[BITS_TEXT];
    TEST_ASSERT_LESS_THAN_UINT32(BITS_TEXT, n);
    for (uint32_t i = 0; i < n; i++) {
        text[i] = "01xz"[bits[i]];
    }
    text[n] = '\0';
    return text;
}

static odin3_value value_of(odin3_ast_id node, bool *is_signed) {
    odin3_value value = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast, node, values, &value, is_signed));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, value.kind);
    TEST_ASSERT_TRUE(odin3_value_valid(&value));
    return value;
}

/* The value's bits, LSB first, and its signedness. */
static void assert_value(odin3_ast_id node, const char *want, bool want_signed) {
    bool is_signed = !want_signed;
    odin3_value value = value_of(node, &is_signed);
    TEST_ASSERT_EQUAL_STRING(want, bits_text(value.bits, value.len));
    TEST_ASSERT_EQUAL(want_signed, is_signed);
}

static void assert_stored(odin3_ast_id node, uint32_t width, const char *bits) {
    const odin3_ast_number *num = odin3_ast_number_of(ast, node);
    TEST_ASSERT_NOT_NULL(num);
    TEST_ASSERT_EQUAL_UINT32(width, num->width);
    TEST_ASSERT_EQUAL_UINT32(strlen(bits), num->nbits);
    TEST_ASSERT_EQUAL_STRING(bits, bits_text(num->bits, num->nbits));
}

/* "1111" followed by zeros up to n characters. */
static const char *padded(const char *low, uint32_t n, char fill) {
    static char text[BITS_TEXT];
    memset(text, fill, n);
    memcpy(text, low, strlen(low));
    text[n] = '\0';
    return text;
}

/* --- §3.5.1 cases (L4–L8) ------------------------------------------------------------------ */

static void test_sized_hex_truncates_with_one_warning(void) {
    odin3_ast_id node = number("4", "'h", "FF");
    assert_stored(node, 4, "11111111");
    assert_value(node, "1111", false);
    TEST_ASSERT_EQUAL_size_t(1, warnings_logged);
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: warning: literal truncated to its size of 4 bits", captured);
    TEST_ASSERT_TRUE(odin3_ast_number_of(ast, node)->warned);
    assert_value(node, "1111", false); /* a second evaluation (a generate loop) stays quiet */
    TEST_ASSERT_EQUAL_size_t(1, warnings_logged);
    /* dropped leading zero digits are not a truncation */
    assert_value(number("8", "'h", "0FF"), "11111111", false);
    TEST_ASSERT_EQUAL_size_t(1, warnings_logged);
}

static void test_xz_extension(void) {
    odin3_ast_id all_x = number("8", "'b", "x");
    assert_stored(all_x, 8, "x");
    TEST_ASSERT_TRUE(odin3_ast_number_of(ast, all_x)->has_xz);
    assert_value(all_x, "xxxxxxxx", false);
    odin3_ast_id one_x = number("8", "'b", "1x");
    assert_stored(one_x, 8, "x1");
    assert_value(one_x, "x1000000", false); /* leading digit 1: zero-extended */
    assert_value(number("8", "'b", "z1"), "1zzzzzzz", false);
    assert_value(number("8", "'h", "x"), "xxxxxxxx", false);
    assert_value(number("8", "'h", "Zf"), "1111zzzz", false);
    assert_value(number("8", "'o", "X"), "xxxxxxxx", false);
    TEST_ASSERT_FALSE(odin3_ast_number_of(ast, number("8", "'b", "10"))->has_xz);
    TEST_ASSERT_EQUAL_size_t(0, warnings_logged);
}

static void test_unsized(void) {
    odin3_ast_id hex = number("", "'h", "F");
    assert_stored(hex, 0, "1111");
    assert_value(hex, padded("1111", 32, '0'), false);
    odin3_ast_id dec = number("", "'d", "5");
    assert_stored(dec, 0, padded("101", 32, '0')); /* an unsized decimal stores 32 bits */
    assert_value(dec, padded("101", 32, '0'), false);
    odin3_ast_id plain = number("", "", "5");
    TEST_ASSERT_EQUAL_UINT32(32, odin3_ast_number_of(ast, plain)->nbits);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_NUMBER_DEC, odin3_ast_sub(ast, plain));
    TEST_ASSERT_EQUAL_UINT16(ODIN3_AST_F_NUMBER_SIGNED, odin3_ast_flags(ast, plain));
    assert_value(plain, padded("101", 32, '0'), true);
    assert_value(number("", "'d", "x"), padded("", 32, 'x'), false);
    assert_value(number("", "'d", "?"), padded("", 32, 'z'), false);
    assert_value(number("", "'b", "z"), padded("", 32, 'z'), false);
}

/* Review Focus 3: an unsized based literal wider than 32 bits keeps every bit, no warning. */
static void test_unsized_wider_than_32(void) {
    odin3_ast_id wide = number("", "'h", "1_0000_0000");
    TEST_ASSERT_EQUAL_UINT32(36, odin3_ast_number_of(ast, wide)->nbits); /* as written */
    bool is_signed = true;
    odin3_value value = value_of(wide, &is_signed);
    TEST_ASSERT_FALSE(is_signed);
    TEST_ASSERT_EQUAL_UINT32(33, value.len);
    TEST_ASSERT_EQUAL_STRING(padded("", 32, '0'), bits_text(value.bits, 32));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, value.bits[32]);
    TEST_ASSERT_EQUAL_size_t(0, warnings_logged);
    /* x-led and zero-led unsized literals */
    assert_value(number("", "'h", "x0"), padded("0000", 32, 'x'), false);
    assert_value(number("", "'h", "0x"), padded("xxxx", 32, '0'), false);
    /* a decimal past 32 bits keeps its magnitude; signed ones gain a 0 sign bit */
    odin3_ast_id big = number("", "", "4294967296"); /* 2^32 */
    value = value_of(big, &is_signed);
    TEST_ASSERT_EQUAL_UINT32(34, value.len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, value.bits[32]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, value.bits[33]);
    TEST_ASSERT_TRUE(is_signed);
    TEST_ASSERT_EQUAL_UINT32(33, value_of(number("", "", "2147483648"), &is_signed).len);
    TEST_ASSERT_EQUAL_UINT32(32, value_of(number("", "", "2147483647"), &is_signed).len);
    TEST_ASSERT_EQUAL_UINT32(32, value_of(number("", "'d", "4294967295"), &is_signed).len);
    TEST_ASSERT_FALSE(is_signed);
    TEST_ASSERT_EQUAL_size_t(0, warnings_logged);
}

static int64_t signed_value(const odin3_value *value) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < value->len; i++) {
        TEST_ASSERT_TRUE(value->bits[i] <= ODIN3_BIT_1);
        sum |= (int64_t)value->bits[i] << i;
    }
    if (value->bits[value->len - 1] == ODIN3_BIT_1) {
        sum -= (int64_t)1 << value->len;
    }
    return sum;
}

static void test_signed(void) {
    odin3_ast_id node = number("4", "'sb", "1000");
    TEST_ASSERT_EQUAL_UINT16(ODIN3_AST_F_NUMBER_SIGNED, odin3_ast_flags(ast, node));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_NUMBER_BIN, odin3_ast_sub(ast, node));
    bool is_signed = false;
    odin3_value value = value_of(node, &is_signed);
    TEST_ASSERT_TRUE(is_signed);
    TEST_ASSERT_EQUAL_INT64(-8, signed_value(&value));
    /* a sized signed literal is zero-padded (sign extension is the expression's business) */
    value = value_of(number("8", "'SB", "1000"), &is_signed);
    TEST_ASSERT_EQUAL_INT64(8, signed_value(&value));
    value = value_of(number("8", "'sd", "200"), &is_signed);
    TEST_ASSERT_EQUAL_INT64(-56, signed_value(&value));
}

static void test_question_and_underscores(void) {
    odin3_ast_id node = number("4", "'b", "1?0z");
    assert_stored(node, 4, "z0z1");
    TEST_ASSERT_TRUE(odin3_ast_number_of(ast, node)->has_question);
    TEST_ASSERT_TRUE(odin3_ast_number_of(ast, node)->has_xz);
    assert_value(node, "z0z1", false);
    TEST_ASSERT_FALSE(odin3_ast_number_of(ast, number("4", "'b", "1z0z"))->has_question);
    assert_value(number("8", "'b", "1010_1010"), "01010101", false);
    assert_value(number("1_6", "'h", "_F_F_"), padded("11111111", 16, '0'), false);
    assert_value(number("4", "'d", "1_2"), "0011", false);
}

static void test_pieces_equal_one_token(void) {
    static const char token[] = "8'hFF";
    odin3_number_text one = {{token, 1}, {token + 1, 2}, {token + 3, 2}};
    odin3_ast_id whole = number_node(one);
    odin3_ast_id spaced = number("8", "'h", "FF"); /* `8 'h FF`: three tokens */
    const odin3_ast_number *lhs = odin3_ast_number_of(ast, whole);
    const odin3_ast_number *rhs = odin3_ast_number_of(ast, spaced);
    TEST_ASSERT_EQUAL_UINT32(lhs->width, rhs->width);
    TEST_ASSERT_EQUAL_UINT32(lhs->nbits, rhs->nbits);
    TEST_ASSERT_EQUAL_MEMORY(lhs->bits, rhs->bits, lhs->nbits);
    TEST_ASSERT_EQUAL_UINT32(odin3_ast_sub(ast, whole), odin3_ast_sub(ast, spaced));
    TEST_ASSERT_EQUAL_UINT16(odin3_ast_flags(ast, whole), odin3_ast_flags(ast, spaced));
}

static void test_sized_decimal(void) {
    odin3_ast_id node = number("4", "'d", "12");
    assert_stored(node, 4, "0011");
    assert_value(node, "0011", false);
    assert_stored(number("16", "'d", "0"), 16, "0");
    assert_value(number("4", "'d", "x"), "xxxx", false);
    assert_value(number("4", "'d", "Z"), "zzzz", false);
    assert_value(number("4", "'d", "17"), "1000", false); /* 10001 truncated */
    TEST_ASSERT_EQUAL_size_t(1, warnings_logged);
    /* 2^64: the carry across limbs */
    odin3_ast_id big = number("", "'d", "18446744073709551616");
    const odin3_ast_number *num = odin3_ast_number_of(ast, big);
    TEST_ASSERT_EQUAL_UINT32(65, num->nbits);
    for (uint32_t i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, num->bits[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, num->bits[64]);
}

static void test_number_kind(void) {
    static const struct {
        const char *size, *base;
        uint8_t sub;
        uint16_t flags;
    } cases[] = {
        {"", "", ODIN3_AST_NUMBER_DEC, ODIN3_AST_F_NUMBER_SIGNED},
        {"", "'d", ODIN3_AST_NUMBER_DEC, 0},
        {"8", "'sd", ODIN3_AST_NUMBER_DEC, ODIN3_AST_F_NUMBER_SIGNED},
        {"8", "'B", ODIN3_AST_NUMBER_BIN, 0},
        {"8", "'So", ODIN3_AST_NUMBER_OCT, ODIN3_AST_F_NUMBER_SIGNED},
        {"", "'H", ODIN3_AST_NUMBER_HEX, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t sub = 99;
        uint16_t flags = 99;
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_ast_number_kind(text3(cases[i].size, cases[i].base, "1"), &sub, &flags));
        TEST_ASSERT_EQUAL_UINT8(cases[i].sub, sub);
        TEST_ASSERT_EQUAL_UINT16(cases[i].flags, flags);
    }
}

/* --- rejections ---------------------------------------------------------------------------- */

static void expect_rejected(odin3_status want, odin3_number_text pieces) {
    uint32_t payloads = (uint32_t)ast->payloads.len;
    size_t errors = errors_logged;
    uint32_t payload = 77;
    TEST_ASSERT_EQUAL_INT(want, odin3_ast_number_parse(ast, at, pieces, &payload));
    TEST_ASSERT_EQUAL_UINT32(77, payload);
    TEST_ASSERT_EQUAL_UINT32(payloads, ast->payloads.len);
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
}

static void test_malformed_pieces(void) {
    static const char *const bad_bases[] = {"h", "'", "'s", "'x", "'hh", "'sbx", "''h"};
    for (size_t i = 0; i < sizeof bad_bases / sizeof bad_bases[0]; i++) {
        expect_rejected(ODIN3_ERR_INVALID_ARG, text3("8", bad_bases[i], "1"));
        uint8_t sub = 7;
        uint16_t flags = 7;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                              odin3_ast_number_kind(text3("8", bad_bases[i], "1"), &sub, &flags));
        TEST_ASSERT_EQUAL_UINT8(7, sub);
        TEST_ASSERT_EQUAL_UINT16(7, flags);
    }
    expect_rejected(ODIN3_ERR_INVALID_ARG, text3("8", "", "1")); /* a size without a base */
    odin3_number_text null_view = text3("", "'h", "F");
    null_view.digits.ptr = NULL;
    expect_rejected(ODIN3_ERR_INVALID_ARG, null_view);
    uint8_t sub = 0;
    uint16_t flags = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_kind(null_view, &sub, &flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_kind(text3("", "", "1"), NULL, &flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_kind(text3("", "", "1"), &sub, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_parse(ast, at, text3("", "", "1"), NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_parse(NULL, at, text3("", "", "1"), &(uint32_t){0}));
}

static void test_bad_literals_are_located(void) {
    expect_rejected(ODIN3_ERR_PARSE, text3("0", "'b", "1"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: literal of zero width", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("_", "'b", "1"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8a", "'b", "1"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: literal size is not a decimal number", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'b", "12"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: invalid digit '2' in a binary literal", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'o", "8"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'h", "G"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'d", "1x"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: invalid digit 'x' in a decimal literal", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'d", "x1"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'d", "xx"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'d", "a"));
    expect_rejected(ODIN3_ERR_PARSE, text3("8", "'h", "__"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: literal without digits", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("", "", ""));
}

static void test_caps(void) {
    expect_rejected(ODIN3_ERR_PARSE, text3("1048577", "'b", "0"));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: literal wider than 1048576 bits", captured);
    expect_rejected(ODIN3_ERR_PARSE, text3("99999999999999999999", "'b", "0"));
    number("1048576", "'b", "0"); /* at the cap */
    static char digits[ODIN3_AST_MAX_NUMBER_BITS + 2];
    memset(digits, '9', ODIN3_AST_MAX_DECIMAL_DIGITS + 1);
    digits[ODIN3_AST_MAX_DECIMAL_DIGITS + 1] = '\0';
    expect_rejected(ODIN3_ERR_PARSE, text3("", "", digits));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: decimal literal longer than 4096 digits", captured);
    digits[ODIN3_AST_MAX_DECIMAL_DIGITS] = '\0'; /* 10^4096 - 1: 13607 bits */
    TEST_ASSERT_EQUAL_UINT32(13607, odin3_ast_number_of(ast, number("", "", digits))->nbits);
    memset(digits, '1', ODIN3_AST_MAX_NUMBER_BITS + 1);
    digits[ODIN3_AST_MAX_NUMBER_BITS + 1] = '\0';
    expect_rejected(ODIN3_ERR_PARSE, text3("", "'b", digits));
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: literal digits spell more than 1048576 bits",
                             captured);
    digits[ODIN3_AST_MAX_NUMBER_BITS / 4 + 1] = '\0';
    expect_rejected(ODIN3_ERR_PARSE, text3("", "'h", digits));
    digits[ODIN3_AST_MAX_NUMBER_BITS / 4] = '\0';
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_NUMBER_BITS,
                             odin3_ast_number_of(ast, number("", "'h", digits))->nbits);
}

static void test_sealed(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect_rejected(ODIN3_ERR_INVALID_ARG, text3("", "", "1"));
    static const uint8_t one = ODIN3_BIT_1;
    odin3_ast_number num = {.nbits = 1, .bits = &one};
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &num, &payload));
}

/* --- ready payloads ------------------------------------------------------------------------ */

static void test_number_new(void) {
    uint8_t bits[4] = {ODIN3_BIT_Z, ODIN3_BIT_0, ODIN3_BIT_Z, ODIN3_BIT_1};
    odin3_ast_number num = {4, 4, bits, true, true, true};
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_BIN, .loc = at};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, at, &num, &spec.payload));
    bits[0] = ODIN3_BIT_1; /* copied, not aliased */
    odin3_ast_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &node));
    const odin3_ast_number *got = odin3_ast_number_of(ast, node);
    TEST_ASSERT_EQUAL_STRING("z0z1", bits_text(got->bits, got->nbits));
    TEST_ASSERT_TRUE(got->has_question);
    TEST_ASSERT_FALSE(got->warned); /* reset */
    bits[0] = ODIN3_BIT_Z;

    uint32_t payload = 77;
    odin3_ast_number bad = num;
    bad.has_xz = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &bad, &payload));
    uint8_t plain[2] = {ODIN3_BIT_1, ODIN3_BIT_X};
    bad = (odin3_ast_number){0, 2, plain, true, true, false}; /* ? without a z */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &bad, &payload));
    plain[1] = 4;
    bad.has_question = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &bad, &payload));
    bad = (odin3_ast_number){0, 0, plain, false, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &bad, &payload));
    bad = (odin3_ast_number){0, 1, NULL, false, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &bad, &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, NULL, &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_number_new(ast, at, &num, NULL));
    bad = num;
    bad.width = ODIN3_AST_MAX_NUMBER_BITS + 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ast_number_new(ast, at, &bad, &payload));
    bad = num;
    bad.nbits = ODIN3_AST_MAX_NUMBER_BITS + 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ast_number_new(ast, at, &bad, &payload));
    TEST_ASSERT_EQUAL_UINT32(77, payload);
    TEST_ASSERT_EQUAL_size_t(9, errors_logged);
}

static void test_value_misuse(void) {
    odin3_ast_id node = number("", "", "1");
    odin3_ast_spec ident = {.kind = ODIN3_AST_IDENT};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr("a"), &ident.name));
    odin3_ast_id other = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &ident, NULL, 0, &other));
    odin3_value value = {.len = 77};
    bool is_signed = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_value(ast, other, values, &value, &is_signed));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_ast_number_value(ast, (odin3_ast_id){99}, values, &value, &is_signed));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_value(NULL, node, values, &value, &is_signed));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_value(ast, node, NULL, &value, &is_signed));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_value(ast, node, values, NULL, &is_signed));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_value(ast, node, values, &value, NULL));
    TEST_ASSERT_EQUAL_UINT32(77, value.len);
    TEST_ASSERT_EQUAL_size_t(6, errors_logged);
    /* works after finish */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    assert_value(node, padded("1", 32, '0'), true);
}

/* --- a reference model over random literals ------------------------------------------------ */

static uint64_t rng = 0x5eed;

static uint32_t next_random(uint32_t bound) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng >> 33) % bound;
}

/* A random based literal; the expected value of width bits, LSB first, built independently. */
static char as_bit(char digit) {
    if (digit == '?') {
        return 'z';
    }
    return digit;
}

static void random_binary(char *digits, char *want, uint32_t width) {
    static const char alphabet[] = "01xz?_";
    uint32_t count = 1 + next_random(width + 4);
    for (uint32_t i = 0; i < count; i++) {
        digits[i] = alphabet[next_random(i == 0 ? 5 : 6)];
    }
    digits[count] = '\0';
    uint32_t pos = 0;
    for (uint32_t i = count; i-- > 0 && pos < width;) {
        if (digits[i] != '_') {
            want[pos++] = as_bit(digits[i]);
        }
    }
    char ext = as_bit(digits[0]);
    if (ext != 'x' && ext != 'z') {
        ext = '0';
    }
    while (pos < width) {
        want[pos++] = ext;
    }
    want[width] = '\0';
}

static void test_random_against_reference(void) {
    char digits[BITS_TEXT];
    char want[BITS_TEXT];
    char size[16];
    for (uint32_t i = 0; i < RANDOM_LITERALS; i++) {
        uint32_t width = 1 + next_random(64);
        random_binary(digits, want, width);
        TEST_ASSERT_GREATER_THAN_INT(0, snprintf(size, sizeof size, "%u", width));
        assert_value(number(size, "'b", digits), want, false);
        uint64_t magnitude = ((uint64_t)next_random(1U << 31) << 31) | next_random(1U << 31);
        TEST_ASSERT_GREATER_THAN_INT(
            0, snprintf(digits, sizeof digits, "%llu", (unsigned long long)magnitude));
        bool is_signed = false;
        odin3_value value = value_of(number("64", "'d", digits), &is_signed);
        uint64_t got = 0;
        for (uint32_t k = 0; k < value.len; k++) {
            got |= (uint64_t)value.bits[k] << k;
        }
        TEST_ASSERT_EQUAL_UINT64(magnitude, got);
    }
}

/* --- OOM ----------------------------------------------------------------------------------- */

static void fill_payloads(void) {
    uint32_t payload = 0;
    while (ast->payloads.len < ast->payloads.cap || ast->payloads.cap == 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 1.0, &payload));
    }
}

static char big_digits[BIG_DIGITS + 1];

static odin3_status step_parse(void) {
    uint32_t payload = 0;
    return odin3_ast_number_parse(ast, at, text3("", "'b", big_digits), &payload);
}

static odin3_status step_new(void) {
    static uint8_t bits[BIG_DIGITS];
    odin3_ast_number num = {0, BIG_DIGITS, bits, false, false, false};
    uint32_t payload = 0;
    return odin3_ast_number_new(ast, at, &num, &payload);
}

static long oom_sweep(odin3_status (*step)(void)) {
    for (long k = 0; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        fill_payloads();
        size_t payloads = ast->payloads.len;
        size_t arena = odin3_arena_bytes_used(ast->arena);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = step();
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            return k;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_size_t(payloads, ast->payloads.len);
        TEST_ASSERT_EQUAL_size_t(arena, odin3_arena_bytes_used(ast->arena));
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT");
    return 0;
}

static void test_oom(void) {
    memset(big_digits, '1', BIG_DIGITS);
    TEST_ASSERT_EQUAL_INT(2, oom_sweep(step_parse)); /* payload table, arena block */
    TEST_ASSERT_EQUAL_INT(2, oom_sweep(step_new));
    /* number_value: the caller's arena; nothing changes and no warning is spent */
    odin3_ast_id node = number("4", "'h", "FF");
    odin3_value value = {.len = 77};
    bool is_signed = true;
    odin3_arena *tiny = odin3_arena_create(1); /* every request needs a new chunk */
    TEST_ASSERT_NOT_NULL(tiny);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                          odin3_ast_number_value(ast, node, tiny, &value, &is_signed));
    odin3_util_set_alloc_fail_after(-1);
    odin3_arena_destroy(tiny);
    TEST_ASSERT_EQUAL_UINT32(77, value.len);
    TEST_ASSERT_TRUE(is_signed);
    TEST_ASSERT_FALSE(odin3_ast_number_of(ast, node)->warned);
    TEST_ASSERT_EQUAL_size_t(0, warnings_logged);
    assert_value(node, "1111", false);
    TEST_ASSERT_EQUAL_size_t(1, warnings_logged);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sized_hex_truncates_with_one_warning);
    RUN_TEST(test_xz_extension);
    RUN_TEST(test_unsized);
    RUN_TEST(test_unsized_wider_than_32);
    RUN_TEST(test_signed);
    RUN_TEST(test_question_and_underscores);
    RUN_TEST(test_pieces_equal_one_token);
    RUN_TEST(test_sized_decimal);
    RUN_TEST(test_number_kind);
    RUN_TEST(test_malformed_pieces);
    RUN_TEST(test_bad_literals_are_located);
    RUN_TEST(test_caps);
    RUN_TEST(test_sealed);
    RUN_TEST(test_number_new);
    RUN_TEST(test_value_misuse);
    RUN_TEST(test_random_against_reference);
    RUN_TEST(test_oom);
    return UNITY_END();
}
