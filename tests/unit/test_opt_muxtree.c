/*
 * test_opt_muxtree.c — opt_muxtree (2F spec OP-7, §7, §20.5; plan Task 10): dead ports of nested
 * muxes under a shared select, constant selects, roots (fan-out, `keep` wires), the known-bit
 * replacements, the in-place/re-queue rule of non-pure roots, the evaluation budget, a chain of
 * 100,000 muxes (no recursion), modules with processes, the lines, names and provenance of the
 * rebuilt nodes, Yosys 0.55's own cases and out of memory.
 *
 * Yosys parity (0.55, the VTR build): each tests/unit/data/opt/muxtree/X.il is run as
 * `yosys -p "read_rtlil X.il; opt_muxtree; write_rtlil X.out"` and X.expect holds the result,
 * normalised by name: one `cell` line per cell (type, WIDTH, S_WIDTH for a $pmux, and each of A, B,
 * S, Y as the bit labels LSB first), one `wire` line per wire bit whose net is not its own label,
 * and `removed N`, Yosys's "Removed N multiplexer ports.". A bit label is `name:k` (the wire and
 * bit position) or 0, 1, x, z; the label of a connected class of wire bits is the smallest of them
 * (strcmp), the constant if the class has one. Lines are compared as a multiset. The header of each
 * .expect records the command and any edit of the Yosys-written input.
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
    LINES_MAX = 512,
    OOM_LIMIT = 20000,
    CASE_BUF = 8192,
    DUMP_LINES = 96,
    DUMP_LEN = 320,
    LABEL_LEN = 64,
    CHAIN_LEN = 100000,
    SPEC_MAX = 128,
    REPEAT = 6
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

/* Lines of `level` that start with prefix. */
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

static void run_muxtree(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_muxtree_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

/* The INFO line of the last run and the counter agree on want. */
static void expect_removed(uint32_t want) {
    char text[LINE_MAX_LEN];
    int len =
        snprintf(text, sizeof text, "opt_muxtree: removed %u multiplexer ports", (unsigned)want);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, text));
    TEST_ASSERT_EQUAL_UINT32(want, fx.counts.muxtree_ports);
}

static char *rtlil_text(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &buf));
    return buf.data;
}

static void set_max_evals(uint32_t evals) {
    odin3_opt_limits limits = {ODIN3_OPT_MAX_ITERATIONS, evals};
    odin3_opt_test_set_limits(&limits);
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
    if (strcmp(what, "0") == 0) {
        return cv == ODIN3_CONST_0;
    }
    if (strcmp(what, "1") == 0) {
        return cv == ODIN3_CONST_1;
    }
    if (strcmp(what, "x") == 0) {
        return cv == ODIN3_CONST_X;
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

enum { MUX_A = 0, MUX_B = 1, MUX_S = 2, MUX_Y = 3, PRM_WIDTH = 0, PRM_S_WIDTH = 1 };

/* --- texts ------------------------------------------------------------------------------------ */

#define MUX(name, a, b, s, y)                                                                      \
    "  cell $mux \\" name "\n    parameter \\WIDTH 1\n    connect \\A " a "\n    connect \\B " b   \
    "\n    connect \\S " s "\n    connect \\Y " y "\n  end\n"

#define PMUX(name, sw, a, b, s, y)                                                                 \
    "  cell $pmux \\" name "\n    parameter \\WIDTH 1\n    parameter \\S_WIDTH " sw                \
    "\n    connect \\A " a "\n    connect \\B " b "\n    connect \\S " s "\n    connect \\Y " y    \
    "\n  end\n"

#define HEAD                                                                                       \
    "module \\m\n  wire input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n  wire input 4 \\c\n" \
    "  wire output 5 \\y\n  wire \\t\n"

/* y = s ? (s ? a : b) : c: the inner mux sits on the outer B and its A port (b) is dead. */
#define NESTED_TEXT                                                                                \
    HEAD MUX("inner", "\\b", "\\a", "\\s", "\\t") MUX("outer", "\\c", "\\t", "\\s", "\\y") "end\n"

/* --- the tests -------------------------------------------------------------------------------- */

static void test_nested_same_sel(void) {
    build(NESTED_TEXT);
    odin3_node_id inner_before = node_named("inner");
    TEST_ASSERT_TRUE(odin3_node_valid(inner_before));
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("inner"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
    odin3_node_id outer = node_named("outer");
    expect_port(outer, MUX_A, "c");
    expect_port(outer, MUX_B, "a");
    expect_port(outer, MUX_S, "s");
    expect_port(outer, MUX_Y, "y");
    TEST_ASSERT_EQUAL_UINT32(wbit("a", 0).v, wbit("t", 0).v);
    expect_removed(1);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port 2/2 on $mux 'inner'"));
    TEST_ASSERT_EQUAL_UINT32(1, count_prefix(ODIN3_LOG_DEBUG, "opt_muxtree: module"));
}

/* The inner mux on the A side of the outer one runs under the opposite knowledge. */
static void test_nested_inverse_sel(void) {
    build(HEAD MUX("inner", "\\b", "\\a", "\\s", "\\t")
              MUX("outer", "\\t", "\\c", "\\s", "\\y") "end\n");
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("inner"));
    expect_port(node_named("outer"), MUX_A, "b");
    expect_removed(1);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port 1/2 on $mux 'inner'"));
}

/* A constant select: 1 is the B port alone, 0 and x leave A. */
static void test_const_s(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n" MUX(
        "mx", "\\a", "\\b", "1'1", "\\y") "end\n");
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("mx"));
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, wbit("b", 0).v);
    expect_removed(1);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port "
                                                             "2/2 on $mux 'mx'"));
    static const char *const k_sels[] = {"1'0", "1'x", "1'z"};
    for (size_t i = 0; i < sizeof k_sels / sizeof k_sels[0]; i++) {
        reset();
        char text[CASE_BUF];
        (void)snprintf(text, sizeof text,
                       "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"
                       "  cell $mux \\mx\n    parameter \\WIDTH 1\n    connect \\A \\a\n"
                       "    connect \\B \\b\n    connect \\S %s\n    connect \\Y \\y\n  end\nend\n",
                       k_sels[i]);
        build(text);
        run_muxtree();
        TEST_ASSERT_TRUE_MESSAGE(node_gone("mx"), k_sels[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wbit("y", 0).v, wbit("a", 0).v, k_sels[i]);
        expect_removed(1);
    }
}

/* A constant-active slice in the middle of a $pmux wins; the others and A are dead. */
static void test_pmux_const(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"
          "  wire input 4 \\d\n  wire input 5 \\s\n  wire output 6 \\y\n" PMUX(
              "p", "3", "\\a", "{ \\b \\c \\d }", "{ \\s 1'1 1'0 }", "\\y") "end\n");
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("p"));
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, wbit("c", 0).v);
    expect_removed(3);
}

/* A mux that feeds two muxes is a root and is evaluated once: m1 and m2 each cost two evaluations
 * (itself and x below its B port), x itself one. The cap is hit by the fifth evaluation. */
#define SHARED_TEXT                                                                                \
    "module \\m\n  wire input 1 \\s\n  wire input 2 \\p\n  wire input 3 \\q\n"                     \
    "  wire input 4 \\r1\n  wire input 5 \\r2\n  wire output 6 \\y1\n  wire output 7 \\y2\n"       \
    "  wire \\xw\n" MUX("x", "\\q", "\\p", "\\s", "\\xw") MUX("m1", "\\r1", "\\xw", "\\s", "\\y1") \
        MUX("m2", "\\r2", "\\xw", "\\s", "\\y2") "end\n"

static void test_root_fanout(void) {
    build(SHARED_TEXT);
    set_max_evals(REPEAT);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_WARN, "opt_muxtree:"));
    TEST_ASSERT_TRUE(node_gone("x")); /* x's A (q) is dead under both users: wiring to p */
    TEST_ASSERT_EQUAL_UINT32(wbit("xw", 0).v, wbit("p", 0).v);
    expect_removed(1);
    reset();
    build(SHARED_TEXT);
    set_max_evals(REPEAT - 1U);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN, "opt_muxtree: module 'm': giving up after 5 evaluations"));
    TEST_ASSERT_FALSE(node_gone("x"));
    expect_removed(0);
}

/* The shared mux is read on the A side of one user: that context enables its A port. */
static void test_root_fanout_live(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\p\n  wire input 3 \\q\n"
          "  wire input 4 \\r1\n  wire input 5 \\r2\n  wire output 6 \\y1\n  wire output 7 \\y2\n"
          "  wire \\xw\n" MUX("x", "\\q", "\\p", "\\s", "\\xw")
              MUX("m1", "\\r1", "\\xw", "\\s", "\\y1")
                  MUX("m2", "\\xw", "\\r2", "\\s", "\\y2") "end\n");
    run_muxtree();
    TEST_ASSERT_FALSE(node_gone("x"));
    expect_removed(0);
}

#define KEEP_TEXT(keep)                                                                            \
    "module \\m\n  wire input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n"                     \
    "  wire input 4 \\c\n  wire output 5 \\y\n" keep                                               \
    "  wire \\t\n" MUX("inner", "\\b", "\\a", "\\s", "\\t")                                        \
        MUX("outer", "\\c", "\\t", "\\s", "\\y") "end\n"

/* A mux output on a `keep` wire is a pure root: it is evaluated alone, with no knowledge. */
static void test_keep_wire(void) {
    build(KEEP_TEXT("  attribute \\keep 1\n"));
    run_muxtree();
    TEST_ASSERT_FALSE(node_gone("inner"));
    expect_removed(0);
    reset();
    build(KEEP_TEXT(""));
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("inner"));
    expect_removed(1);
    reset();
    build(KEEP_TEXT("  attribute \\keep 0\n"));
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("inner"));
}

/* A mux read by a non-mux node is a pure root too. */
static void test_non_mux_reader(void) {
    build(HEAD "  wire output 6 \\z\n" MUX("inner", "\\b", "\\a", "\\s", "\\t")
              MUX("outer", "\\c", "\\t", "\\s",
                  "\\y") "  cell $reduce_or \\r\n    parameter \\A_WIDTH 1\n    connect \\A \\t\n"
                         "    connect \\Y \\z\n  end\nend\n");
    run_muxtree();
    TEST_ASSERT_FALSE(node_gone("inner"));
    expect_removed(0);
}

/* A mux with all its ports dead is deleted: its only reader sits behind a select that is 0. */
static void test_dead_child(void) {
    build(HEAD MUX("inner", "\\b", "\\a", "\\s", "\\t")
              MUX("outer", "\\c", "\\t", "1'0", "\\y") "end\n");
    run_muxtree();
    TEST_ASSERT_TRUE(node_gone("inner"));
    TEST_ASSERT_TRUE(node_gone("outer"));
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, wbit("c", 0).v);
    expect_removed(3);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port 1/2 on $mux 'inner'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port 2/2 on $mux 'inner'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port 1/2 on $mux 'outer'"));
}

/* An unnamed cell is labelled $c<ID>. */
static void test_unnamed_label(void) {
    build(HEAD MUX("inner", "\\b", "\\a", "\\s", "\\t")
              MUX("outer", "\\c", "\\t", "\\s", "\\y") "end\n");
    odin3_node_id inner = node_named("inner");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, inner, 0));
    run_muxtree();
    char want[LINE_MAX_LEN];
    (void)snprintf(want, sizeof want, "opt_muxtree: module 'm': dead port 2/2 on $mux '$c%u'",
                   (unsigned)inner.v);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_DEBUG, want));
}

/* --- known bits ------------------------------------------------------------------------------- */

/* in1 sits on the outer B (s active): its A bit s reads 1; in2 on the outer A: it reads 0. No port
 * is dead, so nothing is counted, but both nodes are rebuilt with the constants. */
static void test_replace_known(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\u\n  wire input 3 \\v\n"
          "  wire input 4 \\a\n  wire input 5 \\a2\n  wire output 6 \\y\n  wire \\i1\n  wire \\i2\n"
          "  attribute \\keep 1\n" MUX("in1", "\\s", "\\a", "\\u", "\\i1")
              MUX("in2", "\\s", "\\a2", "\\v", "\\i2")
                  MUX("outer", "\\i2", "\\i1", "\\s", "\\y") "end\n");
    odin3_prov_id prov = odin3_node_prov(fx.module, node_named("in1"));
    run_muxtree();
    expect_port(node_named("in1"), MUX_A, "1");
    expect_port(node_named("in1"), MUX_B, "a");
    expect_port(node_named("in1"), MUX_Y, "i1");
    expect_port(node_named("in2"), MUX_A, "0");
    expect_port(node_named("in2"), MUX_B, "a2");
    expect_port(node_named("outer"), MUX_A, "i2");
    expect_removed(0);
    TEST_ASSERT_NOT_NULL(odin3_attr_get(
        fx.module, (odin3_objref){ODIN3_OBJ_NODE, node_named("in1").v}, intern("keep")));
    const odin3_prov_record *rec =
        odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, node_named("in1")));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(prov.v, rec->parents.ids[0].v);
    /* nothing left to replace: the second run leaves the nodes alone */
    odin3_node_id before = node_named("in1");
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(before.v, node_named("in1").v);
}

/* A data bit that is one of the mux's own selects: 1 in its own slice, 0 elsewhere and in A. */
static void test_replace_ctrl(void) {
    build("module \\m\n  wire input 1 \\s0\n  wire input 2 \\s1\n  wire input 3 \\a1\n"
          "  wire input 4 \\b0\n  wire input 5 \\a2\n  wire width 2 output 6 \\y\n"
          "  cell $pmux \\pm\n    parameter \\WIDTH 2\n    parameter \\S_WIDTH 2\n"
          "    connect \\A { \\s1 \\a2 }\n    connect \\B { \\s0 \\b0 \\a1 \\s0 }\n"
          "    connect \\S { \\s1 \\s0 }\n    connect \\Y \\y\n  end\nend\n");
    run_muxtree();
    odin3_node_id mux = node_named("pm");
    expect_port(mux, MUX_A, "a2 0");
    expect_port(mux, MUX_B, "1 a1 b0 0");
    expect_removed(0);
}

/* A repeated select: Yosys compares an index into the distinct selects with the slice number and
 * replaces the own select of slices 1 and 2 by 0 (wrong when only that select is set; Yosys 0.55
 * gives B = { 0 0 y } for this cell); here each reads 1 (§17 row 17). */
static void test_replace_dup_select(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\t\n  wire input 3 \\x\n"
          "  wire input 4 \\y\n  wire input 5 \\a\n  wire output 6 \\o\n"
          "  cell $pmux \\pm\n    parameter \\WIDTH 1\n    parameter \\S_WIDTH 3\n"
          "    connect \\A \\a\n    connect \\B { \\t \\s \\y }\n"
          "    connect \\S { \\t \\s \\s }\n    connect \\Y \\o\n  end\nend\n");
    run_muxtree();
    expect_port(node_named("pm"), MUX_B, "y 1 1");
    expect_removed(0);
}

/* Widths above one: the knowledge applies per bit. */
static void test_replace_wide(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\u\n  wire width 2 input 3 \\b\n"
          "  wire width 2 input 4 \\c\n  wire width 2 output 5 \\y\n  wire width 2 \\t\n"
          "  cell $mux \\in\n    parameter \\WIDTH 2\n    connect \\A { \\s \\u }\n"
          "    connect \\B \\b\n    connect \\S \\u\n    connect \\Y \\t\n  end\n"
          "  cell $mux \\outer\n    parameter \\WIDTH 2\n    connect \\A \\c\n"
          "    connect \\B \\t\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    run_muxtree();
    expect_port(node_named("in"), MUX_A, "0 1"); /* u is the select of in; s is active */
    expect_port(node_named("in"), MUX_B, "b:0 b:1");
    expect_removed(0);
}

/* --- narrowing ------------------------------------------------------------------------------- */

#define NARROW_TEXT                                                                                \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\u\n  wire input 5 \\v\n  wire output 6 \\y\n  wire output 7 \\z\n"           \
    "  attribute \\src \"t.v:3.1\"\n  attribute \\keep 1\n" PMUX(                                  \
        "p", "3", "\\a", "{ \\b \\c \\a }", "{ \\u 1'0 \\v }", "\\y")                              \
        PMUX("q", "2", "\\a", "{ \\b \\c }", "{ \\u 1'0 }", "\\z") "end\n"

static void test_narrow(void) {
    build(NARROW_TEXT);
    odin3_prov_id rec = odin3_node_prov(fx.module, node_named("p"));
    run_muxtree();
    odin3_node_id wide = node_named("p");
    TEST_ASSERT_EQUAL_STRING("$pmux", type_name(wide));
    TEST_ASSERT_EQUAL_INT64(1, pint(wide, PRM_WIDTH));
    TEST_ASSERT_EQUAL_INT64(2, pint(wide, PRM_S_WIDTH));
    expect_port(wide, MUX_A, "a");
    expect_port(wide, MUX_B, "a b"); /* slice 0 (v selects a), slice 2 (u selects b) */
    expect_port(wide, MUX_S, "v u");
    expect_port(wide, MUX_Y, "y");
    odin3_node_id tall = node_named("q");
    TEST_ASSERT_EQUAL_STRING("$mux", type_name(tall));
    expect_port(tall, MUX_A, "a");
    expect_port(tall, MUX_B, "b");
    expect_port(tall, MUX_S, "u");
    expect_port(tall, MUX_Y, "z");
    expect_removed(2);
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, wide.v}, intern("keep")));
    const odin3_prov_record *got = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, wide));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, got->kind);
    TEST_ASSERT_EQUAL_UINT32(1, got->parents.count);
    TEST_ASSERT_EQUAL_UINT32(rec.v, got->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_DEBUG, "opt_muxtree: module 'm': dead port "
                                                             "2/4 on $pmux 'p'"));
}

/* A mux with an unconnected pin is left alone, and its inputs are used by a non-mux node. */
static void test_unconnected_pin(void) {
    build(NESTED_TEXT);
    odin3_node_id inner = node_named("inner");
    odin3_pinslice ps = odin3_node_port(fx.module, inner, MUX_B);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(fx.module, ps.first));
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(inner.v, node_named("inner").v);
    expect_removed(0);
}

/* A loop of muxes ends, and nothing is dead. */
static void test_loop(void) {
    build("module \\m\n  wire input 1 \\s1\n  wire input 2 \\s2\n  wire input 3 \\a\n"
          "  wire input 4 \\b\n  wire output 5 \\y1\n  wire output 6 \\y2\n" MUX(
              "m1", "\\a", "\\y2", "\\s1", "\\y1")
              MUX("m2", "\\b", "\\y1", "\\s2", "\\y2") "end\n");
    run_muxtree();
    expect_removed(0);
}

/* A mux read by one mux on both data ports: still a single user, not a root. */
static void test_same_user_twice(void) {
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\u\n  wire input 3 \\p\n"
          "  wire input 4 \\q\n  wire output 5 \\y\n  wire \\t\n" MUX(
              "in", "\\q", "\\p", "\\u", "\\t") MUX("outer", "\\t", "\\t", "\\s", "\\y") "end\n");
    run_muxtree();
    expect_removed(0);
}

/* --- budget, depth, processes ----------------------------------------------------------------- */

/* REPEAT copies of NESTED_TEXT's cells, each its own s, a, b, c, t, y. */
static void repeated_text(odin3_strbuf *text) {
    odin3_strbuf_init(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text, "module \\m\n"));
    for (int i = 0; i < REPEAT; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(text,
                                                   "  wire input %d \\s%d\n  wire input %d \\a%d\n"
                                                   "  wire input %d \\b%d\n  wire input %d \\c%d\n"
                                                   "  wire output %d \\y%d\n  wire \\t%d\n",
                                                   5 * i + 1, i, 5 * i + 2, i, 5 * i + 3, i,
                                                   5 * i + 4, i, 5 * i + 5, i, i));
    }
    for (int i = 0; i < REPEAT; i++) {
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(
                          text,
                          "  cell $mux \\inner%d\n    parameter \\WIDTH 1\n    connect \\A \\b%d\n"
                          "    connect \\B \\a%d\n    connect \\S \\s%d\n    connect \\Y \\t%d\n"
                          "  end\n  cell $mux \\outer%d\n    parameter \\WIDTH 1\n"
                          "    connect \\A \\c%d\n    connect \\B \\t%d\n    connect \\S \\s%d\n"
                          "    connect \\Y \\y%d\n  end\n",
                          i, i, i, i, i, i, i, i, i, i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text, "end\n"));
}

/* The cap stops the analysis: the warning, no change and no count. */
static void test_budget(void) {
    odin3_strbuf text;
    repeated_text(&text);
    build(text.data);
    odin3_strbuf_free(&text);
    char *before = rtlil_text();
    set_max_evals(10);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN, "opt_muxtree: module 'm': giving up after 10 evaluations"));
    expect_removed(0);
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_DEBUG, "opt_muxtree:"));
    /* the same design under the default cap: every pair loses one port */
    reset();
    repeated_text(&text);
    build(text.data);
    odin3_strbuf_free(&text);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_WARN, "opt_muxtree:"));
    expect_removed(REPEAT);
}

/* A giving-up module drops the known-bit replacements Yosys had already made (§17 row 18). */
static void test_budget_drops_bits(void) {
    const char *text =
        "module \\m\n  wire input 1 \\s\n  wire input 2 \\u\n  wire input 3 \\a\n"
        "  wire input 4 \\c\n  wire output 5 \\y\n  wire \\i\n" MUX(
            "in", "\\s", "\\a", "\\u", "\\i") MUX("outer", "\\c", "\\i", "\\s", "\\y") "end\n";
    build(text);
    set_max_evals(2);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_WARN, "opt_muxtree: module 'm': giving up after 2 evaluations"));
    expect_port(node_named("in"), MUX_A, "s");
    reset();
    build(text);
    set_max_evals(3);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_WARN, "opt_muxtree:"));
    expect_port(node_named("in"), MUX_A, "1");
}

/*
 * A chain of n muxes m[i]: A = a, B = the previous Y (b for the first), S = s[i], the last one
 * first in ID order: a recursive evaluation would need n frames of the C stack. Each mux costs one
 * evaluation, so n equal to the cap trips it.
 */
static void build_chain(uint32_t n) {
    char text[CASE_BUF];
    (void)snprintf(
        text, sizeof text,
        "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire width %u input 3 \\s\n"
        "  wire output 4 \\y\nend\n",
        (unsigned)n);
    build(text);
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$mux"), &type));
    odin3_net_id *nets = malloc(sizeof *nets * n);
    TEST_ASSERT_NOT_NULL(nets);
    for (uint32_t i = 0; i + 1U < n; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &nets[i]));
    }
    nets[n - 1U] = wbit("y", 0);
    const odin3_value param = odin3_value_int(1);
    for (uint32_t i = n; i-- > 0;) {
        const odin3_net_id data_a = wbit("a", 0);
        const odin3_net_id data_b = i == 0 ? wbit("b", 0) : nets[i - 1U];
        const odin3_net_id sel = wbit("s", i);
        const odin3_netvec ports[4] = {{&data_a, 1}, {&data_b, 1}, {&sel, 1}, {&nets[i], 1}};
        const odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, &param, 1};
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_node_create_connected(fx.module, &spec, ports, &node));
    }
    free(nets);
}

static void test_deep(void) {
    build_chain(CHAIN_LEN);
    char *before = rtlil_text();
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1,
        count_lines(ODIN3_LOG_WARN, "opt_muxtree: module 'm': giving up after 100000 evaluations"));
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    expect_removed(0);
}

static void test_deep_below_cap(void) {
    build_chain(CHAIN_LEN - 1U);
    char *before = rtlil_text();
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(0, count_prefix(ODIN3_LOG_WARN, "opt_muxtree:"));
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    expect_removed(0);
}

#define PROC_TEXT                                                                                  \
    "module \\p\n  wire input 1 \\a\n  wire output 2 \\z\n"                                        \
    "  process $p\n    sync always\n      update \\z \\a\n  end\nend\n"

static void test_skips_processes(void) {
    build(PROC_TEXT NESTED_TEXT);
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_muxtree: module 'p' has processes; skipped"));
    TEST_ASSERT_TRUE(node_gone("inner")); /* the other module is optimised */
    expect_removed(1);
    reset();
    build("module \\m\n  wire input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n"
          "  wire input 4 \\c\n  wire output 5 \\y\n  wire \\t\n  wire output 6 \\z\n" MUX(
              "inner", "\\b", "\\a", "\\s", "\\t")
              MUX("outer", "\\c", "\\t", "\\s",
                  "\\y") "  process $p\n    sync always\n      update \\z \\a\n  end\nend\n");
    char *before = rtlil_text();
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_muxtree: module 'm' has processes; skipped"));
    char *after = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    expect_removed(0);
}

/* --- the line, idempotence, determinism, arguments -------------------------------------------- */

static void test_idempotent(void) {
    build(NESTED_TEXT);
    run_muxtree();
    char *first = rtlil_text();
    run_muxtree();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_muxtree: removed 0 multiplexer ports"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.muxtree_ports);
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

static void test_determinism(void) {
    odin3_strbuf text;
    repeated_text(&text);
    build(text.data);
    run_muxtree();
    char *first = rtlil_text();
    reset();
    build(text.data);
    odin3_strbuf_free(&text);
    run_muxtree();
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

static void test_bad_arguments(void) {
    build(NESTED_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_muxtree_run(NULL, fx.res.design, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_muxtree_run(&fx.ctx, NULL, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_muxtree_run(&fx.ctx, fx.res.design, NULL, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_muxtree_run(&fx.ctx, fx.res.design, &fx.opts, NULL));
}

/* --- Yosys 0.55's own cases ------------------------------------------------------------------- */

static const char *const k_cases[] = {
    "bug2824",         "const_one",          "const_zero",   "const_x",         "pmux_const",
    "nested_same_sel", "nested_inverse_sel", "shared_ports", "shared_live",     "levels",
    "replace_known",   "replace_ctrl",       "pmux_known",   "same_user_twice", "loop",
    "wide_known",      "dead_child",         "order_a",      "order_b",
};

static void read_case(const char *name, const char *ext, char *buf) {
    char path[LINE_MAX_LEN];
    int len = snprintf(path, sizeof path, "%s/%s%s", ODIN3_MUXTREE_DATA, name, ext);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, CASE_BUF - 1U, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < CASE_BUF - 1U, path);
    buf[got] = '\0';
}

typedef struct dump {
    char line[DUMP_LINES][DUMP_LEN];
    uint32_t n;
    uint32_t removed;
} dump;

static int cmp_line(const void *lhs, const void *rhs) {
    return strcmp((const char *)lhs, (const char *)rhs);
}

static void dump_add(dump *out, const char *text) {
    TEST_ASSERT_TRUE(out->n < DUMP_LINES && strlen(text) < DUMP_LEN);
    (void)snprintf(out->line[out->n++], DUMP_LEN, "%s", text);
}

/* "cell"/"wire" lines and "removed N" of an .expect file. */
static void parse_expect(char *text, dump *out) {
    memset(out, 0, sizeof *out);
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, "cell ", 5) == 0 || strncmp(line, "wire ", 5) == 0) {
            dump_add(out, line);
        } else if (strncmp(line, "removed ", 8) == 0) {
            out->removed = (uint32_t)strtoul(line + 8, NULL, 10);
        }
    }
    qsort(out->line, out->n, DUMP_LEN, cmp_line);
}

/* The label of every net: the constant, else the smallest "wire:bit" of the wires holding it. */
static char (*net_labels(void))[LABEL_LEN] {
    const uint32_t net_end = odin3_module_net_end(fx.module);
    char(*labels)[LABEL_LEN] = calloc(net_end, LABEL_LEN);
    TEST_ASSERT_NOT_NULL(labels);
    const odin3_strtab *strtab = odin3_design_strtab(fx.res.design);
    for (uint32_t id = 1; id < odin3_module_wire_end(fx.module); id++) {
        odin3_wire_id wire = {id};
        if (!odin3_wire_live(fx.module, wire)) {
            continue;
        }
        for (uint32_t bit = 0; bit < odin3_wire_width(fx.module, wire); bit++) {
            char label[LABEL_LEN];
            (void)snprintf(label, sizeof label, "%s:%u",
                           odin3_strtab_get(strtab, odin3_wire_name(fx.module, wire)),
                           (unsigned)bit);
            char *slot = labels[odin3_wire_net(fx.module, wire, bit).v];
            if (slot[0] == '\0' || strcmp(label, slot) < 0) {
                (void)snprintf(slot, LABEL_LEN, "%s", label);
            }
        }
    }
    return labels;
}

static const char *rep_of(char (*labels)[LABEL_LEN], odin3_net_id net) {
    static const char *const k_consts[] = {"", "0", "1", "x", "z"};
    odin3_const cv = odin3_net_const_value(fx.module, net);
    if (cv != ODIN3_CONST_NONE) {
        return k_consts[cv];
    }
    TEST_ASSERT_TRUE_MESSAGE(labels[net.v][0] != '\0', "a net with no wire");
    return labels[net.v];
}

static void append_port(char (*labels)[LABEL_LEN], odin3_node_id node, uint32_t port, char *line) {
    static const char k_names[] = "ABSY";
    char head[8];
    (void)snprintf(head, sizeof head, " %c=", k_names[port]);
    (void)strcat(line, head);
    for (uint32_t bit = 0; bit < pwidth(node, port); bit++) {
        if (bit > 0) {
            (void)strcat(line, " ");
        }
        (void)strcat(line, rep_of(labels, pnet(PIN(node, port, bit))));
    }
}

static void dump_cell(char (*labels)[LABEL_LEN], odin3_node_id node, dump *out) {
    char line[DUMP_LEN];
    (void)snprintf(line, sizeof line, "cell %s WIDTH=%lld", type_name(node),
                   (long long)pint(node, PRM_WIDTH));
    if (strcmp(type_name(node), "$pmux") == 0) {
        size_t at = strlen(line);
        (void)snprintf(line + at, sizeof line - at, " S_WIDTH=%lld",
                       (long long)pint(node, PRM_S_WIDTH));
    }
    for (uint32_t port = 0; port < 4; port++) {
        append_port(labels, node, port, line);
    }
    dump_add(out, line);
}

/* The result as the .expect file spells it. */
static void dump_module(dump *out) {
    char(*labels)[LABEL_LEN] = net_labels();
    memset(out, 0, sizeof *out);
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        if (odin3_node_live(fx.module, node) && def->gran != ODIN3_GRAN_PORT &&
            strncmp(def->name, "$_CONST", 7) != 0) {
            dump_cell(labels, node, out);
        }
    }
    const odin3_strtab *strtab = odin3_design_strtab(fx.res.design);
    for (uint32_t id = 1; id < odin3_module_wire_end(fx.module); id++) {
        odin3_wire_id wire = {id};
        for (uint32_t bit = 0;
             odin3_wire_live(fx.module, wire) && bit < odin3_wire_width(fx.module, wire); bit++) {
            char own[LABEL_LEN];
            char line[DUMP_LEN];
            (void)snprintf(own, sizeof own, "%s:%u",
                           odin3_strtab_get(strtab, odin3_wire_name(fx.module, wire)),
                           (unsigned)bit);
            const char *rep = rep_of(labels, odin3_wire_net(fx.module, wire, bit));
            if (strcmp(own, rep) != 0) {
                (void)snprintf(line, sizeof line, "wire %s -> %s", own, rep);
                dump_add(out, line);
            }
        }
    }
    free(labels);
    qsort(out->line, out->n, DUMP_LEN, cmp_line);
}

static void run_yosys_case(const char *name) {
    static char text[CASE_BUF];
    static char expect[CASE_BUF];
    dump want;
    dump got;
    read_case(name, ".il", text);
    read_case(name, ".expect", expect);
    parse_expect(expect, &want);
    build(text);
    run_muxtree();
    dump_module(&got);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.removed, fx.counts.muxtree_ports, name);
    for (uint32_t i = 0; i < want.n && i < got.n; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.line[i], got.line[i], name);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.n, got.n, name);
}

static void test_yosys_cases(void) {
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        run_yosys_case(k_cases[i]);
        reset();
    }
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* Every kind of rewrite once, and a register with an init that must never change. */
#define OOM_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\s\n  wire input 2 \\u\n  wire input 3 \\v\n"                     \
    "  wire input 4 \\a\n  wire input 5 \\b\n  wire input 6 \\c\n  wire input 7 \\d\n"             \
    "  wire input 8 \\clk\n  wire output 9 \\y1\n  wire output 10 \\y2\n  wire output 11 \\y3\n"   \
    "  wire output 12 \\y4\n  wire output 13 \\y5\n  attribute \\init 1'1\n"                       \
    "  wire output 14 \\q\n  wire \\t1\n  wire \\t2\n  wire \\t6\n" MUX(                           \
        "in1", "\\b", "\\a", "\\s",                                                                \
        "\\t1") MUX("out1", "\\c", "\\t1", "\\s",                                                  \
                    "\\y1") MUX("in2", "\\s", "\\d", "\\u",                                        \
                                "\\t2") MUX("out2", "\\c", "\\t2", "\\s",                          \
                                            "\\y2") PMUX("p3", "3", "\\a", "{ \\b \\c \\d }",      \
                                                         "{ \\u 1'0 \\v }",                        \
                                                         "\\y3") PMUX("p4", "2", "\\a",            \
                                                                      "{ \\b \\c }",               \
                                                                      "{ \\u 1'0 }", "\\y4")       \
        MUX("in5", "\\b", "\\a", "\\u", "\\t6") MUX(                                               \
            "out5", "\\c", "\\t6", "1'0",                                                          \
            "\\y5") "  cell $dff \\f\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"   \
                    "    connect \\CLK \\clk\n    connect \\D \\y1\n    connect \\Q \\q\n  "       \
                    "end\nend\n"

enum { OOM_REMOVED = 6, OOM_MIN_EACH = 10, OOM_MIN_PARTIAL = 3 };

static long g_half_done, g_unchanged, g_partial;

static void count_half_done(void) {
    for (uint32_t i = 0; i < n_lines; i++) {
        g_half_done += strstr(lines[i].text, "in the middle of a rewrite") != NULL ? 1 : 0;
    }
}

/* After a failed run: the IR is valid, the register starts at 1, and a second run without failure
 * finishes the work (it needs no more than the failed run left). */
static void oom_recover(const char *before) {
    check_full();
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("q", 0)));
    char *after = rtlil_text();
    g_unchanged += strcmp(before, after) == 0 ? 1 : 0;
    g_partial += strcmp(before, after) != 0 ? 1 : 0;
    free(after);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_muxtree_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("q", 0)));
}

static void oom_run_once(long fail, bool *done) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    char *before = rtlil_text();
    n_lines = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_muxtree_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    count_half_done();
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    if (st == ODIN3_OK) {
        check_full();
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("q", 0)));
    } else {
        oom_recover(before);
    }
    free(before);
}

/* Every allocation point in turn fails once; the sweep ends when a run needs none to fail. Points
 * before the first rewrite leave the IR as it was, later ones leave some rewrites done. */
static void test_muxtree_oom(void) {
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    printf("opt_muxtree OOM: %ld failing allocation points (%ld before any rewrite, %ld after "
           "some), %ld in the middle of a kit merge\n",
           failures, g_unchanged, g_partial, g_half_done);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(OOM_REMOVED, fx.counts.muxtree_ports);
    /* not vacuous: failures before and after the first rewrite, and nothing else */
    const bool both = g_unchanged > OOM_MIN_EACH && g_partial > OOM_MIN_PARTIAL;
    TEST_ASSERT_TRUE(both && g_unchanged + g_partial == failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nested_same_sel);
    RUN_TEST(test_nested_inverse_sel);
    RUN_TEST(test_const_s);
    RUN_TEST(test_pmux_const);
    RUN_TEST(test_root_fanout);
    RUN_TEST(test_root_fanout_live);
    RUN_TEST(test_keep_wire);
    RUN_TEST(test_non_mux_reader);
    RUN_TEST(test_dead_child);
    RUN_TEST(test_unnamed_label);
    RUN_TEST(test_replace_known);
    RUN_TEST(test_replace_ctrl);
    RUN_TEST(test_replace_dup_select);
    RUN_TEST(test_replace_wide);
    RUN_TEST(test_narrow);
    RUN_TEST(test_unconnected_pin);
    RUN_TEST(test_loop);
    RUN_TEST(test_same_user_twice);
    RUN_TEST(test_budget);
    RUN_TEST(test_budget_drops_bits);
    RUN_TEST(test_deep);
    RUN_TEST(test_deep_below_cap);
    RUN_TEST(test_skips_processes);
    RUN_TEST(test_idempotent);
    RUN_TEST(test_determinism);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_yosys_cases);
    RUN_TEST(test_muxtree_oom);
    return UNITY_END();
}
