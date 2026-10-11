/*
 * test_verilog_parser_expr.c — the expression grammar of 2C Task 5 (spec §3.3 precedence, §3.6
 * "Expressions"; the 2A slot table §4.3): each case parsed as the right-hand side of
 * `assign y = …;`, its tree compared with a literal string, and its module printed, re-parsed and
 * compared with odin3_ast_equal (the round trip of every case). Also the syntax errors the
 * grammar's shape gives, the Bison stack on a long operator chain, the AST depth cap and an OOM
 * sweep over the expression actions.
 *
 * The compact tree form: an IDENT is its name, a NUMBER, REAL or STRING its printed literal, any
 * other node `KIND[sub]:name{flags}(children)` (the sub for UNARY, BINARY and SELECT; the name
 * when it has one; CALL's SYSTEM/HIER flags), an absent child `_`.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/print.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/parser_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "verilog_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    LONG_CHAIN = 2000,          /* terms of the flat-stack chain (2A §10) */
    LOWERED_STACK = 256,        /* the stack-cap hook's floor, YYINITDEPTH */
    DEPTH_CHAIN = 32769,        /* terms whose left-deep tree passes ODIN3_AST_MAX_DEPTH */
    EXPR_OOM_LIMIT = 8192,      /* allocations of the sweep text, with headroom */
    CONT_ASSIGN_NET_ASSIGN = 2, /* CONT_ASSIGN's first tail entry (after STRENGTH, DELAY) */
    MAX_STEPS = 1024            /* the compact form's explicit stack */
};

static const char HEAD[] = "module m; assign y = ";
static const char TAIL[] = "; endmodule";

void setUp(void) {
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_util_set_alloc_fail_after(-1);
}

/* --- helpers ---------------------------------------------------------------------------------- */

static void append(odin3_strbuf *out, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(text)));
}

/* HEAD expr TAIL into text (initialised; the caller frees it). */
static void assign_text(const char *expr, odin3_strbuf *text) {
    odin3_strbuf_init(text);
    append(text, HEAD);
    append(text, expr);
    append(text, TAIL);
}

/* The UNIT's MODULE (its first child). */
static odin3_ast_id module_of(const vt_result *res) {
    odin3_ast_id module = odin3_ast_child(res->ast, res->unit, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_MODULE, odin3_ast_kind_of(res->ast, module));
    return module;
}

/* The right-hand side of the module's first `assign`. */
static odin3_ast_id rhs_of(const vt_result *res) {
    odin3_ast_id module = module_of(res);
    odin3_ast_id assign = odin3_ast_child(res->ast, module, odin3_ast_nchild(res->ast, module) - 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_CONT_ASSIGN, odin3_ast_kind_of(res->ast, assign));
    odin3_ast_id net = odin3_ast_child(res->ast, assign, CONT_ASSIGN_NET_ASSIGN);
    return odin3_ast_child(res->ast, net, 1);
}

/* One step of the compact form: a node to write, or (text non-NULL) text to append. */
typedef struct step {
    const char *text;
    odin3_ast_id node;
} step;

/* The steps still to take (an explicit stack: the lint gate forbids recursion). */
static step steps[MAX_STEPS];
static uint32_t nsteps;

static void push_step(const char *text, odin3_ast_id node) {
    TEST_ASSERT_TRUE(nsteps < MAX_STEPS);
    steps[nsteps++] = (step){text, node};
}

/* Node's own part of the compact form, before its children: KIND[sub]:name{flags}. */
static void compact_head(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    append(out, odin3_ast_kind_name(kind));
    if (kind == ODIN3_AST_UNARY || kind == ODIN3_AST_BINARY || kind == ODIN3_AST_SELECT) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(out, "[%s]", odin3_ast_sub_name(ast, node)));
    }
    if (odin3_ast_name(ast, node) != 0) {
        append(out, ":");
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_pr_name_bytes(ast, node)));
    }
    uint32_t flags = odin3_ast_flags(ast, node);
    if (kind == ODIN3_AST_CALL && flags != 0) {
        append(out, (flags & ODIN3_AST_F_CALL_SYSTEM) != 0 ? "{SYSTEM}" : "{HIER}");
    }
}

/* Writes node, a leaf, or its head with its children pushed (last first) as further steps. */
static void compact_step(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    if (node.v == 0) {
        append(out, "_");
    } else if (kind == ODIN3_AST_IDENT) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_pr_name_bytes(ast, node)));
    } else if (kind == ODIN3_AST_NUMBER || kind == ODIN3_AST_REAL || kind == ODIN3_AST_STRING) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(ast, node, out));
    } else {
        compact_head(ast, node, out);
        uint32_t count = odin3_ast_nchild(ast, node);
        push_step(count > 0 ? ")" : "", (odin3_ast_id){0});
        for (uint32_t i = count; i > 0; i--) {
            push_step(NULL, odin3_ast_child(ast, node, i - 1));
            push_step(i == 1 ? "(" : ", ", (odin3_ast_id){0});
        }
    }
}

/* The compact form of node. */
static void compact(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    nsteps = 0;
    push_step(NULL, node);
    while (nsteps > 0) {
        step next = steps[--nsteps];
        if (next.text != NULL) {
            append(out, next.text);
        } else {
            compact_step(ast, next.node, out);
        }
    }
}

/* Parses text as x.v; it must succeed with exactly the log want ("" for none). */
static void parse_logged(const char *text, const char *want, vt_result *res) {
    vt_parse("x.v", text, res);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, res->log.data != NULL ? res->log.data : "", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res->status);
}

/* res's module prints, the text re-parses (with the log want) and the two modules are equal. */
static void expect_round_trip(const vt_result *res, const char *want_log) {
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res->ast, module_of(res), &printed));
    vt_result again;
    parse_logged(printed.data, want_log, &again);
    bool equal = false;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_equal(res->ast, module_of(res), again.ast, module_of(&again), &equal));
    TEST_ASSERT_TRUE_MESSAGE(equal, printed.data);
    vt_free(&again);
    odin3_strbuf_free(&printed);
}

/* check's informational line for unreachable non-root nodes (none: log stays empty, data NULL). */
static void unreachable_log(uint32_t unreachable, odin3_strbuf *log) {
    odin3_strbuf_init(log);
    if (unreachable > 0) {
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_strbuf_appendf(log, "ast check: rule 3: %u unreachable non-root nodes\n",
                                 (unsigned)unreachable));
    }
}

/* `assign y = expr;` parses (check reporting unreachable nodes, rule 3) to the compact tree tree
 * and round-trips. */
static void expect_tree_logged(const char *expr, uint32_t unreachable, const char *tree) {
    odin3_strbuf text;
    assign_text(expr, &text);
    odin3_strbuf want_log;
    unreachable_log(unreachable, &want_log);
    const char *want = want_log.data != NULL ? want_log.data : "";
    vt_result res;
    parse_logged(text.data, want, &res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    compact(res.ast, rhs_of(&res), &got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(tree, got.data, expr);
    expect_round_trip(&res, want);
    odin3_strbuf_free(&got);
    vt_free(&res);
    odin3_strbuf_free(&want_log);
    odin3_strbuf_free(&text);
}

static void expect_tree(const char *expr, const char *tree) {
    expect_tree_logged(expr, 0, tree);
}

/* `assign y = expr;` parses to the right-hand side whose full dump is want, and round-trips. */
static void expect_dump(const char *expr, const char *want) {
    odin3_strbuf text;
    assign_text(expr, &text);
    vt_result res;
    parse_logged(text.data, "", &res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(res.ast, rhs_of(&res), &got));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got.data, expr);
    expect_round_trip(&res, "");
    odin3_strbuf_free(&got);
    vt_free(&res);
    odin3_strbuf_free(&text);
}

/* The printed text of `assign y = expr;`'s right-hand side. */
static void expect_printed(const char *expr, const char *want) {
    odin3_strbuf text;
    assign_text(expr, &text);
    vt_result res;
    parse_logged(text.data, "", &res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res.ast, rhs_of(&res), &got));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got.data, expr);
    odin3_strbuf_free(&got);
    vt_free(&res);
    odin3_strbuf_free(&text);
}

/* text must fail with status, no UNIT and exactly the log want. */
static void expect_error(const char *text, odin3_status status, const char *want) {
    vt_result res;
    vt_parse("x.v", text, &res);
    TEST_ASSERT_EQUAL_INT(status, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING(want, res.log.data != NULL ? res.log.data : "");
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    vt_free(&res);
}

/* --- precedence and associativity (Table 5-4, §3.3) ----------------------------------------- */

static void test_multiplication_binds_tighter(void) {
    expect_tree("a + b * c", "BINARY[+](a, BINARY[*](b, c))");
    expect_tree("a * b + c", "BINARY[+](BINARY[*](a, b), c)");
}

/* `**` is left-associative: every binary operator associates left to right (Table 5-4). */
static void test_power_is_left_associative(void) {
    expect_tree("a ** b ** c", "BINARY[**](BINARY[**](a, b), c)");
    expect_tree("a * b ** c", "BINARY[*](a, BINARY[**](b, c))");
}

/* A unary operator binds to a primary only. */
static void test_unary_binds_to_a_primary(void) {
    expect_tree("-a ** b", "BINARY[**](UNARY[-](a), b)");
    expect_tree("!a && b", "BINARY[&&](UNARY[!](a), b)");
    expect_tree("~a & b", "BINARY[&](UNARY[~](a), b)");
    expect_tree("a - -b", "BINARY[-](a, UNARY[-](b))");
}

static void test_conditional_is_right_associative(void) {
    expect_tree("a ? b : c ? d : e", "TERNARY(a, b, TERNARY(c, d, e))");
    expect_tree("a ? b ? c : d : e", "TERNARY(a, TERNARY(b, c, d), e)");
    expect_tree("a || b ? c : d + e", "TERNARY(BINARY[||](a, b), c, BINARY[+](d, e))");
}

static void test_relational_before_equality(void) {
    expect_tree("a < b == c", "BINARY[==](BINARY[<](a, b), c)");
    expect_tree("a == b < c", "BINARY[==](a, BINARY[<](b, c))");
    expect_tree("a <= b != c >= d", "BINARY[!=](BINARY[<=](a, b), BINARY[>=](c, d))");
    expect_tree("a === b !== c", "BINARY[!==](BINARY[===](a, b), c)");
}

static void test_bitwise_levels(void) {
    expect_tree("a & b | c", "BINARY[|](BINARY[&](a, b), c)");
    expect_tree("a | b & c", "BINARY[|](a, BINARY[&](b, c))");
    expect_tree("a ^ b & c", "BINARY[^](a, BINARY[&](b, c))");
    expect_tree("a | b ^ c", "BINARY[|](a, BINARY[^](b, c))");
    expect_tree("a && b || c && d", "BINARY[||](BINARY[&&](a, b), BINARY[&&](c, d))");
}

static void test_shifts_and_arithmetic(void) {
    expect_tree("a << b + c", "BINARY[<<](a, BINARY[+](b, c))");
    expect_tree("a >>> b < c", "BINARY[<](BINARY[>>>](a, b), c)");
    expect_tree("a <<< 1 >> 2", "BINARY[>>](BINARY[<<<](a, 1), 2)");
    expect_tree("a / b % c - d", "BINARY[-](BINARY[%](BINARY[/](a, b), c), d)");
}

/* `^~` is stored as `~^`, as a binary and as a unary operator; unary `+` is kept. */
static void test_xnor_spellings_are_one_sub(void) {
    expect_tree("a ~^ b", "BINARY[~^](a, b)");
    expect_tree("a ^~ b", "BINARY[~^](a, b)");
    expect_tree("^~a", "UNARY[~^](a)");
    expect_tree("~^a", "UNARY[~^](a)");
    expect_tree("+a", "UNARY[+](a)");
}

static void test_reduction_operators(void) {
    expect_tree("&a | ~&b", "BINARY[|](UNARY[&](a), UNARY[~&](b))");
    expect_tree("|a ^ ~|b", "BINARY[^](UNARY[|](a), UNARY[~|](b))");
    expect_tree("^a", "UNARY[^](a)");
}

/* A doubled unary operator needs parentheses (§3.3: stricter than Odin II and Yosys). Bison
 * names no expected token when there are more than four. */
static void test_doubled_unary_is_a_syntax_error(void) {
    expect_error("module m; assign y = - -a; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:24: error: syntax: unexpected '-'\n");
    expect_tree("-(-a)", "UNARY[-](UNARY[-](a))");
}

/* The strict forms a looser grammar would accept (review M3): a doubled unary operator, a
 * replication without its inner braces or followed by more items (A.8.1), a user call without an
 * argument (A.8.2), two selects on a non-final component (G7), a min:typ:max outside `#(` (§3.1
 * f), and a binary `~&` (A.8.6 has none). */
static void test_strict_forms_are_syntax_errors(void) {
    expect_error("module m; assign y = !~a; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:23: error: syntax: unexpected '~'\n");
    expect_error("module m; assign y = {2{3{a}}}; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:26: error: syntax: unexpected '{'\n");
    expect_error("module m; assign y = {4{a}, b}; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:27: error: syntax: unexpected ',', expecting '}'\n");
    expect_error("module m; assign y = f(); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:24: error: syntax: unexpected ')'\n");
    expect_error("module m; assign y = u.v[1][2].w; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:31: error: syntax: unexpected '.'\n");
    expect_error("module m; assign y = (1:2:3); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:24: error: syntax: unexpected ':'\n");
    expect_error("module m; assign y = a ~& b; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:24: error: syntax: unexpected ~&\n");
}

/* --- attributes on operators (§3.5) ----------------------------------------------------------- */

static void test_attributes_on_operators(void) {
    expect_dump("- (* k *) a", "(UNARY.- @x.v:1:22\n"
                               "  (IDENT:a @x.v:1:32)\n"
                               "  {(ATTR:k @x.v:1:27\n"
                               "     _)})");
    expect_dump("a + (* k = 1 *) b", "(BINARY.+ @x.v:1:22\n"
                                     "  (IDENT:a @x.v:1:22)\n"
                                     "  (IDENT:b @x.v:1:38)\n"
                                     "  {(ATTR:k @x.v:1:29\n"
                                     "     (NUMBER.dec [SIGNED] @x.v:1:33 1))})");
    expect_dump("a ? (* k *) (* j *) b : c", "(TERNARY @x.v:1:22\n"
                                             "  (IDENT:a @x.v:1:22)\n"
                                             "  (IDENT:b @x.v:1:42)\n"
                                             "  (IDENT:c @x.v:1:46)\n"
                                             "  {(ATTR:k @x.v:1:29\n"
                                             "     _)\n"
                                             "   (ATTR:j @x.v:1:37\n"
                                             "     _)})");
    expect_printed("a + (* k *) b * (* j *) c", "a + (* k *) (b * (* j *) c)");
}

/* Nested operators each take their own attributes: the inner ones are not the outer's. The
 * printer writes a unary operator's attributes right after it. */
static void test_nested_operator_attributes(void) {
    expect_printed("a * (* x *) - (* y *) b + (* z *) c", "(a * (* x *) (-(* y *) b)) + (* z *) c");
}

/* --- primaries ---------------------------------------------------------------------------------
 */

static void test_concatenation_and_replication(void) {
    expect_tree("{a, b}", "CONCAT(a, b)");
    expect_tree("{a}", "CONCAT(a)");
    expect_tree("{4{a, b}}", "REPLICATE(4, CONCAT(a, b))");
    expect_tree("{2{a}}", "REPLICATE(2, CONCAT(a))");
    expect_tree("{a, {2{b, {c}}}}", "CONCAT(a, REPLICATE(2, CONCAT(b, CONCAT(c))))");
    expect_tree("{N + 1{a}}", "REPLICATE(BINARY[+](N, 1), CONCAT(a))");
}

/* The inner CONCAT of a replication is located at its own `{`. */
static void test_replication_locations(void) {
    expect_dump("{2{a}}", "(REPLICATE @x.v:1:22\n"
                          "  (NUMBER.dec [SIGNED] @x.v:1:23 2)\n"
                          "  (CONCAT @x.v:1:24\n"
                          "    (IDENT:a @x.v:1:25)))");
}

/* Selects nest base first (N13); the third slot is present exactly for the part forms. */
static void test_selects(void) {
    expect_tree("m[i][j]", "SELECT[bit](SELECT[bit](m, i, _), j, _)");
    expect_tree("m[i][3:0]", "SELECT[part](SELECT[bit](m, i, _), 3, 0)");
    expect_tree("a[b+:4]", "SELECT[part_plus](a, b, 4)");
    expect_tree("a[b-:4]", "SELECT[part_minus](a, b, 4)");
    expect_tree("a[i + 1] & b[7:0]",
                "BINARY[&](SELECT[bit](a, BINARY[+](i, 1), _), SELECT[part](b, 7, 0))");
}

/* A select on the last component wraps the whole name (G7: a non-final component may carry
 * one bit select). */
static void test_hierarchical_names(void) {
    expect_tree("a.b.c", "HIER_NAME(a, b, c)");
    expect_tree("g[0].t", "HIER_NAME(SELECT[bit](g, 0, _), t)");
    expect_tree("u.w[3]", "SELECT[bit](HIER_NAME(u, w), 3, _)");
    expect_tree("u.w[3:0] + g[1].t", "BINARY[+](SELECT[part](HIER_NAME(u, w), 3, 0), "
                                     "HIER_NAME(SELECT[bit](g, 1, _), t))");
}

static void test_user_calls(void) {
    expect_tree("f(a, b)", "CALL:f(a, b)");
    expect_tree("f(a + 1)", "CALL:f(BINARY[+](a, 1))");
    expect_tree("f(g(a), b) * 2", "BINARY[*](CALL:f(CALL:g(a), b), 2)");
}

static void test_system_calls(void) {
    expect_tree("$signed(a)", "CALL:$signed{SYSTEM}(a)");
    expect_tree("$clog2(N)", "CALL:$clog2{SYSTEM}(N)");
    expect_tree("$time", "CALL:$time{SYSTEM}");
    expect_tree("$random()", "CALL:$random{SYSTEM}");
    expect_tree("$unsigned(s) >> 1", "BINARY[>>](CALL:$unsigned{SYSTEM}(s), 1)");
}

/* System-call arguments may be empty (E*0, §3.1 k): a 0 tail entry per empty argument. */
static void test_system_call_empty_arguments(void) {
    expect_tree("$display(\"x\", , b)", "CALL:$display{SYSTEM}(\"x\", _, b)");
    expect_tree("$f(, a)", "CALL:$f{SYSTEM}(_, a)");
    expect_tree("$f(a, )", "CALL:$f{SYSTEM}(a, _)");
    expect_tree("$f(,)", "CALL:$f{SYSTEM}(_, _)");
}

/* A user call with an empty argument is a syntax error (§3.1 j), not a builder INVALID_ARG. */
static void test_user_call_empty_argument_is_a_syntax_error(void) {
    expect_error("module m; assign y = f(a, , b); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:27: error: syntax: unexpected ','\n");
}

/*
 * A hierarchical call is CALL{HIER} named by its components as the printer spells them, so the
 * printed call re-parses to the same name. The component nodes it was parsed from are left
 * unreachable (`g[0]`: one SELECT over its IDENT and index), which check reports as information
 * (rule 3).
 */
static void test_hierarchical_calls(void) {
    expect_tree_logged("u.f(a)", 1, "CALL:u.f{HIER}(a)");
    expect_tree_logged("g[0].f(a)", 1, "CALL:g[0].f{HIER}(a)");
    expect_tree_logged("\\a+b .f(a)", 1, "CALL:\\a+b .f{HIER}(a)");
    expect_tree_logged("u.v.f(a, b)", 2, "CALL:u.v.f{HIER}(a, b)");
}

/*
 * An escaped identifier may hold a backtick (`\f`x `): the lexer and 2B accept it, the 1F
 * writer calls it unwritable. As a callee component it is spelled as the printer writes it, so
 * the call parses with no diagnostic and round-trips (review I1).
 */
static void test_hierarchical_call_with_a_backtick(void) {
    expect_tree_logged("u.\\f`x (a)", 1, "CALL:u.\\f`x {HIER}(a)");
    expect_tree_logged("\\u`v .f(a)", 1, "CALL:\\u`v .f{HIER}(a)");
    expect_tree_logged("g[0].\\f`x (a)", 1, "CALL:g[0].\\f`x {HIER}(a)");
}

/* The same name as a plain hierarchical reference: the printer writes it `\f`x ` with its
 * "written as stored" warning, and the text re-parses to an equal module. */
static void test_hierarchical_reference_with_a_backtick(void) {
    odin3_strbuf text;
    assign_text("u.\\f`x ", &text); /* the blank ends the escaped name */
    vt_result res;
    parse_logged(text.data, "", &res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    compact(res.ast, rhs_of(&res), &got);
    TEST_ASSERT_EQUAL_STRING("HIER_NAME(u, f`x)", got.data);
    vt_result printed;
    memset(&printed, 0, sizeof printed);
    odin3_strbuf_init(&printed.log);
    odin3_strbuf_init(&printed.dump);
    odin3_log_set_sink(vt_sink, &printed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res.ast, module_of(&res), &printed.dump));
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_NOT_NULL(strstr(printed.dump.data, "assign y = u.\\f`x ;"));
    TEST_ASSERT_NOT_NULL(strstr(printed.log.data, "written as stored"));
    vt_result again;
    parse_logged(printed.dump.data, "", &again);
    bool equal = false;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_equal(res.ast, module_of(&res), again.ast, module_of(&again), &equal));
    TEST_ASSERT_TRUE(equal);
    vt_free(&again);
    odin3_strbuf_free(&printed.log);
    odin3_strbuf_free(&printed.dump);
    odin3_strbuf_free(&got);
    vt_free(&res);
    odin3_strbuf_free(&text);
}

static void test_literals(void) {
    expect_dump("1.5", "(REAL @x.v:1:22 1.5)");
    expect_dump("\"s\"", "(STRING:\"s\" @x.v:1:22)");
    expect_tree("\"s\" == \"t\"", "BINARY[==](\"s\", \"t\")");
}

/* `'sd5` and `5` are the same node, DEC + SIGNED (§3.7); `8'HfF` prints `8'hff`. */
static void test_number_forms(void) {
    expect_dump("'sd5", "(NUMBER.dec [SIGNED] @x.v:1:22 5)");
    expect_dump("5", "(NUMBER.dec [SIGNED] @x.v:1:22 5)");
    expect_printed("8'HfF", "8'hff");
    expect_tree("8'HfF + 'sd5", "BINARY[+](8'hff, 5)");
}

/* Parentheses make no node; the printer parenthesises every nested operator. */
static void test_parentheses(void) {
    expect_tree("(a + b) + c", "BINARY[+](BINARY[+](a, b), c)");
    expect_tree("a + (b + c)", "BINARY[+](a, BINARY[+](b, c))");
    expect_printed("(a + b) + c", "(a + b) + c");
    expect_printed("a + (b + c)", "a + (b + c)");
    expect_printed("a + b + c", "(a + b) + c");
    expect_tree("((a))", "a");
}

/* A parenthesised operand: the outer node starts at the `(`, the inner one at its operand. */
static void test_parenthesised_operand_locations(void) {
    expect_dump("(a + b) >> 1", "(BINARY.>> @x.v:1:22\n"
                                "  (BINARY.+ @x.v:1:23\n"
                                "    (IDENT:a @x.v:1:23)\n"
                                "    (IDENT:b @x.v:1:27))\n"
                                "  (NUMBER.dec [SIGNED] @x.v:1:33 1))");
}

/* Expressions wherever Task 4's rules take one: ranges, initialisers, parameters, delays. */
static void test_expressions_in_declarations(void) {
    vt_result res;
    parse_logged("module m #(parameter W = 2 * 4) (input [W-1:0] a);\n"
                 "  localparam N = $clog2(W) + 1;\n"
                 "  wire [N-1:0] w = {N{1'b0}};\n"
                 "  reg [7:0] mem [0:(1 << N) - 1];\n"
                 "  assign #(1:2:3) w = a[W-1 -: 2] ? ~a : {a[0], a[W-1:1]};\n"
                 "endmodule",
                 "", &res);
    expect_round_trip(&res, "");
    vt_free(&res);
}

/* --- long chains: the Bison stack and the AST depth cap ----------------------------------------
 */

/* HEAD, count terms `a` joined by op, then TAIL into text (initialised; the caller frees it). */
static void chain_text(const char *op, uint32_t count, odin3_strbuf *text) {
    odin3_strbuf_init(text);
    append(text, HEAD);
    for (uint32_t i = 0; i < count; i++) {
        append(text, i == 0 ? "a" : op);
        append(text, i == 0 ? "" : "a");
    }
    append(text, TAIL);
}

/* A fresh design and parsed store in res, with empty buffers; free with vt_free. */
static void fresh_store(vt_result *res) {
    memset(res, 0, sizeof *res);
    odin3_strbuf_init(&res->dump);
    odin3_strbuf_init(&res->log);
    res->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(res->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(res->design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &res->ast));
}

/* Parses text with the run's caps lowered to limits; the store's largest node height. */
static uint32_t parse_with_limits(const char *text, odin3_vp_limits limits, vt_result *res) {
    fresh_store(res);
    odin3_vp_stream stream;
    vt_stream(res->design, "x.v", odin3_bytes_cstr(text), &stream);
    odin3_vl_run run;
    odin3_vl_run_init(&run, res->design);
    odin3_verilog_test_set_limits(&run, limits);
    odin3_log_set_sink(vt_sink, res);
    res->status = odin3_verilog_parse_stream(&run, res->ast, &stream, &res->unit);
    odin3_log_set_sink(NULL, NULL);
    return odin3_ast_max_height(res->ast);
}

/*
 * A 2,000-term `&` chain parses with the Bison stack at its floor (256 entries: the operator
 * rules reduce as they go, 2A §10) into a left-deep tree whose root is 2,000 high.
 */
static void test_long_chain_keeps_the_stack_flat(void) {
    vt_result one;
    odin3_strbuf single;
    chain_text(" & ", 1, &single);
    uint32_t above =
        parse_with_limits(single.data, (odin3_vp_limits){.yymaxdepth = LOWERED_STACK}, &one) -
        1; /* the containers above the right-hand side */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, one.status);
    vt_result res;
    odin3_strbuf text;
    chain_text(" & ", LONG_CHAIN, &text);
    uint32_t height =
        parse_with_limits(text.data, (odin3_vp_limits){.yymaxdepth = LOWERED_STACK}, &res);
    TEST_ASSERT_EQUAL_STRING("", res.log.data != NULL ? res.log.data : "");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    TEST_ASSERT_EQUAL_UINT32(LONG_CHAIN, height - above);
    odin3_ast_id node = rhs_of(&res);
    for (uint32_t i = 1; i < LONG_CHAIN; i++) { /* left-deep: the left operand is the chain */
        TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY, odin3_ast_kind_of(res.ast, node));
        node = odin3_ast_child(res.ast, node, 0);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_IDENT, odin3_ast_kind_of(res.ast, node));
    vt_free(&res);
    vt_free(&one);
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&single);
}

/* A 32,769-term chain passes ODIN3_AST_MAX_DEPTH: located at the chain's start, fatal. */
static void test_chain_past_the_depth_cap(void) {
    vt_result res;
    odin3_strbuf text;
    chain_text("&", DEPTH_CHAIN, &text);
    (void)parse_with_limits(text.data, (odin3_vp_limits){0}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING("x.v:1:22: error: nesting deeper than 32768\n", res.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(res.ast));
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    vt_free(&res);
    odin3_strbuf_free(&text);
}

/* A right-deep conditional chain also meets the AST cap before Bison's stack cap (§5): the
 * second TERNARY from the outside is the first past 32768 high. */
static void test_right_deep_conditional_past_the_depth_cap(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    append(&text, HEAD);
    for (uint32_t i = 0; i < DEPTH_CHAIN; i++) {
        append(&text, "a ? b : ");
    }
    append(&text, "c");
    append(&text, TAIL);
    vt_result res;
    (void)parse_with_limits(text.data, (odin3_vp_limits){0}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING("x.v:1:30: error: nesting deeper than 32768\n", res.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(res.ast));
    vt_free(&res);
    odin3_strbuf_free(&text);
}

/* --- OOM ----------------------------------------------------------------------------------------
 */

/* Every expression action, with attributes, empty system arguments and a hierarchical call. */
static const char OOM_TEXT[] =
    "module m (input [7:0] a, b, output [7:0] y);\n"
    "  assign y = - (* k *) a + (* j = 1 *) b * $signed(a) ? {2{a[1:0], b[3]}} : "
    "f(a, u.w[1]) ** (a ~^ 'sd5);\n"
    "  assign y = $display(\"x\", , g[0].h(b)) | {a, b} >>> a[b+:2] - 1.5;\n"
    "endmodule\n";

/* A parse that failed under the allocation hook: NO_MEMORY with nothing left behind. */
static void expect_oom_failure(odin3_status st, const vt_result *res, const odin3_vl_run *run) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_UINT32(0, res->unit.v);
    TEST_ASSERT_TRUE(run->stop);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(res->ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root_count(res->ast));
}

/* One step: the limit-th allocation fails; a failure is NO_MEMORY with nothing left behind. */
static odin3_status oom_step(long limit) {
    vt_result res;
    fresh_store(&res);
    odin3_vp_stream stream;
    vt_stream(res.design, "x.v", odin3_bytes_cstr(OOM_TEXT), &stream);
    odin3_vl_run run;
    odin3_vl_run_init(&run, res.design);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_verilog_parse_stream(&run, res.ast, &stream, &res.unit);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        expect_oom_failure(st, &res, &run);
    } else {
        TEST_ASSERT_NOT_EQUAL(0, res.unit.v);
    }
    vt_free(&res);
    return st;
}

static void test_expression_oom_sweep(void) {
    long limit = 0;
    while (limit < EXPR_OOM_LIMIT && oom_step(limit) != ODIN3_OK) {
        limit++;
    }
    TEST_ASSERT_TRUE(limit > 0);
    TEST_ASSERT_TRUE(limit < EXPR_OOM_LIMIT);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_multiplication_binds_tighter);
    RUN_TEST(test_power_is_left_associative);
    RUN_TEST(test_unary_binds_to_a_primary);
    RUN_TEST(test_conditional_is_right_associative);
    RUN_TEST(test_relational_before_equality);
    RUN_TEST(test_bitwise_levels);
    RUN_TEST(test_shifts_and_arithmetic);
    RUN_TEST(test_xnor_spellings_are_one_sub);
    RUN_TEST(test_reduction_operators);
    RUN_TEST(test_doubled_unary_is_a_syntax_error);
    RUN_TEST(test_strict_forms_are_syntax_errors);
    RUN_TEST(test_attributes_on_operators);
    RUN_TEST(test_nested_operator_attributes);
    RUN_TEST(test_concatenation_and_replication);
    RUN_TEST(test_replication_locations);
    RUN_TEST(test_selects);
    RUN_TEST(test_hierarchical_names);
    RUN_TEST(test_user_calls);
    RUN_TEST(test_system_calls);
    RUN_TEST(test_system_call_empty_arguments);
    RUN_TEST(test_user_call_empty_argument_is_a_syntax_error);
    RUN_TEST(test_hierarchical_calls);
    RUN_TEST(test_hierarchical_call_with_a_backtick);
    RUN_TEST(test_hierarchical_reference_with_a_backtick);
    RUN_TEST(test_literals);
    RUN_TEST(test_number_forms);
    RUN_TEST(test_parentheses);
    RUN_TEST(test_parenthesised_operand_locations);
    RUN_TEST(test_expressions_in_declarations);
    RUN_TEST(test_long_chain_keeps_the_stack_flat);
    RUN_TEST(test_chain_past_the_depth_cap);
    RUN_TEST(test_right_deep_conditional_past_the_depth_cap);
    RUN_TEST(test_expression_oom_sweep);
    return UNITY_END();
}
