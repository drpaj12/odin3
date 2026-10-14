/*
 * test_wreduce.c — wreduce (3G spec WR-1..WR-12, plan Tasks 1 and 2): the pre-step, the operator
 * rules of Yosys 0.55's run_cell (spec 4.3) with PHASE2 #25's both-signed rule, the mux and FF
 * rules, the worker's rounds, determinism and out of memory. Hand-built IR (proc_build.h's RTLIL
 * subset), FULL check after each run; the expectations are derived by hand from wreduce.cc and
 * checked against Yosys itself by the per-rule goldens (tests/opt/rules/wreduce, `opt-rules`).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_MAX_LEN = 256, LINES_MAX = 64, OOM_LIMIT = 20000, SPEC_MAX = 128 };

typedef struct log_line {
    odin3_log_level level;
    char text[LINE_MAX_LEN];
} log_line;

static log_line lines[LINES_MAX];
static uint32_t n_lines;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LINES_MAX) {
        size_t len = strlen(msg);
        len = len < LINE_MAX_LEN ? len : LINE_MAX_LEN - 1;
        memcpy(lines[n_lines].text, msg, len);
        lines[n_lines].text[len] = '\0';
        lines[n_lines].level = level;
        n_lines++;
    }
}

static uint32_t count_lines(odin3_log_level level, const char *text) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level && strcmp(lines[i].text, text) == 0 ? 1U : 0U;
    }
    return hits;
}

static uint32_t count_prefix(odin3_log_level level, const char *prefix) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level && strncmp(lines[i].text, prefix, strlen(prefix)) == 0 ? 1U
                                                                                               : 0U;
    }
    return hits;
}

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_module *module;
} fixture;

static fixture fx;

void setUp(void) {
    memset(&fx, 0, sizeof fx);
    n_lines = 0;
    odin3_log_set_sink(sink, NULL);
    odin3_log_set_level(ODIN3_LOG_DEBUG);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_opt_test_set_limits(NULL);
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
    odin3_log_set_level(ODIN3_LOG_INFO);
    odin3_log_set_sink(NULL, NULL);
}

static void reset(void) {
    tearDown();
    setUp();
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    odin3_status st = odin3_check_design(fx.res.design, opts);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        if (lines[i].level == ODIN3_LOG_ERROR) {
            printf("check: %s\n", lines[i].text);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

static void build(const char *text) {
    odin3_status st = pb_build(text, &fx.res);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        printf("build: %s\n", lines[i].text);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fx.res.design, intern("wreduce"), &fx.ctx));
    check_full();
}

static void run_wreduce(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_wreduce_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

static char *rtlil_text(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &buf));
    return buf.data;
}

/* --- looking at the result -------------------------------------------------------------------- */

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

static bool node_gone(const char *name) {
    odin3_node_id node = node_named(name);
    return !odin3_node_valid(node) || !odin3_node_live(fx.module, node);
}

static odin3_net_id wbit(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, bit);
}

static odin3_net_id pnet(odin3_node_id node, uint32_t port, uint32_t bit) {
    odin3_pinslice ps = odin3_node_port(fx.module, node, port);
    TEST_ASSERT_TRUE(bit < ps.count);
    return odin3_pin_net(fx.module, (odin3_pin_id){ps.first.v + bit});
}

static uint32_t pwidth(odin3_node_id node, uint32_t port) {
    return odin3_node_port(fx.module, node, port).count;
}

static int64_t pint(odin3_node_id node, uint32_t index) {
    return odin3_node_param(fx.module, node, index)->i;
}

/* One operand: "0", "1", "x", "name" (bit 0 of a wire) or "name:k" (its bit k). */
static bool is_net(odin3_net_id net, const char *what) {
    odin3_const cv = odin3_net_const_value(fx.module, net);
    if (strcmp(what, "x") == 0) {
        return cv == ODIN3_CONST_X;
    }
    if (strcmp(what, "0") == 0) {
        return cv == ODIN3_CONST_0;
    }
    if (strcmp(what, "1") == 0) {
        return cv == ODIN3_CONST_1;
    }
    char name[SPEC_MAX];
    (void)snprintf(name, sizeof name, "%s", what);
    char *colon = strchr(name, ':');
    uint32_t bit = 0;
    if (colon != NULL) {
        *colon = '\0';
        bit = (uint32_t)strtoul(colon + 1, NULL, 10);
    }
    return cv == ODIN3_CONST_NONE && net.v == wbit(name, bit).v;
}

/* The nets of port `port` of node are the space-separated operands of spec, LSB first. */
static void expect_port(odin3_node_id node, uint32_t port, const char *spec) {
    char text[SPEC_MAX];
    (void)snprintf(text, sizeof text, "%s", spec);
    uint32_t bit = 0;
    for (char *tok = strtok(text, " "); tok != NULL; tok = strtok(NULL, " ")) {
        TEST_ASSERT_TRUE_MESSAGE(bit < pwidth(node, port), spec);
        TEST_ASSERT_TRUE_MESSAGE(is_net(pnet(node, port, bit), tok), spec);
        bit++;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(bit, pwidth(node, port), spec);
}

/* wire `name`, bits [from, to) all on the constant `what`. */
static void expect_wire_const(const char *name, uint32_t from, uint32_t to, const char *what) {
    for (uint32_t k = from; k < to; k++) {
        TEST_ASSERT_TRUE_MESSAGE(is_net(wbit(name, k), what), name);
    }
}

/* Binary word cells: ports A B Y; parameters A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH. */
enum {
    OP_A = 0,
    OP_B = 1,
    OP_Y = 2,
    UN_A = 0,
    UN_Y = 1,
    PRM_A_SIGNED = 0,
    PRM_B_SIGNED = 1,
    PRM_A_WIDTH = 2,
    PRM_B_WIDTH = 3,
    PRM_Y_WIDTH = 4,
    PRM_UN_A_SIGNED = 0,
    PRM_UN_A_WIDTH = 1,
    PRM_UN_Y_WIDTH = 2
};

static void expect_bin(const char *name, int64_t sa, int64_t sb, int64_t aw, int64_t bw,
                       int64_t yw) {
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node) && odin3_node_live(fx.module, node), name);
    TEST_ASSERT_EQUAL_INT64(sa, pint(node, PRM_A_SIGNED));
    TEST_ASSERT_EQUAL_INT64(sb, pint(node, PRM_B_SIGNED));
    TEST_ASSERT_EQUAL_INT64(aw, pint(node, PRM_A_WIDTH));
    TEST_ASSERT_EQUAL_INT64(bw, pint(node, PRM_B_WIDTH));
    TEST_ASSERT_EQUAL_INT64(yw, pint(node, PRM_Y_WIDTH));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)aw, pwidth(node, OP_A));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)bw, pwidth(node, OP_B));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)yw, pwidth(node, OP_Y));
}

/* --- texts ------------------------------------------------------------------------------------ */

#define BINW(type, name, sa, sb, aw, bw, yw, a, b, y)                                              \
    "  cell " type " \\" name "\n    parameter \\A_SIGNED " sa "\n    parameter \\B_SIGNED " sb    \
    "\n    parameter \\A_WIDTH " aw "\n    parameter \\B_WIDTH " bw                                \
    "\n    parameter \\Y_WIDTH " yw "\n    connect \\A " a "\n    connect \\B " b                  \
    "\n    connect \\Y " y "\n  end\n"

#define UNW(type, name, sa, aw, yw, a, y)                                                          \
    "  cell " type " \\" name "\n    parameter \\A_SIGNED " sa "\n    parameter \\A_WIDTH " aw     \
    "\n    parameter \\Y_WIDTH " yw "\n    connect \\A " a "\n    connect \\Y " y "\n  end\n"

#define HEAD8                                                                                      \
    "module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"                         \
    "  wire width 8 output 3 \\y\n"

/* --- the pre-step ----------------------------------------------------------------------------- */

static void test_one_bit_result(void) {
    build(HEAD8 BINW("$eq", "eq", "0", "0", "8", "8", "8", "\\a", "\\b", "\\y") "end\n");
    run_wreduce();
    odin3_node_id eq = node_named("eq");
    TEST_ASSERT_EQUAL_INT64(1, pint(eq, PRM_Y_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(1, pwidth(eq, OP_Y));
    expect_wire_const("y", 1, 8, "0");
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.narrowed);
}

static void test_one_bit_all_types(void) {
    const char *types[] = {"$lt", "$le", "$ne", "$ge", "$gt"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        char text[1024];
        (void)snprintf(
            text, sizeof text,
            "%s  cell %s \\c\n    parameter \\A_SIGNED 0\n"
            "    parameter \\B_SIGNED 0\n    parameter \\A_WIDTH 8\n"
            "    parameter \\B_WIDTH 8\n    parameter \\Y_WIDTH 4\n"
            "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y [3:0]\n  end\n"
            "  connect \\y [7:4] 4'0000\nend\n",
            HEAD8, types[i]);
        reset();
        build(text);
        run_wreduce();
        TEST_ASSERT_EQUAL_INT64(1, pint(node_named("c"), PRM_Y_WIDTH));
    }
}

static void test_div_unsigned_trim(void) {
    build(HEAD8 BINW("$div", "d", "0", "0", "8", "8", "8", "{ 3'000 \\a [4:0] }", "\\b",
                     "\\y") "end\n");
    run_wreduce();
    expect_bin("d", 0, 0, 5, 8, 8);
}

static void test_div_signed_keeps_one_zero(void) {
    build(HEAD8 BINW("$div", "d", "1", "1", "8", "8", "8", "{ 3'000 \\a [4:0] }", "\\b",
                     "\\y") "end\n");
    run_wreduce();
    expect_bin("d", 1, 1, 6, 8, 8);
    expect_port(node_named("d"), OP_A, "a a:1 a:2 a:3 a:4 0");
}

static void test_pow_reads_each_flag(void) {
    build(HEAD8 BINW("$pow", "p", "0", "1", "8", "6", "8", "{ 3'000 \\a [4:0] }",
                     "{ 2'00 \\b [3:0] }", "\\y") "end\n");
    run_wreduce();
    /* A unsigned: three zeros go; B signed: one zero stays to carry the sign */
    expect_bin("p", 0, 1, 5, 5, 8);
}

#define MEM_TEXT(abits_text, size_text, addr_width)                                                \
    "module \\m\n  wire width " addr_width " input 1 \\addr\n  wire width 8 output 2 \\q\n"        \
    "  cell $mem \\mem\n    parameter \\MEMID \"\\\\mm\"\n    parameter \\SIZE " size_text         \
    "\n    parameter \\OFFSET 0\n    parameter \\ABITS " abits_text                                \
    "\n    parameter \\WIDTH 8\n    parameter \\RD_PORTS 1\n    parameter \\WR_PORTS 0\n"          \
    "    parameter \\RD_CLK_ENABLE 1'0\n    parameter \\RD_CLK_POLARITY 1'0\n"                     \
    "    parameter \\RD_TRANSPARENT 1'0\n    parameter \\WR_CLK_ENABLE 0'\n"                       \
    "    parameter \\WR_CLK_POLARITY 0'\n    parameter \\INIT 0'\n    connect \\RD_CLK 1'x\n"      \
    "    connect \\RD_EN 1'1\n    connect \\RD_ADDR \\addr\n    connect \\RD_DATA \\q\n"           \
    "    connect \\WR_CLK { }\n    connect \\WR_EN { }\n    connect \\WR_ADDR { }\n"               \
    "    connect \\WR_DATA { }\n  end\nend\n"

enum { MEM_ABITS_PARAM = 3, MEM_RD_ADDR = 2 };

static void test_mem_address_bits(void) {
    build(MEM_TEXT("32", "10", "32"));
    run_wreduce();
    odin3_node_id mem = node_named("mem");
    TEST_ASSERT_EQUAL_INT64(4, pint(mem, MEM_ABITS_PARAM));
    TEST_ASSERT_EQUAL_UINT32(4, pwidth(mem, MEM_RD_ADDR));
    TEST_ASSERT_TRUE(is_net(pnet(mem, MEM_RD_ADDR, 3), "addr:3"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG,
                       "wreduce: module 'm': removed top 28 address bits (of 32) from memory "
                       "'mem'"));
    reset();
    build(MEM_TEXT("32", "10", "32"));
    fx.opts.memx = true;
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(32, pint(node_named("mem"), MEM_ABITS_PARAM));
}

static void test_mem_one_word_abits_zero(void) {
    build(MEM_TEXT("3", "1", "3"));
    run_wreduce();
    odin3_node_id mem = node_named("mem");
    TEST_ASSERT_EQUAL_INT64(0, pint(mem, MEM_ABITS_PARAM));
    TEST_ASSERT_EQUAL_UINT32(0, pwidth(mem, MEM_RD_ADDR));
}

/* --- the operator rules ----------------------------------------------------------------------- */

static void test_add_zero_extended_narrows(void) {
    build("module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"
          "  wire width 32 output 3 \\w\n" BINW(
              "$add", "add", "0", "0", "32", "32", "32", "{ 24'000000000000000000000000 \\a }",
              "{ 24'000000000000000000000000 \\b }", "\\w") "end\n");
    run_wreduce();
    expect_bin("add", 0, 0, 8, 8, 9);
    expect_wire_const("w", 9, 32, "0");
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.narrowed);
}

#define SX_HEAD                                                                                    \
    "module \\m\n  wire width 9 input 1 \\x\n  wire width 9 input 2 \\v\n"                         \
    "  wire width 11 output 3 \\s\n"

static void test_signed_rewrite_sx_add(void) {
    build(SX_HEAD BINW("$add", "add", "0", "0", "11", "11", "11", "{ \\x [8] \\x [8] \\x }",
                       "{ \\v [8] \\v [8] \\v }", "\\s") "end\n");
    run_wreduce();
    /* rule 2 makes it signed, rule 3 drops the repeated sign bits, rule 6 caps Y at 10 and pads
     * Y[10] with the new top Y bit */
    expect_bin("add", 1, 1, 9, 9, 10);
    odin3_node_id add = node_named("add");
    TEST_ASSERT_EQUAL_UINT32(wbit("s", 9).v, wbit("s", 10).v);
    TEST_ASSERT_EQUAL_UINT32(pnet(add, OP_Y, 9).v, wbit("s", 10).v);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.to_signed);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG,
                       "wreduce: module 'm': converting $add 'add' from unsigned to signed"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO,
                       "wreduce: 1 cells narrowed, 5 bits removed, 0 cells removed, 1 cells made "
                       "signed, 0 cells made unsigned"));
}

static void test_mul_signed_choice(void) {
    build(HEAD8 BINW("$mul", "mul", "0", "0", "8", "8", "8",
                     "{ \\a [3] \\a [3] \\a [3] \\a [3] "
                     "\\a [3:0] }",
                     "{ \\b [3] \\b [3] \\b [3] \\b [3] \\b [3:0] }", "\\y") "end\n");
    run_wreduce();
    expect_bin("mul", 1, 1, 4, 4, 8);
}

static void test_mul_stays_unsigned_when_cheaper(void) {
    build(HEAD8 BINW("$mul", "mul", "0", "0", "8", "8", "8", "{ 4'0000 \\a [3:0] }", "\\b",
                     "\\y") "end\n");
    run_wreduce();
    expect_bin("mul", 0, 0, 4, 8, 8);
}

static void test_sub_pad_is_top_bit_even_unsigned(void) {
    build(HEAD8 BINW("$sub", "sub", "0", "0", "8", "8", "8", "{ 4'0000 \\a [3:0] }",
                     "{ 4'0000 \\b [3:0] }", "\\y") "end\n");
    run_wreduce();
    expect_bin("sub", 0, 0, 4, 4, 5);
    for (uint32_t k = 5; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wbit("y", 4).v, wbit("y", k).v);
    }
}

static void test_both_signed_with_zero_tops_turns_unsigned(void) {
    build(HEAD8 BINW("$add", "add", "1", "1", "8", "8", "8", "{ 1'0 \\a [6:0] }",
                     "{ 1'0 \\b [6:0] }", "\\y") "end\n");
    run_wreduce();
    /* unsigned now, so the rerun trims the zero tops as an unsigned port: 7, 7 */
    expect_bin("add", 0, 0, 7, 7, 8);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.to_unsigned);
}

static void test_unary_signed_with_zero_top_turns_unsigned(void) {
    build(HEAD8 UNW("$neg", "neg", "1", "8", "8", "{ 1'0 \\a [6:0] }", "\\y") "end\n");
    run_wreduce();
    odin3_node_id neg = node_named("neg");
    TEST_ASSERT_EQUAL_INT64(0, pint(neg, PRM_UN_A_SIGNED));
    TEST_ASSERT_EQUAL_INT64(7, pint(neg, PRM_UN_A_WIDTH));
}

#define UNUSED_HEAD                                                                                \
    "module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"                         \
    "  wire width 4 output 3 \\y\n  wire width 8 \\t\n"

static void test_unused_top_y_bits_go(void) {
    build(UNUSED_HEAD BINW("$and", "and", "0", "0", "8", "8", "8", "\\a", "\\b",
                           "\\t") "  connect \\y \\t [3:0]\nend\n");
    run_wreduce();
    expect_bin("and", 0, 0, 4, 4, 4);
}

static void test_keep_wire_bits_stay(void) {
    build("module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"
          "  wire width 4 output 3 \\y\n  attribute \\keep 1\n  wire width 8 \\t\n" BINW(
              "$and", "and", "0", "0", "8", "8", "8", "\\a", "\\b",
              "\\t") "  connect \\y \\t [3:0]\nend\n");
    run_wreduce();
    expect_bin("and", 0, 0, 8, 8, 8);
}

static void test_output_port_bits_stay(void) {
    build(HEAD8 BINW("$and", "and", "0", "0", "8", "8", "8", "\\a", "\\b", "\\y") "end\n");
    run_wreduce();
    expect_bin("and", 0, 0, 8, 8, 8);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.wreduce.narrowed);
}

static void test_signed_shr_keeps_y(void) {
    build(UNUSED_HEAD BINW("$shr", "shr", "1", "0", "8", "8", "8", "\\a", "\\b",
                           "\\t") "  connect \\y \\t [3:0]\nend\n");
    run_wreduce();
    expect_bin("shr", 1, 0, 8, 8, 8);
    reset();
    build(UNUSED_HEAD BINW("$shr", "shr", "0", "0", "8", "8", "8", "\\a", "\\b",
                           "\\t") "  connect \\y \\t [3:0]\nend\n");
    run_wreduce();
    expect_bin("shr", 0, 0, 8, 8, 4);
}

static void test_shl_b_is_always_unsigned(void) {
    build(HEAD8 BINW("$shl", "shl", "0", "1", "8", "6", "8", "\\a",
                     "{ \\b [2] \\b [2] \\b [2] \\b [2:0] }", "\\y") "end\n");
    run_wreduce();
    expect_bin("shl", 0, 1, 8, 6, 8); /* signed trimming would have cut B to 4 bits */
}

static void test_shiftx_a_untouched(void) {
    build(HEAD8 BINW("$shiftx", "sx", "0", "0", "8", "8", "8", "{ 4'0000 \\a [3:0] }",
                     "{ 5'00000 \\b [2:0] }", "\\y") "end\n");
    run_wreduce();
    expect_bin("sx", 0, 0, 8, 3, 8);
}

/* --- mixed-sign cells (WR-6) ------------------------------------------------------------------ */

#define MIXED_WARN                                                                                 \
    "opt: module 'm': cell '$add' 'add' has A_SIGNED != B_SIGNED: treated as unsigned "            \
    "(both-signed rule, Yosys simlib; PHASE2 #25)"

#define HEAD6                                                                                      \
    "module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"                         \
    "  wire width 6 output 3 \\y\n"

static void test_mixed_sign_reads_both_signed_flag(void) {
    /* A looks sign-extended; Yosys would trim it as signed. A cell with A_SIGNED != B_SIGNED is
     * unsigned, so nothing is trimmed and the cell is not made signed (no cost gain). */
    build(HEAD6 BINW("$add", "add", "1", "0", "6", "6", "6", "{ \\a [3] \\a [3] \\a [3:0] }",
                     "\\b [5:0]", "\\y") "end\n");
    run_wreduce();
    expect_bin("add", 1, 0, 6, 6, 6);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_WARN, MIXED_WARN));
}

static void test_mixed_sign_free_choice_makes_both_signed(void) {
    build(HEAD6 BINW("$add", "add", "1", "0", "6", "6", "6", "{ \\a [3] \\a [3] \\a [3:0] }",
                     "{ \\b [3] \\b [3] \\b [3:0] }", "\\y") "end\n");
    run_wreduce();
    expect_bin("add", 1, 1, 4, 4, 5);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_WARN, MIXED_WARN));
}

/* --- skips ------------------------------------------------------------------------------------ */

static void test_module_with_process_is_skipped(void) {
    build("module \\m\n  wire width 8 input 1 \\a\n  wire width 8 input 2 \\b\n"
          "  wire width 8 output 3 \\y\n  wire output 4 \\z\n" BINW(
              "$eq", "eq", "0", "0", "8", "8", "8", "\\a", "\\b",
              "\\y") "  process $p\n    sync always\n      update \\z \\a [0]\n  end\nend\n");
    char *before = rtlil_text();
    run_wreduce();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "wreduce: module 'm' has processes; skipped"));
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
}

static void test_unknown_pass_args(void) {
    odin3_opt_args args;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_opt_parse_args("wreduce", odin3_bytes_cstr("-memx -keepdc"), &args));
    TEST_ASSERT_TRUE(args.opts.memx && args.opts.keepdc && !args.opts.mux_undef);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_parse_args("wreduce", odin3_bytes_cstr("-bogus"), &args));
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_ERROR, "wreduce: unknown argument '-bogus'"));
}

/* --- out of memory ---------------------------------------------------------------------------- */

static uint32_t count_half_done(void) {
    return count_prefix(ODIN3_LOG_ERROR, "opt: out of memory in the middle of a rewrite");
}

static void oom_sweep(const char *text, bool strict) {
    bool done = false;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        reset();
        build(text);
        char *before = rtlil_text();
        n_lines = 0;
        odin3_util_set_alloc_fail_after(fail);
        odin3_status st = odin3_opt_wreduce_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
        odin3_util_set_alloc_fail_after(-1);
        done = st == ODIN3_OK;
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        check_full();
        if (strict && st == ODIN3_ERR_NO_MEMORY && count_half_done() == 0) {
            char *after = rtlil_text();
            TEST_ASSERT_EQUAL_STRING(before, after);
            free(after);
        }
        free(before);
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "the sweep never reached a run that needs no failure");
}

static void test_narrowing_oom(void) {
    oom_sweep(SX_HEAD BINW("$add", "add", "0", "0", "11", "11", "11", "{ \\x [8] \\x [8] \\x }",
                           "{ \\v [8] \\v [8] \\v }", "\\s") "end\n",
              true);
}

static void test_prestep_oom(void) {
    oom_sweep(
        HEAD8 "  wire width 8 output 4 \\z\n" BINW("$eq", "eq", "0", "0", "8", "8", "8", "\\a",
                                                   "\\b", "\\y")
            BINW("$div", "d", "0", "0", "8", "8", "8", "{ 3'000 \\a [4:0] }", "\\b", "\\z") "end\n",
        false);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_one_bit_result);
    RUN_TEST(test_one_bit_all_types);
    RUN_TEST(test_div_unsigned_trim);
    RUN_TEST(test_div_signed_keeps_one_zero);
    RUN_TEST(test_pow_reads_each_flag);
    RUN_TEST(test_mem_address_bits);
    RUN_TEST(test_mem_one_word_abits_zero);
    RUN_TEST(test_add_zero_extended_narrows);
    RUN_TEST(test_signed_rewrite_sx_add);
    RUN_TEST(test_mul_signed_choice);
    RUN_TEST(test_mul_stays_unsigned_when_cheaper);
    RUN_TEST(test_sub_pad_is_top_bit_even_unsigned);
    RUN_TEST(test_both_signed_with_zero_tops_turns_unsigned);
    RUN_TEST(test_unary_signed_with_zero_top_turns_unsigned);
    RUN_TEST(test_unused_top_y_bits_go);
    RUN_TEST(test_keep_wire_bits_stay);
    RUN_TEST(test_output_port_bits_stay);
    RUN_TEST(test_signed_shr_keeps_y);
    RUN_TEST(test_shl_b_is_always_unsigned);
    RUN_TEST(test_shiftx_a_untouched);
    RUN_TEST(test_mixed_sign_reads_both_signed_flag);
    RUN_TEST(test_mixed_sign_free_choice_makes_both_signed);
    RUN_TEST(test_module_with_process_is_skipped);
    RUN_TEST(test_unknown_pass_args);
    RUN_TEST(test_narrowing_oom);
    RUN_TEST(test_prestep_oom);
    (void)node_gone;
    return UNITY_END();
}
