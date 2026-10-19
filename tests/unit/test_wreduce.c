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
#include "ir/prov.h"
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
    odin3_opt_test_set_wreduce_rounds(0);
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

typedef struct pin_ref {
    odin3_node_id node;
    uint32_t port, bit;
} pin_ref;

static odin3_net_id pin_net_at(pin_ref ref) {
    odin3_pinslice ps = odin3_node_port(fx.module, ref.node, ref.port);
    TEST_ASSERT_TRUE(ref.bit < ps.count);
    return odin3_pin_net(fx.module, (odin3_pin_id){ps.first.v + ref.bit});
}

#define pnet(node, port, bit) pin_net_at((pin_ref){(node), (port), (bit)})

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
typedef struct bit_range {
    uint32_t from, to;
} bit_range;

static void wire_const_range(const char *name, bit_range range, const char *what) {
    for (uint32_t bit = range.from; bit < range.to; bit++) {
        TEST_ASSERT_TRUE_MESSAGE(is_net(wbit(name, bit), what), name);
    }
}

#define expect_wire_const(name, from, to, what)                                                    \
    wire_const_range((name), (bit_range){(from), (to)}, (what))

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

typedef struct bin_want {
    int64_t sa, sb, aw, bw, yw;
} bin_want;

static void check_bin(const char *name, bin_want want) {
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node) && odin3_node_live(fx.module, node), name);
    const int64_t have[5] = {pint(node, PRM_A_SIGNED), pint(node, PRM_B_SIGNED),
                             pint(node, PRM_A_WIDTH), pint(node, PRM_B_WIDTH),
                             pint(node, PRM_Y_WIDTH)};
    const int64_t wanted[5] = {want.sa, want.sb, want.aw, want.bw, want.yw};
    for (size_t k = 0; k < 5; k++) {
        TEST_ASSERT_EQUAL_INT64_MESSAGE(wanted[k], have[k], name);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)want.aw, pwidth(node, OP_A));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)want.bw, pwidth(node, OP_B));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)want.yw, pwidth(node, OP_Y));
}

#define expect_bin(name, sa, sb, aw, bw, yw)                                                       \
    check_bin((name), (bin_want){(sa), (sb), (aw), (bw), (yw)})

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

/* A rebuilt cell keeps its name and attributes, and its provenance record derives from the old
 * cell's (Peter's reverse-engineering use: the result still points at its source). */
static void test_rebuilt_cell_keeps_name_attrs_and_prov(void) {
    build(SX_HEAD "  attribute \\note 7\n  attribute \\src \"t.v:3.1\"\n" BINW(
        "$add", "add", "0", "0", "11", "11", "11", "{ \\x [8] \\x [8] \\x }",
        "{ \\v [8] \\v [8] \\v }", "\\s") "end\n");
    const odin3_prov_id old_prov = odin3_node_prov(fx.module, node_named("add"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, old_prov.v);
    run_wreduce();
    expect_bin("add", 1, 1, 9, 9, 10);
    odin3_node_id add = node_named("add");
    const odin3_value *note =
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, add.v}, intern("note"));
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_INT64(7, note->i);
    const odin3_prov_record *rec = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, add));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    bool from_old = false;
    for (uint32_t i = 0; i < rec->parents.count; i++) {
        from_old = from_old || rec->parents.ids[i].v == old_prov.v;
    }
    TEST_ASSERT_TRUE(from_old);
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

/* The text a run without a failure ends with. */
static char *final_text(const char *text) {
    reset();
    build(text);
    run_wreduce();
    char *out = rtlil_text();
    reset();
    return out;
}

static void expect_as_before_or_final(const char *before, const char *final) {
    char *after = rtlil_text();
    TEST_ASSERT_TRUE(strcmp(before, after) == 0 || strcmp(final, after) == 0);
    free(after);
}

/* Every allocation point in turn fails once. A failure leaves a valid IR; with one rewrite
 * (strict) the design is as it was, or the rewrite is complete (the failure came after it), or the
 * kit logged its half-done line. */
/* One run of the sweep with the allocation `fail` failing; true when the run needed no failure. */
static bool oom_run_once(const char *text, long fail, bool strict, const char *final) {
    reset();
    build(text);
    char *before = rtlil_text();
    n_lines = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_wreduce_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (strict && st == ODIN3_ERR_NO_MEMORY && count_half_done() == 0) {
        expect_as_before_or_final(before, final);
    }
    free(before);
    return st == ODIN3_OK;
}

static void oom_sweep(const char *text, bool strict) {
    bool done = false;
    char *final = final_text(text);
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        done = oom_run_once(text, fail, strict, final);
    }
    free(final);
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

/* --- $mux and $pmux (spec 4.1) -----------------------------------------------------------------
 */

#define MUXHEAD                                                                                    \
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n  wire input 3 \\c\n"     \
    "  wire input 4 \\s\n  wire width 4 output 5 \\y\n"

#define MUX4(name, a, b, s, y)                                                                     \
    "  cell $mux \\" name "\n    parameter \\WIDTH 4\n    connect \\A " a "\n    connect \\B " b   \
    "\n    connect \\S " s "\n    connect \\Y " y "\n  end\n"

enum { MUX_A = 0, MUX_B = 1, MUX_S = 2, MUX_Y = 3, PRM_MUX_WIDTH = 0 };

static void test_mux_agreeing_top_bit(void) {
    build(MUXHEAD MUX4("m1", "{ \\c \\a [2:0] }", "{ \\c \\b [2:0] }", "\\s", "\\y") "end\n");
    run_wreduce();
    odin3_node_id mux = node_named("m1");
    TEST_ASSERT_EQUAL_INT64(3, pint(mux, PRM_MUX_WIDTH));
    expect_port(mux, MUX_A, "a a:1 a:2");
    expect_port(mux, MUX_B, "b b:1 b:2");
    TEST_ASSERT_EQUAL_UINT32(wbit("c", 0).v, wbit("y", 3).v);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG,
                       "wreduce: module 'm': removed top 1 bits (of 4) from mux cell $mux 'm1'"));
}

static void test_mux_unused_top_bits_become_x(void) {
    build("module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n  wire input 3 \\s\n"
          "  wire width 2 output 4 \\y\n  wire width 4 \\t\n" MUX4(
              "m1", "\\a", "\\b", "\\s", "\\t") "  connect \\y \\t [1:0]\nend\n");
    run_wreduce();
    odin3_node_id mux = node_named("m1");
    TEST_ASSERT_EQUAL_INT64(2, pint(mux, PRM_MUX_WIDTH));
    expect_wire_const("t", 2, 4, "x");
}

static void test_mux_whole_cell_goes(void) {
    build(MUXHEAD MUX4("m1", "\\a", "\\a", "\\s", "\\y") "end\n");
    run_wreduce();
    TEST_ASSERT_TRUE(node_gone("m1"));
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wbit("a", k).v, wbit("y", k).v);
    }
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.removed);
}

#define MUX_UNDEF_TEXT                                                                             \
    MUXHEAD MUX4("m1", "{ 1'x \\a [2:0] }", "{ \\c \\b [2:0] }", "\\s", "\\y") "end\n"

static void test_mux_undef_and_keepdc(void) {
    build(MUX_UNDEF_TEXT);
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(4, pint(node_named("m1"), PRM_MUX_WIDTH)); /* no -mux_undef */
    reset();
    build(MUX_UNDEF_TEXT);
    fx.opts.mux_undef = true;
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(3, pint(node_named("m1"), PRM_MUX_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(wbit("c", 0).v, wbit("y", 3).v);
    reset();
    build(MUX_UNDEF_TEXT);
    fx.opts.mux_undef = true;
    fx.opts.keepdc = true;
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(4, pint(node_named("m1"), PRM_MUX_WIDTH));
}

static void test_pmux_narrows(void) {
    build("module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n  wire width 4 "
          "input 3 "
          "\\c\n  wire width 2 input 4 \\s\n  wire width 4 output 5 \\y\n  wire input 6 \\k\n"
          "  cell $pmux \\p\n    parameter \\WIDTH 4\n    parameter \\S_WIDTH 2\n"
          "    connect \\A { \\k \\a [2:0] }\n    connect \\B { \\k \\b [2:0] \\k \\c [2:0] }\n"
          "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    run_wreduce();
    odin3_node_id pm = node_named("p");
    TEST_ASSERT_EQUAL_INT64(3, pint(pm, PRM_MUX_WIDTH));
    expect_port(pm, MUX_B, "c c:1 c:2 b b:1 b:2");
    TEST_ASSERT_EQUAL_UINT32(wbit("k", 0).v, wbit("y", 3).v);
}

/* --- flip-flops and latches (spec 4.2) ------------------------------------------------------ */

#define FFHEAD(init)                                                                               \
    "module \\m\n  wire input 1 \\clk\n  wire width 8 input 2 \\d\n  wire input 4 \\r\n" init      \
    "  wire width 8 output 3 \\q\n"

#define DFF(name, d)                                                                               \
    "  cell $dff \\" name "\n    parameter \\WIDTH 8\n    parameter \\CLK_POLARITY 1\n"            \
    "    connect \\CLK \\clk\n    connect \\D " d "\n    connect \\Q \\q\n  end\n"

enum { FF_WIDTH = 0 };

static void test_dff_zero_extended(void) {
    build(FFHEAD("") DFF("f", "{ 4'0000 \\d [3:0] }") "end\n");
    run_wreduce();
    odin3_node_id ff = node_named("f");
    TEST_ASSERT_EQUAL_INT64(4, pint(ff, FF_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(4, pwidth(ff, 1));
    expect_wire_const("q", 4, 8, "0");
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG,
                       "wreduce: module 'm': removed top 4 bits (of 8) from FF cell $dff 'f'"));
}

static void test_dff_zero_extended_keepdc_and_init(void) {
    build(FFHEAD("") DFF("f", "{ 4'0000 \\d [3:0] }") "end\n");
    fx.opts.keepdc = true; /* init is x: -keepdc keeps the bits */
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(8, pint(node_named("f"), FF_WIDTH));
    reset();
    build(FFHEAD("  attribute \\init 8'00000000\n") DFF("f", "{ 4'0000 \\d [3:0] }") "end\n");
    fx.opts.keepdc = true; /* init 0 lets a 0 through */
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(4, pint(node_named("f"), FF_WIDTH));
    reset();
    build(FFHEAD("  attribute \\init 8'10000000\n") DFF("f", "{ 4'0000 \\d [3:0] }") "end\n");
    run_wreduce(); /* init 1 on the top bit: it cannot be a 0 */
    TEST_ASSERT_EQUAL_INT64(8, pint(node_named("f"), FF_WIDTH));
}

/* Nothing to narrow: a plain register and a wide add leave the IR exactly as it was (no stray
 * constant cell), and the pass says so. */
static void test_nothing_to_narrow_changes_nothing(void) {
    build(FFHEAD("") DFF("f", "\\d") "end\n");
    const uint32_t nodes = odin3_module_node_end(fx.module);
    const uint32_t nets = odin3_module_net_end(fx.module);
    char *before = rtlil_text();
    run_wreduce();
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(fx.module));
    TEST_ASSERT_EQUAL_UINT32(nets, odin3_module_net_end(fx.module));
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.wreduce.narrowed);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO,
                       "wreduce: 0 cells narrowed, 0 bits removed, 0 cells removed, 0 cells made "
                       "signed, 0 cells made unsigned"));
}

static void test_dff_sign_extended(void) {
    build(FFHEAD("") DFF("f", "{ \\d [3] \\d [3] \\d [3] \\d [3] \\d [3:0] }") "end\n");
    run_wreduce();
    odin3_node_id ff = node_named("f");
    TEST_ASSERT_EQUAL_INT64(4, pint(ff, FF_WIDTH));
    for (uint32_t k = 4; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wbit("q", 3).v, wbit("q", k).v);
    }
}

static void test_dff_sign_extension_needs_equal_init(void) {
    build(FFHEAD("  attribute \\init 8'01000000\n")
              DFF("f", "{ \\d [3] \\d [3] \\d [3] \\d [3] \\d [3:0] }") "end\n");
    run_wreduce(); /* init[7] != init[6]: the top bit stays */
    TEST_ASSERT_EQUAL_INT64(8, pint(node_named("f"), FF_WIDTH));
}

#define SDFF_TEXT(value)                                                                           \
    FFHEAD("")                                                                                     \
    "  cell $sdff \\f\n    parameter \\WIDTH 8\n    parameter \\CLK_POLARITY 1\n"                  \
    "    parameter \\SRST_POLARITY 1\n    parameter \\SRST_VALUE 8'" value                         \
    "\n    connect \\CLK \\clk\n    connect \\SRST \\r\n"                                          \
    "    connect \\D { 4'0000 \\d [3:0] }\n    connect \\Q \\q\n  end\nend\n"

static void test_sdff_reset_value_truncated(void) {
    build(SDFF_TEXT("00001010"));
    run_wreduce();
    odin3_node_id ff = node_named("f");
    TEST_ASSERT_EQUAL_INT64(4, pint(ff, FF_WIDTH));
    const odin3_value *rst = odin3_node_param(fx.module, ff, 3);
    TEST_ASSERT_EQUAL_UINT32(4, rst->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, rst->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, rst->bits[1]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, rst->bits[2]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, rst->bits[3]);
    reset();
    build(SDFF_TEXT("10000000")); /* a 1 reset on the top bit stops the zero extension */
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(8, pint(node_named("f"), FF_WIDTH));
}

static void test_adlatch_and_dlatch(void) {
    build(FFHEAD("") "  cell $dlatch \\l\n    parameter \\WIDTH 8\n    parameter \\EN_POLARITY 1\n"
                     "    connect \\EN \\clk\n    connect \\D { 4'0000 \\d [3:0] }\n"
                     "    connect \\Q \\q\n  end\nend\n");
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(4, pint(node_named("l"), FF_WIDTH));
    reset();
    build(FFHEAD("") "  cell $adlatch \\l\n    parameter \\WIDTH 8\n    parameter \\EN_POLARITY 1\n"
                     "    parameter \\ARST_POLARITY 1\n    parameter \\ARST_VALUE 8'00000000\n"
                     "    connect \\EN \\clk\n    connect \\ARST \\r\n"
                     "    connect \\D { 4'0000 \\d [3:0] }\n    connect \\Q \\q\n  end\nend\n");
    run_wreduce();
    TEST_ASSERT_EQUAL_INT64(4, pint(node_named("l"), FF_WIDTH));
}

#define UNUSED_Q_HEAD(init)                                                                        \
    "module \\m\n  wire input 1 \\clk\n  wire width 4 input 2 \\d\n  wire width 2 output 3 "       \
    "\\y\n" init "  wire width 4 \\t\n"

#define DFF4(d, q)                                                                                 \
    "  cell $dff \\f\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"                   \
    "    connect \\CLK \\clk\n    connect \\D " d "\n    connect \\Q " q "\n  end\n"

static void test_dff_unused_q_bits_and_init(void) {
    build(UNUSED_Q_HEAD("") "  attribute \\init 4'0101\n  wire width 4 \\u\n" DFF4(
        "\\d", "\\u") "  connect \\y \\u [1:0]\nend\n");
    run_wreduce();
    odin3_node_id ff = node_named("f");
    TEST_ASSERT_EQUAL_INT64(2, pint(ff, FF_WIDTH));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("u", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wbit("u", 2)));
}

static void test_dff_all_unused_goes(void) {
    build(UNUSED_Q_HEAD("") DFF4("\\d", "\\t") "  connect \\y 2'00\nend\n");
    run_wreduce();
    TEST_ASSERT_TRUE(node_gone("f"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.wreduce.removed);
}

/* --- the worker: rounds, order, determinism ----------------------------------------------------
 */

/* add2 reads the register that add1 feeds, through named wires (`t`, `u`): a named net is a
 * statement boundary and is not followed by the order walk (WR-7), so the nodes are visited in ID
 * order and each round only sees what the earlier rounds exposed: add1 narrows in round 1, the
 * register in round 2, add2 in round 3. */
#define CASCADE                                                                                    \
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"                         \
    "  wire width 4 input 3 \\c\n  wire input 4 \\clk\n  wire width 8 output 5 \\y\n"              \
    "  wire width 8 \\t\n  wire width 8 \\u\n" BINW(                                               \
        "$add", "add2", "0", "0", "8", "8", "8", "\\u", "{ 4'0000 \\c }",                          \
        "\\y") "  cell $dff \\ff\n    parameter \\WIDTH 8\n    parameter \\CLK_POLARITY 1\n"       \
               "    connect \\CLK \\clk\n    connect \\D \\t\n    connect \\Q \\u\n  end\n" BINW(  \
                   "$add", "add1", "0", "0", "8", "8", "8", "{ 4'0000 \\a }", "{ 4'0000 \\b }",    \
                   "\\t") "end\n"

static void expect_cascade_done(void) {
    expect_bin("add1", 0, 0, 4, 4, 5);
    TEST_ASSERT_EQUAL_INT64(5, pint(node_named("ff"), FF_WIDTH));
    expect_bin("add2", 0, 0, 5, 4, 6);
}

static void test_cascade_needs_rounds(void) {
    build(CASCADE);
    run_wreduce();
    expect_cascade_done();
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_WARN, "wreduce:"));
}

/* --- the order walk (WR-7, wreduce_order.c) -----------------------------------------------------
 */

typedef struct rank_set {
    uint32_t *r;
    uint32_t n;
} rank_set;

static rank_set probe_ranks(void) {
    rank_set set;
    set.n = odin3_module_node_end(fx.module);
    set.r = (uint32_t *)calloc(set.n, sizeof *set.r);
    TEST_ASSERT_NOT_NULL(set.r);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_wreduce_probe_ranks(&fx.ctx, fx.res.design, fx.module,
                                                                  &fx.opts, set.r, set.n));
    return set;
}

/* x is ranked before y. */
static void expect_before(const rank_set *set, const char *x, const char *y);

static uint32_t rank_named(const rank_set *set, const char *name) {
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node) && node.v < set->n, name);
    return set->r[node.v];
}

static void expect_before(const rank_set *set, const char *x, const char *y) {
    const uint32_t rx = rank_named(set, x);
    const uint32_t ry = rank_named(set, y);
    TEST_ASSERT_TRUE_MESSAGE(rx != 0 && ry != 0 && rx < ry, x);
}

enum { BUS_TEXT = 640, TEXT_MAX = 8192, DEEP_CELLS = 100000, BIG_TEXT_PER_CELL = 160 };

/* "{ $n<base+w-1> ... $n<base> }": a bus of anonymous single-bit nets, MSB first. */
static void bus(char *out, size_t cap, uint32_t base, uint32_t width) {
    size_t at = (size_t)snprintf(out, cap, "{");
    for (uint32_t k = width; k > 0; k--) {
        at += (size_t)snprintf(out + at, cap - at, " $tmp%u", base + k - 1U);
    }
    snprintf(out + at, cap - at, " }");
}

/* `(c == s1) ^ (c == s1+5-2)` as Odin III numbers it: each consumer before its operand, every
 * operand net anonymous (an expression temporary). */
static void operand_first_text(char *out, size_t cap) {
    char s1[BUS_TEXT], a[BUS_TEXT], s2[BUS_TEXT];
    bus(s1, sizeof s1, 0, 32);
    bus(a, sizeof a, 32, 32);
    bus(s2, sizeof s2, 64, 32);
    size_t at =
        (size_t)snprintf(out, cap,
                         "module \\m\n  wire width 2 input 1 \\m\n  wire width 5 input 2 \\c\n"
                         "  wire output 3 \\o\n");
    for (uint32_t k = 0; k < 96; k++) {
        at += (size_t)snprintf(out + at, cap - at, "  wire $tmp%u\n", k);
    }
    const char *zero27 = "27'000000000000000000000000000";
    const char *zero30 = "30'000000000000000000000000000000";
    at += (size_t)snprintf(
        out + at, cap - at,
        "  cell $eq \\eq\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 32\n    parameter \\B_WIDTH 32\n    parameter \\Y_WIDTH 1\n"
        "    connect \\A { %s \\c }\n    connect \\B %s\n    connect \\Y \\o\n  end\n"
        "  cell $sub \\sub2\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 32\n    parameter \\B_WIDTH 32\n    parameter \\Y_WIDTH 32\n"
        "    connect \\A %s\n    connect \\B 32'00000000000000000000000000000010\n"
        "    connect \\Y %s\n  end\n"
        "  cell $add \\add\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 32\n    parameter \\B_WIDTH 32\n    parameter \\Y_WIDTH 32\n"
        "    connect \\A %s\n    connect \\B 32'00000000000000000000000000000101\n"
        "    connect \\Y %s\n  end\n"
        "  cell $sub \\sub1\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 32\n    parameter \\B_WIDTH 32\n    parameter \\Y_WIDTH 32\n"
        "    connect \\A { %s \\m }\n    connect \\B 32'00000000000000000000000000000001\n"
        "    connect \\Y %s\n  end\nend\n",
        zero27, s2, a, s2, s1, a, zero30, s1);
}

/* WR-7 (amended): `(c == m-1) ^ (c == m-1+5-2)`, cells in Odin III's elaboration order (a consumer
 * before its operand). A round still visits the operand first, so the $sub narrows to A2:B1:Y3, the
 * $add becomes signed A3:B4:Y5 and the next $sub signed A5:B3:Y6, the widths Yosys's own front end
 * (and Parmys) reach. In node-ID order the $add stayed A32:B3 and cost 33 adder stages. */
static void test_operand_visited_before_consumer(void) {
    char text[TEXT_MAX];
    operand_first_text(text, sizeof text);
    build(text);
    run_wreduce();
    expect_bin("sub1", 0, 0, 2, 1, 3);
    expect_bin("add", 1, 1, 3, 4, 5);
    expect_bin("sub2", 1, 1, 5, 3, 6);
}

/* The same cells, but every net named (a statement boundary): visited in node-ID order, so the
 * order walk changes nothing (the forward-reference case of WR-7). */
static void test_named_nets_keep_id_order(void) {
    char text[TEXT_MAX];
    operand_first_text(text, sizeof text);
    char *named = strdup(text);
    TEST_ASSERT_NOT_NULL(named);
    /* every anonymous temporary gets a wire: declare s1, a, s2 as 32-bit wires aliasing the nets */
    char extra[3 * BUS_TEXT + 256];
    char b0[BUS_TEXT], b1[BUS_TEXT], b2[BUS_TEXT];
    bus(b0, sizeof b0, 0, 32);
    bus(b1, sizeof b1, 32, 32);
    bus(b2, sizeof b2, 64, 32);
    snprintf(extra, sizeof extra,
             "  wire width 32 \\s1\n  wire width 32 \\a\n  wire width 32 \\s2\n"
             "  connect \\s1 %s\n  connect \\a %s\n  connect \\s2 %s\n",
             b0, b1, b2);
    char full[TEXT_MAX + sizeof extra];
    const char *cut = strstr(named, "  cell $eq");
    TEST_ASSERT_NOT_NULL(cut);
    snprintf(full, sizeof full, "%.*s%s%s", (int)(cut - named), named, extra, cut);
    free(named);
    build(full);
    run_wreduce();
    expect_bin("add", 0, 0, 32, 3,
               32); /* not narrowed: consumer before operand, as in node-ID order */
}

#define NOT1(name, a, y) UNW("$not", name, "0", "1", "1", a, y)
#define NETS4 "  wire $tmp1\n  wire $tmp2\n  wire $tmp3\n  wire $tmp4\n"
#define OHEAD "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\clk\n" NETS4

static void test_rank_operand_before_consumer(void) {
    build(OHEAD NOT1("cons", "$tmp1", "$tmp2") NOT1("op", "\\a", "$tmp1") "end\n");
    rank_set set = probe_ranks();
    expect_before(&set, "op", "cons");
    free(set.r);
}

static void test_rank_named_net_is_not_followed(void) {
    build("module \\m\n  wire input 1 \\a\n  wire $tmp2\n  wire \\w\n" NOT1("cons", "\\w", "$tmp2")
              NOT1("op", "\\a", "\\w") "end\n");
    rank_set set = probe_ranks();
    expect_before(&set, "cons", "op");
    free(set.r);
}

static void test_rank_combinational_loop_terminates(void) {
    build(OHEAD NOT1("c1", "$tmp2", "$tmp1") NOT1("c2", "$tmp1", "$tmp2") "end\n");
    rank_set set = probe_ranks();
    const uint32_t r1 = rank_named(&set, "c1");
    const uint32_t r2 = rank_named(&set, "c2");
    TEST_ASSERT_TRUE(r1 != 0 && r2 != 0 && r1 != r2);
    free(set.r);
}

static void test_rank_loop_through_flip_flop(void) {
    build(OHEAD NOT1(
        "c1", "$tmp3",
        "$tmp4") "  cell $dff \\ff\n    parameter \\WIDTH 1\n"
                 "    parameter \\CLK_POLARITY 1\n    connect \\CLK \\clk\n    connect \\D $tmp4\n"
                 "    connect \\Q $tmp3\n  end\nend\n");
    rank_set set = probe_ranks();
    /* the flip-flop's output is not followed: c1 is first, the flip-flop (its D driver) after */
    expect_before(&set, "c1", "ff");
    free(set.r);
}

static void test_rank_first_driver_of_a_shared_net(void) {
    const char *tri = "    parameter \\WIDTH 1\n    connect \\A \\a\n    connect \\EN \\b\n";
    char text[TEXT_MAX];
    snprintf(text, sizeof text,
             "%s%s  cell $tribuf \\t1\n%s    connect \\Y $tmp1\n  end\n"
             "  cell $tribuf \\t2\n%s    connect \\Y $tmp1\n  end\nend\n",
             OHEAD, NOT1("cons", "$tmp1", "$tmp2"), tri, tri);
    build(text);
    rank_set set = probe_ranks();
    /* t1 drives the net first: it goes before the reader; t2 is entered later, after it */
    expect_before(&set, "t1", "cons");
    expect_before(&set, "cons", "t2");
    free(set.r);
}

/* Bit 0 first: a 2-bit port fed by {n2 n1} walks n1's driver before n2's, whatever their IDs. */
static void test_rank_bus_is_walked_lsb_first(void) {
    build(OHEAD "  wire width 2 output 4 \\y\n" BINW("$add", "cons", "0", "0", "2", "2", "2",
                                                     "{ $tmp2 $tmp1 }", "{ $tmp4 $tmp3 }", "\\y")
              NOT1("d2", "\\a", "$tmp2") NOT1("d1", "\\a", "$tmp1") "end\n");
    rank_set set = probe_ranks();
    expect_before(&set, "d1", "d2");
    expect_before(&set, "d2", "cons");
    free(set.r);
}

/* Port A before port B (Yosys's binop: left, then right). */
static void test_rank_port_a_before_port_b(void) {
    build(OHEAD BINW("$and", "cons", "0", "0", "1", "1", "1", "$tmp2", "$tmp1", "$tmp3")
              NOT1("db", "\\a", "$tmp1") NOT1("da", "\\a", "$tmp2") "end\n");
    rank_set set = probe_ranks();
    expect_before(&set, "da", "db");
    expect_before(&set, "db", "cons");
    free(set.r);
}

/* A ternary's operands: the condition first, then the true value (B), then the false value (A). */
static void test_rank_mux_walks_s_then_b_then_a(void) {
    build(OHEAD
          "  wire $tmp5\n  wire $tmp6\n  cell $mux \\mx\n    parameter \\WIDTH 1\n"
          "    connect \\A $tmp1\n    connect \\B $tmp2\n    connect \\S $tmp3\n    connect \\Y "
          "$tmp4\n  end\n" NOT1("da", "\\a", "$tmp1") NOT1("db", "\\a", "$tmp2")
              NOT1("ds", "\\a", "$tmp3") "end\n");
    rank_set set = probe_ranks();
    expect_before(&set, "ds", "db");
    expect_before(&set, "db", "da");
    expect_before(&set, "da", "mx");
    free(set.r);
}

/* 100,000 cells, each before the one it feeds: the walk is as deep as the design. */
static void test_rank_deep_chain_uses_no_recursion(void) {
    const size_t cap = (size_t)DEEP_CELLS * BIG_TEXT_PER_CELL * 2U;
    char *text = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(text);
    size_t at = (size_t)snprintf(text, cap, "module \\m\n  wire input 1 \\a\n");
    for (uint32_t k = 0; k <= DEEP_CELLS; k++) {
        at += (size_t)snprintf(text + at, cap - at, "  wire $tmp%u\n", k);
    }
    for (uint32_t k = DEEP_CELLS; k > 0; k--) { /* cell k reads net k-1: consumer first */
        char in[32];
        snprintf(in, sizeof in, k == 1 ? "\\a" : "$tmp%u", k - 1U);
        at += (size_t)snprintf(text + at, cap - at,
                               "  cell $not \\c%u\n    parameter \\A_SIGNED 0\n"
                               "    parameter \\A_WIDTH 1\n    parameter \\Y_WIDTH 1\n"
                               "    connect \\A %s\n    connect \\Y $tmp%u\n  end\n",
                               k, in, k);
    }
    snprintf(text + at, cap - at, "end\n");
    build(text);
    free(text);
    rank_set set = probe_ranks();
    char last[16];
    snprintf(last, sizeof last, "c%u", (unsigned)DEEP_CELLS);
    expect_before(&set, "c1", "c2");
    TEST_ASSERT_EQUAL_UINT32(rank_named(&set, "c1") + DEEP_CELLS - 1U, rank_named(&set, last));
    free(set.r);
}

/* Every allocation of the ranking fails once: NO_MEMORY, never a wrong rank or a leak. */
static void test_rank_oom(void) {
    char text[TEXT_MAX];
    operand_first_text(text, sizeof text);
    bool done = false;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        reset();
        build(text);
        uint32_t ranks[16] = {0};
        odin3_util_set_alloc_fail_after(fail);
        odin3_status st =
            odin3_opt_wreduce_probe_ranks(&fx.ctx, fx.res.design, fx.module, &fx.opts, ranks, 16);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        done = st == ODIN3_OK;
    }
    TEST_ASSERT_TRUE(done);
}

/* The whole pass with each allocation failing once (the walk's included): a valid IR. */
static void test_order_oom(void) {
    char text[TEXT_MAX];
    operand_first_text(text, sizeof text);
    oom_sweep(text, false);
}

static void test_round_cap_warns_and_stays_valid(void) {
    odin3_opt_test_set_wreduce_rounds(2);
    build(CASCADE);
    run_wreduce();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN,
                       "wreduce: module 'm': no fixed point after 2 rounds; stopping"));
    expect_bin("add1", 0, 0, 4, 4, 5);
    TEST_ASSERT_EQUAL_INT64(5, pint(node_named("ff"), FF_WIDTH));
    TEST_ASSERT_EQUAL_INT64(8, pint(node_named("add2"), PRM_A_WIDTH)); /* round 3 never ran */
}

static void test_two_runs_are_identical(void) {
    build(CASCADE);
    run_wreduce();
    char *first = rtlil_text();
    reset();
    build(CASCADE);
    run_wreduce();
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

/* A mux, an FF and an operator chain together: two runs give the same text. */
#define MIXED                                                                                      \
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"                         \
    "  wire input 3 \\c\n  wire input 4 \\s\n  wire input 5 \\clk\n"                               \
    "  wire width 4 output 6 \\y\n  wire width 8 output 7 \\q\n  wire width 8 \\t\n"               \
    "  wire width 8 \\u\n" MUX4("m1", "{ \\c \\a [2:0] }", "{ \\c \\b [2:0] }", "\\s", "\\y")      \
        BINW("$add", "ad", "0", "0", "8", "8", "8", "{ 4'0000 \\a }", "{ 4'0000 \\b }", "\\t")     \
            BINW("$add", "ad2", "0", "0", "8", "8", "8", "\\t", "{ 4'0000 \\b }", "\\u")           \
                DFF("f", "\\u") "end\n"

static void test_two_runs_are_identical_mixed(void) {
    build(MIXED);
    run_wreduce();
    TEST_ASSERT_TRUE(fx.counts.wreduce.narrowed > 0);
    char *first = rtlil_text();
    reset();
    build(MIXED);
    run_wreduce();
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

/* A cell's line without its name, to compare the cells of two orders as a multiset. */
static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

static char *dup_str(const char *text) {
    char *copy = malloc(strlen(text) + 1U);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, strlen(text) + 1U);
    return copy;
}

static char *cell_summary(void) {
    char buf[1024] = "";
    char *lines_out[16];
    size_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module) && count < 16; id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        if (!odin3_node_live(fx.module, node) || def->n_params == 0 ||
            strncmp(def->name, "$port", 5) == 0) {
            continue;
        }
        char one[128];
        int len = snprintf(one, sizeof one, "%s", def->name);
        for (uint32_t k = 0; k < def->n_params && k < 5; k++) {
            len += snprintf(one + len, sizeof one - (size_t)len, " %lld",
                            (long long)odin3_node_param(fx.module, node, k)->i);
        }
        lines_out[count] = dup_str(one);
        count++;
    }
    qsort((void *)lines_out, count, sizeof lines_out[0], cmp_str);
    for (size_t i = 0; i < count; i++) {
        (void)snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s;", lines_out[i]);
        free(lines_out[i]);
    }
    return dup_str(buf);
}

static void test_reversed_creation_gives_the_same_cells(void) {
    build(CASCADE);
    run_wreduce();
    char *first = cell_summary();
    reset();
    build("module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
          "  wire width 4 input 3 \\c\n  wire input 4 \\clk\n  wire width 8 output 5 \\y\n"
          "  wire width 8 \\t\n  wire width 8 \\u\n" BINW(
              "$add", "add1", "0", "0", "8", "8", "8", "{ 4'0000 \\a }", "{ 4'0000 \\b }",
              "\\t") "  cell $dff \\ff\n    parameter \\WIDTH 8\n    parameter \\CLK_POLARITY 1\n"
                     "    connect \\CLK \\clk\n    connect \\D \\t\n    connect \\Q \\u\n  "
                     "end\n" BINW("$add", "add2", "0", "0", "8", "8", "8", "\\u", "{ 4'0000 \\c }",
                                  "\\y") "end\n");
    run_wreduce();
    char *second = cell_summary();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

/* wreduce then opt: the incremental opt_expr and a full rescan agree. */
static void test_wreduce_then_opt_rescan_agree(void) {
    char *texts[2];
    for (int rescan = 0; rescan < 2; rescan++) {
        reset();
        build(CASCADE);
        run_wreduce();
        odin3_opt_args args;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_opt_parse_args("opt", odin3_bytes_cstr("-full"), &args));
        args.full_rescan = rescan == 1;
        odin3_opt_counts counts;
        memset(&counts, 0, sizeof counts);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_run(&fx.ctx, fx.res.design, &args, &counts));
        check_full();
        texts[rescan] = rtlil_text();
    }
    TEST_ASSERT_EQUAL_STRING(texts[0], texts[1]);
    free(texts[0]);
    free(texts[1]);
}

static void test_ff_and_mux_oom(void) {
    oom_sweep(FFHEAD("") DFF("f", "{ \\d [7] \\d [7] \\d [5:0] }") "end\n", true);
    oom_sweep(MUXHEAD MUX4("m1", "{ \\c \\a [2:0] }", "{ \\c \\b [2:0] }", "\\s", "\\y") "end\n",
              true);
    oom_sweep(CASCADE, false);
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
    RUN_TEST(test_rebuilt_cell_keeps_name_attrs_and_prov);
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
    RUN_TEST(test_mux_agreeing_top_bit);
    RUN_TEST(test_mux_unused_top_bits_become_x);
    RUN_TEST(test_mux_whole_cell_goes);
    RUN_TEST(test_mux_undef_and_keepdc);
    RUN_TEST(test_pmux_narrows);
    RUN_TEST(test_dff_zero_extended);
    RUN_TEST(test_dff_zero_extended_keepdc_and_init);
    RUN_TEST(test_nothing_to_narrow_changes_nothing);
    RUN_TEST(test_dff_sign_extended);
    RUN_TEST(test_dff_sign_extension_needs_equal_init);
    RUN_TEST(test_sdff_reset_value_truncated);
    RUN_TEST(test_adlatch_and_dlatch);
    RUN_TEST(test_dff_unused_q_bits_and_init);
    RUN_TEST(test_dff_all_unused_goes);
    RUN_TEST(test_cascade_needs_rounds);
    RUN_TEST(test_operand_visited_before_consumer);
    RUN_TEST(test_named_nets_keep_id_order);
    RUN_TEST(test_rank_operand_before_consumer);
    RUN_TEST(test_rank_named_net_is_not_followed);
    RUN_TEST(test_rank_combinational_loop_terminates);
    RUN_TEST(test_rank_loop_through_flip_flop);
    RUN_TEST(test_rank_first_driver_of_a_shared_net);
    RUN_TEST(test_rank_bus_is_walked_lsb_first);
    RUN_TEST(test_rank_port_a_before_port_b);
    RUN_TEST(test_rank_mux_walks_s_then_b_then_a);
    RUN_TEST(test_rank_deep_chain_uses_no_recursion);
    RUN_TEST(test_rank_oom);
    RUN_TEST(test_order_oom);
    RUN_TEST(test_round_cap_warns_and_stays_valid);
    RUN_TEST(test_two_runs_are_identical);
    RUN_TEST(test_two_runs_are_identical_mixed);
    RUN_TEST(test_reversed_creation_gives_the_same_cells);
    RUN_TEST(test_wreduce_then_opt_rescan_agree);
    RUN_TEST(test_ff_and_mux_oom);
    (void)node_gone;
    return UNITY_END();
}
