/*
 * test_elab_stage1_decl.c — 2D Task 15: declarations, ports, implicit nets, blocks and
 * subroutines in stage 1 (EL-4, EL-23 stage 1, EL-24, EL-25). Non-ANSI ports merged with their
 * net or variable declarations (M1, M6, M12), implicit nets and `default_nettype, unnamed blocks,
 * function and task shapes, the constant folds of the elaborated form (point (bb)), attributes,
 * genvar reads, M11 across kinds and an OOM sweep. Sources are parsed by 2C (elab_test.h); the
 * elaborated store is read as its dump and every located diagnostic is asserted as the whole line.
 * The semantics follow Yosys 0.55 (the task report's batch).
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 64 };

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* --- running ------------------------------------------------------------------------------- */

static void run_one(const char *text) {
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs); /* an earlier run of this test */
        memset(&rs, 0, sizeof rs);
    }
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
}

/* A run that must succeed: OK, no error, the store passes check FULL and binds (EL-5). */
static void run_clean(const char *text) {
    run_one(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    et_expect_binds(&rs);
}

/* The line is the only error, and stage 1 failed. */
typedef struct err_case {
    const char *text;
    const char *line;
} err_case;

static void run_error_case(const err_case *ec) {
    run_one(ec->text);
    et_expect(&rs, ODIN3_LOG_ERROR, ec->line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

#define run_error(text, line) run_error_case(&(const err_case){(text), (line)})

/* The dump holds the fragments in this order. */
static void expect_seq(const char *const *parts, size_t n) {
    TEST_ASSERT_NOT_NULL_MESSAGE(rs.dump.data, et_log(&rs));
    const char *at = rs.dump.data;
    for (size_t i = 0; i < n; i++) {
        const char *hit = strstr(at, parts[i]);
        if (hit == NULL) {
            static char why[1 << 14];
            int got = snprintf(why, sizeof why, "missing '%s' after offset %d in:\n%s", parts[i],
                               (int)(at - rs.dump.data), rs.dump.data);
            TEST_FAIL_MESSAGE(got > 0 ? why : parts[i]);
        }
        at = hit + strlen(parts[i]);
    }
}

#define EXPECT_SEQ(...)                                                                            \
    do {                                                                                           \
        const char *const parts_[] = {__VA_ARGS__};                                                \
        expect_seq(parts_, sizeof parts_ / sizeof parts_[0]);                                      \
    } while (0)

static void expect_absent(const char *needle) {
    TEST_ASSERT_NOT_NULL(rs.dump.data);
    TEST_ASSERT_NULL_MESSAGE(strstr(rs.dump.data, needle), rs.dump.data);
}

static void expect_warn(const char *line) {
    et_expect(&rs, ODIN3_LOG_WARN, line);
}

/* --- ports (EL-24) ------------------------------------------------------------------------- */

/* M1: `output q; reg [3:0] q;` is one PORT_DECL with the type of the variable; the declaration
 * disappears from the items. */
static void test_m1_merge(void) {
    run_clean("module m(q);\n  output q;\n  reg [3:0] q;\nendmodule\n");
    EXPECT_SEQ("(MODULE:m", "(PORT_DECL.output [IN_HEADER DT=1]", "(RANGE", "32'sd3", "32'sd0",
               "(DECLARATOR:q");
    expect_absent("VAR_DECL");
    run_clean("module m(q, r);\n  output q;\n  output [1:0] r;\n  wire r;\n  reg q;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER DT=1]", "(DECLARATOR:q",
               "(PORT_DECL.output [IN_HEADER NET=1]", "32'sd1", "(DECLARATOR:r");
}

static void test_m1_net_kind_and_order(void) {
    /* the net declaration comes first, and gives the kind */
    run_clean("module m(q);\n  wand q;\n  output q;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER NET=3]", "(DECLARATOR:q");
    expect_absent("NET_DECL");
    /* a second declarator of the same declaration stays */
    run_clean("module m(q);\n  output q;\n  reg q, r;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER DT=1]", "(VAR_DECL.reg", "(DECLARATOR:r");
}

/* Yosys 0.55 accepts a body declaration of an ANSI port: the same merge. */
static void test_ansi_redeclared(void) {
    run_clean("module m(output q, input signed [3:0] a);\n  reg q;\n  wire [3:0] a;\n"
              "  always @* q = a[0];\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER DT=1]", "(DECLARATOR:q",
               "(PORT_DECL.input [SIGNED IN_HEADER NET=1]", "(DECLARATOR:a");
    expect_absent("VAR_DECL");
    expect_absent("NET_DECL");
    run_error("module m(input [3:0] a);\n  wire [7:0] a;\nendmodule\n",
              "t.v:2:14: error: port 'a' is declared with ranges [3:0] and [7:0]");
}

static void test_m1_range_conflict(void) {
    run_error("module m(q);\n  output [3:0] q;\n  reg [7:0] q;\nendmodule\n",
              "t.v:3:13: error: port 'q' is declared with ranges [3:0] and [7:0]");
    run_error("module m(q);\n  reg [7:0] q;\n  output [3:0] q;\nendmodule\n",
              "t.v:3:16: error: port 'q' is declared with ranges [7:0] and [3:0]");
}

static void test_m1_signed(void) {
    run_clean("module m(q);\n  output signed q;\n  reg [3:0] q;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [SIGNED IN_HEADER DT=1]", "(RANGE", "32'sd3");
    run_clean("module m(q);\n  output q;\n  reg signed [3:0] q;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [SIGNED IN_HEADER DT=1]", "32'sd3");
    run_clean("module m(q);\n  output q;\n  integer q;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER DT=2]", "(DECLARATOR:q");
}

static void test_port_lists(void) {
    run_error("module m(a);\nendmodule\n", "t.v:1:10: error: port 'a' is not declared");
    run_error("module m(a);\n  wire a;\nendmodule\n", "t.v:1:10: error: port 'a' is not declared");
    run_error("module m(a);\n  input a;\n  input b;\nendmodule\n",
              "t.v:3:9: error: 'b' is declared as a port but is not in the port list");
    run_error("module m;\n  output b;\nendmodule\n",
              "t.v:2:10: error: 'b' is declared as a port but is not in the port list");
    run_error("module m(a);\n  input a;\n  input a;\nendmodule\n",
              "t.v:3:9: error: 'a' is already declared (first at t.v:2:9)");
}

static void test_untyped_ports(void) {
    run_clean("module m(input a, output [1:0] b, inout c);\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:a",
               "(PORT_DECL.output [IN_HEADER NET=1]", "(DECLARATOR:b",
               "(PORT_DECL.inout [IN_HEADER NET=1]");
    run_clean("`default_nettype none\nmodule m(input a, output b);\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.input [IN_HEADER NET=1]", "(PORT_DECL.output [IN_HEADER NET=1]");
    run_clean("module m(input a, output reg b, output integer c);\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.output [IN_HEADER DT=1]", "(DECLARATOR:b",
               "(PORT_DECL.output [IN_HEADER DT=2]");
    run_clean("module m(a, b);\n  input wire a;\n  output wor b;\nendmodule\n");
    EXPECT_SEQ("(PORT_DECL.input [IN_HEADER NET=1]", "(PORT_DECL.output [IN_HEADER NET=4]");
}

/* M12: an input declared reg is a net, with a warning; an integer one is a signed [31:0] net. */
static void test_m12(void) {
    run_clean("module m(input reg a, input integer i);\nendmodule\n");
    expect_warn("t.v:1:20: warning: input 'a' is declared as a variable ('reg'); it is treated as "
                "a net");
    expect_warn("t.v:1:37: warning: input 'i' is declared as a variable ('integer'); it is "
                "treated as a net");
    EXPECT_SEQ("(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:a",
               "(PORT_DECL.input [SIGNED IN_HEADER NET=1]", "32'sd31", "32'sd0", "(DECLARATOR:i");
    expect_absent("DT=");
    run_clean("module m(a);\n  input a;\n  reg a;\nendmodule\n");
    expect_warn("t.v:2:9: warning: input 'a' is declared as a variable ('reg'); it is treated as a "
                "net");
}

static void test_m6(void) {
    run_error("module m(.x(a));\n  input a;\nendmodule\n",
              "t.v:1:10: error: port expression in a module port list is not supported");
    run_error("module m({a, b});\n  input a, b;\nendmodule\n",
              "t.v:1:10: error: port expression in a module port list is not supported");
}

/* --- implicit nets (EL-25) ----------------------------------------------------------------- */

static void test_implicit_nets(void) {
    run_clean(
        "module m(output y);\n  wire t;\n  assign t = a & b;\n  assign y = a | t;\nendmodule\n");
    EXPECT_SEQ("(MODULE:m", "(NET_DECL.wire", "(DECLARATOR:a", "(DECLARATOR:b", "{(ATTR:implicit",
               "(NET_DECL.wire @t.v:2:3", "(DECLARATOR:t");
    expect_warn("t.v:3:14: warning: 'a' is implicitly declared");
    expect_warn("t.v:3:18: warning: 'b' is implicitly declared");
    TEST_ASSERT_EQUAL_UINT32(2, rs.warnings);
}

static void test_implicit_flag(void) {
    run_one("module m(output y);\n  assign y = a;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_elab_binding bnd = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab_bind(rs.elab, &bnd));
    odin3_ast_id module = odin3_ast_child(rs.elab, odin3_ast_root(rs.elab, 0), 0);
    uint32_t id = 0;
    TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr("a"), &id));
    const odin3_symtab *st = odin3_ast_symtab(rs.elab);
    odin3_sym_id sym = odin3_symtab_lookup_local(st, odin3_elab_scope_of(&bnd, module), id);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sym.v);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_IMPLICIT, odin3_symbol_get(st, sym)->flags);
    TEST_ASSERT_TRUE(odin3_symbol_get(st, sym)->kind == ODIN3_SYM_NET);
    odin3_elab_binding_free(&bnd);
}

static void test_nettype_kinds(void) {
    run_clean("`default_nettype wand\nmodule m(output y);\n  assign y = a;\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wand", "(DECLARATOR:a");
    run_clean("`default_nettype triand\nmodule m(output y);\n  assign y = a;\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wand");
    run_clean("`default_nettype trior\nmodule m(output y);\n  assign y = a;\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wor");
    run_clean("`default_nettype uwire\nmodule m(output y);\n  assign y = a;\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.uwire");
    run_clean("`default_nettype tri\nmodule m(output y);\n  assign y = a;\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wire");
    run_error("`default_nettype none\nmodule m(output y);\n  assign y = a;\nendmodule\n",
              "t.v:3:14: error: 'a' is not declared (`default_nettype is none)");
    run_error("`default_nettype tri0\nmodule m(output y);\n  assign y = a;\nendmodule\n",
              "t.v:3:14: error: net type 'tri0' is not supported");
}

static void test_implicit_places(void) {
    /* a use inside a generate block is declared at module scope; a call is no net */
    run_clean("module m(output y);\n  function f; input x; f = x; endfunction\n  genvar i;\n"
              "  for (i = 0; i < 2; i = i + 1) begin : g\n    assign y = u & f(1);\n  end\n"
              "endmodule\n");
    EXPECT_SEQ("(MODULE:m", "(NET_DECL.wire", "(DECLARATOR:u", "{(ATTR:implicit");
    expect_absent("DECLARATOR:f @t.v:5");
    expect_warn("t.v:5:16: warning: 'u' is implicitly declared");
    /* an instance connection and an event control are uses; a local, a function name and a
     * constant position are not */
    run_clean("module s(input a); endmodule\nmodule m(input c);\n  reg r;\n  s u (.a(w));\n"
              "  always @(posedge ck) begin : b reg q; q = c; r = q; end\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wire", "(DECLARATOR:w", "(DECLARATOR:ck");
    run_error("module m(output y);\n  parameter P = q;\nendmodule\n",
              "t.v:2:17: error: 'q' is not declared");
}

/* --- `default_nettype placement ------------------------------------------------------------- */

static void test_nettype_placement(void) {
    run_clean("module a(output y);\n`default_nettype wor\n  assign y = u;\nendmodule\n"
              "module b(output y);\n  assign y = v;\nendmodule\n"
              "module t(output y);\n  a ua (.y(y));\n  b ub (.y());\nendmodule\n");
    expect_warn("t.v:2:1: warning: `default_nettype inside a module takes effect at the next "
                "module");
    EXPECT_SEQ("(MODULE:a", "(NET_DECL.wire", "(DECLARATOR:u", "(MODULE:b", "(NET_DECL.wor",
               "(DECLARATOR:v");
    /* resetall restores wire */
    run_clean("`default_nettype wand\n`resetall\nmodule a(output y);\n  assign y = u;\n"
              "endmodule\n");
    EXPECT_SEQ("(NET_DECL.wire", "(DECLARATOR:u");
    /* the setting of an earlier unit's directive reaches the next */
    run_clean("`default_nettype wand\nmodule a(output y);\n  assign y = u;\nendmodule\n"
              "module t;\n  wire w;\n  a ua (.y(w));\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wand");
}

/* --- unnamed blocks (EL-23) ---------------------------------------------------------------- */

static void test_unnamed_blocks(void) {
    run_clean("module a(input x, output reg y);\n  always @* begin reg t; t = x; y = t; end\n"
              "  always @* begin reg t; t = x; y = t; end\n  always @* begin : n reg t; end\n"
              "  always @* begin y = x; end\nendmodule\n"
              "module t(input x, output y);\n  a u (.x(x), .y(y));\n"
              "  reg z;\n  always @* begin reg t; z = t; end\nendmodule\n");
    expect_warn("t.v:2:13: warning: declaration in an unnamed block is a SystemVerilog "
                "extension; the block is named '$unnamed_block$1'");
    expect_warn("t.v:3:13: warning: declaration in an unnamed block is a SystemVerilog "
                "extension; the block is named '$unnamed_block$2'");
    expect_warn("t.v:10:13: warning: declaration in an unnamed block is a SystemVerilog "
                "extension; the block is named '$unnamed_block$1'");
    EXPECT_SEQ("(MODULE:a", "(SEQ_BLOCK:$unnamed_block$1", "(VAR_DECL.reg", "(DECLARATOR:t",
               "(SEQ_BLOCK:$unnamed_block$2", "(SEQ_BLOCK:n", "(SEQ_BLOCK @t.v:5");
    TEST_ASSERT_EQUAL_UINT32(3, rs.warnings);
}

/* I1: a local (block reg, function port or local) hides a genvar of the same name. */
static void test_local_hides_genvar(void) {
    run_clean("module m(input a, output reg y);\n  genvar i;\n"
              "  for (i = 0; i < 2; i = i + 1) begin : g\n"
              "    function f;\n      input i;\n      f = i;\n    endfunction\n"
              "    always @* begin : b\n      reg i;\n      i = a;\n      y = i;\n    end\n"
              "  end\nendmodule\n");
    EXPECT_SEQ("(FUNCTION_DECL:f", "(DECLARATOR:i", "(BLOCKING_ASSIGN", "(IDENT:f", "(IDENT:i",
               "(SEQ_BLOCK:b", "(DECLARATOR:i", "(BLOCKING_ASSIGN", "(IDENT:i", "(IDENT:a",
               "(BLOCKING_ASSIGN", "(IDENT:y", "(IDENT:i");
    /* outside the local's scope the genvar is read again */
    run_clean("module m(input a, output reg [1:0] y);\n  genvar i;\n"
              "  for (i = 0; i < 2; i = i + 1) begin : g\n"
              "    always @* begin : b reg i; i = a; end\n    always @* y[0] = i;\n"
              "  end\nendmodule\n");
    EXPECT_SEQ("(SEQ_BLOCK:b", "(IDENT:i", "(SELECT.bit", "(NUMBER.dec [SIGNED] @t.v:5:", "32'sd0");
}

/* I2: the unnamed-block number belongs to the source block; generate iterations share it. */
static void test_unnamed_generate_shares_k(void) {
    run_clean("module m(input a, output reg y);\n  genvar i;\n"
              "  for (i = 0; i < 2; i = i + 1) begin : g\n"
              "    always @* begin reg t; t = a; y = t; end\n  end\n"
              "  always @* begin reg u; u = a; y = u; end\nendmodule\n");
    EXPECT_SEQ("(GEN_BLOCK:g", "(SEQ_BLOCK:$unnamed_block$1", "(GEN_BLOCK:g",
               "(SEQ_BLOCK:$unnamed_block$1", "(SEQ_BLOCK:$unnamed_block$2");
    expect_absent("$unnamed_block$3");
    expect_warn("t.v:4:15: warning: declaration in an unnamed block is a SystemVerilog "
                "extension; the block is named '$unnamed_block$1'");
    expect_warn("t.v:6:13: warning: declaration in an unnamed block is a SystemVerilog "
                "extension; the block is named '$unnamed_block$2'");
    TEST_ASSERT_EQUAL_UINT32(2, rs.warnings);
}

/* m2: a negative repeat count is 0; a local going out of scope ends its hiding; an undeclared
 * name in a part-select's lsb is an error, not an implicit net. */
static void test_scope_exit_and_folds(void) {
    run_clean("module m(input a, output reg y);\n  always @* begin repeat (-1) y = a; end\n"
              "endmodule\n");
    EXPECT_SEQ("(REPEAT", "(NUMBER.dec [SIGNED]", "32'sd0");
    run_clean("module m(input a, output reg y);\n"
              "  always @* begin : b1 reg t; t = a; end\n  always @* begin : b2 y = t; end\n"
              "endmodule\n");
    EXPECT_SEQ("(NET_DECL.wire", "(DECLARATOR:t", "{(ATTR:implicit");
    expect_warn("t.v:3:28: warning: 't' is implicitly declared");
    run_error("module m(input [7:0] v, output y);\n  assign y = v[1:q];\nendmodule\n",
              "t.v:2:18: error: 'q' is not declared");
}

/* m1: an untyped function or task port is a wire (NET = 1 + wire), like a module port. */
static void test_subroutine_port_net(void) {
    run_clean("module m(input a, output y);\n  function f; input x; f = x; endfunction\n"
              "  assign y = f(a);\nendmodule\n");
    EXPECT_SEQ("(FUNCTION_DECL:f", "(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:x");
}

/* --- subroutines --------------------------------------------------------------------------- */

static void test_function_shape(void) {
    run_clean("module m(input [3:0] x, output [3:0] y);\n  function [3:0] f;\n    input [3:0] a;\n"
              "    input b, c;\n    reg t;\n    integer k;\n    begin t = b; f = a; end\n"
              "  endfunction\n  assign y = f(x, 1'b0, 1'b1);\nendmodule\n");
    EXPECT_SEQ("(FUNCTION_DECL:f", "(RANGE", "(LIST", "(PORT_DECL.input [IN_HEADER NET=1]",
               "(RANGE", "(DECLARATOR:a", "(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:b",
               "(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:c", "(LIST", "(VAR_DECL.reg",
               "(DECLARATOR:t", "(VAR_DECL.integer", "(DECLARATOR:k", "(SEQ_BLOCK");
    run_clean(
        "module m(input x);\n  task tk(input a, output b);\n    reg t;\n    begin t = a; b = t; "
        "end\n  endtask\n  task tn;\n    input [1:0] p, q;\n    reg r;\n    r = p[0];\n  "
        "endtask\n  reg v;\n  always @(posedge x) tk(1'b1, v);\nendmodule\n");
    EXPECT_SEQ("(TASK_DECL:tk", "(PORT_DECL.input [IN_HEADER NET=1]", "(DECLARATOR:a",
               "(PORT_DECL.output [IN_HEADER NET=1]", "(DECLARATOR:b", "(VAR_DECL.reg",
               "(TASK_DECL:tn", "(DECLARATOR:p", "(DECLARATOR:q", "(VAR_DECL.reg", "(TASK_CALL:tk");
}

/* --- constant folds (point (bb)) ----------------------------------------------------------- */

static void test_folds(void) {
    run_clean("module m(input a, input [7:0] v, input [2:0] i);\n  localparam N = 3;\n"
              "  localparam P = 2;\n  localparam W = 2;\n  wire [$clog2(16)-1:0] w;\n"
              "  reg [3:0] r;\n  reg [7:0] o;\n"
              "  always @* begin\n    repeat (N) r = r + 1;\n    o = {N{a}};\n"
              "    o = v[P+1:P];\n    o = v[i +: W];\n    o = v[i -: W];\n    o = $clog2(N);\n"
              "  end\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wire", "(RANGE", "(NUMBER.dec [SIGNED]", "32'sd3", "32'sd0", "(REPEAT",
               "(NUMBER.dec [SIGNED]", "32'sd3", "(REPLICATE", "32'sd3", "(SELECT.part", "(IDENT:v",
               "32'sd3", "32'sd2", "(SELECT.part_plus", "(IDENT:i", "32'sd2", "(SELECT.part_minus",
               "32'sd2", "(NUMBER.dec [SIGNED]", "32'sd2");
    expect_absent("CALL");
}

static void test_fold_errors(void) {
    run_error("module m(input a, output [3:0] y);\n  assign y = {-1{a}};\nendmodule\n",
              "t.v:2:15: error: replication count -1 is negative");
    run_error("module m(input a, output [3:0] y);\n  assign y = {2'bx1{a}};\nendmodule\n",
              "t.v:2:15: error: replication count has x or z bits");
    run_error("module m(input a, output [3:0] y);\n  assign y = {0{a}};\nendmodule\n",
              "t.v:2:15: error: zero-width concatenation");
    run_clean("module m(input a, output [3:0] y);\n  assign y = {a, {0{a}}};\nendmodule\n");
    run_error("module m(input a);\n  wire w;\n  wire [w:0] x;\nendmodule\n",
              "t.v:3:9: error: range bound is not constant (it reads 'w')");
    run_error("module m(input [3:0] a, output y);\n  wire w;\n  assign y = a[w +: w];\nendmodule\n",
              "t.v:3:21: error: range bound is not constant (it reads 'w')");
    run_error(
        "module m(input a);\n  reg [3:0] r;\n  wire w;\n  always @* repeat (w) r = 0;\nendmodule\n",
        "t.v:4:21: error: repeat count is not constant (it reads 'w')");
}

static void test_attrs(void) {
    run_clean(
        "module m(input a);\n  parameter P = 3;\n  (* weight = P * 2, name = \"n\", bare *)\n"
        "  wire w;\n  (* full_case *) reg [1:0] s;\n  always @* (* parallel_case *) case (s)\n"
        "    0: ;\n    default: ;\n  endcase\nendmodule\n");
    EXPECT_SEQ("(NET_DECL.wire", "(DECLARATOR:w", "{(ATTR:weight", "(NUMBER.bin [SIGNED]",
               "00000110", "(ATTR:name", "(STRING:\"n\"", "(ATTR:bare");
    run_error("module m(input a);\n  wire w;\n  (* k = w *) wire z;\nendmodule\n",
              "t.v:3:10: error: attribute value is not constant (it reads 'w')");
}

static void test_genvar_read(void) {
    run_clean(
        "module m(output reg [3:0] y);\n  genvar g;\n  for (g = 0; g < 3; g = g + 1) begin : lp\n"
        "    always @* y[g] = g;\n  end\nendmodule\n");
    EXPECT_SEQ("(GEN_BLOCK:lp", "(ALWAYS.always", "(SELECT.bit",
               "(NUMBER.dec [SIGNED] @t.v:4:17 32'sd0", "(NUMBER.dec [SIGNED] @t.v:4:22 32'sd0",
               "(GEN_BLOCK:lp", "(NUMBER.dec [SIGNED] @t.v:4:17 32'sd1", "32'sd1", "(GEN_BLOCK:lp",
               "32'sd2");
}

static void test_m11(void) {
    run_error("module m;\n  wire x;\n  reg x;\nendmodule\n",
              "t.v:3:7: error: 'x' is already declared (first at t.v:2:8)");
    run_error("module m;\n  reg x;\n  integer x;\nendmodule\n",
              "t.v:3:11: error: 'x' is already declared (first at t.v:2:7)");
    run_error("module m;\n  wire x;\n  wire x;\nendmodule\n",
              "t.v:3:8: error: 'x' is already declared (first at t.v:2:8)");
    run_error("module m;\n  wire f;\n  function f; input a; f = a; endfunction\nendmodule\n",
              "t.v:3:3: error: 'f' is already declared (first at t.v:2:8)");
}

/* Every statement kind of the elaborated form, written and checked (FULL, binder). */
static void test_statement_kinds(void) {
    run_clean(
        "module m(input c, input [1:0] s, output reg [3:0] y);\n  integer i;\n"
        "  initial y = 0;\n  always @(posedge c or negedge s[0]) begin\n"
        "    if (c) y <= 1; else y <= 2;\n    case (s) 2'd0, 2'd1: y <= 3; default: ; endcase\n"
        "    for (i = 0; i < 2; i = i + 1) y <= y + i;\n    ;\n  end\n"
        "  function f; input a; begin while (a) a = 0; f = a; end endfunction\n"
        "endmodule\n");
    EXPECT_SEQ("(INITIAL", "(BLOCKING_ASSIGN", "(ALWAYS.always", "(TIMING_STMT", "(EVENT_CONTROL",
               "(EVENT_EXPR.posedge", "(EVENT_EXPR.negedge", "(SEQ_BLOCK", "(IF",
               "(NONBLOCKING_ASSIGN", "(CASE.case", "(CASE_ITEM", "(CASE_ITEM [DEFAULT]", "(FOR",
               "(NULL_STMT", "(FUNCTION_DECL:f", "(WHILE");
}

/* $readmemh/$readmemb are kept in an initial block (S4); elsewhere a system task is Task 16's. */
static void test_readmem(void) {
    run_clean(
        "module m;\n  reg [7:0] mem [0:3];\n  initial $readmemh(\"f.hex\", mem);\nendmodule\n");
    EXPECT_SEQ("(INITIAL", "(TASK_CALL:$readmemh", "(STRING:\"f.hex\"", "(IDENT:mem");
    run_one(
        "module m;\n  reg [7:0] mem [0:3];\n  always @* $readmemh(\"f.hex\", mem);\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:13: error: elaboration is not available yet (2D): 'TASK_CALL'");
}

/* What Task 16 still owns stays a located pending line, never a 2D bug. */
static void test_task16_stays_pending(void) {
    run_one("module m(input c);\n  reg r;\n  always r = 1;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: elaboration is not available yet (2D): 'ALWAYS'");
    run_one("module m(input c);\n  reg r;\n  always @(c) $display(\"x\");\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:15: error: elaboration is not available yet (2D): 'TASK_CALL'");
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

/* Ports, implicit nets, blocks, subroutines, attributes, folds: every allocation fails in turn. */
static void test_oom(void) {
    run_clean(
        "`default_nettype wand\nmodule m(a, q, r);\n  input a;\n  input reg r;\n  output [3:0] q;\n"
        "  reg [3:0] q;\n  localparam N = 2;\n  (* k = N *) wire [$clog2(8)-1:0] w;\n"
        "  assign w = a & b;\n  function [3:0] f;\n    input [3:0] x;\n    reg t;\n"
        "    begin t = x[0]; f = x; end\n  endfunction\n  task tk(input z); begin end endtask\n"
        "  always @* begin reg t; t = a; repeat (N) q = {N{a}}; q = f(q); end\n"
        "  initial q = 0;\nendmodule\n");
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
    RUN_TEST(test_m1_merge);
    RUN_TEST(test_m1_net_kind_and_order);
    RUN_TEST(test_ansi_redeclared);
    RUN_TEST(test_m1_range_conflict);
    RUN_TEST(test_m1_signed);
    RUN_TEST(test_port_lists);
    RUN_TEST(test_untyped_ports);
    RUN_TEST(test_m12);
    RUN_TEST(test_m6);
    RUN_TEST(test_implicit_nets);
    RUN_TEST(test_implicit_flag);
    RUN_TEST(test_nettype_kinds);
    RUN_TEST(test_implicit_places);
    RUN_TEST(test_nettype_placement);
    RUN_TEST(test_unnamed_blocks);
    RUN_TEST(test_local_hides_genvar);
    RUN_TEST(test_unnamed_generate_shares_k);
    RUN_TEST(test_scope_exit_and_folds);
    RUN_TEST(test_subroutine_port_net);
    RUN_TEST(test_function_shape);
    RUN_TEST(test_folds);
    RUN_TEST(test_fold_errors);
    RUN_TEST(test_attrs);
    RUN_TEST(test_genvar_read);
    RUN_TEST(test_m11);
    RUN_TEST(test_statement_kinds);
    RUN_TEST(test_readmem);
    RUN_TEST(test_task16_stays_pending);
    RUN_TEST(test_oom);
    return UNITY_END();
}
