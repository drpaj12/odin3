/*
 * test_icells.c — 5.2 `read_verilog -icells` (spec docs/specs/2026-10-20-5.2-icells-design.md):
 * item 0 of the resolution (I-1 to I-7, N-1), the validation against the registry (V-1 to V-6)
 * and the behaviour without the option (T-1). The whole pass runs through the pass manager.
 */
#include "elab_test.h"

#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* Reads t.v (the given text) with the pass arguments `args` (the file name is appended). */
typedef struct read_req {
    const char *args;
    const char *text;
} read_req;

static void read_text(read_req what) {
    const char *args = what.args;
    const char *text = what.text;
    et_src file = {"t.v", text};
    et_req req = {&file, 1, NULL, NULL, NULL};
    char line[256];
    int got = snprintf(line, sizeof line, "%s t.v", args);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof line);
    et_setup(&req, &rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, ODIN3_KEEP_AST_NONE));
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr(line));
}

#define read_with(args, text) read_text((read_req){(args), (text)})

static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

static void expect_error(const char *line) {
    et_expect(&rs, ODIN3_LOG_ERROR, line);
}

static const odin3_module *top(void) {
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        const odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        if (module != NULL && odin3_module_name(module) != 0) {
            const char *name =
                odin3_strtab_get(odin3_design_strtab(rs.design), odin3_module_name(module));
            if (name != NULL && strcmp(name, "top") == 0) {
                return module;
            }
        }
    }
    TEST_FAIL_MESSAGE("no module top");
    return NULL;
}

/* The nth live node of type `type` in top, 0 when none. */
static odin3_node_id nth_node(const char *type, uint32_t nth) {
    const odin3_module *module = top();
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(rs.design, odin3_node_type(module, node));
        if (odin3_node_live(module, node) && def != NULL && strcmp(def->name, type) == 0 &&
            seen++ == nth) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

#define ADD_PARAMS ".A_SIGNED(0), .B_SIGNED(0), .A_WIDTH(4), .B_WIDTH(4), .Y_WIDTH(4)"
#define ADD_TOP                                                                                    \
    "module top(input [3:0] a, input [3:0] b, output [3:0] y);\n"                                  \
    "  \\$add #(" ADD_PARAMS ") u (.A(a), .B(b), .Y(y));\n"                                        \
    "endmodule\n"

/* T-1: without the option an instance of a `$` name is still an unresolved module. */
static void test_off_by_default(void) {
    read_with("", ADD_TOP);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:2:3: error: unresolved_module: module '$add' is not defined");
}

/* I-1, I-3: both spellings read the instance as the registered cell, with its parameters. */
static void test_resolves_as_cell(void) {
    const char *spellings[] = {"-icells", "--icells"};
    for (uint32_t i = 0; i < 2; i++) {
        read_with(spellings[i], ADD_TOP);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
        odin3_node_id node = nth_node("$add", 0);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
        TEST_ASSERT_EQUAL_INT64(4, odin3_node_param(top(), node, 2)->i); /* A_WIDTH */
        TEST_ASSERT_EQUAL_UINT32(0, nth_node("$add", 1).v);
        expect_ir_ok();
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
}

/* I-5: an unregistered `$` name is an error, also with --auto-blackbox. */
static void test_unknown_cell(void) {
    const char *text = "module top; \\$nosuch u (); endmodule\n";
    read_with("-icells", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:1:13: error: unresolved_module: cell type '$nosuch' is not registered "
                 "(read_verilog -icells)");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_with("-icells --auto-blackbox", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR, "'$nosuch' is not registered"));
}

/* I-4: a process and a port type cannot be instantiated. */
static void test_refused_types(void) {
    read_with("-icells", "module top; \\$proc u (); \\$port_in p (); endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:1:13: error: cell type '$proc' cannot be instantiated from Verilog "
                 "(a process; run proc first)");
    expect_error("t.v:1:26: error: cell type '$port_in' cannot be instantiated from Verilog "
                 "(a module port)");
}

/* I-6: a user module resolves as it does without the option, in the same read. */
static void test_user_module_unchanged(void) {
    read_with("-icells", "module sub(input a, output y); assign y = ~a; endmodule\n"
                         "module top(input a, output y, output z);\n"
                         "  sub s (.a(a), .y(y));\n"
                         "  \\$not #(.A_SIGNED(0), .A_WIDTH(1), .Y_WIDTH(1)) n (.A(a), .Y(z));\n"
                         "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, nth_node("$not", 0).v);
    expect_ir_ok();
}

/* I-2: refused beside --lib and --parse-only. */
static void test_refused_combinations(void) {
    read_with("-icells --lib", ADD_TOP);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rs.status);
    expect_error("read_verilog: -icells cannot be combined with --lib (it is an option of the "
                 "design read)");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_with("-icells --parse-only", ADD_TOP);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rs.status);
}

/* N-1: a (* blackbox *) stub of the writer with the name of the cell does not shadow it. */
static void test_stub_does_not_shadow(void) {
    read_with("-icells", "(* blackbox *) module \\$add #(parameter A_SIGNED = 0) (input [3:0] A, "
                         "input [3:0] B, output [3:0] Y);\nendmodule\n" ADD_TOP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, nth_node("$add", 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_INFO,
                                              "module '$add' has the name of a registered cell "
                                              "type; ignored (read_verilog -icells)"));
    expect_ir_ok();
}

/* V-1, V-4: an unknown parameter and an unknown port, located. */
static void test_unknown_parameter_and_port(void) {
    read_with("-icells", "module top(input a);\n"
                         "  \\$add #(.NOPE(1)) u1 ();\n"
                         "  \\$add u2 (.Q(a));\n"
                         "endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:2:11: error: cell type '$add' has no parameter 'NOPE'");
    expect_error("t.v:3:13: error: cell type '$add' has no port 'Q'");
}

/* V-3: a connection must have the width of its port, input and output; open is allowed. */
static void test_strict_widths(void) {
    read_with("-icells",
              "module top(input [3:0] a, input [3:0] b, output [4:0] z, output [3:0] y);\n"
              "  \\$add #(" ADD_PARAMS ") u (.A(a[2:0]), .B(b), .Y(z));\n"
              "  \\$add #(" ADD_PARAMS ") v (.A(a), .Y(y));\n"
              "endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR,
                                              "port 'A' of '$add' instance 'u' is 4 bits; the "
                                              "connection is 3 bits"));
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR,
                                              "port 'Y' of '$add' instance 'u' is 4 bits; the "
                                              "connection is 5 bits"));
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_ERROR, "instance 'v'"));
}

/* Open pins read: an input as x, an output as a fresh net. */
static void test_open_ports(void) {
    read_with("-icells", "module top(input [3:0] a, output [3:0] y);\n"
                         "  \\$add #(" ADD_PARAMS ") v (.A(a), .Y(y));\n"
                         "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* V-2: the kind of each parameter value is checked (an INT in stage 1, a STRING in stage 2). */
static void test_parameter_kinds(void) {
    read_with("-icells", "module top;\n"
                         "  \\$add #(.A_SIGNED(\"x\")) u1 ();\n"
                         "endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:2:11: error: value of parameter 'A_SIGNED' of cell type '$add' is not an "
                 "integer");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_with("-icells", "module top;\n"
                         "  \\$mem #(.MEMID(5)) u2 ();\n"
                         "endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_error("t.v:2:11: error: value of parameter 'MEMID' of cell type '$mem' is not a string");
}

#define MEM_ZERO_PORTS(width, init)                                                                \
    "module top;\n"                                                                                \
    "  \\$mem #(.MEMID(\"m\"), .SIZE(1), .OFFSET(0), .ABITS(1), .WIDTH(" width                     \
    "), .RD_PORTS(0), .WR_PORTS(0), .RD_CLK_ENABLE(\"\"), .RD_CLK_POLARITY(\"\"), "                \
    ".RD_TRANSPARENT(\"\"), .WR_CLK_ENABLE(\"\"), .WR_CLK_POLARITY(\"\"), .INIT(" init             \
    ")) m ();\n"                                                                                   \
    "endmodule\n"

/* V-5: a string given to a BITS parameter is its bytes, "" the zero-length value. */
static void test_string_bits(void) {
    read_with("-icells", MEM_ZERO_PORTS("2", "\"ab\""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    const odin3_value *init = odin3_node_param(top(), nth_node("$mem", 0), 12);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, init->kind);
    TEST_ASSERT_EQUAL_UINT32(16, init->len);
    TEST_ASSERT_EQUAL_UINT8(0, init->bits[0]); /* 'b' is 0x62: LSB first 0,1,0,0,0,1,1,0 */
    TEST_ASSERT_EQUAL_UINT8(1, init->bits[1]);
    TEST_ASSERT_EQUAL_UINT8(1, init->bits[8]); /* 'a' is 0x61: bit 0 */
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_with("-icells", MEM_ZERO_PORTS("2", "\"\""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_param(top(), nth_node("$mem", 0), 12)->len);
}

/* V-6: the type's own parameter rules, located on the instance. */
static void test_verify_located(void) {
    read_with("-icells", MEM_ZERO_PORTS("0", "\"\""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR,
                                              "error: instance 'm' of '$mem': the parameter "
                                              "values are not valid for the cell type (see the "
                                              "message above)"));
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR, "t.v:2:"));
}

/* L1: a user module whose name begins with `$` and is not a registered type resolves as a module.
 */
static void test_user_module_with_dollar_name(void) {
    const char *text = "module \\$mine (input a, output y); assign y = ~a; endmodule\n"
                       "module top(input a, output y); \\$mine u (.a(a), .y(y)); endmodule\n";
    read_with("", text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_with("-icells", text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* The parameter at index of the first cell of the type, as an INT. */
static int64_t int_param(const char *type, uint32_t index) {
    odin3_node_id node = nth_node(type, 0);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, type);
    return odin3_node_param(top(), node, index)->i;
}

static void read_cell_ok(const char *text) {
    read_with("-icells", text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* T-2: each family the writer emits as an instance, and the bit cells, with non-default values. */
static void test_shiftx_cell(void) {
    read_cell_ok("module top(input [7:0] a, input [2:0] b, output [3:0] y);\n"
                 "  \\$shiftx #(.A_SIGNED(0), .B_SIGNED(1), .A_WIDTH(8), .B_WIDTH(3), .Y_WIDTH(4)) "
                 "u (.A(a), .B(b), .Y(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, int_param("$shiftx", 1));
    TEST_ASSERT_EQUAL_INT64(8, int_param("$shiftx", 2));
    TEST_ASSERT_EQUAL_INT64(4, int_param("$shiftx", 4));
}

static void test_dffsr_cell(void) {
    read_cell_ok("module top(input clk, input s, input r, input [3:0] d, output [3:0] q);\n"
                 "  \\$dffsr #(.WIDTH(4), .CLK_POLARITY(0), .SET_POLARITY(1), .CLR_POLARITY(0)) u "
                 "(.CLK(clk), .SET({4{s}}), .CLR({4{r}}), .D(d), .Q(q));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(4, int_param("$dffsr", 0));
    TEST_ASSERT_EQUAL_INT64(0, int_param("$dffsr", 1));
    TEST_ASSERT_EQUAL_INT64(1, int_param("$dffsr", 2));
    TEST_ASSERT_EQUAL_INT64(0, int_param("$dffsr", 3));
}

static void test_dlatch_cell(void) {
    read_cell_ok(
        "module top(input en, input [2:0] d, output [2:0] q);\n"
        "  \\$dlatch #(.WIDTH(3), .EN_POLARITY(0)) u (.EN(en), .D(d), .Q(q));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(3, int_param("$dlatch", 0));
    TEST_ASSERT_EQUAL_INT64(0, int_param("$dlatch", 1));
}

static void test_bit_flip_flops(void) {
    read_cell_ok("module top(input c, input d, output q1, output q2);\n"
                 "  \\$_DFF_P_ u1 (.C(c), .D(d), .Q(q1));\n"
                 "  \\$_DFF_N_ u2 (.C(c), .D(d), .Q(q2));\nendmodule\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, nth_node("$_DFF_P_", 0).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, nth_node("$_DFF_N_", 0).v);
}

static void test_lut_cell(void) {
    read_cell_ok("module top(input [1:0] a, output y);\n"
                 "  \\$lut #(.WIDTH(2), .LUT(4'b1000)) u (.A(a), .Y(y));\nendmodule\n");
    const odin3_value *lut = odin3_node_param(top(), nth_node("$lut", 0), 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, lut->kind);
    TEST_ASSERT_EQUAL_UINT32(4, lut->len);
    TEST_ASSERT_EQUAL_UINT8(1, lut->bits[3]);
    TEST_ASSERT_EQUAL_UINT8(0, lut->bits[0]);
}

/* M1: the writer's quoted COVER rows read back; bad text is a located, accurate error. */
static void test_sop_cover(void) {
    read_cell_ok("module top(input [1:0] a, output y);\n"
                 "  \\$sop #(.WIDTH(2), .COVER(\"111 -01\")) u (.A(a), .Y(y));\nendmodule\n");
    const odin3_value *cover = odin3_node_param(top(), nth_node("$sop", 0), 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_COVER, cover->kind);
    TEST_ASSERT_EQUAL_UINT32(2, cover->cover_inputs);
    TEST_ASSERT_EQUAL_UINT32(6, cover->len);
    TEST_ASSERT_EQUAL_MEMORY("111-01", cover->bits, 6);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_cell_ok("module top(input [1:0] a, output y);\n"
                 "  \\$sop #(.WIDTH(2), .COVER(\"\")) u (.A(a), .Y(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_param(top(), nth_node("$sop", 0), 1)->len);
}

static void test_sop_cover_errors(void) {
    const char *bad[] = {"11x 1", "111 11", "111 1", "111  1"};
    for (uint32_t i = 0; i < 4; i++) {
        char text[256];
        (void)snprintf(text, sizeof text,
                       "module top(input [1:0] a, output y);\n"
                       "  \\$sop #(.WIDTH(2), .COVER(\"%s\")) u (.A(a), .Y(y));\nendmodule\n",
                       bad[i]);
        read_with("-icells", text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, bad[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(
            1,
            et_count_with(&rs, ODIN3_LOG_ERROR,
                          "value of parameter 'COVER' of cell type '$sop' is not a quoted list of "
                          "cover rows"),
            bad[i]);
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_off_by_default);
    RUN_TEST(test_resolves_as_cell);
    RUN_TEST(test_unknown_cell);
    RUN_TEST(test_refused_types);
    RUN_TEST(test_user_module_unchanged);
    RUN_TEST(test_refused_combinations);
    RUN_TEST(test_stub_does_not_shadow);
    RUN_TEST(test_unknown_parameter_and_port);
    RUN_TEST(test_strict_widths);
    RUN_TEST(test_open_ports);
    RUN_TEST(test_parameter_kinds);
    RUN_TEST(test_string_bits);
    RUN_TEST(test_verify_located);
    RUN_TEST(test_user_module_with_dollar_name);
    RUN_TEST(test_shiftx_cell);
    RUN_TEST(test_dffsr_cell);
    RUN_TEST(test_dlatch_cell);
    RUN_TEST(test_bit_flip_flops);
    RUN_TEST(test_lut_cell);
    RUN_TEST(test_sop_cover);
    RUN_TEST(test_sop_cover_errors);
    return UNITY_END();
}
