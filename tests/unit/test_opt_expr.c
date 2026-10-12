/*
 * test_opt_expr.c — opt_expr (2F spec OP-4, §4; plan Task 6): the logic rules (§4.2), the
 * comparison, shift, arithmetic and mux rules (§4.3), replace_undriven (§4.6), -keepdc, the order
 * of the rules and the counters and log lines, module scope, the opacity of $proc nodes and out of
 * memory. Every case is an RTLIL module through pb_build, one odin3_opt_expr_run (or the design
 * entry) and assertions on the cells and nets after.
 *
 * Oracle: Yosys 0.55, the VTR build (the case files are `read_rtlil`-able as they stand, every
 * parameter spelled out in Yosys's copy of the text):
 *   yosys -p "read_rtlil CASE.il; opt_expr [FLAGS]; write_rtlil"
 * with FLAGS as the comment of each case says. Where Odin III's IR differs from RTLIL (spec §17):
 * identities are wiring, so Yosys's `$pos` is a connection here, and a net that Yosys leaves as a
 * temporary wire tied to a constant is the constant's net here.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/opt/expr.h"
#include "passes/opt/fold.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LINE_MAX_LEN = 256, LINES_MAX = 64, BITS_MAX = 64, OOM_LIMIT = 20000, VALUES = 256 };

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
    odin3_opt_expr_counts counts;
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

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void build(const char *text) {
    pb_free(&fx.res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fx.res.design, intern("opt_expr"), &fx.ctx));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    odin3_status st = odin3_check_design(fx.res.design, opts);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        (void)fprintf(stderr, "  log: %s\n", lines[i].text);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

/* One opt_expr call on the last module of the text with fx.opts, then the full check. */
static void run_expr(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_expr_run(&fx.ctx, fx.res.module, &fx.opts, &fx.counts));
    check_full();
}

static void build_run(const char *text) {
    build(text);
    run_expr();
}

/* --- reading the result ----------------------------------------------------------------------- */

static odin3_net_id wnet(const char *wire, uint32_t bit) {
    odin3_wire_id wire_id = odin3_module_find_wire(fx.res.module, intern(wire));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire_id), wire);
    odin3_net_id net = odin3_wire_net(fx.res.module, wire_id, bit);
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), wire);
    return net;
}

static char cbit(odin3_net_id net) {
    switch (odin3_net_const_value(fx.res.module, net)) {
    case ODIN3_CONST_0:
        return '0';
    case ODIN3_CONST_1:
        return '1';
    case ODIN3_CONST_X:
        return 'x';
    case ODIN3_CONST_Z:
        return 'z';
    case ODIN3_CONST_NONE:
    default:
        return '?';
    }
}

/* The bits of a wire, MSB first: 0 1 x z, or ? for a net that is not constant. */
static void wbits(const char *wire, uint32_t width, char *out) {
    TEST_ASSERT_TRUE(width < BITS_MAX);
    for (uint32_t k = 0; k < width; k++) {
        out[width - 1 - k] = cbit(wnet(wire, k));
    }
    out[width] = '\0';
}

static void expect_wire(const char *wire, uint32_t width, const char *want) {
    char got[BITS_MAX];
    wbits(wire, width, got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, wire);
}

static bool same_net(odin3_net_id lhs, odin3_net_id rhs) {
    return lhs.v == rhs.v;
}

static void expect_same(odin3_net_id got, odin3_net_id want, const char *what) {
    TEST_ASSERT_TRUE_MESSAGE(same_net(got, want), what);
}

static const char *type_name(odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.res.module, node));
    TEST_ASSERT_NOT_NULL(def);
    return def->name;
}

static odin3_node_id driver_node(odin3_net_id net) {
    odin3_pin_id pin = odin3_net_driver(fx.res.module, net);
    return odin3_pin_valid(pin) ? odin3_pin_node(fx.res.module, pin) : (odin3_node_id){0};
}

/* The type name of the node that drives net ("" when none does). */
static const char *driver_type(odin3_net_id net) {
    odin3_node_id node = driver_node(net);
    return odin3_node_valid(node) ? type_name(node) : "";
}

static uint32_t count_type(const char *name) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.res.module); id++) {
        odin3_node_id node = {id};
        hits +=
            odin3_node_live(fx.res.module, node) && strcmp(type_name(node), name) == 0 ? 1U : 0U;
    }
    return hits;
}

/* The live node of the type with the lowest ID, none when there is no such node. */
static odin3_node_id find_type(const char *name) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.res.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.res.module, node) && strcmp(type_name(node), name) == 0) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static odin3_node_id need_type(const char *name) {
    odin3_node_id node = find_type(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    return node;
}

static uint32_t port_index(odin3_node_id node, const char *port) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.res.module, node));
    for (uint32_t i = 0; def != NULL && i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, port) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(port);
    return 0;
}

static uint32_t pwidth(odin3_node_id node, const char *port) {
    return odin3_node_port(fx.res.module, node, port_index(node, port)).count;
}

static odin3_net_id pnet(odin3_node_id node, const char *port, uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(fx.res.module, node, port_index(node, port));
    TEST_ASSERT_TRUE_MESSAGE(bit < pins.count, port);
    return odin3_pin_net(fx.res.module, (odin3_pin_id){pins.first.v + bit});
}

/* The bits of a port, MSB first (as wbits). */
static void pbits(odin3_node_id node, const char *port, char *out) {
    uint32_t width = pwidth(node, port);
    TEST_ASSERT_TRUE(width < BITS_MAX);
    for (uint32_t k = 0; k < width; k++) {
        out[width - 1 - k] = cbit(pnet(node, port, k));
    }
    out[width] = '\0';
}

static void expect_port(odin3_node_id node, const char *port, const char *want) {
    char got[BITS_MAX];
    pbits(node, port, got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, port);
}

static int64_t pparam(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.res.module, node));
    for (uint32_t i = 0; def != NULL && i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            const odin3_value *val = odin3_node_param(fx.res.module, node, i);
            TEST_ASSERT_NOT_NULL(val);
            return val->i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

/* Port `port` of node is the nets of wire `wire` from bit `from`, bit for bit. */
static void expect_port_is_wire(odin3_node_id node, const char *port, const char *wire,
                                uint32_t from) {
    uint32_t width = pwidth(node, port);
    for (uint32_t k = 0; k < width; k++) {
        expect_same(pnet(node, port, k), wnet(wire, from + k), port);
    }
}

static uint32_t rule(odin3_opt_rule which) {
    return fx.counts.rule[which];
}

static uint32_t rules_total(void) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < ODIN3_OPT_RULE_COUNT; i++) {
        total += fx.counts.rule[i];
    }
    return total;
}

/* --- the cases (tools: gen.py's text; every parameter at its default is left out) ------------- */

static const char K_CONST_AND[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $and \\c\n"
                                  "    connect \\A \\a\n"
                                  "    connect \\B 1'0\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_AND_BIT_INVERSE[] = "module \\m\n"
                                        "  wire input 1 \\a\n"
                                        "  wire output 2 \\y\n"
                                        "  wire \\n\n"
                                        "  cell $not \\inv\n"
                                        "    connect \\A \\a\n"
                                        "    connect \\Y \\n\n"
                                        "  end\n"
                                        "  cell $and \\c\n"
                                        "    connect \\A \\a\n"
                                        "    connect \\B \\n\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_CONSUME_X[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $and \\c\n"
                                  "    connect \\A \\a\n"
                                  "    connect \\B 1'x\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_AND_OR_BUFFER[] = "module \\m\n"
                                      "  wire input 1 \\a\n"
                                      "  wire output 2 \\y\n"
                                      "  cell $reduce_or \\c\n"
                                      "    parameter \\A_WIDTH 3\n"
                                      "    connect \\A { 1'0 \\a 1'0 }\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_XOR_BUFFER[] = "module \\m\n"
                                   "  wire input 1 \\a\n"
                                   "  wire output 2 \\y\n"
                                   "  cell $xor \\c\n"
                                   "    connect \\A \\a\n"
                                   "    connect \\B 1'1\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "end\n";

static const char K_REDUCE_XNOR_NOT[] = "module \\m\n"
                                        "  wire input 1 \\a\n"
                                        "  wire output 2 \\y\n"
                                        "  cell $reduce_xnor \\c\n"
                                        "    connect \\A \\a\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_UNARY_BUFFER[] = "module \\m\n"
                                     "  wire input 1 \\a\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $reduce_and \\c\n"
                                     "    connect \\A \\a\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_CONST_XOR_XNOR[] = "module \\m\n"
                                       "  wire input 1 \\a\n"
                                       "  wire output 2 \\y1\n"
                                       "  wire output 3 \\y2\n"
                                       "  cell $xor \\c1\n"
                                       "    connect \\A \\a\n"
                                       "    connect \\B \\a\n"
                                       "    connect \\Y \\y1\n"
                                       "  end\n"
                                       "  cell $xnor \\c2\n"
                                       "    connect \\A \\a\n"
                                       "    connect \\B \\a\n"
                                       "    connect \\Y \\y2\n"
                                       "  end\n"
                                       "end\n";

static const char K_BITWISE_OR_X[] = "module \\m\n"
                                     "  wire width 2 input 1 \\a2\n"
                                     "  wire width 2 output 2 \\y\n"
                                     "  cell $or \\c\n"
                                     "    parameter \\A_WIDTH 2\n"
                                     "    parameter \\B_WIDTH 2\n"
                                     "    parameter \\Y_WIDTH 2\n"
                                     "    connect \\A \\a2\n"
                                     "    connect \\B 2'x0\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_BITWISE_AND_X[] = "module \\m\n"
                                      "  wire width 4 input 1 \\a4\n"
                                      "  wire width 4 output 2 \\y\n"
                                      "  cell $and \\c\n"
                                      "    parameter \\A_WIDTH 4\n"
                                      "    parameter \\B_WIDTH 4\n"
                                      "    parameter \\Y_WIDTH 4\n"
                                      "    connect \\A \\a4\n"
                                      "    connect \\B 4'10x1\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_BITWISE_XOR_X[] = "module \\m\n"
                                      "  wire width 2 input 1 \\a2\n"
                                      "  wire width 2 output 2 \\y\n"
                                      "  cell $xor \\c\n"
                                      "    parameter \\A_WIDTH 2\n"
                                      "    parameter \\B_WIDTH 2\n"
                                      "    parameter \\Y_WIDTH 2\n"
                                      "    connect \\A \\a2\n"
                                      "    connect \\B 2'x1\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_INVERT_DOUBLE[] = "module \\m\n"
                                      "  wire input 1 \\a\n"
                                      "  wire output 2 \\y\n"
                                      "  wire \\t\n"
                                      "  cell $not \\n1\n"
                                      "    connect \\A \\a\n"
                                      "    connect \\Y \\t\n"
                                      "  end\n"
                                      "  cell $not \\n2\n"
                                      "    connect \\A \\t\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_INVERT_MUXSEL[] = "module \\m\n"
                                      "  wire input 1 \\p\n"
                                      "  wire input 2 \\q\n"
                                      "  wire input 3 \\s\n"
                                      "  wire output 4 \\y\n"
                                      "  wire \\ns\n"
                                      "  cell $not \\inv\n"
                                      "    connect \\A \\s\n"
                                      "    connect \\Y \\ns\n"
                                      "  end\n"
                                      "  cell $mux \\c\n"
                                      "    connect \\A \\p\n"
                                      "    connect \\B \\q\n"
                                      "    connect \\S \\ns\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_TRIBUF_EN1[] = "module \\m\n"
                                   "  wire input 1 \\a\n"
                                   "  wire output 2 \\y\n"
                                   "  cell $tribuf \\c\n"
                                   "    connect \\A \\a\n"
                                   "    connect \\EN 1'1\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "end\n";

static const char K_TRIBUF_EN0[] = "module \\m\n"
                                   "  wire input 1 \\a\n"
                                   "  wire output 2 \\y\n"
                                   "  cell $tribuf \\c\n"
                                   "    connect \\A \\a\n"
                                   "    connect \\EN 1'0\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "end\n";

static const char K_GATE_AND0[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $_AND_ \\c\n"
                                  "    connect \\A \\a\n"
                                  "    connect \\B 1'0\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_GATE_AND1[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $_AND_ \\c\n"
                                  "    connect \\A 1'1\n"
                                  "    connect \\B \\a\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_GATE_ANDX[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $_AND_ \\c\n"
                                  "    connect \\A \\a\n"
                                  "    connect \\B 1'x\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_GATE_OR1[] = "module \\m\n"
                                 "  wire input 1 \\a\n"
                                 "  wire output 2 \\y\n"
                                 "  cell $_OR_ \\c\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B 1'1\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_GATE_OR0[] = "module \\m\n"
                                 "  wire input 1 \\a\n"
                                 "  wire output 2 \\y\n"
                                 "  cell $_OR_ \\c\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B 1'0\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_GATE_XOR[] = "module \\m\n"
                                 "  wire input 1 \\a\n"
                                 "  wire output 2 \\y\n"
                                 "  wire output 3 \\z\n"
                                 "  cell $_XOR_ \\c\n"
                                 "    connect \\A 1'1\n"
                                 "    connect \\B 1'1\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "  cell $_XOR_ \\d\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B 1'x\n"
                                 "    connect \\Y \\z\n"
                                 "  end\n"
                                 "end\n";

static const char K_GATE_NOT[] = "module \\m\n"
                                 "  wire output 1 \\y\n"
                                 "  wire output 2 \\z\n"
                                 "  wire output 3 \\w\n"
                                 "  cell $_NOT_ \\c1\n"
                                 "    connect \\A 1'1\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "  cell $_NOT_ \\c2\n"
                                 "    connect \\A 1'0\n"
                                 "    connect \\Y \\z\n"
                                 "  end\n"
                                 "  cell $_NOT_ \\c3\n"
                                 "    connect \\A 1'x\n"
                                 "    connect \\Y \\w\n"
                                 "  end\n"
                                 "end\n";

static const char K_GATE_MUX_SAME[] = "module \\m\n"
                                      "  wire input 1 \\a\n"
                                      "  wire input 2 \\s\n"
                                      "  wire output 3 \\y\n"
                                      "  cell $_MUX_ \\c\n"
                                      "    connect \\A \\a\n"
                                      "    connect \\B \\a\n"
                                      "    connect \\S \\s\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_GATE_MUX_INV[] = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $_MUX_ \\c\n"
                                     "    connect \\A 1'1\n"
                                     "    connect \\B 1'0\n"
                                     "    connect \\S \\s\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_GATE_MUX_BUF[] = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $_MUX_ \\c\n"
                                     "    connect \\A 1'0\n"
                                     "    connect \\B 1'1\n"
                                     "    connect \\S \\s\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_GATE_MUX_SEL[] = "module \\m\n"
                                     "  wire input 1 \\a\n"
                                     "  wire input 2 \\b\n"
                                     "  wire output 3 \\y\n"
                                     "  wire output 4 \\z\n"
                                     "  cell $_MUX_ \\c\n"
                                     "    connect \\A \\a\n"
                                     "    connect \\B \\b\n"
                                     "    connect \\S 1'0\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "  cell $_MUX_ \\d\n"
                                     "    connect \\A \\a\n"
                                     "    connect \\B \\b\n"
                                     "    connect \\S 1'1\n"
                                     "    connect \\Y \\z\n"
                                     "  end\n"
                                     "end\n";

static const char K_GATE_MUX_UNDEF[] = "module \\m\n"
                                       "  wire input 1 \\a\n"
                                       "  wire input 2 \\s\n"
                                       "  wire output 3 \\y\n"
                                       "  cell $_MUX_ \\c\n"
                                       "    connect \\A \\a\n"
                                       "    connect \\B 1'x\n"
                                       "    connect \\S \\s\n"
                                       "    connect \\Y \\y\n"
                                       "  end\n"
                                       "end\n";

static const char K_GATE_MUX_UNDEF_OFF[] = "module \\m\n"
                                           "  wire input 1 \\a\n"
                                           "  wire input 2 \\s\n"
                                           "  wire output 3 \\y\n"
                                           "  cell $_MUX_ \\c\n"
                                           "    connect \\A \\a\n"
                                           "    connect \\B 1'x\n"
                                           "    connect \\S \\s\n"
                                           "    connect \\Y \\y\n"
                                           "  end\n"
                                           "end\n";

static const char K_XBIT_ARITH[] = "module \\m\n"
                                   "  wire width 4 input 1 \\a4\n"
                                   "  wire width 4 output 2 \\y\n"
                                   "  cell $add \\c\n"
                                   "    parameter \\A_WIDTH 4\n"
                                   "    parameter \\B_WIDTH 4\n"
                                   "    parameter \\Y_WIDTH 4\n"
                                   "    connect \\A \\a4\n"
                                   "    connect \\B 4'00x1\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "end\n";

static const char K_XBIT_CMP[] = "module \\m\n"
                                 "  wire width 4 input 1 \\a4\n"
                                 "  wire width 2 output 2 \\y\n"
                                 "  cell $lt \\c\n"
                                 "    parameter \\A_WIDTH 4\n"
                                 "    parameter \\B_WIDTH 4\n"
                                 "    parameter \\Y_WIDTH 2\n"
                                 "    connect \\A \\a4\n"
                                 "    connect \\B 4'1x00\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_EQNEQ_ISNEQ[] = "module \\m\n"
                                    "  wire input 1 \\a\n"
                                    "  wire input 2 \\b\n"
                                    "  wire output 3 \\y\n"
                                    "  cell $eq \\c\n"
                                    "    parameter \\A_WIDTH 2\n"
                                    "    parameter \\B_WIDTH 2\n"
                                    "    connect \\A { \\a 1'0 }\n"
                                    "    connect \\B { \\b 1'1 }\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "end\n";

static const char K_EQNEQ_EMPTY[] = "module \\m\n"
                                    "  wire width 4 input 1 \\a4\n"
                                    "  wire output 2 \\y\n"
                                    "  cell $ne \\c\n"
                                    "    parameter \\A_WIDTH 4\n"
                                    "    parameter \\B_WIDTH 4\n"
                                    "    connect \\A \\a4\n"
                                    "    connect \\B \\a4\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "end\n";

static const char K_EQNEQ_RESIZE[] = "module \\m\n"
                                     "  wire input 1 \\a\n"
                                     "  wire input 2 \\c\n"
                                     "  wire input 3 \\d\n"
                                     "  wire output 4 \\y\n"
                                     "  cell $eq \\q\n"
                                     "    parameter \\A_WIDTH 2\n"
                                     "    parameter \\B_WIDTH 2\n"
                                     "    connect \\A { \\a \\c }\n"
                                     "    connect \\B { \\a \\d }\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_EQNEQ_SWAPCONST_ISNOT[] = "module \\m\n"
                                              "  wire input 1 \\a\n"
                                              "  wire output 2 \\y\n"
                                              "  cell $eq \\c\n"
                                              "    connect \\A 1'0\n"
                                              "    connect \\B \\a\n"
                                              "    connect \\Y \\y\n"
                                              "  end\n"
                                              "end\n";

static const char K_EQNEQ_BUFFER[] = "module \\m\n"
                                     "  wire input 1 \\a\n"
                                     "  wire output 2 \\y\n"
                                     "  wire output 3 \\z\n"
                                     "  cell $eq \\c\n"
                                     "    connect \\A \\a\n"
                                     "    connect \\B 1'1\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "  cell $ne \\d\n"
                                     "    connect \\A 1'0\n"
                                     "    connect \\B \\a\n"
                                     "    connect \\Y \\z\n"
                                     "  end\n"
                                     "end\n";

static const char K_EQNEQ_UNDEF[] = "module \\m\n"
                                    "  wire input 1 \\a\n"
                                    "  wire output 2 \\y\n"
                                    "  cell $eq \\c\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B 1'x\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "end\n";

static const char K_EQNEQ_CMPZERO[] = "module \\m\n"
                                      "  wire width 4 input 1 \\a4\n"
                                      "  wire output 2 \\y\n"
                                      "  wire output 3 \\z\n"
                                      "  cell $eq \\c\n"
                                      "    parameter \\A_WIDTH 4\n"
                                      "    parameter \\B_WIDTH 4\n"
                                      "    connect \\A \\a4\n"
                                      "    connect \\B 4'0000\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "  cell $ne \\d\n"
                                      "    parameter \\A_WIDTH 4\n"
                                      "    parameter \\B_WIDTH 4\n"
                                      "    connect \\A 4'0000\n"
                                      "    connect \\B \\a4\n"
                                      "    connect \\Y \\z\n"
                                      "  end\n"
                                      "end\n";

static const char K_CONSTSHIFT_SHL[] = "module \\m\n"
                                       "  wire width 4 input 1 \\a4\n"
                                       "  wire width 4 output 2 \\y\n"
                                       "  cell $shl \\c\n"
                                       "    parameter \\A_WIDTH 4\n"
                                       "    parameter \\B_WIDTH 2\n"
                                       "    parameter \\Y_WIDTH 4\n"
                                       "    connect \\A \\a4\n"
                                       "    connect \\B 2'01\n"
                                       "    connect \\Y \\y\n"
                                       "  end\n"
                                       "end\n";

static const char K_CONSTSHIFT_SSHR[] = "module \\m\n"
                                        "  wire width 4 input 1 \\s4\n"
                                        "  wire width 4 output 2 \\y\n"
                                        "  cell $sshr \\c\n"
                                        "    parameter \\A_SIGNED 1\n"
                                        "    parameter \\A_WIDTH 4\n"
                                        "    parameter \\B_WIDTH 2\n"
                                        "    parameter \\Y_WIDTH 4\n"
                                        "    connect \\A \\s4\n"
                                        "    connect \\B 2'01\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_CONSTSHIFT_SHIFTX[] = "module \\m\n"
                                          "  wire width 4 input 1 \\a4\n"
                                          "  wire width 4 output 2 \\y\n"
                                          "  cell $shiftx \\c\n"
                                          "    parameter \\A_WIDTH 4\n"
                                          "    parameter \\B_WIDTH 2\n"
                                          "    parameter \\Y_WIDTH 4\n"
                                          "    connect \\A \\a4\n"
                                          "    connect \\B 2'01\n"
                                          "    connect \\Y \\y\n"
                                          "  end\n"
                                          "end\n";

static const char K_CONSTSHIFT_WIDE[] = "module \\m\n"
                                        "  wire width 2 input 1 \\a2\n"
                                        "  wire width 4 output 2 \\y\n"
                                        "  cell $shr \\c\n"
                                        "    parameter \\A_WIDTH 2\n"
                                        "    parameter \\B_WIDTH 8\n"
                                        "    parameter \\Y_WIDTH 4\n"
                                        "    connect \\A \\a2\n"
                                        "    connect \\B 8'11111111\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_SHIFTX_TRIM[] = "module \\m\n"
                                    "  wire width 2 input 1 \\a2\n"
                                    "  wire width 2 input 2 \\b\n"
                                    "  wire width 2 output 3 \\y\n"
                                    "  cell $shiftx \\c\n"
                                    "    parameter \\A_WIDTH 4\n"
                                    "    parameter \\B_WIDTH 2\n"
                                    "    parameter \\Y_WIDTH 2\n"
                                    "    connect \\A { 1'x 1'x \\a2 }\n"
                                    "    connect \\B \\b\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "end\n";

static const char K_IDENTWRT_ADD[] = "module \\m\n"
                                     "  wire width 4 input 1 \\a4\n"
                                     "  wire width 4 output 2 \\y\n"
                                     "  cell $add \\c\n"
                                     "    parameter \\A_WIDTH 4\n"
                                     "    parameter \\B_WIDTH 4\n"
                                     "    parameter \\Y_WIDTH 4\n"
                                     "    connect \\A \\a4\n"
                                     "    connect \\B 4'0000\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_IDENTWRT_MUL[] = "module \\m\n"
                                     "  wire width 4 input 1 \\a4\n"
                                     "  wire width 4 output 2 \\y\n"
                                     "  cell $mul \\c\n"
                                     "    parameter \\A_WIDTH 4\n"
                                     "    parameter \\B_WIDTH 4\n"
                                     "    parameter \\Y_WIDTH 4\n"
                                     "    connect \\A \\a4\n"
                                     "    connect \\B 4'0001\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_IDENTWRT_NEG[] = "module \\m\n"
                                     "  wire width 4 input 1 \\s4\n"
                                     "  wire width 4 output 2 \\y\n"
                                     "  cell $mul \\c\n"
                                     "    parameter \\A_SIGNED 1\n"
                                     "    parameter \\B_SIGNED 1\n"
                                     "    parameter \\A_WIDTH 4\n"
                                     "    parameter \\B_WIDTH 4\n"
                                     "    parameter \\Y_WIDTH 4\n"
                                     "    connect \\A \\s4\n"
                                     "    connect \\B 4'1111\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_POW_SHIFT_TWO[] = "module \\m\n"
                                      "  wire width 3 input 1 \\b3\n"
                                      "  wire width 8 output 2 \\y\n"
                                      "  cell $pow \\c\n"
                                      "    parameter \\A_WIDTH 32\n"
                                      "    parameter \\B_WIDTH 3\n"
                                      "    parameter \\Y_WIDTH 8\n"
                                      "    connect \\A 32'00000000000000000000000000000010\n"
                                      "    connect \\B \\b3\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_POW_SHIFT_EIGHT[] = "module \\m\n"
                                        "  wire width 3 input 1 \\b3\n"
                                        "  wire width 8 output 2 \\y\n"
                                        "  cell $pow \\c\n"
                                        "    parameter \\A_WIDTH 32\n"
                                        "    parameter \\B_WIDTH 3\n"
                                        "    parameter \\Y_WIDTH 8\n"
                                        "    connect \\A 32'00000000000000000000000000001000\n"
                                        "    connect \\B \\b3\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_MUL_SHIFT[] = "module \\m\n"
                                  "  wire width 4 input 1 \\a4\n"
                                  "  wire width 4 output 2 \\y\n"
                                  "  wire width 4 output 3 \\z\n"
                                  "  cell $mul \\c\n"
                                  "    parameter \\A_WIDTH 4\n"
                                  "    parameter \\B_WIDTH 4\n"
                                  "    parameter \\Y_WIDTH 4\n"
                                  "    connect \\A \\a4\n"
                                  "    connect \\B 4'0100\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "  cell $mul \\d\n"
                                  "    parameter \\A_WIDTH 4\n"
                                  "    parameter \\B_WIDTH 4\n"
                                  "    parameter \\Y_WIDTH 4\n"
                                  "    connect \\A \\a4\n"
                                  "    connect \\B 4'0000\n"
                                  "    connect \\Y \\z\n"
                                  "  end\n"
                                  "end\n";

static const char K_MUL_SHIFT_A_CONST[] = "module \\m\n"
                                          "  wire width 4 input 1 \\a4\n"
                                          "  wire width 4 output 2 \\y\n"
                                          "  cell $mul \\c\n"
                                          "    parameter \\A_WIDTH 4\n"
                                          "    parameter \\B_WIDTH 4\n"
                                          "    parameter \\Y_WIDTH 4\n"
                                          "    connect \\A 4'0100\n"
                                          "    connect \\B \\a4\n"
                                          "    connect \\Y \\y\n"
                                          "  end\n"
                                          "end\n";

static const char K_MUL_LOW_ZEROS[] = "module \\m\n"
                                      "  wire width 2 input 1 \\b2\n"
                                      "  wire width 4 input 2 \\a4\n"
                                      "  wire width 8 output 3 \\y\n"
                                      "  cell $mul \\c\n"
                                      "    parameter \\A_WIDTH 4\n"
                                      "    parameter \\B_WIDTH 4\n"
                                      "    parameter \\Y_WIDTH 8\n"
                                      "    connect \\A \\a4\n"
                                      "    connect \\B { \\b2 2'00 }\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_DIVMOD_ZERO[] = "module \\m\n"
                                    "  wire width 4 input 1 \\a4\n"
                                    "  wire width 4 output 2 \\y\n"
                                    "  wire width 4 output 3 \\z\n"
                                    "  cell $div \\c\n"
                                    "    parameter \\A_WIDTH 4\n"
                                    "    parameter \\B_WIDTH 4\n"
                                    "    parameter \\Y_WIDTH 4\n"
                                    "    connect \\A \\a4\n"
                                    "    connect \\B 4'0000\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "  cell $mod \\d\n"
                                    "    parameter \\A_WIDTH 4\n"
                                    "    parameter \\B_WIDTH 4\n"
                                    "    parameter \\Y_WIDTH 4\n"
                                    "    connect \\A \\a4\n"
                                    "    connect \\B 4'0000\n"
                                    "    connect \\Y \\z\n"
                                    "  end\n"
                                    "end\n";

static const char K_DIV_SHIFT_MOD_MASK[] = "module \\m\n"
                                           "  wire width 8 input 1 \\a8\n"
                                           "  wire width 8 output 2 \\y\n"
                                           "  wire width 8 output 3 \\z\n"
                                           "  cell $div \\c\n"
                                           "    parameter \\A_WIDTH 8\n"
                                           "    parameter \\B_WIDTH 8\n"
                                           "    parameter \\Y_WIDTH 8\n"
                                           "    connect \\A \\a8\n"
                                           "    connect \\B 8'00000100\n"
                                           "    connect \\Y \\y\n"
                                           "  end\n"
                                           "  cell $mod \\d\n"
                                           "    parameter \\A_WIDTH 8\n"
                                           "    parameter \\B_WIDTH 8\n"
                                           "    parameter \\Y_WIDTH 8\n"
                                           "    connect \\A \\a8\n"
                                           "    connect \\B 8'00000100\n"
                                           "    connect \\Y \\z\n"
                                           "  end\n"
                                           "end\n";

static const char K_DIV_SHIFT_SIGNED[] = "module \\m\n"
                                         "  wire width 8 input 1 \\s8\n"
                                         "  wire width 8 output 2 \\y\n"
                                         "  wire width 8 output 3 \\z\n"
                                         "  cell $div \\c\n"
                                         "    parameter \\A_SIGNED 1\n"
                                         "    parameter \\B_SIGNED 1\n"
                                         "    parameter \\A_WIDTH 8\n"
                                         "    parameter \\B_WIDTH 8\n"
                                         "    parameter \\Y_WIDTH 8\n"
                                         "    connect \\A \\s8\n"
                                         "    connect \\B 8'00000100\n"
                                         "    connect \\Y \\y\n"
                                         "  end\n"
                                         "  cell $mod \\d\n"
                                         "    parameter \\A_SIGNED 1\n"
                                         "    parameter \\B_SIGNED 1\n"
                                         "    parameter \\A_WIDTH 8\n"
                                         "    parameter \\B_WIDTH 8\n"
                                         "    parameter \\Y_WIDTH 8\n"
                                         "    connect \\A \\s8\n"
                                         "    connect \\B 8'00000100\n"
                                         "    connect \\Y \\z\n"
                                         "  end\n"
                                         "end\n";

static const char K_CONST_FOLD[] = "module \\m\n"
                                   "  wire width 4 output 1 \\y\n"
                                   "  cell $add \\c\n"
                                   "    parameter \\A_WIDTH 4\n"
                                   "    parameter \\B_WIDTH 4\n"
                                   "    parameter \\Y_WIDTH 4\n"
                                   "    connect \\A 4'0011\n"
                                   "    connect \\B 4'0101\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "end\n";

static const char K_MUX_S_X_PICKS_A[] = "module \\m\n"
                                        "  wire input 1 \\a\n"
                                        "  wire input 2 \\b\n"
                                        "  wire output 3 \\y\n"
                                        "  cell $mux \\c\n"
                                        "    connect \\A \\a\n"
                                        "    connect \\B \\b\n"
                                        "    connect \\S 1'x\n"
                                        "    connect \\Y \\y\n"
                                        "  end\n"
                                        "end\n";

static const char K_MUX_SAME[] = "module \\m\n"
                                 "  wire input 1 \\a\n"
                                 "  wire input 2 \\s\n"
                                 "  wire output 3 \\y\n"
                                 "  cell $mux \\c\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B \\a\n"
                                 "    connect \\S \\s\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_MUX_CONST_SEL[] = "module \\m\n"
                                      "  wire input 1 \\a\n"
                                      "  wire input 2 \\b\n"
                                      "  wire output 3 \\y\n"
                                      "  cell $mux \\c\n"
                                      "    connect \\A \\a\n"
                                      "    connect \\B \\b\n"
                                      "    connect \\S 1'1\n"
                                      "    connect \\Y \\y\n"
                                      "  end\n"
                                      "end\n";

static const char K_MUX_BOOL_BUF[] = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $mux \\c\n"
                                     "    connect \\A 1'0\n"
                                     "    connect \\B 1'1\n"
                                     "    connect \\S \\s\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_MUX_BOOL_INV[] = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $mux \\c\n"
                                     "    connect \\A 1'1\n"
                                     "    connect \\B 1'0\n"
                                     "    connect \\S \\s\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_MUX_BOOL_OFF[] = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire output 2 \\y\n"
                                     "  cell $mux \\c\n"
                                     "    connect \\A 1'0\n"
                                     "    connect \\B 1'1\n"
                                     "    connect \\S \\s\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_MUX_AND_OR[] = "module \\m\n"
                                   "  wire input 1 \\b\n"
                                   "  wire input 2 \\s\n"
                                   "  wire output 3 \\y\n"
                                   "  wire input 4 \\a\n"
                                   "  wire output 5 \\z\n"
                                   "  cell $mux \\c\n"
                                   "    connect \\A 1'0\n"
                                   "    connect \\B \\b\n"
                                   "    connect \\S \\s\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "  cell $mux \\d\n"
                                   "    connect \\A \\a\n"
                                   "    connect \\B 1'1\n"
                                   "    connect \\S \\s\n"
                                   "    connect \\Y \\z\n"
                                   "  end\n"
                                   "end\n";

static const char K_MUX_UNDEF[] = "module \\m\n"
                                  "  wire input 1 \\a\n"
                                  "  wire input 2 \\b\n"
                                  "  wire input 3 \\s0\n"
                                  "  wire input 4 \\s1\n"
                                  "  wire output 5 \\y\n"
                                  "  cell $pmux \\c\n"
                                  "    parameter \\S_WIDTH 2\n"
                                  "    connect \\A \\a\n"
                                  "    connect \\B { 1'x \\b }\n"
                                  "    connect \\S { \\s0 \\s1 }\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_MUX_UNDEF_ALLX[] = "module \\m\n"
                                       "  wire input 1 \\s0\n"
                                       "  wire input 2 \\s1\n"
                                       "  wire width 2 output 3 \\y\n"
                                       "  cell $pmux \\c\n"
                                       "    parameter \\WIDTH 2\n"
                                       "    parameter \\S_WIDTH 2\n"
                                       "    connect \\A 2'xx\n"
                                       "    connect \\B 4'xxxx\n"
                                       "    connect \\S { \\s0 \\s1 }\n"
                                       "    connect \\Y \\y\n"
                                       "  end\n"
                                       "end\n";

static const char K_XNOR_BUFFER[] = "module \\m\n"
                                    "  wire input 1 \\a\n"
                                    "  wire output 2 \\y1\n"
                                    "  wire width 2 output 3 \\y2\n"
                                    "  cell $xnor \\c1\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B 1'0\n"
                                    "    connect \\Y \\y1\n"
                                    "  end\n"
                                    "  cell $xnor \\c2\n"
                                    "    parameter \\Y_WIDTH 2\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B 1'1\n"
                                    "    connect \\Y \\y2\n"
                                    "  end\n"
                                    "end\n";

static const char K_MUX_SEL01[] = "module \\m\n"
                                  "  wire input 1 \\s\n"
                                  "  wire output 2 \\y\n"
                                  "  cell $mux \\c\n"
                                  "    connect \\A 1'0\n"
                                  "    connect \\B 1'1\n"
                                  "    connect \\S \\s\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_MUX_EMPTY[] = "module \\m\n"
                                  "  wire input 1 \\b\n"
                                  "  wire input 2 \\s\n"
                                  "  wire output 3 \\y\n"
                                  "  cell $mux \\c\n"
                                  "    connect \\A 1'x\n"
                                  "    connect \\B \\b\n"
                                  "    connect \\S \\s\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_DIV_IDENTITY[] = "module \\m\n"
                                     "  wire width 4 input 1 \\a4\n"
                                     "  wire width 4 output 2 \\y\n"
                                     "  cell $div \\c\n"
                                     "    parameter \\A_WIDTH 4\n"
                                     "    parameter \\B_WIDTH 4\n"
                                     "    parameter \\Y_WIDTH 4\n"
                                     "    connect \\A \\a4\n"
                                     "    connect \\B 4'0001\n"
                                     "    connect \\Y \\y\n"
                                     "  end\n"
                                     "end\n";

static const char K_SIGNED_AND_ONE_BIT[] = "module \\m\n"
                                           "  wire input 1 \\a\n"
                                           "  wire output 2 \\y\n"
                                           "  cell $and \\c\n"
                                           "    parameter \\A_SIGNED 1\n"
                                           "    parameter \\B_SIGNED 1\n"
                                           "    connect \\A \\a\n"
                                           "    connect \\B 1'1\n"
                                           "    connect \\Y \\y\n"
                                           "  end\n"
                                           "end\n";

static const char K_CONSUME_X_REDUCE[] = "module \\m\n"
                                         "  wire input 1 \\a\n"
                                         "  wire output 2 \\y\n"
                                         "  cell $reduce_and \\c\n"
                                         "    parameter \\A_WIDTH 2\n"
                                         "    connect \\A { \\a 1'x }\n"
                                         "    connect \\Y \\y\n"
                                         "  end\n"
                                         "end\n";

static const char K_UNARY_BUFFER_XOR[] = "module \\m\n"
                                         "  wire input 1 \\a\n"
                                         "  wire output 2 \\y\n"
                                         "  wire output 3 \\z\n"
                                         "  cell $reduce_xor \\c\n"
                                         "    connect \\A \\a\n"
                                         "    connect \\Y \\y\n"
                                         "  end\n"
                                         "  cell $neg \\d\n"
                                         "    connect \\A \\a\n"
                                         "    connect \\Y \\z\n"
                                         "  end\n"
                                         "end\n";

static const char K_MUX_REDUCE_RESUMES[] = "module \\m\n"
                                           "  wire input 1 \\v\n"
                                           "  wire output 2 \\y1\n"
                                           "  wire output 3 \\y3\n"
                                           "  cell $pmux \\c1\n"
                                           "    parameter \\S_WIDTH 2\n"
                                           "    connect \\A 1'0\n"
                                           "    connect \\B { 1'0 1'z }\n"
                                           "    connect \\S { 1'0 \\v }\n"
                                           "    connect \\Y \\y1\n"
                                           "  end\n"
                                           "  cell $xor \\c3\n"
                                           "    parameter \\B_WIDTH 2\n"
                                           "    connect \\A 1'z\n"
                                           "    connect \\B { 1'1 \\y1 }\n"
                                           "    connect \\Y \\y3\n"
                                           "  end\n"
                                           "end\n";

static const char K_EQNEQ_RESIZE_RESUMES[] = "module \\m\n"
                                             "  wire input 1 \\a\n"
                                             "  wire input 2 \\c\n"
                                             "  wire output 3 \\y\n"
                                             "  cell $eq \\q\n"
                                             "    parameter \\A_WIDTH 2\n"
                                             "    parameter \\B_WIDTH 2\n"
                                             "    connect \\A { \\a \\c }\n"
                                             "    connect \\B { \\a 1'0 }\n"
                                             "    connect \\Y \\y\n"
                                             "  end\n"
                                             "end\n";

static const char K_FIRST_MATCH[] = "module \\m\n"
                                    "  wire input 1 \\a\n"
                                    "  wire output 2 \\y\n"
                                    "  cell $and \\c\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B 1'0\n"
                                    "    connect \\Y \\y\n"
                                    "  end\n"
                                    "end\n";

static const char K_TOPO_LOOP[] = "module \\m\n"
                                  "  wire output 1 \\y\n"
                                  "  wire \\t1\n"
                                  "  wire \\t2\n"
                                  "  cell $not \\n1\n"
                                  "    connect \\A \\y\n"
                                  "    connect \\Y \\t1\n"
                                  "  end\n"
                                  "  cell $not \\n2\n"
                                  "    connect \\A \\t1\n"
                                  "    connect \\Y \\t2\n"
                                  "  end\n"
                                  "  cell $not \\n3\n"
                                  "    connect \\A \\t2\n"
                                  "    connect \\Y \\y\n"
                                  "  end\n"
                                  "end\n";

static const char K_KEEP_ADD[] = "module \\m\n"
                                 "  wire width 4 output 1 \\y\n"
                                 "  attribute \\keep 1\n"
                                 "  cell $add \\c\n"
                                 "    parameter \\A_WIDTH 4\n"
                                 "    parameter \\B_WIDTH 4\n"
                                 "    parameter \\Y_WIDTH 4\n"
                                 "    connect \\A 4'0011\n"
                                 "    connect \\B 4'0101\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_RULING_A[] = "module \\m\n"
                                 "  wire output 1 \\y\n"
                                 "  wire output 2 \\z\n"
                                 "  cell $reduce_or \\c\n"
                                 "    connect \\A 1'z\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "  cell $reduce_xor \\d\n"
                                 "    connect \\A 1'z\n"
                                 "    connect \\Y \\z\n"
                                 "  end\n"
                                 "end\n";

static const char K_RULING_B[] = "module \\m\n"
                                 "  wire output 1 \\y\n"
                                 "  cell $xor \\c\n"
                                 "    connect \\A 1'1\n"
                                 "    connect \\B 1'0\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_LINES[] = "module \\m\n"
                              "  wire input 1 \\a\n"
                              "  wire input 2 \\b\n"
                              "  wire output 3 \\y1\n"
                              "  wire output 4 \\y2\n"
                              "  wire output 5 \\y3\n"
                              "  wire output 6 \\y4\n"
                              "  wire output 7 \\y5\n"
                              "  cell $and $c5\n"
                              "    connect \\A \\a\n"
                              "    connect \\B 1'0\n"
                              "    connect \\Y \\y1\n"
                              "  end\n"
                              "  cell $or $c6\n"
                              "    connect \\A \\a\n"
                              "    connect \\B 1'1\n"
                              "    connect \\Y \\y2\n"
                              "  end\n"
                              "  cell $xor $c7\n"
                              "    connect \\A \\a\n"
                              "    connect \\B 1'1\n"
                              "    connect \\Y \\y3\n"
                              "  end\n"
                              "  cell $reduce_and $c8\n"
                              "    connect \\A \\b\n"
                              "    connect \\Y \\y4\n"
                              "  end\n"
                              "  cell $reduce_xnor $c9\n"
                              "    connect \\A \\b\n"
                              "    connect \\Y \\y5\n"
                              "  end\n"
                              "end\n";

static const char K_UNDRIVEN[] = "module \\m\n"
                                 "  wire width 2 output 1 \\y\n"
                                 "  attribute \\init 2'1x\n"
                                 "  wire width 2 \\w\n"
                                 "  cell $not \\c\n"
                                 "    parameter \\A_WIDTH 2\n"
                                 "    parameter \\Y_WIDTH 2\n"
                                 "    connect \\A \\w\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_UNDRIVEN_KINDS[] = "module \\m\n"
                                       "  wire input 1 \\a\n"
                                       "  wire output 2 \\y\n"
                                       "  wire \\unread\n"
                                       "  wire $tmp1\n"
                                       "  wire $tmp2\n"
                                       "  cell $and \\c\n"
                                       "    connect \\A \\a\n"
                                       "    connect \\B $tmp1\n"
                                       "    connect \\Y \\y\n"
                                       "  end\n"
                                       "end\n";

static const char K_SCOPE_M1[] = "module \\m1\n"
                                 "  wire input 1 \\a\n"
                                 "  wire output 2 \\y\n"
                                 "  cell $and \\c\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B 1'0\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_SCOPE_M2[] = "module \\m2\n"
                                 "  wire input 1 \\a\n"
                                 "  wire output 2 \\y\n"
                                 "  cell $and \\c\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B 1'0\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

static const char K_OOM[] = "module \\m\n"
                            "  wire input 1 \\a\n"
                            "  wire width 4 input 2 \\a4\n"
                            "  wire width 8 input 3 \\s8\n"
                            "  wire width 3 input 4 \\b3\n"
                            "  wire input 5 \\p\n"
                            "  wire input 6 \\q\n"
                            "  wire input 7 \\s0\n"
                            "  wire input 8 \\s1\n"
                            "  wire output 9 \\y1\n"
                            "  wire width 2 output 10 \\y2\n"
                            "  wire width 8 output 11 \\y3\n"
                            "  wire width 8 output 12 \\y4\n"
                            "  wire width 8 output 13 \\y5\n"
                            "  wire width 8 output 14 \\y6\n"
                            "  wire output 15 \\y7\n"
                            "  wire width 2 \\u\n"
                            "  cell $xor \\c1\n"
                            "    connect \\A \\a\n"
                            "    connect \\B 1'1\n"
                            "    connect \\Y \\y1\n"
                            "  end\n"
                            "  cell $and \\c2\n"
                            "    parameter \\A_WIDTH 2\n"
                            "    parameter \\B_WIDTH 2\n"
                            "    parameter \\Y_WIDTH 2\n"
                            "    connect \\A \\a4 [1:0]\n"
                            "    connect \\B 2'10\n"
                            "    connect \\Y \\y2\n"
                            "  end\n"
                            "  cell $pow \\c3\n"
                            "    parameter \\A_WIDTH 32\n"
                            "    parameter \\B_WIDTH 3\n"
                            "    parameter \\Y_WIDTH 8\n"
                            "    connect \\A 32'00000000000000000000000000001000\n"
                            "    connect \\B \\b3\n"
                            "    connect \\Y \\y3\n"
                            "  end\n"
                            "  cell $div \\c4\n"
                            "    parameter \\A_SIGNED 1\n"
                            "    parameter \\B_SIGNED 1\n"
                            "    parameter \\A_WIDTH 8\n"
                            "    parameter \\B_WIDTH 8\n"
                            "    parameter \\Y_WIDTH 8\n"
                            "    connect \\A \\s8\n"
                            "    connect \\B 8'00000100\n"
                            "    connect \\Y \\y4\n"
                            "  end\n"
                            "  cell $mod \\c5\n"
                            "    parameter \\A_SIGNED 1\n"
                            "    parameter \\B_SIGNED 1\n"
                            "    parameter \\A_WIDTH 8\n"
                            "    parameter \\B_WIDTH 8\n"
                            "    parameter \\Y_WIDTH 8\n"
                            "    connect \\A \\s8\n"
                            "    connect \\B 8'00000100\n"
                            "    connect \\Y \\y5\n"
                            "  end\n"
                            "  cell $mul \\c6\n"
                            "    parameter \\A_WIDTH 8\n"
                            "    parameter \\B_WIDTH 8\n"
                            "    parameter \\Y_WIDTH 8\n"
                            "    connect \\A \\s8\n"
                            "    connect \\B { \\b3 5'00000 }\n"
                            "    connect \\Y \\y6\n"
                            "  end\n"
                            "  cell $pmux \\c7\n"
                            "    parameter \\S_WIDTH 2\n"
                            "    connect \\A \\p\n"
                            "    connect \\B { 1'x \\q }\n"
                            "    connect \\S { \\s0 \\s1 }\n"
                            "    connect \\Y \\y7\n"
                            "  end\n"
                            "  cell $eq \\c9\n"
                            "    parameter \\A_WIDTH 2\n"
                            "    parameter \\B_WIDTH 2\n"
                            "    connect \\A { \\a \\p }\n"
                            "    connect \\B { \\a \\q }\n"
                            "    connect \\Y \\u [0]\n"
                            "  end\n"
                            "end\n";

/* --- logic rules (§4.2) ------------------------------------------------------------------------
 */

/* yosys -p "read_rtlil const_and.il; opt_expr" */
static void test_const_and(void) {
    build_run(K_CONST_AND);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_STRING("$_CONST0_", driver_type(wnet("y", 0)));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_AND));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.rewritten);
}

/* yosys -p "read_rtlil and_bit_inverse.il; opt_expr" */
static void test_and_bit_inverse(void) {
    build_run(K_AND_BIT_INVERSE);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not")); /* opt_clean's, not opt_expr's */
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_AND));
}

/*
 * yosys -p "read_rtlil consume_x.il; opt_expr" and "...; opt_expr -keepdc". The 1-bit $and(a, x)
 * is bitwise_logic_one_const's already in the first round; consume_x_reduce.il, where no such rule
 * applies, needs the consume_x round for const_and.
 */
static void test_consume_x_round(void) {
    build_run(K_CONSUME_X);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST));
    memset(&fx.counts, 0, sizeof fx.counts);
    build_run(K_CONSUME_X_REDUCE);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_AND));
    memset(&fx.counts, 0, sizeof fx.counts);
    fx.opts.keepdc = true;
    build_run(K_CONSUME_X);
    expect_wire("y", 1, "?");
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(0, rules_total());
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.rewritten + fx.counts.folded);
    build_run(K_CONSUME_X_REDUCE);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$reduce_and"));
    TEST_ASSERT_EQUAL_UINT32(0, rules_total());
}

/* yosys -p "read_rtlil and_or_buffer.il; opt_expr" */
static void test_and_or_buffer(void) {
    build_run(K_AND_OR_BUFFER);
    expect_same(wnet("y", 0), wnet("a", 0), "y is a");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$reduce_or"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_AND_OR_BUFFER));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.rewritten);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.folded);
}

/* yosys -p "read_rtlil xor_buffer.il; opt_expr": a new $not drives y */
static void test_xor_buffer(void) {
    build_run(K_XOR_BUFFER);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$xor"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not"));
    TEST_ASSERT_EQUAL_STRING("$not", driver_type(wnet("y", 0)));
    odin3_node_id inv = need_type("$not");
    expect_same(pnet(inv, "A", 0), wnet("a", 0), "$not reads a");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_XOR_BUFFER));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.rewritten);
}

/* yosys -p "read_rtlil xnor_buffer.il; opt_expr": y2 is {1'1, a} */
static void test_xnor_buffer(void) {
    build_run(K_XNOR_BUFFER);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$xnor"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not"));
    TEST_ASSERT_EQUAL_STRING("$not", driver_type(wnet("y1", 0)));
    expect_same(wnet("y2", 0), wnet("a", 0), "y2[0] is a");
    TEST_ASSERT_EQUAL_CHAR('1', cbit(wnet("y2", 1)));
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_XNOR_BUFFER));
}

/* yosys -p "read_rtlil reduce_xnor_not.il; opt_expr" */
static void test_reduce_xnor_not(void) {
    build_run(K_REDUCE_XNOR_NOT);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$reduce_xnor"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not"));
    odin3_node_id inv = need_type("$not");
    expect_same(pnet(inv, "A", 0), wnet("a", 0), "$not reads a");
    expect_same(pnet(inv, "Y", 0), wnet("y", 0), "$not drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_REDUCE_XNOR_NOT));
}

/*
 * yosys -p "read_rtlil unary_buffer.il; opt_expr": a 1-bit $reduce_and is and_or_buffer's (the
 * first rule); unary_buffer_xor.il has the ones unary_buffer takes.
 */
static void test_unary_buffer(void) {
    build_run(K_UNARY_BUFFER);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$reduce_and"));
    expect_same(wnet("y", 0), wnet("a", 0), "y is a");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_AND_OR_BUFFER));
    build_run(K_UNARY_BUFFER_XOR);
    expect_same(wnet("y", 0), wnet("a", 0), "$reduce_xor(a) is a");
    expect_same(wnet("z", 0), wnet("a", 0), "$neg(a) is a");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$reduce_xor") + count_type("$neg"));
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_UNARY_BUFFER));
}

/* yosys -p "read_rtlil const_xor_xnor.il; opt_expr" and "...; opt_expr -keepdc" */
static void test_const_xor_xnor(void) {
    build_run(K_CONST_XOR_XNOR);
    expect_wire("y1", 1, "0");
    expect_wire("y2", 1, "1");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_XOR));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_XNOR));
    memset(&fx.counts, 0, sizeof fx.counts);
    fx.opts.keepdc = true;
    build_run(K_CONST_XOR_XNOR);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$xor"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$xnor"));
    TEST_ASSERT_EQUAL_UINT32(0, rules_total());
}

/* Review Focus 1. yosys -p "read_rtlil bitwise_or_x.il; opt_expr": y = {1, a2[0]} */
static void test_bitwise_or_x(void) {
    build_run(K_BITWISE_OR_X);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$or"));
    TEST_ASSERT_EQUAL_CHAR('1', cbit(wnet("y", 1)));
    expect_same(wnet("y", 0), wnet("a2", 0), "y[0] is a2[0]");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST));
}

/* Review Focus 1. yosys -p "read_rtlil bitwise_and_x.il; opt_expr": y = {a4[3], 0, 0, a4[0]} */
static void test_bitwise_and_x(void) {
    build_run(K_BITWISE_AND_X);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    expect_same(wnet("y", 3), wnet("a4", 3), "y[3]");
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 2)));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 1)));
    expect_same(wnet("y", 0), wnet("a4", 0), "y[0]");
}

/* Review Focus 1. yosys -p "read_rtlil bitwise_xor_x.il; opt_expr": y[0] = ~a2[0], y[1] = x */
static void test_bitwise_xor_x(void) {
    build_run(K_BITWISE_XOR_X);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$xor"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not"));
    TEST_ASSERT_EQUAL_STRING("$not", driver_type(wnet("y", 0)));
    expect_same(pnet(need_type("$not"), "A", 0), wnet("a2", 0), "$not reads a2[0]");
    TEST_ASSERT_EQUAL_CHAR('x', cbit(wnet("y", 1)));
    TEST_ASSERT_EQUAL_STRING("$_CONSTX_", driver_type(wnet("y", 1)));
}

/*
 * Review Focus 1. yosys -p "read_rtlil bitwise_and_x.il; opt_expr -keepdc": the x bit keeps a
 * 1-bit $and. Yosys's module->And(Const(x), b) puts the x on A and a4[1] on B (the brief said B).
 */
static void test_keepdc_keeps_x(void) {
    fx.opts.keepdc = true;
    build_run(K_BITWISE_AND_X);
    expect_same(wnet("y", 3), wnet("a4", 3), "y[3]");
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 2)));
    expect_same(wnet("y", 0), wnet("a4", 0), "y[0]");
    TEST_ASSERT_EQUAL_STRING("$and", driver_type(wnet("y", 1)));
    odin3_node_id gate = need_type("$and");
    TEST_ASSERT_EQUAL_UINT32(1, pwidth(gate, "Y"));
    TEST_ASSERT_EQUAL_CHAR('x', cbit(pnet(gate, "A", 0)));
    expect_same(pnet(gate, "B", 0), wnet("a4", 1), "$and reads a4[1]");
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and"));
}

/* A signed 1-bit $and is not the unsigned buffer case: bitwise_logic_one_const takes it.
 * yosys -p "read_rtlil signed_and_one_bit.il; opt_expr" */
static void test_bitwise_signed_one_bit(void) {
    build_run(K_SIGNED_AND_ONE_BIT);
    expect_same(wnet("y", 0), wnet("a", 0), "a & 1 is a");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST));
}

/* yosys -p "read_rtlil invert_double.il; opt_expr" */
static void test_invert_double(void) {
    build_run(K_INVERT_DOUBLE);
    expect_same(wnet("y", 0), wnet("a", 0), "y is a");
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$not"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_INVERT_DOUBLE));
}

/* yosys -p "read_rtlil invert_muxsel.il; opt_expr": A and B swapped, S = s */
static void test_invert_muxsel(void) {
    build_run(K_INVERT_MUXSEL);
    odin3_node_id mux = need_type("$mux");
    expect_same(pnet(mux, "A", 0), wnet("q", 0), "A is q");
    expect_same(pnet(mux, "B", 0), wnet("p", 0), "B is p");
    expect_same(pnet(mux, "S", 0), wnet("s", 0), "S is s");
    expect_same(pnet(mux, "Y", 0), wnet("y", 0), "Y is y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_INVERT_MUXSEL));
}

/* yosys -p "read_rtlil tribuf_en1.il; opt_expr" and tribuf_en0.il */
static void test_tribuf(void) {
    build_run(K_TRIBUF_EN1);
    expect_same(wnet("y", 0), wnet("a", 0), "y is a");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_TRIBUF_EN1));
    build_run(K_TRIBUF_EN0);
    odin3_node_id buf = need_type("$tribuf");
    expect_port(buf, "A", "x");
    expect_same(pnet(buf, "Y", 0), wnet("y", 0), "Y is y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_TRIBUF_EN0));
    memset(&fx.counts, 0, sizeof fx.counts);
    run_expr(); /* the A bits are all x now: nothing to do */
    TEST_ASSERT_EQUAL_UINT32(0, rules_total());
}

/* yosys -p "read_rtlil gate_*.il; opt_expr" (the tables of opt_expr.cc 1100-1190) */
static void test_gate_tables(void) {
    build_run(K_GATE_AND0);
    expect_wire("y", 1, "0");
    build_run(K_GATE_AND1);
    expect_same(wnet("y", 0), wnet("a", 0), "1 & a is a");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_AND_"));
    build_run(K_GATE_OR1);
    expect_wire("y", 1, "1");
    build_run(K_GATE_OR0);
    expect_same(wnet("y", 0), wnet("a", 0), "a | 0 is a");
    build_run(K_GATE_XOR);
    expect_wire("y", 1, "0");
    expect_wire("z", 1, "0");
    build_run(K_GATE_NOT);
    expect_wire("y", 1, "0");
    expect_wire("z", 1, "1");
    expect_wire("w", 1, "x");
    build_run(K_GATE_MUX_SAME);
    expect_same(wnet("y", 0), wnet("a", 0), "mux(a, a) is a");
    build_run(K_GATE_MUX_BUF);
    expect_same(wnet("y", 0), wnet("s", 0), "mux(0, 1, s) is s");
    build_run(K_GATE_MUX_SEL);
    expect_same(wnet("y", 0), wnet("a", 0), "S = 0 picks A");
    expect_same(wnet("z", 0), wnet("b", 0), "S = 1 picks B");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_MUX_"));
}

static void test_gate_and_x(void) {
    build_run(K_GATE_ANDX); /* the consume_x round */
    expect_wire("y", 1, "0");
    fx.opts.keepdc = true;
    build_run(K_GATE_ANDX);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_AND_"));
}

/* yosys -p "read_rtlil gate_mux_inv.il; opt_expr": the $_MUX_ becomes a $_NOT_ of S */
static void test_gate_mux_inv(void) {
    build_run(K_GATE_MUX_INV);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_MUX_"));
    odin3_node_id inv = need_type("$_NOT_");
    expect_same(pnet(inv, "A", 0), wnet("s", 0), "$_NOT_ reads s");
    expect_same(pnet(inv, "Y", 0), wnet("y", 0), "$_NOT_ drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_TO_INV));
}

/* yosys -p "read_rtlil gate_mux_undef.il; opt_expr -mux_undef", and without the option */
static void test_gate_mux_undef(void) {
    fx.opts.mux_undef = true;
    build_run(K_GATE_MUX_UNDEF);
    expect_same(wnet("y", 0), wnet("a", 0), "an x B input is dropped");
    memset(&fx.opts, 0, sizeof fx.opts);
    build_run(K_GATE_MUX_UNDEF_OFF);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_MUX_"));
}

/* --- comparisons, shifts, arithmetic (§4.3) ------------------------------------------------------
 */

/* yosys -p "read_rtlil xbit_arith.il; opt_expr" */
static void test_xbit_arith(void) {
    build_run(K_XBIT_ARITH);
    expect_wire("y", 4, "xxxx");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_XBIT));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded);
}

/* yosys -p "read_rtlil xbit_cmp.il; opt_expr": Y_WIDTH 2, Y = {0, x} */
static void test_xbit_cmp(void) {
    build_run(K_XBIT_CMP);
    expect_wire("y", 2, "0x");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lt"));
}

/* yosys -p "read_rtlil eqneq_isneq.il; opt_expr" */
static void test_eqneq_isneq(void) {
    build_run(K_EQNEQ_ISNEQ);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$eq"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_ISNEQ));
}

/* yosys -p "read_rtlil eqneq_empty.il; opt_expr" */
static void test_eqneq_empty(void) {
    build_run(K_EQNEQ_EMPTY);
    expect_wire("y", 1, "0");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$ne"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_EMPTY));
}

/* yosys -p "read_rtlil eqneq_resize.il; opt_expr": a 1-bit $eq(c, d) */
static void test_eqneq_resize(void) {
    build_run(K_EQNEQ_RESIZE);
    odin3_node_id eq = need_type("$eq");
    TEST_ASSERT_EQUAL_INT64(1, pparam(eq, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, pparam(eq, "B_WIDTH"));
    expect_same(pnet(eq, "A", 0), wnet("c", 0), "A is c");
    expect_same(pnet(eq, "B", 0), wnet("d", 0), "B is d");
    expect_same(pnet(eq, "Y", 0), wnet("y", 0), "Y is y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_RESIZE));
}

/* yosys -p "read_rtlil eqneq_swapconst_isnot.il; opt_expr": $eq(1'0, a) is $not(a) */
static void test_eqneq_swapconst_isnot(void) {
    build_run(K_EQNEQ_SWAPCONST_ISNOT);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$eq"));
    odin3_node_id inv = need_type("$not");
    expect_same(pnet(inv, "A", 0), wnet("a", 0), "$not reads a");
    expect_same(pnet(inv, "Y", 0), wnet("y", 0), "$not drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_SWAPCONST));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_ISNOT));
}

/* yosys -p "read_rtlil eqneq_buffer.il; opt_expr" and eqneq_undef.il */
static void test_eqneq_buffer_undef(void) {
    build_run(K_EQNEQ_BUFFER);
    expect_same(wnet("y", 0), wnet("a", 0), "$eq(a, 1) is a");
    expect_same(wnet("z", 0), wnet("a", 0), "$ne(0, a) is a");
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_EQNEQ_BUFFER));
    build_run(K_EQNEQ_UNDEF);
    expect_wire("y", 1, "x");
}

/* yosys -p "read_rtlil eqneq_cmpzero.il; opt_expr" */
static void test_eqneq_cmpzero(void) {
    build_run(K_EQNEQ_CMPZERO);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$eq") + count_type("$ne"));
    odin3_node_id lnot = need_type("$logic_not");
    odin3_node_id rbool = need_type("$reduce_bool");
    TEST_ASSERT_EQUAL_INT64(4, pparam(lnot, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(4, pparam(rbool, "A_WIDTH"));
    expect_port_is_wire(lnot, "A", "a4", 0);
    expect_port_is_wire(rbool, "A", "a4", 0);
    expect_same(pnet(lnot, "Y", 0), wnet("y", 0), "$logic_not drives y");
    expect_same(pnet(rbool, "Y", 0), wnet("z", 0), "$reduce_bool drives z");
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_EQNEQ_CMPZERO));
}

/* yosys -p "read_rtlil constshift_shl.il; opt_expr": y = {a4[2], a4[1], a4[0], 0} */
static void test_constshift(void) {
    build_run(K_CONSTSHIFT_SHL);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$shl"));
    expect_same(wnet("y", 3), wnet("a4", 2), "y[3]");
    expect_same(wnet("y", 2), wnet("a4", 1), "y[2]");
    expect_same(wnet("y", 1), wnet("a4", 0), "y[1]");
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 0)));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONSTSHIFT));
    /* constshift_sshr.il: the sign bit fills */
    build_run(K_CONSTSHIFT_SSHR);
    expect_same(wnet("y", 3), wnet("s4", 3), "y[3]");
    expect_same(wnet("y", 2), wnet("s4", 3), "y[2]");
    expect_same(wnet("y", 1), wnet("s4", 2), "y[1]");
    expect_same(wnet("y", 0), wnet("s4", 1), "y[0]");
    /* constshift_shiftx.il: the top bit is x */
    build_run(K_CONSTSHIFT_SHIFTX);
    TEST_ASSERT_EQUAL_CHAR('x', cbit(wnet("y", 3)));
    expect_same(wnet("y", 2), wnet("a4", 3), "y[2]");
    expect_same(wnet("y", 1), wnet("a4", 2), "y[1]");
    expect_same(wnet("y", 0), wnet("a4", 1), "y[0]");
    /* constshift_wide.il: a shift past A's width saturates (all 0) */
    build_run(K_CONSTSHIFT_WIDE);
    expect_wire("y", 4, "0000");
}

/* yosys -p "read_rtlil shiftx_trim.il; opt_expr": A narrowed to a2 */
static void test_shiftx_trim(void) {
    build_run(K_SHIFTX_TRIM);
    odin3_node_id shift = need_type("$shiftx");
    TEST_ASSERT_EQUAL_INT64(2, pparam(shift, "A_WIDTH"));
    expect_port_is_wire(shift, "A", "a2", 0);
    expect_port_is_wire(shift, "B", "b", 0);
    expect_same(pnet(shift, "Y", 0), wnet("y", 0), "Y is y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_TRIM));
}

/* yosys -p "read_rtlil identwrt_add.il; opt_expr" (Yosys leaves a $pos: wiring here) */
static void test_identwrt(void) {
    build_run(K_IDENTWRT_ADD);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$add"));
    for (uint32_t k = 0; k < 4; k++) {
        expect_same(wnet("y", k), wnet("a4", k), "y is a4");
    }
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_IDENTWRT_A));
    build_run(K_IDENTWRT_MUL);
    for (uint32_t k = 0; k < 4; k++) {
        expect_same(wnet("y", k), wnet("a4", k), "$mul by 1 is a4");
    }
    /* identwrt_neg.il: signed times -1 is $neg */
    build_run(K_IDENTWRT_NEG);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mul"));
    odin3_node_id neg = need_type("$neg");
    TEST_ASSERT_EQUAL_INT64(1, pparam(neg, "A_SIGNED"));
    TEST_ASSERT_EQUAL_INT64(4, pparam(neg, "A_WIDTH"));
    expect_port_is_wire(neg, "A", "s4", 0);
    expect_same(pnet(neg, "Y", 0), wnet("y", 0), "$neg drives y");
    /* div_identity.il: $div by 1 */
    build_run(K_DIV_IDENTITY);
    for (uint32_t k = 0; k < 4; k++) {
        expect_same(wnet("y", k), wnet("a4", k), "$div by 1 is a4");
    }
}

/* -keepdc has no consume_x round: a + 0 stays (identwrt is consume_x only) */
static void test_identwrt_needs_consume_x(void) {
    fx.opts.keepdc = true;
    build_run(K_IDENTWRT_ADD);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$add"));
}

/* yosys -p "read_rtlil pow_shift_two.il; opt_expr": $pow(2, b3) is $shl(1, b3) */
static void test_pow_shift_two(void) {
    build_run(K_POW_SHIFT_TWO);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pow"));
    odin3_node_id shl = need_type("$shl");
    TEST_ASSERT_EQUAL_INT64(1, pparam(shl, "A_WIDTH"));
    TEST_ASSERT_EQUAL_CHAR('1', cbit(pnet(shl, "A", 0)));
    expect_port_is_wire(shl, "B", "b3", 0);
    expect_same(pnet(shl, "Y", 0), wnet("y", 0), "$shl drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_POW_SHIFT));
}

/* A signed base: "$pow(4'sb0010, b3)" is "1 << b3" with A_SIGNED 0 (Yosys 0.55 leaves A_SIGNED 1 on
 * the 1-bit A = 1, which sign-extends to -1: yosys -p "read_rtlil powsigned.il; opt_expr" shows
 * `$shl` with A_SIGNED 1 and A 1'1), and a base whose one-hot bit is its sign is not a power of 2.
 */
static const char K_POW_SIGNED[] = "module \\m\n  wire width 3 input 1 \\b3\n"
                                   "  wire width 8 output 2 \\y\n  cell $pow \\c\n"
                                   "    parameter \\A_SIGNED 1\n    parameter \\A_WIDTH 4\n"
                                   "    parameter \\B_WIDTH 3\n    parameter \\Y_WIDTH 8\n"
                                   "    connect \\A 4'%s\n    connect \\B \\b3\n"
                                   "    connect \\Y \\y\n  end\nend\n";

static void test_pow_signed_base(void) {
    char text[512];
    (void)snprintf(text, sizeof text, K_POW_SIGNED, "0010");
    build_run(text);
    odin3_node_id shl = need_type("$shl");
    TEST_ASSERT_EQUAL_INT64(0, pparam(shl, "A_SIGNED"));
    TEST_ASSERT_EQUAL_INT64(1, pparam(shl, "A_WIDTH"));
    (void)snprintf(text, sizeof text, K_POW_SIGNED, "1000"); /* -8 */
    build_run(text);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$pow"));
}

/* yosys -p "read_rtlil pow_shift_eight.il; opt_expr": $pow(8, b3) is $mul(3, b3) into $shl(1, .) */
static void test_pow_shift_eight(void) {
    build_run(K_POW_SHIFT_EIGHT);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pow"));
    odin3_node_id mul = need_type("$mul");
    odin3_node_id shl = need_type("$shl");
    TEST_ASSERT_EQUAL_INT64(32, pparam(mul, "A_WIDTH"));
    TEST_ASSERT_EQUAL_CHAR('1', cbit(pnet(mul, "A", 0)));
    TEST_ASSERT_EQUAL_CHAR('1', cbit(pnet(mul, "A", 1)));
    for (uint32_t k = 2; k < 32; k++) {
        TEST_ASSERT_EQUAL_CHAR('0', cbit(pnet(mul, "A", k)));
    }
    expect_port_is_wire(mul, "B", "b3", 0);
    TEST_ASSERT_EQUAL_INT64(8, pparam(mul, "Y_WIDTH"));
    TEST_ASSERT_EQUAL_CHAR('1', cbit(pnet(shl, "A", 0)));
    TEST_ASSERT_EQUAL_INT64(8, pparam(shl, "B_WIDTH"));
    for (uint32_t k = 0; k < 8; k++) {
        expect_same(pnet(shl, "B", k), pnet(mul, "Y", k), "$shl reads the $mul");
    }
    expect_same(pnet(shl, "Y", 0), wnet("y", 0), "$shl drives y");
}

/* yosys -p "read_rtlil mul_shift.il; opt_expr": $mul by 4 is a $shl by 2, then wiring */
static void test_mul_shift(void) {
    build_run(K_MUL_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mul") + count_type("$shl"));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 0)));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 1)));
    expect_same(wnet("y", 2), wnet("a4", 0), "y[2]");
    expect_same(wnet("y", 3), wnet("a4", 1), "y[3]");
    expect_wire("z", 4, "0000");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUL_SHIFT_SWAPPED)); /* B is the constant */
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUL_SHIFT_ZERO));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONSTSHIFT));
    memset(&fx.counts, 0, sizeof fx.counts);
    build_run(K_MUL_SHIFT_A_CONST);
    expect_same(wnet("y", 2), wnet("a4", 0), "A-constant y[2]");
    expect_same(wnet("y", 3), wnet("a4", 1), "A-constant y[3]");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUL_SHIFT_UNSWAPPED));
}

/* -keepdc: no mul_shift, no mul_low_zeros, no div_shift (!keepdc rules) */
static void test_keepdc_arith(void) {
    fx.opts.keepdc = true;
    build_run(K_MUL_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$mul"));
    build_run(K_DIV_SHIFT_MOD_MASK);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$div"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mod"));
}

/* yosys -p "read_rtlil mul_low_zeros.il; opt_expr": a narrower $mul, y[1:0] = 0 */
static void test_mul_low_zeros(void) {
    build_run(K_MUL_LOW_ZEROS);
    odin3_node_id mul = need_type("$mul");
    TEST_ASSERT_EQUAL_INT64(4, pparam(mul, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(2, pparam(mul, "B_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(6, pparam(mul, "Y_WIDTH"));
    expect_port_is_wire(mul, "A", "a4", 0);
    expect_port_is_wire(mul, "B", "b2", 0);
    expect_port_is_wire(mul, "Y", "y", 2);
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 0)));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 1)));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUL_LOW_ZEROS));
    TEST_ASSERT_EQUAL_STRING("$mul", driver_type(wnet("y", 7)));
}

/* yosys -p "read_rtlil divmod_zero.il; opt_expr" (also with -keepdc) */
static void test_divmod_zero(void) {
    build_run(K_DIVMOD_ZERO);
    expect_wire("y", 4, "xxxx");
    expect_wire("z", 4, "xxxx");
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_DIVMOD_ZERO));
    fx.opts.keepdc = true;
    build_run(K_DIVMOD_ZERO);
    expect_wire("y", 4, "xxxx");
}

/* yosys -p "read_rtlil div_shift_mod_mask.il; opt_expr": a $sshr by 2 and an $and with 2'b11,
 * which fold to wiring */
static void test_div_shift_mod_mask(void) {
    build_run(K_DIV_SHIFT_MOD_MASK);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$div") + count_type("$mod") + count_type("$sshr") +
                                    count_type("$and"));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 7)));
    TEST_ASSERT_EQUAL_CHAR('0', cbit(wnet("y", 6)));
    for (uint32_t k = 0; k < 6; k++) {
        expect_same(wnet("y", k), wnet("a8", k + 2), "y is a8 >> 2");
    }
    expect_wire("z", 8, "000000??");
    expect_same(wnet("z", 0), wnet("a8", 0), "z[0]");
    expect_same(wnet("z", 1), wnet("a8", 1), "z[1]");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_DIV_SHIFT));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MOD_MASK));
}

/* yosys -p "read_rtlil const_fold.il; opt_expr" */
static void test_const_fold(void) {
    build_run(K_CONST_FOLD);
    expect_wire("y", 4, "1000");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded);
    TEST_ASSERT_EQUAL_UINT32(0, rules_total()); /* a fold is counted by const.$<type> only */
}

/*
 * yosys -p "read_rtlil div_shift_signed.il; opt_expr": Yosys 0.55's truncating corrections.
 *   $div by 4: flooring = A >>> 2 ($sshr, signed), $add(flooring, A[7] & |A[1:0])
 *   $mod by 4: {A[7] & |A[1:0], A[1:0]} sign-extended ($pos, signed: wiring here)
 * The $sshr is a constshift by the time opt_expr is done, so the flooring is wiring.
 */
static void test_div_shift_signed_structure(void) {
    build_run(K_DIV_SHIFT_SIGNED);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$div") + count_type("$mod") + count_type("$sshr"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$reduce_or"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$and"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$add"));
    odin3_node_id add = need_type("$add");
    TEST_ASSERT_EQUAL_INT64(8, pparam(add, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, pparam(add, "B_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(0, pparam(add, "A_SIGNED"));
    for (uint32_t k = 0; k < 8; k++) { /* flooring = {s8[7], s8[7], s8[7:2]} */
        expect_same(pnet(add, "A", k), wnet("s8", k < 6 ? k + 2 : 7), "flooring");
    }
    expect_same(pnet(add, "Y", 0), wnet("y", 0), "$add drives y");
    odin3_node_id fix = driver_node(pnet(add, "B", 0));
    TEST_ASSERT_EQUAL_STRING("$and", type_name(fix));
    expect_same(pnet(fix, "A", 0), wnet("s8", 7), "sign");
    odin3_node_id rem = driver_node(pnet(fix, "B", 0));
    TEST_ASSERT_EQUAL_STRING("$reduce_or", type_name(rem));
    TEST_ASSERT_EQUAL_INT64(2, pparam(rem, "A_WIDTH"));
    expect_port_is_wire(rem, "A", "s8", 0);
    expect_same(wnet("z", 0), wnet("s8", 0), "z[0]");
    expect_same(wnet("z", 1), wnet("s8", 1), "z[1]");
    odin3_node_id ext = driver_node(wnet("z", 2));
    TEST_ASSERT_EQUAL_STRING("$and", type_name(ext));
    TEST_ASSERT_TRUE(ext.v != fix.v);
    for (uint32_t k = 3; k < 8; k++) {
        expect_same(wnet("z", k), wnet("z", 2), "z is sign-extended");
    }
    expect_same(pnet(ext, "A", 0), wnet("s8", 7), "sign");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_DIV_SHIFT));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MOD_MASK));
}

/* Holds the 8-bit input port `wire` to the constant `value`: its pins let go and its nets join the
 * constants' (as read_rtlil's `connect \\wire 8'...` would have given before any rewrite). The port
 * node no longer drives its wire then, which odin3_check_design's rule 9 rejects: callers skip it.
 */
static void hold_input(const char *wire, uint32_t value) {
    odin3_opt_ctx oc;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_ctx_init(&oc, &fx.ctx, fx.res.module, &fx.opts));
    odin3_wire_id wire_id = odin3_module_find_wire(fx.res.module, intern(wire));
    odin3_pinslice pins =
        odin3_node_pins(fx.res.module, odin3_wire_port_node(fx.res.module, wire_id));
    TEST_ASSERT_EQUAL_UINT32(8, pins.count);
    for (uint32_t k = 0; k < 8; k++) {
        odin3_net_id net = wnet(wire, k);
        odin3_net_id const_net = {0};
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_opt_const_net(&oc, (value >> k & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0,
                                          &const_net));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_pin_disconnect(fx.res.module, (odin3_pin_id){pins.first.v + k}));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_net_merge(fx.res.module, (odin3_net_pair){const_net, net}));
    }
    odin3_opt_ctx_free(&oc);
}

/* The 8 bits of wire as a number (0..255); every bit must be 0 or 1. */
static uint32_t wire_value(const char *wire) {
    char bits[BITS_MAX];
    wbits(wire, 8, bits);
    uint32_t value = 0;
    for (size_t k = 0; k < 8; k++) {
        TEST_ASSERT_TRUE_MESSAGE(bits[k] == '0' || bits[k] == '1', bits);
        value = value << 1 | (bits[k] == '1' ? 1U : 0U);
    }
    return value;
}

/*
 * 256 values of s8: opt_expr rewrites the $div and $mod by 4 with s8 free; the rewritten cells,
 * with s8 then held to a value and folded (the 2D evaluator, Yosys's calc.cc), must give what C's
 * truncating / and % give on int8_t. (1E's simulator has no $div, $mod or $sshr, so the evaluator
 * is the reference here.)
 */
static void test_div_shift_signed_values(void) {
    for (uint32_t value = 0; value < VALUES; value++) {
        build_run(K_DIV_SHIFT_SIGNED);
        hold_input("s8", value);
        n_lines = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_opt_expr_run(&fx.ctx, fx.res.module, &fx.opts, &fx.counts));
        int32_t signed_value = value < VALUES / 2 ? (int32_t)value : (int32_t)value - VALUES;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(signed_value / 4) & 0xFFU, wire_value("y"),
                                         "div");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(signed_value % 4) & 0xFFU, wire_value("z"),
                                         "mod");
    }
}

/* --- mux rules -------------------------------------------------------------------------------- */

/* Review Focus 1. yosys -p "read_rtlil mux_s_x_picks_a.il; opt_expr" and "...; opt_expr -keepdc" */
static void test_mux_s_x_picks_a(void) {
    build_run(K_MUX_S_X_PICKS_A);
    expect_same(wnet("y", 0), wnet("a", 0), "S = x picks A");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mux"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_CONST_SEL));
    fx.opts.keepdc = true;
    build_run(K_MUX_S_X_PICKS_A);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
}

/* yosys -p "read_rtlil mux_same.il; opt_expr" and mux_const_sel.il */
static void test_mux_same(void) {
    build_run(K_MUX_SAME);
    expect_same(wnet("y", 0), wnet("a", 0), "mux(a, a) is a");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_SAME));
    build_run(K_MUX_CONST_SEL);
    expect_same(wnet("y", 0), wnet("b", 0), "S = 1 picks B");
}

/* yosys -p "read_rtlil mux_bool_buf.il; opt_expr -mux_bool", mux_bool_inv.il, mux_bool_off.il */
static void test_mux_bool(void) {
    fx.opts.mux_bool = true;
    build_run(K_MUX_BOOL_BUF);
    expect_same(wnet("y", 0), wnet("s", 0), "mux(0, 1, s) is s");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_BOOL));
    build_run(K_MUX_BOOL_INV);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mux"));
    odin3_node_id inv = need_type("$not");
    expect_same(pnet(inv, "A", 0), wnet("s", 0), "$not reads s");
    expect_same(pnet(inv, "Y", 0), wnet("y", 0), "$not drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_INVERT));
    memset(&fx.opts, 0, sizeof fx.opts);
    build_run(K_MUX_BOOL_OFF);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$mux"));
}

/* yosys -p "read_rtlil mux_and_or.il; opt_expr -mux_bool": $mux(0, b, s) is $and(s, b) */
static void test_mux_and_or(void) {
    fx.opts.mux_bool = true;
    build_run(K_MUX_AND_OR);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$mux"));
    odin3_node_id gate_and = need_type("$and");
    odin3_node_id gate_or = need_type("$or");
    expect_same(pnet(gate_and, "A", 0), wnet("s", 0), "$and reads s");
    expect_same(pnet(gate_and, "B", 0), wnet("b", 0), "$and reads b");
    expect_same(pnet(gate_and, "Y", 0), wnet("y", 0), "$and drives y");
    expect_same(pnet(gate_or, "A", 0), wnet("a", 0), "$or reads a");
    expect_same(pnet(gate_or, "B", 0), wnet("s", 0), "$or reads s");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_AND));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_OR));
    memset(&fx.counts, 0, sizeof fx.counts);
    fx.opts.keepdc = true; /* mux_and and mux_or are consume_x rules */
    build_run(K_MUX_AND_OR);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$mux"));
}

/* yosys -p "read_rtlil mux_undef.il; opt_expr -mux_undef": the x slice goes, $pmux is a $mux */
static void test_mux_undef(void) {
    fx.opts.mux_undef = true;
    build_run(K_MUX_UNDEF);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    odin3_node_id mux = need_type("$mux");
    expect_same(pnet(mux, "A", 0), wnet("a", 0), "A is a");
    expect_same(pnet(mux, "B", 0), wnet("b", 0), "B is b");
    expect_same(pnet(mux, "S", 0), wnet("s1", 0), "S is s1");
    expect_same(pnet(mux, "Y", 0), wnet("y", 0), "Y is y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_REDUCE));
    memset(&fx.opts, 0, sizeof fx.opts);
    build_run(K_MUX_UNDEF);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$pmux")); /* without -mux_undef nothing happens */
}

/* yosys -p "read_rtlil mux_undef_allx.il; opt_expr -mux_undef": A and B all x, Y = A */
static void test_mux_undef_all_x(void) {
    fx.opts.mux_undef = true;
    build_run(K_MUX_UNDEF_ALLX);
    expect_wire("y", 2, "xx");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_UNDEF));
}

/* yosys -p "read_rtlil mux_sel01.il; opt_expr -mux_undef" and mux_empty.il */
static void test_mux_sel01_empty(void) {
    fx.opts.mux_undef = true;
    build_run(K_MUX_SEL01);
    expect_same(wnet("y", 0), wnet("s", 0), "mux(0, 1, s) is s");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_SEL01));
    build_run(K_MUX_EMPTY);
    expect_same(wnet("y", 0), wnet("b", 0), "an x A takes the last B slice");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_EMPTY));
}

/*
 * Yosys's eqneq.resize and mux_reduce set no `goto next_cell`: the visit goes on with the rewritten
 * cell. yosys -p "read_rtlil eqneq_resize_resumes.il; opt_expr" leaves a $not(c) (the narrowed
 * $eq(c, 1'0) is eqneq.isnot in the same visit).
 */
static void test_eqneq_resize_resumes(void) {
    build_run(K_EQNEQ_RESIZE_RESUMES);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$eq"));
    odin3_node_id inv = need_type("$not");
    expect_same(pnet(inv, "A", 0), wnet("c", 0), "$not reads c");
    expect_same(pnet(inv, "Y", 0), wnet("y", 0), "$not drives y");
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_RESIZE));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_EQNEQ_ISNOT));
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.rewritten); /* two rewrites in one visit */
}

/*
 * yosys -p "read_rtlil mux_reduce_resumes.il; opt_expr -mux_undef": the $pmux loses its undefined
 * slice and becomes a $mux of constants, which the same visit folds, so the $xor after it reads 0
 * and folds to x in the same call (it would be left with an open Y bit otherwise).
 */
static void test_mux_reduce_resumes(void) {
    fx.opts.mux_undef = true;
    build_run(K_MUX_REDUCE_RESUMES);
    expect_wire("y1", 1, "0");
    expect_wire("y3", 1, "x");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$pmux") + count_type("$mux") + count_type("$xor"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_MUX_REDUCE));
}

/* --- replace_undriven (§4.6) ------------------------------------------------------------------ */

/* yosys -p "read_rtlil undriven.il; opt_expr -undriven": w = 2'b1x, the init attribute is gone */
static void test_undriven(void) {
    fx.opts.undriven = true;
    build_run(K_UNDRIVEN);
    TEST_ASSERT_EQUAL_CHAR('1', cbit(wnet("w", 1)));
    TEST_ASSERT_EQUAL_CHAR('x', cbit(wnet("w", 0)));
    TEST_ASSERT_EQUAL_STRING("$_CONST1_", driver_type(wnet("w", 1)));
    TEST_ASSERT_EQUAL_STRING("$_CONSTX_", driver_type(wnet("w", 0)));
    odin3_wire_id wire = odin3_module_find_wire(fx.res.module, intern("w"));
    TEST_ASSERT_NULL(
        odin3_attr_get(fx.res.module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, intern("init")));
    expect_wire("y", 2, "0x"); /* the $not of 1x folds */
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_UNDRIVEN));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded);
    /* without the option the wire stays undriven and keeps its init */
    memset(&fx.opts, 0, sizeof fx.opts);
    memset(&fx.counts, 0, sizeof fx.counts);
    build_run(K_UNDRIVEN);
    TEST_ASSERT_EQUAL_CHAR('?', cbit(wnet("w", 1)));
    TEST_ASSERT_EQUAL_UINT32(0, rule(ODIN3_OPT_RULE_UNDRIVEN));
}

/*
 * yosys -p "read_rtlil undriven_kinds.il; opt_expr -undriven -keepdc": an unread public wire and a
 * $-named wire something reads become x; a $-named wire nothing reads stays as it is.
 */
static void test_undriven_kinds(void) {
    fx.opts.undriven = true;
    fx.opts.keepdc = true;
    build_run(K_UNDRIVEN_KINDS);
    TEST_ASSERT_EQUAL_CHAR('x', cbit(wnet("unread", 0)));
    odin3_node_id gate = need_type("$and");
    TEST_ASSERT_EQUAL_CHAR('x', cbit(pnet(gate, "B", 0)));
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_UNDRIVEN));
}

/* --- order, lines, scope, opacity ----------------------------------------------------------------
 */

/* A 1-bit $and(a, 1'0) matches const_and and bitwise_logic_one_const: only const_and counts. */
static void test_first_match(void) {
    build_run(K_FIRST_MATCH);
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_CONST_AND));
    TEST_ASSERT_EQUAL_UINT32(0, rule(ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST));
    TEST_ASSERT_EQUAL_UINT32(1, rules_total());
}

/* yosys -p "read_rtlil topo_loop.il; opt_expr" prints "Couldn't topologically sort cells" */
static void test_topo_loop_line(void) {
    static const char line[] =
        "opt_expr: module 'm': cells could not be sorted topologically; visiting them in ID order";
    build_run(K_TOPO_LOOP);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, line));
    build_run(K_CONST_AND);
    TEST_ASSERT_EQUAL_UINT32(0, count_lines(ODIN3_LOG_INFO, line));
}

static void test_lines(void) {
    build(K_LINES);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_expr_design(&fx.ctx, fx.res.design, &fx.opts, &counts));
    check_full();
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_expr: 3 cells rewritten, 2 cells folded to constants"));
    TEST_ASSERT_EQUAL_UINT32(3, counts.expr.rewritten);
    TEST_ASSERT_EQUAL_UINT32(2, counts.expr.folded);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_expr: module 'm': const_and on $and '$c5'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_expr: module 'm': const_or on $or '$c6'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_expr: module 'm': xor_buffer on $xor '$c7'"));
    TEST_ASSERT_EQUAL_UINT32(
        1,
        count_lines(ODIN3_LOG_DEBUG, "opt_expr: module 'm': and_or_buffer on $reduce_and '$c8'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG,
                       "opt_expr: module 'm': reduce_xnor_not on $reduce_xnor '$c9'"));
}

/* A fold is logged with its const.$<type> counter name, and counted by type. */
static void test_fold_line_and_counter(void) {
    build(K_CONST_FOLD);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_expr_design(&fx.ctx, fx.res.design, &fx.opts, &counts));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_expr: module 'm': const.$add on $add 'c'"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_expr: 0 cells rewritten, 1 cells folded to constants"));
    uint32_t total = 0;
    for (uint32_t i = 0; i < ODIN3_OPT_FOLD_TYPES; i++) {
        total += counts.fold_type[i];
        if (strcmp(odin3_opt_fold_name(i), "const.$add") == 0) {
            TEST_ASSERT_EQUAL_UINT32(1, counts.fold_type[i]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, total);
    TEST_ASSERT_EQUAL_UINT32(1, counts.expr.folded);
}

static uint32_t live_nodes_of(odin3_module *module, const char *type_text) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(module, node));
        hits += odin3_node_live(module, node) && strcmp(def->name, type_text) == 0 ? 1U : 0U;
    }
    return hits;
}

/* Internal risk 5: odin3_opt_expr_run is module scope, as Yosys's per-module opt_expr. */
static void test_expr_module_scope(void) {
    char text[1 << 12];
    int len = snprintf(text, sizeof text, "%s%s", K_SCOPE_M1, K_SCOPE_M2);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    build(text);
    odin3_module *m1 = pb_module(&fx.res, "m1");
    odin3_module *m2 = pb_module(&fx.res, "m2");
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_NOT_NULL(m2);
    odin3_opt_expr_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.keepdc = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_expr_run(&fx.ctx, m2, &opts, &counts));
    check_full();
    TEST_ASSERT_EQUAL_UINT32(0, live_nodes_of(m2, "$and"));
    TEST_ASSERT_EQUAL_UINT32(1, live_nodes_of(m1, "$and"));
    TEST_ASSERT_EQUAL_UINT32(1, counts.folded);
    TEST_ASSERT_EQUAL_UINT32(0, counts.rewritten);
}

/*
 * A $proc is opaque: not visited, not folded through, its O pin an ordinary driver. The $and reads
 * the process's output and 1'1, so its Y merges with the process's net.
 */
static void test_proc_opaque(void) {
    static const char text[] = "module \\m\n"
                               "  wire input 1 \\a\n"
                               "  wire output 2 \\y\n"
                               "  wire \\procO\n"
                               "  cell $and \\g\n"
                               "    connect \\A \\procO\n"
                               "    connect \\B 1'1\n"
                               "    connect \\Y \\y\n"
                               "  end\n"
                               "  process $p\n"
                               "    sync always\n"
                               "      update \\procO \\a\n"
                               "  end\n"
                               "end\n";
    build(text);
    odin3_node_id proc = pb_proc(&fx.res, "m", 0);
    TEST_ASSERT_TRUE(odin3_node_valid(proc));
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(fx.res.module, proc, &before));
    run_expr();
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$and"));
    expect_same(wnet("y", 0), wnet("procO", 0), "y is procO");
    TEST_ASSERT_EQUAL_STRING("$proc", driver_type(wnet("y", 0)));
    odin3_node_id still = pb_proc(&fx.res, "m", 0);
    TEST_ASSERT_EQUAL_UINT32(proc.v, still.v);
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(fx.res.module, still, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

/* odin3_opt_expr_run adds into its counts. */
static void test_counts_accumulate(void) {
    build_run(K_CONST_AND);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded);
    build_run(K_CONST_AND);
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.folded);
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_CONST_AND));
    build_run(K_XOR_BUFFER);
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.folded);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.rewritten);
}

/* --- Yosys parity found by Task 5 (ruling 1), keep (ruling 2) --------------------------------- */

/* Ruling 1(a). yosys -p "read_rtlil ruling_a.il; opt_expr": a 1-bit $reduce_* is aliased to A
 * before folding, so a z stays z (const_reduce_or would give x). */
static void test_ruling_unary_before_fold(void) {
    build_run(K_RULING_A);
    expect_wire("y", 1, "z");
    expect_wire("z", 1, "z");
    TEST_ASSERT_EQUAL_UINT32(2, rule(ODIN3_OPT_RULE_UNARY_BUFFER));
}

/* Ruling 1(b). yosys -p "read_rtlil ruling_b.il; opt_expr" leaves `connect Y $auto$...Not$2` and
 * `connect $auto$...Not$2 1'1`, the temporary wire tied to a constant. Odin III has one net: y is
 * on the constant, no $not remains and no net is left behind. */
static void test_ruling_temp_wire_tied(void) {
    build_run(K_RULING_B);
    expect_wire("y", 1, "1");
    TEST_ASSERT_EQUAL_STRING("$_CONST1_", driver_type(wnet("y", 0)));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$not") + count_type("$xor"));
    TEST_ASSERT_EQUAL_UINT32(1, rule(ODIN3_OPT_RULE_XOR_BUFFER));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.rewritten);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.folded); /* the helper $not(0), folded */
}

/* Ruling 2, verified against Yosys 0.55: opt_expr does not look at `keep`.
 * yosys -p "read_rtlil keep_add.il; opt_expr" folds the (* keep *) $add to 4'1000. */
static void test_keep_cell_folds(void) {
    build_run(K_KEEP_ADD);
    expect_wire("y", 4, "1000");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$add"));
}

/* --- rule names, misuse, provenance ----------------------------------------------------------- */

/* Rule `index` has a name that no earlier rule has. */
static void expect_unique_name(uint32_t index) {
    const char *name = odin3_opt_rule_name((odin3_opt_rule)index);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(name[0] != '\0');
    for (uint32_t before = 0; before < index; before++) {
        TEST_ASSERT_TRUE(strcmp(name, odin3_opt_rule_name((odin3_opt_rule)before)) != 0);
    }
}

static void test_rule_names(void) {
    for (uint32_t index = 0; index < ODIN3_OPT_RULE_COUNT; index++) {
        expect_unique_name(index);
    }
    TEST_ASSERT_EQUAL_STRING("const_and", odin3_opt_rule_name(ODIN3_OPT_RULE_CONST_AND));
    TEST_ASSERT_EQUAL_STRING("eqneq.isneq", odin3_opt_rule_name(ODIN3_OPT_RULE_EQNEQ_ISNEQ));
    TEST_ASSERT_NULL(odin3_opt_rule_name(ODIN3_OPT_RULE_COUNT));
}

static void test_misuse(void) {
    build(K_CONST_AND);
    odin3_opt_expr_counts counts;
    memset(&counts, 0, sizeof counts);
    odin3_opt_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_opt_counts all;
    memset(&all, 0, sizeof all);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_run(NULL, fx.res.module, &opts, &counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_expr_run(&fx.ctx, NULL, &opts, &counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_run(&fx.ctx, fx.res.module, NULL, &counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_run(&fx.ctx, fx.res.module, &opts, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_design(NULL, fx.res.design, &opts, &all));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_expr_design(&fx.ctx, NULL, &opts, &all));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_design(&fx.ctx, fx.res.design, NULL, &all));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_expr_design(&fx.ctx, fx.res.design, &opts, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and")); /* nothing changed */
}

/* OP-15: a node a rewrite creates carries a DERIVED record whose parent is the old node's. */
static void test_provenance_derived(void) {
    build(K_XOR_BUFFER);
    odin3_node_id old = odin3_module_find_node(fx.res.module, intern("c"));
    odin3_prov_id parent = odin3_node_prov(fx.res.module, old);
    TEST_ASSERT_TRUE(odin3_prov_valid(parent));
    run_expr();
    odin3_node_id inv = need_type("$not");
    const odin3_prov_record *rec =
        odin3_prov_get(fx.res.design, odin3_node_prov(fx.res.module, inv));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(parent.v, rec->parents.ids[0].v);
}

/* --- out of memory -----------------------------------------------------------------------------
 */

static void oom_run_once(long fail, bool *done) {
    memset(&fx.counts, 0, sizeof fx.counts);
    build(K_OOM);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_expr_run(&fx.ctx, fx.res.module, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full(); /* the IR stays valid whichever allocation failed */
}

static void test_expr_oom(void) {
    fx.opts.mux_undef = true;
    fx.opts.undriven = true;
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(failures > 20);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$div") + count_type("$mod") + count_type("$pow"));
    printf("opt_expr OOM: %ld failing allocation points\n", failures);
}

static void run_logic_tests(void) {
    RUN_TEST(test_const_and);
    RUN_TEST(test_and_bit_inverse);
    RUN_TEST(test_consume_x_round);
    RUN_TEST(test_and_or_buffer);
    RUN_TEST(test_xor_buffer);
    RUN_TEST(test_xnor_buffer);
    RUN_TEST(test_reduce_xnor_not);
    RUN_TEST(test_unary_buffer);
    RUN_TEST(test_const_xor_xnor);
    RUN_TEST(test_bitwise_or_x);
    RUN_TEST(test_bitwise_and_x);
    RUN_TEST(test_bitwise_xor_x);
    RUN_TEST(test_bitwise_signed_one_bit);
    RUN_TEST(test_keepdc_keeps_x);
    RUN_TEST(test_invert_double);
    RUN_TEST(test_invert_muxsel);
    RUN_TEST(test_tribuf);
    RUN_TEST(test_gate_tables);
    RUN_TEST(test_gate_and_x);
    RUN_TEST(test_gate_mux_inv);
    RUN_TEST(test_gate_mux_undef);
    RUN_TEST(test_xbit_arith);
    RUN_TEST(test_xbit_cmp);
    RUN_TEST(test_eqneq_isneq);
}

static void run_arith_tests(void) {
    RUN_TEST(test_pow_signed_base);
    RUN_TEST(test_eqneq_empty);
    RUN_TEST(test_eqneq_resize);
    RUN_TEST(test_eqneq_swapconst_isnot);
    RUN_TEST(test_eqneq_buffer_undef);
    RUN_TEST(test_eqneq_cmpzero);
    RUN_TEST(test_constshift);
    RUN_TEST(test_shiftx_trim);
    RUN_TEST(test_identwrt);
    RUN_TEST(test_identwrt_needs_consume_x);
    RUN_TEST(test_pow_shift_two);
    RUN_TEST(test_pow_shift_eight);
    RUN_TEST(test_mul_shift);
    RUN_TEST(test_keepdc_arith);
    RUN_TEST(test_mul_low_zeros);
    RUN_TEST(test_divmod_zero);
    RUN_TEST(test_div_shift_mod_mask);
    RUN_TEST(test_const_fold);
    RUN_TEST(test_div_shift_signed_structure);
    RUN_TEST(test_div_shift_signed_values);
    RUN_TEST(test_mux_s_x_picks_a);
    RUN_TEST(test_mux_same);
    RUN_TEST(test_mux_bool);
    RUN_TEST(test_mux_and_or);
    RUN_TEST(test_mux_undef);
}

static void run_mux_and_driver_tests(void) {
    RUN_TEST(test_mux_undef_all_x);
    RUN_TEST(test_mux_sel01_empty);
    RUN_TEST(test_eqneq_resize_resumes);
    RUN_TEST(test_mux_reduce_resumes);
    RUN_TEST(test_undriven);
    RUN_TEST(test_undriven_kinds);
    RUN_TEST(test_first_match);
    RUN_TEST(test_topo_loop_line);
    RUN_TEST(test_lines);
    RUN_TEST(test_fold_line_and_counter);
    RUN_TEST(test_expr_module_scope);
    RUN_TEST(test_proc_opaque);
    RUN_TEST(test_counts_accumulate);
    RUN_TEST(test_ruling_unary_before_fold);
    RUN_TEST(test_ruling_temp_wire_tied);
    RUN_TEST(test_keep_cell_folds);
    RUN_TEST(test_rule_names);
    RUN_TEST(test_misuse);
    RUN_TEST(test_provenance_derived);
    RUN_TEST(test_expr_oom);
}

int main(void) {
    UNITY_BEGIN();
    run_logic_tests();
    run_arith_tests();
    run_mux_and_driver_tests();
    return UNITY_END();
}
