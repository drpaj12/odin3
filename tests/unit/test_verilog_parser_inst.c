/*
 * test_verilog_parser_inst.c — the instantiation, gate and generate grammar of 2C Task 7 (spec
 * §3.1 (g), (i), (k), §3.3 dangling else, §3.5, §3.6 "Generate", "Instantiation", "Gates"; the
 * 2A slot table §4.3): each case parsed inside `module m; … endmodule`, its tree compared with a
 * literal string, and its module printed, re-parsed and compared with odin3_ast_equal (the round
 * trip of every case). Also the syntax errors the grammar's shape gives, recovery at the generate
 * closers and an OOM sweep over the Task 7 actions.
 *
 * The literal trees are odin3_ast_dump output with the locations removed and each line break
 * (with its indent) turned into one blank: `(KIND.sub:name [FLAGS] children {attributes})`.
 * Where a location is the point of the test, it is asserted on the dump's first line.
 */
#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "ast/srcfmt.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/parser_test.h"
#include "ir/check.h"
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
    INST_OOM_LIMIT = 16384, /* allocations of the sweep text, with headroom */
    OOM_INSTANCES = 40,     /* instances and connections: store and scratch growth */
    INST_OOM_MESSAGE = 64   /* the sweep's count line */
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

/* The UNIT's first MODULE. */
static odin3_ast_id module_of(const vt_result *res) {
    odin3_ast_id module = odin3_ast_child(res->ast, res->unit, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_MODULE, odin3_ast_kind_of(res->ast, module));
    return module;
}

/* The module's index-th item. */
static odin3_ast_id item_at(const vt_result *res, uint32_t index) {
    odin3_ast_id module = module_of(res);
    TEST_ASSERT_TRUE(odin3_ast_nchild(res->ast, module) > MODULE_SLOTS + index);
    return odin3_ast_child(res->ast, module, MODULE_SLOTS + index);
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

/* Parses text as x.v; it must succeed with no log. */
static void parse_clean(const char *text, vt_result *res) {
    vt_parse("x.v", text, res);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", res->log.data != NULL ? res->log.data : "", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res->status);
}

/* res's module prints, the text re-parses and the two modules are equal. */
static void expect_round_trip(const vt_result *res) {
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res->ast, module_of(res), &printed));
    vt_result again;
    parse_clean(printed.data, &again);
    bool equal = false;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_equal(res->ast, module_of(res), again.ast, module_of(&again), &equal));
    TEST_ASSERT_TRUE_MESSAGE(equal, printed.data);
    vt_free(&again);
    odin3_strbuf_free(&printed);
}

/* The body parses cleanly and its module round-trips; the result stays for further checks (the
 * caller frees it). */
static void parse_body(const char *body, vt_result *res) {
    odin3_strbuf text;
    module_text(body, &text);
    parse_clean(text.data, res);
    expect_round_trip(res);
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

/* The module item body parses to the item tree (its first item) and round-trips. */
static void expect_item(const char *body, const char *tree) {
    vt_result res;
    parse_body(body, &res);
    expect_node(&res, item_at(&res, 0), tree, body);
    vt_free(&res);
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

static void fresh_store(vt_result *res) {
    memset(res, 0, sizeof *res);
    odin3_strbuf_init(&res->dump);
    odin3_strbuf_init(&res->log);
    res->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(res->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(res->design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &res->ast));
}

/* Parses text as x.v without vt_parse's check (an unnamed module instance fails rule 4 until
 * the pass reports it, §4.2 step 5). */
static void parse_unchecked(const char *text, vt_result *res) {
    fresh_store(res);
    odin3_vp_stream stream;
    vt_stream(res->design, "x.v", odin3_bytes_cstr(text), &stream);
    odin3_vl_run run;
    odin3_vl_run_init(&run, res->design);
    odin3_log_set_sink(vt_sink, res);
    res->status = odin3_verilog_parse_stream(&run, res->ast, &stream, &res->unit);
    odin3_log_set_sink(NULL, NULL);
}

/* --- instantiation (§3.1 i, k; §3.6 "Instantiation") ----------------------------------------- */

/* The brief's statement: parameter CONNECTIONs ordered and named, the per-slot CONNECTION list
 * (ordered, named, `.q()`, an empty slot), the instance array range and zero connections. */
static void test_instantiation_connections(void) {
    vt_result res;
    parse_body("m #(8, .W(4)) u1 (a, .p(b), .q(), , c), u2 [3:0] ();", &res);
    odin3_ast_id inst = item_at(&res, 0);
    expect_node(&res, inst,
                "(INSTANTIATION:m (LIST (CONNECTION (NUMBER.dec [SIGNED] 8))"
                " (CONNECTION:W (NUMBER.dec [SIGNED] 4)))"
                " (INSTANCE:u1 _ (CONNECTION (IDENT:a)) (CONNECTION:p (IDENT:b)) (CONNECTION:q _)"
                " (CONNECTION _) (CONNECTION (IDENT:c)))"
                " (INSTANCE:u2 (RANGE (NUMBER.dec [SIGNED] 3) (NUMBER.dec [SIGNED] 0))))",
                "");
    expect_head(&res, inst, "(INSTANTIATION:m @x.v:2:1");
    odin3_ast_id params = odin3_ast_child(res.ast, inst, 0);
    expect_head(&res, params, "(LIST @x.v:2:3");
    expect_head(&res, odin3_ast_child(res.ast, params, 1), "(CONNECTION:W @x.v:2:8");
    odin3_ast_id u1 = odin3_ast_child(res.ast, inst, 1);
    expect_head(&res, u1, "(INSTANCE:u1 @x.v:2:15");
    expect_head(&res, odin3_ast_child(res.ast, u1, 1), "(CONNECTION @x.v:2:19");
    expect_head(&res, odin3_ast_child(res.ast, u1, 3), "(CONNECTION:q @x.v:2:29");
    odin3_ast_id empty = odin3_ast_child(res.ast, u1, 4);
    expect_head(&res, empty, "(CONNECTION @x.v:2:34"); /* one past the comma */
    TEST_ASSERT_EQUAL_UINT32(odin3_ast_loc(res.ast, empty).v, odin3_ast_end(res.ast, empty).v);
    expect_head(&res, odin3_ast_child(res.ast, inst, 2), "(INSTANCE:u2 @x.v:2:41");
    vt_free(&res);
}

/* §3.1 k: `()` is zero connections, `(,)` two empty ones, `(a,)` a and an empty one, `(, a)` an
 * empty one and a. */
static void test_empty_connection_lists(void) {
    expect_item("m u ();", "(INSTANTIATION:m _ (INSTANCE:u _))");
    expect_item("m u (,);", "(INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION _) (CONNECTION _)))");
    expect_item("m u (a,);",
                "(INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION (IDENT:a)) (CONNECTION _)))");
    expect_item("m u (, a);",
                "(INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION _) (CONNECTION (IDENT:a))))");
    vt_result res;
    parse_body("m u (,);", &res);
    odin3_ast_id inst = odin3_ast_child(res.ast, item_at(&res, 0), 1);
    expect_head(&res, odin3_ast_child(res.ast, inst, 1), "(CONNECTION @x.v:2:6"); /* past `(` */
    expect_head(&res, odin3_ast_child(res.ast, inst, 2), "(CONNECTION @x.v:2:7"); /* past `,` */
    vt_free(&res);
}

/* Ordered and named connections mix in one list (I2: 2D rejects); an expression on any port
 * (I3); `#()` is an empty parameter LIST, printed back as `#()`. */
static void test_mixed_connections_and_expressions(void) {
    expect_item(
        "m u (a, .p(b));",
        "(INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION (IDENT:a)) (CONNECTION:p (IDENT:b))))");
    expect_item("m u (.a(r + 1), .y({s, t}));",
                "(INSTANTIATION:m _ (INSTANCE:u _"
                " (CONNECTION:a (BINARY.+ (IDENT:r) (NUMBER.dec [SIGNED] 1)))"
                " (CONNECTION:y (CONCAT (IDENT:s) (IDENT:t)))))");
    expect_item("m #() u (a);", "(INSTANTIATION:m (LIST) (INSTANCE:u _ (CONNECTION (IDENT:a))))");
    expect_item("m #(.W(8), .D()) u (a), v (b);",
                "(INSTANTIATION:m (LIST (CONNECTION:W (NUMBER.dec [SIGNED] 8)) (CONNECTION:D _))"
                " (INSTANCE:u _ (CONNECTION (IDENT:a))) (INSTANCE:v _ (CONNECTION (IDENT:b))))");
}

/* An instance without a name is parsed for every module name (a UDP instance may be unnamed);
 * the pass enforces rule 4 (§4.2 step 5, Task 8), so check FULL reports it here. */
static void test_unnamed_instance(void) {
    vt_result res;
    odin3_strbuf text;
    module_text("m (a), (b);", &text);
    parse_unchecked(text.data, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    odin3_ast_id inst = item_at(&res, 0);
    expect_node(&res, inst,
                "(INSTANTIATION:m _ (INSTANCE _ (CONNECTION (IDENT:a)))"
                " (INSTANCE _ (CONNECTION (IDENT:b))))",
                "");
    expect_head(&res, odin3_ast_child(res.ast, inst, 1), "(INSTANCE @x.v:2:3");
    odin3_log_set_sink(vt_sink, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_ast_check(res.ast, ODIN3_CHECK_FULL));
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_NOT_NULL(strstr(res.log.data, "INSTANCE under INSTANTIATION without a name"));
    vt_free(&res);
    odin3_strbuf_free(&text);
    module_text("m (a);", &text); /* the brief's form */
    parse_unchecked(text.data, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    expect_node(&res, item_at(&res, 0), "(INSTANTIATION:m _ (INSTANCE _ (CONNECTION (IDENT:a))))",
                "");
    vt_free(&res);
    odin3_strbuf_free(&text);
    /* An unnamed instance of a UDP of the store passes (Q7). */
    vt_parse("x.v",
             "primitive p (y, a); output y; input a; table 0 : 0; endtable endprimitive\n"
             "module m; p (y, a); endmodule\n",
             &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    vt_free(&res);
}

/* §3.1 i: a strength on a module-style instance is a syntax error; §3.5: an attribute on a
 * connection is not supported (no CONNECTION attach in 2A §7). */
static void test_instantiation_syntax_errors(void) {
    expect_error("m (strong0, strong1) u (a);", "x.v:2:4: error: syntax: unexpected strong0\n");
    expect_error("m u (.p((* x *) e));", "x.v:2:9: error: syntax: unexpected (*\n");
    expect_error("m u ((* x *) e);", "x.v:2:6: error: syntax: unexpected (*\n");
    expect_error("m #(1, , 2) u (a);", "x.v:2:8: error: syntax: unexpected ','\n");
}

/* Attributes before an instantiation attach to the INSTANTIATION (§3.5). */
static void test_instantiation_attributes(void) {
    expect_item("(* keep *) (* a = 1 *) m u (x);",
                "(INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION (IDENT:x)))"
                " {(ATTR:keep _) (ATTR:a (NUMBER.dec [SIGNED] 1))})");
}

/* --- gates (§3.1 g; §3.6 "Gates") ------------------------------------------------------------- */

/* Every branch of the expanded gate rule: no strength or delay, a delay, a strength and a delay,
 * two instances with an array range and an unnamed one. */
static void test_gate_branches(void) {
    expect_item("and g1 [3:0] (y, a, b), (z, c, d);",
                "(GATE_DECL.and _ _ (INSTANCE:g1 (RANGE (NUMBER.dec [SIGNED] 3)"
                " (NUMBER.dec [SIGNED] 0)) (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))"
                " (CONNECTION (IDENT:b))) (INSTANCE _ (CONNECTION (IDENT:z)) (CONNECTION (IDENT:c))"
                " (CONNECTION (IDENT:d))))");
    expect_item("and (strong0, strong1) #2 (y, a);",
                "(GATE_DECL.and (STRENGTH.drive [S0=4 S1=4]) (DELAY (NUMBER.dec [SIGNED] 2))"
                " (INSTANCE _ (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))");
    expect_item("nand (weak1, pull0) g (y, a);",
                "(GATE_DECL.nand (STRENGTH.drive [S0=3 S1=2]) _"
                " (INSTANCE:g _ (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))");
    expect_item("and #(1, 2) (y, a);",
                "(GATE_DECL.and _ (DELAY (NUMBER.dec [SIGNED] 1) (NUMBER.dec [SIGNED] 2))"
                " (INSTANCE _ (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))");
    expect_item("and (y, a);",
                "(GATE_DECL.and _ _ (INSTANCE _ (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))");
    expect_item("xor #d g [0:1] (y, a);",
                "(GATE_DECL.xor _ (DELAY (IDENT:d)) (INSTANCE:g (RANGE (NUMBER.dec [SIGNED] 0)"
                " (NUMBER.dec [SIGNED] 1)) (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))");
}

/* buf/not with several outputs (Q2), the tristate gates (Q3), MOS, pass and pull kinds (Q6). */
static void test_gate_kinds(void) {
    expect_item("buf (o1, o2, i);", "(GATE_DECL.buf _ _ (INSTANCE _ (CONNECTION (IDENT:o1))"
                                    " (CONNECTION (IDENT:o2)) (CONNECTION (IDENT:i))))");
    expect_item("bufif0 (o, i, e);", "(GATE_DECL.bufif0 _ _ (INSTANCE _ (CONNECTION (IDENT:o))"
                                     " (CONNECTION (IDENT:i)) (CONNECTION (IDENT:e))))");
    expect_item("nmos (o, i, c);", "(GATE_DECL.nmos _ _ (INSTANCE _ (CONNECTION (IDENT:o))"
                                   " (CONNECTION (IDENT:i)) (CONNECTION (IDENT:c))))");
    expect_item("pullup (strong1) (n);",
                "(GATE_DECL.pullup (STRENGTH.drive [S1=4]) _ (INSTANCE _ (CONNECTION (IDENT:n))))");
    expect_item("pulldown (n);", "(GATE_DECL.pulldown _ _ (INSTANCE _ (CONNECTION (IDENT:n))))");
    expect_item("pulldown (strong0) p (n);", "(GATE_DECL.pulldown (STRENGTH.drive [S0=4]) _"
                                             " (INSTANCE:p _ (CONNECTION (IDENT:n))))");
    expect_item("pullup (pull0, strong1) (n);", "(GATE_DECL.pullup (STRENGTH.drive [S0=3 S1=4]) _"
                                                " (INSTANCE _ (CONNECTION (IDENT:n))))");
}

/* All 26 gate subs parse to their GATE_DECL sub (the printer writes the keyword back). */
static void test_every_gate_keyword(void) {
    static const char *const KEYWORDS[] = {
        "and",     "nand",     "or",       "nor",    "xor",     "xnor",  "buf",
        "not",     "bufif0",   "bufif1",   "notif0", "notif1",  "nmos",  "pmos",
        "cmos",    "rnmos",    "rpmos",    "rcmos",  "tran",    "rtran", "tranif0",
        "tranif1", "rtranif0", "rtranif1", "pullup", "pulldown"};
    TEST_ASSERT_EQUAL_size_t(ODIN3_AST_GATE_DECL_COUNT, sizeof KEYWORDS / sizeof KEYWORDS[0]);
    for (uint32_t i = 0; i < ODIN3_AST_GATE_DECL_COUNT; i++) {
        odin3_strbuf body;
        odin3_strbuf_init(&body);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&body, "%s g (a, b);", KEYWORDS[i]));
        vt_result res;
        parse_body(body.data, &res);
        odin3_ast_id gate = item_at(&res, 0);
        TEST_ASSERT_EQUAL_INT(ODIN3_AST_GATE_DECL, odin3_ast_kind_of(res.ast, gate));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(i, odin3_ast_sub(res.ast, gate), body.data);
        vt_free(&res);
        odin3_strbuf_free(&body);
    }
}

/* Locations: GATE_DECL at its keyword, a named instance at its name, an unnamed one at `(`, a
 * terminal's CONNECTION at its expression; attributes attach to the GATE_DECL. */
static void test_gate_locations_and_attributes(void) {
    vt_result res;
    parse_body("(* k *) or #1 g (y, a), (z, b);", &res);
    odin3_ast_id gate = item_at(&res, 0);
    expect_head(&res, gate, "(GATE_DECL.or @x.v:2:9");
    expect_head(&res, odin3_ast_child(res.ast, gate, 2), "(INSTANCE:g @x.v:2:15");
    odin3_ast_id unnamed = odin3_ast_child(res.ast, gate, 3);
    expect_head(&res, unnamed, "(INSTANCE @x.v:2:25");
    expect_head(&res, odin3_ast_child(res.ast, unnamed, 2), "(CONNECTION @x.v:2:29");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_attrs(res.ast, gate).n);
    vt_free(&res);
}

/* node's end (one past its last byte) is at file:line:col want. */
static void expect_end(const vt_result *res, odin3_ast_id node, const char *want) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(res->design, &sm));
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_srcman_format(sm, odin3_ast_end(res->ast, node), &got, ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING(want, got.data);
    odin3_strbuf_free(&got);
}

/*
 * The ends of the Task 7 nodes (2C-1 review, deferred to Task 9): an INSTANTIATION past its ';',
 * its parameter LIST past ')', an INSTANCE past its ')', a GATE_DECL past its ';', a GENERATE
 * past 'endgenerate', a GEN_BLOCK past its 'end', a GEN_CASE past 'endcase' and a DEFPARAM pair
 * at its value (its statement's ';' belongs to no pair).
 */
static void test_end_locations(void) {
    vt_result res;
    parse_body("m #(1, 2) u (a, b);\n"
               "and g (y, a);\n"
               "generate if (1) begin : blk wire w; end endgenerate\n"
               "case (P) 0: assign y = a; default: assign y = b; endcase\n"
               "defparam u.W = 3, u.V = 4;",
               &res);
    odin3_ast_id inst = item_at(&res, 0);
    expect_end(&res, inst, "x.v:2:20");
    expect_end(&res, odin3_ast_child(res.ast, inst, 0), "x.v:2:10");
    expect_end(&res, odin3_ast_child(res.ast, inst, 1), "x.v:2:19");
    odin3_ast_id gate = item_at(&res, 1);
    expect_end(&res, gate, "x.v:3:14");
    expect_end(&res, odin3_ast_child(res.ast, gate, 2), "x.v:3:13");
    odin3_ast_id gen = item_at(&res, 2);
    expect_end(&res, gen, "x.v:4:52");
    odin3_ast_id gen_if = odin3_ast_child(res.ast, gen, 0);
    expect_end(&res, gen_if, "x.v:4:40");
    expect_end(&res, odin3_ast_child(res.ast, gen_if, 1), "x.v:4:40");
    expect_end(&res, item_at(&res, 3), "x.v:5:57");
    expect_end(&res, item_at(&res, 4), "x.v:6:17");
    expect_head(&res, item_at(&res, 5), "(DEFPARAM @x.v:6:19");
    expect_end(&res, item_at(&res, 5), "x.v:6:26");
    vt_free(&res);
}

/* Gate terminals are never empty (A.3.3), and a pull gate takes no delay (A.3.1). */
static void test_gate_syntax_errors(void) {
    expect_error("and g (y, , a);", "x.v:2:11: error: syntax: unexpected ','\n");
    expect_error("and g ();", "x.v:2:8: error: syntax: unexpected ')'\n");
    expect_error("pullup #1 (n);",
                 "x.v:2:8: error: syntax: unexpected '#', expecting identifier or "
                 "'('\n");
}

/* --- generate (§3.6 "Generate") ----------------------------------------------------------------
 */

/* A region holding a genvar and a loop whose body is a named block (G1, G2, N6). */
static void test_generate_region_loop(void) {
    vt_result res;
    parse_body("generate genvar i; for (i = 0; i < 4; i = i + 1) begin : g wire w; assign w = i;"
               " end endgenerate",
               &res);
    odin3_ast_id gen = item_at(&res, 0);
    expect_node(&res, gen,
                "(GENERATE (GENVAR_DECL (DECLARATOR:i _))"
                " (GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
                " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 4))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (GEN_BLOCK:g _ (NET_DECL.wire _ _ _ (DECLARATOR:w _))"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:w) (IDENT:i))))))",
                "");
    expect_head(&res, gen, "(GENERATE @x.v:2:1");
    odin3_ast_id loop = odin3_ast_child(res.ast, gen, 1);
    expect_head(&res, loop, "(GEN_FOR @x.v:2:20");
    expect_head(&res, odin3_ast_child(res.ast, loop, 3), "(GEN_BLOCK:g @x.v:2:50");
    vt_free(&res);
    expect_item("generate endgenerate", "(GENERATE)");
}

/* A loop counting down with a bare-item body; an unnamed block (name 0, no index) outside a
 * region (G2, G4). */
static void test_generate_loop_bodies(void) {
    expect_item("for (i = 3; i >= 0; i = i - 1) assign y[i] = a[i];",
                "(GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 3))"
                " (BINARY.>= (IDENT:i) (NUMBER.dec [SIGNED] 0))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.- (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (SELECT.bit (IDENT:y) (IDENT:i) _)"
                " (SELECT.bit (IDENT:a) (IDENT:i) _))))");
    expect_item("for (i = 0; i < 2; i = i + 1) begin m u (a); end",
                "(GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
                " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 2))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (GEN_BLOCK _ (INSTANTIATION:m _ (INSTANCE:u _ (CONNECTION (IDENT:a))))))");
    expect_item("for (i = 0; i < 2; i = i + 1) begin end",
                "(GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
                " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 2))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (GEN_BLOCK _))");
    /* Nested loops and a gate as a bare body. */
    expect_item(
        "for (i = 0; i < 2; i = i + 1) for (j = 0; j < 2; j = j + 1) and (y, a);",
        "(GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
        " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 2))"
        " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
        " (GEN_FOR (BLOCKING_ASSIGN (IDENT:j) _ (NUMBER.dec [SIGNED] 0))"
        " (BINARY.< (IDENT:j) (NUMBER.dec [SIGNED] 2))"
        " (BLOCKING_ASSIGN (IDENT:j) _ (BINARY.+ (IDENT:j) (NUMBER.dec [SIGNED] 1)))"
        " (GATE_DECL.and _ _ (INSTANCE _ (CONNECTION (IDENT:y)) (CONNECTION (IDENT:a))))))");
}

/* An else-if chain of named blocks, a bare-item branch and a null else (G5). */
static void test_if_generate(void) {
    vt_result res;
    parse_body("if (W > 1) begin : a wire x; end else if (W) assign y = 1; else ;", &res);
    odin3_ast_id gif = item_at(&res, 0);
    expect_node(
        &res, gif,
        "(GEN_IF (BINARY.> (IDENT:W) (NUMBER.dec [SIGNED] 1))"
        " (GEN_BLOCK:a _ (NET_DECL.wire _ _ _ (DECLARATOR:x _)))"
        " (GEN_IF (IDENT:W) (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:y) (NUMBER.dec [SIGNED] 1)))"
        " (NULL_STMT)))",
        "");
    expect_head(&res, gif, "(GEN_IF @x.v:2:1");
    odin3_ast_id inner = odin3_ast_child(res.ast, gif, 2);
    expect_head(&res, inner, "(GEN_IF @x.v:2:39");
    expect_head(&res, odin3_ast_child(res.ast, inner, 2), "(NULL_STMT @x.v:2:65)");
    vt_free(&res);
    expect_item("if (c) ;", "(GEN_IF (IDENT:c) (NULL_STMT) _)");
}

/* §3.3: a dangling else in an if-generate goes to the inner if. */
static void test_if_generate_dangling_else(void) {
    expect_item("if (a) if (b) assign x = 1; else assign x = 2;",
                "(GEN_IF (IDENT:a) (GEN_IF (IDENT:b)"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:x) (NUMBER.dec [SIGNED] 1)))"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:x) (NUMBER.dec [SIGNED] 2)))) _)");
    /* The printer wraps the inner if when the outer one has the else; it re-parses equal. */
    expect_item("if (a) begin if (b) assign x = 1; end else assign x = 2;",
                "(GEN_IF (IDENT:a) (GEN_BLOCK _ (GEN_IF (IDENT:b)"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:x) (NUMBER.dec [SIGNED] 1))) _))"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:x) (NUMBER.dec [SIGNED] 2))))");
}

/* A case generate: several expressions per item, a bare-item body, a named block, a default
 * with a null body (G6); the body precedes the match expressions (2A §4.2 convention 4). */
static void test_case_generate(void) {
    vt_result res;
    parse_body("case (W) 1, 2: begin : b wire x; end 3: assign y = 0; default: ; endcase", &res);
    odin3_ast_id gcase = item_at(&res, 0);
    expect_node(&res, gcase,
                "(GEN_CASE (IDENT:W)"
                " (CASE_ITEM (GEN_BLOCK:b _ (NET_DECL.wire _ _ _ (DECLARATOR:x _)))"
                " (NUMBER.dec [SIGNED] 1) (NUMBER.dec [SIGNED] 2))"
                " (CASE_ITEM (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:y) (NUMBER.dec [SIGNED] 0)))"
                " (NUMBER.dec [SIGNED] 3))"
                " (CASE_ITEM [DEFAULT] (NULL_STMT)))",
                "");
    expect_head(&res, gcase, "(GEN_CASE @x.v:2:1");
    expect_head(&res, odin3_ast_child(res.ast, gcase, 3), "(CASE_ITEM [DEFAULT] @x.v:2:55");
    vt_free(&res);
    expect_item("case (W) default assign y = 0; endcase",
                "(GEN_CASE (IDENT:W) (CASE_ITEM [DEFAULT]"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:y) (NUMBER.dec [SIGNED] 0)))))");
}

/* A case generate takes the header's metacomments and attributes into one attach, as CASE does
 * (§2.6, §3.5). */
static void test_case_generate_metacomment(void) {
    expect_item("(* k *) case (W) // synopsys parallel_case\n 0: ; endcase",
                "(GEN_CASE (IDENT:W) (CASE_ITEM (NULL_STMT) (NUMBER.dec [SIGNED] 0))"
                " {(ATTR:k _) (ATTR:parallel_case [METACOMMENT] _)})");
}

/* Attributes on generate constructs attach to their node (§3.5); `g[0].t` is a HIER_NAME with a
 * SELECT part (G7); a single-pair defparam is a bare body (P6). */
static void test_generate_attributes_references_defparam(void) {
    expect_item("(* a *) generate (* b *) for (i = 0; i < 1; i = i + 1) assign y = a; endgenerate",
                "(GENERATE (GEN_FOR (BLOCKING_ASSIGN (IDENT:i) _ (NUMBER.dec [SIGNED] 0))"
                " (BINARY.< (IDENT:i) (NUMBER.dec [SIGNED] 1))"
                " (BLOCKING_ASSIGN (IDENT:i) _ (BINARY.+ (IDENT:i) (NUMBER.dec [SIGNED] 1)))"
                " (CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:y) (IDENT:a))) {(ATTR:b _)}) {(ATTR:a _)})");
    expect_item("assign y = g[0].t;",
                "(CONT_ASSIGN _ _ (NET_ASSIGN (IDENT:y) (HIER_NAME"
                " (SELECT.bit (IDENT:g) (NUMBER.dec [SIGNED] 0) _) (IDENT:t))))");
    expect_item("if (c) (* k *) defparam u.W = 1;",
                "(GEN_IF (IDENT:c) (DEFPARAM (HIER_NAME (IDENT:u) (IDENT:W))"
                " (NUMBER.dec [SIGNED] 1) {(ATTR:k _)}) _)");
}

/* Shapes Annex A rejects: a loop body is never null; a generate case is `case` only; a case item
 * takes no attribute; a bare body holds one item (one defparam pair). */
static void test_generate_syntax_errors(void) {
    expect_error("for (i = 0; i < 1; i = i + 1) ;", "x.v:2:31: error: syntax: unexpected ';'\n");
    expect_error("casez (W) 0: ; endcase", "x.v:2:1: error: syntax: unexpected casez\n");
    expect_error("case (W) (* k *) 0: ; endcase", "x.v:2:10: error: syntax: unexpected (*\n");
    expect_error("if (c) defparam u.W = 1, v.W = 2;", "x.v:2:24: error: syntax: unexpected ','\n");
    expect_error("if (c) (* k *) begin end", "x.v:2:16: error: syntax: unexpected begin\n");
}

/* An item missing its `;` before a generate closer resynchronises there, so the next item's error
 * is still reported. */
static void test_generate_recovery_at_closers(void) {
    expect_error("generate wire a endgenerate\nwire w = = 1;",
                 "x.v:2:17: error: syntax: unexpected endgenerate, expecting ';' or ',' or '=' or "
                 "'['\n"
                 "x.v:3:10: error: syntax: unexpected '='\n");
    expect_error("if (c) begin wire a end\nwire w = = 1;",
                 "x.v:2:21: error: syntax: unexpected end, expecting ';' or ',' or '=' or '['\n"
                 "x.v:3:10: error: syntax: unexpected '='\n");
    expect_error("case (c) 0: begin end 1 + : ; endcase\nwire w = = 1;",
                 "x.v:2:27: error: syntax: unexpected ':'\n"
                 "x.v:3:10: error: syntax: unexpected '='\n");
}

/* --- OOM ------------------------------------------------------------------------------------- */

/* Every Task 7 action: parameter and port connections of every form, unnamed and arrayed
 * instances, gates with strengths, delays and pulls, a generate region, loop, if and case
 * generates with named, unnamed and bare bodies, a metacomment case generate. */
static const char OOM_TEXT[] =
    "primitive p (y, a); output y; input a; table 0 : 0; endtable endprimitive\n"
    "module m (input a, b, output y);\n"
    "  (* k *) n #(8, .W(4), .D()) u1 (a, .p(b), .q(), , y), u2 [1:0] (), u3 (,);\n"
    "  p (y, a);\n"
    "  (* g *) and (strong0, weak1) #(1, 2) g1 [1:0] (y, a, b), (y, b);\n"
    "  pullup (strong1) (y); pulldown (y); nmos #1 (y, a, b);\n"
    "  genvar i;\n"
    "  generate for (i = 0; i < 2; i = i + 1) begin : g wire w; assign w = a; end endgenerate\n"
    "  for (i = 0; i < 2; i = i + 1) begin n v (a); end\n"
    "  for (i = 0; i < 2; i = i + 1) assign y = a;\n"
    "  if (a) begin : t end else if (b) assign y = a; else ;\n"
    "  case (a) // synopsys full_case\n"
    "    0, 1: begin wire x; end 2: assign y = b; default ;\n"
    "  endcase\n"
    "endmodule\n";

static odin3_strbuf oom_text;

/* OOM_TEXT, then a module with many instances and connections (store and scratch growth). */
static void build_oom_text(void) {
    odin3_strbuf_init(&oom_text);
    append(&oom_text, OOM_TEXT);
    append(&oom_text, "module big (input a, output y);\n  n u0 (a)");
    for (int i = 1; i < OOM_INSTANCES; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&oom_text, ", u%d (a, , .y(y))", i));
    }
    append(&oom_text, ";\n  or g (y");
    for (int i = 0; i < OOM_INSTANCES; i++) {
        append(&oom_text, ", a");
    }
    append(&oom_text, ");\nendmodule\n");
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
    vt_stream(res.design, "x.v", (odin3_bytes){oom_text.data, oom_text.len}, &stream);
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

static void test_inst_oom_sweep(void) {
    build_oom_text();
    vt_result clean;
    parse_clean(oom_text.data, &clean); /* the text itself parses and checks */
    vt_free(&clean);
    long limit = 0;
    while (limit < INST_OOM_LIMIT && oom_step(limit) != ODIN3_OK) {
        limit++;
    }
    char text[INST_OOM_MESSAGE];
    (void)snprintf(text, sizeof text, "%ld allocations swept", limit);
    TEST_MESSAGE(text);
    odin3_strbuf_free(&oom_text);
    TEST_ASSERT_TRUE(limit > 0);
    TEST_ASSERT_TRUE(limit < INST_OOM_LIMIT);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_instantiation_connections);
    RUN_TEST(test_empty_connection_lists);
    RUN_TEST(test_mixed_connections_and_expressions);
    RUN_TEST(test_unnamed_instance);
    RUN_TEST(test_instantiation_syntax_errors);
    RUN_TEST(test_instantiation_attributes);
    RUN_TEST(test_gate_branches);
    RUN_TEST(test_gate_kinds);
    RUN_TEST(test_every_gate_keyword);
    RUN_TEST(test_gate_locations_and_attributes);
    RUN_TEST(test_gate_syntax_errors);
    RUN_TEST(test_generate_region_loop);
    RUN_TEST(test_generate_loop_bodies);
    RUN_TEST(test_if_generate);
    RUN_TEST(test_if_generate_dangling_else);
    RUN_TEST(test_case_generate);
    RUN_TEST(test_case_generate_metacomment);
    RUN_TEST(test_generate_attributes_references_defparam);
    RUN_TEST(test_generate_syntax_errors);
    RUN_TEST(test_generate_recovery_at_closers);
    RUN_TEST(test_end_locations);
    RUN_TEST(test_inst_oom_sweep);
    return UNITY_END();
}
