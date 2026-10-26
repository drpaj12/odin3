/*
 * test_opclass_passes.c — each migrated pass declares the class of V1.2.0 3.1 (IR-12a): proc and
 * lower refine, opt rewrite with merge coarsen, flatten context with its tail, hardmap, lpmmap
 * and unmap rewrite. The abc cases (coarsen, and the bulk record) are in test_abc_prov.c.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/opclass.h"
#include "odin3/odin3.h"
#include "opclass_expect.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
#endif

enum { PATH_CAP = 700, ARGS_CAP = 800 };

#define LIB(name) ODIN3_SOURCE_DIR "/lib/" name
#define MICRO(name) ODIN3_SOURCE_DIR "/tests/micro/verilog/" name

static const odin3_opclass PROC = {ODIN3_DIR_REFINE, ODIN3_LEVEL_STMT, ODIN3_LEVEL_WORD, false, 0};
static const odin3_opclass LOWER = {ODIN3_DIR_REFINE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_GATE, false, 0};
static const odin3_opclass REWRITE = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                      0};
static const odin3_opclass COARSEN = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                      0};
static const odin3_opclass CONTEXT1 = {ODIN3_DIR_CONTEXT, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                       1};

static odin3_design *design;
static char tmp_path[] = "/tmp/odin3_opclass_XXXXXX";
static const char TMP_TEMPLATE[] = "/tmp/odin3_opclass_XXXXXX";
static bool have_tmp;

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    have_tmp = false;
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
    if (have_tmp) {
        (void)unlink(tmp_path);
    }
}

static void run(const char *pass, const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(design, pass, args), pass);
}

static void read_file(const char *flags, const char *path) {
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "%s \"%s\"", flags, path);
    run("read_verilog", args);
}

/* Writes text to a temporary .v file, reads it. */
static void read_text(const char *text) {
    (void)memcpy(tmp_path, TMP_TEMPLATE, sizeof TMP_TEMPLATE);
    int fd = mkstemp(tmp_path);
    TEST_ASSERT_TRUE(fd >= 0);
    have_tmp = true;
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)write(fd, text, strlen(text)));
    (void)close(fd);
    read_file("--nolatches", tmp_path);
}

static void check_full(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

static const char *const IF_ALWAYS =
    "module m(input clk, input a, input b, input [3:0] x, output reg [3:0] q);\n"
    "  always @(posedge clk) begin\n"
    "    if (a) q <= x + 4'd1; else if (b) q <= x & 4'd5; else q <= x;\n"
    "  end\n"
    "endmodule\n";

static void test_proc_is_refine_stmt_to_word(void) {
    read_text(IF_ALWAYS);
    run("proc", "");
    /* proc runs opt_expr inside its run: those records are the opt kit's rewrite */
    TEST_ASSERT_TRUE(opx_count(design, "proc", &PROC, true) > 0);
    TEST_ASSERT_EQUAL_UINT32(opx_count(design, "proc", &REWRITE, true),
                             opx_count(design, "proc", &PROC, false));
    check_full();
}

static void test_lower_is_refine_word_to_gate(void) {
    run("read_techlib", "\"" LIB("vtr.o3lib") "\"");
    read_text(IF_ALWAYS);
    run("proc", "");
    run("opt", "");
    run("hardmap", "");
    run("lower", "");
    opx_all(design, "lower", &LOWER);
    check_full();
}

/* Two identical expressions: opt_merge folds them (CSE, coarsen); the rest of opt is rewrite. */
static void test_opt_is_rewrite_and_merge_is_coarsen(void) {
    read_text("module m(input clk, input d, input e, output reg q1, output reg q2, output y);\n"
              "  always @(posedge clk) q1 <= d;\n"
              "  always @(posedge clk) q2 <= d;\n"
              "  assign y = q1 ^ q2 ^ e;\n"
              "endmodule\n");
    run("proc", "");
    run("opt", "");
    TEST_ASSERT_TRUE(opx_count(design, "opt", &COARSEN, true) > 0);
    TEST_ASSERT_TRUE(opx_count(design, "opt", &REWRITE, true) > 0);
    /* nothing else: what is not rewrite is coarsen */
    TEST_ASSERT_EQUAL_UINT32(opx_count(design, "opt", &REWRITE, false),
                             opx_count(design, "opt", &COARSEN, true));
    check_full();
}

/* Two instances of one child: each flatten record is context, the instance record the tail. */
static void test_flatten_is_context_with_tail(void) {
    read_text("module child(input a, input b, output y);\n"
              "  assign y = a & b;\n"
              "endmodule\n"
              "module m(input a, input b, input c, output y1, output y2);\n"
              "  child u1(.a(a), .b(b), .y(y1));\n"
              "  child u2(.a(b), .b(c), .y(y2));\n"
              "endmodule\n");
    run("proc", "");
    run("flatten", "");
    opx_all(design, "flatten", &CONTEXT1);
    check_full();
}

/* Writes text to tmp_path (no read) and returns the path. */
static const char *write_tmp(const char *text) {
    char tmpl[] = "/tmp/odin3_opclass_XXXXXX";
    (void)memcpy(tmp_path, tmpl, sizeof tmpl);
    int fd = mkstemp(tmp_path);
    TEST_ASSERT_TRUE(fd >= 0);
    have_tmp = true;
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)write(fd, text, strlen(text)));
    (void)close(fd);
    return tmp_path;
}

static const char *const ARITH =
    "module m(input [15:0] a, input [15:0] b, input [7:0] c, input [7:0] d, "
    "output [16:0] s, output [15:0] p);\n"
    "  assign s = a + b;\n  assign p = c * d;\nendmodule\n";

static void arith_flow(const char *lib) {
    run("read_techlib", lib);
    read_text(ARITH);
    run("proc", "");
    run("opt", "");
}

static void test_hardmap_is_rewrite(void) {
    arith_flow("\"" LIB("vtr.o3lib") "\"");
    run("hardmap", "");
    opx_all(design, "hardmap", &REWRITE);
    check_full();
}

static void test_lpmmap_is_rewrite(void) {
    arith_flow("\"" LIB("lpm.o3lib") "\"");
    run("lpmmap", "");
    opx_all(design, "lpmmap", &REWRITE);
    check_full();
}

static void test_fsm_map_has_rewrite_and_coarsen(void) {
    read_file("--nolatches", ODIN3_SOURCE_DIR "/tests/fsm/moore_arst.v");
    run("proc", "");
    run("fsm", "");
    TEST_ASSERT_TRUE(opx_count(design, "fsm", &COARSEN, true) > 0);
    TEST_ASSERT_EQUAL_UINT32(opx_count(design, "fsm", &REWRITE, false),
                             opx_count(design, "fsm", &COARSEN, true));
    check_full();
}

/* A mux in front of D folds into the flop as an enable (dff_merge: coarsen over flop and mux). */
static void test_dff_merge_is_coarsen(void) {
    read_text("module m(input clk, input en, input [3:0] d, output reg [3:0] q);\n"
              "  always @(posedge clk) if (en) q <= d;\n"
              "endmodule\n");
    run("proc", "");
    run("opt", "");
    TEST_ASSERT_TRUE(opx_count(design, "opt", &COARSEN, true) > 0);
    TEST_ASSERT_EQUAL_UINT32(opx_count(design, "opt", &REWRITE, false),
                             opx_count(design, "opt", &COARSEN, true));
    check_full();
}

/* The three readers' restored locations are rewrite (one operation each). */
static void test_readers_are_rewrite(void) {
    const char *path = write_tmp(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname g\n"
                                 ".attr src \"a.v:3.1-4.2\"\n.end\n");
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "--hdl-locs \"%s\"", path);
    run("read_blif", args);
    opx_all(design, "read_blif", &REWRITE);
}

static void test_json_reader_is_rewrite(void) {
    const char *path =
        write_tmp("{\"modules\":{\"top\":{\"attributes\":{\"top\":1},\"ports\":{\"a\":{"
                  "\"direction\":\"input\","
                  "\"bits\":[2]},\"y\":{\"direction\":\"output\",\"bits\":[3]}},\"cells\":{\"g\":{"
                  "\"hide_name\":0,\"type\":\"$_NOT_\",\"parameters\":{},\"attributes\":{\"src\":"
                  "\"t.v:3.1-3.9\"},\"port_directions\":{\"A\":\"input\",\"Y\":\"output\"},"
                  "\"connections\":{\"A\":[2],\"Y\":[3]}}},\"netnames\":{}}}}\n");
    char args[ARGS_CAP];
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    run("read_json", args);
    opx_all(design, "read_json", &REWRITE);
}

static void test_netlist_reader_is_rewrite(void) {
    (void)write_tmp("module m(input a, output y);\n  assign y = ~a; // t.v:3\nendmodule\n");
    read_file("--nolatches -netlist", tmp_path);
    opx_all(design, "read_verilog", &REWRITE);
}

static void test_unmap_is_rewrite(void) {
    run("read_techlib", "\"" LIB("vtr.o3lib") "\"");
    read_text(IF_ALWAYS);
    run("proc", "");
    run("opt", "");
    run("hardmap", "");
    run("lower", "");
    run("unmap", "");
    TEST_ASSERT_EQUAL_UINT32(0, opx_count(design, "unmap", &REWRITE, false));
    check_full();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_proc_is_refine_stmt_to_word);
    RUN_TEST(test_lower_is_refine_word_to_gate);
    RUN_TEST(test_opt_is_rewrite_and_merge_is_coarsen);
    RUN_TEST(test_flatten_is_context_with_tail);
    RUN_TEST(test_unmap_is_rewrite);
    RUN_TEST(test_hardmap_is_rewrite);
    RUN_TEST(test_lpmmap_is_rewrite);
    RUN_TEST(test_fsm_map_has_rewrite_and_coarsen);
    RUN_TEST(test_dff_merge_is_coarsen);
    RUN_TEST(test_readers_are_rewrite);
    RUN_TEST(test_json_reader_is_rewrite);
    RUN_TEST(test_netlist_reader_is_rewrite);
    return UNITY_END();
}
