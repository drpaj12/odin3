/*
 * test_elab_vtr_initial.c — the VTR-compat ruling on `initial x = <non-constant>` (Peter,
 * 2026-10-13; 2D spec §3.3 EL-40, §9.2, §17): with `read_verilog --vtr-compat` the statement is a
 * continuous assignment (Yosys 0.55 and Parmys read it as a combinational process whose only
 * driver is the statement), with a located warning; without the option it is ignored as before;
 * a target that an always block, an instance or an assign also drives keeps the ignore, with a
 * warning that says why. Every source goes through the whole read_verilog pass (proc_test.h).
 */
#include "elab_test.h"
#include "proc_test.h"

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef ODIN3_ELAB_TMP
#error "ODIN3_ELAB_TMP must name the scratch directory"
#endif

void setUp(void) {
    memset(&pt_rs, 0, sizeof pt_rs);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    if (pt_rs.design != NULL || pt_rs.dir[0] != '\0') {
        et_free(&pt_rs);
    }
}

static const char k_vtr[] = "--vtr-compat t.v";
static const char k_plain[] = "t.v";

static void read_ok_with(const char *args, const char *text) {
    pass_src src = {args, text};
    pt_read_with(&src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_diag_errors(), et_log(&pt_rs));
    pt_expect_ir_ok();
}

static bool same_net(const char *wire_a, uint32_t bit_a, const char *wire_b, uint32_t bit_b) {
    return pt_wnet(wire_a, bit_a).v == pt_wnet(wire_b, bit_b).v;
}

/* The micro's shape: the constant statement still sets its value, the other is the assign. */
static void test_vtr_micro_shape(void) {
    read_ok_with(k_vtr, "module t(input a, output reg [1:0] o1, output reg o2);\n"
                        "  initial begin\n    o1 = 2'b10;\n    o2 = a;\n  end\nendmodule\n");
    pt_expect_warning((at_loc){4, 5}, "initial value of 'o2' is not constant: treated as a "
                                      "continuous assign (VTR/Parmys compatibility)");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    TEST_ASSERT_TRUE(same_net("o2", 0, "a", 0));
    TEST_ASSERT_EQUAL_UINT32(pt_const_net_of(ODIN3_BIT_0).v, pt_wnet("o1", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pt_const_net_of(ODIN3_BIT_1).v, pt_wnet("o1", 1).v);
}

/* A statement on its own, blocking or not, whole variable or a constant select. */
static void test_vtr_single_statement_forms(void) {
    read_ok_with(k_vtr, "module t(input [3:0] a, output reg [3:0] r2);\n"
                        "  initial r2 = a;\nendmodule\n");
    pt_expect_warning((at_loc){2, 11}, "initial value of 'r2' is not constant: treated as a "
                                       "continuous assign (VTR/Parmys compatibility)");
    for (uint32_t bit = 0; bit < 4; bit++) {
        TEST_ASSERT_TRUE(same_net("r2", bit, "a", bit));
    }
    tearDown();
    setUp();
    read_ok_with(k_vtr, "module t(input a, output reg y);\n  initial y <= a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("treated as a continuous assign"));
    TEST_ASSERT_TRUE(same_net("y", 0, "a", 0));
    tearDown();
    setUp();
    read_ok_with(k_vtr, "module t(input a, output reg [1:0] y);\n  initial y[1] = a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("'y' is not constant: treated as a continuous"));
    TEST_ASSERT_TRUE(same_net("y", 1, "a", 0));
}

/* A delay in front of the statement is ignored, as Yosys ignores it. */
static void test_vtr_delayed_statement(void) {
    read_ok_with(k_vtr, "module t(input a, output reg y);\n  initial #5 y = a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("treated as a continuous assign"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("delay is ignored"));
    TEST_ASSERT_TRUE(same_net("y", 0, "a", 0));
}

/* An expression, not only a wire. */
static void test_vtr_expression(void) {
    read_ok_with(k_vtr, "module t(input a, b, output reg y);\n  initial y = a & b;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("treated as a continuous assign"));
    TEST_ASSERT_FALSE(same_net("y", 0, "a", 0));
    TEST_ASSERT_FALSE(same_net("y", 0, "b", 0));
}

/* The general case: no option, the statement is ignored with the old warning, y stays x. */
static void test_plain_ignores(void) {
    read_ok_with(k_plain, "module t(input [3:0] a, output reg [3:0] r2);\n"
                          "  initial r2 = a;\nendmodule\n");
    pt_expect_warning((at_loc){2, 11}, "initial statement is not constant and is ignored");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("continuous assign"));
    TEST_ASSERT_FALSE(same_net("r2", 0, "a", 0));
}

/* Also assigned in an always block: ignored, and the warning says why. */
static void test_vtr_always_driven_is_ignored(void) {
    read_ok_with(k_vtr, "module t(input a, c, d, output reg y);\n"
                        "  initial y = a;\n  always @(posedge c) y <= d;\nendmodule\n");
    pt_expect_warning((at_loc){2, 11},
                      "initial value of 'y' is not constant and is ignored: 'y' is also assigned "
                      "in an always block (VTR/Parmys compatibility does not apply)");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    TEST_ASSERT_FALSE(same_net("y", 0, "a", 0));
}

/* One bit of the target is assigned in an always block: the whole statement keeps the ignore. */
static void test_vtr_partly_always_driven_is_ignored(void) {
    read_ok_with(k_vtr, "module t(input a, c, d, output reg [1:0] y);\n"
                        "  initial y = {a, a};\n  always @(posedge c) y[0] <= d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("is also assigned in an always block"));
    TEST_ASSERT_FALSE(same_net("y", 1, "a", 0));
}

/* Another driver, not an always block: the ignore, naming the driver. */
static void test_vtr_continuous_driven_is_ignored(void) {
    read_ok_with(k_vtr, "module t(input a, b, output reg w);\n  assign w = a;\n"
                        "  initial w = b;\nendmodule\n");
    pt_expect_warning((at_loc){3, 11},
                      "initial value of 'w' is not constant and is ignored: 'w' is driven by a "
                      "continuous assignment (VTR/Parmys compatibility does not apply)");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("treated as a continuous assign"));
    TEST_ASSERT_TRUE(same_net("w", 0, "a", 0));
}

/* A constant statement is untouched by the option (no warning of the ruling). */
static void test_vtr_constant_statement_unchanged(void) {
    read_ok_with(k_vtr, "module t(output y);\n  reg k;\n  initial k = 1;\n  assign y = k;\n"
                        "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(pt_const_net_of(ODIN3_BIT_1).v, pt_wnet("k", 0).v);
}

/* A statement that is not a plain assignment (an if) keeps the ignore under the option. */
static void test_vtr_other_statement_is_ignored(void) {
    read_ok_with(k_vtr, "module t(input a, c, output reg y);\n"
                        "  initial begin if (c) y = a; end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("initial statement is not constant and is "
                                                 "ignored"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("continuous assign"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_vtr_micro_shape);
    RUN_TEST(test_vtr_single_statement_forms);
    RUN_TEST(test_vtr_expression);
    RUN_TEST(test_vtr_delayed_statement);
    RUN_TEST(test_plain_ignores);
    RUN_TEST(test_vtr_always_driven_is_ignored);
    RUN_TEST(test_vtr_partly_always_driven_is_ignored);
    RUN_TEST(test_vtr_continuous_driven_is_ignored);
    RUN_TEST(test_vtr_constant_statement_unchanged);
    RUN_TEST(test_vtr_other_statement_is_ignored);
    return UNITY_END();
}
