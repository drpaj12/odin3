/*
 * test_ast_print.c — unit tests for the Verilog printer: literal expected text for hand-built
 * subtrees (operators, an ANSI module, declarations, statements, instances, functions, generate,
 * attributes and metacomments, numbers, names, opaque TEXT), the dangling-else wrap, the
 * depth-cap chain, absent slots and abandoned roots, misuse, OOM, and a seeded 10,000-literal
 * parse → value → print → parse property test.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/print.h"
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
#include <string.h>

enum {
    OOM_LIMIT = 400,
    PROPERTY_LITERALS = 10000,
    TEXT_MAX = 128,
    CAP_BITS = 13607, /* bits of the largest decimal literal (10^4096 - 1) */
    LONG_TEXT = 4200,
    PRECEDENCE_TREES = 500,
    POOL_MAX = 8,
    REF_MAX = 1024
};

static odin3_design *design;
static odin3_ast *ast;
static odin3_strbuf out;
static size_t errors_logged;
static size_t warnings_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
    warnings_logged += level == ODIN3_LOG_WARN;
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    warnings_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    odin3_strbuf_init(&out);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_strbuf_free(&out);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- builders ------------------------------------------------------------------------------ */

static const odin3_ast_id NO = {0};

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_spec kd(odin3_ast_kind kind) {
    return (odin3_ast_spec){.kind = (uint8_t)kind};
}

static odin3_ast_spec named(odin3_ast_kind kind, const char *name) {
    return (odin3_ast_spec){.kind = (uint8_t)kind, .name = intern(name)};
}

static odin3_ast_spec with_sub(odin3_ast_spec spec, uint32_t sub) {
    spec.sub = (uint8_t)sub;
    return spec;
}

static odin3_ast_spec with_flags(odin3_ast_spec spec, uint32_t flags) {
    spec.flags = (uint16_t)flags;
    return spec;
}

static odin3_ast_id mk(odin3_ast_spec spec, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, ids, n, &id));
    return id;
}

static odin3_ast_id leaf(odin3_ast_spec spec) {
    return mk(spec, NULL, 0);
}

static odin3_ast_id id(const char *name) {
    return leaf(named(ODIN3_AST_IDENT, name));
}

/* A literal from its source text: `4'hff`, `'sb1`, `5`. */
static odin3_ast_id num(const char *text) {
    const char *tick = strchr(text, '\'');
    odin3_number_text pieces = {{(const uint8_t *)text, 0}, {NULL, 0}, odin3_bytes_cstr(text)};
    if (tick != NULL) {
        size_t base_len = (tick[1] == 's' || tick[1] == 'S') ? 3 : 2;
        pieces.size.len = (size_t)(tick - text);
        pieces.base = (odin3_bytes){(const uint8_t *)tick, base_len};
        pieces.digits = odin3_bytes_cstr(tick + base_len);
    }
    odin3_ast_spec spec = kd(ODIN3_AST_NUMBER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_number_parse(ast, (odin3_loc){0}, pieces, &spec.payload));
    return leaf(spec);
}

static odin3_ast_id real(double value) {
    odin3_ast_spec spec = kd(ODIN3_AST_REAL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, value, &spec.payload));
    return leaf(spec);
}

static odin3_ast_id text_node(odin3_ast_spec spec, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_text_new(ast, odin3_bytes_cstr(text), &spec.payload));
    return leaf(spec);
}

static odin3_ast_id bin(odin3_ast_id lhs, uint32_t op, odin3_ast_id rhs) {
    return mk(with_sub(kd(ODIN3_AST_BINARY), op), (odin3_ast_id[]){lhs, rhs}, 2);
}

static odin3_ast_id un(uint32_t op, odin3_ast_id operand) {
    return mk(with_sub(kd(ODIN3_AST_UNARY), op), &operand, 1);
}

static odin3_ast_id range(const char *msb, const char *lsb) {
    return mk(kd(ODIN3_AST_RANGE), (odin3_ast_id[]){num(msb), num(lsb)}, 2);
}

static odin3_ast_id decl(const char *name) {
    return mk(named(ODIN3_AST_DECLARATOR, name), &NO, 1);
}

static odin3_ast_id decl_init(const char *name, odin3_ast_id init) {
    return mk(named(ODIN3_AST_DECLARATOR, name), &init, 1);
}

static odin3_ast_id list(const odin3_ast_id *ids, uint32_t n) {
    return mk(kd(ODIN3_AST_LIST), ids, n);
}

static odin3_ast_id attr(const char *key, odin3_ast_id value, uint32_t flags) {
    return mk(with_flags(named(ODIN3_AST_ATTR, key), flags), &value, 1);
}

static void attach(odin3_ast_id node, const odin3_ast_id *attrs, uint32_t n) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, node, attrs, n));
}

/* lhs = rhs as a BLOCKING_ASSIGN (or NONBLOCKING when nonblocking). */
static odin3_ast_id assign(odin3_ast_id lhs, odin3_ast_id rhs, bool nonblocking) {
    odin3_ast_kind kind = nonblocking ? ODIN3_AST_NONBLOCKING_ASSIGN : ODIN3_AST_BLOCKING_ASSIGN;
    return mk(kd(kind), (odin3_ast_id[]){lhs, NO, rhs}, 3);
}

static odin3_ast_id cont_assign(odin3_ast_id lhs, odin3_ast_id rhs) {
    odin3_ast_id net = mk(kd(ODIN3_AST_NET_ASSIGN), (odin3_ast_id[]){lhs, rhs}, 2);
    return mk(kd(ODIN3_AST_CONT_ASSIGN), (odin3_ast_id[]){NO, NO, net}, 3);
}

static odin3_ast_id block(const odin3_ast_id *stmts, uint32_t n) {
    odin3_ast_id ids[8] = {NO};
    TEST_ASSERT_LESS_THAN_UINT32(8, n);
    for (uint32_t i = 0; i < n; i++) {
        ids[i + 1] = stmts[i];
    }
    return mk(kd(ODIN3_AST_SEQ_BLOCK), ids, n + 1);
}

static odin3_ast_id event(uint32_t edge, const char *signal, bool after_or) {
    odin3_ast_spec spec = with_sub(kd(ODIN3_AST_EVENT_EXPR), edge);
    spec.flags = after_or ? (uint16_t)ODIN3_AST_F_EVENT_EXPR_AFTER_OR : 0;
    odin3_ast_id sig = id(signal);
    return mk(spec, &sig, 1);
}

/* --- expectations -------------------------------------------------------------------------- */

static void expect(odin3_ast_id root, const char *want) {
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, root, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data != NULL ? out.data : "");
}

/* --- operators and continuous assignments --------------------------------------------------- */

static void test_cont_assign_parenthesises_operands(void) {
    odin3_ast_id rhs =
        bin(id("a"), ODIN3_AST_BINARY_ADD, bin(id("b"), ODIN3_AST_BINARY_MUL, id("c")));
    expect(cont_assign(id("y"), rhs), "assign y = a + (b * c);\n");
    expect(rhs, "a + (b * c)");
    odin3_ast_id left = bin(bin(id("a"), ODIN3_AST_BINARY_SUB, id("b")), ODIN3_AST_BINARY_SHL,
                            un(ODIN3_AST_UNARY_NOT, id("c")));
    expect(left, "(a - b) << (~c)");
    expect(un(ODIN3_AST_UNARY_MINUS, un(ODIN3_AST_UNARY_MINUS, id("a"))), "-(-a)");
    odin3_ast_id cond = mk(kd(ODIN3_AST_TERNARY), (odin3_ast_id[]){id("s"), left, id("d")}, 3);
    expect(bin(cond, ODIN3_AST_BINARY_LAND, id("e")), "(s ? ((a - b) << (~c)) : d) && e");
    expect(un(ODIN3_AST_UNARY_XNOR, id("v")), "~^v");
}

static odin3_ast_id ansi_module(void) {
    odin3_ast_spec in = with_flags(with_sub(kd(ODIN3_AST_PORT_DECL), ODIN3_AST_PORT_DECL_INPUT),
                                   ODIN3_AST_F_PORT_DECL_IN_HEADER);
    odin3_ast_id port_a = mk(in, (odin3_ast_id[]){range("3", "0"), decl("a")}, 2);
    odin3_ast_spec outp = with_sub(in, ODIN3_AST_PORT_DECL_OUTPUT);
    odin3_ast_id port_y = mk(outp, (odin3_ast_id[]){NO, decl("y")}, 2);
    odin3_ast_id ports = list((odin3_ast_id[]){port_a, port_y}, 2);
    odin3_ast_id body = cont_assign(id("y"), un(ODIN3_AST_UNARY_AND, id("a")));
    return mk(named(ODIN3_AST_MODULE, "m"), (odin3_ast_id[]){NO, ports, body}, 3);
}

static void test_ansi_module(void) {
    expect(ansi_module(), "module m (\n"
                          "  input [3:0] a,\n"
                          "  output y\n"
                          ");\n"
                          "  assign y = &a;\n"
                          "endmodule\n");
    odin3_ast_id empty = mk(named(ODIN3_AST_MODULE, "e"), (odin3_ast_id[]){NO, NO}, 2);
    expect(empty, "module e;\nendmodule\n");
    odin3_ast_id unit = mk(named(ODIN3_AST_UNIT, "top.v"), &empty, 1);
    expect(unit, "module e;\nendmodule\n");
}

/* --- attributes and metacomments ------------------------------------------------------------ */

static void test_case_metacomment(void) {
    odin3_ast_id null_stmt = leaf(kd(ODIN3_AST_NULL_STMT));
    odin3_ast_spec item = with_flags(kd(ODIN3_AST_CASE_ITEM), ODIN3_AST_F_CASE_ITEM_DEFAULT);
    odin3_ast_id dflt = mk(item, &null_stmt, 1); /* body first, printed last */
    odin3_ast_id cs = mk(kd(ODIN3_AST_CASE), (odin3_ast_id[]){id("s"), dflt}, 2);
    odin3_ast_id metas[2] = {attr("full_case", NO, ODIN3_AST_F_ATTR_METACOMMENT),
                             attr("parallel_case", NO, ODIN3_AST_F_ATTR_METACOMMENT)};
    attach(cs, metas, 2);
    expect(cs, "case (s) // synopsys full_case parallel_case\n"
               "  default: ;\n"
               "endcase\n");
    /* mixed with a (* *) attribute, on a casez */
    odin3_ast_id one =
        mk(kd(ODIN3_AST_CASE_ITEM),
           (odin3_ast_id[]){assign(id("y"), id("a"), false), num("2'd0"), num("1")}, 3);
    odin3_ast_id cz =
        mk(with_sub(kd(ODIN3_AST_CASE), ODIN3_AST_CASE_CASEZ), (odin3_ast_id[]){id("s"), one}, 2);
    attach(cz, (odin3_ast_id[]){attr("keep", NO, 0), attr("full_case", NO, 1)}, 2);
    expect(cz, "(* keep *) casez (s) // synopsys full_case\n"
               "  2'd0, 1: y = a;\n"
               "endcase\n");
}

static void test_attributes(void) {
    odin3_ast_id wire = mk(kd(ODIN3_AST_NET_DECL), (odin3_ast_id[]){NO, NO, NO, decl("w")}, 4);
    attach(wire, (odin3_ast_id[]){attr("keep", NO, 0)}, 1);
    expect(wire, "(* keep *) wire w;\n");
    odin3_ast_id reg = mk(kd(ODIN3_AST_VAR_DECL), (odin3_ast_id[]){NO, decl("r")}, 2);
    attach(reg, (odin3_ast_id[]){attr("keep", NO, 0), attr("max_fanout", num("4"), 0)}, 2);
    expect(reg, "(* keep, max_fanout = 4 *) reg r;\n");
    odin3_ast_id sum = bin(id("a"), ODIN3_AST_BINARY_ADD, id("b"));
    attach(sum, (odin3_ast_id[]){attr("keep", NO, 0)}, 1);
    expect(sum, "a + (* keep *) b");
    odin3_ast_id inv = un(ODIN3_AST_UNARY_NOT, id("a"));
    attach(inv, (odin3_ast_id[]){attr("keep", NO, 0)}, 1);
    expect(inv, "~(* keep *) a");
    odin3_ast_id mux = mk(kd(ODIN3_AST_TERNARY), (odin3_ast_id[]){id("c"), id("a"), id("b")}, 3);
    attach(mux, (odin3_ast_id[]){attr("s", leaf(named(ODIN3_AST_STRING, "v")), 0)}, 1);
    expect(mux, "c ? (* s = \"v\" *) a : b");
    odin3_ast_id mod = mk(named(ODIN3_AST_MODULE, "m"), (odin3_ast_id[]){NO, NO}, 2);
    attach(mod, (odin3_ast_id[]){attr("top", NO, ODIN3_AST_F_ATTR_METACOMMENT)}, 1);
    expect(mod, "(* top *) module m;\nendmodule\n"); /* a metacomment off a case: (* *) */
}

/* --- numbers, reals and strings ------------------------------------------------------------- */

static void expect_number(const char *text, const char *want) {
    expect(num(text), want);
}

static void test_numbers(void) {
    expect_number("4'hFF", "4'hff");
    expect_number("8'bx", "8'bx");
    expect_number("'hF", "'hf");
    expect_number("4'sb1000", "4'sb1000");
    expect_number("4'b1?0z", "4'b1?0?");
    expect_number("4'b1z0z", "4'b1z0z");
    expect_number("5", "5");
    expect_number("'sd5", "5"); /* the same literal as a plain decimal */
    expect_number("'d5", "'d5");
    expect_number("8'd0", "8'd0");
    expect_number("8'd2_55", "8'd255");
    expect_number("'dx", "'dx");
    expect_number("4'd?", "4'd?");
    expect_number("12'o7_x?", "12'o7x?");
    expect_number("'h0_0000_0001", "'h000000001");
    expect_number("16'hxZ", "16'hxz");
    expect_number("100'd1267650600228229401496703205376", "100'd1267650600228229401496703205376");
    expect_number("4294967296", "4294967296");
    expect(real(1.5), "1.5");
    expect(real(0.1), "0.10000000000000001");
    expect(real(1.0), "1.0");
    expect(real(1e20), "1e+20");
}

static void test_numbers_mixing_xz_print_in_binary(void) {
    const uint8_t hex_bits[8] = {ODIN3_BIT_X, ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_0,
                                 ODIN3_BIT_1, ODIN3_BIT_1, ODIN3_BIT_1, ODIN3_BIT_1};
    odin3_ast_number lit = {.width = 8, .nbits = 8, .bits = hex_bits, .has_xz = true};
    odin3_ast_spec spec = with_sub(kd(ODIN3_AST_NUMBER), ODIN3_AST_NUMBER_HEX);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, (odin3_loc){0}, &lit, &spec.payload));
    expect(leaf(spec), "8'b1111010x");
    spec.sub = ODIN3_AST_NUMBER_DEC;
    spec.flags = ODIN3_AST_F_NUMBER_SIGNED;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, (odin3_loc){0}, &lit, &spec.payload));
    expect(leaf(spec), "8'sb1111010x");
    const uint8_t odd_bits[5] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_0, ODIN3_BIT_0, ODIN3_BIT_1};
    odin3_ast_number odd = {.width = 0, .nbits = 5, .bits = odd_bits};
    spec = with_sub(kd(ODIN3_AST_NUMBER), ODIN3_AST_NUMBER_HEX); /* 5 bits: 0 padded */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, (odin3_loc){0}, &odd, &spec.payload));
    expect(leaf(spec), "'h11");
}

static void test_strings(void) {
    expect(leaf(named(ODIN3_AST_STRING, "x=%d")), "\"x=%d\"");
    expect(leaf(named(ODIN3_AST_STRING, "a\"b\\c\n\td\001\177\xe9")),
           "\"a\\\"b\\\\c\\n\\td\\001\\177\\351\"");
}

/* A literal that parsed as a decimal prints as one, up to the 4096-digit cap (Yosys reads it). */
static void expect_round_trip(odin3_ast_id first, const char *want) {
    static char again[LONG_TEXT];
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, first, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data);
    TEST_ASSERT_LESS_THAN_size_t(sizeof again, out.len);
    memcpy(again, out.data, out.len + 1);
    bool same = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_equal(ast, first, ast, num(again), &same));
    TEST_ASSERT_TRUE(same);
}

/* A plain decimal 2^power from its bits (nbits = power + 1, as number_parse stores it). */
static odin3_ast_id power_of_two(uint32_t power, uint8_t *bits) {
    memset(bits, ODIN3_BIT_0, power + 1);
    bits[power] = ODIN3_BIT_1;
    odin3_ast_number lit = {.width = 0, .nbits = power + 1, .bits = bits};
    odin3_ast_spec spec = with_flags(kd(ODIN3_AST_NUMBER), ODIN3_AST_F_NUMBER_SIGNED);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(ast, (odin3_loc){0}, &lit, &spec.payload));
    return leaf(spec);
}

static void test_decimal_digit_cap(void) {
    static char nines[ODIN3_AST_MAX_DECIMAL_DIGITS + 1];
    static uint8_t bits[CAP_BITS + 1];
    memset(nines, '9', ODIN3_AST_MAX_DECIMAL_DIGITS);
    nines[ODIN3_AST_MAX_DECIMAL_DIGITS] = '\0';
    TEST_ASSERT_EQUAL_UINT32(CAP_BITS, odin3_ast_number_of(ast, num(nines))->nbits);
    expect_round_trip(num(nines), nines); /* 4096 digits, 13607 bits */
    static char sized[LONG_TEXT];
    (void)snprintf(sized, sizeof sized, "13607'd%s", nines);
    expect_round_trip(num(sized), sized);
    odin3_ast_id top = power_of_two(CAP_BITS - 1, bits); /* 2^13606: 4096 digits */
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, top, &out));
    TEST_ASSERT_EQUAL_size_t(ODIN3_AST_MAX_DECIMAL_DIGITS, out.len);
    static char copy[LONG_TEXT];
    memcpy(copy, out.data, out.len + 1);
    expect_round_trip(top, copy);
    odin3_ast_id over = power_of_two(CAP_BITS, bits); /* 2^13607: 4097 digits, no decimal */
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, over, &out));
    TEST_ASSERT_EQUAL_STRING_LEN("'sh8000", out.data, 7);
}

/* --- names (Review Focus 4) ----------------------------------------------------------------- */

static void test_names(void) {
    expect(id("a+b"), "\\a+b ");
    expect(id("wire"), "\\wire ");
    expect(id("g[3]"), "\\g[3] ");
    odin3_ast_id part =
        mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("g"), num("3"), NO}, 3); /* g + index 3 */
    odin3_ast_id hier = mk(kd(ODIN3_AST_HIER_NAME), (odin3_ast_id[]){part, id("x")}, 2);
    expect(hier, "g[3].x");
    odin3_ast_id gen = mk(named(ODIN3_AST_GEN_BLOCK, "g"), (odin3_ast_id[]){num("3")}, 1);
    expect(gen, "begin : g[3]\nend\n");
    odin3_ast_id esc = mk(named(ODIN3_AST_GEN_BLOCK, "g[3]"), &NO, 1);
    expect(esc, "begin : \\g[3] \nend\n");
    odin3_ast_spec sys = with_flags(named(ODIN3_AST_CALL, "$clog2"), ODIN3_AST_F_CALL_SYSTEM);
    expect(mk(sys, (odin3_ast_id[]){id("n")}, 1), "$clog2(n)");
    expect(leaf(with_flags(named(ODIN3_AST_CALL, "$time"), ODIN3_AST_F_CALL_SYSTEM)), "$time");
    odin3_ast_spec hcall = with_flags(named(ODIN3_AST_CALL, "u.f"), ODIN3_AST_F_CALL_HIER);
    expect(mk(hcall, (odin3_ast_id[]){id("a"), id("b")}, 2), "u.f(a, b)");
    expect(leaf(named(ODIN3_AST_CALL, "f")), "f()");
    expect(leaf(named(ODIN3_AST_CALL, "begin")), "\\begin ()");
    size_t warnings = warnings_logged;
    expect(id("a b"), "\\a b "); /* unwritable: as stored, with a warning */
    TEST_ASSERT_EQUAL_size_t(warnings + 1, warnings_logged);
}

/* --- opaque text and directives ------------------------------------------------------------- */

static void test_opaque_text_verbatim(void) {
    const char *spec_text = "specify\n    (a => y) = (1, 2);\n  endspecify";
    odin3_ast_id spec = text_node(kd(ODIN3_AST_SPECIFY_BLOCK), spec_text);
    expect(spec, "specify\n    (a => y) = (1, 2);\n  endspecify\n");
    odin3_ast_id mod = mk(named(ODIN3_AST_MODULE, "m"), (odin3_ast_id[]){NO, NO, spec}, 3);
    expect(mod, "module m;\n  specify\n    (a => y) = (1, 2);\n  endspecify\nendmodule\n");
    odin3_ast_id udp =
        text_node(named(ODIN3_AST_UDP_DECL, "u"), "primitive u(o, a);\nendprimitive");
    odin3_ast_id cfg = text_node(named(ODIN3_AST_CONFIG_DECL, "c"), "config c;\nendconfig");
    odin3_ast_spec ts =
        with_sub(named(ODIN3_AST_DIRECTIVE, "1ns/1ps"), ODIN3_AST_DIRECTIVE_TIMESCALE);
    odin3_ast_spec cd = with_sub(kd(ODIN3_AST_DIRECTIVE), ODIN3_AST_DIRECTIVE_CELLDEFINE);
    odin3_ast_id unit =
        mk(named(ODIN3_AST_UNIT, "t.v"), (odin3_ast_id[]){leaf(ts), leaf(cd), udp, cfg}, 4);
    expect(unit, "`timescale 1ns/1ps\n`celldefine\nprimitive u(o, a);\nendprimitive\n"
                 "config c;\nendconfig\n");
}

/* --- declarations --------------------------------------------------------------------------- */

/* A drive strength (s0, s1), or a charge strength when charge is non-zero. */
static odin3_ast_id strength(uint32_t s0, uint32_t s1, bool charge) {
    uint32_t flags = (s0 << ODIN3_AST_F_STRENGTH_S0_SHIFT) | (s1 << ODIN3_AST_F_STRENGTH_S1_SHIFT);
    uint32_t sub = charge ? ODIN3_AST_STRENGTH_CHARGE : ODIN3_AST_STRENGTH_DRIVE;
    return leaf(with_flags(with_sub(kd(ODIN3_AST_STRENGTH), sub), flags));
}

static odin3_ast_id delay(const odin3_ast_id *values, uint32_t n) {
    return mk(kd(ODIN3_AST_DELAY), values, n);
}

static void test_net_and_var_declarations(void) {
    odin3_ast_spec net = with_flags(kd(ODIN3_AST_NET_DECL),
                                    ODIN3_AST_F_NET_DECL_VECTORED | ODIN3_AST_F_NET_DECL_SIGNED);
    odin3_ast_id str = strength(ODIN3_AST_DRIVE_STRONG, ODIN3_AST_DRIVE_WEAK, false);
    odin3_ast_id del = delay((odin3_ast_id[]){num("1")}, 1);
    odin3_ast_id wire =
        mk(net, (odin3_ast_id[]){str, range("7", "0"), del, decl_init("w", id("a")), decl("v")}, 5);
    expect(wire, "wire (strong0, weak1) vectored signed [7:0] #(1) w = a, v;\n");
    odin3_ast_spec tri = with_sub(kd(ODIN3_AST_NET_DECL), ODIN3_AST_NET_DECL_TRIREG);
    odin3_ast_id charge = strength(ODIN3_AST_CHARGE_SMALL, 0, true);
    expect(mk(tri, (odin3_ast_id[]){charge, NO, NO, decl("t")}, 4), "trireg (small) t;\n");
    odin3_ast_id dims = mk(named(ODIN3_AST_DECLARATOR, "mem"),
                           (odin3_ast_id[]){NO, range("0", "3"), range("1", "0")}, 3);
    odin3_ast_spec reg = with_flags(kd(ODIN3_AST_VAR_DECL), ODIN3_AST_F_VAR_DECL_SIGNED);
    expect(mk(reg, (odin3_ast_id[]){range("7", "0"), dims}, 2),
           "reg signed [7:0] mem[0:3][1:0];\n");
    odin3_ast_spec integer = with_sub(kd(ODIN3_AST_VAR_DECL), ODIN3_AST_VAR_DECL_INTEGER);
    expect(mk(integer, (odin3_ast_id[]){NO, decl("i"), decl("j")}, 3), "integer i, j;\n");
    expect(mk(kd(ODIN3_AST_GENVAR_DECL), (odin3_ast_id[]){decl("k")}, 1), "genvar k;\n");
    expect(mk(kd(ODIN3_AST_EVENT_DECL), (odin3_ast_id[]){decl("e")}, 1), "event e;\n");
    odin3_ast_id pull = strength(0, ODIN3_AST_DRIVE_PULL, false);
    odin3_ast_spec sup = with_sub(kd(ODIN3_AST_NET_DECL), ODIN3_AST_NET_DECL_SUPPLY1);
    expect(mk(sup, (odin3_ast_id[]){pull, NO, NO, decl("vdd")}, 4), "supply1 (pull1) vdd;\n");
}

static void test_param_and_port_declarations(void) {
    uint32_t dt_int = (uint32_t)ODIN3_AST_DT_INTEGER << ODIN3_AST_F_PARAM_DECL_DT_SHIFT;
    odin3_ast_spec lp =
        with_flags(with_sub(kd(ODIN3_AST_PARAM_DECL), ODIN3_AST_PARAM_DECL_LOCALPARAM), dt_int);
    expect(mk(lp, (odin3_ast_id[]){NO, decl_init("L", num("3"))}, 2),
           "localparam integer L = 3;\n");
    odin3_ast_spec par = with_flags(kd(ODIN3_AST_PARAM_DECL), ODIN3_AST_F_PARAM_DECL_SIGNED);
    expect(mk(par, (odin3_ast_id[]){range("3", "0"), decl_init("P", num("4'sd5"))}, 2),
           "parameter signed [3:0] P = 4'sd5;\n");
    uint32_t net_wire = (1U + ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    odin3_ast_spec inout =
        with_flags(with_sub(kd(ODIN3_AST_PORT_DECL), ODIN3_AST_PORT_DECL_INOUT), net_wire);
    expect(mk(inout, (odin3_ast_id[]){NO, decl("x")}, 2), "inout wire x;\n");
    uint32_t dt_reg = (uint32_t)ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    odin3_ast_spec qs = with_flags(with_sub(kd(ODIN3_AST_PORT_DECL), ODIN3_AST_PORT_DECL_OUTPUT),
                                   dt_reg | ODIN3_AST_F_PORT_DECL_SIGNED);
    expect(mk(qs, (odin3_ast_id[]){range("3", "0"), decl("q"), decl("r")}, 3),
           "output reg signed [3:0] q, r;\n");
    odin3_ast_id part = mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("u0"), num("1"), NO}, 3);
    odin3_ast_id target = mk(kd(ODIN3_AST_HIER_NAME), (odin3_ast_id[]){part, id("P")}, 2);
    expect(mk(kd(ODIN3_AST_DEFPARAM), (odin3_ast_id[]){target, num("2")}, 2),
           "defparam u0[1].P = 2;\n");
}

static odin3_ast_id header_module(void) {
    odin3_ast_spec par = with_flags(kd(ODIN3_AST_PARAM_DECL), ODIN3_AST_F_PARAM_DECL_IN_HEADER);
    odin3_ast_id params =
        list((odin3_ast_id[]){mk(par, (odin3_ast_id[]){NO, decl_init("W", num("8"))}, 2),
                              mk(par, (odin3_ast_id[]){NO, decl_init("D", num("1"))}, 2)},
             2);
    odin3_ast_spec dotted = with_flags(named(ODIN3_AST_PORT_REF, "b"), ODIN3_AST_F_PORT_REF_DOTTED);
    odin3_ast_id cat = mk(kd(ODIN3_AST_CONCAT), (odin3_ast_id[]){id("d"), id("e")}, 2);
    odin3_ast_id ports =
        list((odin3_ast_id[]){mk(named(ODIN3_AST_PORT_REF, "a"), &NO, 1),
                              mk(dotted, (odin3_ast_id[]){id("c")}, 1),
                              mk(kd(ODIN3_AST_PORT_REF), &cat, 1), mk(dotted, &NO, 1)},
             4);
    odin3_ast_spec mod = with_flags(named(ODIN3_AST_MODULE, "m"), ODIN3_AST_F_MODULE_MACROMODULE);
    return mk(mod, (odin3_ast_id[]){params, ports, leaf(kd(ODIN3_AST_NULL_STMT))}, 3);
}

static void test_module_headers(void) {
    expect(header_module(), "macromodule m #(\n"
                            "  parameter W = 8,\n"
                            "  parameter D = 1\n"
                            ") (\n"
                            "  a,\n"
                            "  .b(c),\n"
                            "  {d, e},\n"
                            "  .b()\n"
                            ");\n"
                            "  ;\n"
                            "endmodule\n");
    odin3_ast_id empty = list(NULL, 0);
    odin3_ast_id mod = mk(named(ODIN3_AST_MODULE, "n"), (odin3_ast_id[]){empty, list(NULL, 0)}, 2);
    expect(mod, "module n #() ();\nendmodule\n");
}

/* --- statements ----------------------------------------------------------------------------- */

static odin3_ast_id posedge_block(void) {
    odin3_ast_id reset = assign(id("q"), num("'d0"), true);
    odin3_ast_id incr = assign(id("q"), bin(id("q"), ODIN3_AST_BINARY_ADD, num("1")), true);
    odin3_ast_id sel = mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("mem"), num("0"), NO}, 3);
    odin3_ast_id del = delay((odin3_ast_id[]){num("1")}, 1);
    odin3_ast_id store = mk(kd(ODIN3_AST_BLOCKING_ASSIGN), (odin3_ast_id[]){sel, del, id("d")}, 3);
    odin3_ast_id inner = mk(kd(ODIN3_AST_IF),
                            (odin3_ast_id[]){id("en"), block((odin3_ast_id[]){incr, store}, 2),
                                             assign(id("q"), id("q"), true)},
                            3);
    odin3_ast_id outer = mk(kd(ODIN3_AST_IF),
                            (odin3_ast_id[]){un(ODIN3_AST_UNARY_LNOT, id("rst")), reset, inner}, 3);
    odin3_ast_id body = mk(named(ODIN3_AST_SEQ_BLOCK, "blk"), (odin3_ast_id[]){NO, outer}, 2);
    odin3_ast_id ctl = mk(kd(ODIN3_AST_EVENT_CONTROL),
                          (odin3_ast_id[]){NO, event(ODIN3_AST_EVENT_EXPR_POSEDGE, "clk", false),
                                           event(ODIN3_AST_EVENT_EXPR_NEGEDGE, "rst", true)},
                          3);
    odin3_ast_id timed = mk(kd(ODIN3_AST_TIMING_STMT), (odin3_ast_id[]){ctl, body}, 2);
    return mk(kd(ODIN3_AST_ALWAYS), &timed, 1);
}

static void test_always_if_else(void) {
    expect(posedge_block(), "always @(posedge clk or negedge rst) begin : blk\n"
                            "  if (!rst) q <= 'd0;\n"
                            "  else if (en) begin\n"
                            "    q <= q + 1;\n"
                            "    mem[0] = #(1) d;\n"
                            "  end\n"
                            "  else q <= q;\n"
                            "end\n");
}

static odin3_ast_id comb_block(void) {
    odin3_ast_id item =
        mk(kd(ODIN3_AST_CASE_ITEM),
           (odin3_ast_id[]){assign(id("y"), id("a"), false), num("2'd0"), num("2'd1")}, 3);
    odin3_ast_spec dflt = with_flags(kd(ODIN3_AST_CASE_ITEM), ODIN3_AST_F_CASE_ITEM_DEFAULT);
    odin3_ast_id other = mk(dflt, (odin3_ast_id[]){block(NULL, 0)}, 1);
    odin3_ast_id cs = mk(kd(ODIN3_AST_CASE), (odin3_ast_id[]){id("s"), item, other}, 3);
    odin3_ast_id init = assign(id("i"), num("0"), false);
    odin3_ast_id step = assign(id("i"), bin(id("i"), ODIN3_AST_BINARY_ADD, num("1")), false);
    odin3_ast_id idx = mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("mem"), id("i"), NO}, 3);
    odin3_ast_id loop = mk(kd(ODIN3_AST_FOR),
                           (odin3_ast_id[]){init, bin(id("i"), ODIN3_AST_BINARY_LT, num("4")), step,
                                            assign(idx, num("8'h0"), false)},
                           4);
    odin3_ast_spec star =
        with_flags(kd(ODIN3_AST_EVENT_CONTROL),
                   ODIN3_AST_F_EVENT_CONTROL_STAR | ODIN3_AST_F_EVENT_CONTROL_NO_PARENS);
    odin3_ast_id ctl = mk(star, &NO, 1);
    odin3_ast_id timed = mk(kd(ODIN3_AST_TIMING_STMT),
                            (odin3_ast_id[]){ctl, block((odin3_ast_id[]){cs, loop}, 2)}, 2);
    return mk(kd(ODIN3_AST_ALWAYS), &timed, 1);
}

static void test_always_case_for(void) {
    expect(comb_block(), "always @* begin\n"
                         "  case (s)\n"
                         "    2'd0, 2'd1: y = a;\n"
                         "    default: begin\n"
                         "    end\n"
                         "  endcase\n"
                         "  for (i = 0; i < 4; i = i + 1) mem[i] = 8'h0;\n"
                         "end\n");
}

static odin3_ast_id stmt_of(odin3_ast_kind kind, const odin3_ast_id *ids, uint32_t n) {
    return mk(kd(kind), ids, n);
}

static void test_other_statements(void) {
    odin3_ast_id null_stmt = leaf(kd(ODIN3_AST_NULL_STMT));
    expect(stmt_of(ODIN3_AST_WHILE, (odin3_ast_id[]){id("c"), null_stmt}, 2), "while (c) ;\n");
    expect(stmt_of(ODIN3_AST_REPEAT, (odin3_ast_id[]){num("3"), null_stmt}, 2), "repeat (3) ;\n");
    odin3_ast_id tick =
        stmt_of(ODIN3_AST_TIMING_STMT,
                (odin3_ast_id[]){delay((odin3_ast_id[]){num("1")}, 1),
                                 assign(id("clk"), un(ODIN3_AST_UNARY_NOT, id("clk")), false)},
                2);
    odin3_ast_id forever = stmt_of(ODIN3_AST_FOREVER, &tick, 1);
    expect(stmt_of(ODIN3_AST_INITIAL, &forever, 1), "initial forever #(1) clk = ~clk;\n");
    expect(stmt_of(ODIN3_AST_WAIT, (odin3_ast_id[]){id("r"), NO}, 2), "wait (r);\n");
    odin3_ast_id hier = stmt_of(ODIN3_AST_HIER_NAME, (odin3_ast_id[]){id("top"), id("blk")}, 2);
    expect(stmt_of(ODIN3_AST_DISABLE, &hier, 1), "disable top.blk;\n");
    odin3_ast_id ev = stmt_of(ODIN3_AST_HIER_NAME, (odin3_ast_id[]){id("e")}, 1);
    expect(stmt_of(ODIN3_AST_EVENT_TRIGGER, &ev, 1), "-> e;\n");
    odin3_ast_spec par = named(ODIN3_AST_PAR_BLOCK, "p");
    expect(mk(par, (odin3_ast_id[]){NO, null_stmt}, 2), "fork : p\n  ;\njoin\n");
    odin3_ast_spec force =
        with_sub(kd(ODIN3_AST_PROC_CONT_ASSIGN), ODIN3_AST_PROC_CONT_ASSIGN_FORCE);
    expect(mk(force, (odin3_ast_id[]){id("a"), num("1'b1")}, 2), "force a = 1'b1;\n");
    odin3_ast_spec release = with_sub(force, ODIN3_AST_PROC_CONT_ASSIGN_RELEASE);
    expect(mk(release, (odin3_ast_id[]){id("a"), NO}, 2), "release a;\n");
    odin3_ast_id wait_del = stmt_of(ODIN3_AST_TIMING_STMT,
                                    (odin3_ast_id[]){delay((odin3_ast_id[]){num("5")}, 1), NO}, 2);
    expect(wait_del, "#(5);\n");
}

static void test_task_calls_and_event_controls(void) {
    odin3_ast_spec sys =
        with_flags(named(ODIN3_AST_TASK_CALL, "$display"), ODIN3_AST_F_TASK_CALL_SYSTEM);
    odin3_ast_id args[4] = {leaf(named(ODIN3_AST_STRING, "x=%d")), id("a"), NO, real(1.5)};
    expect(mk(sys, args, 4), "$display(\"x=%d\", a, , 1.5);\n");
    expect(leaf(with_flags(named(ODIN3_AST_TASK_CALL, "$finish"), ODIN3_AST_F_TASK_CALL_SYSTEM)),
           "$finish;\n");
    expect(leaf(named(ODIN3_AST_TASK_CALL, "t")), "t;\n");
    expect(mk(named(ODIN3_AST_TASK_CALL, "t"), (odin3_ast_id[]){id("a")}, 1), "t(a);\n");
    odin3_ast_spec star = with_flags(kd(ODIN3_AST_EVENT_CONTROL), ODIN3_AST_F_EVENT_CONTROL_STAR);
    expect(mk(star, &NO, 1), "@(*)");
    odin3_ast_spec bare =
        with_flags(kd(ODIN3_AST_EVENT_CONTROL), ODIN3_AST_F_EVENT_CONTROL_NO_PARENS);
    expect(mk(bare, (odin3_ast_id[]){NO, event(ODIN3_AST_EVENT_EXPR_NONE, "clk", false)}, 2),
           "@clk");
    odin3_ast_id rep =
        mk(kd(ODIN3_AST_EVENT_CONTROL),
           (odin3_ast_id[]){num("3"), event(ODIN3_AST_EVENT_EXPR_POSEDGE, "clk", false),
                            event(ODIN3_AST_EVENT_EXPR_NONE, "b", false)},
           3);
    odin3_ast_id intra =
        mk(kd(ODIN3_AST_NONBLOCKING_ASSIGN), (odin3_ast_id[]){id("a"), rep, id("b")}, 3);
    expect(intra, "a <= repeat (3) @(posedge clk, b) b;\n");
    odin3_ast_id mtm =
        mk(kd(ODIN3_AST_MINTYPMAX), (odin3_ast_id[]){num("1"), num("2"), num("3")}, 3);
    expect(delay((odin3_ast_id[]){mtm}, 1), "#(1:2:3)");
}

static void test_dangling_else_is_wrapped(void) {
    odin3_ast_id inner =
        mk(kd(ODIN3_AST_IF), (odin3_ast_id[]){id("b"), assign(id("x"), num("1"), false), NO}, 3);
    odin3_ast_id outer =
        mk(kd(ODIN3_AST_IF), (odin3_ast_id[]){id("a"), inner, assign(id("y"), num("2"), false)}, 3);
    expect(outer, "if (a) begin\n"
                  "  if (b) x = 1;\n"
                  "end\n"
                  "else y = 2;\n");
    odin3_ast_id closed = mk(
        kd(ODIN3_AST_IF),
        (odin3_ast_id[]){id("b"), assign(id("x"), num("1"), false), leaf(kd(ODIN3_AST_NULL_STMT))},
        3);
    odin3_ast_id fine =
        mk(kd(ODIN3_AST_IF), (odin3_ast_id[]){id("a"), closed, leaf(kd(ODIN3_AST_NULL_STMT))}, 3);
    expect(fine, "if (a) if (b) x = 1;\nelse ;\nelse ;\n");
    odin3_ast_id loop = mk(kd(ODIN3_AST_WHILE), (odin3_ast_id[]){id("c"), inner}, 2);
    odin3_ast_id via_loop =
        mk(kd(ODIN3_AST_IF), (odin3_ast_id[]){id("a"), loop, leaf(kd(ODIN3_AST_NULL_STMT))}, 3);
    expect(via_loop, "if (a) begin\n  while (c) if (b) x = 1;\nend\nelse ;\n");
}

/* --- instances, gates, functions, generate --------------------------------------------------- */

static odin3_ast_id conn(const char *name, odin3_ast_id value) {
    return mk(name != NULL ? named(ODIN3_AST_CONNECTION, name) : kd(ODIN3_AST_CONNECTION), &value,
              1);
}

static void test_instances_and_gates(void) {
    odin3_ast_id params = list((odin3_ast_id[]){conn("P", num("1")), conn("Q", NO)}, 2);
    odin3_ast_id u0 = mk(named(ODIN3_AST_INSTANCE, "u0"),
                         (odin3_ast_id[]){NO, conn("a", id("x")), conn("b", NO)}, 3);
    odin3_ast_id u1 = mk(
        named(ODIN3_AST_INSTANCE, "u1"),
        (odin3_ast_id[]){range("1", "0"), conn(NULL, id("x")), conn(NULL, NO), conn(NULL, id("y"))},
        4);
    expect(mk(named(ODIN3_AST_INSTANTIATION, "sub"), (odin3_ast_id[]){params, u0, u1}, 3),
           "sub #(.P(1), .Q()) u0 (.a(x), .b()), u1 [1:0] (x, , y);\n");
    odin3_ast_id u2 = mk(named(ODIN3_AST_INSTANCE, "u2"), &NO, 1);
    expect(mk(named(ODIN3_AST_INSTANTIATION, "sub"), (odin3_ast_id[]){list(NULL, 0), u2}, 2),
           "sub #() u2 ();\n");
    odin3_ast_id g1 = mk(named(ODIN3_AST_INSTANCE, "g1"),
                         (odin3_ast_id[]){NO, conn(NULL, id("o")), conn(NULL, id("a"))}, 3);
    odin3_ast_id g2 = mk(kd(ODIN3_AST_INSTANCE),
                         (odin3_ast_id[]){NO, conn(NULL, id("p")), conn(NULL, id("b"))}, 3);
    odin3_ast_id del = delay((odin3_ast_id[]){num("1"), num("2")}, 2);
    odin3_ast_spec gate = with_sub(kd(ODIN3_AST_GATE_DECL), ODIN3_AST_GATE_DECL_AND);
    expect(mk(gate, (odin3_ast_id[]){NO, del, g1, g2}, 4), "and #(1, 2) g1 (o, a), (p, b);\n");
    odin3_ast_id up = mk(kd(ODIN3_AST_INSTANCE), (odin3_ast_id[]){NO, conn(NULL, id("y"))}, 2);
    odin3_ast_spec pullup = with_sub(kd(ODIN3_AST_GATE_DECL), ODIN3_AST_GATE_DECL_PULLUP);
    odin3_ast_id str = strength(0, ODIN3_AST_DRIVE_STRONG, false);
    expect(mk(pullup, (odin3_ast_id[]){str, NO, up}, 3), "pullup (strong1) (y);\n");
    odin3_ast_id cont =
        mk(kd(ODIN3_AST_CONT_ASSIGN),
           (odin3_ast_id[]){strength(ODIN3_AST_DRIVE_PULL, ODIN3_AST_DRIVE_PULL, false), del,
                            mk(kd(ODIN3_AST_NET_ASSIGN), (odin3_ast_id[]){id("y"), id("a")}, 2),
                            mk(kd(ODIN3_AST_NET_ASSIGN), (odin3_ast_id[]){id("z"), id("b")}, 2)},
           4);
    expect(cont, "assign (pull0, pull1) #(1, 2) y = a, z = b;\n");
}

static odin3_ast_id function_decl(void) {
    odin3_ast_spec in = with_flags(with_sub(kd(ODIN3_AST_PORT_DECL), ODIN3_AST_PORT_DECL_INPUT),
                                   ODIN3_AST_F_PORT_DECL_IN_HEADER);
    odin3_ast_id ports =
        list((odin3_ast_id[]){mk(in, (odin3_ast_id[]){range("3", "0"), decl("v")}, 2)}, 1);
    odin3_ast_id body = assign(id("f"), un(ODIN3_AST_UNARY_NOT, id("v")), false);
    odin3_ast_spec fn =
        with_flags(named(ODIN3_AST_FUNCTION_DECL, "f"), ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC);
    return mk(fn, (odin3_ast_id[]){range("3", "0"), ports, list(NULL, 0), body}, 4);
}

static odin3_ast_id task_decl(void) {
    odin3_ast_spec in = with_sub(kd(ODIN3_AST_PORT_DECL), ODIN3_AST_PORT_DECL_INPUT);
    odin3_ast_id ports = list((odin3_ast_id[]){mk(in, (odin3_ast_id[]){NO, decl("a")}, 2)}, 1);
    odin3_ast_spec integer = with_sub(kd(ODIN3_AST_VAR_DECL), ODIN3_AST_VAR_DECL_INTEGER);
    odin3_ast_id locals =
        list((odin3_ast_id[]){mk(integer, (odin3_ast_id[]){NO, decl("n")}, 2)}, 1);
    odin3_ast_id call =
        mk(with_flags(named(ODIN3_AST_TASK_CALL, "$display"), ODIN3_AST_F_TASK_CALL_SYSTEM),
           (odin3_ast_id[]){id("a")}, 1);
    odin3_ast_id body = block((odin3_ast_id[]){call}, 1);
    return mk(named(ODIN3_AST_TASK_DECL, "t"), (odin3_ast_id[]){ports, locals, body}, 3);
}

static void test_functions_and_tasks(void) {
    expect(function_decl(), "function automatic [3:0] f(input [3:0] v);\n"
                            "  f = ~v;\n"
                            "endfunction\n");
    expect(task_decl(), "task t;\n"
                        "  input a;\n"
                        "  integer n;\n"
                        "  begin\n"
                        "    $display(a);\n"
                        "  end\n"
                        "endtask\n");
    uint32_t dt_int = (uint32_t)ODIN3_AST_DT_INTEGER << ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT;
    odin3_ast_spec fn = with_flags(named(ODIN3_AST_FUNCTION_DECL, "g"), dt_int);
    odin3_ast_id empty = mk(
        fn, (odin3_ast_id[]){NO, list(NULL, 0), list(NULL, 0), leaf(kd(ODIN3_AST_NULL_STMT))}, 4);
    expect(empty, "function integer g;\n  ;\nendfunction\n");
}

static odin3_ast_id generate_block(void) {
    odin3_ast_id init = assign(id("k"), num("0"), false);
    odin3_ast_id step = assign(id("k"), bin(id("k"), ODIN3_AST_BINARY_ADD, num("1")), false);
    odin3_ast_id lhs = mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("w"), id("k"), NO}, 3);
    odin3_ast_id body =
        mk(named(ODIN3_AST_GEN_BLOCK, "g"), (odin3_ast_id[]){NO, cont_assign(lhs, id("k"))}, 2);
    odin3_ast_id loop =
        mk(kd(ODIN3_AST_GEN_FOR),
           (odin3_ast_id[]){init, bin(id("k"), ODIN3_AST_BINARY_LT, num("2")), step, body}, 4);
    odin3_ast_id null_item = leaf(kd(ODIN3_AST_NULL_STMT));
    odin3_ast_id cond = mk(kd(ODIN3_AST_GEN_IF), (odin3_ast_id[]){id("P"), null_item, NO}, 3);
    odin3_ast_id item =
        mk(kd(ODIN3_AST_CASE_ITEM), (odin3_ast_id[]){leaf(kd(ODIN3_AST_NULL_STMT)), num("1")}, 2);
    odin3_ast_id gcase = mk(kd(ODIN3_AST_GEN_CASE), (odin3_ast_id[]){id("P"), item}, 2);
    return mk(kd(ODIN3_AST_GENERATE), (odin3_ast_id[]){loop, cond, gcase}, 3);
}

static void test_generate(void) {
    expect(generate_block(), "generate\n"
                             "  for (k = 0; k < 2; k = k + 1) begin : g\n"
                             "    assign w[k] = k;\n"
                             "  end\n"
                             "  if (P) ;\n"
                             "  case (P)\n"
                             "    1: ;\n"
                             "  endcase\n"
                             "endgenerate\n");
}

static void test_expressions(void) {
    odin3_ast_id cat = mk(kd(ODIN3_AST_CONCAT), (odin3_ast_id[]){id("a"), num("1'b0")}, 2);
    expect(mk(kd(ODIN3_AST_REPLICATE), (odin3_ast_id[]){num("2"), cat}, 2), "{2{a, 1'b0}}");
    odin3_ast_spec part = with_sub(kd(ODIN3_AST_SELECT), ODIN3_AST_SELECT_PART);
    expect(mk(part,
              (odin3_ast_id[]){id("d"), bin(id("W"), ODIN3_AST_BINARY_SUB, num("1")), num("0")}, 3),
           "d[W - 1:0]");
    odin3_ast_spec plus = with_sub(kd(ODIN3_AST_SELECT), ODIN3_AST_SELECT_PART_PLUS);
    expect(mk(plus, (odin3_ast_id[]){id("d"), id("i"), num("4")}, 3), "d[i+:4]");
    odin3_ast_spec minus = with_sub(kd(ODIN3_AST_SELECT), ODIN3_AST_SELECT_PART_MINUS);
    odin3_ast_id nested = mk(kd(ODIN3_AST_SELECT), (odin3_ast_id[]){id("m"), num("2"), NO}, 3);
    expect(mk(minus, (odin3_ast_id[]){nested, id("i"), num("4")}, 3), "m[2][i-:4]");
    odin3_ast_id odd_base =
        mk(kd(ODIN3_AST_SELECT),
           (odin3_ast_id[]){bin(id("a"), ODIN3_AST_BINARY_OR, id("b")), num("0"), NO}, 3);
    expect(odd_base, "(a | b)[0]");
}

/* --- precedence property ------------------------------------------------------------------ */

/* A random operator tree built bottom-up beside an independent fully parenthesised text. */
typedef struct pool {
    odin3_ast_id ids[POOL_MAX];
    char refs[POOL_MAX][REF_MAX];
    bool is_op[POOL_MAX];
    uint32_t n;
} pool;

typedef struct rng {
    uint64_t state;
} rng;

static uint32_t next_rand(rng *gen, uint32_t bound) {
    gen->state = gen->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)((gen->state >> 33) % bound);
}

/* dst = the pieces concatenated (they must fit in REF_MAX). */
static void join(char *dst, const char *const *pieces, uint32_t n) {
    size_t len = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t piece = strlen(pieces[i]);
        TEST_ASSERT_LESS_THAN_size_t(REF_MAX, len + piece);
        memcpy(dst + len, pieces[i], piece);
        len += piece;
    }
    dst[len] = '\0';
}

/* Removes a random entry; its text goes to ref, parenthesised when it is an operator. */
static odin3_ast_id take(pool *pl, rng *gen, char *ref) {
    uint32_t at = next_rand(gen, pl->n);
    odin3_ast_id node = pl->ids[at];
    bool op = pl->is_op[at];
    join(ref, (const char *const[]){op ? "(" : "", pl->refs[at], op ? ")" : ""}, 3);
    pl->n--;
    pl->ids[at] = pl->ids[pl->n];
    pl->is_op[at] = pl->is_op[pl->n];
    memcpy(pl->refs[at], pl->refs[pl->n], REF_MAX);
    return node;
}

static void combine(pool *pl, rng *gen) {
    static char parts[3][REF_MAX];
    static char ref[REF_MAX];
    uint32_t pick = next_rand(gen, 3);
    odin3_ast_id node = {0};
    if (pick == 0 || pl->n < 2) {
        uint32_t op = next_rand(gen, ODIN3_AST_UNARY_COUNT);
        odin3_ast_id operand = take(pl, gen, parts[0]);
        node = un(op, operand);
        join(ref, (const char *const[]){odin3_ast_sub_name(ast, node), parts[0]}, 2);
    } else if (pick == 1 || pl->n < 3) {
        uint32_t op = next_rand(gen, ODIN3_AST_BINARY_COUNT);
        odin3_ast_id lhs = take(pl, gen, parts[0]);
        node = bin(lhs, op, take(pl, gen, parts[1]));
        join(ref,
             (const char *const[]){parts[0], " ", odin3_ast_sub_name(ast, node), " ", parts[1]}, 5);
    } else {
        odin3_ast_id ids[3] = {take(pl, gen, parts[0]), take(pl, gen, parts[1]),
                               take(pl, gen, parts[2])};
        node = mk(kd(ODIN3_AST_TERNARY), ids, 3);
        join(ref, (const char *const[]){parts[0], " ? ", parts[1], " : ", parts[2]}, 5);
    }
    memcpy(pl->refs[pl->n], ref, REF_MAX);
    pl->ids[pl->n] = node;
    pl->is_op[pl->n] = true;
    pl->n++;
}

static void test_precedence_property(void) {
    static const char *const LEAVES[] = {"a", "b", "c", "d", "e"};
    rng gen = {7};
    for (uint32_t tree = 0; tree < PRECEDENCE_TREES; tree++) {
        static pool pl;
        pl.n = 0;
        for (uint32_t i = 0; i < 5; i++) {
            pl.ids[pl.n] = id(LEAVES[i]);
            (void)snprintf(pl.refs[pl.n], REF_MAX, "%s", LEAVES[i]);
            pl.is_op[pl.n++] = false;
        }
        while (pl.n > 1 || !pl.is_op[0]) {
            combine(&pl, &gen);
        }
        expect(pl.ids[0], pl.refs[0]);
    }
}

/* --- robustness ------------------------------------------------------------------------------ */

static void test_chain_at_cap(void) {
    odin3_ast_spec unary = with_sub(kd(ODIN3_AST_UNARY), ODIN3_AST_UNARY_MINUS);
    odin3_ast_id top = id("x");
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &unary, &top, 1, &top));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, top, &out));
    uint32_t ops = ODIN3_AST_MAX_DEPTH - 1;
    TEST_ASSERT_EQUAL_size_t(1 + 2 * (size_t)(ops - 1) + 1 + (size_t)(ops - 1), out.len);
    TEST_ASSERT_EQUAL_MEMORY("-(-(-", out.data, 5);
    TEST_ASSERT_EQUAL_MEMORY("(-x)))", out.data + 2 * (size_t)(ops - 1) - 1, 6);
}

/* Review Focus 5: absent optional slots and abandoned roots print without faulting. */
static void test_absent_slots_and_abandoned_roots(void) {
    odin3_ast_id abandoned = bin(id("p"), ODIN3_AST_BINARY_ADD, id("q")); /* never used */
    odin3_ast_id gif =
        mk(kd(ODIN3_AST_GEN_IF), (odin3_ast_id[]){id("c"), leaf(kd(ODIN3_AST_NULL_STMT)), NO}, 3);
    odin3_ast_id inst = mk(named(ODIN3_AST_INSTANCE, "u"), &NO, 1);
    odin3_ast_id unit = mk(named(ODIN3_AST_UNIT, "t.v"), NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect(abandoned, "p + q");
    expect(gif, "if (c) ;\n");
    expect(inst, "u ()");
    expect(unit, "");
}

static void test_misuse(void) {
    odin3_ast_id node = id("a");
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_print(NULL, node, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_print(ast, node, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_print(ast, NO, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_print(ast, (odin3_ast_id){99}, &out));
    TEST_ASSERT_EQUAL_size_t(errors + 4, errors_logged);
}

static void test_oom_sweep(void) {
    odin3_ast_id big = cont_assign(id("y"), num("100'd1267650600228229401496703205376"));
    odin3_ast_id items[6] = {NO, NO, generate_block(), comb_block(), posedge_block(), big};
    odin3_ast_id mod = mk(named(ODIN3_AST_MODULE, "z"), items, 6);
    attach(mod, (odin3_ast_id[]){attr("keep", num("1"), 0)}, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    bool done = false;
    for (long k = 0; k < OOM_LIMIT && !done; k++) {
        odin3_strbuf buf;
        odin3_strbuf_init(&buf);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_ast_print(ast, mod, &buf);
        odin3_util_set_alloc_fail_after(-1);
        odin3_strbuf_free(&buf);
        done = st == ODIN3_OK;
        if (!done) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
    }
    TEST_ASSERT_TRUE(done);
}

/* --- the property test ----------------------------------------------------------------------- */

/* A random size and base (`12'sh`, `'b`), or nothing for a plain decimal; the base index. */
static uint32_t random_prefix(rng *gen, char *buf, size_t cap, size_t *len) {
    static const char *const BASES = "dboh";
    uint32_t base = next_rand(gen, 5); /* 4: a plain decimal */
    if (base == 4) {
        return 0;
    }
    uint32_t width = next_rand(gen, 3) == 0 ? 0 : 1 + next_rand(gen, 80);
    *len = width > 0 ? (size_t)snprintf(buf, cap, "%u", width) : 0;
    const char *sgn = next_rand(gen, 2) != 0 ? "s" : "";
    *len += (size_t)snprintf(buf + *len, cap - *len, "'%s%c", sgn, BASES[base]);
    return base;
}

/* A random literal's text (`12'sh_f?x`, `'d42`, `4'dx`, `5` …) into buf. */
static void random_literal(rng *gen, char *buf, size_t cap) {
    static const char *const DIGITS[4] = {"0123456789", "01xzXZ?", "01234567xz?",
                                          "0123456789abcdefABCDEFxz?"};
    size_t len = 0;
    uint32_t base = random_prefix(gen, buf, cap, &len);
    bool decimal_xz = base == 0 && len > 0 && next_rand(gen, 4) == 0;
    uint32_t count = decimal_xz ? 1 : 1 + next_rand(gen, 24);
    const char *digits = decimal_xz ? "xzX?" : DIGITS[base];
    for (uint32_t i = 0; i < count && len + 2 < cap; i++) {
        buf[len++] = digits[next_rand(gen, (uint32_t)strlen(digits))];
        bool underscore = !decimal_xz && i + 1 < count && next_rand(gen, 6) == 0;
        buf[len] = '_';
        len += underscore ? 1 : 0;
    }
    buf[len] = '\0';
}

static void assert_same_value(odin3_ast_id first, odin3_ast_id second, odin3_arena *arena) {
    odin3_ast_literal one = {{0}, false, false};
    odin3_ast_literal two = {{0}, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast, first, arena, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(ast, second, arena, &two));
    TEST_ASSERT_EQUAL_UINT32(one.value.len, two.value.len);
    TEST_ASSERT_EQUAL_MEMORY(one.value.bits, two.value.bits, one.value.len);
    TEST_ASSERT_EQUAL(one.is_signed, two.is_signed);
    TEST_ASSERT_EQUAL(one.is_unsized, two.is_unsized);
}

static void test_number_round_trip_property(void) {
    rng gen = {20261010};
    odin3_arena *arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    char text[TEXT_MAX];
    char again[TEXT_MAX];
    for (uint32_t i = 0; i < PROPERTY_LITERALS; i++) {
        random_literal(&gen, text, sizeof text);
        odin3_ast_id first = num(text);
        odin3_strbuf_clear(&out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, first, &out));
        TEST_ASSERT_LESS_THAN_size_t(sizeof again, out.len);
        memcpy(again, out.data, out.len + 1);
        odin3_ast_id second = num(again);
        bool same = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_equal(ast, first, ast, second, &same));
        if (!same) {
            TEST_FAIL_MESSAGE(text);
        }
        assert_same_value(first, second, arena);
    }
    odin3_arena_destroy(arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cont_assign_parenthesises_operands);
    RUN_TEST(test_ansi_module);
    RUN_TEST(test_case_metacomment);
    RUN_TEST(test_attributes);
    RUN_TEST(test_numbers);
    RUN_TEST(test_numbers_mixing_xz_print_in_binary);
    RUN_TEST(test_decimal_digit_cap);
    RUN_TEST(test_strings);
    RUN_TEST(test_names);
    RUN_TEST(test_opaque_text_verbatim);
    RUN_TEST(test_net_and_var_declarations);
    RUN_TEST(test_param_and_port_declarations);
    RUN_TEST(test_module_headers);
    RUN_TEST(test_always_if_else);
    RUN_TEST(test_always_case_for);
    RUN_TEST(test_other_statements);
    RUN_TEST(test_task_calls_and_event_controls);
    RUN_TEST(test_dangling_else_is_wrapped);
    RUN_TEST(test_instances_and_gates);
    RUN_TEST(test_functions_and_tasks);
    RUN_TEST(test_generate);
    RUN_TEST(test_expressions);
    RUN_TEST(test_precedence_property);
    RUN_TEST(test_chain_at_cap);
    RUN_TEST(test_absent_slots_and_abandoned_roots);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_number_round_trip_property);
    return UNITY_END();
}
