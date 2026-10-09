/*
 * test_abi_ir.c — the public IR ABI (design, module, node, pin, net, wire, attributes, logging,
 * pass scripts) walked over a BLIF fixture; links only libodin3.so, like a plugin.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif

enum { LOG_CAP = 1 << 14, PATH_BUF = 512 };

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%d %s\n", (int)level, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

static void reset_log(void) {
    log_text[0] = '\0';
    log_len = 0;
}

/* Reads fixture `file` into the design through the read_blif pass. */
static void read_fixture(const char *file) {
    char args[PATH_BUF];
    (void)snprintf(args, sizeof args, "\"%s/%s\"", ODIN3_BLIF_FIXTURES, file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_blif", args));
}

void setUp(void) {
    reset_log();
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    read_fixture("hand_body.blif");
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top(NULL));
}

/* --- helpers over the ABI ------------------------------------------------------------------ */

static uint32_t module_named(const char *name) {
    uint32_t module = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_lookup_module(design, name, &module));
    return module;
}

static uint32_t top_module(void) {
    uint32_t top = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
    return top;
}

static const char *module_name(uint32_t module) {
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_name(design, module, &name));
    return name;
}

static uint32_t node_named(uint32_t module, const char *name) {
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(design, module, name, &node));
    return node;
}

static uint32_t net_named(uint32_t module, const char *name) {
    uint32_t net = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, module, name, &net));
    return net;
}

static const char *type_name(odin3_ref node) {
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_type_name(design, node, &name));
    return name;
}

static const char *param_text(odin3_ref node, uint32_t index) {
    const char *text = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_text(design, node, index, &text));
    return text;
}

static const char *param_name(odin3_ref node, uint32_t index) {
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_name(design, node, index, &name));
    return name;
}

static uint32_t pin_node(odin3_ref pin) {
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_node(design, pin, &node));
    return node;
}

static uint32_t pin_net(odin3_ref pin) {
    uint32_t net = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, pin, &net));
    return net;
}

static uint32_t net_driver(odin3_ref net) {
    uint32_t pin = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver(design, net, &pin));
    return pin;
}

static const char *attr(odin3_obj obj, const char *key) {
    const char *value = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_get_string(design, obj, key, &value));
    return value;
}

/* The first node of cell type `type` with a pin on net, 0 when none. */
static uint32_t sink_of_type(odin3_ref net, const char *type) {
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_pin_count(design, net, &count));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pin = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_pin_at(design, net, i, &pin));
        uint32_t node = pin_node((odin3_ref){net.module, pin});
        if (strcmp(type_name((odin3_ref){net.module, node}), type) == 0) {
            return node;
        }
    }
    return 0;
}

/* --- design -------------------------------------------------------------------------------- */

static void test_abi_version_is_3(void) {
    TEST_ASSERT_EQUAL_UINT32(3, odin3_abi_version());
}

static void test_design_modules_and_top(void) {
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count); /* top, sub; bb is a black box, not a module */
    uint32_t first = 0;
    uint32_t second = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_at(design, 0, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_at(design, 1, &second));
    TEST_ASSERT_EQUAL_STRING("top", module_name(first));
    TEST_ASSERT_EQUAL_STRING("sub", module_name(second));
    TEST_ASSERT_EQUAL_UINT32(first, top_module()); /* BLIF: the first model is the top */
    TEST_ASSERT_EQUAL_UINT32(second, module_named("sub"));
    TEST_ASSERT_EQUAL_UINT32(0, module_named("bb"));
    TEST_ASSERT_EQUAL_UINT32(0, module_named("nope"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top_module(design, second));
    TEST_ASSERT_EQUAL_UINT32(second, top_module());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_top_module(design, 3));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_top_module(design, 0));
    TEST_ASSERT_EQUAL_UINT32(second, top_module());
    uint32_t keep = 77;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_module_at(design, 2, &keep));
    TEST_ASSERT_EQUAL_UINT32(77, keep); /* outputs unchanged on failure */
}

static void test_design_null_and_bad_arguments(void) {
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_module_count(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_module_count(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_top_module(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_top_module(NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_lookup_module(design, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_lookup_module(NULL, "top", &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(NULL, "stats", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "nope", NULL));
    TEST_ASSERT_TRUE(log_has("unknown pass 'nope'"));
    TEST_ASSERT_TRUE(log_has("odin3_design_get_module_count: invalid argument"));
    odin3_design_destroy(NULL); /* a no-op */
}

static void test_run_pass_without_arguments(void) {
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "stats", NULL));
    TEST_ASSERT_TRUE(log_has("stats: module top: ports 6, nodes 18, nets 14, wires 6"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "stats", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "stats", "x"));
}

/* --- modules ------------------------------------------------------------------------------- */

static void test_module_counts_match_stats(void) {
    uint32_t top = module_named("top");
    uint32_t nodes = 0;
    uint32_t nets = 0;
    uint32_t wires = 0;
    uint32_t ports = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &nodes));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_net_count(design, top, &nets));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_wire_count(design, top, &wires));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_port_count(design, top, &ports));
    TEST_ASSERT_EQUAL_UINT32(18, nodes); /* the `stats` line for hand_body.blif */
    TEST_ASSERT_EQUAL_UINT32(14, nets);
    TEST_ASSERT_EQUAL_UINT32(6, wires);
    TEST_ASSERT_EQUAL_UINT32(6, ports);
    uint32_t node_end = 0;
    uint32_t net_end = 0;
    uint32_t wire_end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(design, top, &node_end));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_net_end(design, top, &net_end));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_wire_end(design, top, &wire_end));
    TEST_ASSERT_EQUAL_UINT32(nodes + 1, node_end); /* nothing deleted: IDs are dense */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(nets + 1, net_end);
    TEST_ASSERT_EQUAL_UINT32(wires + 1, wire_end);
}

/* Walks every live node of a module in ID order and builds the cell-type histogram text. */
static void test_walk_nodes_histogram(void) {
    uint32_t sub = module_named("sub");
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(design, sub, &end));
    char seen[PATH_BUF] = "";
    for (uint32_t id = 1; id < end; id++) {
        bool live = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_is_live(design, (odin3_ref){sub, id}, &live));
        TEST_ASSERT_TRUE(live);
        (void)strncat(seen, type_name((odin3_ref){sub, id}), sizeof seen - strlen(seen) - 2);
        (void)strncat(seen, " ", sizeof seen - strlen(seen) - 1);
    }
    TEST_ASSERT_EQUAL_STRING("$port_in $port_out $sop ", seen);
}

static void test_module_ports(void) {
    uint32_t top = module_named("top");
    const char *expected[] = {"a", "b", "clk", "y", "q", "l"};
    const odin3_dir dirs[] = {ODIN3_DIR_IN,  ODIN3_DIR_IN,  ODIN3_DIR_IN,
                              ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_OUT};
    const char *types[] = {"$port_in", "$port_out"};
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t node = 0;
        uint32_t wire = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_module_get_port_node(design, (odin3_ref){top, i}, &node));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_module_get_port_wire(design, (odin3_ref){top, i}, &wire));
        const char *name = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_get_name(design, (odin3_ref){top, wire}, &name));
        TEST_ASSERT_EQUAL_STRING(expected[i], name);
        TEST_ASSERT_EQUAL_STRING(types[dirs[i] == ODIN3_DIR_OUT],
                                 type_name((odin3_ref){top, node}));
        odin3_granularity gran = ODIN3_GRAN_WORD;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_node_get_granularity(design, (odin3_ref){top, node}, &gran));
        TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_PORT, gran);
    }
    uint32_t wire = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_wire(design, top, "q", &wire));
    uint32_t q_wire = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_get_port_wire(design, (odin3_ref){top, 4}, &q_wire));
    TEST_ASSERT_EQUAL_UINT32(q_wire, wire);
    uint32_t keep = 9;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_module_get_port_node(design, (odin3_ref){top, 6}, &keep));
    TEST_ASSERT_EQUAL_UINT32(9, keep);
}

static void test_module_lookups(void) {
    uint32_t top = module_named("top");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node_named(top, "u_sub"));
    TEST_ASSERT_EQUAL_UINT32(0, node_named(top, "nope"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, net_named(top, "n1"));
    TEST_ASSERT_EQUAL_UINT32(0, net_named(top, "nope"));
    uint32_t wire = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_wire(design, top, "nope", &wire));
    TEST_ASSERT_EQUAL_UINT32(0, wire);
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_lookup_node(design, top, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_lookup_net(design, 0, "n1", &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_lookup_wire(design, 3, "q", &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_get_name(design, 3, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_get_node_end(NULL, top, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_get_net_count(design, 0, &out));
}

/* --- nodes, pins, nets --------------------------------------------------------------------- */

static void test_subckt_instance(void) {
    uint32_t top = module_named("top");
    odin3_ref sub = {top, node_named(top, "u_sub")};
    TEST_ASSERT_EQUAL_STRING("sub", type_name(sub));
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_name(design, sub, &name));
    TEST_ASSERT_EQUAL_STRING("u_sub", name);
    odin3_granularity gran = ODIN3_GRAN_WORD;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_granularity(design, sub, &gran));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_MODULE, gran);
    uint32_t params = 9;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_count(design, sub, &params));
    TEST_ASSERT_EQUAL_UINT32(0, params);
    uint32_t ports = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_count(design, sub, &ports));
    TEST_ASSERT_EQUAL_UINT32(2, ports); /* sub's ports: i[0..1], o */
    const char *port = NULL;
    odin3_dir dir = ODIN3_DIR_INOUT;
    uint32_t width = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_name(design, sub, 0, &port));
    TEST_ASSERT_EQUAL_STRING("i", port);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_dir(design, sub, 0, &dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_width(design, sub, 0, &width));
    TEST_ASSERT_EQUAL_UINT32(2, width);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_dir(design, sub, 1, &dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, dir);
    odin3_span all = {0, 0};
    odin3_span bits = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, sub, &all));
    TEST_ASSERT_EQUAL_UINT32(3, all.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_pins(design, sub, 0, &bits));
    TEST_ASSERT_EQUAL_UINT32(all.first, bits.first);
    TEST_ASSERT_EQUAL_UINT32(2, bits.count);
    /* .subckt sub i[1]=a i[0]=b: bit 1 of port i is on the net of input a */
    odin3_ref bit1 = {top, bits.first + 1};
    uint32_t pin_port = 9;
    uint32_t pin_bit = 9;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_port(design, bit1, &pin_port));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_bit(design, bit1, &pin_bit));
    TEST_ASSERT_EQUAL_UINT32(0, pin_port);
    TEST_ASSERT_EQUAL_UINT32(1, pin_bit);
    TEST_ASSERT_EQUAL_UINT32(sub.id, pin_node(bit1));
    uint32_t a_node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_get_port_node(design, (odin3_ref){top, 0}, &a_node));
    odin3_span a_pins = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, (odin3_ref){top, a_node}, &a_pins));
    TEST_ASSERT_EQUAL_UINT32(pin_net((odin3_ref){top, a_pins.first}), pin_net(bit1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_get_port_name(design, sub, 2, &port));
}

static void test_sop_parameters(void) {
    uint32_t top = module_named("top");
    odin3_ref n1 = {top, net_named(top, "n1")};
    uint32_t driver = net_driver(n1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, driver);
    odin3_ref sop = {top, pin_node((odin3_ref){top, driver})};
    TEST_ASSERT_EQUAL_STRING("$sop", type_name(sop)); /* .names a b n1 / 11 1 */
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_count(design, sop, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_STRING("WIDTH", param_name(sop, 0));
    TEST_ASSERT_EQUAL_STRING("COVER", param_name(sop, 1));
    odin3_value_kind kind = ODIN3_VAL_STRING;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_kind(design, sop, 0, &kind));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_kind(design, sop, 1, &kind));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_COVER, kind);
    int64_t width = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_int(design, sop, 0, &width));
    TEST_ASSERT_EQUAL_INT64(2, width);
    TEST_ASSERT_EQUAL_STRING("2", param_text(sop, 0));
    TEST_ASSERT_EQUAL_STRING("11 1\n", param_text(sop, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_get_param_int(design, sop, 1, &width));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_get_param_text(design, sop, 2, NULL));
    const char *text = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_get_param_text(design, sop, 2, &text));
    TEST_ASSERT_NULL(text);
}

static void test_constant_cover_and_sub_cover(void) {
    uint32_t top = module_named("top");
    /* .names k1 / 1: a zero-input cover with one row */
    odin3_ref k1 = {top,
                    pin_node((odin3_ref){top, net_driver((odin3_ref){top, net_named(top, "k1")})})};
    TEST_ASSERT_EQUAL_STRING("$sop", type_name(k1));
    TEST_ASSERT_EQUAL_STRING("0", param_text(k1, 0));
    TEST_ASSERT_EQUAL_STRING("1\n", param_text(k1, 1));
    /* .names k0 (no rows): constant 0 */
    odin3_ref k0 = {top,
                    pin_node((odin3_ref){top, net_driver((odin3_ref){top, net_named(top, "k0")})})};
    TEST_ASSERT_EQUAL_STRING("", param_text(k0, 1));
    uint32_t sub = module_named("sub");
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(design, sub, &end));
    odin3_ref sop = {sub, end - 1};
    TEST_ASSERT_EQUAL_STRING("$sop", type_name(sop));
    TEST_ASSERT_EQUAL_STRING("1- 1\n-1 1\n", param_text(sop, 1));
}

static void test_latch_parameters(void) {
    uint32_t top = module_named("top");
    /* .latch n1 q[0] re clk 2: a rising-edge flip-flop reading n1, INIT 2 */
    odin3_ref n1 = {top, net_named(top, "n1")};
    odin3_ref dff = {top, sink_of_type(n1, "$_DFF_P_")};
    TEST_ASSERT_NOT_EQUAL_UINT32(0, dff.id);
    TEST_ASSERT_EQUAL_STRING("INIT", param_name(dff, 0));
    int64_t init = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_int(design, dff, 0, &init));
    TEST_ASSERT_EQUAL_INT64(2, init);
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_name(design, dff, &name));
    TEST_ASSERT_EQUAL_STRING("", name); /* unnamed */
    const char *port = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_name(design, dff, 2, &port));
    TEST_ASSERT_EQUAL_STRING("Q", port);
}

static void test_net_pins_drivers_first(void) {
    uint32_t top = module_named("top");
    odin3_ref n1 = {top, net_named(top, "n1")};
    uint32_t count = 0;
    uint32_t drivers = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_pin_count(design, n1, &count));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver_count(design, n1, &drivers));
    TEST_ASSERT_EQUAL_UINT32(4, count); /* its $sop, then .names n1 y, .latch n1, .subckt bb */
    TEST_ASSERT_EQUAL_UINT32(1, drivers);
    uint32_t first = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_pin_at(design, n1, 0, &first));
    TEST_ASSERT_EQUAL_UINT32(net_driver(n1), first);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pin = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_pin_at(design, n1, i, &pin));
        TEST_ASSERT_EQUAL_UINT32(n1.id, pin_net((odin3_ref){top, pin}));
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sink_of_type(n1, "bb"));
    uint32_t keep = 5;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_get_pin_at(design, n1, 4, &keep));
    TEST_ASSERT_EQUAL_UINT32(5, keep);
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_name(design, n1, &name));
    TEST_ASSERT_EQUAL_STRING("n1", name);
    bool live = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_is_live(design, n1, &live));
    TEST_ASSERT_TRUE(live);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_alias_count(design, n1, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_get_alias_name(design, n1, 0, &name));
}

static void test_unconnected_pin_and_port_net(void) {
    uint32_t top = module_named("top");
    /* .subckt bb x=n1: output z of the black box is left open */
    odin3_ref bb = {top, sink_of_type((odin3_ref){top, net_named(top, "n1")}, "bb")};
    odin3_span z_pins = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_pins(design, bb, 1, &z_pins));
    TEST_ASSERT_EQUAL_UINT32(1, z_pins.count);
    TEST_ASSERT_EQUAL_UINT32(0, pin_net((odin3_ref){top, z_pins.first}));
    odin3_granularity gran = ODIN3_GRAN_WORD;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_granularity(design, bb, &gran));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, gran);
    /* the BLIF reader names a port bit's net after the port */
    uint32_t clk = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_port_node(design, (odin3_ref){top, 2}, &clk));
    odin3_span pins = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, (odin3_ref){top, clk}, &pins));
    const char *name = NULL;
    odin3_ref net = {top, pin_net((odin3_ref){top, pins.first})};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_name(design, net, &name));
    TEST_ASSERT_EQUAL_STRING("clk", name);
    TEST_ASSERT_EQUAL_UINT32(net.id, net_named(top, "clk"));
}

static void test_ref_bad_ids(void) {
    uint32_t top = module_named("top");
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(design, top, &end));
    const char *name = "keep";
    bool live = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_name(design, (odin3_ref){top, 0}, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_name(design, (odin3_ref){top, end}, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_name(design, (odin3_ref){3, 1}, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_name(NULL, (odin3_ref){top, 1}, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_name(design, (odin3_ref){top, 1}, NULL));
    TEST_ASSERT_EQUAL_STRING("keep", name);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_is_live(design, (odin3_ref){top, end}, &live));
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pin_get_net(design, (odin3_ref){top, 1000000}, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_get_driver(design, (odin3_ref){top, 0}, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_wire_is_live(design, (odin3_ref){top, 100}, &live));
    odin3_span span = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_get_pins(design, (odin3_ref){top, end}, &span));
    TEST_ASSERT_TRUE(log_has("odin3_node_get_name: invalid argument"));
}

/* --- attributes ---------------------------------------------------------------------------- */

static void test_attributes_from_blif(void) {
    uint32_t top = module_named("top");
    odin3_obj sub = {top, ODIN3_OBJ_NODE, node_named(top, "u_sub")};
    /* .attr src "top.v:3", .param P 01 01, .attr src "top.v:4": a repeated key keeps its last */
    TEST_ASSERT_EQUAL_STRING("\"top.v:4\"", attr(sub, "blif.attr:src"));
    TEST_ASSERT_EQUAL_STRING("01 01", attr(sub, "blif.param:P"));
    TEST_ASSERT_EQUAL_STRING("blif.attr:src blif.param:P", attr(sub, "blif_extras"));
    TEST_ASSERT_NULL(attr(sub, "blif.attr:nope"));
    TEST_ASSERT_NULL(attr(sub, "never interned key"));
}

static void test_attribute_set_and_kinds(void) {
    uint32_t top = module_named("top");
    odin3_obj objs[] = {
        {top, ODIN3_OBJ_NODE, node_named(top, "u_sub")},
        {top, ODIN3_OBJ_NET, net_named(top, "n1")},
        {top, ODIN3_OBJ_WIRE, 1},
        {top, ODIN3_OBJ_MODULE, 0},
    };
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set_string(design, objs[i], "note", "first"));
        TEST_ASSERT_EQUAL_STRING("first", attr(objs[i], "note"));
    }
    odin3_obj sub_module = {module_named("sub"), ODIN3_OBJ_MODULE, 0};
    TEST_ASSERT_NULL(attr(sub_module, "note"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set_string(design, objs[0], "", "v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set_string(design, objs[0], NULL, "v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set_string(design, objs[0], "k", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set_string(NULL, objs[0], "k", "v"));
    odin3_obj bad_kind = {top, (odin3_objkind)7, 1};
    odin3_obj bad_id = {top, ODIN3_OBJ_NET, 1000};
    odin3_obj bad_module = {0, ODIN3_OBJ_MODULE, 0};
    const char *value = "keep";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set_string(design, bad_kind, "k", "v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_get_string(design, bad_kind, "k", &value));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_get_string(design, bad_id, "k", &value));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_get_string(design, bad_module, "k", &value));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_get_string(design, objs[0], NULL, &value));
    TEST_ASSERT_EQUAL_STRING("keep", value);
}

/* Review Focus 2: strings stay valid until the next IR mutation; after one, fetch them again. */
static void test_strings_refetched_after_mutation(void) {
    uint32_t top = module_named("top");
    odin3_obj sub = {top, ODIN3_OBJ_NODE, node_named(top, "u_sub")};
    const char *before = attr(sub, "blif.attr:src");
    const char *type_before = type_name((odin3_ref){top, sub.id});
    TEST_ASSERT_EQUAL_STRING("\"top.v:4\"", before);
    /* a mutation through the ABI: the old pointer is no longer promised; a re-fetch is current */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set_string(design, sub, "blif.attr:src", "new.v:9"));
    TEST_ASSERT_EQUAL_STRING("new.v:9", attr(sub, "blif.attr:src"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set_string(design, sub, "blif.attr:src", "v2"));
    TEST_ASSERT_EQUAL_STRING("v2", attr(sub, "blif.attr:src"));
    /* a mutation through passes: compact renumbers nothing here, hierarchy re-selects the top */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "compact", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "hierarchy", "--top sub"));
    TEST_ASSERT_EQUAL_STRING("sub", module_name(top_module()));
    uint32_t again = node_named(top, "u_sub");
    TEST_ASSERT_EQUAL_UINT32(sub.id, again);
    TEST_ASSERT_EQUAL_STRING("sub", type_name((odin3_ref){top, again}));
    TEST_ASSERT_EQUAL_STRING(type_before, type_name((odin3_ref){top, again}));
    TEST_ASSERT_EQUAL_STRING("v2", attr(sub, "blif.attr:src"));
    /* computed text is cached, so a second fetch gives an equal string */
    odin3_ref sop = {
        top, pin_node((odin3_ref){top, net_driver((odin3_ref){top, net_named(top, "n1")})})};
    const char *cover = param_text(sop, 1);
    TEST_ASSERT_EQUAL_STRING(cover, param_text(sop, 1));
}

/* --- passes and scripts -------------------------------------------------------------------- */

static void test_pass_listing(void) {
    uint32_t count = odin3_pass_get_count();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(6, count);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = NULL;
        const char *help = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_get_name(i, &name));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_get_help(i, &help));
        TEST_ASSERT_EQUAL_INT(0, strncmp(help, name, strlen(name)));
        found |= strcmp(name, "read_blif") == 0;
    }
    TEST_ASSERT_TRUE(found);
    const char *name = "keep";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_get_name(count, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_get_help(count, &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_get_name(0, NULL));
    TEST_ASSERT_EQUAL_STRING("keep", name);
}

static void test_scripts(void) {
    odin3_script_src inline_src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_script_resolve("stats; check", inline_src));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_script_resolve("stats; nope", inline_src));
    TEST_ASSERT_TRUE(log_has("-p: command 2: unknown pass 'nope'"));
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_script(design, "check; stats", inline_src));
    TEST_ASSERT_TRUE(log_has("stats: design: modules 2, top top"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_run_script(NULL, "stats", inline_src));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_script(design, NULL, inline_src));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_script_resolve(NULL, inline_src));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_script_resolve_file("/nonexistent/x.o3"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_design_run_script_file(design, "/nonexistent/x.o3"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_script_file(NULL, "x.o3"));
}

static void test_pass_top_option(void) {
    odin3_design *other = odin3_design_create();
    TEST_ASSERT_NOT_NULL(other);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top("sub"));
    char args[PATH_BUF];
    (void)snprintf(args, sizeof args, "%s/hand_body.blif", ODIN3_BLIF_FIXTURES);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(other, "read_blif", args));
    uint32_t top = 0;
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(other, &top));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_name(other, top, &name));
    TEST_ASSERT_EQUAL_STRING("sub", name);
    odin3_design_destroy(other);
    odin3_pass_set_check(true); /* Release: check around passes; Debug always checks */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "stats", NULL));
    odin3_pass_set_check(false);
}

/* --- logging ------------------------------------------------------------------------------- */

static void test_logging(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_INFO, odin3_log_get_level());
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_write(ODIN3_LOG_WARN, "from a plugin"));
    TEST_ASSERT_TRUE(log_has("1 from a plugin"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_ERROR, odin3_log_get_level());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_write(ODIN3_LOG_WARN, "filtered"));
    TEST_ASSERT_FALSE(log_has("filtered"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_log_set_level(ODIN3_LOG_LEVEL_COUNT));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_log_set_level((odin3_log_level)-1));
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_ERROR, odin3_log_get_level());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_log_write(ODIN3_LOG_ERROR, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_log_write(ODIN3_LOG_LEVEL_COUNT, "x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_write(ODIN3_LOG_DEBUG, "%s is not a format"));
    TEST_ASSERT_TRUE(log_has("3 %s is not a format"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_abi_version_is_3);
    RUN_TEST(test_design_modules_and_top);
    RUN_TEST(test_design_null_and_bad_arguments);
    RUN_TEST(test_run_pass_without_arguments);
    RUN_TEST(test_module_counts_match_stats);
    RUN_TEST(test_walk_nodes_histogram);
    RUN_TEST(test_module_ports);
    RUN_TEST(test_module_lookups);
    RUN_TEST(test_subckt_instance);
    RUN_TEST(test_sop_parameters);
    RUN_TEST(test_constant_cover_and_sub_cover);
    RUN_TEST(test_latch_parameters);
    RUN_TEST(test_net_pins_drivers_first);
    RUN_TEST(test_unconnected_pin_and_port_net);
    RUN_TEST(test_ref_bad_ids);
    RUN_TEST(test_attributes_from_blif);
    RUN_TEST(test_attribute_set_and_kinds);
    RUN_TEST(test_strings_refetched_after_mutation);
    RUN_TEST(test_pass_listing);
    RUN_TEST(test_scripts);
    RUN_TEST(test_pass_top_option);
    RUN_TEST(test_logging);
    return UNITY_END();
}
