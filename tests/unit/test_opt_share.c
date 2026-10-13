/*
 * test_opt_share.c — opt_share (2F spec OP-11, §11, §20.5; plan Task 14): the groups of cells that
 * share an operand under one mux, the cases it refuses, the order of the shared operands, the
 * add/sub mix, the provenance of the new cells, Yosys 0.55's own cases, a simulation check that
 * every rewrite keeps the function, and out of memory.
 *
 * Yosys parity (0.55, the VTR build): each tests/unit/data/opt/share/X.il is run as
 * `yosys -p "read_rtlil X.il; opt_share; write_rtlil"` (recorded in each X.expect); X.expect keeps,
 * per cell of the result, its type and the parameters A_WIDTH, B_WIDTH, Y_WIDTH, WIDTH and S_WIDTH.
 * Cells are compared as a multiset of lines. Where Odin III is narrower than Yosys (spec 17 #44)
 * the file also holds the lines `odin3 cell ...` that Odin III must give instead.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "proc_build.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LINE_MAX_LEN = 256,
    LINES_MAX = 64,
    OOM_LIMIT = 20000,
    CASE_BUF = 8192,
    MAX_CELLS = 32,
    SPEC_MAX = 128,
    ITEM_LEN = 96,
    MAX_SIM_BITS = 16,
    MAX_INPUT_BITS = 32
};

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

static void build(const char *text) {
    odin3_status st = pb_build(text, &fx.res);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        printf("build: %s\n", lines[i].text);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
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

static void run_share(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_share_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

/* The INFO line of the last run and the counter agree on want. */
static void expect_merged(uint32_t want) {
    char text[LINE_MAX_LEN];
    int len = snprintf(text, sizeof text, "opt_share: merged %u cells", (unsigned)want);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, text));
    TEST_ASSERT_EQUAL_UINT32(want, fx.counts.share_merged);
}

/* --- looking at the result -------------------------------------------------------------------- */

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

static odin3_net_id wbit(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, bit);
}

static const char *type_name(odin3_node_id node) {
    return odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node))->name;
}

static uint32_t count_type(const char *type) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        hits += odin3_node_live(fx.module, node) && strcmp(type_name(node), type) == 0 ? 1U : 0U;
    }
    return hits;
}

static uint32_t count_live(void) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        hits += odin3_node_live(fx.module, (odin3_node_id){id}) ? 1U : 0U;
    }
    return hits;
}

/* The nth (from 0) live node of type in ID order. */
static odin3_node_id nth_of(const char *type, uint32_t nth) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node) && strcmp(type_name(node), type) == 0 && nth-- == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(type);
    return (odin3_node_id){0};
}

/* The unnamed mux the pass made: the $mux or $pmux that is not the one called `old`. */
static odin3_node_id new_mux(const char *type, const char *old) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node) && strcmp(type_name(node), type) == 0 &&
            node.v != node_named(old).v) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(type);
    return (odin3_node_id){0};
}

typedef struct pin_ref {
    odin3_node_id node;
    uint32_t port, bit;
} pin_ref;

#define PIN(node, port, bit) ((pin_ref){(node), (port), (bit)})

static odin3_net_id pnet(pin_ref ref) {
    odin3_pinslice ps = odin3_node_port(fx.module, ref.node, ref.port);
    TEST_ASSERT_TRUE(ref.bit < ps.count);
    return odin3_pin_net(fx.module, (odin3_pin_id){ps.first.v + ref.bit});
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
        TEST_ASSERT_TRUE_MESSAGE(is_net(pnet(PIN(node, port, bit)), tok), spec);
        bit++;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(bit, pwidth(node, port), spec);
}

/* Every bit of port pa of a is on the same net as that bit of port pb of b. */
static void expect_same_port(odin3_node_id a, uint32_t pa, odin3_node_id b, uint32_t pb) {
    TEST_ASSERT_EQUAL_UINT32(pwidth(a, pa), pwidth(b, pb));
    for (uint32_t bit = 0; bit < pwidth(a, pa); bit++) {
        TEST_ASSERT_EQUAL_UINT32(pnet(PIN(a, pa, bit)).v, pnet(PIN(b, pb, bit)).v);
    }
}

/* $mux and $pmux: A, B, S, Y; the binary word cells: A, B, Y; $neg: A, Y. */
enum { MUX_A = 0, MUX_B = 1, MUX_S = 2, MUX_Y = 3, OP_A = 0, OP_B = 1, OP_Y = 2, NEG_A = 0 };
/* The parameters: $mux WIDTH; $pmux WIDTH, S_WIDTH; binary cells A_SIGNED .. Y_WIDTH. */
enum { PRM_WIDTH = 0, PRM_S_WIDTH = 1, PRM_A_WIDTH = 2, PRM_B_WIDTH = 3, PRM_Y_WIDTH = 4 };

/* --- building texts --------------------------------------------------------------------------- */

#define HEAD4                                                                                      \
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"                         \
    "  wire width 4 input 3 \\c\n  wire width 4 input 4 \\d\n  wire input 5 \\s\n"                \
    "  wire input 6 \\s1\n  wire input 7 \\s2\n  wire width 4 output 8 \\y\n"                      \
    "  wire width 4 output 9 \\y2\n  wire width 4 \\t1\n  wire width 4 \\t2\n"                     \
    "  wire width 4 \\t3\n  wire width 4 \\t4\n"

/* A binary word cell with both signed flags sg and all three widths 4. */
#define BIN(type, name, sg, a, b, y) BINW(type, name, sg, sg, "4", "4", "4", a, b, y)

#define BINW(type, name, sa, sb, aw, bw, yw, a, b, y)                                              \
    "  cell " type " \\" name "\n    parameter \\A_SIGNED " sa "\n    parameter \\B_SIGNED " sb    \
    "\n    parameter \\A_WIDTH " aw "\n    parameter \\B_WIDTH " bw "\n    parameter \\Y_WIDTH " yw \
    "\n    connect \\A " a "\n    connect \\B " b "\n    connect \\Y " y "\n  end\n"

#define MUX4(name, a, b, s, y)                                                                     \
    "  cell $mux \\" name "\n    parameter \\WIDTH 4\n    connect \\A " a "\n    connect \\B " b    \
    "\n    connect \\S " s "\n    connect \\Y " y "\n  end\n"

#define PMUX4(name, n, a, b, s, y)                                                                 \
    "  cell $pmux \\" name "\n    parameter \\WIDTH 4\n    parameter \\S_WIDTH " n                  \
    "\n    connect \\A " a "\n    connect \\B " b "\n    connect \\S " s "\n    connect \\Y " y      \
    "\n  end\n"

#define SAME_TYPE                                                                                  \
    HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")                                           \
        BIN("$and", "and2", "0", "\\a", "\\c", "\\t2") MUX4("m", "\\t1", "\\t2", "\\s", "\\y")     \
            "end\n"

/* --- the plain rewrite ------------------------------------------------------------------------ */

static void test_share_same_type(void) {
    build(SAME_TYPE);
    run_share();
    expect_merged(2);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$mux"));
    odin3_node_id op = node_named("and1");
    TEST_ASSERT_TRUE(odin3_node_valid(op) && odin3_node_live(fx.module, op));
    TEST_ASSERT_EQUAL_STRING("$and", type_name(op));
    odin3_node_id inner = new_mux("$mux", "m");
    expect_port(op, OP_A, "a a:1 a:2 a:3");
    expect_same_port(op, OP_B, inner, MUX_Y);
    expect_port(inner, MUX_A, "b b:1 b:2 b:3");
    expect_port(inner, MUX_B, "c c:1 c:2 c:3");
    expect_port(inner, MUX_S, "s");
    odin3_node_id old = node_named("m");
    expect_same_port(old, MUX_A, op, OP_Y);
    expect_same_port(old, MUX_B, op, OP_Y);
    TEST_ASSERT_EQUAL_INT64(4, pint(inner, PRM_WIDTH));
}

/* The unmerged cell is gone from the module, the old mux is still there. */
static void test_share_old_mux_stays(void) {
    build(SAME_TYPE);
    odin3_node_id first = node_named("and1");
    odin3_node_id second = node_named("and2");
    run_share();
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, second));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, first)); /* rebuilt as a new node */
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node_named("m")));
}

/* A second call finds nothing: the old mux reads one net on both ports. */
static void test_share_second_call(void) {
    build(SAME_TYPE);
    run_share();
    memset(&fx.counts, 0, sizeof fx.counts);
    run_share();
    expect_merged(0);
}

static void test_share_add_sub(void) {
    build(HEAD4 BIN("$add", "add1", "0", "\\a", "\\b", "\\t1")
              BIN("$sub", "sub1", "0", "\\a", "\\c", "\\t2")
                  MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    expect_merged(2);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$add"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$sub"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$neg"));
    odin3_node_id op = nth_of("$add", 0);
    odin3_node_id neg = nth_of("$neg", 0);
    odin3_node_id inner = new_mux("$mux", "m");
    expect_port(op, OP_A, "a a:1 a:2 a:3");
    expect_same_port(op, OP_B, inner, MUX_Y);
    expect_port(inner, MUX_A, "b b:1 b:2 b:3");
    expect_same_port(inner, MUX_B, neg, 1);
    expect_port(neg, NEG_A, "c c:1 c:2 c:3");
    TEST_ASSERT_EQUAL_INT64(4, pint(neg, 1)); /* A_WIDTH */
    TEST_ASSERT_EQUAL_INT64(4, pint(neg, 2)); /* Y_WIDTH */
}

/* The lowest-ID cell gives the type: a $sub first negates the $add's operand. */
static void test_share_sub_first(void) {
    build(HEAD4 BIN("$sub", "sub1", "0", "\\a", "\\b", "\\t1")
              BIN("$add", "add1", "0", "\\a", "\\c", "\\t2")
                  MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$sub"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$add"));
    odin3_node_id neg = nth_of("$neg", 0);
    expect_port(neg, NEG_A, "c c:1 c:2 c:3");
    odin3_node_id inner = new_mux("$mux", "m");
    expect_port(inner, MUX_A, "b b:1 b:2 b:3");
    expect_same_port(inner, MUX_B, neg, 1);
    expect_same_port(nth_of("$sub", 0), OP_B, inner, MUX_Y);
}

/* A narrower operand under a wider sum: the $neg runs at the sum's width, the other is extended. */
static void test_share_add_sub_widths(void) {
    build("module \\m\n  wire width 3 input 1 \\a\n  wire width 3 input 2 \\b\n"
          "  wire width 3 input 3 \\c\n  wire input 4 \\s\n  wire width 5 output 5 \\y\n"
          "  wire width 5 \\t1\n  wire width 5 \\t2\n" BINW(
              "$add", "add1", "1", "1", "3", "3", "5", "\\a", "\\b", "\\t1")
              BINW("$sub", "sub1", "1", "1", "3", "3", "5", "\\a", "\\c", "\\t2")
                  "  cell $mux \\m\n    parameter \\WIDTH 5\n    connect \\A \\t1\n"
                  "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    run_share();
    expect_merged(2);
    odin3_node_id neg = nth_of("$neg", 0);
    TEST_ASSERT_EQUAL_INT64(5, pint(neg, 2));
    odin3_node_id inner = new_mux("$mux", "m");
    TEST_ASSERT_EQUAL_INT64(5, pint(inner, PRM_WIDTH));
    expect_port(inner, MUX_A, "b b:1 b:2 b:2 b:2"); /* sign extended */
    TEST_ASSERT_EQUAL_INT64(5, pint(nth_of("$add", 0), PRM_B_WIDTH));
}

static void test_share_fanout(void) {
    build(HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
              BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")
                  MUX4("m", "\\t1", "\\t2", "\\s", "\\y")
                      "  connect \\y2 \\t1\nend\n");
    run_share();
    expect_merged(0);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
}

/* Two shared operands: the one with the lower net ID is chosen, whichever port it is on. */
static void test_share_order(void) {
    build("module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n  wire input 3 \\s\n"
          "  wire width 4 output 4 \\y\n  wire width 4 \\t1\n  wire width 4 \\t2\n" BIN(
              "$and", "and1", "0", "\\a", "\\b", "\\t1") BIN("$and", "and2", "0", "\\a", "\\b",
                                                             "\\t2")
              MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    expect_merged(2);
    odin3_node_id op = nth_of("$and", 0);
    expect_port(op, OP_A, "a a:1 a:2 a:3"); /* a has the lower net ID */
    reset();
    build("module \\m\n  wire width 4 input 1 \\b\n  wire width 4 input 2 \\a\n  wire input 3 \\s\n"
          "  wire width 4 output 4 \\y\n  wire width 4 \\t1\n  wire width 4 \\t2\n" BIN(
              "$and", "and1", "0", "\\a", "\\b", "\\t1") BIN("$and", "and2", "0", "\\a", "\\b",
                                                             "\\t2")
              MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    op = nth_of("$and", 0);
    expect_port(op, OP_B, "b b:1 b:2 b:3"); /* now b is lower: shared on port B */
}

/* The shared operand on port B of every cell. */
static void test_share_port_b(void) {
    build(HEAD4 BIN("$xor", "x1", "0", "\\b", "\\a", "\\t1") BIN("$xor", "x2", "0", "\\c", "\\a",
                                                                 "\\t2")
              MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    expect_merged(2);
    odin3_node_id op = nth_of("$xor", 0);
    expect_port(op, OP_B, "a a:1 a:2 a:3");
    expect_same_port(op, OP_A, new_mux("$mux", "m"), MUX_Y);
}

/* Two groups under one mux. */
static void test_share_two_groups(void) {
    build(HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1") BIN("$and", "and2", "0", "\\a", "\\c",
                                                                   "\\t2")
              BIN("$or", "or1", "0", "\\d", "\\b", "\\t3") BIN("$or", "or2", "0", "\\d", "\\c",
                                                               "\\t4")
                  PMUX4("p", "4", "\\a", "{ \\t4 \\t3 \\t2 \\t1 }", "{ \\s2 \\s1 \\s \\s }",
                        "\\y") "end\n");
    run_share();
    expect_merged(4);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$or"));
}

/* A group of three under a $pmux whose A is a default: the new mux is a $pmux with A = x. */
static void test_share_pmux(void) {
    build(HEAD4 BIN("$add", "add1", "0", "\\a", "\\b", "\\t1") BIN("$sub", "sub1", "0", "\\a", "\\b",
                                                                   "\\t2")
              BIN("$add", "add2", "0", "\\a", "\\c", "\\t3")
                  PMUX4("p", "3", "4'xxxx", "{ \\t3 \\t2 \\t1 }", "{ \\s2 \\s1 \\s }", "\\y")
                      "end\n");
    run_share();
    expect_merged(3);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$neg"));
    odin3_node_id inner = new_mux("$pmux", "p");
    TEST_ASSERT_EQUAL_INT64(3, pint(inner, PRM_S_WIDTH));
    expect_port(inner, MUX_A, "x x x x");
    expect_port(inner, MUX_S, "s s1 s2");
    odin3_node_id neg = nth_of("$neg", 0);
    TEST_ASSERT_EQUAL_UINT32(pnet(PIN(inner, MUX_B, 4)).v, pnet(PIN(neg, 1, 0)).v);
    expect_port(inner, MUX_S, "s s1 s2");
}

/* --- provenance ------------------------------------------------------------------------------- */

static void test_share_parents(void) {
    build(HEAD4 "  attribute \\src \"t.v:1.1\"\n" BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
              "  attribute \\src \"t.v:2.1\"\n" BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")
                  "  attribute \\src \"t.v:3.1\"\n" MUX4("m", "\\t1", "\\t2", "\\s", "\\y")
                      "end\n");
    odin3_prov_id first = odin3_node_prov(fx.module, node_named("and1"));
    odin3_prov_id second = odin3_node_prov(fx.module, node_named("and2"));
    odin3_prov_id old = odin3_node_prov(fx.module, node_named("m"));
    TEST_ASSERT_TRUE(first.v != 0 && second.v != 0 && old.v != 0);
    run_share();
    odin3_node_id op = nth_of("$and", 0);
    odin3_node_id inner = new_mux("$mux", "m");
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(fx.module, op).v,
                             odin3_node_prov(fx.module, inner).v);
    const odin3_prov_record *got = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, op));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, got->kind);
    TEST_ASSERT_EQUAL_UINT32(3, got->parents.count);
    TEST_ASSERT_EQUAL_UINT32(first.v, got->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(second.v, got->parents.ids[1].v);
    TEST_ASSERT_EQUAL_UINT32(old.v, got->parents.ids[2].v);
}

/* The $neg of an add/sub mix has the same record; the cell keeps the first cell's name. */
static void test_share_neg_record_and_name(void) {
    build(HEAD4 BIN("$add", "add1", "0", "\\a", "\\b", "\\t1")
              BIN("$sub", "sub1", "0", "\\a", "\\c", "\\t2")
                  MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    odin3_node_id op = nth_of("$add", 0);
    TEST_ASSERT_EQUAL_UINT32(op.v, node_named("add1").v);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(fx.module, op).v,
                             odin3_node_prov(fx.module, nth_of("$neg", 0)).v);
}

/* --- what it leaves alone --------------------------------------------------------------------- */

typedef struct refusal {
    const char *name;
    const char *text;
} refusal;

#define KEEP_OP                                                                                    \
    HEAD4 "  attribute \\keep 1\n" BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")                  \
        BIN("$and", "and2", "0", "\\a", "\\c", "\\t2") MUX4("m", "\\t1", "\\t2", "\\s", "\\y")     \
            "end\n"

static const refusal k_refusals[] = {
    {"other class", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         BIN("$or", "or1", "0", "\\a", "\\c", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"no shared operand", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         BIN("$and", "and2", "0", "\\c", "\\d", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"shared on different ports", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         BIN("$and", "and2", "0", "\\c", "\\a", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"signed flags differ", HEAD4 BIN("$add", "add1", "1", "\\a", "\\b", "\\t1")
         BIN("$add", "add2", "0", "\\a", "\\c", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"shift amounts of other widths", HEAD4 BIN("$shl", "shl1", "0", "\\a", "\\b", "\\t1")
         BINW("$shl", "shl2", "0", "0", "4", "2", "4", "\\a", "\\c [1:0]", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"mixed-sign add/sub", HEAD4
         BINW("$add", "add1", "1", "0", "4", "4", "4", "\\a", "\\b", "\\t1")
             BINW("$sub", "sub1", "1", "0", "4", "4", "4", "\\a", "\\c", "\\t2")
                 MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"add/sub on port B", HEAD4 BIN("$add", "add1", "0", "\\b", "\\a", "\\t1")
         BIN("$sub", "sub1", "0", "\\c", "\\a", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"only part of the port", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")
             MUX4("m", "{ \\t1 [3:2] \\t2 [1:0] }", "\\t2", "\\s", "\\y") "end\n"},
    {"kept cell", KEEP_OP},
    {"x select", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")
             MUX4("m", "\\t1", "\\t2", "1'x", "\\y") "end\n"},
    {"open input", HEAD4 "  cell $and \\and1\n    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n"
         "    parameter \\Y_WIDTH 4\n    connect \\A \\a\n    connect \\Y \\t1\n  end\n"
             BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")
                 MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"one cell on both ports", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         MUX4("m", "\\t1", "\\t1", "\\s", "\\y") "end\n"},
    {"one cell and a plain port", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\t1")
         MUX4("m", "\\t1", "\\c", "\\s", "\\y") "end\n"},
    {"read by a port", HEAD4 BIN("$and", "and1", "0", "\\a", "\\b", "\\y2")
         BIN("$and", "and2", "0", "\\a", "\\c", "\\t2") MUX4("m", "\\y2", "\\t2", "\\s", "\\y")
             "end\n"},
};

static void test_share_refusals(void) {
    for (size_t i = 0; i < sizeof k_refusals / sizeof k_refusals[0]; i++) {
        build(k_refusals[i].text);
        uint32_t before = count_live();
        run_share();
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, count_live(), k_refusals[i].name);
        expect_merged(0);
        reset();
    }
}

/* A `keep` wire on a cell's output keeps the cell out. */
static void test_share_keep_wire(void) {
    build("module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
          "  wire width 4 input 3 \\c\n  wire input 4 \\s\n  wire width 4 output 5 \\y\n"
          "  attribute \\keep 1\n  wire width 4 \\t1\n  wire width 4 \\t2\n" BIN(
              "$and", "and1", "0", "\\a", "\\b", "\\t1") BIN("$and", "and2", "0", "\\a", "\\c",
                                                             "\\t2")
              MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n");
    run_share();
    expect_merged(0);
}

/* --- the function is kept --------------------------------------------------------------------- */


static uint32_t bits_set(uint32_t value) {
    uint32_t hits = 0;
    for (; value != 0; value >>= 1) {
        hits += value & 1U;
    }
    return hits;
}

typedef struct sim_pair {
    odin3_sim *left, *right;
} sim_pair;

static uint32_t input_bits(const odin3_sim *sim) {
    uint32_t bits = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        bits += odin3_sim_input_width(sim, port);
    }
    return bits;
}

static void drive(odin3_sim *sim, uint32_t vector) {
    uint32_t at = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim, port); bit++) {
            odin3_sim_bit ref = {port, bit};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(sim, ref, ((vector >> at) & 1U) != 0));
            at++;
        }
    }
    odin3_sim_cycle(sim);
}

static void compare_outputs(const sim_pair *pair, uint32_t vector, const char *what) {
    for (uint32_t port = 0; port < odin3_sim_output_count(pair->left); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_output_width(pair->left, port); bit++) {
            odin3_sim_bit ref = {port, bit};
            bool lhs = false;
            bool rhs = false;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(pair->left, ref, &lhs));
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(pair->right, ref, &rhs));
            if (lhs != rhs) {
                printf("%s: vector %u output %u bit %u\n", what, (unsigned)vector,
                       (unsigned)port, (unsigned)bit);
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(lhs ? 1 : 0, rhs ? 1 : 0, what);
        }
    }
}

/*
 * Builds text twice, runs opt_share on one copy (it must merge `merged` cells) and simulates both
 * on every input vector (a sample when the inputs total more than MAX_SIM_BITS bits).
 */
static void expect_function_kept(const refusal *ec, uint32_t merged, uint32_t onehot) {
    const char *what = ec->name;
    const char *text = ec->text;
    pb_result ref;
    memset(&ref, 0, sizeof ref);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pb_build(text, &ref), what);
    build(text);
    run_share();
    expect_merged(merged);
    sim_pair pair = {NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_build(ref.design, (odin3_module_id){1}, &pair.left));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build(fx.res.design, (odin3_module_id){1}, &pair.right));
    const uint32_t bits = input_bits(pair.left);
    TEST_ASSERT_TRUE_MESSAGE(bits <= MAX_INPUT_BITS, what);
    /* every vector up to MAX_SIM_BITS inputs, else a xorshift sample of as many */
    uint32_t rng = 0x2545F491U;
    const uint32_t count = bits <= MAX_SIM_BITS ? (1U << bits) : (1U << MAX_SIM_BITS);
    for (uint32_t i = 0; i < count; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const uint32_t vector = bits <= MAX_SIM_BITS ? i : rng;
        if (bits_set(vector & onehot) > 1) {
            continue;
        }
        drive(pair.left, vector);
        drive(pair.right, vector);
        compare_outputs(&pair, vector, what);
    }
    odin3_sim_destroy(pair.left);
    odin3_sim_destroy(pair.right);
    pb_free(&ref);
}

#define OUT1(w) "  wire width " w " output 6 \\y\n"


/* The select inputs of a $pmux case: two of them set is undefined (x) in RTLIL, so such vectors
 * are skipped (spec 17 #46). The xor case has them at input bits 16 to 18, the add/sub one at 9
 * to 11. */
enum { XOR_PMUX_CASE = 7, ADDSUB_PMUX_CASE = 8, XOR_PMUX_SELECTS = 0x70000, ADDSUB_SELECTS = 0xE00 };

static const refusal k_equiv[] = {
    {"same type", SAME_TYPE},
    {"add sub", HEAD4 BIN("$add", "add1", "0", "\\a", "\\b", "\\t1")
         BIN("$sub", "sub1", "0", "\\a", "\\c", "\\t2")
             MUX4("m", "\\t1", "\\t2", "\\s", "\\y") "end\n"},
    {"add sub signed narrow", "module \\m\n  wire width 3 input 1 \\a\n  wire width 3 input 2 \\b\n"
         "  wire width 3 input 3 \\c\n  wire input 4 \\s\n  wire width 5 output 5 \\y\n"
         "  wire width 5 \\t1\n  wire width 5 \\t2\n"
             BINW("$add", "add1", "1", "1", "3", "3", "5", "\\a", "\\b", "\\t1")
                 BINW("$sub", "sub1", "1", "1", "3", "3", "5", "\\a", "\\c", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 5\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
    {"add sub unsigned wide operand", "module \\m\n  wire width 2 input 1 \\a\n"
         "  wire width 5 input 2 \\b\n  wire width 5 input 3 \\c\n  wire input 4 \\s\n"
         "  wire width 3 output 5 \\y\n  wire width 3 \\t1\n  wire width 3 \\t2\n"
             BINW("$add", "add1", "0", "0", "2", "5", "3", "\\a", "\\b", "\\t1")
                 BINW("$sub", "sub1", "0", "0", "2", "5", "3", "\\a", "\\c", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 3\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
    {"add widths differ", "module \\m\n  wire width 3 input 1 \\a\n  wire width 2 input 2 \\b\n"
         "  wire width 4 input 3 \\c\n  wire input 4 \\s\n  wire width 5 output 5 \\y\n"
         "  wire width 5 \\t1\n  wire width 5 \\t2\n"
             BINW("$add", "add1", "1", "1", "3", "2", "5", "\\a", "\\b", "\\t1")
                 BINW("$add", "add2", "1", "1", "3", "4", "5", "\\a", "\\c", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 5\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
    {"lt signed widths differ", "module \\m\n  wire width 3 input 1 \\a\n"
         "  wire width 2 input 2 \\b\n  wire width 4 input 3 \\c\n  wire input 4 \\s\n"
         "  wire output 5 \\y\n  wire \\t1\n  wire \\t2\n"
             BINW("$lt", "lt1", "1", "1", "3", "2", "1", "\\a", "\\b", "\\t1")
                 BINW("$lt", "lt2", "1", "1", "3", "4", "1", "\\a", "\\c", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 1\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
    {"mul unsigned", "module \\m\n  wire width 3 input 1 \\a\n  wire width 2 input 2 \\b\n"
         "  wire width 3 input 3 \\c\n  wire input 4 \\s\n  wire width 4 output 5 \\y\n"
         "  wire width 4 \\t1\n  wire width 4 \\t2\n"
             BINW("$mul", "mul1", "0", "0", "3", "2", "4", "\\a", "\\b", "\\t1")
                 BINW("$mul", "mul2", "0", "0", "3", "3", "4", "\\a", "\\c", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 4\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
    {"xor pmux", HEAD4 BIN("$xor", "x1", "0", "\\a", "\\b", "\\t1") BIN("$xor", "x2", "0", "\\a",
                                                                       "\\c", "\\t2")
         BIN("$xor", "x3", "0", "\\a", "\\d", "\\t3")
             PMUX4("p", "3", "4'0110", "{ \\t3 \\t2 \\t1 }", "{ \\s2 \\s1 \\s }", "\\y") "end\n"},
    {"add sub pmux", "module \\m\n  wire width 3 input 1 \\a\n  wire width 3 input 2 \\b\n"
         "  wire width 3 input 3 \\c\n  wire input 4 \\s\n  wire input 5 \\s1\n  wire input 6 \\s2\n"
         "  wire width 3 output 7 \\y\n  wire width 3 \\t1\n  wire width 3 \\t2\n"
         "  wire width 3 \\t3\n" BINW("$add", "add1", "0", "0", "3", "3", "3", "\\a", "\\b", "\\t1")
             BINW("$sub", "sub1", "0", "0", "3", "3", "3", "\\a", "\\b", "\\t2")
                 BINW("$sub", "sub2", "0", "0", "3", "3", "3", "\\a", "\\c", "\\t3")
                     "  cell $pmux \\p\n    parameter \\WIDTH 3\n    parameter \\S_WIDTH 3\n"
                     "    connect \\A 3'101\n    connect \\B { \\t3 \\t2 \\t1 }\n"
                     "    connect \\S { \\s2 \\s1 \\s }\n    connect \\Y \\y\n  end\nend\n"},
    {"sub port B", "module \\m\n  wire width 3 input 1 \\a\n  wire width 3 input 2 \\b\n"
         "  wire width 3 input 3 \\c\n  wire input 4 \\s\n  wire width 3 output 5 \\y\n"
         "  wire width 3 \\t1\n  wire width 3 \\t2\n"
             BINW("$sub", "sub1", "1", "1", "3", "3", "3", "\\b", "\\a", "\\t1")
                 BINW("$sub", "sub2", "1", "1", "3", "3", "3", "\\c", "\\a", "\\t2")
                     "  cell $mux \\m\n    parameter \\WIDTH 3\n    connect \\A \\t1\n"
                     "    connect \\B \\t2\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"},
};

static void test_share_function_kept(void) {
    for (size_t i = 0; i < sizeof k_equiv / sizeof k_equiv[0]; i++) {
        const uint32_t merged = i == XOR_PMUX_CASE || i == ADDSUB_PMUX_CASE ? 3U : 2U; /* the two pmux groups have three */
        const uint32_t onehot = i == XOR_PMUX_CASE ? XOR_PMUX_SELECTS
                                : (i == ADDSUB_PMUX_CASE ? ADDSUB_SELECTS : 0U);
        expect_function_kept(&k_equiv[i], merged, onehot);
        reset();
    }
}

/* --- Yosys 0.55's own cases ------------------------------------------------------------------- */

static const char *const k_cases[] = {
    "add_sub", "mux_tree", "large_pmux_part", "diff_port_widths", "extend",
    "bug2334", "bug2335",  "cat",             "xor3",             "same_type",
};

static void read_case(const char *name, const char *ext, char *buf) {
    char path[LINE_MAX_LEN];
    int len = snprintf(path, sizeof path, "%s/%s%s", ODIN3_SHARE_DATA, name, ext);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, CASE_BUF - 1U, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < CASE_BUF - 1U, path);
    buf[got] = '\0';
}

static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp((const char *)lhs, (const char *)rhs);
}

typedef struct summary {
    char cells[MAX_CELLS][ITEM_LEN];
    uint32_t n_cells;
} summary;

/* The .expect lines with the given prefix ("cell " or "odin3 cell "), sorted, prefix cut. */
static void parse_expect(char *text, const char *prefix, summary *out) {
    memset(out, 0, sizeof *out);
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            TEST_ASSERT_TRUE(out->n_cells < MAX_CELLS && strlen(line) < ITEM_LEN);
            (void)snprintf(out->cells[out->n_cells++], ITEM_LEN, "cell %s", line + strlen(prefix));
        }
    }
    qsort(out->cells, out->n_cells, ITEM_LEN, cmp_str);
}

static void describe_cell(odin3_node_id node, char *line) {
    static const char *const k_watch[] = {"A_WIDTH", "B_WIDTH", "Y_WIDTH", "WIDTH", "S_WIDTH"};
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    (void)snprintf(line, ITEM_LEN, "cell %s", def->name);
    for (size_t name = 0; name < sizeof k_watch / sizeof k_watch[0]; name++) {
        for (uint32_t i = 0; i < def->n_params; i++) {
            if (strcmp(def->params[i].name, k_watch[name]) == 0) {
                size_t at = strlen(line);
                (void)snprintf(line + at, ITEM_LEN - at, " %s=%lld", k_watch[name],
                               (long long)odin3_node_param(fx.module, node, i)->i);
            }
        }
    }
}

static void summarize(summary *out) {
    memset(out, 0, sizeof *out);
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        if (!odin3_node_live(fx.module, node) || def->gran == ODIN3_GRAN_PORT ||
            strncmp(def->name, "$_CONST", 7) == 0) {
            continue;
        }
        TEST_ASSERT_TRUE(out->n_cells < MAX_CELLS);
        describe_cell(node, out->cells[out->n_cells++]);
    }
    qsort(out->cells, out->n_cells, ITEM_LEN, cmp_str);
}

static void run_yosys_case(const char *name) {
    static char text[CASE_BUF];
    static char expect[CASE_BUF];
    static char again[CASE_BUF];
    summary want;
    summary ours;
    summary got;
    read_case(name, ".il", text);
    read_case(name, ".expect", expect);
    memcpy(again, expect, sizeof again);
    parse_expect(expect, "cell ", &want);
    parse_expect(again, "odin3 cell ", &ours);
    const summary *use = ours.n_cells > 0 ? &ours : &want;
    build(text);
    run_share();
    summarize(&got);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(use->n_cells, got.n_cells, name);
    for (uint32_t i = 0; i < use->n_cells; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(use->cells[i], got.cells[i], name);
    }
}

static void test_share_yosys_cases(void) {
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        run_yosys_case(k_cases[i]);
        reset();
    }
}

/* --- determinism, arguments ------------------------------------------------------------------- */

static void rtlil_of_run(odin3_strbuf *out) {
    build(HEAD4 BIN("$add", "add1", "0", "\\a", "\\b", "\\t1")
              BIN("$sub", "sub1", "0", "\\a", "\\b", "\\t2")
                  BIN("$sub", "sub2", "0", "\\a", "\\c", "\\t3")
                      PMUX4("p", "3", "4'xxxx", "{ \\t3 \\t2 \\t1 }", "{ \\s2 \\s1 \\s }", "\\y")
                          "end\n");
    run_share();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, out));
}

static void test_share_determinism(void) {
    odin3_strbuf first;
    odin3_strbuf second;
    odin3_strbuf_init(&first);
    odin3_strbuf_init(&second);
    rtlil_of_run(&first);
    reset();
    rtlil_of_run(&second);
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

static void test_share_bad_arguments(void) {
    build(SAME_TYPE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_share_run(NULL, fx.res.design, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_share_run(&fx.ctx, NULL, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_share_run(&fx.ctx, fx.res.design, NULL, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_share_run(&fx.ctx, fx.res.design, &fx.opts, NULL));
    TEST_ASSERT_EQUAL_UINT32(4, count_lines(ODIN3_LOG_ERROR, "opt_share: no pass run, design, options or counts"));
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* Every shape of one rewrite: a plain group, an add/sub mix with an extension, a $pmux group. */
#define OOM_TEXT                                                                                   \
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"                         \
    "  wire width 4 input 3 \\c\n  wire width 3 input 4 \\e\n  wire width 3 input 5 \\f\n"         \
    "  wire input 6 \\s\n  wire input 7 \\s1\n  wire input 8 \\s2\n"                               \
    "  wire width 4 output 9 \\y1\n  wire width 6 output 10 \\y2\n  wire width 4 output 11 \\y3\n" \
    "  wire width 4 \\t1\n  wire width 4 \\t2\n  wire width 6 \\u1\n  wire width 6 \\u2\n"         \
    "  wire width 4 \\v1\n  wire width 4 \\v2\n  wire width 4 \\v3\n"                              \
    BIN("$and", "and1", "0", "\\a", "\\b", "\\t1") BIN("$and", "and2", "0", "\\a", "\\c", "\\t2")   \
        MUX4("m1", "\\t1", "\\t2", "\\s", "\\y1")                                                  \
        BINW("$add", "add1", "1", "1", "4", "3", "6", "\\a", "\\e", "\\u1")                        \
        BINW("$sub", "sub1", "1", "1", "4", "3", "6", "\\a", "\\f", "\\u2")                        \
        "  cell $mux \\m2\n    parameter \\WIDTH 6\n    connect \\A \\u1\n    connect \\B \\u2\n"    \
        "    connect \\S \\s1\n    connect \\Y \\y2\n  end\n"                                       \
        BIN("$add", "add2", "0", "\\a", "\\b", "\\v1") BIN("$sub", "sub2", "0", "\\a", "\\b",      \
                                                           "\\v2")                                 \
            BIN("$sub", "sub3", "0", "\\a", "\\c", "\\v3")                                         \
                PMUX4("p", "3", "4'xxxx", "{ \\v3 \\v2 \\v1 }", "{ \\s2 \\s1 \\s }", "\\y3")      \
                    "end\n"

enum { OOM_MERGED = 7 }; /* 2 + 2 + 3 */

static long g_half_done;

static void count_half_done(void) {
    for (uint32_t i = 0; i < n_lines; i++) {
        g_half_done += strstr(lines[i].text, "in the middle of a rewrite") != NULL ? 1 : 0;
    }
}

/* One run with the fail-th allocation failing; the IR is valid and every start state is kept. */
static void oom_run_once(long fail, bool *done) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    const uint32_t cells = count_live();
    n_lines = 0;
    const long half_before = g_half_done;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_share_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    count_half_done();
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (st != ODIN3_OK && g_half_done == half_before) {
        /* undone: nothing but a canonical constant node (the x of a $pmux's A) is left over */
        TEST_ASSERT_TRUE(count_live() >= cells && count_live() <= cells + 1U);
    }
}

static void test_share_oom(void) {
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(OOM_MERGED, fx.counts.share_merged);
    TEST_ASSERT_TRUE(failures > 50 && g_half_done > 0 && g_half_done < failures);
    printf("opt_share OOM: %ld failing allocation points, %ld in the middle of a kit merge\n",
           failures, g_half_done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_share_same_type);
    RUN_TEST(test_share_old_mux_stays);
    RUN_TEST(test_share_second_call);
    RUN_TEST(test_share_add_sub);
    RUN_TEST(test_share_sub_first);
    RUN_TEST(test_share_add_sub_widths);
    RUN_TEST(test_share_fanout);
    RUN_TEST(test_share_order);
    RUN_TEST(test_share_port_b);
    RUN_TEST(test_share_two_groups);
    RUN_TEST(test_share_pmux);
    RUN_TEST(test_share_parents);
    RUN_TEST(test_share_neg_record_and_name);
    RUN_TEST(test_share_refusals);
    RUN_TEST(test_share_keep_wire);
    RUN_TEST(test_share_function_kept);
    RUN_TEST(test_share_yosys_cases);
    RUN_TEST(test_share_determinism);
    RUN_TEST(test_share_bad_arguments);
    RUN_TEST(test_share_oom);
    return UNITY_END();
}
