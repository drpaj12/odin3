/*
 * test_elab_stage1_gen.c — 2D Task 14: generate in stage 1 (EL-19–EL-22). Loops, if and case
 * generate, label_genblks's names (the review's probe: genblk1.genblk1.x, genblk3[0].w; user
 * `genblk` names before and after), references into generate blocks (G7, E21, the not-elaborated
 * error), misplaced and clashing names, scoped defparam push-down, recursion through generate and
 * an OOM sweep. Sources are parsed by 2C (elab_test.h); the elaborated GEN_BLOCKs (names, index
 * slots, nesting) are read from the store; every located diagnostic is asserted as the whole line.
 * The names were checked against Yosys 0.55 (the task report's batch).
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 64 };

static et_result rs;
static odin3_arena *values;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_elab_test_set_limits(NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
    odin3_arena_destroy(values);
}

/* --- running ------------------------------------------------------------------------------- */

static void run_one(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
}

/* A run that must succeed: OK, no error, the store passes check FULL and binds (EL-5). */
static void expect_clean(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    et_expect_binds(&rs);
}

static void run_clean(const char *text) {
    run_one(text);
    expect_clean();
}

/* The line is the only error, and stage 1 failed. */
static void expect_only_error(const char *line) {
    et_expect(&rs, ODIN3_LOG_ERROR, line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

/* A source and the only error it must give. */
typedef struct gen_case {
    const char *text;
    const char *line;
} gen_case;

static void run_error(const gen_case *gc) {
    run_one(gc->text);
    expect_only_error(gc->line);
}

/* --- reading the elaborated store ---------------------------------------------------------- */

static const char *name_of(odin3_ast_id node) {
    return odin3_ast_name_str(rs.elab, node);
}

static odin3_ast_id find(odin3_ast_kind kind, const char *name) {
    TEST_ASSERT_NOT_NULL_MESSAGE(rs.elab, et_log(&rs));
    uint32_t end = odin3_ast_node_end(rs.elab).v;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(rs.elab, node) == kind && strcmp(name_of(node), name) == 0) {
            return node;
        }
    }
    return (odin3_ast_id){0};
}

static odin3_ast_id module_named(const char *name) {
    odin3_ast_id node = find(ODIN3_AST_MODULE, name);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, name);
    return node;
}

/* A NUMBER's value as an integer (known, fitting). */
static int64_t number_int(odin3_ast_id node) {
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NUMBER, odin3_ast_kind_of(rs.elab, node));
    odin3_ast_literal lit;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(rs.elab, node, values, &lit));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(values, &lit, &val));
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &out));
    return out;
}

/* The k-th GEN_BLOCK child of parent (a MODULE or GEN_BLOCK), else 0. */
static odin3_ast_id nth_block(odin3_ast_id parent, uint32_t nth) {
    odin3_ast_span kids = odin3_ast_children(rs.elab, parent);
    for (uint32_t i = 0; i < kids.n; i++) {
        if (kids.ids[i].v != 0 && odin3_ast_kind_of(rs.elab, kids.ids[i]) == ODIN3_AST_GEN_BLOCK &&
            nth-- == 0) {
            return kids.ids[i];
        }
    }
    return (odin3_ast_id){0};
}

static uint32_t count_blocks(odin3_ast_id parent) {
    uint32_t count = 0;
    while (nth_block(parent, count).v != 0) {
        count++;
    }
    return count;
}

/* The k-th GEN_BLOCK of parent: named name, without an index (has_index false) or with index. */
static odin3_ast_id expect_block(odin3_ast_id parent, uint32_t nth, const char *name,
                                 const int64_t *index) {
    odin3_ast_id blk = nth_block(parent, nth);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, blk.v, name);
    TEST_ASSERT_EQUAL_STRING(name, name_of(blk));
    odin3_ast_id slot = odin3_ast_child(rs.elab, blk, 0);
    if (index == NULL) {
        TEST_ASSERT_EQUAL_UINT32(0, slot.v);
    } else {
        TEST_ASSERT_EQUAL_INT64(*index, number_int(slot));
        TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_NUMBER_DEC, odin3_ast_sub(rs.elab, slot));
        TEST_ASSERT_TRUE((odin3_ast_flags(rs.elab, slot) & ODIN3_AST_F_NUMBER_SIGNED) != 0);
    }
    return blk;
}

/* True when holder declares a net or variable named name directly. */
static bool declares(odin3_ast_id holder, const char *name) {
    odin3_ast_span kids = odin3_ast_children(rs.elab, holder);
    for (uint32_t i = 0; i < kids.n; i++) {
        odin3_ast_kind kind = kids.ids[i].v != 0 ? odin3_ast_kind_of(rs.elab, kids.ids[i]) : 0;
        if (kind != ODIN3_AST_NET_DECL && kind != ODIN3_AST_VAR_DECL) {
            continue;
        }
        odin3_ast_span decls = odin3_ast_children(rs.elab, kids.ids[i]);
        for (uint32_t j = odin3_ast_slot_count(kind); j < decls.n; j++) {
            if (strcmp(name_of(decls.ids[j]), name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* The first INSTANTIATION directly in holder. */
static odin3_ast_id instantiation_in(odin3_ast_id holder) {
    odin3_ast_span kids = odin3_ast_children(rs.elab, holder);
    for (uint32_t i = 0; i < kids.n; i++) {
        if (kids.ids[i].v != 0 &&
            odin3_ast_kind_of(rs.elab, kids.ids[i]) == ODIN3_AST_INSTANTIATION) {
            return kids.ids[i];
        }
    }
    TEST_FAIL_MESSAGE("no INSTANTIATION");
    return (odin3_ast_id){0};
}

/* --- label_genblks (EL-21) ----------------------------------------------------------------- */

/* Review Focus 5: genblk1.genblk1.x, a bare `if` consumes genblk2, the loop is genblk3[0].w. */
static void test_review_probe(void) {
    run_clean("module t (input a, output y);\n  localparam A = 0, B = 1;\n"
              "  if (A) begin wire x; end else if (B) begin wire x; end\n"
              "  if (B) assign y = a;\n  genvar i;\n"
              "  for (i = 0; i < 1; i = i + 1) begin wire w; end\nendmodule\n");
    odin3_ast_id top = module_named("t");
    TEST_ASSERT_EQUAL_UINT32(3, count_blocks(top));
    odin3_ast_id outer = expect_block(top, 0, "genblk1", NULL);
    TEST_ASSERT_EQUAL_UINT32(1, count_blocks(outer));
    TEST_ASSERT_TRUE(declares(expect_block(outer, 0, "genblk1", NULL), "x"));
    odin3_ast_id bare = expect_block(top, 1, "genblk2", NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_CONT_ASSIGN,
                          odin3_ast_kind_of(rs.elab, odin3_ast_child(rs.elab, bare, 1)));
    int64_t zero = 0;
    TEST_ASSERT_TRUE(declares(expect_block(top, 2, "genblk3", &zero), "w"));
}

/* A user `genblk1` declared after the first construct does not pad it, and both bind (Yosys
 * keeps the wire and the block: the IDENT reads the wire, the HIER_NAME the block); one declared
 * before pads it. */
static void test_user_genblk_names(void) {
    run_clean("module t (output y, output z);\n  if (1) begin wire x; end\n  wire genblk1;\n"
              "  assign y = genblk1.x;\n  assign z = genblk1;\nendmodule\n");
    expect_block(module_named("t"), 0, "genblk1", NULL);
    TEST_ASSERT_TRUE(declares(module_named("t"), "genblk1"));
    et_free(&rs);
    run_clean("module t;\n  wire genblk1;\n  if (1) begin wire x; end\nendmodule\n");
    expect_block(module_named("t"), 0, "genblk01", NULL);
    et_free(&rs);
    /* a localparam pads too; a user-named block `genblk3` does not enter the set */
    run_clean("module t;\n  if (1) begin wire x; end\n  localparam genblk2 = 0;\n"
              "  if (1) begin wire y; end\n  if (1) begin : genblk3 wire z; end\n"
              "  if (1) begin wire v; end\nendmodule\n");
    odin3_ast_id top = module_named("t");
    expect_block(top, 0, "genblk1", NULL);
    expect_block(top, 1, "genblk02", NULL);
    expect_block(top, 2, "genblk3", NULL);
    expect_block(top, 3, "genblk4", NULL);
}

/* An ancestor's `genblk` names make a nested block pad too (the review's M5, Yosys's inherited
 * seen set): `wire genblk1` outside pads the outer block and the inner one. */
static void test_inherited_seen_set(void) {
    run_clean(
        "module t;\n  wire genblk1;\n  if (1) begin if (1) begin wire q; end end\nendmodule\n");
    odin3_ast_id outer = expect_block(module_named("t"), 0, "genblk01", NULL);
    expect_block(outer, 0, "genblk01", NULL);
}

/* An unselected construct consumes a number; a null branch is an (empty) block; a region is
 * transparent; case and if blocks are named after their construct. */
static void test_counter_and_regions(void) {
    run_clean("module t;\n  localparam W = 2;\n"
              "  case (W) 2'b1x: begin wire p; end 1, 2: begin wire q; end default: begin wire d; "
              "end endcase\n"
              "  generate if (0) ; else begin wire e; end endgenerate\n"
              "  if (0) begin wire n; end\n  if (1) begin wire f; end\nendmodule\n");
    odin3_ast_id top = module_named("t");
    TEST_ASSERT_EQUAL_UINT32(3, count_blocks(top));
    TEST_ASSERT_TRUE(declares(expect_block(top, 0, "genblk1", NULL), "q"));
    TEST_ASSERT_TRUE(declares(expect_block(top, 1, "genblk2", NULL), "e"));
    TEST_ASSERT_TRUE(declares(expect_block(top, 2, "genblk4", NULL), "f"));
    et_free(&rs);
    run_clean("module t;\n  if (1) ; else begin wire e; end\nendmodule\n");
    odin3_ast_id empty = expect_block(module_named("t"), 0, "genblk1", NULL);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(rs.elab, empty));
}

/* --- loops (EL-19) ------------------------------------------------------------------------- */

static void test_loop_descending(void) {
    run_clean("module t;\n  genvar i;\n  for (i = 3; i >= 0; i = i - 1) begin : g\n"
              "    wire [i:0] t;\n  end\nendmodule\n");
    odin3_ast_id top = module_named("t");
    TEST_ASSERT_EQUAL_UINT32(4, count_blocks(top));
    for (int64_t k = 0; k < 4; k++) {
        int64_t index = 3 - k;
        odin3_ast_id blk = expect_block(top, (uint32_t)k, "g", &index);
        TEST_ASSERT_TRUE(declares(blk, "t"));
        odin3_ast_id range = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, blk, 1), 1);
        TEST_ASSERT_EQUAL_INT64(index, number_int(odin3_ast_child(rs.elab, range, 0)));
    }
}

static void test_loop_negative(void) {
    run_clean("module t;\n  genvar i;\n  for (i = -1; i < 1; i = i + 1) begin : g\n"
              "    localparam P = i * 2;\n  end\nendmodule\n");
    odin3_ast_id top = module_named("t");
    TEST_ASSERT_EQUAL_UINT32(2, count_blocks(top));
    int64_t minus = -1;
    int64_t zero = 0;
    odin3_ast_id first = expect_block(top, 0, "g", &minus);
    expect_block(top, 1, "g", &zero);
    odin3_ast_id param = odin3_ast_child(rs.elab, first, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_PARAM_DECL, odin3_ast_kind_of(rs.elab, param));
    odin3_ast_id decl = odin3_ast_child(rs.elab, param, 1);
    TEST_ASSERT_EQUAL_INT64(-2, number_int(odin3_ast_child(rs.elab, decl, 0)));
}

/* A genvar read in the body is a 32-bit signed NUMBER (EL-19). */
static void test_genvar_read(void) {
    run_clean(
        "module t (output [1:0] y);\n  genvar i;\n"
        "  for (i = 0; i < 2; i = i + 1) begin : g\n    assign y[i] = i;\n  end\nendmodule\n");
    odin3_ast_id blk = nth_block(module_named("t"), 1);
    odin3_ast_id assign = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, blk, 1), 2);
    odin3_ast_id rhs = odin3_ast_child(rs.elab, assign, 1);
    TEST_ASSERT_EQUAL_INT64(1, number_int(rhs));
    TEST_ASSERT_TRUE((odin3_ast_flags(rs.elab, rhs) & ODIN3_AST_F_NUMBER_SIGNED) != 0);
    odin3_ast_id lhs = odin3_ast_child(rs.elab, assign, 0);
    TEST_ASSERT_EQUAL_INT64(1, number_int(odin3_ast_child(rs.elab, lhs, 1)));
}

static void test_loop_errors(void) {
    run_error(&(gen_case){
        "module t;\n  genvar i;\n  for (i = 0; i < 2; i = i * 1) begin end\nendmodule\n",
        "t.v:3:3: error: generate loop of 'i' repeats the value 0"});
    et_free(&rs);
    run_error(&(gen_case){
        "module t;\n  integer k;\n  for (k = 0; k < 2; k = k + 1) begin end\nendmodule\n",
        "t.v:3:8: error: 'k' is not a genvar"});
    et_free(&rs);
    run_error(&(gen_case){
        "module t;\n  genvar i, j;\n  for (i = 0; i < 2; j = j + 1) begin end\nendmodule\n",
        "t.v:3:3: error: generate loop must assign genvar 'i' in its initialisation and step"});
    et_free(&rs);
    run_error(&(gen_case){"module t;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : a\n"
                          "    for (i = 0; i < 2; i = i + 1) begin : b end\n  end\nendmodule\n",
                          "t.v:4:10: error: genvar 'i' is already used by an enclosing loop"});
    et_free(&rs);
    run_error(
        &(gen_case){"module t;\n  genvar i;\n  wire w;\n  for (i = 0; i < w; i = i + 1) begin end\n"
                    "endmodule\n",
                    "t.v:4:19: error: generate loop condition is not constant (it reads 'w')"});
}

static void test_loop_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_loop_iterations = 8;
    odin3_elab_test_set_limits(&limits);
    run_error(&(gen_case){
        "module t;\n  genvar i;\n  for (i = 0; i < 100; i = i + 1) begin end\nendmodule\n",
        "t.v:3:3: error: loop exceeds 8 iterations"});
    et_free(&rs);
    run_clean("module t;\n  genvar i;\n  for (i = 0; i < 8; i = i + 1) begin end\nendmodule\n");
    odin3_elab_test_set_limits(NULL);
    limits = *odin3_elab_limits_get();
    limits.max_total_iterations = 5;
    odin3_elab_test_set_limits(&limits);
    et_free(&rs);
    run_error(&(gen_case){"module t;\n  genvar i;\n  for (i = 0; i < 3; i = i + 1) begin end\n"
                          "  for (i = 0; i < 3; i = i + 1) begin end\nendmodule\n",
                          "t.v:4:3: error: unrolled loops exceed 5 iterations in this design"});
}

/* --- if and case (EL-20) ------------------------------------------------------------------- */

static void test_if_errors_and_x(void) {
    run_error(&(gen_case){"module t;\n  wire w;\n  if (w) begin end\nendmodule\n",
                          "t.v:3:7: error: generate condition is not constant (it reads 'w')"});
    et_free(&rs);
    run_clean(
        "module t;\n  if (1'bx) begin : a wire p; end else begin : b wire q; end\nendmodule\n");
    TEST_ASSERT_TRUE(declares(expect_block(module_named("t"), 0, "b", NULL), "q"));
}

/* The case of W: item 0 (W = 2), default (W = 5), `2'b1x` never matches, no match no block. */
static void case_with(int value, const char *body) {
    char text[512];
    int len = snprintf(text, sizeof text, "module t;\n  localparam W = %d;\n  %s\nendmodule\n",
                       value, body);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    run_clean(text);
}

static void test_case_select(void) {
    const char *items =
        "case (W) 1, 2: begin : a wire p; end default: begin : d wire q; end endcase";
    case_with(2, items);
    TEST_ASSERT_TRUE(declares(expect_block(module_named("t"), 0, "a", NULL), "p"));
    et_free(&rs);
    case_with(5, items);
    TEST_ASSERT_TRUE(declares(expect_block(module_named("t"), 0, "d", NULL), "q"));
    et_free(&rs);
    case_with(2, "case (W) 2'b1x: begin : x end default: begin : d end endcase");
    expect_block(module_named("t"), 0, "d", NULL);
    et_free(&rs);
    case_with(5, "case (W) 1: begin : a end endcase");
    TEST_ASSERT_EQUAL_UINT32(0, count_blocks(module_named("t")));
}

static void test_case_item_error(void) {
    run_error(&(gen_case){
        "module t;\n  wire w;\n  case (2) w: begin end endcase\nendmodule\n",
        "t.v:3:12: error: case item in a generate case is not constant (it reads 'w')"});
}

/* --- references (EL-22) -------------------------------------------------------------------- */

#define LOOP_G                                                                                     \
    "module t;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n    wire t;\n  end\n"

static void test_reference(void) {
    run_clean(LOOP_G "  wire z;\n  assign z = g[1].t;\nendmodule\n");
    odin3_ast_id hier = find(ODIN3_AST_HIER_NAME, "");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, hier.v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, hier));
    odin3_ast_id sel = odin3_ast_child(rs.elab, hier, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_SELECT, odin3_ast_kind_of(rs.elab, sel));
    TEST_ASSERT_EQUAL_INT64(1, number_int(odin3_ast_child(rs.elab, sel, 1)));
    TEST_ASSERT_EQUAL_STRING("t", name_of(odin3_ast_child(rs.elab, hier, 1)));
}

static void test_reference_errors(void) {
    run_error(&(gen_case){LOOP_G "  wire z;\n  assign z = g[5].t;\nendmodule\n",
                          "t.v:7:14: error: 'g[5].t' does not exist (generate block 'g[5]' was not "
                          "elaborated)"});
    et_free(&rs);
    run_error(&(gen_case){"module m;\n  wire w;\nendmodule\nmodule t;\n  m u ();\n  wire z;\n"
                          "  assign z = u.w;\nendmodule\n",
                          "t.v:7:14: error: hierarchical reference 'u.w' is not supported"});
    et_free(&rs);
    run_error(&(gen_case){LOOP_G "  wire z;\n  assign z = g[0].q;\nendmodule\n",
                          "t.v:7:14: error: 'g[0].q' is not declared"});
}

/* --- misplaced and clashing names ---------------------------------------------------------- */

static void test_port_in_block(void) {
    run_error(&(gen_case){
        "module t;\n  genvar i;\n  for (i = 0; i < 1; i = i + 1) begin : g\n    input p;\n"
        "  end\nendmodule\n",
        "t.v:4:5: error: port declarations are not allowed in a generate block"});
}

static void test_block_clash(void) {
    run_error(
        &(gen_case){"module t;\n  wire g;\n  genvar i;\n  for (i = 0; i < 1; i = i + 1) begin : g\n"
                    "  end\nendmodule\n",
                    "t.v:4:33: error: 'g' is already declared (first at t.v:2:8)"});
    et_free(&rs);
    run_clean("module t;\n  if (0) begin : b wire x; end else begin : b wire y; end\nendmodule\n");
}

/* --- defparams, recursion, parameters ------------------------------------------------------ */

#define M_W "module m #(parameter W = 1) (output [W-1:0] o);\n  assign o = 0;\nendmodule\n"

static void test_scoped_defparam(void) {
    run_clean(M_W
              "module t;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n"
              "    wire [3:0] t;\n    m u (.o(t));\n  end\n  defparam g[1].u.W = 2;\nendmodule\n");
    odin3_ast_id top = module_named("t");
    TEST_ASSERT_EQUAL_STRING("m", name_of(instantiation_in(nth_block(top, 0))));
    TEST_ASSERT_EQUAL_STRING("m$W=2", name_of(instantiation_in(nth_block(top, 1))));
    module_named("m$W=2");
}

/* A defparam into a block that was not elaborated is the target error, its path as written. */
static void test_defparam_bad_scope(void) {
    run_error(&(gen_case){M_W "module t;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n"
                              "    wire [3:0] t;\n    m u (.o(t));\n  end\n"
                              "  defparam g[1 + 4].u.W = 2;\nendmodule\n",
                          "t.v:10:12: error: defparam target 'g[1 + 4].u.W' does not name a "
                          "parameter of an instance below this module"});
}

/* A defparam written inside a block names an instance of that block. */
static void test_defparam_in_block(void) {
    run_clean(M_W "module t;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n"
                  "    wire [3:0] t;\n    if (i == 0) begin m u (.o(t)); defparam u.W = 3; end\n"
                  "  end\nendmodule\n");
    odin3_ast_id inner = nth_block(nth_block(module_named("t"), 0), 0);
    TEST_ASSERT_EQUAL_STRING("genblk1", name_of(inner));
    TEST_ASSERT_EQUAL_STRING("m$W=3", name_of(instantiation_in(inner)));
}

/* A defparam pushed through an instance into its generate block (a pending `g[1]` path). */
static void test_defparam_pushed_into_block(void) {
    run_clean(M_W "module n;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n"
                  "    wire [3:0] t;\n    m u (.o(t));\n  end\nendmodule\n"
                  "module t;\n  n x ();\n  defparam x.g[0].u.W = 4;\nendmodule\n");
    odin3_ast_id spec = module_named(name_of(instantiation_in(module_named("t"))));
    TEST_ASSERT_EQUAL_STRING("m$W=4", name_of(instantiation_in(nth_block(spec, 0))));
    TEST_ASSERT_EQUAL_STRING("m", name_of(instantiation_in(nth_block(spec, 1))));
}

static void test_recursion(void) {
    run_clean("module r #(parameter N = 3) ();\n  if (N > 0) begin : s\n    r #(N - 1) u ();\n"
              "  end\nendmodule\nmodule t;\n  r x ();\nendmodule\n");
    module_named("r");
    module_named("r$N=2");
    module_named("r$N=1");
    module_named("r$N=0");
    TEST_ASSERT_EQUAL_UINT32(0, count_blocks(module_named("r$N=0")));
}

/* A `parameter` in a generate block is a localparam: no #() reaches it. */
static void test_block_parameter_local(void) {
    run_error(&(gen_case){"module m;\n  if (1) begin : b\n    parameter P = 1;\n  end\nendmodule\n"
                          "module t;\n  m #(.P(2)) u ();\nendmodule\n",
                          "t.v:7:7: error: module 'm' has no parameter 'P'"});
    et_free(&rs);
    run_clean("module m;\n  if (1) begin : b\n    parameter P = 1;\n  end\nendmodule\n"
              "module t;\n  m u ();\nendmodule\n");
    odin3_ast_id param = odin3_ast_child(rs.elab, nth_block(module_named("m"), 0), 1);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_PARAM_DECL_LOCALPARAM, odin3_ast_sub(rs.elab, param));
}

/* Nothing of an unselected branch is elaborated, so its errors are never reported. */
static void test_unselected_branch_silent(void) {
    run_clean("module t;\n  if (0) begin\n    wire [nope:0] q;\n    assign q = g[9].t;\n"
              "  end else begin : e\n  end\n"
              "  case (1) 0: begin wire [bad:0] r; end default: ; endcase\nendmodule\n");
}

/* --- OOM ----------------------------------------------------------------------------------- */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

/* One step of the sweep: the allocation limit and the parsed store's node count to keep. */
typedef struct oom_at {
    long limit;
    uint32_t nodes;
} oom_at;

static odin3_status oom_step(const oom_at *at) {
    odin3_ast *elab = NULL;
    odin3_util_set_alloc_fail_after(at->limit);
    odin3_status st = odin3_elab_stage1(&rs.ctx, rs.parsed, &rs.opts, &elab);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_TRUE(st == ODIN3_OK ? elab != NULL : elab == NULL);
    TEST_ASSERT_EQUAL_UINT32(at->nodes, odin3_ast_node_end(rs.parsed).v);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(elab, ODIN3_CHECK_FULL));
    }
    odin3_ast_destroy(elab);
    return st;
}

/* Loops, if, case, a reference, a scoped defparam: every allocation fails in turn. */
static void test_oom(void) {
    run_clean(
        M_W
        "module t (output z);\n  genvar i;\n  wire genblk1;\n"
        "  for (i = 0; i < 3; i = i + 1) begin : g\n    wire [i:0] t;\n    reg [3:0] mem [0:i+1];\n"
        "    if (i == 1) begin m u (.o(t[0])); end else m v (.o(t[0]));\n  end\n"
        "  case (2) 1: ; 2: begin wire c; end endcase\n  assign z = g[2].t[0];\n"
        "  defparam g[1].genblk01.u.W = 1;\nendmodule\n");
    uint32_t nodes = odin3_ast_node_end(rs.parsed).v;
    odin3_log_set_sink(quiet_sink, NULL);
    int failed = 0;
    int settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        oom_at at = {limit, nodes};
        bool oom = oom_step(&at) == ODIN3_ERR_NO_MEMORY;
        failed += oom ? 1 : 0;
        settled = oom ? 0 : settled + 1;
    }
    TEST_ASSERT_EQUAL_INT(OOM_SETTLE, settled);
    TEST_ASSERT_TRUE(failed > 100);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.parsed, ODIN3_CHECK_FULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_review_probe);
    RUN_TEST(test_user_genblk_names);
    RUN_TEST(test_inherited_seen_set);
    RUN_TEST(test_counter_and_regions);
    RUN_TEST(test_loop_descending);
    RUN_TEST(test_loop_negative);
    RUN_TEST(test_genvar_read);
    RUN_TEST(test_loop_errors);
    RUN_TEST(test_loop_cap);
    RUN_TEST(test_if_errors_and_x);
    RUN_TEST(test_case_select);
    RUN_TEST(test_case_item_error);
    RUN_TEST(test_reference);
    RUN_TEST(test_reference_errors);
    RUN_TEST(test_port_in_block);
    RUN_TEST(test_block_clash);
    RUN_TEST(test_scoped_defparam);
    RUN_TEST(test_defparam_bad_scope);
    RUN_TEST(test_defparam_in_block);
    RUN_TEST(test_defparam_pushed_into_block);
    RUN_TEST(test_recursion);
    RUN_TEST(test_block_parameter_local);
    RUN_TEST(test_unselected_branch_silent);
    RUN_TEST(test_oom);
    return UNITY_END();
}
