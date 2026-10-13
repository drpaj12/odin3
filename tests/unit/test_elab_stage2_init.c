/*
 * test_elab_stage2_init.c — 2D Task 24: memory writes (EL-39 v4: the write ports of `$mem`, their
 * process-local variables), initial values and `$readmemh/b` (EL-40), the per-bit rules of EL-41
 * that Task 21 built (their remaining cases), the empty pending tables (point (l)) and the sink
 * module that uses every construct of Tasks 17 to 24 (OOM, determinism). Every source is read
 * through the whole read_verilog pass (proc_test.h); each `$proc` body is asserted through the text
 * dump of the node and every located diagnostic as a whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report, and the CTest elab_mem_bridge runs elab-equiv on tests/verilog/mem-bridge.
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

#ifndef ODIN3_READMEM_DATA
#error "ODIN3_READMEM_DATA must name tests/unit/data/elab/readmem"
#endif
#ifndef ODIN3_SINK_V
#error "ODIN3_SINK_V must name tests/elab/sink/sink.v"
#endif

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, FILE_MAX = 1 << 16, BYTE_BITS = 8 };

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

/* --- reading the IR back ---------------------------------------------------------------------- */

static const odin3_celltype_def *def_of(odin3_node_id node) {
    return odin3_celltype_get(pt_rs.design, odin3_node_type(pt_top(), node));
}

static const odin3_value *param_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_params; index++) {
        if (strcmp(def->params[index].name, name) == 0) {
            const odin3_value *value = odin3_node_param(pt_top(), node, index);
            TEST_ASSERT_NOT_NULL(value);
            return value;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

/* A BITS parameter as text, LSB first ('0', '1', 'x', 'z'); "" for an empty one. */
static const char *bits_text(const odin3_value *value) {
    static char buf[256];
    TEST_ASSERT_TRUE(value->len < sizeof buf);
    for (uint32_t k = 0; k < value->len; k++) {
        buf[k] = "01xz"[value->bits[k]];
    }
    buf[value->len] = '\0';
    return buf;
}

static uint32_t port_width(odin3_node_id node, const char *port) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            return odin3_node_port(pt_top(), node, index).count;
        }
    }
    TEST_FAIL_MESSAGE(port);
    return 0;
}

static odin3_net_id pin_net(odin3_node_id node, const char *port, uint32_t bit) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            odin3_pinslice slice = odin3_node_port(pt_top(), node, index);
            TEST_ASSERT_TRUE_MESSAGE(bit < slice.count, port);
            return odin3_pin_net(pt_top(), (odin3_pin_id){slice.first.v + bit});
        }
    }
    TEST_FAIL_MESSAGE(port);
    return (odin3_net_id){0};
}

/* Bits [from, from + n) of a port are the O pins first .. first + n of the one `$proc`. */
typedef struct pin_run {
    const char *port;
    uint32_t from, n, first;
} pin_run;

static void expect_pins_are_outs(odin3_node_id mem, const pin_run *run) {
    odin3_node_id proc = pt_proc_node(0);
    for (uint32_t k = 0; k < run->n; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pt_out_net(proc, run->first + k).v,
                                         pin_net(mem, run->port, run->from + k).v, run->port);
    }
}

/* Bits of a port are the constant net of the bit of value (LSB first). */
static void expect_pins_const(odin3_node_id mem, const char *port, uint32_t from,
                              const char *bits) {
    for (uint32_t k = 0; bits[k] != '\0'; k++) {
        odin3_bit want = bits[k] == '1' ? ODIN3_BIT_1 : ODIN3_BIT_0;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pt_const_net_of(want).v, pin_net(mem, port, from + k).v,
                                         port);
    }
}

static void expect_pins_net(odin3_node_id mem, const char *port, uint32_t from, const char *wire,
                            uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pt_wnet(wire, k).v, pin_net(mem, port, from + k).v, wire);
    }
}

static odin3_node_id the_mem(void) {
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$mem"));
    return pt_nth_node("$mem", 0);
}

/* --- write ports (EL-39 v4, ruling C1) -------------------------------------------------------- */

static const char k_cond[] = "module t(input c, we, input [2:0] wa, ra, input [7:0] d,\n"
                             "         output [7:0] q);\n"
                             "  reg [7:0] m [1:4];\n"
                             "  always @(posedge c) if (we) m[wa] <= d;\n"
                             "  assign q = m[ra];\n"
                             "endmodule\n";

/* A conditional write: one port; WR_ADDR/WR_DATA/WR_EN are three O pins of the $proc, which has no
 * sync action; the root assigns the switch's case temporaries; the switch has the written case
 * and the added default with the x/x/0 defaults (review R2). */
static void test_conditional_write_port(void) {
    pt_read_clean(k_cond);
    odin3_node_id mem = the_mem();
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "WR_PORTS")->i);
    TEST_ASSERT_EQUAL_STRING("1", bits_text(param_of(mem, "WR_CLK_ENABLE")));
    TEST_ASSERT_EQUAL_STRING("1", bits_text(param_of(mem, "WR_CLK_POLARITY")));
    TEST_ASSERT_EQUAL_UINT32(pt_wnet("c", 0).v, pin_net(mem, "WR_CLK", 0).v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "OFFSET")->i);
    TEST_ASSERT_EQUAL_INT64(3, param_of(mem, "ABITS")->i);
    expect_pins_net(mem, "RD_ADDR", 0, "ra", 3);
    /* the O pins in net-ID order: addr, its t1, data, its t1, en, its t1 */
    expect_pins_are_outs(mem, &(pin_run){"WR_ADDR", 0, 3, 0});
    expect_pins_are_outs(mem, &(pin_run){"WR_DATA", 0, 8, 6});
    expect_pins_are_outs(mem, &(pin_run){"WR_EN", 0, 8, 22});
    pt_expect_dump(0, "I16 O38\n"
                      "C0 [O0..O2<-O3..O5 O6..O13<-O14..O21 O22..O29<-O30..O37] sw0+1\n"
                      "C1 cmp(I13) [O3..O5<-I2..I4 O14..O21<-I5..I12 "
                      "O30..O37<-I13,I13,I13,I13,I13,I13,I13,I13]\n"
                      "C2 [O3..O5<-I14,I14,I14 O14..O21<-I14,I14,I14,I14,I14,I14,I14,I14 "
                      "O30..O37<-I15,I15,I15,I15,I15,I15,I15,I15]\n"
                      "W0 (I1) f0 C1+2\n"
                      "S posedge I0 []\n");
}

/* A constant index and value need no addr or data variable; the unconditional write's action
 * removed the inserted `en = 0` (review R2). */
static void test_constant_index_and_value(void) {
    pt_read_clean("module t(input c, input [2:0] ra, output [7:0] q);\n"
                  "  (* nomem2reg *) reg [7:0] m [1:4];\n"
                  "  always @(posedge c) m[2] <= 8'h5A;\n"
                  "  assign q = m[ra];\n"
                  "endmodule\n");
    odin3_node_id mem = the_mem();
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "WR_PORTS")->i);
    expect_pins_const(mem, "WR_ADDR", 0, "010"); /* the raw index 2, a 32-bit integer */
    expect_pins_const(mem, "WR_DATA", 0, "01011010");
    TEST_ASSERT_EQUAL_UINT32(8, port_width(mem, "WR_EN"));
    pt_expect_dump(0, "I2 O8\n"
                      "C0 [O0..O7<-I1,I1,I1,I1,I1,I1,I1,I1]\n"
                      "S posedge I0 []\n");
}

static void test_other_bases(void) {
    pt_read_clean("module t(input c, we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);\n"
                  "  reg [7:0] m [4:1];\n"
                  "  always @(posedge c) if (we) m[wa] <= d;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, param_of(the_mem(), "OFFSET")->i);
    tearDown();
    setUp();
    pt_read_clean("module t(input c, we, input signed [2:0] wa, ra, input [7:0] d,\n"
                  "         output [7:0] q);\n"
                  "  reg [7:0] m [-2:1];\n"
                  "  always @(posedge c) if (we) m[wa] <= d;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(-2, param_of(the_mem(), "OFFSET")->i);
}

/* A partial write sets en on those bits only; the other bits of data are x below and 0 above. */
static void test_partial_write(void) {
    pt_read_clean("module t(input c, we, input [2:0] wa, ra, input [3:0] d4, output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(posedge c) if (we) m[wa][3:0] <= d4;\n"
                  "  assign q = m[ra];\nendmodule\n");
    odin3_node_id mem = the_mem();
    TEST_ASSERT_EQUAL_UINT32(8, port_width(mem, "WR_EN"));
    pt_expect_dump(0, "I12 O38\n"
                      "C0 [O0..O2<-O3..O5 O6..O13<-O14..O21 O22..O29<-O30..O37] sw0+1\n"
                      "C1 cmp(I9) [O3..O5<-I2..I4 O14..O21<-I5..I8,I10,I10,I10,I10 "
                      "O30..O37<-I9,I9,I9,I9..I10,I10,I10,I10]\n"
                      "C2 [O3..O5<-I11,I11,I11 O14..O21<-I11,I11,I11,I11,I11,I11,I11,I11 "
                      "O30..O37<-I10,I10,I10,I10,I10,I10,I10,I10]\n"
                      "W0 (I1) f0 C1+2\n"
                      "S posedge I0 []\n");
}

/* Two write statements: two ports in source order (the first statement's variables first). */
static void test_two_writes_in_source_order(void) {
    pt_read_clean("module t(input c, we, we2, input [2:0] wa, wb, ra, input [7:0] d, e,\n"
                  "         output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(posedge c) begin\n"
                  "    if (we) m[wa] <= d;\n"
                  "    if (we2) m[wb] <= e;\n"
                  "  end\n"
                  "  assign q = m[ra];\nendmodule\n");
    odin3_node_id mem = the_mem();
    TEST_ASSERT_EQUAL_INT64(2, param_of(mem, "WR_PORTS")->i);
    TEST_ASSERT_EQUAL_STRING("11", bits_text(param_of(mem, "WR_CLK_ENABLE")));
    expect_pins_are_outs(mem, &(pin_run){"WR_ADDR", 0, 3, 0});
    expect_pins_are_outs(mem, &(pin_run){"WR_ADDR", 3, 3, 38});
    expect_pins_are_outs(mem, &(pin_run){"WR_DATA", 0, 8, 6});
    expect_pins_are_outs(mem, &(pin_run){"WR_DATA", 8, 8, 44});
}

/* Two blocks: one port each, in the order of the blocks. */
static void test_two_blocks_two_ports(void) {
    pt_read_clean("module t(input c, we, input [2:0] wa, wb, ra, input [7:0] d, e,\n"
                  "         output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(posedge c) if (we) m[wa] <= d;\n"
                  "  always @(posedge c) if (!we) m[wb] <= e;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$proc"));
    odin3_node_id mem = the_mem();
    TEST_ASSERT_EQUAL_INT64(2, param_of(mem, "WR_PORTS")->i);
    odin3_node_id first = pt_proc_node(0);
    odin3_node_id second = pt_proc_node(1);
    TEST_ASSERT_EQUAL_UINT32(pt_out_net(first, 0).v, pin_net(mem, "WR_ADDR", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pt_out_net(second, 0).v, pin_net(mem, "WR_ADDR", 3).v);
}

/* A blocking write is the same port, and an unconditional one leaves the root three actions. */
static void test_unconditional_blocking_write(void) {
    pt_read_clean("module t(input c, input [2:0] wa, ra, input [7:0] d, output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(posedge c) m[wa] = d;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, param_of(the_mem(), "WR_PORTS")->i);
    pt_expect_dump(0, "I13 O19\n"
                      "C0 [O0..O2<-I1..I3 O3..O10<-I4..I11 O11..O18<-I12,I12,I12,I12,I12,I12,I12,"
                      "I12]\n"
                      "S posedge I0 []\n");
}

static void test_negedge_polarity(void) {
    pt_read_clean("module t(input c, we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(negedge c) if (we) m[wa] <= d;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("0", bits_text(param_of(the_mem(), "WR_CLK_POLARITY")));
}

/* Writes in an if/else chain, a case, a loop, a task and a function reach the same port builder:
 * the variables join every open switch (their dumps are covered by the bridge sources). */
static void test_writes_in_every_context(void) {
    pt_read_clean("module t(input c, a, b, input [1:0] s, wa, ra, input [7:0] d, e,\n"
                  "         output [7:0] q);\n"
                  "  reg [7:0] m [0:3];\n  integer i;\n"
                  "  task wr(input [1:0] x, input [7:0] v);\n    m[x] <= v;\n  endtask\n"
                  "  always @(posedge c) begin\n"
                  "    if (a) m[wa] <= d; else if (b) m[wa + 1] <= e; else m[wa + 2] <= d;\n"
                  "    case (s) 2'd0: wr(wa, d); 2'd1: begin wr(wa, e); wr(wa + 1, d); end\n"
                  "      default: ; endcase\n"
                  "    for (i = 0; i < 2; i = i + 1) if (b) m[wa + i] <= d + i;\n"
                  "  end\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(8, param_of(the_mem(), "WR_PORTS")->i);
}

/* `(* nomem2reg *)` leaves the array a memory; a write in a block without exactly one edge is
 * refused at the write (Task 20 carry). */
static void test_nomem2reg_combinational_write(void) {
    pt_read_failing("module t(input we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);\n"
                    "  (* nomem2reg *) reg [7:0] m [1:4];\n"
                    "  always @* if (we) m[wa] = d;\n"
                    "  assign q = m[ra];\nendmodule\n");
    pt_expect_error((at_loc){3, 21},
                    "array 'm' cannot be a memory (it is written in a block without exactly one "
                    "edge) and (* nomem2reg *) is set");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_diag_errors(), et_log(&pt_rs));
}

/* A concatenation target with a memory word is split into one write per part, as Yosys does
 * (the least significant part first); a word in a nested concatenation is refused. */
static void test_concatenation_target(void) {
    pt_read_clean("module t(input c, we, input [2:0] wa, ra, input [11:0] x, output [7:0] q,\n"
                  "         output reg [3:0] t);\n"
                  "  reg [7:0] m [1:4];\n"
                  "  always @(posedge c) if (we) {m[wa], t} <= x;\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, param_of(the_mem(), "WR_PORTS")->i);
    tearDown();
    setUp();
    pt_read_failing("module t(input c, input [2:0] wa, ra, input [11:0] x, output [7:0] q,\n"
                    "         output reg [3:0] t);\n"
                    "  reg [7:0] m [1:4];\n"
                    "  always @(posedge c) {{m[wa], t}} <= x;\n"
                    "  assign q = m[ra];\nendmodule\n");
    pt_expect_error((at_loc){4, 25}, "memory word of 'm' in a concatenation target is not "
                                     "supported");
}

/* A non-constant bit select of a word is refused (Yosys shifts; not supported). */
static void test_variable_bit_of_a_word(void) {
    pt_read_failing("module t(input c, input [2:0] wa, ra, input [1:0] b, input x,\n"
                    "         output [7:0] q);\n"
                    "  reg [7:0] m [1:4];\n"
                    "  always @(posedge c) m[wa][b] <= x;\n"
                    "  assign q = m[ra];\nendmodule\n");
    pt_expect_error((at_loc){4, 23}, "non-constant index into 'm' is not supported here");
}

/* A read that succeeds (warnings allowed) with no error and a valid IR. */
static void read_ok(const char *text) {
    pt_read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_diag_errors(), et_log(&pt_rs));
    pt_expect_ir_ok();
}

/* --- initial values (EL-40) ------------------------------------------------------------------- */

static void test_initial_value_of_a_register(void) {
    pt_read_clean("module t(input c, output reg [3:0] r);\n"
                  "  reg [3:0] r2 = 4'd5;\n"
                  "  always @(posedge c) r2 <= r2 + 1;\n"
                  "  always @(posedge c) r <= r2;\nendmodule\n");
    const odin3_value *init = odin3_attr_get(
        pt_top(),
        (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(pt_top(), pt_id_of("r2")).v},
        pt_id_of("init"));
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_STRING("1010", bits_text(init)); /* 4'b0101, LSB first */
}

/* Task 24 fix round 1 (I1): a task or function called from an always block assigns the register,
 * so its initial value is the wire's init attribute (a bound constant would give two drivers). */
static void expect_r_init_is_3(void) {
    const odin3_value *init = odin3_attr_get(
        pt_top(), (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(pt_top(), pt_id_of("r")).v},
        pt_id_of("init"));
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_STRING("1100", bits_text(init));
}

static void test_initial_register_assigned_in_task(void) {
    pt_read_clean("module t(input c, input [3:0] a, output [3:0] q);\n"
                  "  reg [3:0] r = 4'd3;\n"
                  "  task tk(input [3:0] x); begin r <= x; end endtask\n"
                  "  always @(posedge c) tk(a);\n"
                  "  assign q = r;\nendmodule\n");
    expect_r_init_is_3();
    pt_expect_ir_ok();
}

/* PR 2D-6 review I1: the register is the output (or inout) actual of a task enable: the block
 * drives it, so its initial value is the init attribute and the IR checks (no second driver). */
static void expect_r_init_is_5(void) {
    const odin3_value *init = odin3_attr_get(
        pt_top(), (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(pt_top(), pt_id_of("r")).v},
        pt_id_of("init"));
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_STRING("10100000", bits_text(init));
}

static void test_initial_register_as_task_output_actual(void) {
    pt_read_clean("module t(input clk, output reg [7:0] y0);\n"
                  "  reg [7:0] r = 8'h5;\n"
                  "  task tk(output [7:0] z); z = 7; endtask\n"
                  "  always @(posedge clk) begin tk(r); y0 <= r; end\nendmodule\n");
    expect_r_init_is_5();
    pt_expect_ir_ok();
}

static void test_initial_register_as_task_inout_actual(void) {
    pt_read_clean("module t(input clk, input [7:0] a, output reg [7:0] y0);\n"
                  "  reg [7:0] r = 8'h5;\n"
                  "  task tk(inout [7:0] z, input [7:0] x); z = z + x; endtask\n"
                  "  always @(posedge clk) begin tk(r, a); y0 <= r; end\nendmodule\n");
    expect_r_init_is_5();
    pt_expect_ir_ok();
}

static void test_initial_register_assigned_in_function(void) {
    pt_read_clean("module t(input c, input [3:0] a, output [3:0] q, output reg [3:0] p);\n"
                  "  reg [3:0] r = 4'd3;\n"
                  "  function [3:0] f(input [3:0] x); begin r = x; f = x + 1; end endfunction\n"
                  "  always @(posedge c) p <= f(a);\n"
                  "  assign q = r;\nendmodule\n");
    expect_r_init_is_3();
    pt_expect_ir_ok();
}

static void test_initial_task_call_not_applied(void) {
    read_ok("module t(output reg [3:0] r);\n"
            "  task tk(input [3:0] x); begin r = x; end endtask\n"
            "  initial tk(7);\nendmodule\n");
    pt_expect_warning((at_loc){3, 11},
                      "task call in an initial block is not applied and is ignored");
}

static void test_initial_memory_contents(void) {
    pt_read_clean("module t(input [2:0] ra, output [7:0] q);\n"
                  "  reg [7:0] m [1:4];\n  integer i;\n"
                  "  initial begin\n    m[1] = 8'h12;\n"
                  "    for (i = 2; i <= 4; i = i + 1) m[i] = i;\n  end\n"
                  "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("01001000"
                             "01000000"
                             "11000000"
                             "00100000",
                             bits_text(param_of(the_mem(), "INIT")));
}

static void test_initial_statement_not_constant(void) {
    read_ok("module t(input [3:0] a, output reg [3:0] r2);\n"
            "  initial r2 = a;\nendmodule\n");
    pt_expect_warning((at_loc){2, 11}, "initial statement is not constant and is ignored");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
}

/* One statement of a block that is not constant does not take the others with it. */
static void test_initial_block_statements_are_separate(void) {
    read_ok("module t(input [3:0] a, input c, output reg [3:0] r, s);\n"
            "  initial begin\n    r = 4'd9;\n    s = a;\n  end\n"
            "  always @(posedge c) begin r <= r + 1; s <= s + 1; end\nendmodule\n");
    pt_expect_warning((at_loc){4, 5}, "initial statement is not constant and is ignored");
    const odin3_value *init = odin3_attr_get(
        pt_top(), (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(pt_top(), pt_id_of("r")).v},
        pt_id_of("init"));
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_STRING("1001", bits_text(init));
}

/* A variable nothing else drives: its net is the constant net. */
static void test_initial_only_variable_is_constant(void) {
    pt_read_clean("module t(output y);\n  reg k;\n  initial k = 1;\n  assign y = k;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(pt_const_net_of(ODIN3_BIT_1).v, pt_wnet("k", 0).v);
}

static void test_initial_value_of_a_driven_net(void) {
    read_ok("module t(input a, output w);\n  assign w = a;\n  initial w = 0;\nendmodule\n");
    pt_expect_warning((at_loc){3, 11},
                      "initial value of 'w' is ignored: 'w' is driven by a continuous assignment");
    tearDown();
    setUp();
    read_ok("module sub(output o);\n  assign o = 1'b1;\nendmodule\n"
            "module t(output w);\n  sub u (.o(w));\n  initial w = 0;\nendmodule\n");
    pt_expect_warning((at_loc){6, 11}, "initial value of 'w' is ignored: 'w' is driven by an "
                                       "instance");
    tearDown();
    setUp();
    read_ok("module t(input a, output y);\n  assign y = a;\n  initial a = 0;\nendmodule\n");
    pt_expect_warning((at_loc){3, 11}, "initial value of 'a' is ignored: 'a' is driven by an "
                                       "input port");
}

/* The init-bits cap: 32 bits of memory over a hook of 16. */
static void test_init_bits_cap(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_init_bits = 16;
    odin3_elab_test_set_limits(&lim);
    pt_read_failing("module t(input [1:0] ra, output [7:0] q);\n"
                    "  reg [7:0] m [0:3];\n"
                    "  initial m[0] = 8'd1;\n"
                    "  assign q = m[ra];\nendmodule\n");
    pt_expect_error((at_loc){3, 11}, "initial values need more than 16 bits");
}

/* --- $readmemh / $readmemb -------------------------------------------------------------------- */

static char g_files[4][FILE_MAX];

/* The text of a data file of tests/unit/data/elab/readmem. */
static const char *data_text(unsigned slot, const char *name) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s", ODIN3_READMEM_DATA, name) > 0);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(g_files[slot], 1, FILE_MAX - 1U, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    g_files[slot][got] = '\0';
    return g_files[slot];
}

/* "sub/t.v" and "sub/<name>": the file is beside the source, never in the working directory. */
static void read_with_file(const char *name, const char *source) {
    const et_src srcs[] = {{"sub/t.v", source}, {name, data_text(0, strrchr(name, '/') + 1)}};
    pt_read_files(srcs, 2, "sub/t.v");
}

/* One word of the INIT value as two hex digits, or "xx" when a bit is not 0 or 1. */
static void word_text(const odin3_value *init, uint32_t word, char *out) {
    unsigned value = 0;
    bool known = true;
    for (uint32_t bit = 0; bit < BYTE_BITS; bit++) {
        uint8_t level = init->bits[word * BYTE_BITS + bit];
        known = known && level <= 1U;
        value |= (level == 1U ? 1U : 0U) << bit;
    }
    int got = known ? snprintf(out, 4, "%02x ", value) : snprintf(out, 4, "xx ");
    TEST_ASSERT_EQUAL_INT(3, got);
}

static const char *words_of_init(void) {
    static char buf[16];
    const odin3_value *init = param_of(the_mem(), "INIT");
    TEST_ASSERT_EQUAL_UINT32(32, init->len);
    for (uint32_t word = 0; word < 4; word++) {
        word_text(init, word, buf + (size_t)word * 3U);
    }
    buf[11] = '\0';
    return buf;
}

static void test_readmemh(void) {
    read_with_file("sub/init.hex", "module t(input [1:0] ra, output [7:0] q);\n"
                                   "  reg [7:0] m [0:3];\n"
                                   "  initial $readmemh(\"init.hex\", m);\n"
                                   "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_STRING("xx xx 12 34", words_of_init());
}

static void test_readmemb_with_start_and_finish(void) {
    read_with_file("sub/init.bin", "module t(input [1:0] ra, output [7:0] q);\n"
                                   "  reg [7:0] m [0:3];\n"
                                   "  initial $readmemb(\"init.bin\", m, 1, 2);\n"
                                   "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_STRING("xx a5 3c xx", words_of_init());
}

static void test_readmem_errors(void) {
    read_with_file("sub/init.hex", "module t(input [1:0] ra, output [7:0] q);\n"
                                   "  reg [7:0] m [0:3];\n"
                                   "  initial $readmemh(\"nope.hex\", m);\n"
                                   "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pt_rs.status);
    et_expect(&pt_rs, ODIN3_LOG_ERROR, "sub/t.v:3:11: error: cannot open 'nope.hex' for $readmemh");
    tearDown();
    setUp();
    const et_src bad[] = {{"sub/t.v", "module t(input [1:0] ra, output [7:0] q);\n"
                                      "  reg [7:0] m [0:3];\n"
                                      "  initial $readmemh(\"bad_addr.hex\", m);\n"
                                      "  assign q = m[ra];\nendmodule\n"},
                          {"sub/bad_addr.hex", data_text(0, "bad_addr.hex")}};
    pt_read_files(bad, 2, "sub/t.v");
    et_expect(&pt_rs, ODIN3_LOG_ERROR,
              "sub/t.v:3:11: error: bad address '@zz' in 'bad_addr.hex' line 3");
    tearDown();
    setUp();
    const et_src word[] = {{"sub/t.v", "module t(input [1:0] ra, output [7:0] q);\n"
                                       "  reg [7:0] m [0:3];\n"
                                       "  initial $readmemh(\"bad_word.hex\", m);\n"
                                       "  assign q = m[ra];\nendmodule\n"},
                           {"sub/bad_word.hex", data_text(0, "bad_word.hex")}};
    pt_read_files(word, 2, "sub/t.v");
    et_expect(&pt_rs, ODIN3_LOG_ERROR,
              "sub/t.v:3:11: error: bad word 'g1' in 'bad_word.hex' line 1");
}

/* Words the array cannot hold are dropped with one warning that names the first address. */
static void test_readmem_dropped_words_warn(void) {
    read_with_file("sub/drop_after_address.hex",
                   "module t(input [1:0] ra, output [7:0] q);\n"
                   "  reg [7:0] m [0:3];\n"
                   "  initial $readmemh(\"drop_after_address.hex\", m);\n"
                   "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_STRING("11 22 xx xx", words_of_init());
    et_expect(&pt_rs, ODIN3_LOG_WARN,
              "sub/t.v:3:11: warning: $readmemh of 'drop_after_address.hex' dropped 6 words "
              "outside the array (the first at address 7)");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
}

static void test_readmem_words_past_the_end_warn(void) {
    read_with_file("sub/drop_past_end.hex", "module t(input [1:0] ra, output [7:0] q);\n"
                                            "  reg [7:0] m [0:3];\n"
                                            "  initial $readmemh(\"drop_past_end.hex\", m);\n"
                                            "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_STRING("xx 44 55 66", words_of_init());
    et_expect(&pt_rs, ODIN3_LOG_WARN,
              "sub/t.v:3:11: warning: $readmemh of 'drop_past_end.hex' dropped 2 words "
              "outside the array (the first at address 4)");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
}

static void test_readmem_outside_initial(void) {
    pt_read_failing("module t(input c, input [1:0] ra, output [7:0] q);\n"
                    "  reg [7:0] m [0:3];\n"
                    "  always @(posedge c) $readmemh(\"init.hex\", m);\n"
                    "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, pt_diag_errors(), et_log(&pt_rs));
}

static void test_readmem_size_cap(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_readmem_bytes = 16;
    odin3_elab_test_set_limits(&lim);
    read_with_file("sub/init.hex", "module t(input [1:0] ra, output [7:0] q);\n"
                                   "  reg [7:0] m [0:3];\n"
                                   "  initial $readmemh(\"init.hex\", m);\n"
                                   "  assign q = m[ra];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pt_rs.status);
    et_expect(&pt_rs, ODIN3_LOG_ERROR,
              "sub/t.v:3:11: error: 'init.hex' is larger than 16 bytes (the $readmem limit)");
}

/* --- per-bit rules (EL-41; built by Task 21, the remaining cases) ----------------------------- */

static void test_inlined_task_output_counts_as_blocking(void) {
    pt_read_failing("module t(input c, a, output reg r);\n"
                    "  task put(output o);\n    o = a;\n  endtask\n"
                    "  always @(posedge c) begin\n    put(r);\n    r <= a;\n  end\nendmodule\n");
    pt_expect_error((at_loc){7, 5}, "'r' is assigned with both '=' and '<=' (also at t.v:6:5)");
}

static void test_bits_split_across_blocks_are_fine(void) {
    pt_read_clean("module t(input c, a, b, output reg [3:0] r);\n"
                  "  always @(posedge c) r[1] <= a;\n  always @(posedge c) r[2] <= b;\n"
                  "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$proc"));
}

/* --- the pending tables are empty (point (l)) ------------------------------------------------- */

static void test_pending_tables_are_empty(void) {
    for (uint32_t stage = ODIN3_ELAB_STAGE1; stage <= ODIN3_ELAB_STAGE2; stage++) {
        for (uint32_t kind = 0; kind < ODIN3_PEND_END; kind++) {
            TEST_ASSERT_FALSE_MESSAGE(odin3_elab_pending((uint8_t)stage, kind),
                                      odin3_ast_kind_name(kind));
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_pending_form_count(ODIN3_ELAB_STAGE1));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_pending_form_count(ODIN3_ELAB_STAGE2));
}

/* --- the sink: out of memory and determinism -------------------------------------------------- */

static char g_sink[FILE_MAX];

static const char *sink_text(void) {
    FILE *file = fopen(ODIN3_SINK_V, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, ODIN3_SINK_V);
    size_t got = fread(g_sink, 1, sizeof g_sink - 1U, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    g_sink[got] = '\0';
    return g_sink;
}

static bool g_saw_error;

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    (void)msg;
    g_saw_error |= level == ODIN3_LOG_ERROR;
}

static void fresh_quiet_design(void) {
    odin3_design_destroy(pt_rs.design);
    pt_rs.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(pt_rs.design);
    odin3_log_set_sink(quiet_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
}

/* What a sweep reads: the pass arguments and the modules the finished design holds. */
static const char *g_args = "t.v";
static uint32_t g_modules = 2;

static bool read_complete(void) {
    return odin3_design_module_end(pt_rs.design) == g_modules + 1U &&
           odin3_design_top(pt_rs.design).v != 0;
}

/* A swallowed failure is an OK read that is incomplete or that logged an error. */
static void check_not_swallowed(odin3_status st) {
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE_MESSAGE(g_saw_error, "an OK read logged an error");
        TEST_ASSERT_TRUE_MESSAGE(read_complete(), "an OK read is incomplete");
    }
}

/* One run of the whole read of sink.v with the limit-th allocation failing: NO_MEMORY (the IR
 * valid for FULL) or an OK read that is complete and logged no error. */
static odin3_status read_sink_at(long limit) {
    fresh_quiet_design();
    g_saw_error = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(pt_rs.design, "read_verilog", odin3_bytes_cstr(g_args));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_not_swallowed(st);
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(pt_rs.design, full));
    return st;
}

/* Every allocation failure of the whole read in turn: NO_MEMORY or a complete OK read, then
 * OOM_SETTLE complete ones in a row. */
static uint32_t sweep_files(const et_src *srcs, uint32_t count) {
    et_req req = {srcs, count, NULL, NULL, NULL};
    et_setup(&req, &pt_rs);
    uint32_t failures = 0;
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = read_sink_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK && read_complete() ? settled + 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    return failures;
}

static void test_oom_sweep_sink(void) {
    et_src src = {"t.v", sink_text()};
    TEST_ASSERT_GREATER_THAN_UINT32(1000, sweep_files(&src, 1));
}

/* The same for initial contents read from a file (the path resolved beside the source). */
static void test_oom_sweep_readmem(void) {
    const et_src srcs[] = {{"sub/t.v", "module t(input [1:0] ra, output [7:0] q);\n"
                                       "  reg [7:0] m [0:3];\n  reg [3:0] r = 4'd5;\n"
                                       "  initial $readmemh(\"init.hex\", m);\n"
                                       "  assign q = m[ra] ^ r;\nendmodule\n"},
                           {"sub/init.hex", data_text(0, "init.hex")}};
    g_args = "sub/t.v";
    g_modules = 1;
    TEST_ASSERT_GREATER_THAN_UINT32(100, sweep_files(srcs, 2));
    g_args = "t.v";
    g_modules = 2;
}

/* The RTLIL of a run of sink.v. */
static const char *sink_rtlil(void) {
    static char path[ET_PATH + 16];
    et_src src = {"t.v", sink_text()};
    pt_read_files(&src, 1, "t.v");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/out.il", pt_rs.dir) > 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(pt_rs.design, "write_rtlil", odin3_bytes_cstr(path)),
        et_log(&pt_rs));
    return path;
}

static void test_sink_is_deterministic(void) {
    static char first[1 << 20];
    static char second[1 << 20];
    const char *path = sink_rtlil();
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(first, 1, sizeof first - 1U, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    first[len] = '\0';
    TEST_ASSERT_TRUE(len > 1000);
    tearDown();
    setUp();
    path = sink_rtlil();
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len2 = fread(second, 1, sizeof second - 1U, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    second[len2] = '\0';
    TEST_ASSERT_EQUAL_size_t(len, len2);
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* --- the write primitive under allocation failure and misuse (white-box) -----------------------
 */

typedef struct wb_probe {
    bool ran;
    uint32_t failures, settled;
} wb_probe;

static wb_probe g_wb;

/* The first node of the given kind under the module. */
static odin3_ast_id wb_first(odin3_elab2 *e2, odin3_ast_id module_node, odin3_ast_kind kind) {
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, e2->elab, module_node));
    odin3_ast_event ev;
    odin3_ast_id node;
    odin3_ast_id found = {0};
    while (found.v == 0 && odin3_ast_cursor_next(&cur, &ev, &node)) {
        found = ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == kind ? node : found;
    }
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, found.v);
    return found;
}

/* A target that is no memory word, a value of the wrong width and no request are refused. */
static void wb_misuse(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    odin3_ast_id write = wb_first(e2, module_node, ODIN3_AST_NONBLOCKING_ASSIGN);
    odin3_ast_id plain = wb_first(e2, module_node, ODIN3_AST_NET_ASSIGN);
    odin3_sig sig = {NULL, 0, false};
    odin3_pg_assign_req narrow = {write, odin3_ast_child(e2->elab, write, 0), &sig, false};
    odin3_pg_assign_req plain_req = {plain, odin3_ast_child(e2->elab, plain, 0), &sig, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_mem_write(pg, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_mem_write(pg, &narrow));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_mem_write(pg, &plain_req));
    odin3_pg_destroy(pg);
}

/* odin3_pg_always over a block with a conditional write: every failing point is NO_MEMORY. */
static void wb_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    odin3_ast_id always = wb_first(e2, module_node, ODIN3_AST_ALWAYS);
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

static void test_oom_sweep_write_primitive(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = wb_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    pt_read_src(k_cond);
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32(1, g_wb.settled);
    TEST_ASSERT_GREATER_THAN_UINT32(5, g_wb.failures);
    pt_expect_ir_ok();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_conditional_write_port);
    RUN_TEST(test_constant_index_and_value);
    RUN_TEST(test_other_bases);
    RUN_TEST(test_partial_write);
    RUN_TEST(test_two_writes_in_source_order);
    RUN_TEST(test_two_blocks_two_ports);
    RUN_TEST(test_unconditional_blocking_write);
    RUN_TEST(test_negedge_polarity);
    RUN_TEST(test_writes_in_every_context);
    RUN_TEST(test_nomem2reg_combinational_write);
    RUN_TEST(test_concatenation_target);
    RUN_TEST(test_variable_bit_of_a_word);
    RUN_TEST(test_initial_value_of_a_register);
    RUN_TEST(test_initial_register_assigned_in_task);
    RUN_TEST(test_initial_register_as_task_output_actual);
    RUN_TEST(test_initial_register_as_task_inout_actual);
    RUN_TEST(test_initial_register_assigned_in_function);
    RUN_TEST(test_initial_task_call_not_applied);
    RUN_TEST(test_initial_memory_contents);
    RUN_TEST(test_initial_statement_not_constant);
    RUN_TEST(test_initial_block_statements_are_separate);
    RUN_TEST(test_initial_only_variable_is_constant);
    RUN_TEST(test_initial_value_of_a_driven_net);
    RUN_TEST(test_init_bits_cap);
    RUN_TEST(test_readmemh);
    RUN_TEST(test_readmemb_with_start_and_finish);
    RUN_TEST(test_readmem_errors);
    RUN_TEST(test_readmem_dropped_words_warn);
    RUN_TEST(test_readmem_words_past_the_end_warn);
    RUN_TEST(test_readmem_outside_initial);
    RUN_TEST(test_readmem_size_cap);
    RUN_TEST(test_inlined_task_output_counts_as_blocking);
    RUN_TEST(test_bits_split_across_blocks_are_fine);
    RUN_TEST(test_pending_tables_are_empty);
    RUN_TEST(test_oom_sweep_sink);
    RUN_TEST(test_oom_sweep_readmem);
    RUN_TEST(test_sink_is_deterministic);
    RUN_TEST(test_oom_sweep_write_primitive);
    return UNITY_END();
}
