/*
 * test_elab_cval.c — 2D constant values (spec §4.1, EL-12/14/15): every line of the Yosys 0.55
 * constant table (tests/elab/const_table.txt, tools/elab-check/gen-const-table) replayed through
 * odin3_cval_op, the corner cases of division, power, 4-state logic, shifts and the mux, wide
 * (Knuth) division, literals and their unsized extension, strings, printing, NUMBER payloads,
 * hashing, parts, the width cap and its test hook, misuse and OOM.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_ELAB_DATA
#error "ODIN3_ELAB_DATA (tests/elab) must be defined by the build"
#endif

enum {
    TEXT = 1 << 12,
    LINE = 1 << 11,
    PLAIN_FIELDS = 6,
    MIXED_FIELDS = 8,
    MIN_MIXED = 2000,
    MIN_TABLE = 3000,
    MAX_REPORTED = 20,
    OOM_LIMIT = 64,
};

static odin3_arena *arena;
static odin3_design *design;
static odin3_ast *ast;
static odin3_loc at;
static size_t errors_logged;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    errors_logged = 0;
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr("c.v"), &name));
    odin3_srcfile_spec spec = {.name = name, .len = 64};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    at = odin3_srcman_loc(sm, buf, 0);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_arena_destroy(arena);
    arena = NULL;
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

/* A value from MSB-first text in 0 1 x z. */
static odin3_cval cv(const char *msb, uint8_t flags) {
    size_t len = strlen(msb);
    odin3_cval val = {0};
    const odin3_cval_spec spec = {(uint32_t)len, flags, ODIN3_BIT_0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_new(arena, &spec, &val));
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;
        uint64_t mask = (uint64_t)1 << (pos % 64);
        char ch = msb[i];
        TEST_ASSERT_NOT_NULL(strchr("01xz", ch));
        val.aval[pos / 64] |= ch == '1' || ch == 'x' ? mask : 0;
        val.bval[pos / 64] |= ch == 'x' || ch == 'z' ? mask : 0;
    }
    return val;
}

/* The value's bits, MSB first (a static buffer). */
static const char *text_of(const odin3_cval *val) {
    static char text[TEXT];
    TEST_ASSERT_LESS_THAN_UINT32(TEXT, val->width);
    for (uint32_t i = 0; i < val->width; i++) {
        text[i] = "01xz"[odin3_cval_bit(val, val->width - 1 - i)];
    }
    text[val->width] = '\0';
    return text;
}

static odin3_cval op_at(const odin3_cop_args *args) {
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_op(arena, args, &out));
    TEST_ASSERT_EQUAL_UINT32(args->width, out.width);
    return out;
}

/* a op b at the operands' width with the context sign; b NULL for a unary op. */
static const char *op2(odin3_cop op, const char *lhs, const char *rhs, bool is_signed) {
    uint8_t flags = is_signed ? ODIN3_CVAL_SIGNED : 0;
    odin3_cval left = cv(lhs, flags);
    odin3_cval right = cv(rhs != NULL ? rhs : "0", flags);
    const odin3_cop_args args = {op, &left, rhs != NULL ? &right : NULL, left.width, is_signed};
    odin3_cval out = op_at(&args);
    return text_of(&out);
}

static const char *op1(odin3_cop op, const char *lhs, uint32_t width) {
    odin3_cval left = cv(lhs, 0);
    const odin3_cop_args args = {op, &left, NULL, width, false};
    odin3_cval out = op_at(&args);
    return text_of(&out);
}

static odin3_cval from_i64(int64_t value, uint32_t width, bool is_signed) {
    odin3_cval out = {0};
    const odin3_cval_int num = {value, width, is_signed};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_i64(arena, &num, &out));
    return out;
}

/* A literal's value through the AST (odin3_ast_number_value) and odin3_cval_from_literal. */
static odin3_cval lit(const char *size, const char *base, const char *digits) {
    odin3_number_text pieces = {odin3_bytes_cstr(size), odin3_bytes_cstr(base),
                                odin3_bytes_cstr(digits)};
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .loc = at.v};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_parse(ast, at, pieces, &spec.payload));
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &id));
    odin3_ast_literal value = {{0}, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast, id, arena, &value));
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(arena, &value, &out));
    return out;
}

static const char *printed(const odin3_cval *val) {
    static char text[TEXT];
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_print(val, &buf));
    TEST_ASSERT_LESS_THAN_size_t(TEXT, buf.len);
    memcpy(text, buf.data != NULL ? buf.data : "", buf.len);
    text[buf.len] = '\0';
    odin3_strbuf_free(&buf);
    return text;
}

static const char *repeat(char ch, uint32_t n) {
    static char text[TEXT];
    TEST_ASSERT_LESS_THAN_UINT32(TEXT, n);
    memset(text, ch, n);
    text[n] = '\0';
    return text;
}

/* --- the Yosys 0.55 constant table --------------------------------------------------------- */

static const char *const k_op_names[ODIN3_COP_COUNT] = {
    "ADD",  "SUB",  "MUL", "DIV",  "MOD",  "POW",   "AND", "OR",   "XOR",  "XNOR", "SHL",  "SHR",
    "SSHL", "SSHR", "EQ",  "NE",   "EQX",  "NEX",   "LT",  "LE",   "GT",   "GE",   "LAND", "LOR",
    "NEG",  "POS",  "NOT", "LNOT", "RAND", "RNAND", "ROR", "RNOR", "RXOR", "RXNOR"};

static odin3_cop op_named(const char *name) {
    for (int i = 0; i < ODIN3_COP_COUNT; i++) {
        if (strcmp(name, k_op_names[i]) == 0) {
            return (odin3_cop)i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return ODIN3_COP_COUNT;
}

/* A table line's fields: six in const_table.txt, eight in const_table_mixed.txt. */
typedef struct table_line {
    char text[LINE];
    const char *field[MIXED_FIELDS];
} table_line;

static void split_line(table_line *row, const char *line, int fields) {
    TEST_ASSERT_LESS_THAN_size_t(LINE, strlen(line));
    strcpy(row->text, line);
    char *save = NULL;
    char *word = strtok_r(row->text, " ", &save);
    for (int i = 0; i < fields; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(word, line);
        row->field[i] = word;
        word = strtok_r(NULL, " ", &save);
    }
    TEST_ASSERT_NULL_MESSAGE(word, line);
}

/* A mixed table's operand flags: u s sized unsigned/signed, U S unsized unsigned/signed. */
static uint8_t flags_of(const char *code) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strchr("usUS", code[0]), code);
    uint8_t flags = code[0] == 's' || code[0] == 'S' ? ODIN3_CVAL_SIGNED : 0;
    return (uint8_t)(flags | (code[0] == 'U' || code[0] == 'S' ? ODIN3_CVAL_UNSIZED : 0));
}

/* EL-14's result flags: the context sign for arithmetic, bitwise, shifts and **; unsigned for
 * comparisons, logical operators and reductions; never UNSIZED or STRING. */
static uint8_t result_flags(odin3_cop op, bool is_signed) {
    bool by_context =
        op <= ODIN3_COP_SSHR || op == ODIN3_COP_NEG || op == ODIN3_COP_POS || op == ODIN3_COP_NOT;
    return by_context && is_signed ? ODIN3_CVAL_SIGNED : 0;
}

/* One case of a table: false (reported) when odin3_cval_op disagrees with Yosys. */
typedef struct table_case {
    const odin3_cop_args *args;
    const char *want;
    const char *line;
} table_case;

static bool check_case(const table_case *tc) {
    odin3_cval out = op_at(tc->args);
    uint8_t flags = result_flags(tc->args->op, tc->args->is_signed);
    if (strcmp(tc->want, text_of(&out)) == 0 && out.flags == flags) {
        return true;
    }
    printf("const_table: %s  got %s (flags %u)\n", tc->line, text_of(&out), out.flags);
    return false;
}

/* <op> <width> <s> <a> <b> <result>: both operands at the width and sign. */
static bool plain_line(const char *line) {
    static table_line row;
    split_line(&row, line, PLAIN_FIELDS);
    bool is_signed = strcmp(row.field[2], "1") == 0;
    bool unary = strcmp(row.field[4], "-") == 0;
    uint8_t flags = is_signed ? ODIN3_CVAL_SIGNED : 0;
    odin3_cval left = cv(row.field[3], flags);
    odin3_cval right = cv(unary ? "0" : row.field[4], flags);
    const odin3_cop_args args = {op_named(row.field[0]), &left, unary ? NULL : &right,
                                 (uint32_t)strtoul(row.field[1], NULL, 10), is_signed};
    const table_case tc = {&args, row.field[5], line};
    return check_case(&tc);
}

/* <op> <width> <s> <a> <a-flags> <b> <b-flags> <result>: the context, then each operand. */
static bool mixed_line(const char *line) {
    static table_line row;
    split_line(&row, line, MIXED_FIELDS);
    bool unary = strcmp(row.field[5], "-") == 0;
    odin3_cval left = cv(row.field[3], flags_of(row.field[4]));
    odin3_cval right = cv(unary ? "0" : row.field[5], unary ? 0 : flags_of(row.field[6]));
    const odin3_cop_args args = {op_named(row.field[0]), &left, unary ? NULL : &right,
                                 (uint32_t)strtoul(row.field[1], NULL, 10),
                                 strcmp(row.field[2], "1") == 0};
    const table_case tc = {&args, row.field[7], line};
    return check_case(&tc);
}

typedef bool (*line_check)(const char *line);

/* Replays every case line of a table; returns how many there were. */
static uint32_t replay(const char *path, line_check check) {
    FILE *file = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char line[LINE];
    uint32_t cases = 0;
    uint32_t wrong = 0;
    bool header = false;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#') {
            header = header || strncmp(line, "# yosys: Yosys 0.55", 19) == 0;
            continue;
        }
        cases++;
        wrong += check(line) ? 0 : 1;
        TEST_ASSERT_LESS_THAN_UINT32(MAX_REPORTED, wrong);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE_MESSAGE(header, "the table names Yosys 0.55");
    TEST_ASSERT_EQUAL_UINT32(0, wrong);
    return cases;
}

static void test_const_table(void) {
    uint32_t cases = replay(ODIN3_ELAB_DATA "/const_table.txt", plain_line);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MIN_TABLE, cases);
}

/* Operand widths, signs and sizes drawn independently of each other and of the context. */
static void test_const_table_mixed(void) {
    uint32_t cases = replay(ODIN3_ELAB_DATA "/const_table_mixed.txt", mixed_line);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MIN_MIXED, cases);
}

/* The context sign is not an operand's (the review's three mutants; Yosys 0.55 probe p2.v). */
static void test_context_sign(void) {
    /* ** reads b's own SIGNED: 8'sd3 ** 8'b11111111 is 3^255 mod 256 */
    odin3_cval base = cv("00000011", ODIN3_CVAL_SIGNED);
    odin3_cval power = cv("11111111", 0);
    odin3_cop_args args = {ODIN3_COP_POW, &base, &power, 8, true};
    odin3_cval out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("10101011", text_of(&out));
    /* an unsigned / zero-extends a signed divisor: 8'd7 / 8'sb11111110 is 7 / 254 */
    odin3_cval seven = cv("00000111", 0);
    odin3_cval divisor = cv("11111110", ODIN3_CVAL_SIGNED);
    args = (odin3_cop_args){ODIN3_COP_DIV, &seven, &divisor, 8, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("00000000", text_of(&out));
    /* >>> fills by the context sign: (4'sb1000 >>> 1) + 1'b0 */
    odin3_cval negative = cv("1000", ODIN3_CVAL_SIGNED);
    odin3_cval one = cv("1", 0);
    args = (odin3_cop_args){ODIN3_COP_SSHR, &negative, &one, 4, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("0100", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
}

static void test_result_flags(void) {
    odin3_cval three = cv("0011", ODIN3_CVAL_SIGNED);
    odin3_cval four = cv("0100", ODIN3_CVAL_SIGNED);
    odin3_cop_args args = {ODIN3_COP_LT, &three, &four, 4, true};
    odin3_cval out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("0001", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags); /* a comparison is unsigned in any context */
    odin3_cval wide = lit("", "'h", "FFFF_FFFF");
    args = (odin3_cop_args){ODIN3_COP_POS, &wide, NULL, 32, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_UINT8(0, out.flags); /* an operator's result is sized (EL-15) */
    args.is_signed = true;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, out.flags);
    odin3_cval text = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("A"), &text));
    args = (odin3_cop_args){ODIN3_COP_NOT, &text, NULL, 8, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_UINT8(0, out.flags); /* and never STRING */
}

/* --- the brief's corner cases -------------------------------------------------------------- */

static void test_division(void) {
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_DIV, "00000111", "00000000", false));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_MOD, "00000111", "00000000", false));
    TEST_ASSERT_EQUAL_STRING("11111101", op2(ODIN3_COP_DIV, "11111001", "00000010", true));
    TEST_ASSERT_EQUAL_STRING("11111111", op2(ODIN3_COP_MOD, "11111001", "00000010", true));
    /* unsigned: 249 / 2 and 249 % 2 */
    TEST_ASSERT_EQUAL_STRING("01111100", op2(ODIN3_COP_DIV, "11111001", "00000010", false));
    TEST_ASSERT_EQUAL_STRING("00000001", op2(ODIN3_COP_MOD, "11111001", "00000010", false));
    /* the most negative value / -1 wraps; % takes the dividend's sign */
    TEST_ASSERT_EQUAL_STRING("10000000", op2(ODIN3_COP_DIV, "10000000", "11111111", true));
    TEST_ASSERT_EQUAL_STRING("00000001", op2(ODIN3_COP_MOD, "00000111", "11111101", true));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_DIV, "0000011x", "00000010", false));
}

static void test_power(void) {
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_POW, "00000000", "11111111", true));
    TEST_ASSERT_EQUAL_STRING("00000000", op2(ODIN3_COP_POW, "00000000", "00000011", true));
    TEST_ASSERT_EQUAL_STRING("00000000", op2(ODIN3_COP_POW, "00000010", "11111111", true));
    TEST_ASSERT_EQUAL_STRING("11111111", op2(ODIN3_COP_POW, "11111111", "11111101", true));
    TEST_ASSERT_EQUAL_STRING("00000001", op2(ODIN3_COP_POW, "11111111", "11111110", true));
    TEST_ASSERT_EQUAL_STRING("00000001", op2(ODIN3_COP_POW, "00000001", "11111011", true));
    TEST_ASSERT_EQUAL_STRING("11110011", op2(ODIN3_COP_POW, "00000011", "00000101", true));
    TEST_ASSERT_EQUAL_STRING("01010111", op2(ODIN3_COP_POW, "00000111", "00000011", true));
    /* 0 ** 0 is 1; an unsigned exponent of all ones is positive; Yosys's x order */
    TEST_ASSERT_EQUAL_STRING("00000001", op2(ODIN3_COP_POW, "00000000", "00000000", true));
    TEST_ASSERT_EQUAL_STRING("00000000", op2(ODIN3_COP_POW, "00000010", "11111111", false));
    TEST_ASSERT_EQUAL_STRING("00000000", op2(ODIN3_COP_POW, "0000000x", "00000011", false));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_POW, "0000001x", "00000011", false));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", op2(ODIN3_COP_POW, "00000011", "0000000x", false));
}

static void test_bitwise(void) {
    TEST_ASSERT_EQUAL_STRING("00x1", op2(ODIN3_COP_AND, "10x1", "0011", false));
    TEST_ASSERT_EQUAL_STRING("11x1", op2(ODIN3_COP_OR, "10x1", "0100", false));
    TEST_ASSERT_EQUAL_STRING("x000", op2(ODIN3_COP_AND, "z000", "1000", false));
    TEST_ASSERT_EQUAL_STRING("1xx0", op2(ODIN3_COP_XOR, "10x1", "0zx1", false));
    TEST_ASSERT_EQUAL_STRING("0xx1", op2(ODIN3_COP_XNOR, "10x1", "0zx1", false));
    TEST_ASSERT_EQUAL_STRING("01xx", op2(ODIN3_COP_NOT, "10xz", NULL, false));
    TEST_ASSERT_EQUAL_STRING("10xz", op2(ODIN3_COP_POS, "10xz", NULL, false));
    TEST_ASSERT_EQUAL_STRING("xxxx", op2(ODIN3_COP_NEG, "10xz", NULL, false));
    TEST_ASSERT_EQUAL_STRING("1101", op2(ODIN3_COP_NEG, "0011", NULL, false));
}

static void test_reductions(void) {
    TEST_ASSERT_EQUAL_STRING("x", op1(ODIN3_COP_RAND, "1x11", 1));
    TEST_ASSERT_EQUAL_STRING("0", op1(ODIN3_COP_RAND, "0x11", 1));
    TEST_ASSERT_EQUAL_STRING("x", op1(ODIN3_COP_ROR, "0x00", 1));
    TEST_ASSERT_EQUAL_STRING("1", op1(ODIN3_COP_ROR, "1x00", 1));
    TEST_ASSERT_EQUAL_STRING("00000001", op1(ODIN3_COP_RAND, "11111111", 8));
    TEST_ASSERT_EQUAL_STRING("0000000x", op1(ODIN3_COP_RNAND, "1z11", 8));
    TEST_ASSERT_EQUAL_STRING("1", op1(ODIN3_COP_RNOR, "0000", 1));
    TEST_ASSERT_EQUAL_STRING("1", op1(ODIN3_COP_RXOR, "0111", 1));
    TEST_ASSERT_EQUAL_STRING("0", op1(ODIN3_COP_RXNOR, "0111", 1));
    TEST_ASSERT_EQUAL_STRING("x", op1(ODIN3_COP_RXNOR, "01z1", 1));
    TEST_ASSERT_EQUAL_STRING("0", op1(ODIN3_COP_LNOT, "0010", 1));
    TEST_ASSERT_EQUAL_STRING("x", op1(ODIN3_COP_LNOT, "00x0", 1));
    TEST_ASSERT_EQUAL_STRING("0000", op2(ODIN3_COP_LAND, "0000", "x111", false));
    TEST_ASSERT_EQUAL_STRING("000x", op2(ODIN3_COP_LAND, "0010", "x000", false));
    TEST_ASSERT_EQUAL_STRING("0001", op2(ODIN3_COP_LOR, "0100", "xxxx", false));
    TEST_ASSERT_EQUAL_STRING("000x", op2(ODIN3_COP_LOR, "0000", "z000", true));
}

static void test_equality(void) {
    TEST_ASSERT_EQUAL_STRING("000x", op2(ODIN3_COP_EQ, "1x00", "1000", false));
    TEST_ASSERT_EQUAL_STRING("0001", op2(ODIN3_COP_EQX, "1x00", "1x00", false));
    TEST_ASSERT_EQUAL_STRING("0000", op2(ODIN3_COP_NEX, "1x00", "1x00", false));
    /* Yosys const_eq: a 0/1 mismatch decides even beside an x bit */
    TEST_ASSERT_EQUAL_STRING("0000", op2(ODIN3_COP_EQ, "1x01", "1000", false));
    TEST_ASSERT_EQUAL_STRING("0001", op2(ODIN3_COP_NE, "1x01", "1000", false));
    TEST_ASSERT_EQUAL_STRING("0000", op2(ODIN3_COP_EQX, "1z00", "1x00", false));
    TEST_ASSERT_EQUAL_STRING("000x", op2(ODIN3_COP_LT, "1z00", "1100", false));
    TEST_ASSERT_EQUAL_STRING("0001", op2(ODIN3_COP_LT, "1000", "0111", true));
    TEST_ASSERT_EQUAL_STRING("0000", op2(ODIN3_COP_LT, "1000", "0111", false));
}

static void test_mixed_widths(void) {
    /* comparisons size the operands together, signed only when both are */
    odin3_cval lhs = cv("1111", ODIN3_CVAL_SIGNED);
    odin3_cval rhs = cv("11111111", ODIN3_CVAL_SIGNED);
    odin3_cop_args args = {ODIN3_COP_EQ, &lhs, &rhs, 1, false};
    odin3_cval out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("1", text_of(&out));
    rhs.flags = 0;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("0", text_of(&out));
    /* arithmetic extends both by the context sign, then works at the context width */
    args = (odin3_cop_args){ODIN3_COP_ADD, &lhs, &lhs, 8, true};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("11111110", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, out.flags);
    args.is_signed = false;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("00011110", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    /* an unsized operand repeats its top bit in any context: 2^40 - 1 + 15 at 40 bits */
    odin3_cval wide = lit("", "'h", "FFFF_FFFF");
    args = (odin3_cop_args){ODIN3_COP_ADD, &wide, &lhs, 40, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000001110", text_of(&out));
    TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000001110",
                             op1(ODIN3_COP_POS, "1110", 40));
}

static void test_shifts(void) {
    TEST_ASSERT_EQUAL_STRING("11100000", op2(ODIN3_COP_SSHR, "10000000", "00000010", true));
    TEST_ASSERT_EQUAL_STRING("00100000", op2(ODIN3_COP_SSHR, "10000000", "00000010", false));
    odin3_cval lhs = cv("10000000", ODIN3_CVAL_SIGNED);
    odin3_cval amount = cv("x1", 0);
    odin3_cop_args args = {ODIN3_COP_SSHR, &lhs, &amount, 8, true};
    odin3_cval out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("xxxxxxxx", text_of(&out));
    amount = cv("1000", 0);
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("11111111", text_of(&out));
    args.op = ODIN3_COP_SHL;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("00000000", text_of(&out));
    args.op = ODIN3_COP_SHR;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING("00000000", text_of(&out));
    /* a signed amount is still an unsigned amount; x shifted out leaves no trace */
    TEST_ASSERT_EQUAL_STRING("00000000", op2(ODIN3_COP_SHL, "00000001", "11111111", true));
    TEST_ASSERT_EQUAL_STRING("11100000", op2(ODIN3_COP_SSHR, "1000000x", "00000010", true));
    TEST_ASSERT_EQUAL_STRING("xxx00000", op2(ODIN3_COP_SSHR, "x0000000", "00000010", true));
    TEST_ASSERT_EQUAL_STRING("0x100000", op2(ODIN3_COP_SSHL, "000x1000", "00000010", true));
}

static void test_mux(void) {
    odin3_cval cond = cv("x", 0);
    odin3_cval lhs = cv("1100", 0);
    odin3_cval rhs = cv("1010", 0);
    odin3_cval out = {0};
    odin3_cval_mux_args args = {&cond, &lhs, &rhs};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_mux(arena, &args, &out));
    TEST_ASSERT_EQUAL_STRING("1xx0", text_of(&out));
    cond = cv("0z1", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_mux(arena, &args, &out));
    TEST_ASSERT_EQUAL_STRING("1100", text_of(&out));
    cond = cv("000", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_mux(arena, &args, &out));
    TEST_ASSERT_EQUAL_STRING("1010", text_of(&out));
    cond = cv("z", 0);
    lhs = cv("zx10", ODIN3_CVAL_SIGNED);
    rhs = cv("zz10", ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_mux(arena, &args, &out));
    TEST_ASSERT_EQUAL_STRING("zx10", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, out.flags);
}

static void test_resize(void) {
    odin3_cval wide = lit("", "'h", "FFFF_FFFF");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_UNSIZED, wide.flags);
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, &wide, 64, false, &out));
    TEST_ASSERT_EQUAL_STRING(repeat('1', 64), text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    odin3_cval sized = lit("32", "'h", "FFFF_FFFF");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, &sized, 64, false, &out));
    char want[65];
    memcpy(want, repeat('0', 32), 32);
    memcpy(want + 32, repeat('1', 32), 33);
    TEST_ASSERT_EQUAL_STRING(want, text_of(&out));
    odin3_cval five = lit("", "'sd", "5");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED, five.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, &five, 64, true, &out));
    int64_t got = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&out, &got));
    TEST_ASSERT_EQUAL_INT64(5, got);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, out.flags);
    /* sign extension repeats an x/z top bit; truncation keeps the low bits */
    odin3_cval xz = cv("z01", ODIN3_CVAL_SIGNED);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, &xz, 6, true, &out));
    TEST_ASSERT_EQUAL_STRING("zzzz01", text_of(&out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, &xz, 2, false, &out));
    TEST_ASSERT_EQUAL_STRING("01", text_of(&out));
}

static void test_strings(void) {
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("AB"), &out));
    TEST_ASSERT_EQUAL_STRING("0100000101000010", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_STRING, out.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr(""), &out));
    TEST_ASSERT_EQUAL_STRING("00000000", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_STRING, out.flags);
    TEST_ASSERT_EQUAL_STRING("\"\"", printed(&out));
}

static void test_print(void) {
    odin3_cval val = from_i64(8, 32, true);
    TEST_ASSERT_EQUAL_STRING("8", printed(&val));
    val = from_i64(-3, 32, true);
    TEST_ASSERT_EQUAL_STRING("-3", printed(&val));
    val = from_i64(8, 32, false);
    TEST_ASSERT_EQUAL_STRING("32'b00000000000000000000000000001000", printed(&val));
    val = cv("10x0", 0);
    TEST_ASSERT_EQUAL_STRING("4'b10x0", printed(&val));
    val = cv("11110000", ODIN3_CVAL_SIGNED);
    TEST_ASSERT_EQUAL_STRING("8'sb11110000", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("a\"b"), &val));
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\"", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("c\\"), &val));
    TEST_ASSERT_EQUAL_STRING("\"c\\\\\"", printed(&val));
    val = lit("", "'sd", "5");
    TEST_ASSERT_EQUAL_STRING("5", printed(&val));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("W=")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_print(&val, &buf));
    TEST_ASSERT_EQUAL_STRING("W=5", buf.data);
    odin3_strbuf_free(&buf);
}

/* A NUMBER node holding num, with the given SIGNED flag. */
static odin3_ast_id number_node(const odin3_ast_number *num, bool is_signed) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .loc = at.v};
    spec.flags = is_signed ? ODIN3_AST_F_NUMBER_SIGNED : 0;
    spec.sub = ODIN3_AST_NUMBER_BIN;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, at, num, &spec.payload));
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &id));
    return id;
}

static void assert_number_round_trip(const odin3_cval *val) {
    odin3_ast_number num = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_to_number(arena, val, &num));
    bool unsized = (val->flags & ODIN3_CVAL_UNSIZED) != 0;
    TEST_ASSERT_EQUAL_UINT32(unsized ? 0 : val->width, num.width);
    TEST_ASSERT_EQUAL_UINT32(val->width, num.nbits);
    TEST_ASSERT_EQUAL(!odin3_cval_is_known(val), num.has_xz);
    odin3_ast_id id = number_node(&num, (val->flags & ODIN3_CVAL_SIGNED) != 0);
    odin3_ast_literal back = {{0}, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast, id, arena, &back));
    TEST_ASSERT_EQUAL(unsized, back.is_unsized);
    odin3_cval again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(arena, &back, &again));
    TEST_ASSERT_TRUE(odin3_cval_equal(val, &again));
}

static void test_to_number(void) {
    odin3_cval val = lit("", "'h", "FFFF_FFFF");
    assert_number_round_trip(&val);
    val = lit("", "'sd", "5");
    assert_number_round_trip(&val);
    val = lit("", "'sh", "8000_0000");
    TEST_ASSERT_EQUAL_UINT32(33, val.width);
    assert_number_round_trip(&val);
    val = lit("", "'b", "x1");
    assert_number_round_trip(&val);
    val = cv("10x0z", ODIN3_CVAL_SIGNED);
    assert_number_round_trip(&val);
    val = from_i64(-7, 70, true);
    assert_number_round_trip(&val);
}

/* lhs and rhs differ: not equal, and (for these cases) different hashes. */
static void assert_distinct(const odin3_cval *lhs, const odin3_cval *rhs) {
    TEST_ASSERT_FALSE(odin3_cval_equal(lhs, rhs));
    TEST_ASSERT_NOT_EQUAL_UINT64(odin3_cval_hash(lhs, ODIN3_HASH_SEED),
                                 odin3_cval_hash(rhs, ODIN3_HASH_SEED));
}

static void test_hash(void) {
    odin3_cval lhs = cv("10x1", ODIN3_CVAL_SIGNED);
    odin3_cval rhs = cv("10x1", ODIN3_CVAL_SIGNED);
    TEST_ASSERT_TRUE(odin3_cval_equal(&lhs, &rhs));
    TEST_ASSERT_EQUAL_UINT64(odin3_cval_hash(&lhs, ODIN3_HASH_SEED),
                             odin3_cval_hash(&rhs, ODIN3_HASH_SEED));
    rhs.flags = 0;
    assert_distinct(&lhs, &rhs);
    rhs = cv("10z1", ODIN3_CVAL_SIGNED);
    assert_distinct(&lhs, &rhs);
    rhs = cv("010x1", ODIN3_CVAL_SIGNED);
    assert_distinct(&lhs, &rhs);
}

/* --- wide values ---------------------------------------------------------------------------- */

/* A 128-bit value from four 32-bit digits, most significant first. */
static odin3_cval digits128(const uint32_t digit[4], bool is_signed) {
    odin3_cval val = from_i64(0, 128, is_signed);
    val.aval[1] = (uint64_t)digit[0] << 32 | digit[1];
    val.aval[0] = (uint64_t)digit[2] << 32 | digit[3];
    return val;
}

static void assert_digits128(const uint32_t want[4], const odin3_cval *val) {
    TEST_ASSERT_EQUAL_UINT32(128, val->width);
    TEST_ASSERT_TRUE(odin3_cval_is_known(val));
    TEST_ASSERT_EQUAL_HEX64((uint64_t)want[0] << 32 | want[1], val->aval[1]);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)want[2] << 32 | want[3], val->aval[0]);
}

static void test_wide_division(void) {
    /* Knuth D's add-back step (Hacker's Delight divmnu's test vector) */
    static const uint32_t dividend[4] = {0x7fffffff, 0x80000000, 0, 0};
    static const uint32_t divisor[4] = {0, 0x80000000, 0, 1};
    static const uint32_t quotient[4] = {0, 0, 0, 0xfffffffe};
    static const uint32_t remainder[4] = {0, 0x7fffffff, 0xffffffff, 2};
    odin3_cval lhs = digits128(dividend, false);
    odin3_cval rhs = digits128(divisor, false);
    odin3_cop_args args = {ODIN3_COP_DIV, &lhs, &rhs, 128, false};
    odin3_cval out = op_at(&args);
    assert_digits128(quotient, &out);
    args.op = ODIN3_COP_MOD;
    out = op_at(&args);
    assert_digits128(remainder, &out);
    /* one-digit divisor; a dividend below the divisor; signed at 128 bits */
    static const uint32_t big[4] = {0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321};
    static const uint32_t seven[4] = {0, 0, 0, 7};
    static const uint32_t big_q[4] = {0x0299c335, 0xccf668fd, 0xb921f885, 0xeec552bb};
    static const uint32_t big_r[4] = {0, 0, 0, 4};
    static const uint32_t zero[4] = {0, 0, 0, 0};
    lhs = digits128(big, false);
    rhs = digits128(seven, false);
    args = (odin3_cop_args){ODIN3_COP_DIV, &lhs, &rhs, 128, false};
    out = op_at(&args);
    assert_digits128(big_q, &out);
    args.op = ODIN3_COP_MOD;
    out = op_at(&args);
    assert_digits128(big_r, &out);
    args = (odin3_cop_args){ODIN3_COP_DIV, &rhs, &lhs, 128, false};
    out = op_at(&args);
    assert_digits128(zero, &out);
    args.op = ODIN3_COP_MOD;
    out = op_at(&args);
    assert_digits128(seven, &out);
}

static void test_wide_signed(void) {
    /* -(2^100 + 4) / 3 and % 3 at 128 bits: q * 3 = dividend + 2, remainder -2 */
    odin3_cval lhs = from_i64(4, 128, true);
    lhs.aval[1] |= (uint64_t)1 << 36;
    odin3_cop_args args = {ODIN3_COP_NEG, &lhs, NULL, 128, true};
    odin3_cval neg = op_at(&args);
    odin3_cval three = from_i64(3, 128, true);
    args = (odin3_cop_args){ODIN3_COP_DIV, &neg, &three, 128, true};
    odin3_cval out = op_at(&args);
    args = (odin3_cop_args){ODIN3_COP_MUL, &out, &three, 128, true};
    odin3_cval back = op_at(&args);
    odin3_cval rem = from_i64(-2, 128, true);
    args = (odin3_cop_args){ODIN3_COP_SUB, &neg, &rem, 128, true};
    odin3_cval want = op_at(&args);
    TEST_ASSERT_TRUE(odin3_cval_equal(&want, &back));
    args = (odin3_cop_args){ODIN3_COP_MOD, &neg, &three, 128, true};
    out = op_at(&args);
    TEST_ASSERT_TRUE(odin3_cval_equal(&rem, &out));
    /* 2 ** 200 is 0 at 200 bits and 2^200 at 201 */
    odin3_cval exp = from_i64(200, 200, false);
    odin3_cval base = from_i64(2, 200, false);
    args = (odin3_cop_args){ODIN3_COP_POW, &base, &exp, 200, false};
    out = op_at(&args);
    TEST_ASSERT_EQUAL_STRING(repeat('0', 200), text_of(&out));
    args.width = 201;
    out = op_at(&args);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_cval_bit(&out, 200));
}

/* --- parts and inspection ------------------------------------------------------------------- */

static void test_parts(void) {
    odin3_cval val = cv("1x0z", ODIN3_CVAL_SIGNED);
    odin3_cval out = {0};
    odin3_cval_part part = {&val, -2, 8};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_select(arena, &part, &out));
    TEST_ASSERT_EQUAL_STRING("xx1x0zxx", text_of(&out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    part = (odin3_cval_part){&val, 1, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_select(arena, &part, &out));
    TEST_ASSERT_EQUAL_STRING("x0", text_of(&out));
    part = (odin3_cval_part){&val, INT64_MIN, 3};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_select(arena, &part, &out));
    TEST_ASSERT_EQUAL_STRING("xxx", text_of(&out));
    odin3_cval parts[3] = {cv("1", 0), cv("0x", ODIN3_CVAL_SIGNED), from_i64(-1, 70, true)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_concat(arena, parts, 3, &out));
    TEST_ASSERT_EQUAL_UINT32(73, out.width);
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    part = (odin3_cval_part){&out, 68, 5};
    odin3_cval top = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_select(arena, &part, &top));
    TEST_ASSERT_EQUAL_STRING("10x11", text_of(&top));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_replicate(arena, &parts[1], 40, &out));
    TEST_ASSERT_EQUAL_UINT32(80, out.width);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_cval_bit(&out, 64));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_cval_bit(&out, 79));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_cval_bit(&out, 80));
}

static void test_inspection(void) {
    odin3_cval val = cv("0z00", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_cval_truth(&val));
    TEST_ASSERT_FALSE(odin3_cval_is_known(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_Z, odin3_cval_bit(&val, 2));
    val = cv("0z10", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_cval_truth(&val));
    val = cv("0000", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_cval_truth(&val));
    TEST_ASSERT_TRUE(odin3_cval_is_known(&val));
}

static void test_to_i64(void) {
    int64_t got = 7;
    odin3_cval val = cv("1x", 0);
    TEST_ASSERT_FALSE(odin3_cval_to_i64(&val, &got));
    TEST_ASSERT_EQUAL_INT64(7, got);
    val = cv("1110", ODIN3_CVAL_SIGNED);
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &got));
    TEST_ASSERT_EQUAL_INT64(-2, got);
    val = from_i64(INT64_MIN, 100, true);
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &got));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, got);
}

static void test_to_i64_range(void) {
    int64_t got = 7;
    odin3_cval val = from_i64(-1, 64, false);
    TEST_ASSERT_FALSE(odin3_cval_to_i64(&val, &got));
    val.aval[0] = (uint64_t)INT64_MAX;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &got));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, got);
    val = from_i64(INT64_MIN, 65, true);
    val.aval[1] = 0; /* +2^63 at 65 bits */
    TEST_ASSERT_FALSE(odin3_cval_to_i64(&val, &got));
}

static void test_conversions(void) {
    odin3_cval val = cv("z1x0", ODIN3_CVAL_SIGNED | ODIN3_CVAL_STRING);
    odin3_cval copy = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_copy(arena, &val, &copy));
    TEST_ASSERT_TRUE(odin3_cval_equal(&val, &copy));
    TEST_ASSERT_TRUE(copy.aval != val.aval);
    odin3_value ir = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_to_value(arena, &val, &ir));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, ir.kind);
    TEST_ASSERT_EQUAL_UINT32(4, ir.len);
    static const uint8_t want[4] = {ODIN3_BIT_0, ODIN3_BIT_X, ODIN3_BIT_1, ODIN3_BIT_Z};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, ir.bits, 4);
    val = from_i64(-2, 66, false);
    char want_ones[67];
    memcpy(want_ones, repeat('1', 65), 65);
    memcpy(want_ones + 65, "0", 2);
    TEST_ASSERT_EQUAL_STRING(want_ones, text_of(&val));
    TEST_ASSERT_EQUAL_UINT8(0, val.flags);
    val = from_i64(6, 2, true);
    TEST_ASSERT_EQUAL_STRING("10", text_of(&val));
    odin3_cval fill = {0};
    const odin3_cval_spec spec = {70, ODIN3_CVAL_UNSIZED, ODIN3_BIT_Z};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_new(arena, &spec, &fill));
    TEST_ASSERT_EQUAL_STRING(repeat('z', 70), text_of(&fill));
    TEST_ASSERT_EQUAL_HEX64(0x3f, fill.bval[1]); /* nothing above the width */
    TEST_ASSERT_EQUAL_HEX64(0, fill.aval[1]);
}

/* --- caps, misuse and OOM ------------------------------------------------------------------- */

static void test_width_cap(void) {
    odin3_cval out = {0};
    odin3_cval_spec spec = {ODIN3_ELAB_MAX_WIDTH + 1, 0, ODIN3_BIT_0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_new(arena, &spec, &out));
    spec.width = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_new(arena, &spec, &out));
    spec.width = ODIN3_ELAB_MAX_WIDTH;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_new(arena, &spec, &out));
    odin3_cval two = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_replicate(arena, &out, 2, &two));
    odin3_cop_args args = {ODIN3_COP_NOT, &out, NULL, ODIN3_ELAB_MAX_WIDTH + 1, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_op(arena, &args, &two));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
    /* a lowered cap applies at once; NULL restores the defaults */
    odin3_elab_limits limits = *odin3_elab_limits_get();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_WIDTH, limits.max_width);
    limits.max_width = 16;
    odin3_elab_test_set_limits(&limits);
    spec.width = 17;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_new(arena, &spec, &out));
    odin3_elab_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_new(arena, &spec, &out));
}

static void test_limits_hook(void) {
    const odin3_elab_limits *now = odin3_elab_limits_get();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_ERRORS, now->max_errors);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_CELL_PINS, now->max_cell_pins);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_READMEM_BYTES, now->max_readmem_bytes);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_ELAB_MAX_MEMORY_BITS, now->max_memory_bits);
    TEST_ASSERT_EQUAL_UINT64(odin3_elab_default_max_bytes(), now->max_bytes);
    TEST_ASSERT_TRUE(now->max_bytes > 0);
    odin3_elab_limits wish = *now;
    wish.max_errors = 3;
    wish.max_eval_steps = UINT32_MAX; /* above the default: clamped */
    wish.max_bytes = UINT64_MAX;
    odin3_elab_test_set_limits(&wish);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_elab_limits_get()->max_errors);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_EVAL_STEPS, odin3_elab_limits_get()->max_eval_steps);
    TEST_ASSERT_EQUAL_UINT64(odin3_elab_default_max_bytes(), odin3_elab_limits_get()->max_bytes);
    odin3_elab_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ELAB_MAX_ERRORS, odin3_elab_limits_get()->max_errors);
}

static void test_misuse(void) {
    odin3_cval val = cv("10", 0);
    odin3_cval four = cv("1010", 0);
    odin3_cval out = {.width = 77};
    odin3_cop_args args = {ODIN3_COP_ADD, &val, NULL, 2, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_op(arena, &args, &out));
    args = (odin3_cop_args){ODIN3_COP_COUNT, &val, &val, 2, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_op(arena, &args, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_op(NULL, &args, &out));
    odin3_cval_mux_args mux = {&val, &val, &four};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_mux(arena, &mux, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_concat(arena, &val, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_replicate(arena, &val, 0, &out));
    odin3_cval_part part = {&val, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_select(arena, &part, &out));
    static const uint8_t bad[2] = {ODIN3_BIT_0, 7};
    odin3_ast_literal lit_bad = {{.kind = ODIN3_VAL_BITS, .bits = bad, .len = 2}, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_from_literal(arena, &lit_bad, &out));
    lit_bad.value.kind = ODIN3_VAL_INT;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_from_literal(arena, &lit_bad, &out));
    const odin3_cval_spec spec = {4, 8, ODIN3_BIT_0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_new(arena, &spec, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_print(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(77, out.width);
    TEST_ASSERT_EQUAL_size_t(11, errors_logged);
}

/* Every allocating entry point, each with a fresh arena of one-request chunks. */
typedef odin3_status (*alloc_step)(odin3_arena *small, odin3_cval *out);

static odin3_cval g_a;
static odin3_cval g_b;

static odin3_status step_new(odin3_arena *small, odin3_cval *out) {
    const odin3_cval_spec spec = {300, 0, ODIN3_BIT_X};
    return odin3_cval_new(small, &spec, out);
}
static odin3_status step_literal(odin3_arena *small, odin3_cval *out) {
    static const uint8_t bits[3] = {ODIN3_BIT_1, ODIN3_BIT_Z, ODIN3_BIT_0};
    const odin3_ast_literal value = {{.kind = ODIN3_VAL_BITS, .bits = bits, .len = 3}, true, true};
    return odin3_cval_from_literal(small, &value, out);
}
static odin3_status step_string(odin3_arena *small, odin3_cval *out) {
    return odin3_cval_from_string(small, odin3_bytes_cstr("abc"), out);
}
static odin3_status step_i64(odin3_arena *small, odin3_cval *out) {
    const odin3_cval_int num = {-9, 130, true};
    return odin3_cval_from_i64(small, &num, out);
}
static odin3_status step_copy(odin3_arena *small, odin3_cval *out) {
    return odin3_cval_copy(small, &g_a, out);
}
static odin3_status step_resize(odin3_arena *small, odin3_cval *out) {
    return odin3_cval_resize(small, &g_a, 260, true, out);
}
static odin3_status step_select(odin3_arena *small, odin3_cval *out) {
    const odin3_cval_part part = {&g_a, -3, 100};
    return odin3_cval_select(small, &part, out);
}
static odin3_status step_concat(odin3_arena *small, odin3_cval *out) {
    const odin3_cval parts[2] = {g_a, g_b};
    return odin3_cval_concat(small, parts, 2, out);
}
static odin3_status step_replicate(odin3_arena *small, odin3_cval *out) {
    return odin3_cval_replicate(small, &g_b, 3, out);
}
static odin3_status step_mux(odin3_arena *small, odin3_cval *out) {
    const odin3_cval cond = g_b;
    const odin3_cval_mux_args args = {&cond, &g_a, &g_a};
    return odin3_cval_mux(small, &args, out);
}

static odin3_cop g_op;

static odin3_status step_op(odin3_arena *small, odin3_cval *out) {
    const odin3_cop_args args = {g_op, &g_a, &g_b, 130, true};
    return odin3_cval_op(small, &args, out);
}

/* The other two conversions write other types: run them, then copy a value so out is set. */
static odin3_status step_to_value(odin3_arena *small, odin3_cval *out) {
    odin3_value value = {0};
    odin3_status st = odin3_cval_to_value(small, &g_a, &value);
    return st == ODIN3_OK ? odin3_cval_copy(small, &g_b, out) : st;
}
static odin3_status step_to_number(odin3_arena *small, odin3_cval *out) {
    odin3_ast_number num = {0};
    odin3_status st = odin3_cval_to_number(small, &g_a, &num);
    return st == ODIN3_OK ? odin3_cval_copy(small, &g_b, out) : st;
}

/* Fails allocation k = 0, 1, … until the step succeeds; each failure leaves *out unchanged. */
/* The step with allocation `fail_at` failing: NO_MEMORY and *out unchanged, or OK (true). */
static bool oom_step(alloc_step step, long fail_at) {
    odin3_arena *small = odin3_arena_create(1);
    TEST_ASSERT_NOT_NULL(small);
    odin3_cval out = {.width = 77};
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = step(small, &out);
    odin3_util_set_alloc_fail_after(-1);
    odin3_arena_destroy(small);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(77, out.width);
        TEST_ASSERT_NULL(out.aval);
    }
    return st == ODIN3_OK;
}

/* Fails allocation k = 0, 1, … until the step succeeds (it must allocate at least once). */
static void oom_sweep(alloc_step step) {
    TEST_ASSERT_FALSE(oom_step(step, 0));
    for (long k = 1; k < OOM_LIMIT; k++) {
        if (oom_step(step, k)) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT allocations");
}

static void test_oom(void) {
    g_a = cv("1x0z1111000010101", ODIN3_CVAL_SIGNED);
    g_b = cv("0110000000000000000000000000000000000000000000000000000000000000000011",
             ODIN3_CVAL_SIGNED);
    static const alloc_step steps[] = {step_new,       step_literal, step_string,   step_i64,
                                       step_copy,      step_resize,  step_select,   step_concat,
                                       step_replicate, step_mux,     step_to_value, step_to_number};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        oom_sweep(steps[i]);
    }
}

/* Every operator over one pair of (signed) operands, each allocation failing in turn. */
static void sweep_ops(const char *lhs, const char *rhs) {
    g_a = cv(lhs, ODIN3_CVAL_SIGNED);
    g_b = cv(rhs, ODIN3_CVAL_SIGNED);
    for (int op = 0; op < ODIN3_COP_COUNT; op++) {
        g_op = (odin3_cop)op;
        oom_sweep(step_op);
    }
}

static void test_oom_operators(void) {
    sweep_ops("0110000000000000000000000000000000000000000000000000000000000000000111",
              "0110000000000000000000000000000000000000000000000000000000000000000011");
    sweep_ops("01x0", "0011"); /* the all-x results */
    sweep_ops("0111", "0000"); /* division by zero */
    sweep_ops("1111", "1101"); /* (-1) ** negative */
    sweep_ops("0001", "1101"); /* 1 ** negative */
    sweep_ops("0110", "1101"); /* 6 ** negative */
    sweep_ops("1000000000000000000000000000000000000000000000000000000000000000000000", "11");
}

/* print with allocation `fail_at` failing: NO_MEMORY with the buffer as it was, or OK (true). */
static bool print_oom_step(const odin3_cval *val, long fail_at) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("W=")));
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_cval_print(val, &buf);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_size_t(2, buf.len);
        TEST_ASSERT_EQUAL_STRING("W=", buf.data);
    }
    odin3_strbuf_free(&buf);
    return st == ODIN3_OK;
}

static void test_oom_print(void) {
    odin3_cval val = cv(repeat('1', 300), 0);
    TEST_ASSERT_FALSE(print_oom_step(&val, 0));
    for (long k = 1; k < OOM_LIMIT; k++) {
        if (print_oom_step(&val, k)) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT allocations");
}

/* Strings print as text only when every byte is printable (distinct values, safe names). */
static void test_print_strings(void) {
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, (odin3_bytes){"a\0b", 3}, &val));
    TEST_ASSERT_EQUAL_STRING("24'b011000010000000001100010", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("a\nb"), &val));
    TEST_ASSERT_EQUAL_STRING("24'b011000010000101001100010", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("ab"), &val));
    TEST_ASSERT_EQUAL_STRING("\"ab\"", printed(&val));
    val = cv("0110000101100010", ODIN3_CVAL_STRING | ODIN3_CVAL_SIGNED);
    TEST_ASSERT_EQUAL_STRING("\"ab\"", printed(&val));
    val = cv("0110000x01100010", ODIN3_CVAL_STRING);
    TEST_ASSERT_EQUAL_STRING("16'b0110000x01100010", printed(&val));
}

/* --- EL-16's parameter conversion and string bytes (final review, finding 2) ---------------- */

static odin3_cval to_param(const odin3_cval *val, uint8_t mode, uint32_t width, bool is_signed) {
    odin3_cval out = {0};
    const odin3_cparam_type decl = {mode, width, is_signed};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_to_param(arena, val, &decl, &out));
    return out;
}

/* The value as an operand of a 64-bit unsigned context (an UNSIZED value repeats its top bit). */
static const char *in_context64(const odin3_cval *val) {
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_resize(arena, val, 64, false, &out));
    return text_of(&out);
}

/* Review Focus 2 (plan, stage 1): `localparam P = 'hFFFFFFFF; localparam [31:0] Q = …;
 * localparam [35:0] R = …;` into 64 bits: P and Q all ones, R zero-extended. */
static void test_to_param_pqr(void) {
    odin3_cval ones = lit("", "'h", "FFFFFFFF");
    TEST_ASSERT_EQUAL_UINT32(32, ones.width);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_UNSIZED, ones.flags);
    odin3_cval par = to_param(&ones, ODIN3_CPARAM_UNTYPED, 0, false); /* P */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_UNSIZED, par.flags);
    TEST_ASSERT_EQUAL_STRING(repeat('1', 64), in_context64(&par));
    par = to_param(&ones, ODIN3_CPARAM_SIZED, 32, false); /* Q: equal width keeps UNSIZED */
    TEST_ASSERT_EQUAL_UINT32(32, par.width);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_UNSIZED, par.flags);
    TEST_ASSERT_EQUAL_STRING(repeat('1', 64), in_context64(&par));
    par = to_param(&ones, ODIN3_CPARAM_SIZED, 36, false); /* R: extended by its own sign */
    TEST_ASSERT_EQUAL_UINT32(36, par.width);
    TEST_ASSERT_EQUAL_UINT8(0, par.flags);
    char want[65];
    memset(want, '0', 32);
    memset(want + 32, '1', 32);
    want[64] = '\0';
    TEST_ASSERT_EQUAL_STRING(want, in_context64(&par));
    par = to_param(&ones, ODIN3_CPARAM_SIZED, 32, true); /* integer: re-signed, still unsized */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED, par.flags);
    TEST_ASSERT_EQUAL_STRING(repeat('1', 64), in_context64(&par));
}

static void test_to_param_modes(void) {
    odin3_cval val = cv("1010", 0);
    odin3_cval par = to_param(&val, ODIN3_CPARAM_SIGNED, 0, false); /* `signed`: width kept */
    TEST_ASSERT_EQUAL_STRING("1010", text_of(&par));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, par.flags);
    val = cv("1010", ODIN3_CVAL_SIGNED);
    par = to_param(&val, ODIN3_CPARAM_SIZED, 8, false); /* extended by the value's own sign */
    TEST_ASSERT_EQUAL_STRING("11111010", text_of(&par));
    TEST_ASSERT_EQUAL_UINT8(0, par.flags);
    par = to_param(&val, ODIN3_CPARAM_UNTYPED, 0, false);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, par.flags);
    val = cv("11110000", 0);
    par = to_param(&val, ODIN3_CPARAM_SIZED, 4, true); /* truncated */
    TEST_ASSERT_EQUAL_STRING("0000", text_of(&par));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, par.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("AB"), &val));
    par = to_param(&val, ODIN3_CPARAM_SIZED, 16, true); /* equal width: STRING kept */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_STRING | ODIN3_CVAL_SIGNED, par.flags);
    par = to_param(&val, ODIN3_CPARAM_SIZED, 24, false); /* another width: a plain number */
    TEST_ASSERT_EQUAL_STRING("000000000100000101000010", text_of(&par));
    TEST_ASSERT_EQUAL_UINT8(0, par.flags);
    odin3_cval empty = {0};
    par = to_param(&empty, ODIN3_CPARAM_SIZED, 8, false); /* {0{…}}: as it is */
    TEST_ASSERT_EQUAL_UINT32(0, par.width);
    val = cv("01", 0);
    const odin3_cparam_type sized = {ODIN3_CPARAM_SIZED, 4, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_to_param(arena, &val, &sized, &val)); /* aliased */
    TEST_ASSERT_EQUAL_STRING("0001", text_of(&val));
    const odin3_cparam_type bad_mode = {ODIN3_CPARAM_SIZED + 1, 4, false};
    const odin3_cparam_type bad_width = {ODIN3_CPARAM_SIZED, 0, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_to_param(arena, &val, &bad_mode, &par));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_cval_to_param(arena, &val, &bad_width, &par));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_to_param(arena, &val, NULL, &par));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
}

static void test_string_bytes(void) {
    odin3_cval val = {0};
    odin3_bytes text = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr("AB"), &val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_string_bytes(arena, &val, &text));
    TEST_ASSERT_EQUAL_size_t(2, text.len);
    TEST_ASSERT_EQUAL_STRING("AB", text.ptr);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, odin3_bytes_cstr(""), &val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_string_bytes(arena, &val, &text));
    TEST_ASSERT_EQUAL_size_t(0, text.len); /* 8'h00 is "" */
    TEST_ASSERT_EQUAL_STRING("", text.ptr);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, (odin3_bytes){"a\0b", 3}, &val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_string_bytes(arena, &val, &text));
    TEST_ASSERT_EQUAL_size_t(3, text.len); /* a NUL inside is kept */
    TEST_ASSERT_EQUAL_MEMORY("a\0b", text.ptr, 3);
    odin3_cval back = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_string(arena, text, &back));
    TEST_ASSERT_TRUE(odin3_cval_equal(&val, &back));
    val = cv("0110000x01100010", ODIN3_CVAL_STRING);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_string_bytes(arena, &val, &text));
    val = cv("011000010110", ODIN3_CVAL_STRING);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_string_bytes(arena, &val, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cval_string_bytes(arena, NULL, &text));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_const_table);
    RUN_TEST(test_const_table_mixed);
    RUN_TEST(test_context_sign);
    RUN_TEST(test_result_flags);
    RUN_TEST(test_division);
    RUN_TEST(test_power);
    RUN_TEST(test_bitwise);
    RUN_TEST(test_reductions);
    RUN_TEST(test_equality);
    RUN_TEST(test_mixed_widths);
    RUN_TEST(test_shifts);
    RUN_TEST(test_mux);
    RUN_TEST(test_resize);
    RUN_TEST(test_strings);
    RUN_TEST(test_print);
    RUN_TEST(test_print_strings);
    RUN_TEST(test_to_number);
    RUN_TEST(test_hash);
    RUN_TEST(test_wide_division);
    RUN_TEST(test_wide_signed);
    RUN_TEST(test_parts);
    RUN_TEST(test_inspection);
    RUN_TEST(test_to_i64);
    RUN_TEST(test_to_i64_range);
    RUN_TEST(test_conversions);
    RUN_TEST(test_width_cap);
    RUN_TEST(test_limits_hook);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom);
    RUN_TEST(test_oom_operators);
    RUN_TEST(test_oom_print);
    RUN_TEST(test_to_param_pqr);
    RUN_TEST(test_to_param_modes);
    RUN_TEST(test_string_bytes);
    return UNITY_END();
}
