/*
 * test_elab_stage2_call.c — 2D Task 23: loops (EL-37), function and task calls (EL-38), calls in
 * continuous contexts, and the words of register arrays with a variable index (EL-27, EL-39 reads
 * and EL-35 writes), through the whole read_verilog pass (proc_test.h). Each `$proc` body is
 * asserted through the text dump of the node; every located diagnostic is asserted as a whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report, and the CTest elab_call_bridge runs elab-equiv on tests/verilog/call-bridge.
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
#include "ir/prov.h"
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

static uint32_t count_str(const char *hay, const char *needle) {
    uint32_t found = 0;
    for (const char *at = strstr(hay, needle); at != NULL; at = strstr(at + 1, needle)) {
        found++;
    }
    return found;
}

/* The caps a test lowers (0: the default). */
typedef struct caps {
    uint32_t loop, total, depth;
} caps;

static void with_limits(const caps *want) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_loop_iterations = want->loop != 0 ? want->loop : lim.max_loop_iterations;
    lim.max_total_iterations = want->total != 0 ? want->total : lim.max_total_iterations;
    lim.max_call_depth = want->depth != 0 ? want->depth : lim.max_call_depth;
    odin3_elab_test_set_limits(&lim);
}

/* --- for (EL-37) --------------------------------------------------------------------------------
 */

static const char k_for[] = "module t(input [3:0] a, output reg [3:0] y);\n"
                            "  integer i;\n"
                            "  always @* begin\n"
                            "    for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];\n"
                            "  end\nendmodule\n";

/* Four actions with constant selects; i is process-local: no O pin, no sync action. */
static void test_for_unrolled(void) {
    pt_read_clean(k_for);
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    odin3_node_id proc = pt_proc_node(0);
    pt_expect_dump(0, "I4 O8\n"
                      "C0 [O4<-I3 O5<-I2 O6<-I1 O7<-I0]\n"
                      "S always [O0..O3<-O4..O7]\n");
    for (uint32_t k = 0; k < 8; k++) {
        for (uint32_t bit = 0; bit < 32; bit++) {
            TEST_ASSERT_NOT_EQUAL_UINT32(pt_wnet("i", bit).v, pt_out_net(proc, k).v);
        }
    }
}

/* i read outside the block is an ordinary output whose final action assigns 4. */
static void test_for_variable_read_outside(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y, output [31:0] z);\n"
                  "  integer i;\n  assign z = i;\n"
                  "  always @* begin\n"
                  "    for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];\n"
                  "  end\nendmodule\n");
    const char *dump = pt_dump(pt_proc_node(0));
    TEST_ASSERT_NOT_NULL(
        strstr(dump, "C0 [O36<-I3 O37<-I2 O38<-I1 O39<-I0 O40..O71<-I4,I4..I5,I4"));
    TEST_ASSERT_NOT_NULL(strstr(dump, "S always [O0..O3<-O36..O39 O4..O35<-O40..O71]"));
    TEST_ASSERT_EQUAL_UINT32(1, count_str(dump, "C0 ")); /* the final action alone assigns i */
}

static void test_for_counts_down(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y);\n"
                  "  integer i;\n  always @* begin\n"
                  "    for (i = 3; i >= 0; i = i - 1) y[i] = a[i];\n  end\nendmodule\n");
    pt_expect_dump(0, "I4 O8\nC0 [O7<-I3 O6<-I2 O5<-I1 O4<-I0]\nS always [O0..O3<-O4..O7]\n");
}

static void test_for_cap(void) {
    with_limits(&(caps){16, 0, 0});
    pt_read_failing("module t(input a, output reg y);\n  reg [1:0] k;\n  always @* begin\n"
                    "    for (k = 0; k < 4; k = k + 1) y = a;\n  end\nendmodule\n");
    pt_expect_error((at_loc){4, 5}, "loop exceeds 16 iterations");
}

static void test_for_condition_not_constant(void) {
    pt_read_failing("module t(input [2:0] n, input a, output reg y);\n  integer i;\n"
                    "  always @* begin\n    for (i = 0; i < n; i = i + 1) y = a;\n  end\n"
                    "endmodule\n");
    pt_expect_error((at_loc){4, 5}, "for-loop condition is not constant (it reads 'n')");
}

static void test_for_variable_assigned_in_body(void) {
    pt_read_failing("module t(input a, output reg y);\n  integer i;\n  always @* begin\n"
                    "    for (i = 0; i < 4; i = i + 1) begin\n      i = 0;\n      y = a;\n"
                    "    end\n  end\nendmodule\n");
    pt_expect_error((at_loc){5, 7}, "loop variable 'i' is assigned in the loop body");
}

static void test_for_init_step_differ(void) {
    pt_read_failing("module t(input a, output reg y);\n  integer i, j;\n  always @* begin\n"
                    "    for (i = 0; i < 4; j = j + 1) y = a;\n  end\nendmodule\n");
    pt_expect_error((at_loc){4, 5},
                    "for-loop initialisation and step must assign the same variable");
}

static const char k_two_loops[] =
    "module t(input a, output reg y);\n  integer i;\n  always @* begin\n"
    "    for (i = 0; i < 20; i = i + 1) y = a;\n"
    "    for (i = 0; i < 20; i = i + 1) y = ~a;\n  end\nendmodule\n";

static void test_total_iterations_cap(void) {
    with_limits(&(caps){0, 32, 0});
    pt_read_failing(k_two_loops);
    pt_expect_error((at_loc){5, 5}, "unrolled loops exceed 32 iterations in this design");
}

/* Point (k): the generate loops of stage 1 count in the run's total. */
static void test_total_counts_generate_loops(void) {
    with_limits(&(caps){0, 32, 0});
    pt_read_failing("module t(input a, output reg y, output [11:0] g);\n  genvar j;\n"
                    "  for (j = 0; j < 12; j = j + 1) begin : gg\n    assign g[j] = a;\n  end\n"
                    "  integer i;\n  always @* begin\n"
                    "    for (i = 0; i < 24; i = i + 1) y = a;\n  end\nendmodule\n");
    pt_expect_error((at_loc){8, 5}, "unrolled loops exceed 32 iterations in this design");
}

static void test_total_loops_alone_pass(void) {
    with_limits(&(caps){0, 32, 0});
    pt_read_clean("module t(input a, output reg y, output [11:0] g);\n  genvar j;\n"
                  "  for (j = 0; j < 12; j = j + 1) begin : gg\n    assign g[j] = a;\n  end\n"
                  "  always @* y = a;\nendmodule\n");
    tearDown();
    setUp();
    with_limits(&(caps){0, 32, 0});
    pt_read_clean("module t(input a, output reg y);\n  integer i;\n  always @* begin\n"
                  "    for (i = 0; i < 24; i = i + 1) y = a;\n  end\nendmodule\n");
}

static void test_repeat(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y);\n  always @* begin\n"
                  "    y = a;\n    repeat (3) y = y + 4'd1;\n  end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(3, pt_count_of("$add"));
}

/* A shared counter in two blocks: neither drives it, so no T10. */
static void test_shared_counter(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y, z);\n  integer i;\n"
                  "  always @* begin\n    for (i = 0; i < 4; i = i + 1) y[i] = a[i];\n  end\n"
                  "  always @* begin\n    for (i = 0; i < 4; i = i + 1) z[i] = ~a[i];\n  end\n"
                  "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$proc"));
}

/* Task 22's carry: inside an unrolled loop a branch on the variable folds in every iteration. */
static void test_loop_folds_branches(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y);\n  integer i;\n"
                  "  always @* begin\n    for (i = 0; i < 4; i = i + 1)\n"
                  "      if (i == 2) y[i] = ~a[i]; else y[i] = a[i];\n  end\nendmodule\n");
    pt_expect_dump(0, "I4 O8\nC0 [O4<-I0 O5<-I1 O6<-I3 O7<-I2]\nS always [O0..O3<-O4..O7]\n");
}

/* --- functions and tasks (EL-38) ---------------------------------------------------------------
 */

static const char k_inc[] = "module t(input clk, input [3:0] d, output reg [3:0] q);\n"
                            "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n"
                            "  endfunction\n  always @(posedge clk) q <= inc(d);\nendmodule\n";

/* One $add; the frame temporaries are O pins with no sync action. */
static void test_function_inlined(void) {
    pt_read_clean(k_inc);
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    pt_expect_dump(0, "I9 O16\nC0 [O12..O15<-I1..I4 O8..O11<-I5..I8 O4..O7<-I5..I8]\n"
                      "S posedge I0 [O0..O3<-O4..O7]\n");
}

static void test_function_constant_call_folds(void) {
    pt_read_clean("module t(output reg [3:0] y);\n"
                  "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                  "  always @* y = inc(4'd3);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$add"));
    pt_expect_dump(0, "I2 O8\nC0 [O4..O7<-I0,I0..I1,I0]\nS always [O0..O3<-O4..O7]\n");
}

static const char k_task[] =
    "module t(input p, a, b, output reg y);\n  task tk(input x, output z);\n    z = ~x;\n"
    "  endtask\n  always @* tk(p, y);\nendmodule\n";

static void test_task_output(void) {
    pt_read_clean(k_task);
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$not"));
    pt_expect_dump(0, "I2 O4\nC0 [O2<-I0 O3<-I1 O1<-I1]\nS always [O0<-O1]\n");
}

/* K10: an enable without parentheses. */
static void test_task_without_parentheses(void) {
    pt_read_clean("module t(output reg y);\n  task clr;\n    y = 0;\n  endtask\n"
                  "  always @* clr;\nendmodule\n");
    pt_expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
}

static void test_task_output_must_be_variable(void) {
    pt_read_failing("module t(input p, a, b, output reg y);\n  task tk(input x, output z);\n"
                    "    z = ~x;\n  endtask\n  always @* tk(p, a & b);\nendmodule\n");
    pt_expect_error((at_loc){5, 19}, "output argument 2 of task 'tk' must be a variable");
}

/* K6, K11: calls in a body, and a call in a loop. */
static void test_nested_calls_and_call_in_loop(void) {
    pt_read_clean("module t(input [3:0] a, output reg [3:0] y);\n  integer i;\n"
                  "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                  "  function [3:0] inc2(input [3:0] v);\n    inc2 = inc(inc(v));\n  endfunction\n"
                  "  always @* begin\n    y = a;\n"
                  "    for (i = 0; i < 2; i = i + 1) y = inc2(y);\n  end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(4, pt_count_of("$add"));
}

/* A function with if/case inside, and a task writing a module variable under a condition. */
static void test_function_with_branches(void) {
    pt_read_clean("module t(input [3:0] a, input s, output reg [3:0] y, output reg w);\n"
                  "  function [3:0] sel(input [3:0] v, input c);\n"
                  "    if (c) sel = v; else sel = ~v;\n  endfunction\n"
                  "  task setw(input c);\n    if (c) w = 1; else w = 0;\n  endtask\n"
                  "  always @* begin\n    y = sel(a, s);\n    setw(s);\n  end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    pt_expect_dump(
        0,
        "I11 O25\n"
        "C0 [O14..O17<-I0..I3 O18<-I4 O10..O13<-O19..O22 O5..O8<-O19..O22 O23<-I4 O9<-O24] sw0+2\n"
        "C1 cmp(I9) [O22<-I3 O21<-I2 O20<-I1 O19<-I0]\n"
        "C2 [O22<-I8 O21<-I7 O20<-I6 O19<-I5]\n"
        "C3 cmp(I9) [O24<-I9]\n"
        "C4 [O24<-I10]\n"
        "W0 (I4) f0 C1+2\n"
        "W1 (I4) f0 C3+2\n"
        "S always [O0..O3<-O5..O8 O4<-O9]\n");
}

/* Continuous context: one extra $proc with one ALWAYS sync; y is driven by its O pin. */
static void test_continuous_call(void) {
    pt_read_clean("module t(input [3:0] d, output [3:0] y);\n"
                  "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                  "  assign y = inc(d);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    const char *dump = pt_dump(pt_proc_node(0));
    TEST_ASSERT_EQUAL_UINT32(1, count_str(dump, "S always"));
    odin3_node_id proc = pt_proc_node(0);
    bool driven = false;
    for (uint32_t k = 0; k < 12; k++) {
        driven |= pt_out_net(proc, k).v == pt_wnet("y", 0).v;
    }
    TEST_ASSERT_TRUE(driven);
    pt_expect_dump(0, "I8 O12\nC0 [O4..O7<-I0..I3 O8..O11<-I4..I7]\nS always [O0..O3<-O8..O11]\n");
}

/* EL-45: two call sites give two SOURCE records, each [the body's BINARY, the CALL]. */
static void test_inlined_provenance(void) {
    pt_read_clean("module t(input clk, input [3:0] d, e, output reg [3:0] q, r);\n"
                  "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                  "  always @(posedge clk) begin\n    q <= inc(d);\n    r <= inc(e);\n  end\n"
                  "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$add"));
    odin3_prov_id first = odin3_node_prov(pt_top(), pt_nth_node("$add", 0));
    odin3_prov_id second = odin3_node_prov(pt_top(), pt_nth_node("$add", 1));
    TEST_ASSERT_NOT_EQUAL_UINT32(first.v, second.v);
    const odin3_prov_record *rec = odin3_prov_get(pt_rs.design, first);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT32(2, rec->n_locs);
    TEST_ASSERT_EQUAL_UINT32(3, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(6, rec->locs[1].line);
    const odin3_prov_record *other = odin3_prov_get(pt_rs.design, second);
    TEST_ASSERT_NOT_NULL(other);
    TEST_ASSERT_EQUAL_UINT32(2, other->n_locs);
    TEST_ASSERT_EQUAL_UINT32(3, other->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(7, other->locs[1].line);
}

/* --- call errors (point (aa)) -------------------------------------------------------------------
 */

static void test_recursion_not_constant(void) {
    pt_read_failing(
        "module t(input [3:0] a, output reg [3:0] y);\n"
        "  function [3:0] f(input [3:0] n);\n    if (n == 0) f = 0; else f = f(n - 1);\n"
        "  endfunction\n  always @* y = f(a);\nendmodule\n");
    pt_expect_error((at_loc){3, 33},
                    "recursive call of 'f' cannot be inlined (its arguments are not constant)");
}

static void test_nesting_depth(void) {
    with_limits(&(caps){0, 0, 4});
    pt_read_failing("module t(input [3:0] a, output reg [3:0] y);\n"
                    "  function [3:0] f1(input [3:0] v); f1 = v + 1; endfunction\n"
                    "  function [3:0] f2(input [3:0] v); f2 = f1(v); endfunction\n"
                    "  function [3:0] f3(input [3:0] v); f3 = f2(v); endfunction\n"
                    "  function [3:0] f4(input [3:0] v); f4 = f3(v); endfunction\n"
                    "  function [3:0] f5(input [3:0] v); f5 = f4(v); endfunction\n"
                    "  always @* y = f5(a);\nendmodule\n");
    pt_expect_error((at_loc){3, 42}, "function call nesting deeper than 4 at 'f1'");
}

static void test_while_needs_constant_arguments(void) {
    pt_read_failing("module t(input [3:0] a, output reg [3:0] y);\n"
                    "  function [3:0] f(input [3:0] n);\n    begin f = 0;\n"
                    "      while (n != 0) begin f = f + 1; n = n - 1; end\n    end\n"
                    "  endfunction\n  always @* y = f(a);\nendmodule\n");
    pt_expect_error((at_loc){7, 17},
                    "function 'f' can be called only with constant arguments (it uses 'while')");
}

static void test_argument_count(void) {
    pt_read_failing("module t(input [3:0] a, b, output reg [3:0] y);\n"
                    "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                    "  always @* y = inc(a, b);\nendmodule\n");
    pt_expect_error((at_loc){5, 17}, "function 'inc' has 1 inputs; 2 arguments given");
}

/* --- register arrays with a variable index ------------------------------------------------------
 */

/* mem2reg rules 2 to 5 scan the bodies of called functions and tasks (Task 20's carry): an array
 * written in a task called from a combinational block becomes registers, as in Yosys. */
static void test_array_written_in_task_forces_registers(void) {
    pt_read_src("module t(input [1:0] s, input [3:0] d, output [3:0] y);\n"
                "  reg [3:0] m [0:3];\n  task wr(input [1:0] i, input [3:0] v);\n    m[i] = v;\n"
                "  endtask\n  assign y = m[0];\n  always @* wr(s, d);\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_warning((at_loc){2, 13}, "replacing memory 'm' with list of registers; see t.v:4");
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$mem"));
    pt_expect_ir_ok();
}

/* A net array read with a variable index: a $proc with a switch on the index (Task 20's pending
 * assertion flips). */
static void test_word_variable_read(void) {
    pt_read_clean("module t(input [1:0] s, input [3:0] d, output [3:0] y);\n"
                  "  wire [3:0] w [0:3];\n  assign w[0] = d;\n  assign w[1] = ~d;\n"
                  "  assign w[2] = d;\n  assign w[3] = ~d;\n  assign y = w[s];\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$proc"));
    pt_expect_dump(0, "I17 O8\nC0 [O4..O7<-O0..O3] sw0+1\n"
                      "C1 cmp(I14,I14) [O4..O7<-I2..I5]\n"
                      "C2 cmp(I15,I14) [O4..O7<-I6..I9]\n"
                      "C3 cmp(I14..I15) [O4..O7<-I2..I5]\n"
                      "C4 cmp(I15,I15) [O4..O7<-I10..I13]\n"
                      "C5 [O4..O7<-I16,I16,I16,I16]\n"
                      "W0 (I0..I1) f0 C1+5\n"
                      "S always [O0..O3<-O4..O7]\n");
}

/* m[s] <= d on a register array: a case over s assigning one word each. */
static void test_word_variable_write(void) {
    pt_read_src("module t(input c, input [1:0] s, input [3:0] d, output [3:0] y0, y1);\n"
                "  reg [3:0] m [0:3];\n  assign y0 = m[0];\n  assign y1 = m[1];\n"
                "  always @* m[s] = d;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_ir_ok();
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$mem"));
    pt_expect_dump(
        0,
        "I8 O48\nC0 [O28..O31<-O44..O47 O24..O27<-O40..O43 O20..O23<-O36..O39 O16..O19<-O32..O35] "
        "sw0+1\n"
        "C1 cmp(I6,I6) [O36..O39<-O4..O7 O40..O43<-O8..O11 O44..O47<-O12..O15 O32..O35<-I2..I5]\n"
        "C2 cmp(I7,I6) [O32..O35<-O0..O3 O40..O43<-O8..O11 O44..O47<-O12..O15 O36..O39<-I2..I5]\n"
        "C3 cmp(I6..I7) [O32..O35<-O0..O3 O36..O39<-O4..O7 O44..O47<-O12..O15 O40..O43<-I2..I5]\n"
        "C4 cmp(I7,I7) [O32..O35<-O0..O3 O36..O39<-O4..O7 O40..O43<-O8..O11 O44..O47<-I2..I5]\n"
        "C5 [O32..O35<-O0..O3 O36..O39<-O4..O7 O40..O43<-O8..O11 O44..O47<-O12..O15]\n"
        "W0 (I0..I1) f0 C1+5\n"
        "S always [O0..O3<-O16..O19 O4..O7<-O20..O23 O8..O11<-O24..O27 O12..O15<-O28..O31]\n");
}

/* A register array read inside a process: the case is inline. */
static void test_word_variable_read_in_process(void) {
    pt_read_clean("module t(input [1:0] s, input [3:0] d, output reg [3:0] y);\n"
                  "  wire [3:0] w [0:3];\n  assign w[0] = d;\n  assign w[1] = ~d;\n"
                  "  assign w[2] = d;\n  assign w[3] = ~d;\n  always @* y = w[s];\nendmodule\n");
    pt_expect_dump(0, "I17 O16\nC0 [O8..O11<-O12..O15 O4..O7<-O12..O15] sw0+1\n"
                      "C1 cmp(I14,I14) [O15<-I5 O14<-I4 O13<-I3 O12<-I2]\n"
                      "C2 cmp(I15,I14) [O15<-I9 O14<-I8 O13<-I7 O12<-I6]\n"
                      "C3 cmp(I14..I15) [O15<-I5 O14<-I4 O13<-I3 O12<-I2]\n"
                      "C4 cmp(I15,I15) [O15<-I13 O14<-I12 O13<-I11 O12<-I10]\n"
                      "C5 [O15<-I16 O14<-I16 O13<-I16 O12<-I16]\n"
                      "W0 (I0..I1) f0 C1+5\n"
                      "S always [O0..O3<-O4..O7]\n");
}

/* Memory writes keep their located pending line (Task 24): in a loop and in a task body. */
static void test_memory_write_in_loop_and_task_is_pending(void) {
    pt_read_failing("module t(input c, input [1:0] i, input [3:0] d, output [3:0] y);\n"
                    "  reg [3:0] m [0:3];\n  integer k;\n  assign y = m[i];\n"
                    "  always @(posedge c) begin\n    for (k = 0; k < 2; k = k + 1) m[i] <= d;\n"
                    "  end\nendmodule\n");
    pt_expect_error((at_loc){6, 35}, "elaboration is not available yet (2D): 'memory write'");
    tearDown();
    setUp();
    pt_read_failing("module t(input c, input [1:0] i, input [3:0] d, output [3:0] y);\n"
                    "  reg [3:0] m [0:3];\n  task wr;\n    m[i] <= d;\n  endtask\n"
                    "  assign y = m[i];\n  always @(posedge c) wr;\nendmodule\n");
    pt_expect_error((at_loc){4, 5}, "elaboration is not available yet (2D): 'memory write'");
}

/* --- out of memory ------------------------------------------------------------------------------
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
static const char k_oom_calls[] =
    "module t(input c, input [1:0] s, input [3:0] a, b, output reg [3:0] y, z, output [3:0] q,\n"
    "         output reg [3:0] r);\n"
    "  integer i;\n  wire [3:0] w [0:3];\n  reg [3:0] m [0:3];\n"
    "  assign w[0] = a;\n  assign w[1] = b;\n  assign w[2] = a;\n  assign w[3] = b;\n"
    "  function [3:0] inc(input [3:0] v);\n    if (v[0]) inc = v + 1; else inc = v;\n"
    "  endfunction\n"
    "  task flip(input [3:0] x, output [3:0] o);\n    o = ~x;\n  endtask\n"
    "  assign q = inc(a) + w[s];\n"
    "  always @* begin\n    y = a;\n    for (i = 0; i < 3; i = i + 1) y = inc(y);\n"
    "    repeat (2) y = y + b;\n    flip(y, z);\n    r = w[s];\n  end\n"
    "  always @* m[s] = a;\n"
    "endmodule\n";

static void test_oom_sweep_calls(void) {
    oom_sweep(k_oom_calls, 300);
}

/* --- the primitives under allocation failure and misuse (white-box) --------------------------- */

typedef struct wb_probe {
    bool ran;
    uint32_t failures, settled;
} wb_probe;

static wb_probe g_wb;

/* The first ALWAYS under the module. */
static odin3_ast_id wb_always(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, e2->elab, module_node));
    odin3_ast_event ev;
    odin3_ast_id node;
    odin3_ast_id found = {0};
    while (found.v == 0 && odin3_ast_cursor_next(&cur, &ev, &node)) {
        found = ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_ALWAYS
                    ? node
                    : found;
    }
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, found.v);
    return found;
}

static void wb_misuse(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    odin3_sig sig;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_for(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_repeat(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_call(pg, module_node, &sig));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_comb_call(pg, module_node, &sig));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_comb_mem_read(pg, module_node, &sig));
    odin3_pg_destroy(pg);
}

/* odin3_pg_always over a block with a loop, a function and a task: every failing point is NO_MEMORY
 * and the first run that gets through makes the node. */
static void wb_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    odin3_ast_id always = wb_always(e2, module_node);
    for (long limit = 0; limit < OOM_LIMIT && g_wb.settled == 0; limit++) {
        odin3_pg *pg = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
        odin3_util_set_alloc_fail_after(limit);
        odin3_status st = odin3_pg_always(pg, always);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        g_wb.failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        g_wb.settled = st == ODIN3_OK ? 1U : 0U;
        odin3_pg_destroy(pg);
    }
    wb_misuse(e2, module_node);
    g_wb.ran = true;
}

static void test_oom_sweep_primitives_and_misuse(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = wb_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    pt_read_src("module t(input [3:0] a, output reg [3:0] y);\n  integer i;\n"
                "  function [3:0] inc(input [3:0] v);\n    inc = v + 1;\n  endfunction\n"
                "  task flip(input [3:0] x, output [3:0] o);\n    o = ~x;\n  endtask\n"
                "  always @* begin\n    for (i = 0; i < 2; i = i + 1) y = inc(a);\n"
                "    flip(y, y);\n  end\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32(1, g_wb.settled);
    TEST_ASSERT_GREATER_THAN_UINT32(5, g_wb.failures);
    /* the sweep's node claimed the outputs: the real pass of the block is the T10 conflict */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pt_rs.status);
    pt_expect_ir_ok();
}

/* Yosys folds m[f(1)] before mem2reg_as_needed (verified on 0.55): a constant write, so rule 5
 * makes registers. */
static void test_constant_call_index_is_constant(void) {
    pt_read_src("module t(input c, input [3:0] d, output [3:0] y);\n  reg [3:0] m [0:3];\n"
                "  function [1:0] f(input [1:0] x);\n    f = x;\n  endfunction\n"
                "  always @(posedge c) m[f(1)] <= d;\n  assign y = m[1];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    pt_expect_warning((at_loc){2, 13}, "replacing memory 'm' with list of registers; see t.v:6");
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$mem"));
    pt_expect_ir_ok();
}

#define ALL_TESTS(X)                                                                               \
    X(test_for_unrolled)                                                                           \
    X(test_for_variable_read_outside)                                                              \
    X(test_for_counts_down)                                                                        \
    X(test_for_cap)                                                                                \
    X(test_for_condition_not_constant)                                                             \
    X(test_for_variable_assigned_in_body)                                                          \
    X(test_for_init_step_differ)                                                                   \
    X(test_total_iterations_cap)                                                                   \
    X(test_total_counts_generate_loops)                                                            \
    X(test_total_loops_alone_pass)                                                                 \
    X(test_repeat)                                                                                 \
    X(test_shared_counter)                                                                         \
    X(test_loop_folds_branches)                                                                    \
    X(test_function_inlined)                                                                       \
    X(test_function_constant_call_folds)                                                           \
    X(test_task_output)                                                                            \
    X(test_task_without_parentheses)                                                               \
    X(test_task_output_must_be_variable)                                                           \
    X(test_nested_calls_and_call_in_loop)                                                          \
    X(test_function_with_branches)                                                                 \
    X(test_continuous_call)                                                                        \
    X(test_inlined_provenance)                                                                     \
    X(test_recursion_not_constant)                                                                 \
    X(test_nesting_depth)                                                                          \
    X(test_while_needs_constant_arguments)                                                         \
    X(test_argument_count)                                                                         \
    X(test_array_written_in_task_forces_registers)                                                 \
    X(test_word_variable_read)                                                                     \
    X(test_word_variable_write)                                                                    \
    X(test_word_variable_read_in_process)                                                          \
    X(test_memory_write_in_loop_and_task_is_pending)                                               \
    X(test_oom_sweep_calls)                                                                        \
    X(test_oom_sweep_primitives_and_misuse)                                                        \
    X(test_constant_call_index_is_constant)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
