/*
 * test_verilog_parser_stmt.c — the statement grammar of 2C Task 6 (spec §3.1 (e), (f), (h), (j),
 * §3.3 dangling else, §3.5 metacomments, §3.6 "Procedural blocks", "if, case", "Loops",
 * "Functions and tasks"; the 2A slot table §4.3): each case parsed inside `module m; … endmodule`,
 * its tree compared with a literal string, and its module printed, re-parsed and compared with
 * odin3_ast_equal (the round trip of every case). Also the located errors of the reordering
 * reductions, the syntax errors the grammar's shape gives, `statement: error ';'` recovery and an
 * OOM sweep over the statement actions.
 *
 * The literal trees are odin3_ast_dump output with the locations removed and each line break
 * (with its indent) turned into one blank: `(KIND.sub:name [FLAGS] children {attributes})`.
 * Where a location is the point of the test, it is asserted on the dump's first line.
 */
#include "ast/ast.h"
#include "ast/print.h"
#include "frontends/verilog/parser.h"
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
#include <stdio.h>
#include <string.h>

enum {
    MODULE_SLOTS = 2,       /* MODULE's params and ports slots; its items follow */
    STMT_OOM_LIMIT = 16384, /* allocations of the sweep text, with headroom */
    STMT_OOM_MESSAGE = 64   /* the sweep's count line */
};

static const char HEAD[] = "module m;\n";
static const char TAIL[] = "\nendmodule\n";

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

/* HEAD body TAIL into text (initialised; the caller frees it): the body is line 2. */
static void module_text(const char *body, odin3_strbuf *text) {
    odin3_strbuf_init(text);
    append(text, HEAD);
    append(text, body);
    append(text, TAIL);
}

/* The UNIT's MODULE (its first child). */
static odin3_ast_id module_of(const vt_result *res) {
    odin3_ast_id module = odin3_ast_child(res->ast, res->unit, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_MODULE, odin3_ast_kind_of(res->ast, module));
    return module;
}

/* The module's first item. */
static odin3_ast_id item_of(const vt_result *res) {
    odin3_ast_id module = module_of(res);
    TEST_ASSERT_TRUE(odin3_ast_nchild(res->ast, module) > MODULE_SLOTS);
    return odin3_ast_child(res->ast, module, MODULE_SLOTS);
}

/* The statement of the module's first item (an `initial`). */
static odin3_ast_id stmt_of(const vt_result *res) {
    odin3_ast_id initial = item_of(res);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_INITIAL, odin3_ast_kind_of(res->ast, initial));
    return odin3_ast_child(res->ast, initial, 0);
}

/* The dump of node without its locations, one line (see the file comment). */
static void squashed(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    odin3_strbuf dump;
    odin3_strbuf_init(&dump);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(ast, node, &dump));
    odin3_strbuf_init(out);
    const char *at = dump.data;
    while (*at != '\0') {
        if (at[0] == ' ' && at[1] == '@') { /* ` @file:line:col` */
            at += 2;
            at += strcspn(at, " ()\n");
        } else if (at[0] == '\n') {
            at += 1 + strspn(at + 1, " ");
            append(out, " ");
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at, 1}));
            at++;
        }
    }
    odin3_strbuf_free(&dump);
}

/* The first line of node's dump: its head with its location. */
static void expect_head(const vt_result *res, odin3_ast_id node, const char *want) {
    odin3_strbuf dump;
    odin3_strbuf_init(&dump);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(res->ast, node, &dump));
    size_t len = strcspn(dump.data, "\n");
    TEST_ASSERT_EQUAL_STRING_LEN(want, dump.data, len);
    TEST_ASSERT_EQUAL_size_t(strlen(want), len);
    odin3_strbuf_free(&dump);
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

/* The body parses with the log want_log and its module round-trips; the result stays for further
 * checks (the caller frees it). */
static void parse_body(const char *body, vt_result *res, const char *want_log) {
    odin3_strbuf text;
    module_text(body, &text);
    parse_logged(text.data, want_log, res);
    expect_round_trip(res, want_log);
    odin3_strbuf_free(&text);
}

/* node's squashed dump is tree (source: the text it was parsed from, for the message). */
static void expect_node(const vt_result *res, odin3_ast_id node, const char *tree,
                        const char *source) {
    odin3_strbuf got;
    squashed(res->ast, node, &got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(tree, got.data, source);
    odin3_strbuf_free(&got);
}

/* check's informational line for unreachable non-root nodes (none: the log stays empty). */
static const char *unreachable_log(uint32_t unreachable) {
    return unreachable == 0 ? "" : "ast check: rule 3: 1 unreachable non-root nodes\n";
}

/* `initial <stmt>` parses (check reporting unreachable nodes, rule 3: none or 1) to the
 * statement tree and round-trips. */
static void expect_stmt_logged(const char *stmt, uint32_t unreachable, const char *tree) {
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    append(&body, "initial ");
    append(&body, stmt);
    vt_result res;
    parse_body(body.data, &res, unreachable_log(unreachable));
    expect_node(&res, stmt_of(&res), tree, stmt);
    vt_free(&res);
    odin3_strbuf_free(&body);
}

static void expect_stmt(const char *stmt, const char *tree) {
    expect_stmt_logged(stmt, 0, tree);
}

/* The module item body parses to the item tree and round-trips. */
static void expect_item(const char *body, const char *tree) {
    vt_result res;
    parse_body(body, &res, "");
    expect_node(&res, item_of(&res), tree, body);
    vt_free(&res);
}

/* `initial <ctrl> x = y;`: the timing statement's control has the tree. */
static void expect_control(const char *ctrl, const char *tree) {
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    append(&body, "initial ");
    append(&body, ctrl);
    append(&body, " x = y;");
    vt_result res;
    parse_body(body.data, &res, "");
    odin3_ast_id timing = stmt_of(&res);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_TIMING_STMT, odin3_ast_kind_of(res.ast, timing));
    expect_node(&res, odin3_ast_child(res.ast, timing, 0), tree, ctrl);
    vt_free(&res);
    odin3_strbuf_free(&body);
}

/* The body must fail with PARSE, no UNIT and exactly the log want. */
static void expect_error(const char *body, const char *want) {
    odin3_strbuf text;
    module_text(body, &text);
    vt_result res;
    vt_parse("x.v", text.data, &res);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, res.status, body);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, res.log.data != NULL ? res.log.data : "", body);
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    vt_free(&res);
    odin3_strbuf_free(&text);
}

/* --- case (§3.6 "if, case"; 2A §4.2 convention 4) ------------------------------------------- */

/* The body precedes the match expressions; `default` without a colon has an empty tail; `2: ;`
 * has a NULL_STMT body. */
static void test_case_items(void) {
    expect_stmt("case (s) 0, 1: a = 1; default b = 0; 2: ; endcase",
                "(CASE.case (IDENT:s)"
                " (CASE_ITEM (BLOCKING_ASSIGN (IDENT:a) _ (NUMBER.dec [SIGNED] 1))"
                " (NUMBER.dec [SIGNED] 0) (NUMBER.dec [SIGNED] 1))"
                " (CASE_ITEM [DEFAULT] (BLOCKING_ASSIGN (IDENT:b) _ (NUMBER.dec [SIGNED] 0)))"
                " (CASE_ITEM (NULL_STMT) (NUMBER.dec [SIGNED] 2)))");
    expect_stmt("casez (s) default: ; endcase",
                "(CASE.casez (IDENT:s) (CASE_ITEM [DEFAULT] (NULL_STMT)))");
    expect_stmt("casex (s) a ? b : c, d: ; endcase",
                "(CASE.casex (IDENT:s) (CASE_ITEM (NULL_STMT)"
                " (TERNARY (IDENT:a) (IDENT:b) (IDENT:c)) (IDENT:d)))");
}

/* A CASE_ITEM is located at its first match expression or at `default`. */
static void test_case_item_locations(void) {
    vt_result res;
    parse_body("initial case (s) p: ; default ; endcase", &res, "");
    odin3_ast_id kase = stmt_of(&res);
    expect_head(&res, kase, "(CASE.case @x.v:2:9");
    expect_head(&res, odin3_ast_child(res.ast, kase, 1), "(CASE_ITEM @x.v:2:18");
    expect_head(&res, odin3_ast_child(res.ast, kase, 2), "(CASE_ITEM [DEFAULT] @x.v:2:23");
    vt_free(&res);
}

/* An empty case and an attribute on a case item are syntax errors (A.6.7: case_item+). */
static void test_case_syntax_errors(void) {
    expect_error("initial case (s) endcase", "x.v:2:18: error: syntax: unexpected endcase\n");
    expect_error("initial case (s) (* a *) 0: ; endcase",
                 "x.v:2:18: error: syntax: unexpected (*\n");
}

/* --- functions and tasks (§3.2 FUNCTION_DECL/TASK_DECL, §3.6) --------------------------------- */

/* Interleaved body items: ports LIST (a, b), locals LIST (t), one statement; each LIST at its
 * first element. */
static void test_function_interleaved_items(void) {
    vt_result res;
    parse_body("function f; input a; reg t; input b; begin t = a; f = t; end endfunction", &res,
               "");
    odin3_ast_id fn = item_of(&res);
    expect_node(&res, fn,
                "(FUNCTION_DECL:f _"
                " (LIST (PORT_DECL.input _ (DECLARATOR:a _)) (PORT_DECL.input _ (DECLARATOR:b _)))"
                " (LIST (VAR_DECL.reg _ (DECLARATOR:t _)))"
                " (SEQ_BLOCK _ (BLOCKING_ASSIGN (IDENT:t) _ (IDENT:a))"
                " (BLOCKING_ASSIGN (IDENT:f) _ (IDENT:t))))",
                "");
    expect_head(&res, fn, "(FUNCTION_DECL:f @x.v:2:1");
    expect_head(&res, odin3_ast_child(res.ast, fn, 1), "(LIST @x.v:2:13");
    expect_head(&res, odin3_ast_child(res.ast, fn, 2), "(LIST @x.v:2:22");
    vt_free(&res);
}

/* An ANSI header: PORT_DECL{IN_HEADER} entries, a bare name joining the open port; the empty
 * locals LIST sits one past the header's `;` (zero width). */
static void test_function_ansi_header(void) {
    vt_result res;
    parse_body("function [3:0] f (input [3:0] x, y, input integer n); f = x; endfunction", &res,
               "");
    odin3_ast_id fn = item_of(&res);
    expect_node(&res, fn,
                "(FUNCTION_DECL:f (RANGE (NUMBER.dec [SIGNED] 3) (NUMBER.dec [SIGNED] 0))"
                " (LIST (PORT_DECL.input [IN_HEADER] (RANGE (NUMBER.dec [SIGNED] 3)"
                " (NUMBER.dec [SIGNED] 0)) (DECLARATOR:x _) (DECLARATOR:y _))"
                " (PORT_DECL.input [IN_HEADER DT=2] _ (DECLARATOR:n _)))"
                " (LIST)"
                " (BLOCKING_ASSIGN (IDENT:f) _ (IDENT:x)))",
                "");
    expect_head(&res, odin3_ast_child(res.ast, fn, 1), "(LIST @x.v:2:19");
    odin3_ast_id locals = odin3_ast_child(res.ast, fn, 2);
    expect_head(&res, locals, "(LIST @x.v:2:54)");
    TEST_ASSERT_EQUAL_UINT32(odin3_ast_loc(res.ast, locals).v, odin3_ast_end(res.ast, locals).v);
    vt_free(&res);
}

/* The return types and flags: automatic, signed with a range, integer, real (K3, K8). */
static void test_function_return_types(void) {
    expect_item("function automatic integer f (input integer n); f = n; endfunction",
                "(FUNCTION_DECL:f [AUTOMATIC DT=2] _"
                " (LIST (PORT_DECL.input [IN_HEADER DT=2] _ (DECLARATOR:n _))) (LIST)"
                " (BLOCKING_ASSIGN (IDENT:f) _ (IDENT:n)))");
    expect_item("function signed [7:0] g; input a; g = a; endfunction",
                "(FUNCTION_DECL:g [SIGNED] (RANGE (NUMBER.dec [SIGNED] 7) (NUMBER.dec [SIGNED] 0))"
                " (LIST (PORT_DECL.input _ (DECLARATOR:a _))) (LIST)"
                " (BLOCKING_ASSIGN (IDENT:g) _ (IDENT:a)))");
    expect_item("function real h; input a; h = a; endfunction",
                "(FUNCTION_DECL:h [DT=3] _"
                " (LIST (PORT_DECL.input _ (DECLARATOR:a _))) (LIST)"
                " (BLOCKING_ASSIGN (IDENT:h) _ (IDENT:a)))");
}

/* A function body is exactly one statement (Annex A.2.6); a declaration after it is an error. */
static void test_function_statement_count(void) {
    expect_error("function f; input a; endfunction",
                 "x.v:2:1: error: function 'f' has no statement\n");
    expect_error("function f; input a; f = a; f = 1; endfunction",
                 "x.v:2:29: error: function 'f' has more than one statement\n");
    expect_error("function f; input a; f = a; reg t; endfunction",
                 "x.v:2:29: error: declaration after a statement in block\n");
    expect_error("task t; a = 1; b = 2; endtask",
                 "x.v:2:16: error: task 't' has more than one statement\n");
    expect_error("task t; a = 1; input b; endtask",
                 "x.v:2:16: error: declaration after a statement in block\n");
}

/* A task without a statement gets a NULL_STMT; empty LISTs and that NULL_STMT have zero width:
 * the LISTs one past the header's `;`, the statement one past the last body item. */
static void test_task_without_statement(void) {
    vt_result res;
    parse_body("task t; endtask", &res, "");
    odin3_ast_id task = item_of(&res);
    expect_node(&res, task, "(TASK_DECL:t (LIST) (LIST) (NULL_STMT))", "");
    expect_head(&res, odin3_ast_child(res.ast, task, 0), "(LIST @x.v:2:8)");
    expect_head(&res, odin3_ast_child(res.ast, task, 1), "(LIST @x.v:2:8)");
    expect_head(&res, odin3_ast_child(res.ast, task, 2), "(NULL_STMT @x.v:2:8)");
    vt_free(&res);
    parse_body("task automatic t; input a; endtask", &res, "");
    task = item_of(&res);
    expect_node(&res, task,
                "(TASK_DECL:t [AUTOMATIC] (LIST (PORT_DECL.input _ (DECLARATOR:a _))) (LIST)"
                " (NULL_STMT))",
                "");
    expect_head(&res, odin3_ast_child(res.ast, task, 1), "(LIST @x.v:2:18)");
    odin3_ast_id null = odin3_ast_child(res.ast, task, 2);
    expect_head(&res, null, "(NULL_STMT @x.v:2:27)");
    TEST_ASSERT_EQUAL_UINT32(odin3_ast_loc(res.ast, null).v, odin3_ast_end(res.ast, null).v);
    vt_free(&res);
    expect_item("task t; ; endtask", "(TASK_DECL:t (LIST) (LIST) (NULL_STMT))");
}

/* §3.5: an attribute on an ANSI function or task port is not supported (a syntax error). */
static void test_attribute_on_header_port(void) {
    expect_error("function f ((* k *) input a); f = a; endfunction",
                 "x.v:2:13: error: syntax: unexpected (*, expecting inout or input or output\n");
}

static void test_task_ansi_header(void) {
    expect_item("task t (input a, output [3:0] b); b = a; endtask",
                "(TASK_DECL:t (LIST (PORT_DECL.input [IN_HEADER] _ (DECLARATOR:a _))"
                " (PORT_DECL.output [IN_HEADER] (RANGE (NUMBER.dec [SIGNED] 3)"
                " (NUMBER.dec [SIGNED] 0)) (DECLARATOR:b _))) (LIST)"
                " (BLOCKING_ASSIGN (IDENT:b) _ (IDENT:a)))");
}

/* --- blocks (§3.1 e, §3.2 SEQ_BLOCK/PAR_BLOCK) ------------------------------------------------ */

/* One mixed item list: attributes start a declaration and a statement alike. */
static void test_block_attributes_on_both(void) {
    expect_stmt("begin (* a *) reg x; (* b *) x = 1; end",
                "(SEQ_BLOCK (LIST (VAR_DECL.reg _ (DECLARATOR:x _) {(ATTR:a _)}))"
                " (BLOCKING_ASSIGN (IDENT:x) _ (NUMBER.dec [SIGNED] 1) {(ATTR:b _)}))");
}

static void test_declaration_after_statement(void) {
    expect_error("initial begin x = 1; reg y; end",
                 "x.v:2:22: error: declaration after a statement in block\n");
}

/* Nested blocks and cases: the scratch vectors serve one reduction at a time. */
static void test_nested_blocks_and_cases(void) {
    expect_stmt("begin : outer integer i; begin : inner reg q; q = 1; end"
                " case (s) 0: case (t) 1: begin reg r; r = 1; end endcase default ; endcase"
                " i = 2; end",
                "(SEQ_BLOCK:outer (LIST (VAR_DECL.integer _ (DECLARATOR:i _)))"
                " (SEQ_BLOCK:inner (LIST (VAR_DECL.reg _ (DECLARATOR:q _)))"
                " (BLOCKING_ASSIGN (IDENT:q) _ (NUMBER.dec [SIGNED] 1)))"
                " (CASE.case (IDENT:s) (CASE_ITEM (CASE.case (IDENT:t) (CASE_ITEM"
                " (SEQ_BLOCK (LIST (VAR_DECL.reg _ (DECLARATOR:r _)))"
                " (BLOCKING_ASSIGN (IDENT:r) _ (NUMBER.dec [SIGNED] 1)))"
                " (NUMBER.dec [SIGNED] 1))) (NUMBER.dec [SIGNED] 0))"
                " (CASE_ITEM [DEFAULT] (NULL_STMT)))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 2)))");
}

/* A block's locals LIST sits at its first declaration; a block without one has none. */
static void test_block_locals_location(void) {
    vt_result res;
    parse_body("initial begin : b reg x; integer i; end", &res, "");
    odin3_ast_id block = stmt_of(&res);
    expect_head(&res, block, "(SEQ_BLOCK:b @x.v:2:9");
    expect_head(&res, odin3_ast_child(res.ast, block, 0), "(LIST @x.v:2:19");
    vt_free(&res);
    expect_stmt("begin end", "(SEQ_BLOCK _)");
}

static void test_fork_join(void) {
    expect_stmt("fork a = 1; join",
                "(PAR_BLOCK _ (BLOCKING_ASSIGN (IDENT:a) _ (NUMBER.dec [SIGNED] 1)))");
    expect_stmt("fork : p reg r; r = 1; join",
                "(PAR_BLOCK:p (LIST (VAR_DECL.reg _ (DECLARATOR:r _)))"
                " (BLOCKING_ASSIGN (IDENT:r) _ (NUMBER.dec [SIGNED] 1)))");
}

/* A directive is not a statement or a block item. */
static void test_directive_in_a_block(void) {
    expect_error("initial begin `celldefine x = 1; end",
                 "x.v:2:15: error: syntax: unexpected directive\n");
}

/* --- timing (§2.7, §3.1 f, §3.6 "Procedural blocks and timing") ------------------------------ */

static void test_event_lists(void) {
    expect_control("@(posedge clk or negedge rst)",
                   "(EVENT_CONTROL _ (EVENT_EXPR.posedge (IDENT:clk))"
                   " (EVENT_EXPR.negedge [AFTER_OR] (IDENT:rst)))");
    expect_control("@(a, b or c)",
                   "(EVENT_CONTROL _ (EVENT_EXPR.none (IDENT:a)) (EVENT_EXPR.none (IDENT:b))"
                   " (EVENT_EXPR.none [AFTER_OR] (IDENT:c)))");
    expect_control("@clk", "(EVENT_CONTROL [NO_PARENS] _ (EVENT_EXPR.none (IDENT:clk)))");
    expect_control("@u.e", "(EVENT_CONTROL [NO_PARENS] _"
                           " (EVENT_EXPR.none (HIER_NAME (IDENT:u) (IDENT:e))))");
}

/* The implicit event: the one-token forms (§2.7) and the three a comment defeats. */
static void test_implicit_event_forms(void) {
    static const char *const FORMS[] = {"@*",           "@(*)",        "@( * )",     "@ *",
                                        "@( /*c*/ * )", "@(* /*c*/ )", "@( /*c*/ *)"};
    for (size_t i = 0; i < sizeof FORMS / sizeof FORMS[0]; i++) {
        expect_control(FORMS[i], "(EVENT_CONTROL [STAR] _)");
    }
}

/* TIMING_STMT and EVENT_CONTROL sit at the `@` or `#`, an EVENT_EXPR at its edge keyword. */
static void test_timing_locations(void) {
    vt_result res;
    parse_body("initial @(posedge c or d) x = y;", &res, "");
    odin3_ast_id timing = stmt_of(&res);
    expect_head(&res, timing, "(TIMING_STMT @x.v:2:9");
    odin3_ast_id control = odin3_ast_child(res.ast, timing, 0);
    expect_head(&res, control, "(EVENT_CONTROL @x.v:2:9");
    expect_head(&res, odin3_ast_child(res.ast, control, 1), "(EVENT_EXPR.posedge @x.v:2:11");
    expect_head(&res, odin3_ast_child(res.ast, control, 2),
                "(EVENT_EXPR.none [AFTER_OR] @x.v:2:24");
    vt_free(&res);
}

static void test_delays_and_intra_assignment_controls(void) {
    expect_stmt("#5;", "(TIMING_STMT (DELAY (NUMBER.dec [SIGNED] 5)) _)");
    expect_stmt("#5 (* a *) ;",
                "(TIMING_STMT (DELAY (NUMBER.dec [SIGNED] 5)) (NULL_STMT {(ATTR:a _)}))");
    expect_stmt("#(5) x = 1;", "(TIMING_STMT (DELAY (NUMBER.dec [SIGNED] 5))"
                               " (BLOCKING_ASSIGN (IDENT:x) _ (NUMBER.dec [SIGNED] 1)))");
    expect_stmt(
        "#(1:2:3) x = 1;",
        "(TIMING_STMT (DELAY (MINTYPMAX (NUMBER.dec [SIGNED] 1) (NUMBER.dec [SIGNED] 2)"
        " (NUMBER.dec [SIGNED] 3))) (BLOCKING_ASSIGN (IDENT:x) _ (NUMBER.dec [SIGNED] 1)))");
    expect_stmt("a = #1 b;",
                "(BLOCKING_ASSIGN (IDENT:a) (DELAY (NUMBER.dec [SIGNED] 1)) (IDENT:b))");
    expect_stmt("a <= @(posedge c) b;",
                "(NONBLOCKING_ASSIGN (IDENT:a) (EVENT_CONTROL _ (EVENT_EXPR.posedge (IDENT:c)))"
                " (IDENT:b))");
    expect_stmt("a <= repeat (2) @(posedge c) b;",
                "(NONBLOCKING_ASSIGN (IDENT:a) (EVENT_CONTROL (NUMBER.dec [SIGNED] 2)"
                " (EVENT_EXPR.posedge (IDENT:c))) (IDENT:b))");
    expect_stmt("@(e);", "(TIMING_STMT (EVENT_CONTROL _ (EVENT_EXPR.none (IDENT:e))) _)");
}

/* A delay value is a number, a real, an identifier or a parenthesised expression (§3.1 f):
 * `#a-b x;` is `#a` followed by `-b x;`, which no statement starts with. */
static void test_delay_value_is_not_an_expression(void) {
    expect_error("initial #a-b x;", "x.v:2:11: error: syntax: unexpected '-'\n");
    expect_error("initial #(1, 2) x = 1;", "x.v:2:12: error: syntax: unexpected ','\n");
}

/* --- assignments, task enables and the rest (§3.1 d, h, j) ----------------------------------- */

/* `<=` after an lvalue is the nonblocking assignment, inside an expression the comparison. */
static void test_lvalue_versus_comparison(void) {
    expect_stmt("q <= a & b;", "(NONBLOCKING_ASSIGN (IDENT:q) _ (BINARY.& (IDENT:a) (IDENT:b)))");
    expect_stmt(
        "if (a <= b) q <= c;",
        "(IF (BINARY.<= (IDENT:a) (IDENT:b)) (NONBLOCKING_ASSIGN (IDENT:q) _ (IDENT:c)) _)");
    expect_stmt(
        "{a, b[1]} = c;",
        "(BLOCKING_ASSIGN (CONCAT (IDENT:a) (SELECT.bit (IDENT:b) (NUMBER.dec [SIGNED] 1) _))"
        " _ (IDENT:c))");
    expect_error("initial a + b = c;", "x.v:2:11: error: syntax: unexpected '+'\n");
}

static void test_task_enables(void) {
    expect_stmt("t;", "(TASK_CALL:t)");
    expect_stmt("t(a, b);", "(TASK_CALL:t (IDENT:a) (IDENT:b))");
    expect_stmt("$display(\"x\", , a);",
                "(TASK_CALL:$display [SYSTEM] (STRING:\"x\") _ (IDENT:a))");
    expect_stmt("$finish;", "(TASK_CALL:$finish [SYSTEM])");
    expect_stmt("$finish();", "(TASK_CALL:$finish [SYSTEM])");
    expect_stmt("$display(, a);", "(TASK_CALL:$display [SYSTEM] _ (IDENT:a))");
    expect_error("initial t();", "x.v:2:11: error: syntax: unexpected ')'\n");
}

/* A hierarchical task enable: CALL{HIER}'s name rule; the prefix components stay unreachable
 * (2A §4.1, check rule 3 information: one root, `g[0]`'s IDENT and index being its children). */
static void test_hierarchical_task_enables(void) {
    expect_stmt_logged("u.t(a);", 1, "(TASK_CALL:u.t [HIER] (IDENT:a))");
    expect_stmt_logged("u.t;", 1, "(TASK_CALL:u.t [HIER])");
    expect_stmt_logged("g[0].t;", 1, "(TASK_CALL:g[0].t [HIER])");
}

static void test_dangling_else_binds_inner(void) {
    expect_stmt("if (a) if (b) x = 1; else y = 1;",
                "(IF (IDENT:a) (IF (IDENT:b) (BLOCKING_ASSIGN (IDENT:x) _ (NUMBER.dec [SIGNED] 1))"
                " (BLOCKING_ASSIGN (IDENT:y) _ (NUMBER.dec [SIGNED] 1))) _)");
    expect_stmt("if (a) ; else if (b) ; else ;",
                "(IF (IDENT:a) (NULL_STMT) (IF (IDENT:b) (NULL_STMT) (NULL_STMT)))");
}

static void test_procedural_continuous_assignments(void) {
    expect_stmt("assign a = b;", "(PROC_CONT_ASSIGN.assign (IDENT:a) (IDENT:b))");
    expect_stmt("deassign a;", "(PROC_CONT_ASSIGN.deassign (IDENT:a) _)");
    expect_stmt(
        "force a[1] = b;",
        "(PROC_CONT_ASSIGN.force (SELECT.bit (IDENT:a) (NUMBER.dec [SIGNED] 1) _) (IDENT:b))");
    expect_stmt("release a;", "(PROC_CONT_ASSIGN.release (IDENT:a) _)");
}

static void test_wait_disable_trigger(void) {
    expect_stmt("wait (e) ;", "(WAIT (IDENT:e) _)");
    expect_stmt("wait (e) x = 1;",
                "(WAIT (IDENT:e) (BLOCKING_ASSIGN (IDENT:x) _ (NUMBER.dec [SIGNED] 1)))");
    expect_stmt("disable b;", "(DISABLE (HIER_NAME (IDENT:b)))");
    expect_stmt("disable u.b;", "(DISABLE (HIER_NAME (IDENT:u) (IDENT:b)))");
    expect_stmt("-> e;", "(EVENT_TRIGGER (HIER_NAME (IDENT:e)))");
}

static void test_loops(void) {
    expect_stmt("for (i = 0; i < 4; i = i + 1) x = i;",
                "(FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
                " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 4))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (BLOCKING_ASSIGN (IDENT:x) _ (IDENT:i)))");
    expect_stmt("while (a) a = 0;",
                "(WHILE (IDENT:a) (BLOCKING_ASSIGN (IDENT:a) _ (NUMBER.dec [SIGNED] 0)))");
    expect_stmt("repeat (2) ;", "(REPEAT (NUMBER.dec [SIGNED] 2) (NULL_STMT))");
    expect_stmt("forever #5 c = ~c;", "(FOREVER (TIMING_STMT (DELAY (NUMBER.dec [SIGNED] 5))"
                                      " (BLOCKING_ASSIGN (IDENT:c) _ (UNARY.~ (IDENT:c)))))");
}

/* ALWAYS and INITIAL take one statement (not a null one, A.6.2), with its attributes. */
static void test_always_and_initial(void) {
    expect_item("always @(posedge c) q <= d;",
                "(ALWAYS.always (TIMING_STMT (EVENT_CONTROL _ (EVENT_EXPR.posedge (IDENT:c)))"
                " (NONBLOCKING_ASSIGN (IDENT:q) _ (IDENT:d))))");
    expect_item("(* k *) always (* s *) begin end",
                "(ALWAYS.always (SEQ_BLOCK _ {(ATTR:s _)}) {(ATTR:k _)})");
    expect_error("initial ;", "x.v:2:9: error: syntax: unexpected ';'\n"); /* A.6.2: a statement */
}

/* --- metacomments (§2.6, §3.5) ---------------------------------------------------------------- */

static void test_metacomment_case(void) {
    expect_stmt("case (s) // synopsys full_case parallel_case\n  0: ;\nendcase",
                "(CASE.case (IDENT:s) (CASE_ITEM (NULL_STMT) (NUMBER.dec [SIGNED] 0)) "
                "{(ATTR:full_case [METACOMMENT] _) (ATTR:parallel_case [METACOMMENT] _)})");
    expect_stmt("casez (s) /* synthesis parallel_case full_case */ 0: ; endcase",
                "(CASE.casez (IDENT:s) (CASE_ITEM (NULL_STMT) (NUMBER.dec [SIGNED] 0)) "
                "{(ATTR:full_case [METACOMMENT] _) (ATTR:parallel_case [METACOMMENT] _)})");
}

/* `(* full_case *)` and the metacomments: one attach of three, the statement's first. */
static void test_metacomment_merges_with_attributes(void) {
    vt_result res;
    parse_body("initial (* full_case *) case (s) // synopsys full_case parallel_case\n"
               "  0: ;\nendcase",
               &res, "");
    odin3_ast_id kase = stmt_of(&res);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_ast_attrs(res.ast, kase).n);
    expect_node(&res, kase,
                "(CASE.case (IDENT:s) (CASE_ITEM (NULL_STMT) (NUMBER.dec [SIGNED] 0)) "
                "{(ATTR:full_case _) (ATTR:full_case [METACOMMENT] _)"
                " (ATTR:parallel_case [METACOMMENT] _)})",
                "");
    expect_head(&res, odin3_ast_attrs(res.ast, kase).ids[1],
                "(ATTR:full_case [METACOMMENT] @x.v:2:34");
    vt_free(&res);
}

/* The inner case reduces first and takes only its own window. */
static void test_metacomment_nested_cases(void) {
    vt_result res;
    parse_body("initial case (s) // synopsys parallel_case\n"
               "  0: case (t) // synopsys full_case\n"
               "       1: ;\n"
               "     endcase\n"
               "endcase",
               &res, "");
    odin3_ast_id outer = stmt_of(&res);
    odin3_ast_span outer_attrs = odin3_ast_attrs(res.ast, outer);
    TEST_ASSERT_EQUAL_UINT32(1, outer_attrs.n);
    expect_node(&res, outer_attrs.ids[0], "(ATTR:parallel_case [METACOMMENT] _)", "");
    odin3_ast_id inner = odin3_ast_child(res.ast, odin3_ast_child(res.ast, outer, 1), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_CASE, odin3_ast_kind_of(res.ast, inner));
    odin3_ast_span inner_attrs = odin3_ast_attrs(res.ast, inner);
    TEST_ASSERT_EQUAL_UINT32(1, inner_attrs.n);
    expect_node(&res, inner_attrs.ids[0], "(ATTR:full_case [METACOMMENT] _)", "");
    vt_free(&res);
}

/* A metacomment between items attaches nowhere: it stays a flagged comment. */
static void test_metacomment_between_items(void) {
    vt_result res;
    parse_body("initial case (s)\n  0: ;\n  // synopsys full_case\n  1: ;\nendcase", &res, "");
    odin3_ast_id kase = stmt_of(&res);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(res.ast, kase).n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(res.ast));
    odin3_range all = {odin3_ast_loc(res.ast, res.unit), odin3_ast_end(res.ast, res.unit)};
    odin3_ast_comment_span comments = odin3_ast_comments_in(res.ast, all);
    TEST_ASSERT_EQUAL_UINT32(1, comments.n);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_COMMENT_METACOMMENT,
                             comments.items[0].flags & ODIN3_AST_COMMENT_METACOMMENT);
    vt_free(&res);
}

/* The printed metacomment (`// synopsys …` after the header) re-parses equal (§3.7): every
 * expect_* round-trips; here the printed text itself is pinned. */
static void test_metacomment_printed_form(void) {
    vt_result res;
    parse_body("initial (* full_case *) case (s) /* synopsys parallel_case */ 0: ; endcase", &res,
               "");
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res.ast, stmt_of(&res), &printed));
    TEST_ASSERT_EQUAL_STRING("(* full_case *) case (s) // synopsys parallel_case\n"
                             "  0: ;\n"
                             "endcase\n",
                             printed.data);
    odin3_strbuf_free(&printed);
    vt_free(&res);
}

/* --- recovery (§3.4) -------------------------------------------------------------------------- */

/* `statement: error ';'` resynchronises inside an always; the next statement's error is still
 * reported. */
static void test_statement_recovery(void) {
    expect_error("always begin\n"
                 "    x = = 1;\n"
                 "    y = 2;\n"
                 "    z = = 3;\n"
                 "  end",
                 "x.v:3:9: error: syntax: unexpected '='\n"
                 "x.v:5:9: error: syntax: unexpected '='\n");
    expect_error("always @(posedge c) q <= <= d;\n"
                 "always @(posedge c) r <= <= d;",
                 "x.v:2:26: error: syntax: unexpected <=\n"
                 "x.v:3:26: error: syntax: unexpected <=\n");
}

/* --- OOM -------------------------------------------------------------------------------------- */

/* Every statement action: functions and tasks, blocks with locals, every control and statement
 * kind, a metacomment case with attributes. */
static const char OOM_TEXT[] =
    "module m (input clk, a, b, output reg q);\n"
    "  function automatic integer f (input integer n); integer k; begin k = n; f = k; end\n"
    "  endfunction\n"
    "  task t; input a; reg r; r = a; endtask\n"
    "  always @(posedge clk or negedge a) begin : blk reg [1:0] v;\n"
    "    (* k *) v = #1 a; q <= repeat (2) @(posedge clk) b;\n"
    "    (* full_case *) case (v) // synopsys full_case parallel_case\n"
    "      0, 1: q = 1; default ; endcase\n"
    "    if (a) q = 0; else q = 1;\n"
    "    for (v = 0; v < 2; v = v + 1) q = v;\n"
    "    while (a) q = 0; repeat (2) q = 1; forever #5 q = 0; wait (a); disable blk; -> e;\n"
    "    t(a); u.t(b); $display(\"x\", , a); assign q = a; deassign q; force q = a; release q;\n"
    "    fork q = 1; join @*; @clk q = 1; #5 (* n *) ;\n"
    "  end\n"
    "  initial #5;\n"
    "endmodule\n";

static void fresh_store(vt_result *res) {
    memset(res, 0, sizeof *res);
    odin3_strbuf_init(&res->dump);
    odin3_strbuf_init(&res->log);
    res->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(res->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(res->design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &res->ast));
}

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

static void test_statement_oom_sweep(void) {
    long limit = 0;
    while (limit < STMT_OOM_LIMIT && oom_step(limit) != ODIN3_OK) {
        limit++;
    }
    char text[STMT_OOM_MESSAGE];
    (void)snprintf(text, sizeof text, "%ld allocations swept", limit);
    TEST_MESSAGE(text);
    TEST_ASSERT_TRUE(limit > 0);
    TEST_ASSERT_TRUE(limit < STMT_OOM_LIMIT);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_case_items);
    RUN_TEST(test_case_item_locations);
    RUN_TEST(test_case_syntax_errors);
    RUN_TEST(test_function_interleaved_items);
    RUN_TEST(test_function_ansi_header);
    RUN_TEST(test_function_return_types);
    RUN_TEST(test_function_statement_count);
    RUN_TEST(test_task_without_statement);
    RUN_TEST(test_task_ansi_header);
    RUN_TEST(test_attribute_on_header_port);
    RUN_TEST(test_block_attributes_on_both);
    RUN_TEST(test_declaration_after_statement);
    RUN_TEST(test_nested_blocks_and_cases);
    RUN_TEST(test_block_locals_location);
    RUN_TEST(test_fork_join);
    RUN_TEST(test_directive_in_a_block);
    RUN_TEST(test_event_lists);
    RUN_TEST(test_implicit_event_forms);
    RUN_TEST(test_timing_locations);
    RUN_TEST(test_delays_and_intra_assignment_controls);
    RUN_TEST(test_delay_value_is_not_an_expression);
    RUN_TEST(test_lvalue_versus_comparison);
    RUN_TEST(test_task_enables);
    RUN_TEST(test_hierarchical_task_enables);
    RUN_TEST(test_dangling_else_binds_inner);
    RUN_TEST(test_procedural_continuous_assignments);
    RUN_TEST(test_wait_disable_trigger);
    RUN_TEST(test_loops);
    RUN_TEST(test_always_and_initial);
    RUN_TEST(test_metacomment_case);
    RUN_TEST(test_metacomment_merges_with_attributes);
    RUN_TEST(test_metacomment_nested_cases);
    RUN_TEST(test_metacomment_between_items);
    RUN_TEST(test_metacomment_printed_form);
    RUN_TEST(test_statement_recovery);
    RUN_TEST(test_statement_oom_sweep);
    return UNITY_END();
}
