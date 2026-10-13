/*
 * test_elab_stage1_diag.c — 2D Task 16: the REJECT and IGNORE rows of stage 1 (EL-42, EL-43,
 * EL-44; §9.1, §9.2). One source per row, each asserting the exact located line §9's tables
 * name; a warning once per parsed node (a generate loop, a module specialized twice); the error
 * cap; resuming at the next item and module; OOM sweeps over the IGNORE and REJECT paths; the
 * stage-1 pending tables empty, with every
 * coverage row running stage 1 without the not-available line. Sources are parsed by 2C
 * (elab_test.h).
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_COVERAGE_DIR
#error "ODIN3_COVERAGE_DIR must name tests/verilog/coverage"
#endif

enum {
    ROW_SOURCE_MAX = 64,
    COVERAGE_MAX = 512,
    COVERAGE_MIN_ROWS = 100, /* a floor: fewer .v files means the directory was not found */
    COVERAGE_HEADERS = 8,
    COVERAGE_MODULES = 16,
    COVERAGE_PATH = 512,
    COVERAGE_FILE_MAX = 1U << 20,
    CAP_VARS = 25,
    CAP_ERRORS = 20,
    CAP_TEXT = 4096,
    OOM_LIMIT = 400000,
    OOM_SETTLE = 64,
    OOM_MIN_FAILS = 20
};

static et_result rs;

static void cov_free(void);
static void set_top(const char *top);
static bool log_has(const char *needle);

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    cov_free();
    set_top(NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

static void run_one(const char *text) {
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
}

/* --- REJECT rows ---------------------------------------------------------------------------- */

typedef struct row {
    const char *text;
    const char *line; /* the whole located line, without the logger's prefix */
    uint32_t warns;   /* IGNORE rows: the exact number of warnings the source logs */
} row;

/* The row's source fails stage 1 with exactly the line (and maybe other errors). */
static void expect_rejected(const row *rw) {
    run_one(rw->text);
    et_expect(&rs, ODIN3_LOG_ERROR, rw->line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, rw->line);
    TEST_ASSERT_NULL(rs.elab);
}

#define REJECT(text, line) expect_rejected(&(const row){(text), (line), 0})

static void test_reject_expression_and_declaration_rows(void) {
    REJECT("module m(output [7:0] y);\n  assign y = 1.5;\nendmodule\n",
           "t.v:2:14: error: real number in an expression is not supported");
    REJECT("module m;\n  real r;\nendmodule\n",
           "t.v:2:3: error: variable type 'real' is not supported");
    REJECT("module m;\n  time r;\nendmodule\n",
           "t.v:2:3: error: variable type 'time' is not supported");
    REJECT("module m;\n  realtime r;\nendmodule\n",
           "t.v:2:3: error: variable type 'realtime' is not supported");
    REJECT("module m;\n  event e;\nendmodule\n",
           "t.v:2:3: error: named event 'e' is not supported");
    REJECT("module m(output [31:0] y);\n  assign y = $random;\nendmodule\n",
           "t.v:2:14: error: system function '$random' is not supported");
    REJECT("module m(output y);\n  assign y = u.f(1);\nendmodule\n",
           "t.v:2:14: error: hierarchical reference 'u.f' is not supported");
    REJECT("module leaf (input a);\n  wire w = a;\nendmodule\nmodule m(input a, output y);\n"
           "  leaf u (a);\n  assign y = u.w;\nendmodule\n",
           "t.v:6:14: error: hierarchical reference 'u.w' is not supported");
    REJECT("module m;\n  tri0 t;\nendmodule\n", "t.v:2:3: error: net type 'tri0' is not supported");
    REJECT("module m;\n  tri1 t;\nendmodule\n", "t.v:2:3: error: net type 'tri1' is not supported");
    REJECT("module m;\n  trireg t;\nendmodule\n",
           "t.v:2:3: error: net type 'trireg' is not supported");
    REJECT("module m(input a, output y);\n  nmos n1 (y, a, a);\nendmodule\n",
           "t.v:2:3: error: switch-level primitive 'nmos' is not supported");
    REJECT("module m(input a, output y);\n  rtranif1 n1 (y, a, a);\nendmodule\n",
           "t.v:2:3: error: switch-level primitive 'rtranif1' is not supported");
    REJECT("module m(output y);\n  pullup (y);\nendmodule\n",
           "t.v:2:3: error: 'pullup' is not supported");
    REJECT("module m(output y);\n  pulldown (y);\nendmodule\n",
           "t.v:2:3: error: 'pulldown' is not supported");
}

static void test_reject_statement_rows(void) {
    REJECT("module m(output reg x);\n  always #5 x = ~x;\nendmodule\n",
           "t.v:2:3: error: always block without an event control is not supported");
    REJECT("module m(input clk, a, output reg x, z);\n  always @(posedge clk) begin\n"
           "    x = a;\n    @(negedge clk) z = a;\n  end\nendmodule\n",
           "t.v:4:5: error: event control inside an always body is not supported");
    REJECT("module m(input a, output reg x);\n  always @(a) wait (a) x = 1;\nendmodule\n",
           "t.v:2:15: error: 'wait' is not supported");
    REJECT("module m(input a);\n  event e;\n  always @(a) -> e;\nendmodule\n",
           "t.v:3:15: error: event trigger '->' is not supported");
    REJECT("module m(input a, output reg x);\n  always @(a) fork x = 1; join\nendmodule\n",
           "t.v:2:15: error: 'fork ... join' is not supported");
    REJECT(
        "module m(input a, output reg r);\n  initial begin\n    assign r = a;\n  end\nendmodule\n",
        "t.v:3:5: error: procedural 'assign' is not supported");
    REJECT("module m(input a, output reg r);\n  initial begin\n    deassign r;\n  end\nendmodule\n",
           "t.v:3:5: error: procedural 'deassign' is not supported");
    REJECT(
        "module m(input a, output reg r);\n  initial begin\n    force r = a;\n  end\nendmodule\n",
        "t.v:3:5: error: procedural 'force' is not supported");
    REJECT("module m(input a, output reg r);\n  initial begin\n    release r;\n  end\nendmodule\n",
           "t.v:3:5: error: procedural 'release' is not supported");
    REJECT("module m(output reg r);\n  initial begin : b\n    disable b;\n  end\nendmodule\n",
           "t.v:3:5: error: 'disable' is not supported");
    REJECT("module m(output reg x);\n  initial forever x = ~x;\nendmodule\n",
           "t.v:2:11: error: 'forever' is not supported");
    REJECT("module m(input clk, output reg x);\n  integer i;\n  always @(posedge clk) begin\n"
           "    i = 0;\n    while (i < 4) i = i + 1;\n    x = i;\n  end\nendmodule\n",
           "t.v:5:5: error: 'while' is supported only in constant functions");
    REJECT("module m(input a, b, output reg x);\n  always @(posedge a or b) x = 1;\nendmodule\n",
           "t.v:2:10: error: edge and level events in one event list are not supported");
    REJECT("module m;\n  reg [7:0] mem [0:3];\n  always @* $readmemh(\"f\", mem);\nendmodule\n",
           "t.v:3:13: error: $readmemh is supported only in an initial block");
}

static void test_reject_function_rows(void) {
    REJECT("module m(input a, output y);\n  function f;\n    input i;\n    begin\n      #1 f = i;\n"
           "    end\n  endfunction\n  assign y = f(a);\nendmodule\n",
           "t.v:5:7: error: timing control in function 'f' is not supported");
    REJECT("module m(input a, output y);\n  reg r;\n  function f;\n    input i;\n"
           "    begin r <= i; f = i; end\n  endfunction\n  assign y = f(a);\nendmodule\n",
           "t.v:5:11: error: nonblocking assignment in function 'f' is not supported");
}

/* --- IGNORE rows ---------------------------------------------------------------------------- */

/* The source passes stage 1 with the warning (and exactly rw->warns warnings in all: no spurious
 * second line), and its store checks and binds. */
static void expect_ignored(const row *rw) {
    run_one(rw->text);
    et_expect(&rs, ODIN3_LOG_WARN, rw->line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rw->warns, et_count_with(&rs, ODIN3_LOG_WARN, ""),
                                     et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    et_expect_binds(&rs);
}

#define IGNORED(text, line) expect_ignored(&(const row){(text), (line), 1})
#define IGNORED_OF(text, line, warns) expect_ignored(&(const row){(text), (line), (warns)})

static void test_ignore_directive_rows(void) {
    IGNORED("`timescale 1ns/1ps\nmodule m;\nendmodule\n",
            "t.v:1:1: warning: `timescale is ignored");
    IGNORED_OF("`celldefine\nmodule m;\nendmodule\n`endcelldefine\n",
               "t.v:1:1: warning: `celldefine is ignored", 2);
    IGNORED_OF("`celldefine\nmodule m;\nendmodule\n`endcelldefine\n",
               "t.v:4:1: warning: `endcelldefine is ignored", 2);
    IGNORED_OF("`unconnected_drive pull1\nmodule m;\nendmodule\n`nounconnected_drive\n",
               "t.v:1:1: warning: `unconnected_drive is ignored", 2);
    IGNORED_OF("`unconnected_drive pull1\nmodule m;\nendmodule\n`nounconnected_drive\n",
               "t.v:4:1: warning: `nounconnected_drive is ignored", 2);
    IGNORED("module m;\n`pragma protect\nendmodule\n", "t.v:2:1: warning: `pragma is ignored");
    IGNORED("module m;\n`timescale 1ns/1ps\nendmodule\n",
            "t.v:2:1: warning: `timescale is ignored");
}

static void test_ignore_declaration_rows(void) {
    IGNORED("module m(input a, output y);\n  specparam p = 1;\n  assign y = a;\nendmodule\n",
            "t.v:2:3: warning: specparam 'p' is ignored");
    IGNORED("module m(input a, output y);\n  wire (strong0, strong1) w = a;\n  assign y = w;\n"
            "endmodule\n",
            "t.v:2:8: warning: drive strength is ignored");
    IGNORED("module m(input a, output y);\n  wire #5 w;\n  assign w = a;\n  assign y = w;\n"
            "endmodule\n",
            "t.v:2:8: warning: net delay is ignored");
    IGNORED("module m(input a, output y);\n  wire vectored [3:0] w;\n  assign w = {4{a}};\n"
            "  assign y = w[0];\nendmodule\n",
            "t.v:2:3: warning: 'vectored' is ignored");
    IGNORED("module m(input a, output y);\n  wire scalared [3:0] w;\n  assign w = {4{a}};\n"
            "  assign y = w[0];\nendmodule\n",
            "t.v:2:3: warning: 'scalared' is ignored");
}

/* P7: one warning per specparam declarator, named, and none for the absent RANGE slot. */
static void test_specparam_one_warning_each(void) {
    IGNORED_OF(
        "module m(input a, output y);\n  specparam p = 1, q = 2;\n  assign y = a;\nendmodule\n",
        "t.v:2:3: warning: specparam 'p' is ignored", 2);
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:2:3: warning: specparam 'q' is ignored");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_count_with(&rs, ODIN3_LOG_WARN, "specparam ''"),
                                     et_log(&rs));
    IGNORED("module m(input a, output y);\n  specparam [3:0] r = 1;\n  assign y = a;\nendmodule\n",
            "t.v:2:3: warning: specparam 'r' is ignored");
}

static void test_charge_strength_row(void) {
    run_one("module m;\n  trireg (small) t;\nendmodule\n"); /* N15 rejects it, N19 notes it */
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:3: error: net type 'trireg' is not supported");
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:2:10: warning: charge strength is ignored");
}

static void test_ignore_assignment_and_gate_rows(void) {
    IGNORED("module m(input a, output y);\n  assign #1 y = a;\nendmodule\n",
            "t.v:2:10: warning: delay on a continuous assignment is ignored");
    IGNORED("module m(input a, output y);\n  assign (strong0, strong1) y = a;\nendmodule\n",
            "t.v:2:10: warning: drive strength on a continuous assignment is ignored");
    IGNORED(
        "module m(input clk, d, output reg q);\n  always @(posedge clk) q <= #1 d;\nendmodule\n",
        "t.v:2:30: warning: delay is ignored");
    IGNORED("module m(input a, output reg x);\n  initial #5 x = 1;\nendmodule\n",
            "t.v:2:11: warning: delay is ignored");
    IGNORED("module m(input y, output reg x);\n  initial x = #1 y;\nendmodule\n",
            "t.v:2:15: warning: delay is ignored");
    IGNORED("module m(input a, b, output y);\n  and #2 g1 (y, a, b);\nendmodule\n",
            "t.v:2:7: warning: gate delay is ignored");
    IGNORED("module m(input a, b, output y);\n  and (strong0, strong1) g1 (y, a, b);\nendmodule\n",
            "t.v:2:7: warning: gate drive strength is ignored");
}

static void test_ignore_block_and_task_rows(void) {
    IGNORED("module m(input a, output y);\n  assign y = a;\n  specify\n    (a => y) = 1;\n"
            "  endspecify\nendmodule\n",
            "t.v:3:3: warning: specify block is ignored");
    IGNORED("module m(input a, output y);\n  assign y = a;\n  initial $display(\"a = %b\", a);\n"
            "endmodule\n",
            "t.v:3:11: warning: system task '$display' is ignored");
}

/* An ignored system task takes its arguments with it (no $random error from inside one). */
static void test_ignored_task_arguments_are_not_scanned(void) {
    run_one("module m(input a, output y);\n  assign y = a;\n"
            "  initial $display(\"%d %f\", $random, 1.5);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rs.status);
}

static void expect_dump_lacks(const char *needle) {
    TEST_ASSERT_NOT_NULL(rs.dump.data);
    TEST_ASSERT_NULL_MESSAGE(strstr(rs.dump.data, needle), rs.dump.data);
}

/* The elaborated form keeps the statement a delay or a system task stood before. */
static void test_ignored_constructs_leave_the_statements(void) {
    run_one("module m(input a, output reg x, y);\n  initial begin\n    #3 x = a;\n"
            "    $display(\"x\");\n    #4;\n    y = #2 a;\n  end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    expect_dump_lacks("(DELAY");
    expect_dump_lacks("(TIMING_STMT");
    expect_dump_lacks("$display");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(NULL_STMT"), rs.dump.data);
}

/* --- once per node ---------------------------------------------------------------------------- */

static void test_warning_once_per_node(void) {
    run_one(
        "module m(input a, output [3:0] w);\n  genvar i;\n"
        "  for (i = 0; i < 4; i = i + 1) begin : g\n    assign #1 w[i] = a;\n  end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1, et_count_with(&rs, ODIN3_LOG_WARN, "delay on a continuous assignment is ignored"),
        et_log(&rs));
    run_one("module leaf #(parameter P = 1) (input a, output y);\n  assign #1 y = a;\nendmodule\n"
            "module top(input a, output y, z);\n  leaf #(1) u1 (a, y);\n  leaf #(2) u2 (a, z);\n"
            "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1, et_count_with(&rs, ODIN3_LOG_WARN, "delay on a continuous assignment is ignored"),
        et_log(&rs));
}

/* --- the cap, resuming --------------------------------------------------------------------------
 */

static void test_error_cap(void) {
    char text[CAP_TEXT] = "module m;\n";
    size_t len = strlen(text);
    for (int i = 0; i < CAP_VARS; i++) {
        int got = snprintf(text + len, sizeof text - len, "  real r%d;\n", i);
        TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof text - len);
        len += (size_t)got;
    }
    TEST_ASSERT_TRUE(len + sizeof "endmodule\n" < sizeof text);
    memcpy(text + len, "endmodule\n", sizeof "endmodule\n");
    run_one(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(CAP_ERRORS + 1, rs.errors,
                                     et_log(&rs)); /* and the stop line */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        CAP_ERRORS, et_count_with(&rs, ODIN3_LOG_ERROR, "variable type 'real' is not supported"),
        et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:22:3: error: too many errors; elaboration stopped after 20");
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR, "too many errors"));
}

static void test_resume_at_next_item_and_module(void) {
    run_one("module a;\n  real x;\n  time y;\nendmodule\nmodule b;\n  realtime z;\nendmodule\n"
            "module top; a ua (); b ub (); endmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:3: error: variable type 'real' is not supported");
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:3:3: error: variable type 'time' is not supported");
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:6:3: error: variable type 'realtime' is not supported");
}

/* A module whose scan found an error skips passes E and W (§9.3, §17 #36), so its children are
 * never reached: the info "not instantiated under the top" would be wrong, and is not logged once
 * an error has been (review minor 1). */
static void test_no_not_elaborated_after_an_error(void) {
    run_one("module child (input a, output y);\n  assign y = a;\nendmodule\n"
            "module top (input a, output y);\n  real r;\n  child u (.a(a), .y(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:5:3: error: variable type 'real' is not supported");
    TEST_ASSERT_FALSE_MESSAGE(log_has("not instantiated under the top"), et_log(&rs));
    set_top("top"); /* without an error the info is still logged */
    run_one("module child (input a, output y);\n  assign y = a;\nendmodule\n"
            "module top (input a, output y);\n  assign y = a;\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(log_has("not instantiated under the top, not elaborated: 'child'"),
                             et_log(&rs));
}

static void test_failed_parameter_adds_no_second_error(void) {
    run_one("module leaf #(parameter real R = 1) ();\nendmodule\n"
            "module top;\n  leaf u ();\n  leaf #(.R(2)) v ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:15: error: parameter type 'real' is not supported");
}

/* --- out of memory ---------------------------------------------------------------------------- */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

/* One step of a sweep: the allocation limit, the parsed store's node count, the status a run
 * that does not run out of memory returns. */
typedef struct oom_at {
    long limit;
    uint32_t nodes;
    odin3_status done;
} oom_at;

static odin3_status oom_step(const oom_at *at) {
    odin3_ast *elab = NULL;
    odin3_util_set_alloc_fail_after(at->limit);
    odin3_status st = odin3_elab_stage1(&rs.ctx, rs.parsed, &rs.opts, &elab);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == at->done || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_TRUE(st == ODIN3_OK ? elab != NULL : elab == NULL);
    TEST_ASSERT_EQUAL_UINT32(at->nodes, odin3_ast_node_end(rs.parsed).v);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(elab, ODIN3_CHECK_FULL));
    }
    odin3_ast_destroy(elab);
    return st;
}

/* Every allocation of stage 1 over the parsed source fails in turn: each run returns NO_MEMORY or
 * the status done, frees what it made (ASan's leak check at exit), and the parsed store is kept. */
static void oom_sweep(odin3_status done) {
    uint32_t nodes = odin3_ast_node_end(rs.parsed).v;
    odin3_log_set_sink(quiet_sink, NULL);
    int failed = 0;
    int settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        oom_at at = {limit, nodes, done};
        bool oom = oom_step(&at) == ODIN3_ERR_NO_MEMORY;
        failed += oom ? 1 : 0;
        settled = oom ? 0 : settled + 1;
    }
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(OOM_SETTLE, settled);
    TEST_ASSERT_TRUE(failed > OOM_MIN_FAILS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.parsed, ODIN3_CHECK_FULL));
}

/* The IGNORE paths (the warn-once map, its format list, the scan's frames, pass W's drops). */
static void test_oom_ignore_rows(void) {
    run_one("`timescale 1ns/1ps\nmodule m(input a, b, output y, z, output reg x);\n"
            "  specparam p = 1, q = 2;\n  wire #2 w;\n  assign #1 w = a;\n"
            "  assign (strong0, strong1) y = w;\n  and #3 g (z, a, b);\n"
            "  specify\n    (a => y) = 1;\n  endspecify\n"
            "  initial begin\n    #3 x = a;\n    $display(\"x = %b\", x);\n    x = #2 b;\n  end\n"
            "  always @(posedge a) x <= #1 b;\nendmodule\n`celldefine\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    oom_sweep(ODIN3_OK);
}

/* The REJECT paths: the scan's errors, a skipped definition, the next module's scan. */
static void test_oom_reject_rows(void) {
    run_one("module a(input c);\n  real r;\n  always @(posedge c or c) r = 1;\n"
            "  initial $display($random);\nendmodule\n"
            "module top(input c);\n  a u (c);\n  wire #1 w = c;\n  event e;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    oom_sweep(ODIN3_ERR_PARSE);
}

/* --- the tables, the coverage rows -------------------------------------------------------------
 */

static void test_stage1_tables_are_empty(void) {
    for (uint32_t kind = 0; kind < ODIN3_PEND_END; kind++) {
        char why[32];
        TEST_ASSERT_TRUE(snprintf(why, sizeof why, "kind %u", (unsigned)kind) > 0);
        TEST_ASSERT_FALSE_MESSAGE(odin3_elab_pending(ODIN3_ELAB_STAGE1, kind), why);
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_pending_form_count(ODIN3_ELAB_STAGE1));
}

/* What the coverage test owns, freed by tearDown so a failed assertion leaks nothing. */
static char *cov_names[COVERAGE_MAX];
static size_t cov_count;
static char *cov_text;
static char *cov_headers[COVERAGE_HEADERS];
static char cov_header_paths[COVERAGE_HEADERS][COVERAGE_PATH];
static size_t cov_header_count;

static void cov_free(void) {
    for (size_t i = 0; i < cov_count; i++) {
        free(cov_names[i]);
        cov_names[i] = NULL;
    }
    cov_count = 0;
    for (size_t i = 0; i < cov_header_count; i++) {
        free(cov_headers[i]);
        cov_headers[i] = NULL;
    }
    cov_header_count = 0;
    free(cov_text);
    cov_text = NULL;
}

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *buf = malloc(COVERAGE_FILE_MAX);
    if (buf == NULL) {
        (void)fclose(fp);
        TEST_FAIL_MESSAGE("out of memory");
    }
    size_t got = fread(buf, 1, COVERAGE_FILE_MAX - 1, fp);
    buf[got] = '\0';
    (void)fclose(fp);
    return buf;
}

/* The coverage rows' file names (the directory's .v files), in directory order. */
static void coverage_names(void) {
    DIR *dir = opendir(ODIN3_COVERAGE_DIR);
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        size_t len = strlen(ent->d_name);
        if (len > 2 && strcmp(ent->d_name + len - 2, ".v") == 0 && cov_count < COVERAGE_MAX) {
            cov_names[cov_count++] = strdup(ent->d_name);
        }
    }
    (void)closedir(dir);
}

/* One header of the coverage directory's inc/, read and named inc/<name> for the request. */
static void coverage_header(const char *file_name) {
    TEST_ASSERT_TRUE_MESSAGE(cov_header_count < COVERAGE_HEADERS,
                             "more headers than COVERAGE_HEADERS");
    char file[COVERAGE_PATH];
    TEST_ASSERT_TRUE(snprintf(file, sizeof file, ODIN3_COVERAGE_DIR "/inc/%s", file_name) > 0);
    TEST_ASSERT_TRUE(
        snprintf(cov_header_paths[cov_header_count], COVERAGE_PATH, "inc/%s", file_name) > 0);
    cov_headers[cov_header_count++] = slurp(file);
}

/* The headers a row's `include names, resolved: every file of the coverage directory's inc/ (the
 * rows' include path) is read once and written beside the source. */
static void coverage_headers(void) {
    DIR *dir = opendir(ODIN3_COVERAGE_DIR "/inc");
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        if (ent->d_name[0] != '.') {
            coverage_header(ent->d_name);
        }
    }
    (void)closedir(dir);
}

/* Some message holds needle. */
static bool log_has(const char *needle) {
    for (uint32_t i = 0; i < et_count(&rs); i++) {
        if (strstr(et_text(&rs, i), needle) != NULL) {
            return true;
        }
    }
    return false;
}

static void set_top(const char *top) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = top;
    odin3_pass_set_options(opts);
}

/* One run of a coverage row with the --top option top (NULL: automatic). Rejected input is fine; an
 * internal error, a 2D bug line, the not-available line, and (with a top given) ambiguous_top are
 * not. */
typedef struct cov_run {
    const char *name;
    const char *top;
} cov_run;

/* The request of a run: the row's source, and the headers when it includes any. */
static uint32_t coverage_request(const cov_run *run, et_src *srcs) {
    uint32_t count = 0;
    srcs[count++] = (et_src){run->name, cov_text};
    for (size_t i = 0; strstr(cov_text, "`include") != NULL && i < cov_header_count; i++) {
        srcs[count++] = (et_src){cov_header_paths[i], cov_headers[i]};
    }
    return count;
}

/* What is wrong with the run just made, or NULL: only OK and rejected input are fine. */
static const char *coverage_fault(const cov_run *run) {
    if (rs.status != ODIN3_OK && rs.status != ODIN3_ERR_PARSE) {
        return "status is neither OK nor PARSE";
    }
    if (log_has("(a 2D bug)")) {
        return "a 2D bug line";
    }
    if (log_has("elaboration is not available yet (2D): ")) {
        return "the not-available line";
    }
    return run->top != NULL && log_has("ambiguous_top") ? "ambiguous_top with a top given" : NULL;
}

static void run_coverage(const cov_run *run) {
    et_src srcs[1 + COVERAGE_HEADERS];
    et_req req = {srcs, coverage_request(run, srcs), NULL, NULL, NULL};
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
    set_top(run->top);
    et_stage1(&req, &rs);
    const char *fault = coverage_fault(run);
    if (fault != NULL) {
        char why[COVERAGE_PATH];
        TEST_ASSERT_TRUE(snprintf(why, sizeof why, "%s (top %s): %s", run->name,
                                  run->top != NULL ? run->top : "automatic", fault) > 0);
        TEST_FAIL_MESSAGE(why);
    }
    if (rs.status == ODIN3_OK) {
        et_expect_binds(&rs);
    }
}

/* The MODULE names of the last run's parsed store, copied (the store goes with the next run). */
static void copy_module_name(char (*names)[COVERAGE_PATH], size_t count, size_t cap,
                             const char *name) {
    TEST_ASSERT_TRUE_MESSAGE(count < cap, "a coverage row declares more modules than "
                                          "COVERAGE_MODULES: raise the cap (it was silent)");
    TEST_ASSERT_TRUE(snprintf(names[count], COVERAGE_PATH, "%s", name) > 0);
}

static size_t parsed_modules(char (*names)[COVERAGE_PATH], size_t cap) {
    size_t count = 0;
    odin3_ast_span units = odin3_ast_children(rs.parsed, odin3_ast_root(rs.parsed, 0));
    for (uint32_t i = 0; i < units.n; i++) {
        if (odin3_ast_kind_of(rs.parsed, units.ids[i]) != ODIN3_AST_MODULE) {
            continue;
        }
        copy_module_name(names, count, cap, odin3_ast_name_str(rs.parsed, units.ids[i]));
        count++;
    }
    return count;
}

/* A row runs once with the automatic top; one that declares several modules runs again with each
 * of them as the top, so none stops at ambiguous_top and passes E and W elaborate every module. */
static void run_coverage_row(const char *name) {
    char path[COVERAGE_PATH];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s", ODIN3_COVERAGE_DIR, name) > 0);
    free(cov_text);
    cov_text = slurp(path);
    run_coverage(&(cov_run){name, NULL});
    char modules[COVERAGE_MODULES][COVERAGE_PATH];
    size_t count = parsed_modules(modules, COVERAGE_MODULES);
    for (size_t i = 0; count > 1 && i < count; i++) {
        run_coverage(&(cov_run){name, modules[i]});
    }
}

/* Every coverage row's .v runs stage 1, each module of a multi-module row as the top. */
static void test_coverage_rows_run_stage_1(void) {
    coverage_names();
    coverage_headers();
    TEST_ASSERT_TRUE(cov_count >= COVERAGE_MIN_ROWS);
    for (size_t i = 0; i < cov_count; i++) {
        run_coverage_row(cov_names[i]);
    }
    set_top(NULL);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reject_expression_and_declaration_rows);
    RUN_TEST(test_reject_statement_rows);
    RUN_TEST(test_reject_function_rows);
    RUN_TEST(test_ignore_directive_rows);
    RUN_TEST(test_ignore_declaration_rows);
    RUN_TEST(test_specparam_one_warning_each);
    RUN_TEST(test_charge_strength_row);
    RUN_TEST(test_ignore_assignment_and_gate_rows);
    RUN_TEST(test_ignore_block_and_task_rows);
    RUN_TEST(test_ignored_task_arguments_are_not_scanned);
    RUN_TEST(test_ignored_constructs_leave_the_statements);
    RUN_TEST(test_warning_once_per_node);
    RUN_TEST(test_error_cap);
    RUN_TEST(test_resume_at_next_item_and_module);
    RUN_TEST(test_no_not_elaborated_after_an_error);
    RUN_TEST(test_failed_parameter_adds_no_second_error);
    RUN_TEST(test_oom_ignore_rows);
    RUN_TEST(test_oom_reject_rows);
    RUN_TEST(test_stage1_tables_are_empty);
    RUN_TEST(test_coverage_rows_run_stage_1);
    return UNITY_END();
}
