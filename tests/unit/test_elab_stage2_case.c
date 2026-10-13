/*
 * test_elab_stage2_case.c — 2D Task 22: `if`, `case`, `casez`, `casex`, the case temporaries and
 * the switch flags of EL-36, and the case method for a non-constant lvalue select (EL-32 writes).
 * Each `$proc` body is asserted exactly through a text dump of the node (the pins as I<k> / O<k>
 * in net-ID order; every case, switch and sync in IR-19's canonical layout; a compare lists its
 * bits least significant first, `-` is ANY), the whole read_verilog pass runs through the pass
 * manager (elab_test.h), and every located diagnostic is asserted as the whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report (read_verilog; write_rtlil for the process text, proc for the lowering), and the
 * CTest elab_case_bridge runs elab-equiv on the sources under tests/verilog/case-bridge.
 */
#include "elab_test.h"
#include "proc_test.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "frontends/elab/pg_internal.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32 };

void setUp(void) {
    memset(&pt_rs, 0, sizeof pt_rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_elab_test_set_hooks(NULL);
    odin3_log_set_sink(NULL, NULL);
    if (pt_rs.design != NULL || pt_rs.dir[0] != '\0') {
        et_free(&pt_rs);
    }
}

/* --- if / else (EL-36) ------------------------------------------------------------------------ */

/* `if (s) y = a; else y = b;`: t1 for y, a switch on s, case 1'1 and the default (the brief). */
static void test_if_else_default_mode(void) {
    pt_read_clean("module t(input s, a, b, output reg y);\n"
                  "  always @* if (s) y = a; else y = b;\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I3) [O2<-I1]\n"
                      "C2 [O2<-I2]\n"
                      "W0 (I0) f0 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 0, pt_wnet("s", 0));
    pt_expect_in(proc, 1, pt_wnet("a", 0));
    pt_expect_in(proc, 2, pt_wnet("b", 0));
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_1));
    pt_expect_out(proc, 0, pt_wnet("y", 0));
    TEST_ASSERT_TRUE(pt_out_net(proc, 1).v > pt_wnet("y", 0).v &&
                     pt_out_net(proc, 2).v > pt_out_net(proc, 1).v);
}

/* Latch mode: the added default holds the output (a latch path); the root default is replaced. */
static void test_if_latch_default_mode(void) {
    pt_read_clean("module t(input s, a, output reg y);\n  always @* if (s) y = a;\nendmodule\n");
    pt_expect_dump(0, "I3 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-O0]\n"
                      "W0 (I0) f0 C1+2\n"
                      "S always [O0<-O1]\n");
}

/* --nolatches: no root default; y reads x until assigned, so the added default holds 1'x. */
static void test_if_latch_nolatches(void) {
    pass_src nolatches = {"--nolatches t.v", "module t(input s, a, output reg y);\n"
                                             "  always @* if (s) y = a;\nendmodule\n"};
    pt_read_with(&nolatches);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_ir_ok();
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-I3]\n"
                      "W0 (I0) f0 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 2, pt_const_net_of(ODIN3_BIT_1));
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_X));
}

/* A condition wider than a bit is $reduce_bool(c). */
static void test_if_wide_condition(void) {
    pt_read_clean("module t(input [2:0] s, input a, output reg y);\n"
                  "  always @* if (s) y = a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$reduce_bool"));
    odin3_node_id reduce = pt_nth_node("$reduce_bool", 0);
    odin3_pinslice y_pins = odin3_node_port(pt_top(), reduce, 1);
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I3 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I0]\n"
                      "C2 [O2<-O0]\n"
                      "W0 (I1) f0 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 0, pt_wnet("a", 0));
    pt_expect_in(proc, 1, odin3_pin_net(pt_top(), y_pins.first));
}

/* Non-blocking assignments write the root temporary: no t1, no added default. */
static void test_nonblocking_branch_has_no_case_temporary(void) {
    pt_read_clean("module t(input c, e, d, output reg q);\n"
                  "  always @(posedge c) if (e) q <= d;\nendmodule\n");
    pt_expect_dump(0, "I4 O2\n"
                      "C0 [O1<-O0] sw0+1\n"
                      "C1 cmp(I3) [O1<-I2]\n"
                      "W0 (I1) f0 C1+1\n"
                      "S posedge I0 [O0<-O1]\n");
}

/* --- case, casez, casex (EL-36) --------------------------------------------------------------- */

static const char k_case_items[] =
    "module t(input [1:0] sel, input a, b, c, output reg y);\n"
    "  always @* case (sel) 2'd0: y = a; 2'd1, 2'd2: y = b; default: y = c; endcase\n"
    "endmodule\n";

/* The items in source order, one case per item with its compares, the default last. */
static void test_case_items_and_default(void) {
    pt_read_clean(k_case_items);
    pt_expect_dump(0, "I7 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I5,I5) [O2<-I2]\n"
                      "C2 cmp(I6,I5|I5..I6) [O2<-I3]\n"
                      "C3 [O2<-I4]\n"
                      "W0 (I0..I1) f0 C1+3\n"
                      "S always [O0<-O1]\n");
}

/* The default written first is still the last case (C6). */
static void test_case_default_first_is_last(void) {
    pt_read_clean("module t(input [1:0] sel, input a, b, c, output reg y);\n"
                  "  always @* case (sel) default: y = c; 2'd0: y = a; 2'd1, 2'd2: y = b; endcase\n"
                  "endmodule\n");
    pt_expect_dump(0, "I7 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I5,I5) [O2<-I2]\n"
                      "C2 cmp(I6,I5|I5..I6) [O2<-I3]\n"
                      "C3 [O2<-I4]\n"
                      "W0 (I0..I1) f0 C1+3\n"
                      "S always [O0<-O1]\n");
}

/* C8 (review M10): `case (1'b1)` with non-constant items compares on the items' own pins. */
static void test_case_non_constant_items(void) {
    pt_read_clean("module t(input a, b, output reg y);\n"
                  "  always @* case (1'b1) a: y = 1'b1; b: y = 1'b0; endcase\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I0) [O2<-I2]\n"
                      "C2 cmp(I1) [O2<-I3]\n"
                      "C3 [O2<-O0]\n"
                      "W0 (I2) f0 C1+3\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 0, pt_wnet("a", 0));
    pt_expect_in(proc, 1, pt_wnet("b", 0));
    pt_expect_in(proc, 2, pt_const_net_of(ODIN3_BIT_1));
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_0));
}

/* Wildcards: casez `?`, casex `x`; the plain case keeps x as a bit (C9). Bits least significant
 * first, so `2'b1?` is [ANY, 1]. */
static void test_wildcards(void) {
    pt_read_clean("module t(input [1:0] sel, input a, output reg y);\n"
                  "  always @* casez (sel) 2'b1?: y = a; default: y = 1'b0; endcase\nendmodule\n");
    pt_expect_dump(0, "I5 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(-,I3) [O2<-I2]\n"
                      "C2 [O2<-I4]\n"
                      "W0 (I0..I1) f0 C1+2\n"
                      "S always [O0<-O1]\n");
}

static void test_casex_wildcard(void) {
    pt_read_clean("module t(input [1:0] sel, input a, output reg y);\n"
                  "  always @* casex (sel) 2'bx1: y = a; default: y = 1'b0; endcase\nendmodule\n");
    pt_expect_dump(0, "I5 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I3,-) [O2<-I2]\n"
                      "C2 [O2<-I4]\n"
                      "W0 (I0..I1) f0 C1+2\n"
                      "S always [O0<-O1]\n");
}

static void test_plain_case_keeps_x(void) {
    pt_read_clean("module t(input [1:0] sel, input a, output reg y);\n"
                  "  always @* case (sel) 2'bx1: y = a; default: y = 1'b0; endcase\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I6 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I3..I4) [O2<-I2]\n"
                      "C2 [O2<-I5]\n"
                      "W0 (I0..I1) f0 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_1));
    pt_expect_in(proc, 4, pt_const_net_of(ODIN3_BIT_X));
    pt_expect_in(proc, 5, pt_const_net_of(ODIN3_BIT_0));
}

/* `case (a4) 3'd1:` compares at the widest width: 4 bits. */
static void test_case_width(void) {
    pt_read_clean("module t(input [3:0] a4, input a, output reg y);\n"
                  "  always @* case (a4) 3'd1: y = a; endcase\nendmodule\n");
    pt_expect_dump(0, "I7 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I5..I6,I6,I6) [O2<-I4]\n"
                      "C2 [O2<-O0]\n"
                      "W0 (I0..I3) f0 C1+2\n"
                      "S always [O0<-O1]\n");
}

/* v4.2 chunks: an 8-bit y is one 8-bit action in every case, the added default and the exit. */
static void test_chunks_case_default_item(void) {
    pt_read_clean("module t(input [1:0] s, input [7:0] a8, b8, c8, output reg [7:0] y);\n"
                  "  always @* case (s) 2'd0: y = a8; 2'd1: y = b8; default: y = c8; endcase\n"
                  "endmodule\n");
    pt_expect_dump(0, "I28 O24\n"
                      "C0 [O8..O15<-O16..O23] sw0+1\n"
                      "C1 cmp(I26,I26) [O16..O23<-I2..I9]\n"
                      "C2 cmp(I27,I26) [O16..O23<-I10..I17]\n"
                      "C3 [O16..O23<-I18..I25]\n"
                      "W0 (I0..I1) f0 C1+3\n"
                      "S always [O0..O7<-O8..O15]\n");
}

static void test_chunks_case_added_default(void) {
    pt_read_clean("module t(input [1:0] s, input [7:0] a8, b8, output reg [7:0] y);\n"
                  "  always @* case (s) 2'd0: y = a8; 2'd1: y = b8; endcase\nendmodule\n");
    pt_expect_dump(0, "I20 O24\n"
                      "C0 [O8..O15<-O16..O23] sw0+1\n"
                      "C1 cmp(I18,I18) [O16..O23<-I2..I9]\n"
                      "C2 cmp(I19,I18) [O16..O23<-I10..I17]\n"
                      "C3 [O16..O23<-O0..O7]\n"
                      "W0 (I0..I1) f0 C1+3\n"
                      "S always [O0..O7<-O8..O15]\n");
}

/* --- nesting (internal risk 16 #3) ------------------------------------------------------------ */

/* Each switch has its own t1 and the outer state is restored after the inner one. */
static void test_nested_if(void) {
    pt_read_clean("module t(input a, b, output reg y);\n"
                  "  always @* if (a) begin y = 1'b1; if (b) y = 1'b0; end\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O4\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-O3] sw1+1\n"
                      "C2 [O2<-O0]\n"
                      "C3 cmp(I2) [O3<-I3]\n"
                      "C4 [O3<-I2]\n"
                      "W0 (I0) f0 C1+2\n"
                      "W1 (I1) f0 C3+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 2, pt_const_net_of(ODIN3_BIT_1));
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_0));
}

/* A blocking read after the switch sees t1 (rv): `z = y` takes the switch's temporary. */
static void test_read_after_switch_sees_t1(void) {
    pt_read_clean("module t(input s, a, b, output reg y, z);\n"
                  "  always @* begin if (s) y = a; else y = b; z = y; end\nendmodule\n");
    pt_expect_dump(0, "I4 O5\n"
                      "C0 [O2<-O4 O3<-O4] sw0+1\n"
                      "C1 cmp(I3) [O4<-I1]\n"
                      "C2 [O4<-I2]\n"
                      "W0 (I0) f0 C1+2\n"
                      "S always [O0<-O2 O1<-O3]\n");
}

/* --- full_case, parallel_case (C10, D1) --------------------------------------------------------
 */

static const char k_full_case[] = "module t(input s, a, output reg y);\n"
                                  "  always @* (* full_case *) case (s) 1'b0: y = a; endcase\n"
                                  "endmodule\n";

/* The flag, the added default holding 1'x, and D1's warning (2D emits it, not 2E). */
static void test_full_case_attribute(void) {
    pt_read_src(k_full_case);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_ir_ok();
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-I3]\n"
                      "W0 (I0) f1 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_warning((at_loc){2, 16},
                      "'full_case' is honoured: case values no item lists assign x");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_soft_warnings(), et_log(&pt_rs));
}

/* The metacomment form: both flags, both warnings. */
static void test_synopsys_metacomment_both_flags(void) {
    pt_read_src("module t(input s, a, output reg y);\n"
                "  always @* case (s) // synopsys full_case parallel_case\n"
                "    1'b0: y = a;\n  endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_ir_ok();
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-I3]\n"
                      "W0 (I0) f3 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_warning((at_loc){2, 22},
                      "'full_case' is honoured: case values no item lists assign x");
    pt_expect_warning((at_loc){2, 22},
                      "'parallel_case' is honoured: case items are not prioritised");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, pt_soft_warnings(), et_log(&pt_rs));
}

/* parallel_case alone sets its flag only; `= 0` is no flag at all. */
static void test_parallel_case_and_false_attribute(void) {
    pt_read_src("module t(input s, a, output reg y);\n"
                "  always @* (* parallel_case *) case (s) 1'b0: y = a; endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_dump(0, "I3 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-O0]\n"
                      "W0 (I0) f2 C1+2\n"
                      "S always [O0<-O1]\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("'parallel_case' is honoured"));
    et_free(&pt_rs);
    memset(&pt_rs, 0, sizeof pt_rs);
    pt_read_clean("module t(input s, a, output reg y);\n"
                  "  always @* (* full_case = 0 *) case (s) 1'b0: y = a; endcase\nendmodule\n");
    pt_expect_dump(0, "I3 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I1]\n"
                      "C2 [O2<-O0]\n"
                      "W0 (I0) f0 C1+2\n"
                      "S always [O0<-O1]\n");
}

/* --- constants fold (the brief; Yosys folds only what the AST makes constant) ---------------- */

static void test_constant_if_folds(void) {
    pt_read_clean("module t(input a, b, output reg y);\n"
                  "  always @* if (1) y = a; else y = b;\nendmodule\n");
    pt_expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    pt_expect_in(pt_proc_node(0), 0, pt_wnet("a", 0));
    et_free(&pt_rs);
    memset(&pt_rs, 0, sizeof pt_rs);
    pt_read_clean("module t(input a, b, output reg y);\n"
                  "  always @* if (1'b0) y = a; else y = b;\nendmodule\n");
    pt_expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    pt_expect_in(pt_proc_node(0), 0, pt_wnet("b", 0));
}

/* A parameter is constant; the dead branch's output is no output (Yosys simplifies it away). */
static void test_constant_parameter_and_dead_output(void) {
    pt_read_clean("module t(input a, b, output reg y, z);\n  parameter P = 2;\n"
                  "  always @* if (P > 1) y = a; else z = b;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    pt_expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    pt_expect_out(pt_proc_node(0), 0, pt_wnet("y", 0));
}

static void test_constant_case_folds(void) {
    pt_read_clean("module t(input a, b, c, output reg y);\n"
                  "  always @* case (2'd1) 2'd0: y = a; 2'd1: y = b; default: y = c; endcase\n"
                  "endmodule\n");
    pt_expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    pt_expect_in(pt_proc_node(0), 0, pt_wnet("b", 0));
    et_free(&pt_rs);
    memset(&pt_rs, 0, sizeof pt_rs);
    pt_read_clean("module t(input a, b, c, output reg y);\n"
                  "  always @* case (2'd3) 2'd0: y = a; 2'd1: y = b; default: y = c; endcase\n"
                  "endmodule\n");
    pt_expect_in(pt_proc_node(0), 0, pt_wnet("c", 0));
}

/* No item matches and there is no default: nothing runs, so the block has no output. */
static void test_constant_case_no_match_makes_no_node(void) {
    pt_read_clean("module t(input a, output reg y);\n"
                  "  always @* case (2'd3) 2'd0: y = a; endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$proc"));
}

/* An x condition is not folded: proc decides (the switch is kept on the x constant). */
static void test_x_condition_is_not_folded(void) {
    pt_read_clean("module t(input a, b, output reg y);\n"
                  "  always @* if (1'bx) y = a; else y = b;\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O3\n"
                      "C0 [O1<-O2] sw0+1\n"
                      "C1 cmp(I2) [O2<-I0]\n"
                      "C2 [O2<-I1]\n"
                      "W0 (I3) f0 C1+2\n"
                      "S always [O0<-O1]\n");
    pt_expect_in(proc, 3, pt_const_net_of(ODIN3_BIT_X));
}

/* A variable that holds a constant is not constant by the AST: the switch stays. */
static void test_variable_holding_a_constant_is_not_folded(void) {
    pt_read_clean("module t(input a, b, output reg y);\n  reg s;\n"
                  "  always @* begin s = 1'b1; if (s) y = a; else y = b; end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    TEST_ASSERT_TRUE(strstr(pt_dump(pt_proc_node(0)), "sw") != NULL);
}

/* --- nesting depth: no recursion ---------------------------------------------------------------
 */

enum { CHAIN_LENGTH = 400, CHAIN_TEXT = 1 << 15 };

/* An else-if chain of CHAIN_LENGTH tests nests that many switches; frames, not the C stack. */
static void test_deep_else_if_chain(void) {
    static char text[CHAIN_TEXT];
    size_t len = 0;
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "module t(input [15:0] s, output reg [15:0] y);\n  always @* begin\n"
                            "    if (s == 16'd0) y = 16'd0;\n");
    for (unsigned k = 1; k < CHAIN_LENGTH; k++) {
        len += (size_t)snprintf(text + len, sizeof text - len,
                                "    else if (s == 16'd%u) y = 16'd%u;\n", k, k);
        TEST_ASSERT_TRUE(len < sizeof text);
    }
    len +=
        (size_t)snprintf(text + len, sizeof text - len, "    else y = 16'd7;\n  end\nendmodule\n");
    TEST_ASSERT_TRUE(len < sizeof text);
    pt_read_clean(text);
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    odin3_proc_body storage;
    const odin3_proc_body *body = pt_body_of(pt_proc_node(0), &storage);
    TEST_ASSERT_EQUAL_UINT32(CHAIN_LENGTH, body->n_switches);
    TEST_ASSERT_EQUAL_UINT32(2U * CHAIN_LENGTH + 1U, body->n_cases);
}

/* --- non-constant lvalue selects: the case method (EL-32 writes) -------------------------------
 */

static void test_variable_index_write(void) {
    pt_read_clean("module t(input c, input [1:0] s, input d, output reg [3:0] y);\n"
                  "  always @(posedge c) y[s] <= d;\nendmodule\n");
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I6 O10\n"
                      "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                      "C1 cmp(I4,I4) [O4<-I3]\n"
                      "C2 cmp(I5,I4) [O5<-I3]\n"
                      "C3 cmp(I4..I5) [O6<-I3]\n"
                      "C4 cmp(I5,I5) [O7<-I3]\n"
                      "W0 (O8..O9) f0 C1+4\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
    pt_expect_in(proc, 3, pt_wnet("d", 0));
    for (uint32_t k = 0; k < 4; k++) {
        pt_expect_out(proc, k, pt_wnet("y", k));
    }
}

/* b = 3 straddles the top of a [3:0] target: its case writes y[3] (d2[0]) only. */
static void test_indexed_part_select_write(void) {
    pt_read_clean("module t(input c, input [1:0] b, input [1:0] d2, output reg [3:0] y);\n"
                  "  always @(posedge c) y[b +: 2] <= d2;\nendmodule\n");
    pt_expect_dump(0, "I7 O10\n"
                      "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                      "C1 cmp(I5,I5) [O4..O5<-I3..I4]\n"
                      "C2 cmp(I6,I5) [O5..O6<-I3..I4]\n"
                      "C3 cmp(I5..I6) [O6..O7<-I3..I4]\n"
                      "C4 cmp(I6,I6) [O7<-I3]\n"
                      "W0 (O8..O9) f0 C1+4\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* [b -: w] covers b, b-1 .. b-w+1: b = 0 straddles the bottom (y[0] takes d2[1]) and b = 1..3 lie
 * inside a [3:0] target for a 2-bit base. A straddling slice writes the bits inside the object
 * (IEEE 1364-2005 4.2.1; the high end for `+:` and a signed base: spec 17 #54 has the one place
 * Yosys 0.55 differs). */
static void test_indexed_part_select_minus_write(void) {
    pt_read_clean("module t(input c, input [1:0] b, input [1:0] d2, output reg [3:0] y);\n"
                  "  always @(posedge c) y[b -: 2] <= d2;\nendmodule\n");
    pt_expect_dump(0, "I7 O10\n"
                      "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                      "C1 cmp(I5,I5) [O4<-I4]\n"
                      "C2 cmp(I6,I5) [O4..O5<-I3..I4]\n"
                      "C3 cmp(I5..I6) [O5..O6<-I3..I4]\n"
                      "C4 cmp(I6,I6) [O6..O7<-I3..I4]\n"
                      "W0 (O8..O9) f0 C1+4\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* The low end of a [7:4] target: s = 3 covers 3 and 4 and writes y[4] (d[0]); s = 7 covers 7 and 8
 * and writes y[7]. A 3-bit base takes 0..7, of which 3..7 reach the target. */
static void test_straddle_at_both_ends_of_an_offset_target(void) {
    pt_read_clean("module t(input c, input [2:0] s, input [1:0] d, output reg [7:4] y);\n"
                  "  always @(posedge c) y[s +: 2] <= d;\nendmodule\n");
    pt_expect_dump(0, "I8 O11\n"
                      "C0 [O4..O7<-O0..O3 O8..O10<-I1..I3] sw0+1\n"
                      "C1 cmp(I6,I6..I7) [O4<-I5]\n"
                      "C2 cmp(I7,I7,I6) [O4..O5<-I4..I5]\n"
                      "C3 cmp(I6..I7,I6) [O5..O6<-I4..I5]\n"
                      "C4 cmp(I7,I6,I6) [O6..O7<-I4..I5]\n"
                      "C5 cmp(I6,I6,I6) [O7<-I4]\n"
                      "W0 (O8..O10) f0 C1+5\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* A signed base holds negative values: s = -1 covers -1 and 0 and writes y[0] (d[1]); s = -2
 * covers -2 and -1, which is nothing, and has no case. */
static void test_signed_base_straddles_the_bottom(void) {
    pt_read_clean("module t(input c, input signed [1:0] s, input [1:0] d, output reg [3:0] y);\n"
                  "  always @(posedge c) y[s +: 2] <= d;\nendmodule\n");
    pt_expect_dump(0, "I7 O8\n"
                      "C0 [O3..O5<-O0..O2 O6..O7<-I1..I2] sw0+1\n"
                      "C1 cmp(I5,I5) [O3<-I4]\n"
                      "C2 cmp(I6,I6) [O3..O4<-I3..I4]\n"
                      "C3 cmp(I5..I6) [O4..O5<-I3..I4]\n"
                      "W0 (O6..O7) f0 C1+3\n"
                      "S posedge I0 [O0..O2<-O3..O5]\n");
}

/* An ascending target: y[s +: 2] of [0:3] covers s and s+1 and writes the declared bits inside it
 * (s = 3 writes y[3], the lowest position, d[0]). */
static void test_ascending_target_straddles(void) {
    pt_read_clean("module t(input c, input [1:0] s, input [1:0] d, output reg [0:3] y);\n"
                  "  always @(posedge c) y[s +: 2] <= d;\nendmodule\n");
    pt_expect_dump(0, "I7 O10\n"
                      "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                      "C1 cmp(I5,I5) [O6..O7<-I3..I4]\n"
                      "C2 cmp(I6,I5) [O5..O6<-I3..I4]\n"
                      "C3 cmp(I5..I6) [O4..O5<-I3..I4]\n"
                      "C4 cmp(I6,I6) [O4<-I4]\n"
                      "W0 (O8..O9) f0 C1+4\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* A blocking straddling write: the case temporaries cover every bit some position writes. */
static void test_blocking_straddle(void) {
    pt_read_clean("module t(input [1:0] s, input [1:0] d, output reg [3:0] y);\n"
                  "  always @* y[s +: 2] = d;\nendmodule\n");
    pt_expect_dump(0, "I6 O14\n"
                      "C0 [O8..O9<-I0..I1 O4..O7<-O10..O13] sw0+1\n"
                      "C1 cmp(I4,I4) [O12..O13<-O2..O3 O10..O11<-I2..I3]\n"
                      "C2 cmp(I5,I4) [O10,O13<-O0,O3 O11..O12<-I2..I3]\n"
                      "C3 cmp(I4..I5) [O10..O11<-O0..O1 O12..O13<-I2..I3]\n"
                      "C4 cmp(I5,I5) [O10..O12<-O0..O2 O13<-I2]\n"
                      "C5 [O10..O13<-O0..O3]\n"
                      "W0 (O8..O9) f0 C1+5\n"
                      "S always [O0..O3<-O4..O7]\n");
}

/* An ascending target maps the declared index to the bit position by EL-23. */
static void test_ascending_target(void) {
    pt_read_clean("module t(input c, input [1:0] s, input d, output reg [0:3] y);\n"
                  "  always @(posedge c) y[s] <= d;\nendmodule\n");
    pt_expect_dump(0, "I6 O10\n"
                      "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                      "C1 cmp(I4,I4) [O7<-I3]\n"
                      "C2 cmp(I5,I4) [O6<-I3]\n"
                      "C3 cmp(I4..I5) [O5<-I3]\n"
                      "C4 cmp(I5,I5) [O4<-I3]\n"
                      "W0 (O8..O9) f0 C1+4\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* A signed 2-bit index holds 0 and 1 of a [3:0] target: two cases, and only y[1:0] is an output
 * (Yosys 0.55 writes the same two items). */
static void test_signed_index_limits_the_positions(void) {
    pt_read_clean("module t(input c, input signed [1:0] s, input d, output reg [3:0] y);\n"
                  "  always @(posedge c) y[s] <= d;\nendmodule\n");
    pt_expect_dump(0, "I6 O6\n"
                      "C0 [O2..O3<-O0..O1 O4..O5<-I1..I2] sw0+1\n"
                      "C1 cmp(I4,I4) [O2<-I3]\n"
                      "C2 cmp(I5,I4) [O3<-I3]\n"
                      "W0 (O4..O5) f0 C1+2\n"
                      "S posedge I0 [O0..O1<-O2..O3]\n");
}

/* A blocking write is the switch of EL-36: case temporaries for every bit a position writes, and
 * the default that holds them (a latch path in default mode). */
static void test_blocking_variable_index_write(void) {
    pt_read_clean("module t(input [1:0] s, input d, output reg [3:0] y);\n"
                  "  always @* y[s] = d;\nendmodule\n");
    pt_expect_dump(0, "I5 O14\n"
                      "C0 [O8..O9<-I0..I1 O4..O7<-O10..O13] sw0+1\n"
                      "C1 cmp(I3,I3) [O11..O13<-O1..O3 O10<-I2]\n"
                      "C2 cmp(I4,I3) [O10,O12..O13<-O0,O2..O3 O11<-I2]\n"
                      "C3 cmp(I3..I4) [O10..O11,O13<-O0..O1,O3 O12<-I2]\n"
                      "C4 cmp(I4,I4) [O10..O12<-O0..O2 O13<-I2]\n"
                      "C5 [O10..O13<-O0..O3]\n"
                      "W0 (O8..O9) f0 C1+5\n"
                      "S always [O0..O3<-O4..O7]\n");
}

/* A select that can reach nothing inside its target writes nothing: warned, no output. */
static void test_index_that_selects_nothing(void) {
    pt_read_src("module t(input c, input [1:0] s, input d, output reg [7:4] y);\n"
                "  always @(posedge c) y[s] <= d;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_warning((at_loc){2, 23}, "assignment to bits outside 'y' [7:4] is ignored");
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$proc"));
}

/* A constant x or z index writes nothing, with the warning of any write outside the object
 * (IEEE 1364-2005 4.2.1; Yosys 0.55 writes bit 0, spec 17 #54): y keeps its value. */
static void test_x_index_write_is_ignored_with_a_warning(void) {
    pt_read_src("module t(input c, input d, output reg [3:0] y);\n"
                "  always @(posedge c) begin y <= 4'd5; y[1'bx] <= d; end\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_warning((at_loc){2, 40}, "assignment to bits outside 'y' [3:0] is ignored");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_soft_warnings(), et_log(&pt_rs));
    pt_expect_dump(0, "I3 O8\n"
                      "C0 [O4..O7<-I1..I2,I1..I2]\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* The same through a blocking write and a z index, nothing else written. */
static void test_z_index_blocking_write_is_ignored_with_a_warning(void) {
    pt_read_src("module t(input d, output reg [3:0] y);\n"
                "  always @* begin y = 4'd5; y[1'bz] = d; end\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_warning((at_loc){2, 29}, "assignment to bits outside 'y' [3:0] is ignored");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_soft_warnings(), et_log(&pt_rs));
    pt_expect_dump(0, "I2 O8\n"
                      "C0 [O4..O7<-I0..I1,I0..I1]\n"
                      "S always [O0..O3<-O4..O7]\n");
}

/* A non-constant select inside a concatenation: Yosys 0.55 silently drops the write (spec 17). */
static void test_variable_index_in_concatenation_is_an_error(void) {
    pt_read_failing("module t(input c, input [1:0] s, d2, output reg [3:0] y, output reg z);\n"
                    "  always @(posedge c) {y[s], z} <= d2;\nendmodule\n");
    pt_expect_error((at_loc){2, 24},
                    "non-constant index into 'y' is not supported in a concatenation target");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_diag_errors(), et_log(&pt_rs));
}

/* --- what is still pending (point (l)) ---------------------------------------------------------
 */

/* A memory write inside an if arm and inside a case arm is reported, not dropped (Task 24). */
static void test_memory_write_in_if_arm_is_pending(void) {
    pt_read_failing("module t(input c, e, input [1:0] i, input [7:0] d, output [7:0] y);\n"
                    "  reg [7:0] m [0:3];\n  assign y = m[i];\n"
                    "  always @(posedge c) if (e) m[i] <= d;\nendmodule\n");
    pt_expect_error((at_loc){4, 30}, "elaboration is not available yet (2D): 'memory write'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_diag_errors(), et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$proc"));
}

static void test_memory_write_in_case_arm_is_pending(void) {
    pt_read_failing("module t(input c, e, input [1:0] i, input [7:0] d, output [7:0] y);\n"
                    "  reg [7:0] m [0:3];\n  assign y = m[i];\n"
                    "  always @(posedge c) case (e) 1'b1: m[i] <= d; endcase\nendmodule\n");
    pt_expect_error((at_loc){4, 38}, "elaboration is not available yet (2D): 'memory write'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_diag_errors(), et_log(&pt_rs));
}

/* --- out of memory ----------------------------------------------------------------------------
 */

static bool g_saw_missing_file;
static bool g_saw_error; /* any ERROR-level line of the run */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    g_saw_error |= level == ODIN3_LOG_ERROR;
    g_saw_missing_file |= strstr(msg, "missing_file:") != NULL;
}

static void fresh_quiet_design(void) {
    odin3_design_destroy(pt_rs.design);
    pt_rs.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(pt_rs.design);
    odin3_log_set_sink(quiet_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
}

static bool read_complete(void) {
    return odin3_design_module_end(pt_rs.design) == 2 && odin3_design_top(pt_rs.design).v != 0;
}

/* A swallowed failure is an OK read that is incomplete or that logged an error. */
static void check_not_swallowed(odin3_status st) {
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE_MESSAGE(g_saw_error, "an OK read logged an error");
        TEST_ASSERT_TRUE_MESSAGE(read_complete(), "an OK read is incomplete");
    }
}

/* One run of the whole read with the limit-th allocation failing. */
static odin3_status read_failing_at(long limit) {
    fresh_quiet_design();
    g_saw_missing_file = false;
    g_saw_error = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(pt_rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    odin3_util_set_alloc_fail_after(-1);
    /* an injected allocation failure is NO_MEMORY, never a user error and never a missing file;
     * an OK read is complete and logged no error */
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_FALSE(g_saw_missing_file);
    check_not_swallowed(st);
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(pt_rs.design, full));
    return st;
}

static void oom_sweep(const char *text, uint32_t min_failures) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &pt_rs);
    uint32_t failures = 0;
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = read_failing_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK && read_complete() ? settled + 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    TEST_ASSERT_GREATER_THAN_UINT32(min_failures, failures);
}

/* if, nested if, case with temporaries, a variable-index write and a blocking one. */
static const char k_rich[] =
    "module t(input c, input [1:0] s, input [3:0] a, b, output reg [3:0] y, z, w, v);\n"
    "  always @(posedge c) begin\n"
    "    if (a[0]) begin y = a; if (b[1]) y = b; end else y = 4'd0;\n"
    "    case (s) 2'd0: z <= a; 2'd1, 2'd2: z <= b; default: z <= 4'd1; endcase\n"
    "    w[s] <= a[0];\n"
    "    v[b[1:0] +: 2] <= a[1:0];\n"
    "  end\n"
    "endmodule\n";

/* Flags, wildcards, folding and a blocking select inside a branch. */
static const char k_rich_flags[] =
    "module t(input [1:0] s, input [3:0] a, b, input e, output reg [3:0] y, z);\n"
    "  parameter P = 1;\n"
    "  always @* begin\n"
    "    (* full_case, parallel_case *) casez (s) 2'b1?: y = a; 2'b01: y = b; endcase\n"
    "    if (P) z = a; else z = b;\n"
    "    if (e) z[s] = b[0];\n"
    "  end\n"
    "endmodule\n";

/* Every allocation of the processes fails in turn: NO_MEMORY, a valid IR, no leak (ASan). */
static void test_oom_sweep(void) {
    oom_sweep(k_rich, 300);
}

static void test_oom_sweep_flags_and_folding(void) {
    oom_sweep(k_rich_flags, 300);
}

/* --- the builder primitives under allocation failure (white-box; review M2 of Task 21) ---------
 */

typedef struct wb_probe {
    bool ran;
    uint32_t failures;
    uint32_t emit_failures; /* of those, in the emission */
    uint32_t settled;
    odin3_ast_id lhs; /* the declarator of `y` */
} wb_probe;

static wb_probe g_wb;

static odin3_net_id wb_net(odin3_elab2 *e2) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &net));
    return net;
}

/* The DECLARATOR named y under the module. */
static odin3_ast_id wb_declarator(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, e2->elab, module_node));
    odin3_ast_event ev;
    odin3_ast_id node;
    odin3_ast_id found = {0};
    while (found.v == 0 && odin3_ast_cursor_next(&cur, &ev, &node)) {
        bool declarator =
            ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_DECLARATOR;
        found = declarator && strcmp(odin3_ast_name_str(e2->elab, node), "y") == 0 ? node : found;
    }
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, found.v);
    return found;
}

/* The primitives in use: a temporary, a switch on it, a case with a compare in which an
 * assignment to y[1:0] lands (lv set up before), the case ended, a default case. */
static odin3_status wb_sequence(odin3_pg *pg, odin3_ast_id module_node) {
    odin3_net_id first = {0};
    odin3_status st = odin3_pg_temp(pg, 2, module_node, &first);
    odin3_sigbit signal_bit = first.v;
    odin3_sig signal = {&signal_bit, 1, false};
    odin3_pg_switch_req sw = {module_node, &signal, 0};
    st = st != ODIN3_OK ? st : odin3_pg_switch(pg, &sw);
    odin3_sigbit one = odin3_sb_of_bit(ODIN3_BIT_1);
    odin3_sig compare = {&one, 1, false};
    odin3_pg_case_req item = {module_node, &compare, 1};
    st = st != ODIN3_OK ? st : odin3_pg_case(pg, &item);
    odin3_sigbit value[2] = {odin3_sb_of_bit(ODIN3_BIT_0), odin3_sb_of_bit(ODIN3_BIT_1)};
    odin3_sig rhs = {value, 2, false};
    odin3_pg_assign_req assign = {module_node, g_wb.lhs, &rhs, true};
    st = st != ODIN3_OK ? st : odin3_pg_assign(pg, &assign);
    odin3_pg_case_end(pg);
    odin3_pg_case_req dflt = {module_node, NULL, 0};
    st = st != ODIN3_OK ? st : odin3_pg_case(pg, &dflt);
    odin3_pg_case_end(pg);
    return st;
}

/* The live $proc nodes made since node id `first` (the shared constant nodes a failed emission
 * may have made are not processes). */
static uint32_t wb_new_procs(const odin3_elab2 *e2, uint32_t first) {
    uint32_t live = 0;
    for (uint32_t id = first; id < odin3_module_node_end(e2->mod); id++) {
        odin3_node_id node = {id};
        live += odin3_node_live(e2->mod, node) &&
                        odin3_node_type(e2->mod, node).v == e2->types[ODIN3_E2T_PROC].v
                    ? 1U
                    : 0U;
    }
    return live;
}

/* One iteration: a fresh generator, then the sequence and the emission with the limit-th allocation
 * (counted across both) failing. The result is OK or NO_MEMORY, and a $proc node exists exactly
 * when both went through: the emission is whole or makes nothing. */
static odin3_status wb_run(odin3_elab2 *e2, odin3_ast_id module_node, long limit) {
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    odin3_wire_id wire = odin3_module_find_wire(e2->mod, pt_id_of("y"));
    for (uint32_t k = 0; k < 2; k++) { /* lv(y[k]) = a fresh net, so the assignment makes actions */
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              pg_lv_set(pg, odin3_wire_net(e2->mod, wire, k), wb_net(e2)));
    }
    uint32_t nodes = odin3_module_node_end(e2->mod);
    odin3_node_id node = {0};
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = wb_sequence(pg, module_node);
    bool sequenced = st == ODIN3_OK;
    st = st != ODIN3_OK ? st : odin3_pg_emit(pg, module_node, &node);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    g_wb.emit_failures += sequenced && st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
    odin3_pg_destroy(pg);
    if (st == ODIN3_OK) {
        TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
        TEST_ASSERT_EQUAL_UINT32(1, wb_new_procs(e2, nodes));
        TEST_ASSERT_EQUAL_UINT32(e2->types[ODIN3_E2T_PROC].v, odin3_node_type(e2->mod, node).v);
    } else {
        TEST_ASSERT_EQUAL_UINT32(0, wb_new_procs(e2, nodes));
    }
    return st;
}

static void wb_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    g_wb.lhs = wb_declarator(e2, module_node);
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = wb_run(e2, module_node, limit);
        g_wb.failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK ? settled + 1 : 0;
    }
    g_wb.settled = settled;
    g_wb.ran = true;
}

/* odin3_pg_temp, _switch, _case, _assign and _emit: every failing point is NO_MEMORY with no node
 * made, and the sequence settles to OK with one $proc node. */
static void test_oom_sweep_builder_primitives(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = wb_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    pt_read_src("module t(output reg [1:0] y);\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, g_wb.settled);
    TEST_ASSERT_GREATER_THAN_UINT32(10, g_wb.failures);
    TEST_ASSERT_GREATER_THAN_UINT32(0, g_wb.emit_failures);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_diag_errors(), et_log(&pt_rs));
    pt_expect_ir_ok();
}

/* --- the statement entry points and the select write refuse what they are not (white-box) ------
 */

static void misuse_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_if(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_case_stmt(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_if(pg, (odin3_ast_id){0}));
    odin3_sigbit bit = odin3_sb_of_bit(ODIN3_BIT_0);
    odin3_sig rhs = {&bit, 1, false};
    /* the target is a plain name: no select to write through the case method */
    odin3_pg_assign_req req = {module_node, wb_declarator(e2, module_node), &rhs, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_select_write(pg, &req));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_select_write(pg, NULL));
    g_wb.ran = true;
    odin3_pg_destroy(pg);
}

static void test_public_entry_points_refuse_misuse(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = misuse_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    pt_read_src("module t(output reg y);\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&pt_rs));
}

#define ALL_TESTS(X)                                                                               \
    X(test_if_else_default_mode)                                                                   \
    X(test_if_latch_default_mode)                                                                  \
    X(test_if_latch_nolatches)                                                                     \
    X(test_if_wide_condition)                                                                      \
    X(test_nonblocking_branch_has_no_case_temporary)                                               \
    X(test_case_items_and_default)                                                                 \
    X(test_case_default_first_is_last)                                                             \
    X(test_case_non_constant_items)                                                                \
    X(test_wildcards)                                                                              \
    X(test_casex_wildcard)                                                                         \
    X(test_plain_case_keeps_x)                                                                     \
    X(test_case_width)                                                                             \
    X(test_chunks_case_default_item)                                                               \
    X(test_chunks_case_added_default)                                                              \
    X(test_nested_if)                                                                              \
    X(test_read_after_switch_sees_t1)                                                              \
    X(test_full_case_attribute)                                                                    \
    X(test_synopsys_metacomment_both_flags)                                                        \
    X(test_parallel_case_and_false_attribute)                                                      \
    X(test_constant_if_folds)                                                                      \
    X(test_constant_parameter_and_dead_output)                                                     \
    X(test_constant_case_folds)                                                                    \
    X(test_constant_case_no_match_makes_no_node)                                                   \
    X(test_x_condition_is_not_folded)                                                              \
    X(test_variable_holding_a_constant_is_not_folded)                                              \
    X(test_deep_else_if_chain)                                                                     \
    X(test_variable_index_write)                                                                   \
    X(test_indexed_part_select_write)                                                              \
    X(test_indexed_part_select_minus_write)                                                        \
    X(test_ascending_target)                                                                       \
    X(test_straddle_at_both_ends_of_an_offset_target)                                              \
    X(test_signed_base_straddles_the_bottom)                                                       \
    X(test_ascending_target_straddles)                                                             \
    X(test_blocking_straddle)                                                                      \
    X(test_signed_index_limits_the_positions)                                                      \
    X(test_blocking_variable_index_write)                                                          \
    X(test_index_that_selects_nothing)                                                             \
    X(test_x_index_write_is_ignored_with_a_warning)                                                \
    X(test_z_index_blocking_write_is_ignored_with_a_warning)                                       \
    X(test_variable_index_in_concatenation_is_an_error)                                            \
    X(test_memory_write_in_if_arm_is_pending)                                                      \
    X(test_memory_write_in_case_arm_is_pending)                                                    \
    X(test_public_entry_points_refuse_misuse)                                                      \
    X(test_oom_sweep_builder_primitives)                                                           \
    X(test_oom_sweep)                                                                              \
    X(test_oom_sweep_flags_and_folding)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
