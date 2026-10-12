/*
 * test_opt_kit.c — the opt context and the rewrite kit (2F spec OP-2, OP-15, §20.1; plan Task 1):
 * constant canonicalisation (duplicate constant nodes, a constant $sop from read_blif), kit_connect
 * (aliases, a constant, the keep == drop loop, a port on the dropped net, misuse), kit_new_cell and
 * the budget, kit_rebuild_node (same signature, new type: names, attributes, provenance), kit_prov,
 * the invert map, topological order with a loop, keep, readers, the folded-alias RTLIL text (M11)
 * and out of memory at every allocation of every kit operation.
 *
 * Yosys re-derivation: none (the kit is Odin III's own IR plumbing; no sub-pass runs here).
 */
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/budget.h"
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
#include <string.h>

enum { LOG_TEXT = 512, OOM_LIMIT = 2000, MAX_PARAMS = 8, BIG_WIDTH = 32768 };

static const char *const BLIF_PATH = "odin3_opt_kit_test.blif";

static char last_error[LOG_TEXT];
static uint32_t n_errors;
static uint32_t n_discard; /* errors saying the design must be discarded (a half-done rewrite) */

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
        n_errors++;
        n_discard += strstr(msg, "discard it") != NULL ? 1U : 0U;
    }
}

/* A built design, a pass run named opt and the opt context on its last module. */
typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_opt_ctx oc;
    odin3_module *module;
    bool open;
} fixture;

static fixture fx;

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void open_ctx(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_ctx_init(&fx.oc, &fx.ctx, fx.module, &fx.opts));
    fx.oc.counts = &fx.counts;
    fx.open = true;
}

static void close_fixture(void) {
    if (fx.open) {
        odin3_opt_ctx_free(&fx.oc);
    }
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
}

static void build(const char *text) {
    close_fixture();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    fx.module = fx.res.module;
    open_ctx();
}

void setUp(void) {
    memset(&fx, 0, sizeof fx);
    last_error[0] = '\0';
    n_errors = 0;
    n_discard = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_budget_test_set_limit(0);
    odin3_opt_test_set_limits(NULL);
    odin3_opt_test_set_hooks(NULL);
    close_fixture();
    odin3_log_set_sink(NULL, NULL);
    (void)remove(BLIF_PATH);
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

/* The net of bit `bit` of the wire named name. */
static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, bit);
}

static odin3_node_id node_named(const char *name) {
    odin3_node_id node = odin3_module_find_node(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    return node;
}

/* The index of the port named name of node's type. */
static uint32_t port_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

/* The net of bit `bit` of port `port` (by name) of node. */
static odin3_net_id pin_net_of(odin3_node_id node, const char *port, uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(fx.module, node, port_of(node, port));
    TEST_ASSERT_TRUE(bit < pins.count);
    return odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + bit});
}

static odin3_celltype_id type_of(const char *name) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern(name), &type));
    return type;
}

static uint32_t live_nodes(void) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        count += odin3_node_live(fx.module, (odin3_node_id){id}) ? 1U : 0U;
    }
    return count;
}

static uint32_t live_nets(void) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_net_end(fx.module); id++) {
        count += odin3_net_live(fx.module, (odin3_net_id){id}) ? 1U : 0U;
    }
    return count;
}

/* --- constants ---------------------------------------------------------------------------- */

/* Node IDs: ports 1-3, g1 4, c1 5, f1 6, f2 7, g2 8, c2 9. */
static const char *const k_consts = "module \\m\n"
                                    "  wire input 1 \\a\n"
                                    "  wire output 2 \\y1\n"
                                    "  wire output 3 \\y2\n"
                                    "  wire \\k1\n"
                                    "  wire \\k2\n"
                                    "  wire \\t1\n"
                                    "  wire \\t2\n"
                                    "  cell $and \\g1\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B \\k1\n"
                                    "    connect \\Y \\y1\n"
                                    "  end\n"
                                    "  cell $_CONST1_ \\c1\n"
                                    "    connect \\Y \\k1\n"
                                    "  end\n"
                                    "  cell $not \\f1\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\Y \\t1\n"
                                    "  end\n"
                                    "  cell $not \\f2\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\Y \\t2\n"
                                    "  end\n"
                                    "  cell $and \\g2\n"
                                    "    connect \\A \\a\n"
                                    "    connect \\B \\k2\n"
                                    "    connect \\Y \\y2\n"
                                    "  end\n"
                                    "  cell $_CONST1_ \\c2\n"
                                    "    connect \\Y \\k2\n"
                                    "  end\n"
                                    "end\n";

static void test_canon_consts(void) {
    build(k_consts);
    odin3_node_id c1 = {5};
    odin3_node_id c2 = {9};
    TEST_ASSERT_EQUAL_UINT32(type_of("$_CONST1_").v, odin3_node_type(fx.module, c1).v);
    TEST_ASSERT_EQUAL_UINT32(type_of("$_CONST1_").v, odin3_node_type(fx.module, c2).v);
    odin3_net_id one = pin_net_of(c1, "Y", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_canon_consts(&fx.oc));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, c1));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, c2));
    TEST_ASSERT_EQUAL_UINT32(one.v, pin_net_of(node_named("g1"), "B", 0).v);
    TEST_ASSERT_EQUAL_UINT32(one.v, pin_net_of(node_named("g2"), "B", 0).v);
    TEST_ASSERT_EQUAL_UINT32(one.v, wnet("k2", 0).v); /* the name stays, as an alias */
    TEST_ASSERT_EQUAL_UINT32(one.v, fx.oc.const_net[ODIN3_BIT_1].v);
    TEST_ASSERT_FALSE(fx.oc.changed);
    odin3_net_id got = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_1, &got));
    TEST_ASSERT_EQUAL_UINT32(one.v, got.v);
    check_full();
}

/* A constant $sop (BLIF `.names y` / `1`) is no constant cell: canon leaves it, it reads ONE. */
/* Writes text to BLIF_PATH and reads it into a new design as the fixture. */
static void read_blif_fixture(const char *text) {
    FILE *file = fopen(BLIF_PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    fx.res.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(fx.res.design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(fx.res.design, BLIF_PATH));
    fx.module = odin3_module_get(fx.res.design, (odin3_module_id){1});
    open_ctx();
}

/* The last live node of type `type`, none when there is none. */
static odin3_node_id last_of_type(const char *type) {
    odin3_celltype_id want = type_of(type);
    odin3_node_id found = {0};
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node) && odin3_node_type(fx.module, node).v == want.v) {
            found = node;
        }
    }
    return found;
}

/* A constant $sop (BLIF `.names y` / `1`) is no constant cell: canon leaves it, it reads ONE. */
static void test_canon_leaves_sop(void) {
    read_blif_fixture(".model top\n.outputs y\n.names y\n1\n.end\n");
    odin3_node_id sop = last_of_type("$sop");
    TEST_ASSERT_TRUE(odin3_node_valid(sop));
    uint32_t nodes = live_nodes();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_canon_consts(&fx.oc));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, sop));
    TEST_ASSERT_EQUAL_UINT32(nodes, live_nodes()); /* creates none */
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(fx.module, wnet("y", 0)));
    TEST_ASSERT_FALSE(odin3_net_valid(fx.oc.const_net[ODIN3_BIT_1]));
    check_full();
}

/* const_net creates the node on demand, once; an invalid value is refused. */
static void test_const_net_on_demand(void) {
    build(k_consts);
    uint32_t nodes = live_nodes();
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_UINT32(nodes + 1, live_nodes());
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(fx.module, zero));
    odin3_net_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &again));
    TEST_ASSERT_EQUAL_UINT32(zero.v, again.v);
    TEST_ASSERT_EQUAL_UINT32(nodes + 1, live_nodes());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_const_net(&fx.oc, (odin3_bit)ODIN3_OPT_N_CONST, &again));
    TEST_ASSERT_EQUAL_STRING("opt: const_net: 4 is not 0, 1, x or z", last_error);
    check_full();
}

/* A cached constant net that lost its driver is not handed out again: a new one is made. */
static void test_const_net_stale(void) {
    build(k_consts);
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &zero));
    odin3_node_id made = {odin3_module_node_end(fx.module) - 1};
    TEST_ASSERT_EQUAL_UINT32(type_of("$_CONST0_").v, odin3_node_type(fx.module, made).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(fx.module, made));
    odin3_net_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &again));
    TEST_ASSERT_NOT_EQUAL_UINT32(zero.v, again.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(fx.module, again));
}

/* --- kit_connect -------------------------------------------------------------------------- */

static const char *const k_alias = "module \\m\n"
                                   "  wire input 1 \\a\n"
                                   "  wire input 2 \\b\n"
                                   "  wire output 3 \\o\n"
                                   "  wire \\y\n"
                                   "  wire \\y2\n"
                                   "  cell $and \\g\n"
                                   "    connect \\A \\a\n"
                                   "    connect \\B \\b\n"
                                   "    connect \\Y \\y\n"
                                   "  end\n"
                                   "  cell $not \\r\n"
                                   "    connect \\A \\y\n"
                                   "    connect \\Y \\o\n"
                                   "  end\n"
                                   "  connect \\y2 \\y\n"
                                   "end\n";

static odin3_status connect_to(odin3_node_id node, const odin3_net_id *src, uint32_t width) {
    odin3_kit_conn conn = {node, port_of(node, "Y"), src, width};
    return odin3_kit_connect(&fx.oc, &conn);
}

static void test_connect_alias(void) {
    build(k_alias);
    odin3_node_id gate = node_named("g");
    odin3_net_id net_a = wnet("a", 0);
    TEST_ASSERT_EQUAL_UINT32(wnet("y", 0).v, wnet("y2", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(gate, &net_a, 1));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, gate));
    TEST_ASSERT_EQUAL_UINT32(net_a.v, wnet("y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, wnet("y2", 0).v);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, pin_net_of(node_named("r"), "A", 0).v);
    TEST_ASSERT_TRUE(fx.oc.changed);
    check_full();
}

static void test_connect_constant(void) {
    build(k_alias);
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(node_named("g"), &zero, 1));
    TEST_ASSERT_EQUAL_UINT32(zero.v, wnet("y", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(fx.module, wnet("y", 0)));
    check_full();
}

static void test_connect_self(void) {
    build("module \\m\n"
          "  wire output 1 \\o\n"
          "  wire \\w\n"
          "  cell $not \\n\n"
          "    connect \\A \\w\n"
          "    connect \\Y \\w\n"
          "  end\n"
          "  cell $not \\r\n"
          "    connect \\A \\w\n"
          "    connect \\Y \\o\n"
          "  end\n"
          "end\n");
    odin3_node_id node_n = node_named("n");
    odin3_net_id net_w = wnet("w", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(node_n, &net_w, 1));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, node_n));
    TEST_ASSERT_TRUE(odin3_net_live(fx.module, net_w));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(fx.module, net_w));
    TEST_ASSERT_EQUAL_UINT32(net_w.v, pin_net_of(node_named("r"), "A", 0).v);
    check_full();
}

/* Multi-bit with a swap: y[0] <- y[1] merges the two nets, so bit 1's y[0] resolves to the kept net
 * and is left undriven (fed back to itself) rather than merging a dead net. */
static void test_connect_swap(void) {
    build("module \\m\n"
          "  wire width 2 input 1 \\a\n  wire width 2 input 2 \\b\n"
          "  wire width 2 output 3 \\o\n  wire width 2 \\y\n"
          "  cell $and \\g\n    parameter \\A_WIDTH 2\n    parameter \\B_WIDTH 2\n"
          "    parameter \\Y_WIDTH 2\n    connect \\A \\a\n    connect \\B \\b\n"
          "    connect \\Y \\y\n  end\n"
          "  cell $not \\r\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\y\n    connect \\Y \\o\n  end\n"
          "end\n");
    odin3_net_id src[2] = {wnet("y", 1), wnet("y", 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(node_named("g"), src, 2));
    check_full();
    TEST_ASSERT_EQUAL_UINT32(wnet("y", 0).v, wnet("y", 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(fx.module, wnet("y", 0)));
}

static void test_connect_port(void) {
    build("module \\m\n"
          "  wire input 1 \\a\n"
          "  wire input 2 \\b\n"
          "  wire output 3 \\y\n"
          "  cell $and \\g\n"
          "    connect \\A \\a\n"
          "    connect \\B \\b\n"
          "    connect \\Y \\y\n"
          "  end\n"
          "end\n");
    odin3_net_id net_a = wnet("a", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(node_named("g"), &net_a, 1));
    odin3_node_id port = odin3_module_port(fx.module, 2);
    odin3_pinslice pins = odin3_node_pins(fx.module, port);
    TEST_ASSERT_EQUAL_UINT32(1, pins.count);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, odin3_pin_net(fx.module, pins.first).v);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, wnet("y", 0).v);
    check_full();
}

/* A width mismatch, a port node and a missing source are misuses: nothing changes. */
static void test_connect_misuse(void) {
    build(k_alias);
    odin3_node_id gate = node_named("g");
    odin3_net_id two[2] = {wnet("a", 0), wnet("b", 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, connect_to(gate, two, 2));
    TEST_ASSERT_EQUAL_STRING("opt: kit_connect: $and output is not 2 bits wide", last_error);
    odin3_kit_conn bad_port = {gate, 0, two, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_kit_connect(&fx.oc, &bad_port));
    odin3_kit_conn port_node = {odin3_module_port(fx.module, 0), 0, two, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_kit_connect(&fx.oc, &port_node));
    odin3_net_id none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, connect_to(gate, &none, 1));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, gate));
    TEST_ASSERT_FALSE(fx.oc.changed);
    check_full();
}

/* --- kit_new_cell and the budget ------------------------------------------------------------ */

static void test_new_cell(void) {
    build(k_alias);
    odin3_net_id net_a = wnet("a", 0);
    odin3_netvec ports[2] = {{&net_a, 1}, {NULL, 0}};
    odin3_kit_cell cell = {type_of("$not"), NULL, 0, ports, odin3_module_prov(fx.module)};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_new_cell(&fx.oc, &cell, &node));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_name(fx.module, node));
    odin3_net_id net_y = pin_net_of(node, "Y", 0);
    TEST_ASSERT_TRUE(odin3_net_live(fx.module, net_y));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_prov(fx.module).v, odin3_net_prov(fx.module, net_y).v);
    TEST_ASSERT_EQUAL_UINT64(2, fx.oc.pins_created);
    TEST_ASSERT_TRUE(fx.oc.changed);
    check_full();
}

/* The budget is checked when pins_created crosses a multiple of 2^16, before anything is made. */
static void test_new_cell_budget(void) {
    build(k_alias);
    odin3_budget_test_set_limit(1);
    odin3_net_id net_a = wnet("a", 0);
    odin3_netvec small[2] = {{&net_a, 1}, {NULL, 0}};
    odin3_kit_cell cell = {type_of("$not"), NULL, 0, small, {0}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_new_cell(&fx.oc, &cell, &node));
    static odin3_net_id open_a[BIG_WIDTH]; /* none: the pins stay open */
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(BIG_WIDTH),
                             odin3_value_int(BIG_WIDTH)};
    odin3_netvec big[2] = {{open_a, BIG_WIDTH}, {NULL, 0}};
    odin3_kit_cell wide = {type_of("$not"), params, 3, big, {0}};
    uint32_t nets = odin3_module_net_end(fx.module);
    uint32_t nodes = odin3_module_node_end(fx.module);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_kit_new_cell(&fx.oc, &wide, &node));
    TEST_ASSERT_EQUAL_STRING(
        "opt needs more than 1 bytes (the design's budget is 60% of physical memory)", last_error);
    TEST_ASSERT_EQUAL_UINT32(nets, odin3_module_net_end(fx.module));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(fx.module));
    check_full();
}

/* --- kit_rebuild_node --------------------------------------------------------------------- */

static const char *const k_add = "module \\m\n"
                                 "  wire width 4 input 1 \\a\n"
                                 "  wire width 4 input 2 \\b\n"
                                 "  wire width 4 output 3 \\y\n"
                                 "  attribute \\keep 1\n"
                                 "  attribute \\src \"t.v:3.1\"\n"
                                 "  cell $add \\u\n"
                                 "    parameter \\A_WIDTH 4\n"
                                 "    parameter \\B_WIDTH 4\n"
                                 "    parameter \\Y_WIDTH 4\n"
                                 "    connect \\A \\a\n"
                                 "    connect \\B \\b\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "end\n";

/* A copy of node's parameters into params (at most MAX_PARAMS); returns their count. */
static uint32_t params_of(odin3_node_id node, odin3_value *params) {
    uint32_t count = 0;
    while (count < MAX_PARAMS && odin3_node_param(fx.module, node, count) != NULL) {
        params[count] = *odin3_node_param(fx.module, node, count);
        count++;
    }
    return count;
}

static void expect_keep(odin3_node_id node) {
    const odin3_value *keep =
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern("keep"));
    TEST_ASSERT_NOT_NULL(keep);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, keep->kind);
    TEST_ASSERT_EQUAL_INT64(1, keep->i);
    TEST_ASSERT_TRUE(odin3_kit_keep(&fx.oc, (odin3_objref){ODIN3_OBJ_NODE, node.v}));
}

/* node's record is DERIVED with the one parent rec. */
static void expect_derived_from(odin3_node_id node, odin3_prov_id rec) {
    const odin3_prov_record *got = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, node));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, got->kind);
    TEST_ASSERT_EQUAL_UINT32(1, got->parents.count);
    TEST_ASSERT_EQUAL_UINT32(rec.v, got->parents.ids[0].v);
}

/* node's A and Y pins are on the nets of wires a and y. */
static void expect_add_nets(odin3_node_id node) {
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("y", k).v, pin_net_of(node, "Y", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("a", k).v, pin_net_of(node, "A", k).v);
    }
}

static void test_rebuild_same_signature(void) {
    build(k_add);
    odin3_node_id old = node_named("u");
    odin3_prov_id rec = odin3_node_prov(fx.module, old);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, odin3_prov_get(fx.res.design, rec)->kind);
    odin3_value params[MAX_PARAMS];
    uint32_t n_params = params_of(old, params);
    uint32_t nets = odin3_module_net_end(fx.module);
    odin3_kit_begin_rewrite(&fx.oc);
    odin3_kit_rebuild req = {old, {type_of("$add"), params, n_params, NULL, {0}}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_rebuild_node(&fx.oc, &req, &node));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node));
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("u").v);
    expect_keep(node);
    expect_derived_from(node, rec);
    expect_add_nets(node);
    TEST_ASSERT_EQUAL_UINT32(nets, odin3_module_net_end(fx.module)); /* no fresh nets */
    TEST_ASSERT_TRUE(fx.oc.changed);
    check_full();
}

static void test_rebuild_new_type(void) {
    build(k_alias);
    odin3_node_id old = node_named("g");
    odin3_net_id net_y = wnet("y", 0);
    uint32_t nets = odin3_module_net_end(fx.module);
    odin3_kit_begin_rewrite(&fx.oc);
    odin3_kit_rebuild req = {old, {type_of("$or"), NULL, 0, NULL, {0}}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_rebuild_node(&fx.oc, &req, &node));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
    TEST_ASSERT_EQUAL_UINT32(type_of("$or").v, odin3_node_type(fx.module, node).v);
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("g").v);
    TEST_ASSERT_EQUAL_UINT32(net_y.v, pin_net_of(node, "Y", 0).v); /* the old net, its names kept */
    TEST_ASSERT_EQUAL_UINT32(net_y.v, wnet("y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(net_y.v, wnet("y2", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 0).v, pin_net_of(node, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(nets + 1, odin3_module_net_end(fx.module)); /* one fresh net... */
    TEST_ASSERT_FALSE(odin3_net_live(fx.module, (odin3_net_id){nets}));  /* ...merged away */
    check_full();
}

/* Output widths must agree on the fresh path: an $add Y of 5 bits for a 4-bit Y is refused. */
static void test_rebuild_width_mismatch(void) {
    build(k_add);
    odin3_node_id old = node_named("u");
    odin3_value params[MAX_PARAMS];
    uint32_t n_params = params_of(old, params);
    params[n_params - 1] = odin3_value_int(5); /* Y_WIDTH */
    odin3_kit_rebuild req = {old, {type_of("$add"), params, n_params, NULL, {0}}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_kit_rebuild_node(&fx.oc, &req, &node));
    TEST_ASSERT_EQUAL_STRING("opt: kit_rebuild_node: the outputs of $add and $add differ",
                             last_error);
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, old));
    TEST_ASSERT_FALSE(fx.oc.changed);
    check_full();
}

/* Outputs pair in order, not by index: $adff's Q (port 3) is $adffe's Q (port 4), given as the old
 * node's nets; any other nets for it are refused. */
static void test_rebuild_moves_output(void) {
    build("module \\m\n"
          "  wire input 1 \\c\n  wire input 2 \\r\n  wire input 3 \\e\n  wire input 4 \\d\n"
          "  wire output 5 \\q\n"
          "  cell $adff \\f\n    connect \\CLK \\c\n    connect \\ARST \\r\n"
          "    connect \\D \\d\n    connect \\Q \\q\n  end\n"
          "end\n");
    odin3_node_id old = node_named("f");
    odin3_net_id nets[5] = {wnet("c", 0), wnet("r", 0), wnet("e", 0), wnet("d", 0), wnet("q", 0)};
    odin3_netvec ports[5] = {
        {&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}, {&nets[3], 1}, {&nets[4], 1}};
    odin3_kit_begin_rewrite(&fx.oc);
    odin3_kit_rebuild req = {old, {type_of("$adffe"), NULL, 0, ports, {0}}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_kit_rebuild_node(&fx.oc, &req, &node),
                                  last_error);
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
    TEST_ASSERT_EQUAL_UINT32(type_of("$adffe").v, odin3_node_type(fx.module, node).v);
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("f").v);
    TEST_ASSERT_EQUAL_UINT32(nets[4].v, wnet("q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(nets[4].v, pin_net_of(node, "Q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(nets[2].v, pin_net_of(node, "EN", 0).v);
    check_full();
    ports[4] = (odin3_netvec){&nets[3], 1}; /* Q on d: not the old node's */
    odin3_kit_rebuild bad = {node, {type_of("$adffe"), NULL, 0, ports, {0}}};
    odin3_node_id other = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_kit_rebuild_node(&fx.oc, &bad, &other));
    TEST_ASSERT_EQUAL_STRING("opt: kit_rebuild_node: output Q is not the old node's", last_error);
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node));
    check_full();
}

static void test_prov_parents(void) {
    build("module \\m\n"
          "  wire input 1 \\a\n"
          "  wire \\y\n"
          "  wire \\z\n"
          "  attribute \\src \"t.v:1.1\"\n"
          "  cell $not \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"
          "  attribute \\src \"t.v:2.1\"\n"
          "  cell $not \\r\n    connect \\A \\y\n    connect \\Y \\z\n  end\n"
          "end\n");
    odin3_node_id parents[2] = {node_named("r"), node_named("g")};
    odin3_kit_begin_rewrite(&fx.oc);
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_prov(&fx.oc, parents, 2, &prov));
    odin3_prov_list list = odin3_prov_parents(fx.res.design, prov);
    TEST_ASSERT_EQUAL_UINT32(2, list.count);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(fx.module, parents[0]).v, list.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(fx.module, parents[1]).v, list.ids[1].v);
}

/* --- invert map, topological order, keep, readers ------------------------------------------- */

static void test_invert_map(void) {
    build("module \\m\n"
          "  wire input 1 \\a\n"
          "  wire input 2 \\s\n"
          "  wire width 2 input 3 \\v\n"
          "  wire \\na\n  wire \\la\n  wire \\ms\n  wire width 2 \\nv\n"
          "  cell $not \\n1\n    connect \\A \\a\n    connect \\Y \\na\n  end\n"
          "  cell $logic_not \\l1\n    connect \\A \\a\n    connect \\Y \\la\n  end\n"
          "  cell $mux \\m1\n    connect \\A 1'1\n    connect \\B 1'0\n"
          "    connect \\S \\s\n    connect \\Y \\ms\n  end\n"
          "  cell $not \\n2\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\v\n    connect \\Y \\nv\n  end\n"
          "  wire width 2 \\lw\n"
          "  cell $logic_not \\l2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\a\n    connect \\Y \\lw\n  end\n"
          "end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_invert_map(&fx.oc));
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 0).v, odin3_kit_inverse(&fx.oc, wnet("na", 0)).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 0).v, odin3_kit_inverse(&fx.oc, wnet("la", 0)).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("s", 0).v, odin3_kit_inverse(&fx.oc, wnet("ms", 0)).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_kit_inverse(&fx.oc, wnet("nv", 0)).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_kit_inverse(&fx.oc, wnet("lw", 0)).v); /* a wide Y: no */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_kit_inverse(&fx.oc, wnet("a", 0)).v);
}

/* $or 5 <- $and 7 <- $not 9, with constant cells (not evaluable) at 4, 6 and 8. */
static const char *const k_chain =
    "module \\m\n"
    "  wire input 1 \\a\n"
    "  wire input 2 \\b\n"
    "  wire output 3 \\y\n"
    "  wire \\t1\n  wire \\t2\n  wire \\k4\n  wire \\k6\n  wire \\k8\n"
    "  cell $_CONST0_ \\z4\n    connect \\Y \\k4\n  end\n"
    "  cell $or \\o\n    connect \\A \\t2\n    connect \\B \\b\n"
    "    connect \\Y \\y\n  end\n"
    "  cell $_CONST0_ \\z6\n    connect \\Y \\k6\n  end\n"
    "  cell $and \\n\n    connect \\A \\t1\n    connect \\B \\b\n"
    "    connect \\Y \\t2\n  end\n"
    "  cell $_CONST0_ \\z8\n    connect \\Y \\k8\n  end\n"
    "  cell $not \\x\n    connect \\A \\a\n    connect \\Y \\t1\n"
    "  end\n"
    "end\n";

static void expect_order(const odin3_vec *order, const uint32_t *ids, size_t count) {
    TEST_ASSERT_EQUAL_size_t(count, order->len);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(ids[i], ((const odin3_node_id *)odin3_vec_cat(order, i))->v);
    }
}

static void test_topo_order(void) {
    build(k_chain);
    TEST_ASSERT_EQUAL_UINT32(5, node_named("o").v);
    TEST_ASSERT_EQUAL_UINT32(7, node_named("n").v);
    TEST_ASSERT_EQUAL_UINT32(9, node_named("x").v);
    odin3_vec order;
    odin3_vec_init(&order, sizeof(odin3_node_id));
    bool sorted = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_topo(&fx.oc, &order, &sorted));
    TEST_ASSERT_TRUE(sorted);
    static const uint32_t want[] = {9, 7, 5};
    expect_order(&order, want, 3);
    odin3_vec_free(&order);
}

/* A mux is an inverter only for A = 1, B = 0: A = 0, B = 1 is a buffer of S. */
static void test_invert_mux01(void) {
    build("module \\m\n"
          "  wire input 1 \\s\n  wire output 2 \\y\n  wire output 3 \\z\n"
          "  wire \\k0\n  wire \\k1\n"
          "  cell $_CONST0_ \\c0\n    connect \\Y \\k0\n  end\n"
          "  cell $_CONST1_ \\c1\n    connect \\Y \\k1\n  end\n"
          "  cell $_MUX_ \\m1\n    connect \\A \\k0\n    connect \\B \\k1\n"
          "    connect \\S \\s\n    connect \\Y \\y\n  end\n"
          "  cell $_MUX_ \\m2\n    connect \\A \\k1\n    connect \\B \\k0\n"
          "    connect \\S \\s\n    connect \\Y \\z\n  end\n"
          "end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_invert_map(&fx.oc));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_kit_inverse(&fx.oc, wnet("y", 0)).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("s", 0).v, odin3_kit_inverse(&fx.oc, wnet("z", 0)).v);
}

/* Two readers ready at once after g: the tie goes to the lower node ID (OP-14). */
static void test_topo_ties(void) {
    build("module \\m\n"
          "  wire width 2 input 1 \\a\n  wire width 2 input 2 \\b\n"
          "  wire width 2 output 3 \\o1\n  wire width 2 output 4 \\o2\n  wire width 2 \\t\n"
          "  cell $and \\g\n    parameter \\A_WIDTH 2\n    parameter \\B_WIDTH 2\n"
          "    parameter \\Y_WIDTH 2\n    connect \\A \\a\n    connect \\B \\b\n"
          "    connect \\Y \\t\n  end\n"
          "  cell $not \\r1\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\t\n    connect \\Y \\o1\n  end\n"
          "  cell $xor \\r2\n    parameter \\A_WIDTH 2\n    parameter \\B_WIDTH 2\n"
          "    parameter \\Y_WIDTH 2\n    connect \\A \\t\n    connect \\B \\a\n"
          "    connect \\Y \\o2\n  end\n"
          "end\n");
    uint32_t gate = node_named("g").v;
    uint32_t r1 = node_named("r1").v;
    uint32_t r2 = node_named("r2").v;
    odin3_vec order;
    odin3_vec_init(&order, sizeof(odin3_node_id));
    bool sorted = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_topo(&fx.oc, &order, &sorted));
    TEST_ASSERT_TRUE(sorted);
    const uint32_t want[] = {gate, r1 < r2 ? r1 : r2, r1 < r2 ? r2 : r1};
    expect_order(&order, want, 3);
    odin3_vec_free(&order);
}

static void test_topo_loop(void) {
    build("module \\m\n"
          "  wire \\r1\n  wire \\r2\n  wire \\r3\n"
          "  cell $not \\x1\n    connect \\A \\r3\n    connect \\Y \\r1\n  end\n"
          "  cell $not \\x2\n    connect \\A \\r1\n    connect \\Y \\r2\n  end\n"
          "  cell $not \\x3\n    connect \\A \\r2\n    connect \\Y \\r3\n  end\n"
          "end\n");
    odin3_vec order;
    odin3_vec_init(&order, sizeof(odin3_node_id));
    bool sorted = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_topo(&fx.oc, &order, &sorted));
    TEST_ASSERT_FALSE(sorted);
    static const uint32_t want[] = {1, 2, 3};
    expect_order(&order, want, 3);
    odin3_vec_free(&order);
}

static void test_keep(void) {
    build(k_add);
    odin3_node_id add = node_named("u");
    TEST_ASSERT_TRUE(odin3_kit_keep(&fx.oc, (odin3_objref){ODIN3_OBJ_NODE, add.v}));
    odin3_objref wire = {ODIN3_OBJ_WIRE, odin3_module_find_wire(fx.module, intern("a")).v};
    TEST_ASSERT_FALSE(odin3_kit_keep(&fx.oc, wire));
    odin3_value zero = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(fx.module, wire, intern("keep"), &zero));
    TEST_ASSERT_FALSE(odin3_kit_keep(&fx.oc, wire));
}

/* keep as BITS counts when a bit is 1 (Yosys Const::as_bool). */
static void test_keep_bits(void) {
    build(k_add);
    odin3_objref wire = {ODIN3_OBJ_WIRE, odin3_module_find_wire(fx.module, intern("a")).v};
    static const uint8_t bits[2] = {ODIN3_BIT_0, ODIN3_BIT_1};
    odin3_value vec = {ODIN3_VAL_BITS, 0, bits, 2, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(fx.module, wire, intern("keep"), &vec));
    TEST_ASSERT_TRUE(odin3_kit_keep(&fx.oc, wire));
    static const uint8_t zeros[2] = {ODIN3_BIT_0, ODIN3_BIT_X};
    odin3_value none = {ODIN3_VAL_BITS, 0, zeros, 2, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(fx.module, wire, intern("keep"), &none));
    TEST_ASSERT_FALSE(odin3_kit_keep(&fx.oc, wire));
}

static void test_readers(void) {
    build(k_add);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_kit_readers(&fx.oc, wnet("a", 0))); /* $add A */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_kit_readers(&fx.oc, wnet("y", 0))); /* $port_out */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_kit_readers(&fx.oc, (odin3_net_id){0}));
}

/* M11: a wire bit folded into a constant net prints as Yosys's `connect \w [k] 1'0`. */
static void test_rtlil_folded_alias(void) {
    build("module \\m\n"
          "  wire input 1 \\a\n"
          "  wire width 2 output 2 \\w\n"
          "  cell $not \\n0\n    connect \\A \\a\n    connect \\Y \\w [0]\n  end\n"
          "  cell $not \\n1\n    connect \\A \\a\n    connect \\Y \\w [1]\n  end\n"
          "end\n");
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, connect_to(node_named("n1"), &zero, 1));
    check_full();
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &text));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text.data, "  connect \\w [1] 1'0\n"), text.data);
    odin3_strbuf_free(&text);
}

/* --- out of memory at every allocation ------------------------------------------------------- */

/* The nets of every pin of node, for "nets unchanged" (at most `max`). */
static uint32_t snapshot_pins(odin3_node_id node, odin3_net_id *nets, uint32_t max) {
    odin3_pinslice pins = odin3_node_pins(fx.module, node);
    for (uint32_t k = 0; k < pins.count && k < max; k++) {
        nets[k] = odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + k});
    }
    return pins.count;
}

static void expect_pins(odin3_node_id node, const odin3_net_id *nets, uint32_t count) {
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node));
    odin3_pinslice pins = odin3_node_pins(fx.module, node);
    TEST_ASSERT_EQUAL_UINT32(count, pins.count);
    for (uint32_t k = 0; k < count; k++) {
        TEST_ASSERT_EQUAL_UINT32(nets[k].v,
                                 odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + k}).v);
    }
}

enum { SNAP = 16 };

/* One rebuild under a failure after `fail` allocations: OK, or undone (old node, nets, names). */
/* A rebuild to sweep: the module text, the cell's name and the new type. */
typedef struct rebuild_case {
    const char *text, *name, *type;
} rebuild_case;

/* After a failed rebuild: the old node keeps its pins' nets and name, nothing else is live. */
static void expect_rebuild_undone(const rebuild_case *rc, odin3_node_id old,
                                  const odin3_net_id *nets, uint32_t count) {
    expect_pins(old, nets, count);
    TEST_ASSERT_EQUAL_UINT32(old.v, node_named(rc->name).v);
    TEST_ASSERT_FALSE(fx.oc.changed);
}

/* One rebuild under a failure after `fail` allocations: OK, or undone (old node, nets, names). */
static bool rebuild_oom_step(const rebuild_case *rc, long fail) {
    build(rc->text);
    odin3_node_id old = node_named(rc->name);
    odin3_net_id nets[SNAP];
    uint32_t count = snapshot_pins(old, nets, SNAP);
    uint32_t live = live_nodes() + live_nets();
    odin3_value params[MAX_PARAMS];
    uint32_t n_params = params_of(old, params); /* $and and $or share the binary parameters */
    odin3_kit_begin_rewrite(&fx.oc);
    odin3_kit_rebuild req = {old, {type_of(rc->type), params, n_params, NULL, {0}}};
    odin3_node_id node = {0};
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_kit_rebuild_node(&fx.oc, &req, &node);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(node.v, node_named(rc->name).v);
        return true;
    }
    expect_rebuild_undone(rc, old, nets, count);
    TEST_ASSERT_EQUAL_UINT32(live, live_nodes() + live_nets());
    return false;
}

static void test_rebuild_oom(void) {
    const rebuild_case same_case = {k_add, "u", "$add"};
    const rebuild_case fresh_case = {k_alias, "g", "$or"};
    bool same = false;
    for (long fail = 0; fail < OOM_LIMIT && !same; fail++) {
        same = rebuild_oom_step(&same_case, fail);
    }
    TEST_ASSERT_TRUE(same);
    bool fresh = false;
    for (long fail = 0; fail < OOM_LIMIT && !fresh; fail++) {
        fresh = rebuild_oom_step(&fresh_case, fail);
    }
    TEST_ASSERT_TRUE(fresh);
}

/* y is merged into net_a or left undriven (a failure inside kit_connect's merge). */
static void expect_merged_or_undriven(odin3_net_id net_a) {
    odin3_net_id net_y = wnet("y", 0);
    TEST_ASSERT_TRUE(net_y.v == net_a.v || odin3_net_driver_count(fx.module, net_y) == 0);
}

/*
 * kit_connect under a failure after `fail` allocations. A failure up to the delete leaves it
 * undone; one inside a merge (the IR cannot reserve merges, kit.h) leaves the node dead and y
 * either merged into a or undriven, the IR valid. *merge_phase counts the latter.
 */
static bool connect_oom_step(long fail, uint32_t *merge_phase) {
    build(k_alias);
    odin3_node_id gate = node_named("g");
    odin3_net_id nets[SNAP];
    uint32_t count = snapshot_pins(gate, nets, SNAP);
    odin3_net_id net_a = wnet("a", 0);
    n_discard = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = connect_to(gate, &net_a, 1);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(0, n_discard);
        return true;
    }
    if (odin3_node_live(fx.module, gate)) {
        expect_pins(gate, nets, count);
        TEST_ASSERT_FALSE(fx.oc.changed);
        TEST_ASSERT_EQUAL_UINT32(0, n_discard);
        return false;
    }
    TEST_ASSERT_EQUAL_UINT32(1, n_discard);
    (*merge_phase)++;
    expect_merged_or_undriven(net_a);
    return false;
}

static void test_connect_oom(void) {
    bool done = false;
    uint32_t merge_phase = 0;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = connect_oom_step(fail, &merge_phase);
    }
    TEST_ASSERT_TRUE(done);
    /* With the current IR the merges find room here (0 measured); the bound allows the merge's
     * pin block and alias record. */
    TEST_ASSERT_TRUE(merge_phase <= 2);
}

static bool new_cell_oom_step(long fail) {
    build(k_alias);
    uint32_t live = live_nodes();
    uint32_t live_n = live_nets();
    odin3_net_id net_a = wnet("a", 0);
    odin3_netvec ports[2] = {{&net_a, 1}, {NULL, 0}};
    odin3_kit_cell cell = {type_of("$not"), NULL, 0, ports, {0}};
    odin3_node_id node = {0};
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_kit_new_cell(&fx.oc, &cell, &node);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(live, live_nodes());
        TEST_ASSERT_EQUAL_UINT32(live_n, live_nets());
    }
    return st == ODIN3_OK;
}

static void test_new_cell_oom(void) {
    bool done = false;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = new_cell_oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
}

/* canon_consts, const_net, invert_map and topo: NO_MEMORY leaves a valid IR (the reads change
 * nothing; canon's duplicate stays live unless its delete went through). */
/* canon_consts, const_net(0), invert_map and topo, stopping at the first failure. */
static odin3_status run_reads(odin3_vec *order, bool *sorted, odin3_net_id *zero) {
    odin3_status st = odin3_opt_canon_consts(&fx.oc);
    st = st == ODIN3_OK ? odin3_opt_const_net(&fx.oc, ODIN3_BIT_0, zero) : st;
    st = st == ODIN3_OK ? odin3_kit_invert_map(&fx.oc) : st;
    return st == ODIN3_OK ? odin3_kit_topo(&fx.oc, order, sorted) : st;
}

/* 1 when g2.B lost its constant (a failed canon merge), which must have logged "discard it". */
static uint32_t expect_half_done(odin3_status st) {
    odin3_net_id g2_b = pin_net_of(node_named("g2"), "B", 0);
    uint32_t lost = odin3_net_driver_count(fx.module, g2_b) == 0 ? 1U : 0U;
    TEST_ASSERT_TRUE(lost == 0 || st != ODIN3_OK);
    TEST_ASSERT_EQUAL_UINT32(lost, n_discard);
    return lost;
}

/*
 * One run of the reads with a failure after `fail` allocations; true when done. A failure in
 * canon's merge after c2's delete leaves g2.B undriven where it read 1 (the function changed): it
 * must log "discard it" once (I1); *half_done counts it. Every other failure logs no such error.
 */
static bool reads_oom_step(long fail, uint32_t *half_done) {
    build(k_consts);
    odin3_vec order;
    odin3_vec_init(&order, sizeof(odin3_node_id));
    bool sorted = false;
    odin3_net_id zero = {0};
    uint32_t live = live_nodes();
    n_discard = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = run_reads(&order, &sorted, &zero);
    odin3_util_set_alloc_fail_after(-1);
    odin3_vec_free(&order);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, (odin3_node_id){5}));
    TEST_ASSERT_TRUE(live_nodes() >= live - 1 && live_nodes() <= live + 1);
    *half_done += expect_half_done(st);
    return st == ODIN3_OK;
}

static void test_reads_oom(void) {
    bool done = false;
    uint32_t half_done = 0;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = reads_oom_step(fail, &half_done);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(half_done >= 1); /* the review's point on k_consts is still reached */
}

/* --- test hooks ------------------------------------------------------------------------------ */

static void test_limits_clamped(void) {
    odin3_opt_limits low = {0, 7};
    odin3_opt_test_set_limits(&low);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_opt_limits_now().max_iterations);
    TEST_ASSERT_EQUAL_UINT32(7, odin3_opt_limits_now().muxtree_max_evals);
    odin3_opt_limits high = {UINT32_MAX, UINT32_MAX};
    odin3_opt_test_set_limits(&high);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OPT_MAX_ITERATIONS, odin3_opt_limits_now().max_iterations);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OPT_MUXTREE_MAX_EVALS, odin3_opt_limits_now().muxtree_max_evals);
    odin3_opt_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OPT_MAX_ITERATIONS, odin3_opt_limits_now().max_iterations);
    TEST_ASSERT_NULL(odin3_opt_test_hooks_now()->on_call);
}

/* The vector helpers: pushes, truncate, sort_unify of a tail, push OOM. */
static void test_vec_helpers(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(uint64_t));
    static const uint64_t k_in[] = {9, 3, 3, 7, 1, 9, 1};
    for (size_t i = 0; i < sizeof k_in / sizeof k_in[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_push_u64(&vec, k_in[i]));
    }
    odin3_kit_sort_unify(&vec, 1); /* the first element stays where it is */
    static const uint64_t k_want[] = {9, 1, 3, 7, 9};
    TEST_ASSERT_EQUAL_UINT64(sizeof k_want / sizeof k_want[0], vec.len);
    TEST_ASSERT_EQUAL_UINT64_ARRAY(k_want, vec.data, vec.len);
    odin3_kit_truncate(&vec, 2);
    TEST_ASSERT_EQUAL_UINT64(2, vec.len);
    odin3_kit_truncate(&vec, 10);
    TEST_ASSERT_EQUAL_UINT64(2, vec.len);
    odin3_vec_free(&vec);
    odin3_vec_init(&vec, sizeof(uint32_t));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_push_u32(&vec, 5));
    TEST_ASSERT_EQUAL_UINT32(5, *(const uint32_t *)odin3_vec_cat(&vec, 0));
    odin3_vec_free(&vec);
    odin3_vec_init(&vec, sizeof(uint32_t));
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_kit_push_u32(&vec, 1));
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_kit_push_u64(&vec, 1));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT64(0, vec.len);
    odin3_vec_free(&vec);
}

/* Every helper on an empty vec that never allocated (data NULL): no pointer arithmetic on NULL,
 * which clang's UBSan reports, and no lookup in a type without ports or parameters. */
static void test_helpers_empty(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(uint64_t));
    TEST_ASSERT_NULL(vec.data);
    odin3_kit_sort_unify(&vec, 0);
    odin3_kit_truncate(&vec, 0);
    odin3_kit_truncate(&vec, 3);
    TEST_ASSERT_EQUAL_UINT64(0, vec.len);
    TEST_ASSERT_NULL(vec.data);
    odin3_vec one;
    odin3_vec_init(&one, sizeof(uint32_t));
    odin3_kit_truncate(&one, 0);
    TEST_ASSERT_NULL(one.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_push_u32(&one, 7));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_kit_push_u64(&vec, 7));
    odin3_kit_sort_unify(&vec, 1); /* from == len */
    TEST_ASSERT_EQUAL_UINT64(1, vec.len);
    odin3_vec_free(&one);
    odin3_vec_free(&vec);
    odin3_celltype_def bare;
    memset(&bare, 0, sizeof bare);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_KIT_NONE, odin3_kit_port_index(&bare, "A"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_KIT_NONE, odin3_kit_param_index(&bare, "WIDTH"));
}

/* The log label of a node: its name, else $c<ID>, truncated to the buffer. */
static void test_node_label(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\inverter\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    odin3_node_id node = node_named("inverter");
    char label[32];
    odin3_kit_node_label(&fx.oc, node, label, sizeof label);
    TEST_ASSERT_EQUAL_STRING("inverter", label);
    char small[4];
    odin3_kit_node_label(&fx.oc, node, small, sizeof small);
    TEST_ASSERT_EQUAL_STRING("inv", small);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, node, 0));
    char want[32];
    (void)snprintf(want, sizeof want, "$c%u", (unsigned)node.v);
    odin3_kit_node_label(&fx.oc, node, label, sizeof label);
    TEST_ASSERT_EQUAL_STRING(want, label);
}

/* One lookup by name for ports and parameters, one "not found". */
static void test_name_lookup(void) {
    build("module \\m\n  wire input 1 \\a\nend\n");
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$pmux"), &type));
    const odin3_celltype_def *def = odin3_celltype_get(fx.res.design, type);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_kit_port_index(def, "B"));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_kit_port_index(def, "Y"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_KIT_NONE, odin3_kit_port_index(def, "Q"));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_kit_param_index(def, "S_WIDTH"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_KIT_NONE, odin3_kit_param_index(def, "A_SIGNED"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canon_consts);
    RUN_TEST(test_canon_leaves_sop);
    RUN_TEST(test_const_net_on_demand);
    RUN_TEST(test_const_net_stale);
    RUN_TEST(test_connect_alias);
    RUN_TEST(test_connect_constant);
    RUN_TEST(test_connect_self);
    RUN_TEST(test_connect_swap);
    RUN_TEST(test_connect_port);
    RUN_TEST(test_connect_misuse);
    RUN_TEST(test_new_cell);
    RUN_TEST(test_new_cell_budget);
    RUN_TEST(test_rebuild_same_signature);
    RUN_TEST(test_rebuild_new_type);
    RUN_TEST(test_rebuild_width_mismatch);
    RUN_TEST(test_rebuild_moves_output);
    RUN_TEST(test_prov_parents);
    RUN_TEST(test_invert_map);
    RUN_TEST(test_topo_order);
    RUN_TEST(test_invert_mux01);
    RUN_TEST(test_topo_ties);
    RUN_TEST(test_topo_loop);
    RUN_TEST(test_keep);
    RUN_TEST(test_keep_bits);
    RUN_TEST(test_readers);
    RUN_TEST(test_rtlil_folded_alias);
    RUN_TEST(test_rebuild_oom);
    RUN_TEST(test_connect_oom);
    RUN_TEST(test_new_cell_oom);
    RUN_TEST(test_reads_oom);
    RUN_TEST(test_limits_clamped);
    RUN_TEST(test_vec_helpers);
    RUN_TEST(test_helpers_empty);
    RUN_TEST(test_node_label);
    RUN_TEST(test_name_lookup);
    return UNITY_END();
}
