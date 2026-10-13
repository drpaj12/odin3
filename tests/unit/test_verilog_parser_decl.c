/*
 * test_verilog_parser_decl.c — the shapes 2C Task 4's grammar builds (spec §3.6 "Units and
 * directives", "Modules and ports", "Parameters", "Nets and variables", "Continuous assignment";
 * the 2A slot table §4.3), each by odin3_ast_dump against a literal string, plus the printer's
 * round trip of the header forms.
 */
#include "ast/ast.h"
#include "ast/print.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "verilog_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint32_t WHOLE = UINT32_MAX; /* the node itself, not one of its children */

void setUp(void) {
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

/* Parses src as x.v; it must succeed with no error logged. */
static void parse_ok(const char *src, vt_result *res) {
    vt_parse("x.v", src, res);
    TEST_ASSERT_EQUAL_STRING("", res->log.data != NULL ? res->log.data : "");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res->status);
}

static void expect_node(const vt_result *res, odin3_ast_id node, const char *want) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(res->ast, node, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data != NULL ? out.data : "");
    odin3_strbuf_free(&out);
}

/* The dump of the UNIT's child unit_child (WHOLE: the UNIT). */
static void expect_unit(const char *src, uint32_t unit_child, const char *want) {
    vt_result res;
    parse_ok(src, &res);
    expect_node(&res,
                unit_child == WHOLE ? res.unit : odin3_ast_child(res.ast, res.unit, unit_child),
                want);
    vt_free(&res);
}

/* The UNIT's first MODULE. */
static odin3_ast_id first_module(const vt_result *res) {
    for (uint32_t i = 0; i < odin3_ast_nchild(res->ast, res->unit); i++) {
        odin3_ast_id child = odin3_ast_child(res->ast, res->unit, i);
        if (odin3_ast_kind_of(res->ast, child) == ODIN3_AST_MODULE) {
            return child;
        }
    }
    TEST_FAIL_MESSAGE("no MODULE");
    return (odin3_ast_id){0};
}

/* The dump of the first module's child item (WHOLE: the MODULE). */
static void expect_item(const char *src, uint32_t item, const char *want) {
    vt_result res;
    parse_ok(src, &res);
    odin3_ast_id module = first_module(&res);
    expect_node(&res, item == WHOLE ? module : odin3_ast_child(res.ast, module, item), want);
    vt_free(&res);
}

/* The printer's text of the UNIT's child unit_child. */
static void expect_printed(const char *src, uint32_t unit_child, const char *want) {
    vt_result res;
    parse_ok(src, &res);
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_print(res.ast, odin3_ast_child(res.ast, res.unit, unit_child), &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data);
    odin3_strbuf_free(&out);
    vt_free(&res);
}

/* src must fail with status and exactly the log want (each line ending in '\n'). */
static void expect_error(const char *src, odin3_status status, const char *want) {
    vt_result res;
    vt_parse("x.v", src, &res);
    TEST_ASSERT_EQUAL_INT(status, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING(want, res.log.data != NULL ? res.log.data : "");
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    vt_free(&res);
}

/* --- modules and their headers ------------------------------------------------------------ */

static void test_ports_absent_vs_empty(void) {
    expect_item("module m; endmodule", WHOLE, "(MODULE:m @x.v:1:1\n  _\n  _)");
    expect_item("module m (); endmodule", WHOLE, "(MODULE:m @x.v:1:1\n  _\n  (LIST @x.v:1:10))");
    expect_printed("module m; endmodule", 0, "module m;\nendmodule\n");
    expect_printed("module m (); endmodule", 0, "module m ();\nendmodule\n");
}

static void test_empty_parameter_list(void) {
    expect_item("module m #() (); endmodule", WHOLE,
                "(MODULE:m @x.v:1:1\n  (LIST @x.v:1:10)\n  (LIST @x.v:1:14))");
    expect_printed("module m #() (); endmodule", 0, "module m #() ();\nendmodule\n");
}

static void test_macromodule(void) {
    expect_item("macromodule m; endmodule", WHOLE, "(MODULE:m [MACROMODULE] @x.v:1:1\n  _\n  _)");
    expect_printed("macromodule m; endmodule", 0, "macromodule m;\nendmodule\n");
}

/* Three ANSI nodes: two declarators share the first, a reg with a sign and an initialiser, an
 * inout with nothing written (NET 0 = unspecified, 1 + the net kind otherwise). */
static void test_ansi_ports(void) {
    expect_item("module m (input wire [3:0] a, b, output reg signed [7:0] q = 0, inout c); "
                "endmodule",
                1,
                "(LIST @x.v:1:10\n"
                "  (PORT_DECL.input [IN_HEADER NET=1] @x.v:1:11\n"
                "    (RANGE @x.v:1:22\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:23 3)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:25 0))\n"
                "    (DECLARATOR:a @x.v:1:28\n"
                "      _)\n"
                "    (DECLARATOR:b @x.v:1:31\n"
                "      _))\n"
                "  (PORT_DECL.output [SIGNED IN_HEADER DT=1] @x.v:1:34\n"
                "    (RANGE @x.v:1:52\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:53 7)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:55 0))\n"
                "    (DECLARATOR:q @x.v:1:58\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:62 0)))\n"
                "  (PORT_DECL.inout [IN_HEADER] @x.v:1:65\n"
                "    _\n"
                "    (DECLARATOR:c @x.v:1:71\n"
                "      _)))");
}

static void test_unsigned_sets_no_flag(void) {
    expect_item("module m (input unsigned [3:0] a); endmodule", 1,
                "(LIST @x.v:1:10\n"
                "  (PORT_DECL.input [IN_HEADER] @x.v:1:11\n"
                "    (RANGE @x.v:1:26\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:27 3)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:29 0))\n"
                "    (DECLARATOR:a @x.v:1:32\n"
                "      _)))");
}

/* Six PORT_REFs in order; the trailing empty port sits one past its comma. */
static void test_nonansi_ports(void) {
    expect_item("module m (a, .x(e), .y(), {b, c}, d[3:0], ); endmodule", 1,
                "(LIST @x.v:1:10\n"
                "  (PORT_REF:a @x.v:1:11\n"
                "    _)\n"
                "  (PORT_REF:x [DOTTED] @x.v:1:14\n"
                "    (IDENT:e @x.v:1:17))\n"
                "  (PORT_REF:y [DOTTED] @x.v:1:21\n"
                "    _)\n"
                "  (PORT_REF @x.v:1:27\n"
                "    (CONCAT @x.v:1:27\n"
                "      (IDENT:b @x.v:1:28)\n"
                "      (IDENT:c @x.v:1:31)))\n"
                "  (PORT_REF @x.v:1:35\n"
                "    (SELECT.part @x.v:1:35\n"
                "      (IDENT:d @x.v:1:35)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:37 3)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:39 0)))\n"
                "  (PORT_REF @x.v:1:42\n"
                "    _))");
}

/* Only the last non-ANSI port may be empty (a trailing comma): Yosys 0.55 rejects an empty port
 * anywhere else with a syntax error (PR 2D-3 final review m5). */
static void test_empty_port_not_last_is_a_syntax_error(void) {
    expect_error("module m (, a); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:11: error: syntax: unexpected ','\n");
    expect_error("module n (a, , b); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:14: error: syntax: unexpected ',', expecting identifier or ')' or '.' or "
                 "'{'\n");
    expect_error("module n (,); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:11: error: syntax: unexpected ','\n");
}

/* A body port declaration is an item (IN_HEADER clear); input reg / input integer keep DT. */
static void test_body_port_declarations(void) {
    const char *src = "module m (a, r, n);\n  input [3:0] a;\n  input reg r;\n  input integer n;\n"
                      "endmodule";
    expect_item(src, 2,
                "(PORT_DECL.input @x.v:2:3\n"
                "  (RANGE @x.v:2:9\n"
                "    (NUMBER.dec [SIGNED] @x.v:2:10 3)\n"
                "    (NUMBER.dec [SIGNED] @x.v:2:12 0))\n"
                "  (DECLARATOR:a @x.v:2:15\n"
                "    _))");
    expect_item(src, 3,
                "(PORT_DECL.input [DT=1] @x.v:3:3\n  _\n  (DECLARATOR:r @x.v:3:13\n    _))");
    expect_item(src, 4,
                "(PORT_DECL.input [DT=2] @x.v:4:3\n  _\n  (DECLARATOR:n @x.v:4:17\n    _))");
}

/* --- parameters ------------------------------------------------------------------------------ */

static void test_parameter_item(void) {
    expect_item("module m; parameter [3:0] A = 1, B = 2; endmodule", 2,
                "(PARAM_DECL.parameter @x.v:1:11\n"
                "  (RANGE @x.v:1:21\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:22 3)\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:24 0))\n"
                "  (DECLARATOR:A @x.v:1:27\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:31 1))\n"
                "  (DECLARATOR:B @x.v:1:34\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:38 2)))");
}

static void test_header_parameters(void) {
    expect_item("module m #(parameter A = 1, localparam B = 2, parameter integer C = 3); endmodule",
                0,
                "(LIST @x.v:1:10\n"
                "  (PARAM_DECL.parameter [IN_HEADER] @x.v:1:12\n"
                "    _\n"
                "    (DECLARATOR:A @x.v:1:22\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:26 1)))\n"
                "  (PARAM_DECL.localparam [IN_HEADER] @x.v:1:29\n"
                "    _\n"
                "    (DECLARATOR:B @x.v:1:40\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:44 2)))\n"
                "  (PARAM_DECL.parameter [IN_HEADER DT=2] @x.v:1:47\n"
                "    _\n"
                "    (DECLARATOR:C @x.v:1:65\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:69 3))))");
}

static void test_defparam_pairs(void) {
    const char *src = "module m; defparam a.b.W = 8, c.W = 9; endmodule";
    expect_item(src, 2,
                "(DEFPARAM @x.v:1:20\n"
                "  (HIER_NAME @x.v:1:20\n"
                "    (IDENT:a @x.v:1:20)\n"
                "    (IDENT:b @x.v:1:22)\n"
                "    (IDENT:W @x.v:1:24))\n"
                "  (NUMBER.dec [SIGNED] @x.v:1:28 8))");
    expect_item(src, 3,
                "(DEFPARAM @x.v:1:31\n"
                "  (HIER_NAME @x.v:1:31\n"
                "    (IDENT:c @x.v:1:31)\n"
                "    (IDENT:W @x.v:1:33))\n"
                "  (NUMBER.dec [SIGNED] @x.v:1:37 9))");
}

static void test_specparam(void) {
    expect_item("module m; specparam d = 1; endmodule", 2,
                "(PARAM_DECL.specparam @x.v:1:11\n"
                "  _\n"
                "  (DECLARATOR:d @x.v:1:21\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:25 1)))");
}

/* --- nets and variables ---------------------------------------------------------------------- */

static void test_net_strength_delay_initialiser(void) {
    expect_item("module m; wire (strong0, pull1) #5 w = a; endmodule", 2,
                "(NET_DECL.wire @x.v:1:11\n"
                "  (STRENGTH.drive [S0=4 S1=3] @x.v:1:16)\n"
                "  _\n"
                "  (DELAY @x.v:1:33\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:34 5))\n"
                "  (DECLARATOR:w @x.v:1:36\n"
                "    (IDENT:a @x.v:1:40)))");
}

static void test_trireg_charge(void) {
    expect_item("module m; trireg (small) t; endmodule", 2,
                "(NET_DECL.trireg @x.v:1:11\n"
                "  (STRENGTH.charge [S0=1] @x.v:1:18)\n"
                "  _\n"
                "  _\n"
                "  (DECLARATOR:t @x.v:1:26\n"
                "    _))");
}

static void test_vectored_scalared(void) {
    const char *src = "module m; wire vectored [3:0] v; wire scalared s; endmodule";
    expect_item(src, 2,
                "(NET_DECL.wire [VECTORED] @x.v:1:11\n"
                "  _\n"
                "  (RANGE @x.v:1:25\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:26 3)\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:28 0))\n"
                "  _\n"
                "  (DECLARATOR:v @x.v:1:31\n"
                "    _))");
    expect_item(src, 3,
                "(NET_DECL.wire [SCALARED] @x.v:1:34\n"
                "  _\n"
                "  _\n"
                "  _\n"
                "  (DECLARATOR:s @x.v:1:48\n"
                "    _))");
}

/* Unpacked dimensions outermost first; an ascending range stays as written. */
static void test_dims_and_ascending_range(void) {
    expect_item("module m; reg m [0:7][0:3]; endmodule", 2,
                "(VAR_DECL.reg @x.v:1:11\n"
                "  _\n"
                "  (DECLARATOR:m @x.v:1:15\n"
                "    _\n"
                "    (RANGE @x.v:1:17\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:18 0)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:20 7))\n"
                "    (RANGE @x.v:1:22\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:23 0)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:25 3))))");
    expect_item("module m; wire [0:3] w; endmodule", 2,
                "(NET_DECL.wire @x.v:1:11\n"
                "  _\n"
                "  (RANGE @x.v:1:16\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:17 0)\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:19 3))\n"
                "  _\n"
                "  (DECLARATOR:w @x.v:1:22\n"
                "    _))");
}

static void test_variables_events_genvars(void) {
    const char *src =
        "module m; integer i; time t; real r; realtime rt; event e; genvar g, h; endmodule";
    expect_item(src, 2, "(VAR_DECL.integer @x.v:1:11\n  _\n  (DECLARATOR:i @x.v:1:19\n    _))");
    expect_item(src, 3, "(VAR_DECL.time @x.v:1:22\n  _\n  (DECLARATOR:t @x.v:1:27\n    _))");
    expect_item(src, 4, "(VAR_DECL.real @x.v:1:30\n  _\n  (DECLARATOR:r @x.v:1:35\n    _))");
    expect_item(src, 5, "(VAR_DECL.realtime @x.v:1:38\n  _\n  (DECLARATOR:rt @x.v:1:47\n    _))");
    expect_item(src, 6, "(EVENT_DECL @x.v:1:51\n  (DECLARATOR:e @x.v:1:57\n    _))");
    expect_item(src, 7,
                "(GENVAR_DECL @x.v:1:60\n"
                "  (DECLARATOR:g @x.v:1:67\n"
                "    _)\n"
                "  (DECLARATOR:h @x.v:1:70\n"
                "    _))");
}

/* --- continuous assignment ------------------------------------------------------------------- */

static void test_assign_strength_delay_lvalues(void) {
    expect_item("module m; assign (strong0, strong1) #(1, 2) a = b, {c, d[1], e[3:0]} = f; "
                "endmodule",
                2,
                "(CONT_ASSIGN @x.v:1:11\n"
                "  (STRENGTH.drive [S0=4 S1=4] @x.v:1:18)\n"
                "  (DELAY @x.v:1:37\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:39 1)\n"
                "    (NUMBER.dec [SIGNED] @x.v:1:42 2))\n"
                "  (NET_ASSIGN @x.v:1:45\n"
                "    (IDENT:a @x.v:1:45)\n"
                "    (IDENT:b @x.v:1:49))\n"
                "  (NET_ASSIGN @x.v:1:52\n"
                "    (CONCAT @x.v:1:52\n"
                "      (IDENT:c @x.v:1:53)\n"
                "      (SELECT.bit @x.v:1:56\n"
                "        (IDENT:d @x.v:1:56)\n"
                "        (NUMBER.dec [SIGNED] @x.v:1:58 1)\n"
                "        _)\n"
                "      (SELECT.part @x.v:1:62\n"
                "        (IDENT:e @x.v:1:62)\n"
                "        (NUMBER.dec [SIGNED] @x.v:1:64 3)\n"
                "        (NUMBER.dec [SIGNED] @x.v:1:66 0)))\n"
                "    (IDENT:f @x.v:1:72)))");
}

/* A hierarchical lvalue with an indexed component, a part-select width and a mintypmax delay. */
static void test_assign_hierarchical_and_mintypmax(void) {
    expect_item("module m; assign #(1:2:3) g[0].u.w[1+:2] = x; endmodule", 2,
                "(CONT_ASSIGN @x.v:1:11\n"
                "  _\n"
                "  (DELAY @x.v:1:18\n"
                "    (MINTYPMAX @x.v:1:20\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:20 1)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:22 2)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:24 3)))\n"
                "  (NET_ASSIGN @x.v:1:27\n"
                "    (SELECT.part_plus @x.v:1:27\n"
                "      (HIER_NAME @x.v:1:27\n"
                "        (SELECT.bit @x.v:1:27\n"
                "          (IDENT:g @x.v:1:27)\n"
                "          (NUMBER.dec [SIGNED] @x.v:1:29 0)\n"
                "          _)\n"
                "        (IDENT:u @x.v:1:32)\n"
                "        (IDENT:w @x.v:1:34))\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:36 1)\n"
                "      (NUMBER.dec [SIGNED] @x.v:1:39 2))\n"
                "    (IDENT:x @x.v:1:44)))");
}

/* The lvalue grammar has no operator (§3.1 d): the error is at the `+`. */
static void test_assign_to_an_expression_is_a_syntax_error(void) {
    expect_error("module m; assign a + b = c; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:20: error: syntax: unexpected '+', expecting '.' or '=' or '['\n");
}

/* --- stray `;` module items (PHASE2 #16) -------------------------------------------------- */

/* A source with stray `;`s, the same text without them (laid out so every node keeps its
 * location) and the exact log the source gives. */
typedef struct stray_case {
    const char *src;
    const char *plain;
    const char *want_log;
} stray_case;

/* sc.src parses to sc.plain's tree, logging exactly sc.want_log. */
static void expect_same_tree(stray_case sc) {
    vt_result want;
    parse_ok(sc.plain, &want);
    vt_result got;
    vt_parse("x.v", sc.src, &got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, got.status);
    TEST_ASSERT_EQUAL_UINT32(0, got.errors);
    TEST_ASSERT_EQUAL_STRING(sc.want_log, got.log.data != NULL ? got.log.data : "");
    TEST_ASSERT_EQUAL_STRING(want.dump.data, got.dump.data);
    vt_free(&got);
    vt_free(&want);
}

static void test_stray_semicolon_is_an_empty_item(void) {
    expect_same_tree((stray_case){"module m;\n  wire a;;\n  wire b;\nendmodule\n",
                                  "module m;\n  wire a;\n  wire b;\nendmodule\n",
                                  "x.v:2:10: warning: empty module item ';' ignored\n"});
    expect_same_tree((stray_case){"module m;\n  wire a;\n  ;\n  wire b;\nendmodule\n",
                                  "module m;\n  wire a;\n   \n  wire b;\nendmodule\n",
                                  "x.v:3:3: warning: empty module item ';' ignored\n"});
    /* Right after the header, and twice in a row: one warning each. */
    expect_same_tree((stray_case){"module m(a);;; input a;\nendmodule\n",
                                  "module m(a);   input a;\nendmodule\n",
                                  "x.v:1:13: warning: empty module item ';' ignored\n"
                                  "x.v:1:14: warning: empty module item ';' ignored\n"});
    /* module_items also lists a generate region's and a generate block's items (Yosys too). */
    expect_same_tree(
        (stray_case){"module m;\ngenerate ; if (1) begin wire w;; end endgenerate\nendmodule\n",
                     "module m;\ngenerate   if (1) begin wire w;  end endgenerate\nendmodule\n",
                     "x.v:2:10: warning: empty module item ';' ignored\n"
                     "x.v:2:32: warning: empty module item ';' ignored\n"});
}

/* Only a module item may be empty: a `;` at unit level, in a port list or in a declarator list
 * stays a syntax error. */
static void test_stray_semicolon_elsewhere_is_a_syntax_error(void) {
    expect_error("; module m; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:1: error: syntax: unexpected ';'\n");
    expect_error("module m (a;); endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:12: error: syntax: unexpected ';', expecting ')' or ',' or '.' or '['\n");
    expect_error("module m; wire a,; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:18: error: syntax: unexpected ';', expecting identifier\n");
}

/* --- units, directives, opaque bodies ------------------------------------------------------- */

static void test_directives(void) {
    const char *src = "`timescale 1ns/1ps\nmodule m;\n  wire a;\n  `default_nettype none\n"
                      "  wire b;\nendmodule\n";
    expect_unit(src, 0, "(DIRECTIVE.timescale:1ns/1ps @x.v:1:1)");
    expect_item(src, 3, "(DIRECTIVE.default_nettype:none @x.v:4:3)");
    expect_unit("`celldefine\n", 0, "(DIRECTIVE.celldefine @x.v:1:1)");
}

static void test_opaque_units_and_specify(void) {
    const char *src = "primitive p (y, a); output y; input a; table 0 : 1; endtable endprimitive\n"
                      "config c; design work.m; endconfig\n"
                      "module m; specify (a => y) = 1; endspecify endmodule\n";
    expect_unit(src, 0,
                "(UDP_DECL:p @x.v:1:1 \"primitive p (y, a); output y; input a; table 0 : 1; "
                "endtable endprimitive\")");
    expect_unit(src, 1, "(CONFIG_DECL:c @x.v:2:1 \"config c; design work.m; endconfig\")");
    vt_result res;
    parse_ok(src, &res);
    odin3_ast_id module = odin3_ast_child(res.ast, res.unit, 2);
    expect_node(&res, odin3_ast_child(res.ast, module, 2),
                "(SPECIFY_BLOCK @x.v:3:11 \"specify (a => y) = 1; endspecify\")");
    vt_free(&res);
}

/* --- attributes ------------------------------------------------------------------------------ */

static void test_attributes_merge_and_attach(void) {
    const char *src = "(* a *) (* b = 1 *) module m; (* c *) wire w; endmodule";
    expect_unit(src, 0,
                "(MODULE:m @x.v:1:21\n"
                "  _\n"
                "  _\n"
                "  (NET_DECL.wire @x.v:1:39\n"
                "    _\n"
                "    _\n"
                "    _\n"
                "    (DECLARATOR:w @x.v:1:44\n"
                "      _)\n"
                "    {(ATTR:c @x.v:1:34\n"
                "       _)})\n"
                "  {(ATTR:a @x.v:1:4\n"
                "     _)\n"
                "   (ATTR:b @x.v:1:12\n"
                "     (NUMBER.dec [SIGNED] @x.v:1:16 1))})");
}

static void test_attributes_on_header_entries(void) {
    expect_item("module m #((* k *) parameter A = 1) ((* k *) input a); endmodule", WHOLE,
                "(MODULE:m @x.v:1:1\n"
                "  (LIST @x.v:1:10\n"
                "    (PARAM_DECL.parameter [IN_HEADER] @x.v:1:20\n"
                "      _\n"
                "      (DECLARATOR:A @x.v:1:30\n"
                "        (NUMBER.dec [SIGNED] @x.v:1:34 1))\n"
                "      {(ATTR:k @x.v:1:15\n"
                "         _)}))\n"
                "  (LIST @x.v:1:37\n"
                "    (PORT_DECL.input [IN_HEADER] @x.v:1:46\n"
                "      _\n"
                "      (DECLARATOR:a @x.v:1:52\n"
                "        _)\n"
                "      {(ATTR:k @x.v:1:41\n"
                "         _)})))");
}

static void test_attribute_on_a_declarator_is_a_syntax_error(void) {
    expect_error("module m; wire a, (* k *) b; endmodule", ODIN3_ERR_PARSE,
                 "x.v:1:19: error: syntax: unexpected (*, expecting identifier\n");
}

/* "loc end" of node, each as file:line:col. */
static void expect_range(const vt_result *res, odin3_ast_id node, const char *want) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(res->design, &sm));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format(sm, odin3_ast_loc(res->ast, node), &text,
                                                        ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(" ")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format(sm, odin3_ast_end(res->ast, node), &text,
                                                        ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING(want, text.data);
    odin3_strbuf_free(&text);
}

/* "loc end" of the first module's child index. */
static void expect_item_range(const char *src, uint32_t index, const char *want) {
    vt_result res;
    parse_ok(src, &res);
    expect_range(&res, odin3_ast_child(res.ast, first_module(&res), index), want);
    vt_free(&res);
}

/* Point (g) / review I2: an item's node starts at its own first token, attributes or not, and
 * ends one past its ';'. */
static void test_item_range_ignores_its_attributes(void) {
    expect_item_range("module m; wire a; (* k *) wire b; endmodule", 3, "x.v:1:27 x.v:1:34");
    expect_item_range("module m; wire a; wire b; endmodule", 3, "x.v:1:19 x.v:1:26");
}

/* Minor 5 of the review: the ends of header groups and LISTs (the dump shows starts only). */
static void test_header_group_and_list_ends(void) {
    vt_result res;
    parse_ok("module m #(parameter A = 1, B = 2) (input a, output reg b, c, inout d); endmodule",
             &res);
    odin3_ast_id module = first_module(&res);
    odin3_ast_id params = odin3_ast_child(res.ast, module, 0);
    odin3_ast_id ports = odin3_ast_child(res.ast, module, 1);
    expect_range(&res, params, "x.v:1:10 x.v:1:35");
    expect_range(&res, odin3_ast_child(res.ast, params, 0), "x.v:1:12 x.v:1:34");
    expect_range(&res, ports, "x.v:1:36 x.v:1:71");
    expect_range(&res, odin3_ast_child(res.ast, ports, 1), "x.v:1:46 x.v:1:61");
    vt_free(&res);
}

/* --- numbers --------------------------------------------------------------------------------- */

/* Blanks between a size and its base make the sized literal 5'b1 outside a delay. */
static void test_spaced_sized_literal(void) {
    expect_item("module m; assign x = 5 'b1; endmodule", 2,
                "(CONT_ASSIGN @x.v:1:11\n"
                "  _\n"
                "  _\n"
                "  (NET_ASSIGN @x.v:1:18\n"
                "    (IDENT:x @x.v:1:18)\n"
                "    (NUMBER.bin @x.v:1:22 5'b1)))");
}

/* The empty string (controller ruling): a STRING flagged EMPTY with name 0, printed `""`, and the
 * printed text re-parses to an equal tree (2A §4.3 amendment). */
static void test_empty_string(void) {
    expect_item("module m; parameter p = \"\"; endmodule", 2,
                "(PARAM_DECL.parameter @x.v:1:11\n"
                "  _\n"
                "  (DECLARATOR:p @x.v:1:21\n"
                "    (STRING:\"\" [EMPTY] @x.v:1:25)))");
    expect_printed("module m; parameter p = \"\"; endmodule", 0,
                   "module m;\n  parameter p = \"\";\nendmodule\n");
    vt_result first;
    vt_result again;
    parse_ok("module m; parameter p = \"\"; endmodule", &first);
    parse_ok("module m;\n  parameter p = \"\";\nendmodule\n", &again);
    bool equal = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_equal(first.ast, first_module(&first), again.ast,
                                                    first_module(&again), &equal));
    TEST_ASSERT_TRUE(equal);
    vt_free(&again);
    vt_free(&first);
}

/* Review Focus 4: words reserved only in later standards are identifiers. */
static void test_later_keywords_are_names(void) {
    expect_item("module m; wire logic, bit, int; endmodule", 2,
                "(NET_DECL.wire @x.v:1:11\n"
                "  _\n"
                "  _\n"
                "  _\n"
                "  (DECLARATOR:logic @x.v:1:16\n"
                "    _)\n"
                "  (DECLARATOR:bit @x.v:1:23\n"
                "    _)\n"
                "  (DECLARATOR:int @x.v:1:28\n"
                "    _))");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ports_absent_vs_empty);
    RUN_TEST(test_empty_parameter_list);
    RUN_TEST(test_macromodule);
    RUN_TEST(test_ansi_ports);
    RUN_TEST(test_unsigned_sets_no_flag);
    RUN_TEST(test_nonansi_ports);
    RUN_TEST(test_empty_port_not_last_is_a_syntax_error);
    RUN_TEST(test_body_port_declarations);
    RUN_TEST(test_parameter_item);
    RUN_TEST(test_header_parameters);
    RUN_TEST(test_defparam_pairs);
    RUN_TEST(test_specparam);
    RUN_TEST(test_net_strength_delay_initialiser);
    RUN_TEST(test_trireg_charge);
    RUN_TEST(test_vectored_scalared);
    RUN_TEST(test_dims_and_ascending_range);
    RUN_TEST(test_variables_events_genvars);
    RUN_TEST(test_assign_strength_delay_lvalues);
    RUN_TEST(test_assign_hierarchical_and_mintypmax);
    RUN_TEST(test_assign_to_an_expression_is_a_syntax_error);
    RUN_TEST(test_stray_semicolon_is_an_empty_item);
    RUN_TEST(test_stray_semicolon_elsewhere_is_a_syntax_error);
    RUN_TEST(test_directives);
    RUN_TEST(test_opaque_units_and_specify);
    RUN_TEST(test_attributes_merge_and_attach);
    RUN_TEST(test_attributes_on_header_entries);
    RUN_TEST(test_attribute_on_a_declarator_is_a_syntax_error);
    RUN_TEST(test_item_range_ignores_its_attributes);
    RUN_TEST(test_spaced_sized_literal);
    RUN_TEST(test_header_group_and_list_ends);
    RUN_TEST(test_empty_string);
    RUN_TEST(test_later_keywords_are_names);
    return UNITY_END();
}
