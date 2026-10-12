/*
 * test_opt_reduce.c — opt_reduce (2F spec OP-8, §8, §20.5; plan Task 9): the $reduce_or/$reduce_and
 * worker (sort and de-duplicate, absorbing and neutral constants, inlining of same-type children),
 * opt_mux_bits under -fine and for write enables, opt_pmux (slices equal to A, hash grouping in
 * first-occurrence order), the INFO line, names, attributes and provenance of the rebuilt nodes,
 * Yosys 0.55's own cases and out of memory.
 *
 * Yosys parity (0.55, the VTR build): each tests/unit/data/opt/reduce/X.il is run as
 * `yosys -p "read_rtlil X.il; opt_reduce [-fine]; write_rtlil"`; X.expect keeps, per cell of the
 * result, its type, the parameters A_WIDTH, WIDTH and S_WIDTH and, for a reduction with a constant
 * input, that constant (recorded in the header of each .expect). Cells are compared as a multiset
 * of lines: Odin III sorts inputs by net ID where Yosys sorts by wire name, so the order of the
 * inputs is checked by the hand-built tests only.
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
    LINES_MAX = 64,
    OOM_LIMIT = 20000,
    CASE_BUF = 8192,
    MAX_CELLS = 32,
    WIDE_PAIRS = 2048,
    SPEC_MAX = 128,
    CHAIN_LEN = 50000
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
    odin3_opt_reduce_test_set_key_hash(NULL);
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

static void run_reduce(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_reduce_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

/* The INFO line of the last run and the counter agree on want. */
static void expect_changes(uint32_t want) {
    char text[LINE_MAX_LEN];
    int len = snprintf(text, sizeof text, "opt_reduce: performed %u changes", (unsigned)want);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, text));
    TEST_ASSERT_EQUAL_UINT32(want, fx.counts.reduce_changes);
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

/* The live nodes whose type is called type. */
static uint32_t count_type(const char *type) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        hits += odin3_node_live(fx.module, node) && strcmp(type_name(node), type) == 0 ? 1U : 0U;
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

/* Pin `bit` of port `port` of a node. */
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

enum { RED_A = 0, RED_Y = 1, MUX_A = 0, MUX_B = 1, MUX_S = 2, MUX_Y = 3 };
enum { PRM_RED_A_WIDTH = 1, PRM_MUX_WIDTH = 0, PRM_PMUX_S_WIDTH = 1 };

/* A reduction named name has A_WIDTH inputs spec, and its Y is wire y. */
static void expect_reduce(const char *name, uint32_t width, const char *spec) {
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node) && odin3_node_live(fx.module, node), name);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(width, pint(node, PRM_RED_A_WIDTH), name);
    expect_port(node, RED_A, spec);
}

#define HEAD3                                                                                      \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire output 4 \\y\n"

#define RED(type, name, w, a, y)                                                                   \
    "  cell " type " \\" name "\n    parameter \\A_WIDTH " w "\n    connect \\A { " a              \
    " }\n    connect \\Y " y "\n  end\n"

/* --- $reduce_or, $reduce_and ------------------------------------------------------------------ */

static void test_reduce_or_dedup(void) {
    build(HEAD3 RED("$reduce_or", "r", "4", "1'0 \\b \\a \\b", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 2, "a b");
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$reduce_or"));
    TEST_ASSERT_TRUE(is_net(pnet(PIN(nth_of("$reduce_or", 0), RED_Y, 0)), "y"));
    expect_changes(1);
}

static void test_reduce_or_one(void) {
    build(HEAD3 RED("$reduce_or", "r", "2", "1'1 \\a", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 1, "1");
    expect_changes(1);
}

static void test_reduce_and_zero(void) {
    build(HEAD3 RED("$reduce_and", "r", "3", "\\a 1'0 \\b", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 1, "0");
    expect_changes(1);
}

/* An absorbing constant of the other kind is neutral, and x and z stay inputs. */
static void test_reduce_neutral(void) {
    build(HEAD3 RED("$reduce_or", "r", "3", "1'x \\a 1'0", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 2, "x a");
    expect_changes(1);
    reset();
    build(HEAD3 RED("$reduce_and", "r", "3", "1'1 \\a 1'1", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 1, "a");
    expect_changes(1);
}

static void test_reduce_empty(void) {
    build(HEAD3 RED("$reduce_and", "r", "2", "1'1 1'1", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 1, "1");
    expect_changes(1);
    reset();
    build(HEAD3 RED("$reduce_or", "r", "2", "1'0 1'0", "\\y") "end\n");
    run_reduce();
    expect_reduce("r", 1, "0");
}

/* A reduction that is already sorted, unique and constant-free is left alone, not rebuilt. */
static void test_reduce_unchanged(void) {
    build(HEAD3 RED("$reduce_or", "r", "2", "\\b \\a", "\\y") "end\n");
    odin3_node_id before = node_named("r");
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(before.v, node_named("r").v);
    expect_changes(0);
}

#define INLINE_TEXT                                                                                \
    HEAD3 "  wire \\t\n  wire \\u\n" RED("$reduce_or", "r1", "2", "\\b \\a", "\\t")                \
        RED("$reduce_or", "r2", "2", "\\c \\t", "\\u")                                             \
            RED("$reduce_or", "r3", "2", "\\u \\a", "\\y") "end\n"

static void test_reduce_inline(void) {
    build(HEAD3 "  wire \\t\n" RED("$reduce_or", "r1", "2", "\\b \\a", "\\t")
              RED("$reduce_or", "r2", "2", "\\c \\t", "\\y") "end\n");
    run_reduce();
    expect_reduce("r2", 3, "a b c");
    expect_reduce("r1", 2, "a b"); /* stays for opt_clean */
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$reduce_or"));
    expect_changes(1);
}

/* A chain is processed child first: r3 gets a, b, c however the IDs run. */
static void test_reduce_inline_chain(void) {
    build(INLINE_TEXT);
    run_reduce();
    expect_reduce("r3", 3, "a b c");
    expect_reduce("r2", 3, "a b c");
    expect_reduce("r1", 2, "a b");
    expect_changes(2);
    reset();
    /* the parent first in ID order: the same result */
    build(HEAD3 "  wire \\t\n  wire \\u\n" RED("$reduce_or", "r3", "2", "\\u \\a", "\\y")
              RED("$reduce_or", "r2", "2", "\\c \\t", "\\u")
                  RED("$reduce_or", "r1", "2", "\\b \\a", "\\t") "end\n");
    run_reduce();
    expect_reduce("r3", 3, "a b c");
    expect_reduce("r2", 3, "a b c");
    expect_changes(2);
}

/* The child feeds several readers: it is inlined whatever its fanout. */
static void test_reduce_inline_fanout(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"
          "  wire output 4 \\y\n  wire output 5 \\z\n  wire \\t\n" RED("$reduce_or", "r1", "2",
                                                                       "\\b \\a", "\\t")
              RED("$reduce_or", "r2", "2", "\\c \\t", "\\y")
                  RED("$reduce_or", "r3", "2", "\\t \\c", "\\z") "end\n");
    run_reduce();
    expect_reduce("r2", 3, "a b c");
    expect_reduce("r3", 3, "a b c");
    expect_changes(2);
}

/* Reductions of the other type are not inlined; an AND under an OR stays a leaf. */
static void test_reduce_inline_other_type(void) {
    build(HEAD3 "  wire \\t\n" RED("$reduce_and", "r1", "2", "\\b \\a", "\\t")
              RED("$reduce_or", "r2", "2", "\\c \\t", "\\y") "end\n");
    run_reduce();
    expect_reduce("r2", 2, "t c");
    expect_changes(0);
}

/* The AND phase reads nets driven by $reduce_or cells that the OR phase has just rebuilt (new IDs).
 */
static void test_reduce_cross_type(void) {
    build(HEAD3 "  wire \\t\n  wire \\u\n" RED("$reduce_or", "r1", "2", "\\a \\a", "\\t")
              RED("$reduce_or", "r2", "2", "\\b \\b", "\\u")
                  RED("$reduce_and", "r3", "3", "\\c \\u \\t", "\\y") "end\n");
    run_reduce();
    expect_reduce("r1", 1, "a");
    expect_reduce("r2", 1, "b");
    expect_reduce("r3", 3, "t u c"); /* an OR under an AND stays a leaf */
    expect_changes(2);
}

/* A child that absorbs: r1 = or(a, 1) is 1, r2 = or(t, c) takes that 1 (round 1), then absorbs (2).
 */
static void test_reduce_child_constant(void) {
    build(HEAD3 "  wire \\t\n" RED("$reduce_or", "r1", "2", "1'1 \\a", "\\t")
              RED("$reduce_or", "r2", "2", "\\c \\t", "\\y") "end\n");
    run_reduce();
    expect_reduce("r1", 1, "1");
    expect_reduce("r2", 1, "1");
    expect_changes(3); /* as Yosys 0.55: data/ror_const_child.il */
}

/* Reductions that feed each other still end: Yosys gives both three inputs. */
static void test_reduce_loop(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\x\n"
          "  wire output 4 \\y\n" RED("$reduce_or", "rx", "2", "\\y \\a", "\\x")
              RED("$reduce_or", "ry", "2", "\\x \\b", "\\y") "end\n");
    run_reduce();
    TEST_ASSERT_EQUAL_INT64(3, pint(node_named("rx"), PRM_RED_A_WIDTH));
    TEST_ASSERT_EQUAL_INT64(3, pint(node_named("ry"), PRM_RED_A_WIDTH));
    expect_changes(2);
}

/* Y_WIDTH 2: Y[1] of the child is constant 0, so the parent that reads it gets a 0 (Yosys
 * inserts S0, which the next round drops). */
static void test_reduce_ywidth(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"
          "  wire width 2 \\t\n  wire output 4 \\y\n"
          "  cell $reduce_or \\r1\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A { \\a \\b }\n    connect \\Y \\t\n  end\n"
          "  cell $reduce_or \\r2\n    parameter \\A_WIDTH 2\n"
          "    connect \\A { \\t [1] \\c }\n    connect \\Y \\y\n  end\nend\n");
    run_reduce();
    expect_reduce("r1", 2, "b a"); /* unique: not rebuilt, so not sorted (and not counted) */
    expect_reduce("r2", 1, "c");
    expect_changes(2);
}

/* An input pin with no net: the cell is left alone. */
static void test_reduce_unconnected_pin(void) {
    build(HEAD3 RED("$reduce_or", "r", "4", "1'0 \\b \\a \\b", "\\y") "end\n");
    odin3_node_id node = node_named("r");
    odin3_pinslice ps = odin3_node_port(fx.module, node, RED_A);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_disconnect(fx.module, (odin3_pin_id){ps.first.v + 1}));
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("r").v); /* left alone */
    expect_changes(0);
}

/* One $reduce_or with A = in[0..2) and Y = out, made through the IR (pb_build is too slow for
 * this). */
static void add_or2(odin3_celltype_id type, const odin3_net_id in[2], odin3_net_id out) {
    const odin3_value params[3] = {odin3_value_int(0), odin3_value_int(2), odin3_value_int(1)};
    const odin3_netvec ports[2] = {{in, 2}, {&out, 1}};
    const odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, params, 3};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(fx.module, &spec, ports, &node));
}

/*
 * A chain of CHAIN_LEN reductions r[i] = or(r[i - 1], a), r[0] = or(b, a), the last one first in ID
 * order: the visit starts at the deepest parent, so a recursive visit would need CHAIN_LEN frames
 * of the C stack. Here it is an explicit stack, and every r[i] ends with the inputs {a, b}.
 */
static void test_reduce_deep_chain(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\nend\n");
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$reduce_or"), &type));
    odin3_net_id *nets = malloc(sizeof *nets * CHAIN_LEN);
    TEST_ASSERT_NOT_NULL(nets);
    for (uint32_t i = 0; i < CHAIN_LEN; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &nets[i]));
    }
    for (uint32_t i = CHAIN_LEN; i-- > 0;) {
        const odin3_net_id in[2] = {i == 0 ? wbit("b", 0) : nets[i - 1], wbit("a", 0)};
        add_or2(type, in, nets[i]);
    }
    const odin3_net_id last = nets[CHAIN_LEN - 1];
    free(nets);
    run_reduce();
    odin3_node_id tail = odin3_pin_node(fx.module, odin3_net_driver(fx.module, last));
    expect_port(tail, RED_A, "a b");
    TEST_ASSERT_EQUAL_INT64(2, pint(tail, PRM_RED_A_WIDTH));
    expect_changes(CHAIN_LEN - 1U);
}

/* --- $pmux, $mux ------------------------------------------------------------------------------ */

#define PMUX3(b, s)                                                                                \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\s0\n  wire input 5 \\s1\n  wire input 6 \\s2\n  wire output 7 \\y\n"         \
    "  cell $pmux \\p\n    parameter \\S_WIDTH 3\n    connect \\A \\a\n    connect \\B { " b       \
    " }\n    connect \\S { " s " }\n    connect \\Y \\y\n  end\nend\n"

static void test_pmux_slice_eq_a(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s0\n"
          "  wire input 4 \\s1\n  wire output 5 \\y\n"
          "  attribute \\keep 1\n  cell $pmux \\p\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"
          "    connect \\B { \\b \\a }\n    connect \\S { \\s1 \\s0 }\n    connect \\Y \\y\n  end\n"
          "end\n");
    run_reduce();
    odin3_node_id mux = node_named("p");
    TEST_ASSERT_TRUE(odin3_node_valid(mux));
    TEST_ASSERT_EQUAL_STRING("$mux", type_name(mux));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    expect_port(mux, MUX_A, "a");
    expect_port(mux, MUX_B, "b");
    expect_port(mux, MUX_S, "s1");
    expect_port(mux, MUX_Y, "y");
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, mux.v}, intern("keep")));
    expect_changes(1);
}

static void test_pmux_equal_slices(void) {
    build(PMUX3("\\b \\c \\b", "\\s2 \\s1 \\s0"));
    run_reduce();
    odin3_node_id pmux = nth_of("$pmux", 0);
    TEST_ASSERT_EQUAL_INT64(2, pint(pmux, PRM_PMUX_S_WIDTH));
    expect_port(pmux, MUX_A, "a");
    expect_port(pmux, MUX_B, "b c"); /* LSB first: slice 0 is b */
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$reduce_or"));
    odin3_node_id ror = nth_of("$reduce_or", 0);
    expect_port(ror, RED_A, "s0 s2");
    TEST_ASSERT_EQUAL_INT64(2, pint(ror, PRM_RED_A_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(pnet(PIN(ror, RED_Y, 0)).v, pnet(PIN(pmux, MUX_S, 0)).v);
    TEST_ASSERT_TRUE(is_net(pnet(PIN(pmux, MUX_S, 1)), "s1"));
    expect_port(pmux, MUX_Y, "y");
    expect_changes(1); /* the new $reduce_or is already sorted and unique */
}

/* The groups come in first-occurrence order, as Yosys's does (data/pmux_groups.il, g4). */
static void test_pmux_group_order(void) {
    build(
        "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"
        "  wire input 4 \\d\n  wire input 5 \\e\n  wire width 6 input 6 \\s\n  wire output 7 \\y\n"
        "  cell $pmux \\p\n    parameter \\S_WIDTH 6\n    connect \\A \\a\n"
        "    connect \\B { \\b \\c \\d \\e \\d \\c }\n    connect \\S \\s\n    connect \\Y \\y\n"
        "  end\nend\n");
    run_reduce();
    odin3_node_id pmux = nth_of("$pmux", 0);
    TEST_ASSERT_EQUAL_INT64(4, pint(pmux, PRM_PMUX_S_WIDTH));
    expect_port(pmux, MUX_B, "c d e b");
    expect_port(nth_of("$reduce_or", 0), RED_A, "s:0 s:4");
    expect_port(nth_of("$reduce_or", 1), RED_A, "s:1 s:3");
    TEST_ASSERT_EQUAL_UINT32(pnet(PIN(nth_of("$reduce_or", 0), RED_Y, 0)).v,
                             pnet(PIN(pmux, MUX_S, 0)).v);
    TEST_ASSERT_EQUAL_UINT32(pnet(PIN(nth_of("$reduce_or", 1), RED_Y, 0)).v,
                             pnet(PIN(pmux, MUX_S, 1)).v);
    TEST_ASSERT_TRUE(is_net(pnet(PIN(pmux, MUX_S, 2)), "s:2"));
    TEST_ASSERT_TRUE(is_net(pnet(PIN(pmux, MUX_S, 3)), "s:5"));
    expect_changes(1);
}

static void test_pmux_all_a(void) {
    build(PMUX3("\\a \\a \\a", "\\s2 \\s1 \\s0"));
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mux"));
    TEST_ASSERT_EQUAL_UINT32(wbit("a", 0).v, wbit("y", 0).v);
    expect_changes(1);
}

/* A $mux whose B is A is wiring; a $pmux with S_WIDTH 1 becomes a $mux whatever B is. */
static void test_mux_forms(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s\n"
          "  wire output 4 \\y\n  wire output 5 \\z\n"
          "  cell $mux \\m1\n    connect \\A \\a\n    connect \\B \\a\n    connect \\S \\s\n"
          "    connect \\Y \\y\n  end\n"
          "  cell $pmux \\p1\n    parameter \\S_WIDTH 1\n    connect \\A \\a\n"
          "    connect \\B \\b\n    connect \\S \\s\n    connect \\Y \\z\n  end\nend\n");
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(wbit("a", 0).v, wbit("y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
    odin3_node_id mux = nth_of("$mux", 0);
    expect_port(mux, MUX_B, "b");
    expect_port(mux, MUX_Y, "z");
    expect_changes(2);
    reset();
    /* a plain $mux with B != A has nothing to do */
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s\n"
          "  wire output 4 \\y\n  cell $mux \\m1\n    connect \\A \\a\n    connect \\B \\b\n"
          "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    run_reduce();
    expect_changes(0);
}

/* --- opt_mux_bits ----------------------------------------------------------------------------- */

#define MUXB(attr)                                                                                 \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s\n"                     \
    "  wire width 2 output 4 \\y\n  attribute \\src \"t.v:3.1\"\n" attr                            \
    "  cell $mux \\m\n    parameter \\WIDTH 2\n    connect \\A { \\a \\a }\n"                      \
    "    connect \\B { \\b \\b }\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n"

static void test_mux_bits_fine(void) {
    build(MUXB("  attribute \\keep 1\n"));
    fx.opts.fine = true;
    run_reduce();
    odin3_node_id mux = node_named("m");
    TEST_ASSERT_TRUE(odin3_node_valid(mux) && odin3_node_live(fx.module, mux));
    TEST_ASSERT_EQUAL_INT64(1, pint(mux, PRM_MUX_WIDTH));
    expect_port(mux, MUX_A, "a");
    expect_port(mux, MUX_B, "b");
    expect_port(mux, MUX_S, "s");
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, wbit("y", 1).v); /* Y feeds both bits */
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, pnet(PIN(mux, MUX_Y, 0)).v);
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, mux.v}, intern("keep")));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
    expect_changes(1);
}

/* Without -fine and without a write enable the mux is left alone. */
static void test_mux_bits_nofine(void) {
    build(MUXB(""));
    odin3_node_id before = node_named("m");
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(before.v, node_named("m").v);
    TEST_ASSERT_TRUE(wbit("y", 0).v != wbit("y", 1).v);
    expect_changes(0);
}

/* Bit 1 has A = B: it is wired to A; bit 0 stays; unequal tuples are not shared. */
static void test_mux_bits_same_bit(void) {
    build(
        "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"
        "  wire input 4 \\s\n  wire width 3 output 5 \\y\n"
        "  cell $mux \\m\n    parameter \\WIDTH 3\n    connect \\A { \\c \\b \\a }\n"
        "    connect \\B { \\c \\c \\b }\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    fx.opts.fine = true;
    run_reduce();
    odin3_node_id mux = node_named("m");
    TEST_ASSERT_EQUAL_INT64(2, pint(mux, PRM_MUX_WIDTH));
    expect_port(mux, MUX_A, "a b");
    expect_port(mux, MUX_B, "b c");
    TEST_ASSERT_EQUAL_UINT32(wbit("c", 0).v, wbit("y", 2).v); /* A = B = c */
    TEST_ASSERT_TRUE(is_net(pnet(PIN(mux, MUX_Y, 0)), "y:0"));
    TEST_ASSERT_TRUE(is_net(pnet(PIN(mux, MUX_Y, 1)), "y:1"));
    expect_changes(1);
}

/* Every bit the same on every input: the cell goes, Y = A. */
static void test_mux_bits_all_same(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s\n"
          "  wire width 2 output 4 \\y\n"
          "  cell $mux \\m\n    parameter \\WIDTH 2\n    connect \\A { \\b \\a }\n"
          "    connect \\B { \\b \\a }\n    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n");
    fx.opts.fine = true;
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mux"));
    TEST_ASSERT_EQUAL_UINT32(wbit("a", 0).v, wbit("y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wbit("b", 0).v, wbit("y", 1).v);
    expect_changes(1);
}

/* A $pmux narrows too, and opt_pmux then runs on the narrowed cell. */
static void test_mux_bits_pmux(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s0\n"
          "  wire input 4 \\s1\n  wire width 2 output 5 \\y\n"
          "  cell $pmux \\p\n    parameter \\WIDTH 2\n    parameter \\S_WIDTH 2\n"
          "    connect \\A { \\a \\a }\n    connect \\B { \\b \\b \\a \\a }\n"
          "    connect \\S { \\s1 \\s0 }\n    connect \\Y \\y\n  end\nend\n");
    fx.opts.fine = true;
    run_reduce();
    /* bits merge (1 change), then the slice equal to A goes: a $mux (1 change) */
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    odin3_node_id mux = nth_of("$mux", 0);
    TEST_ASSERT_EQUAL_INT64(1, pint(mux, PRM_MUX_WIDTH));
    expect_port(mux, MUX_A, "a");
    expect_port(mux, MUX_B, "b");
    expect_port(mux, MUX_S, "s1"); /* the slice that was equal to A (select s0) went */
    TEST_ASSERT_EQUAL_UINT32(wbit("y", 0).v, wbit("y", 1).v);
    expect_changes(2);
}

#define MEM_CELL(en)                                                                               \
    "  cell $mem \\mem\n    parameter \\MEMID \"$mem1\"\n    parameter \\SIZE 2\n"                 \
    "    parameter \\OFFSET 0\n    parameter \\ABITS 1\n    parameter \\WIDTH 2\n"                 \
    "    parameter \\INIT 4'xxxx\n    parameter \\RD_PORTS 1\n"                                    \
    "    parameter \\RD_CLK_ENABLE 1'0\n    parameter \\RD_CLK_POLARITY 1'0\n"                     \
    "    parameter \\RD_TRANSPARENT 1'0\n    parameter \\WR_PORTS 1\n"                             \
    "    parameter \\WR_CLK_ENABLE 1'1\n    parameter \\WR_CLK_POLARITY 1'1\n"                     \
    "    connect \\RD_CLK 1'0\n    connect \\RD_EN 1'1\n    connect \\RD_ADDR \\ad\n"              \
    "    connect \\RD_DATA \\q\n    connect \\WR_CLK \\clk\n    connect \\WR_EN " en "\n"          \
    "    connect \\WR_ADDR \\ad\n    connect \\WR_DATA \\d\n  end\n"

#define MEM_HEAD                                                                                   \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\s\n  wire input 5 \\t\n  wire input 6 \\clk\n  wire input 7 \\ad\n"          \
    "  wire width 2 input 8 \\d\n  wire width 2 output 9 \\q\n  wire width 2 \\w\n"                \
    "  wire width 2 \\en\n  wire width 2 \\other\n"

#define MUX2(name, a, b, sel, y)                                                                   \
    "  cell $mux \\" name "\n    parameter \\WIDTH 2\n    connect \\A " a "\n    connect \\B " b   \
    "\n    connect \\S " sel "\n    connect \\Y " y "\n  end\n"

/* A mux on a write enable is narrowed without -fine; one that is not on a write enable is not. */
static void test_mux_bits_mem_wren(void) {
    build(MEM_HEAD MUX2("m", "{ \\a \\a }", "{ \\b \\b }", "\\s", "\\en") MUX2(
        "other", "{ \\a \\a }", "{ \\b \\b }", "\\s", "\\other") MEM_CELL("\\en") "end\n");
    run_reduce();
    TEST_ASSERT_EQUAL_INT64(1, pint(node_named("m"), PRM_MUX_WIDTH));
    TEST_ASSERT_EQUAL_INT64(2, pint(node_named("other"), PRM_MUX_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(wbit("en", 0).v, wbit("en", 1).v);
    expect_changes(1);
}

/* The write-enable set follows A and B of the muxes on it. */
static void test_mux_bits_wren_chain(void) {
    build(MEM_HEAD MUX2("m1", "{ \\a \\a }", "{ \\b \\b }", "\\s", "\\w")
              MUX2("m2", "\\w", "{ \\c \\c }", "\\t", "\\en") MEM_CELL("\\en") "end\n");
    run_reduce();
    TEST_ASSERT_EQUAL_INT64(1, pint(node_named("m1"), PRM_MUX_WIDTH));
    TEST_ASSERT_EQUAL_INT64(1, pint(node_named("m2"), PRM_MUX_WIDTH));
    expect_changes(2);
}

/* And a register whose Q is on the write enable adds its D. */
static void test_mux_bits_wren_dff(void) {
    build(MEM_HEAD MUX2(
        "m", "{ \\a \\a }", "{ \\b \\b }", "\\s",
        "\\w") "  cell $dff \\f\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 2\n"
               "    connect \\CLK \\clk\n    connect \\D \\w\n    connect \\Q \\en\n  "
               "end\n" MEM_CELL("\\en") "end\n");
    run_reduce();
    TEST_ASSERT_EQUAL_INT64(1, pint(node_named("m"), PRM_MUX_WIDTH));
    expect_changes(1);
}

/* --- wide $pmux ------------------------------------------------------------------------------- */

static void wide_text(odin3_strbuf *text) {
    odin3_strbuf_init(text);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(text,
                             "module \\m\n  wire input 1 \\a\n  wire width %d input 2 \\b\n"
                             "  wire width %d input 3 \\s\n  wire output 4 \\y\n"
                             "  cell $pmux \\p\n    parameter \\S_WIDTH %d\n    connect \\A \\a\n"
                             "    connect \\B {",
                             WIDE_PAIRS, 2 * WIDE_PAIRS, 2 * WIDE_PAIRS));
    for (int slice = 2 * WIDE_PAIRS - 1; slice >= 0; slice--) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(text, " \\b [%d]", slice % WIDE_PAIRS));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(text, " }\n    connect \\S \\s\n"
                                                     "    connect \\Y \\y\n  end\nend\n"));
}

static void test_wide_pmux(void) {
    odin3_strbuf text;
    wide_text(&text);
    build(text.data);
    odin3_strbuf_free(&text);
    run_reduce();
    odin3_node_id pmux = nth_of("$pmux", 0);
    TEST_ASSERT_EQUAL_INT64(WIDE_PAIRS, pint(pmux, PRM_PMUX_S_WIDTH));
    TEST_ASSERT_EQUAL_UINT32(WIDE_PAIRS, count_type("$reduce_or"));
    for (uint32_t grp = 0; grp < WIDE_PAIRS; grp++) {
        TEST_ASSERT_EQUAL_UINT32(wbit("b", grp).v, pnet(PIN(pmux, MUX_B, grp)).v);
        odin3_pin_id drv = odin3_net_driver(fx.module, pnet(PIN(pmux, MUX_S, grp)));
        TEST_ASSERT_TRUE(odin3_pin_valid(drv));
        odin3_node_id ror = odin3_pin_node(fx.module, drv);
        TEST_ASSERT_EQUAL_STRING("$reduce_or", type_name(ror));
        TEST_ASSERT_EQUAL_UINT32(wbit("s", grp).v, pnet(PIN(ror, RED_A, 0)).v);
        TEST_ASSERT_EQUAL_UINT32(wbit("s", grp + WIDE_PAIRS).v, pnet(PIN(ror, RED_A, 1)).v);
    }
    expect_changes(1);
}

/* --- hash collisions -------------------------------------------------------------------------- */

static uint64_t const_hash(const void *key, size_t len) {
    (void)key;
    (void)len;
    return 0;
}

static void test_collisions(void) {
    odin3_opt_reduce_test_set_key_hash(const_hash);
    test_pmux_group_order();
    reset();
    odin3_opt_reduce_test_set_key_hash(const_hash);
    test_pmux_equal_slices();
    reset();
    odin3_opt_reduce_test_set_key_hash(const_hash);
    test_mux_bits_same_bit();
    reset();
    odin3_opt_reduce_test_set_key_hash(const_hash);
    test_mux_bits_pmux();
    reset();
    /* 4,096 slices: above the direct scan, so every group sits on one chain of 2,048 */
    odin3_opt_reduce_test_set_key_hash(const_hash);
    test_wide_pmux();
}

/* --- the line, idempotence, determinism ------------------------------------------------------- */

#define LINES_TEXT                                                                                 \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\s0\n  wire input 5 \\s1\n  wire input 6 \\s2\n  wire output 7 \\y1\n"        \
    "  wire output 8 \\y2\n  wire output 9 \\y3\n" RED("$reduce_or", "r1", "4", "1'0 \\b \\a \\b", \
                                                       "\\y1")                                     \
        RED("$reduce_and", "r2", "2", "\\a 1'0",                                                   \
            "\\y2") "  cell $pmux \\p\n    parameter \\S_WIDTH 3\n    connect \\A \\a\n"           \
                    "    connect \\B { \\b \\c \\b }\n    connect \\S { \\s2 \\s1 \\s0 }\n"        \
                    "    connect \\Y \\y3\n  end\nend\n"

static void test_lines(void) {
    build(LINES_TEXT);
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt_reduce: performed 3 changes"));
    TEST_ASSERT_EQUAL_UINT32(3, fx.counts.reduce_changes);
    /* the second call finds nothing */
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &before));
    run_reduce();
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, "opt_reduce: performed 0 changes"));
    TEST_ASSERT_EQUAL_UINT32(3, fx.counts.reduce_changes);
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

/* No DEBUG line is logged: opt_reduce has only its INFO line (spec §16). */
static void test_no_debug_lines(void) {
    build(LINES_TEXT);
    run_reduce();
    for (uint32_t i = 0; i < n_lines; i++) {
        TEST_ASSERT_TRUE(lines[i].level != ODIN3_LOG_DEBUG);
    }
}

static void test_determinism(void) {
    odin3_strbuf first;
    odin3_strbuf_init(&first);
    fx.opts.fine = true;
    build(LINES_TEXT);
    run_reduce();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &first));
    reset();
    fx.opts.fine = true;
    build(LINES_TEXT);
    run_reduce();
    odin3_strbuf second;
    odin3_strbuf_init(&second);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &second));
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

static void test_bad_arguments(void) {
    build(LINES_TEXT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_reduce_run(NULL, fx.res.design, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_reduce_run(&fx.ctx, NULL, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_reduce_run(&fx.ctx, fx.res.design, NULL, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_reduce_run(&fx.ctx, fx.res.design, &fx.opts, NULL));
}

/* --- provenance ------------------------------------------------------------------------------- */

#define PROV_TEXT                                                                                  \
    HEAD3 "  attribute \\src \"t.v:3.1\"\n  attribute \\keep 1\n" RED(                             \
        "$reduce_or", "r", "4", "1'0 \\b \\a \\b", "\\y") "end\n"

/* node has the attribute `keep` (the rebuild copies attributes). */
static void expect_keep(odin3_node_id node) {
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern("keep")));
}

/* node's record is DERIVED from the single record parent. */
static void expect_derived_from(odin3_node_id node, odin3_prov_id parent) {
    const odin3_prov_record *got = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, node));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, got->kind);
    TEST_ASSERT_EQUAL_UINT32(1, got->parents.count);
    TEST_ASSERT_EQUAL_UINT32(parent.v, got->parents.ids[0].v);
}

static void test_provenance(void) {
    build(PROV_TEXT);
    odin3_node_id old = node_named("r");
    odin3_prov_id rec = odin3_node_prov(fx.module, old);
    TEST_ASSERT_TRUE(rec.v != 0);
    run_reduce();
    odin3_node_id fresh = node_named("r");
    TEST_ASSERT_TRUE(fresh.v != old.v); /* rebuilt */
    expect_derived_from(fresh, rec);
    expect_keep(fresh);
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
}

/* The $reduce_or an opt_pmux makes and the $mux it leaves both derive from the $pmux's record. */
static void test_provenance_pmux(void) {
    build(PMUX3("\\b \\c \\b", "\\s2 \\s1 \\s0"));
    odin3_prov_id rec = odin3_node_prov(fx.module, nth_of("$pmux", 0));
    run_reduce();
    expect_derived_from(nth_of("$reduce_or", 0), rec);
    expect_derived_from(nth_of("$pmux", 0), rec);
}

/* --- Yosys 0.55's own cases ------------------------------------------------------------------- */

static const char *const k_cases[] = {
    "andor",          "ror_dedup",    "ror_inline", "consts",          "pmux_groups",
    "pmux_to_mux",    "pmux_all_a",   "mux_fine",   "mux_nofine",      "mux_wren",
    "mux_wren_chain", "mux_wren_dff", "ror_loop",   "ror_const_child", "ror_ywidth",
};

/* Reads DATA/<name><ext> into a NUL-terminated buffer (CASE_BUF bytes). */
static void read_case(const char *name, const char *ext, char *buf) {
    char path[LINE_MAX_LEN];
    int len = snprintf(path, sizeof path, "%s/%s%s", ODIN3_REDUCE_DATA, name, ext);
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

enum { ITEM_LEN = 96 };

typedef struct summary {
    char cells[MAX_CELLS][ITEM_LEN];
    uint32_t n_cells;
} summary;

/* The .expect file: "cell ..." lines and a "# flags: ..." header. */
static void parse_expect(char *text, summary *out, bool *fine) {
    memset(out, 0, sizeof *out);
    *fine = strstr(text, "# flags: -fine") != NULL;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, "cell ", 5) == 0) {
            TEST_ASSERT_TRUE(out->n_cells < MAX_CELLS && strlen(line) < ITEM_LEN);
            (void)snprintf(out->cells[out->n_cells++], ITEM_LEN, "%s", line);
        }
    }
    qsort(out->cells, out->n_cells, ITEM_LEN, cmp_str);
}

/* "A=<digits>" when every input of the reduction is a constant, MSB first like Yosys's. */
static void append_const_a(odin3_node_id node, char *line) {
    odin3_pinslice ps = odin3_node_port(fx.module, node, RED_A);
    char digits[SPEC_MAX] = "";
    for (uint32_t bit = ps.count; bit-- > 0;) {
        odin3_const cv = odin3_net_const_value(fx.module, pnet(PIN(node, RED_A, bit)));
        if (cv == ODIN3_CONST_NONE) {
            return;
        }
        digits[strlen(digits)] = "01xz"[cv - ODIN3_CONST_0];
    }
    (void)strcat(line, " A=");
    (void)strcat(line, digits);
}

static void describe_cell(odin3_node_id node, char *line) {
    static const char *const k_watch[] = {"A_WIDTH", "WIDTH", "S_WIDTH"};
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    (void)snprintf(line, ITEM_LEN, "cell %s", def->name);
    for (size_t name = 0; name < sizeof k_watch / sizeof k_watch[0]; name++) {
        for (uint32_t i = 0; i < def->n_params; i++) {
            if (strcmp(def->params[i].name, k_watch[name]) == 0) {
                size_t at = strlen(line);
                (void)snprintf(line + at, ITEM_LEN - at, " %s=%lld", k_watch[name],
                               (long long)pint(node, i));
            }
        }
    }
    if (strncmp(def->name, "$reduce", 7) == 0) {
        append_const_a(node, line);
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
    summary want;
    summary got;
    bool fine = false;
    read_case(name, ".il", text);
    read_case(name, ".expect", expect);
    parse_expect(expect, &want, &fine);
    build(text);
    fx.opts.fine = fine;
    run_reduce();
    summarize(&got);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.n_cells, got.n_cells, name);
    for (uint32_t i = 0; i < want.n_cells; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.cells[i], got.cells[i], name);
    }
}

static void test_reduce_yosys_cases(void) {
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        run_yosys_case(k_cases[i]);
        reset();
    }
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* Every kind of rewrite once, and a register with an init that must never change. */
#define OOM_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\s0\n  wire input 5 \\s1\n  wire input 6 \\s2\n  wire input 7 \\clk\n"        \
    "  wire input 8 \\s\n  wire output 9 \\y1\n  wire output 10 \\y2\n  wire output 11 \\y3\n"     \
    "  wire width 2 output 12 \\y4\n  wire output 13 \\y5\n  wire output 14 \\y6\n"                \
    "  attribute \\init 1'1\n  wire output 15 \\q\n  wire \\t\n" RED("$reduce_or", "r1", "4",      \
                                                                     "1'0 \\b \\a \\b", "\\t")     \
        RED("$reduce_or", "r2", "2", "\\c \\t", "\\y1") RED(                                       \
            "$reduce_and", "r3", "2", "\\a 1'0",                                                   \
            "\\y2") "  cell $pmux \\p\n    parameter \\S_WIDTH 3\n    connect \\A \\a\n"           \
                    "    connect \\B { \\b \\c \\b }\n    connect \\S { \\s2 \\s1 \\s0 }\n"        \
                    "    connect \\Y \\y3\n  end\n"                                                \
                    "  cell $mux \\mb\n    parameter \\WIDTH 2\n    connect \\A { \\a \\a }\n"     \
                    "    connect \\B { \\b \\b }\n    connect \\S \\s\n    connect \\Y \\y4\n  "   \
                    "end\n"                                                                        \
                    "  cell $pmux \\q1\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"          \
                    "    connect \\B { \\b \\a }\n    connect \\S { \\s1 \\s0 }\n    connect \\Y " \
                    "\\y5\n  end\n"                                                                \
                    "  cell $mux \\q2\n    connect \\A \\b\n    connect \\B \\b\n    connect \\S " \
                    "\\s\n"                                                                        \
                    "    connect \\Y \\y6\n  end\n"                                                \
                    "  cell $dff \\f\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"   \
                    "    connect \\CLK \\clk\n    connect \\D \\y3\n    connect \\Q \\q\n  "       \
                    "end\nend\n"

enum { OOM_CHANGES = 7 };

static long g_half_done;

/* A failure inside one of the kit's merges, after its delete (kit.h), is logged: the IR is valid.
 */
static void count_half_done(void) {
    for (uint32_t i = 0; i < n_lines; i++) {
        g_half_done += strstr(lines[i].text, "in the middle of a rewrite") != NULL ? 1 : 0;
    }
}

static void oom_run_once(long fail, bool *done) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_reduce_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    count_half_done();
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    /* OP-2: whichever point failed, the register still starts at 1 */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("q", 0)));
}

static void test_reduce_oom(void) {
    fx.opts.fine = true;
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(OOM_CHANGES, fx.counts.reduce_changes);
    TEST_ASSERT_TRUE(failures > 20 && g_half_done > 0 && g_half_done < failures);
    printf("opt_reduce OOM: %ld failing allocation points, %ld in the middle of a kit merge\n",
           failures, g_half_done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reduce_or_dedup);
    RUN_TEST(test_reduce_or_one);
    RUN_TEST(test_reduce_and_zero);
    RUN_TEST(test_reduce_neutral);
    RUN_TEST(test_reduce_empty);
    RUN_TEST(test_reduce_unchanged);
    RUN_TEST(test_reduce_inline);
    RUN_TEST(test_reduce_inline_chain);
    RUN_TEST(test_reduce_inline_fanout);
    RUN_TEST(test_reduce_inline_other_type);
    RUN_TEST(test_reduce_cross_type);
    RUN_TEST(test_reduce_child_constant);
    RUN_TEST(test_reduce_loop);
    RUN_TEST(test_reduce_ywidth);
    RUN_TEST(test_reduce_deep_chain);
    RUN_TEST(test_reduce_unconnected_pin);
    RUN_TEST(test_pmux_slice_eq_a);
    RUN_TEST(test_pmux_equal_slices);
    RUN_TEST(test_pmux_group_order);
    RUN_TEST(test_pmux_all_a);
    RUN_TEST(test_mux_forms);
    RUN_TEST(test_mux_bits_fine);
    RUN_TEST(test_mux_bits_nofine);
    RUN_TEST(test_mux_bits_same_bit);
    RUN_TEST(test_mux_bits_all_same);
    RUN_TEST(test_mux_bits_pmux);
    RUN_TEST(test_mux_bits_mem_wren);
    RUN_TEST(test_mux_bits_wren_chain);
    RUN_TEST(test_mux_bits_wren_dff);
    RUN_TEST(test_wide_pmux);
    RUN_TEST(test_collisions);
    RUN_TEST(test_lines);
    RUN_TEST(test_no_debug_lines);
    RUN_TEST(test_determinism);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_provenance);
    RUN_TEST(test_provenance_pmux);
    RUN_TEST(test_reduce_yosys_cases);
    RUN_TEST(test_reduce_oom);
    return UNITY_END();
}
