/*
 * test_ir_wire.c — unit tests for ports, wires, aliases, merge, replace, create-connected and
 * attributes (IR-2, IR-3, IR-7, IR-10, IR-14, IR-15).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QUAD_W = 4,
    QUAD_PINS = 3 * QUAD_W + 1,
    MANY_PORTS = 2000,
    OOM_LIMIT = 10000,
    NAME_BUF = 32,
    NAME_MAP_FULL = 13, /* u64map: 16 slots at 85% load; the 14th entry grows it */
    WIRE_PAGE = 1 << ODIN3_WIRE_PAGE_SHIFT,
    SNAP_MAX = 64,
    SMALL_MODULE_BYTES = 48 * 1024,
    ADD_PORT_ALLOCS = 9,    /* ports, port defs, node/pin/net/wire pages, arena, pool, wire map */
    WIRE_CREATE_ALLOCS = 4, /* wire page, net page, arena, wire map */
    MERGE_ALLOCS = 2,       /* alias table, keep's pin block */
    CONNECTED_ALLOCS = 5,   /* arena, node page, pin page, node map, a net's pin block */
    ATTR_ALLOCS = 3,        /* arena, attribute table, attribute map */
};

static odin3_design *design;
static odin3_module *module;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static uint32_t numbered_name(const char *prefix, uint32_t index) {
    char buf[NAME_BUF];
    (void)snprintf(buf, sizeof buf, "%s%u", prefix, (unsigned)index);
    return intern(buf);
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(name), &id));
    return id;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    TEST_ASSERT_NOT_NULL(mod);
    return mod;
}

/* A new design holding one empty module "top"; replaces the current one. */
static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    module = new_module("top");
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = NULL;
    fresh_design();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
}

/* A test type: A in (WIDTH), B in (WIDTH), Y out (WIDTH), C in (1); 13 pins at WIDTH 4. */
static const odin3_param_def k_quad_params[] = {
    {"WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, QUAD_W, NULL, 0, 0, 0}},
};
static const odin3_port_def k_quad_ports[] = {
    {"A", ODIN3_DIR_IN, false, 0, "WIDTH", NULL, NULL},
    {"B", ODIN3_DIR_IN, false, 0, "WIDTH", NULL, NULL},
    {"Y", ODIN3_DIR_OUT, false, 0, "WIDTH", NULL, NULL},
    {"C", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
};
static const odin3_celltype_def k_quad = {
    "test_t4_quad", ODIN3_GRAN_WORD, 0, k_quad_ports, 4, k_quad_params, 1, NULL, NULL, NULL};

/* A one-port WIDTH-parameter sink shaped like $port_out (which node_create refuses). */
static const odin3_port_def k_sink_ports[] = {{"P", ODIN3_DIR_IN, false, 0, "WIDTH", NULL, NULL}};
static const odin3_celltype_def k_sink = {
    "test_t4_sink", ODIN3_GRAN_WORD, 0, k_sink_ports, 1, k_quad_params, 1, NULL, NULL, NULL};

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_node_id node_named(const char *type, uint32_t name, const odin3_value *params) {
    odin3_node_spec spec = {type_id(type), name, {0}, params, params != NULL ? 1 : 0};
    odin3_node_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &id));
    return id;
}

static odin3_node_id node_of(const char *type) {
    return node_named(type, 0, NULL);
}

static odin3_net_id net_named(const char *name) {
    odin3_net_id id = {0};
    uint32_t str = name != NULL ? intern(name) : 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, str, (odin3_prov_id){0}, &id));
    return id;
}

static odin3_pin_id pin_of(odin3_node_id node, uint32_t index) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    TEST_ASSERT_TRUE(index < pins.count);
    return (odin3_pin_id){pins.first.v + index};
}

static void connect(odin3_pin_id pin, odin3_net_id net) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(module, pin, net));
}

static odin3_node_id add_port(const char *name, odin3_dir dir, uint32_t width) {
    odin3_port_spec spec = {intern(name), dir, width, width == 1, {0}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, &node));
    TEST_ASSERT_TRUE(odin3_node_live(module, node));
    return node;
}

static odin3_wire_id wire_range(const char *name, int32_t msb, int32_t lsb,
                                const odin3_net_id *nets) {
    odin3_wire_spec spec = {name != NULL ? intern(name) : 0, msb, lsb, false, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, nets, &wire));
    TEST_ASSERT_TRUE(odin3_wire_live(module, wire));
    return wire;
}

static bool has_alias(odin3_net_id net, odin3_net_alias want) {
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        if (alias.wb.wire.v == want.wb.wire.v && alias.wb.bit == want.wb.bit &&
            alias.name == want.name) {
            return true;
        }
    }
    return false;
}

/* The net's aliases are exactly want[0 .. count), in this order (insertion order, oldest first). */
static void assert_aliases(odin3_net_id net, const odin3_net_alias *want, uint32_t count) {
    uint32_t cursor = 0;
    uint32_t seen = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        TEST_ASSERT_TRUE(seen < count);
        TEST_ASSERT_EQUAL_UINT32(want[seen].wb.wire.v, alias.wb.wire.v);
        TEST_ASSERT_EQUAL_UINT32(want[seen].wb.bit, alias.wb.bit);
        TEST_ASSERT_EQUAL_UINT32(want[seen].name, alias.name);
        seen++;
    }
    TEST_ASSERT_EQUAL_UINT32(count, seen);
    TEST_ASSERT_FALSE(odin3_net_alias_next(module, net, &cursor, &alias)); /* stays ended */
}

static odin3_net_alias wb_alias(odin3_wire_id wire, uint32_t bit) {
    return (odin3_net_alias){{wire, bit}, 0};
}

static odin3_net_alias name_alias(uint32_t name) {
    return (odin3_net_alias){{{0}, 0}, name};
}

static void assert_primary(odin3_net_id net, odin3_wire_id wire, uint32_t bit) {
    odin3_wirebit primary = odin3_net_primary(module, net);
    TEST_ASSERT_EQUAL_UINT32(wire.v, primary.wire.v);
    TEST_ASSERT_EQUAL_UINT32(bit, primary.bit);
}

/* Drivers first, then sinks, every pin's slot and net agree with the array. */
static void assert_partition(odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(module, net);
    uint32_t drivers = odin3_net_driver_count(module, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        TEST_ASSERT_EQUAL(i < drivers, odin3_pin_drives(module, pins.pins[i]));
        TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(module, pins.pins[i]).v);
        TEST_ASSERT_EQUAL_UINT32(i, odin3_pin_rec_cat(module, pins.pins[i])->slot);
    }
}

/* --- ports --------------------------------------------------------------------------------- */

static void assert_port_bits(odin3_node_id node, odin3_wire_id wire, uint32_t width) {
    TEST_ASSERT_EQUAL_UINT32(width, odin3_wire_width(module, wire));
    TEST_ASSERT_EQUAL_INT32((int32_t)width - 1, odin3_wire_msb(module, wire));
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_lsb(module, wire));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_wire_port_node(module, wire).v);
    TEST_ASSERT_EQUAL_UINT32(width, odin3_node_pins(module, node).count);
    for (uint32_t k = 0; k < width; k++) {
        odin3_net_id net = odin3_wire_net(module, wire, k);
        TEST_ASSERT_TRUE(odin3_net_live(module, net));
        TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(module, pin_of(node, k)).v);
        assert_primary(net, wire, k);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_name(module, net));
    }
}

static void test_add_port_creates_node_wire_and_type_port(void) {
    odin3_node_id in = add_port("a", ODIN3_DIR_IN, 4);
    odin3_node_id out = add_port("y", ODIN3_DIR_OUT, 1);
    TEST_ASSERT_EQUAL_UINT32(type_id("$port_in").v, odin3_node_type(module, in).v);
    TEST_ASSERT_EQUAL_UINT32(type_id("$port_out").v, odin3_node_type(module, out).v);
    TEST_ASSERT_EQUAL_INT64(4, odin3_node_param(module, in, 0)->i);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(module));
    TEST_ASSERT_EQUAL_UINT32(in.v, odin3_module_port(module, 0).v);
    TEST_ASSERT_EQUAL_UINT32(out.v, odin3_module_port(module, 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_port(module, 2).v);
    odin3_wire_id wire_a = odin3_module_find_wire(module, intern("a"));
    odin3_wire_id wire_y = odin3_module_find_wire(module, intern("y"));
    TEST_ASSERT_EQUAL_UINT32(wire_a.v, odin3_module_port_wire(module, 0).v);
    TEST_ASSERT_EQUAL_UINT32(wire_y.v, odin3_module_port_wire(module, 1).v);
    assert_port_bits(in, wire_a, 4);
    assert_port_bits(out, wire_y, 1);
    /* $port_in drives its nets; $port_out sinks */
    TEST_ASSERT_EQUAL_UINT32(pin_of(in, 2).v,
                             odin3_net_driver(module, odin3_wire_net(module, wire_a, 2)).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_sinks(module, odin3_wire_net(module, wire_y, 0)).count);
    /* IR-7: the module's cell type mirrors the ports */
    odin3_celltype_id type = odin3_module_celltype(module);
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    TEST_ASSERT_EQUAL_UINT32(2, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("a", def->ports[0].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, def->ports[0].dir);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_celltype_port_width(design, type, NULL, 0));
    TEST_ASSERT_FALSE(def->ports[0].scalar);
    TEST_ASSERT_EQUAL_STRING("y", def->ports[1].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[1].dir);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_port_width(design, type, NULL, 1));
    TEST_ASSERT_TRUE(def->ports[1].scalar);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_add_port_inout(void) {
    odin3_node_id io = add_port("t", ODIN3_DIR_INOUT, 2);
    TEST_ASSERT_EQUAL_UINT32(type_id("$port_inout").v, odin3_node_type(module, io).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_INOUT,
                          odin3_celltype_get(design, odin3_module_celltype(module))->ports[0].dir);
}

static void test_instance_pins_follow_ports(void) {
    (void)add_port("a", ODIN3_DIR_IN, 3);
    (void)add_port("y", ODIN3_DIR_OUT, 2);
    odin3_module *parent = new_module("parent");
    odin3_node_spec spec = {odin3_module_celltype(module), 0, {0}, NULL, 0};
    odin3_node_id inst = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(parent, &spec, &inst));
    TEST_ASSERT_EQUAL_UINT32(5, odin3_node_pins(parent, inst).count);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_node_port(parent, inst, 0).count);
    odin3_pinslice outs = odin3_node_port(parent, inst, 1);
    TEST_ASSERT_EQUAL_UINT32(2, outs.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, odin3_pin_dir(parent, outs.first));
}

static void test_add_port_refused_once_instantiated(void) {
    (void)add_port("a", ODIN3_DIR_IN, 1);
    odin3_module *parent = new_module("parent");
    odin3_node_spec spec = {odin3_module_celltype(module), 0, {0}, NULL, 0};
    odin3_node_id inst = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(parent, &spec, &inst));
    uint32_t node_end = odin3_module_node_end(module);
    uint32_t wire_end = odin3_module_wire_end(module);
    odin3_port_spec port = {intern("b"), ODIN3_DIR_IN, 1, true, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_add_port(module, &port, NULL));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_port_count(module));
    TEST_ASSERT_EQUAL_UINT32(node_end, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(wire_end, odin3_module_wire_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_get(design, odin3_module_celltype(module))->n_ports);
    /* with no live instance left the refusal lifts */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(parent, inst));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &port, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(module));
}

static void test_add_port_rejects_bad_specs(void) {
    (void)add_port("a", ODIN3_DIR_IN, 1);
    uint32_t node_end = odin3_module_node_end(module);
    uint32_t net_end = odin3_module_net_end(module);
    uint32_t wire_end = odin3_module_wire_end(module);
    odin3_port_spec bad[] = {
        {0, ODIN3_DIR_IN, 1, true, {0}},                     /* no name */
        {UINT32_MAX, ODIN3_DIR_IN, 1, true, {0}},            /* not a string ID */
        {intern("a"), ODIN3_DIR_IN, 1, true, {0}},           /* a wire has the name */
        {intern("b"), ODIN3_DIR_IN, 0, false, {0}},          /* width 0 */
        {intern("b"), ODIN3_DIR_IN, 2, true, {0}},           /* scalar but wide */
        {intern("b"), (odin3_dir)7, 1, true, {0}},           /* bad direction */
        {intern("b"), ODIN3_DIR_IN, UINT32_MAX, false, {0}}, /* wider than 2^31 */
    };
    size_t count = sizeof bad / sizeof bad[0];
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_add_port(module, &bad[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_add_port(module, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(count + 1, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_port_count(module));
    TEST_ASSERT_EQUAL_UINT32(node_end, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(net_end, odin3_module_net_end(module));
    TEST_ASSERT_EQUAL_UINT32(wire_end, odin3_module_wire_end(module));
}

/* Everything a module has reserved, including its vectors' capacity. */
static size_t module_bytes(const odin3_module *mod) {
    return odin3_pagevec_bytes_reserved(mod->nodes) + odin3_pagevec_bytes_reserved(mod->pins) +
           odin3_pagevec_bytes_reserved(mod->nets) + odin3_pagevec_bytes_reserved(mod->wires) +
           odin3_arena_bytes_reserved(mod->arena) + odin3_pinpool_bytes_reserved(&mod->pinpool) +
           mod->ports.cap * mod->ports.elem_size + mod->port_defs.cap * mod->port_defs.elem_size +
           mod->aliases.cap * mod->aliases.elem_size + mod->attrs.cap * mod->attrs.elem_size;
}

static void test_small_module_stays_small(void) {
    TEST_ASSERT_TRUE(module_bytes(module) < SMALL_MODULE_BYTES);
}

/* Ruling: the module type is not deep-copied per port; memory grows linearly in the ports. */
static void test_add_port_memory_linear(void) {
    size_t design_bytes = odin3_arena_bytes_reserved(design->arena);
    size_t start = module_bytes(module);
    for (uint32_t i = 0; i < MANY_PORTS / 2; i++) {
        odin3_port_spec spec = {numbered_name("p", i), ODIN3_DIR_IN, 1, true, {0}};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, NULL));
    }
    size_t half = module_bytes(module);
    for (uint32_t i = MANY_PORTS / 2; i < MANY_PORTS; i++) {
        odin3_port_spec spec = {numbered_name("p", i), ODIN3_DIR_OUT, 1, true, {0}};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, NULL));
    }
    size_t full = module_bytes(module);
    TEST_ASSERT_EQUAL_size_t(design_bytes, odin3_arena_bytes_reserved(design->arena));
    TEST_ASSERT_TRUE(full - half < 2 * (half - start)); /* quadratic growth would be ~3x */
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(module));
    TEST_ASSERT_EQUAL_UINT32(MANY_PORTS, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("p1999", def->ports[MANY_PORTS - 1].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[MANY_PORTS - 1].dir);
    odin3_module *parent = new_module("parent");
    odin3_node_spec spec = {odin3_module_celltype(module), 0, {0}, NULL, 0};
    odin3_node_id inst = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(parent, &spec, &inst));
    TEST_ASSERT_EQUAL_UINT32(MANY_PORTS, odin3_node_pins(parent, inst).count);
}

/* --- wires --------------------------------------------------------------------------------- */

static void test_wire_downto_creates_nets(void) {
    uint32_t net_end = odin3_module_net_end(module);
    odin3_wire_id wire = wire_range("w", 7, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(wire.v, odin3_module_find_wire(module, intern("w")).v);
    TEST_ASSERT_EQUAL_UINT32(intern("w"), odin3_wire_name(module, wire));
    TEST_ASSERT_EQUAL_UINT32(8, odin3_wire_width(module, wire));
    TEST_ASSERT_EQUAL_UINT32(net_end + 8, odin3_module_net_end(module));
    TEST_ASSERT_FALSE(odin3_wire_signed(module, wire));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_wire_port_node(module, wire).v);
    for (uint32_t k = 0; k < 8; k++) {
        odin3_net_id net = odin3_wire_net(module, wire, k);
        TEST_ASSERT_EQUAL_UINT32(net_end + k, net.v);
        assert_primary(net, wire, k);
        TEST_ASSERT_EQUAL_INT32((int32_t)k, odin3_wire_index(module, wire, k));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(module, net));
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_wire_net(module, wire, 8).v);
}

static void test_wire_upto_bit0_is_lsb(void) {
    odin3_wire_spec spec = {intern("u"), 0, 7, true, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &wire));
    TEST_ASSERT_EQUAL_UINT32(8, odin3_wire_width(module, wire));
    TEST_ASSERT_TRUE(odin3_wire_signed(module, wire));
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_msb(module, wire));
    TEST_ASSERT_EQUAL_INT32(7, odin3_wire_lsb(module, wire));
    TEST_ASSERT_EQUAL_INT32(7, odin3_wire_index(module, wire, 0)); /* bit 0: the LSB, u[7] */
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_index(module, wire, 7)); /* bit 7: the MSB, u[0] */
    odin3_wire_id neg = wire_range("n", -1, -3, NULL);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_wire_width(module, neg));
    TEST_ASSERT_EQUAL_INT32(-3, odin3_wire_index(module, neg, 0));
    TEST_ASSERT_EQUAL_INT32(-1, odin3_wire_index(module, neg, 2));
}

static void test_wire_over_existing_nets(void) {
    odin3_wire_id wire_a = wire_range("a", 1, 0, NULL);
    odin3_net_id a0 = odin3_wire_net(module, wire_a, 0);
    odin3_net_id a1 = odin3_wire_net(module, wire_a, 1);
    odin3_net_id loose = net_named("loose");
    uint32_t net_end = odin3_module_net_end(module);
    odin3_net_id nets[] = {a1, a0, loose, loose};
    odin3_wire_id wire_b = wire_range("b", 3, 0, nets);
    TEST_ASSERT_EQUAL_UINT32(net_end, odin3_module_net_end(module)); /* no new nets */
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(nets[k].v, odin3_wire_net(module, wire_b, k).v);
    }
    assert_primary(a1, wire_a, 1); /* keeps its primary; b[0] is an alias */
    TEST_ASSERT_TRUE(has_alias(a1, wb_alias(wire_b, 0)));
    TEST_ASSERT_TRUE(has_alias(a0, wb_alias(wire_b, 1)));
    assert_primary(loose, wire_b, 2); /* had none: b[2] becomes primary, b[3] an alias */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_alias_count(module, loose));
    TEST_ASSERT_TRUE(has_alias(loose, wb_alias(wire_b, 3)));
    odin3_net_id twice[] = {loose, loose};
    odin3_wire_id wire_c = wire_range("c", 1, 0, twice);
    odin3_net_alias want[] = {wb_alias(wire_b, 3), wb_alias(wire_c, 0), wb_alias(wire_c, 1)};
    assert_aliases(loose, want, 3);                    /* insertion order */
    odin3_wire_id anon = wire_range(NULL, 0, 0, NULL); /* unnamed wires are allowed */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_wire_name(module, anon));
}

static void test_wire_create_rejects(void) {
    (void)wire_range("w", 0, 0, NULL);
    odin3_net_id dead = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, dead));
    uint32_t net_end = odin3_module_net_end(module);
    uint32_t wire_end = odin3_module_wire_end(module);
    odin3_wire_spec dup = {intern("w"), 0, 0, false, {0}};
    odin3_wire_spec bad_name = {UINT32_MAX, 0, 0, false, {0}};
    odin3_wire_spec huge = {intern("h"), INT32_MAX, INT32_MIN, false, {0}};
    odin3_wire_spec ok = {intern("x"), 0, 0, false, {0}};
    odin3_net_id far[] = {{999}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, &dup, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, &bad_name, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, &huge, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, &ok, &dead, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_create(module, &ok, far, NULL));
    TEST_ASSERT_EQUAL_size_t(6, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(net_end, odin3_module_net_end(module));
    TEST_ASSERT_EQUAL_UINT32(wire_end, odin3_module_wire_end(module));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_wire(module, intern("x")).v);
}

/* A net with a primary already (bit 0 of a new wire named name). */
static odin3_net_id net_with_primary(const char *name) {
    return odin3_wire_net(module, wire_range(name, 0, 0, NULL), 0);
}

static void test_wire_add_alias_rebinds(void) {
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    odin3_wirebit bit0 = {wire, 0};
    odin3_net_id own = odin3_wire_net(module, wire, 0);
    /* to a net without a primary: the bit becomes its primary (as in wire_create) */
    odin3_net_id bare = net_named("m");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, bit0, bare));
    TEST_ASSERT_EQUAL_UINT32(bare.v, odin3_wire_net(module, wire, 0).v);
    assert_primary(bare, wire, 0);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(module, bare));
    assert_primary(own, (odin3_wire_id){0}, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, own));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, bit0, bare)); /* no-op */
    assert_primary(bare, wire, 0);
    /* primary -> alias of a net that has a primary */
    odin3_net_id held = net_with_primary("h");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, bit0, held));
    odin3_net_alias want[] = {wb_alias(wire, 0)};
    assert_aliases(held, want, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, bare));
    /* alias -> alias: the record moves */
    odin3_net_id next = net_with_primary("n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, bit0, next));
    assert_aliases(held, NULL, 0);
    assert_aliases(next, want, 1);
    /* alias -> primary of a net without one */
    odin3_net_id last = net_named("l");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, bit0, last));
    assert_aliases(next, NULL, 0);
    assert_primary(last, wire, 0);
    TEST_ASSERT_EQUAL_UINT32(last.v, odin3_wire_net(module, wire, 0).v);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_wire_add_alias_rejects(void) {
    odin3_node_id port = add_port("p", ODIN3_DIR_IN, 1);
    odin3_wire_id port_wire = odin3_module_port_wire(module, 0);
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    odin3_net_id net = net_named("n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_wire_add_alias(module, (odin3_wirebit){port_wire, 0}, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_wire_add_alias(module, (odin3_wirebit){wire, 1}, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_wire_add_alias(module, (odin3_wirebit){{99}, 0}, net));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_wire_add_alias(module, (odin3_wirebit){wire, 0}, (odin3_net_id){99}));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(odin3_pin_net(module, pin_of(port, 0)).v,
                             odin3_wire_net(module, port_wire, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(module, net));
}

static void test_net_delete_refuses_wire_membership(void) {
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    odin3_net_id member = odin3_wire_net(module, wire, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, member));
    TEST_ASSERT_TRUE(odin3_net_live(module, member));
    /* a net whose only membership is an alias */
    odin3_net_id loose = net_named("loose");
    odin3_net_id nets[] = {loose};
    odin3_wire_id primary = wire_range("v", 0, 0, nets); /* primary of loose */
    (void)wire_range("x", 0, 0, nets);                   /* alias of loose */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_wire_add_alias(module, (odin3_wirebit){primary, 0}, member));
    assert_primary(loose, (odin3_wire_id){0}, 0);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_alias_count(module, loose));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, loose));
    TEST_ASSERT_TRUE(odin3_net_live(module, loose));
    TEST_ASSERT_EQUAL_size_t(2, errors_logged);
}

/* --- wire delete -------------------------------------------------------------------------- */

static void assert_full_check_clean(void) {
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(module, full));
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

/*
 * b = {a1, a0, loose, loose} holds only aliases, two of them in the middle of loose's chain
 * (d[0] after them); deleting b releases exactly those, then deleting a clears two primaries.
 */
static void test_wire_delete_releases_memberships(void) {
    odin3_wire_id wire_a = wire_range("a", 1, 0, NULL);
    odin3_net_id a0 = odin3_wire_net(module, wire_a, 0);
    odin3_net_id a1 = odin3_wire_net(module, wire_a, 1);
    odin3_net_id loose = net_named("loose");
    odin3_wire_id wire_c = wire_range("c", 0, 0, &loose); /* primary of loose */
    odin3_net_id nets[] = {a1, a0, loose, loose};
    odin3_wire_id wire_b = wire_range("b", 3, 0, nets);
    odin3_wire_id wire_d = wire_range("d", 0, 0, &loose);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){wire_c, 0}, a0));
    odin3_net_alias before[] = {wb_alias(wire_b, 2), wb_alias(wire_b, 3), wb_alias(wire_d, 0)};
    assert_aliases(loose, before, 3); /* c[0] moved to a0, so b[2] is an alias too */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(module, wire_b));
    TEST_ASSERT_FALSE(odin3_wire_live(module, wire_b));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_wire(module, intern("b")).v);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_wire_width(module, wire_b)); /* history stays readable */
    TEST_ASSERT_EQUAL_UINT32(a1.v, odin3_wire_net(module, wire_b, 0).v);
    assert_aliases(a1, NULL, 0);
    odin3_net_alias c_only[] = {wb_alias(wire_c, 0)};
    assert_aliases(a0, c_only, 1);
    odin3_net_alias d_only[] = {wb_alias(wire_d, 0)};
    assert_aliases(loose, d_only, 1);
    TEST_ASSERT_TRUE(odin3_net_live(module, a0) && odin3_net_live(module, a1) &&
                     odin3_net_live(module, loose)); /* nets stay */
    assert_full_check_clean();
    /* the primary case: deleting a clears both primaries; the name can be reused */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(module, wire_a));
    assert_primary(a0, (odin3_wire_id){0}, 0);
    assert_primary(a1, (odin3_wire_id){0}, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, a1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(module, wire_d));
    assert_aliases(loose, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, loose));
    odin3_wire_id again = wire_range("b", 0, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(again.v, odin3_module_find_wire(module, intern("b")).v);
    assert_full_check_clean();
}

static void test_wire_delete_rejects(void) {
    (void)add_port("p", ODIN3_DIR_IN, 2);
    odin3_wire_id port_wire = odin3_module_port_wire(module, 0);
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(module, wire));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_delete(module, port_wire));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_delete(module, wire)); /* dead */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_delete(module, (odin3_wire_id){99}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_wire_delete(module, (odin3_wire_id){0}));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
    TEST_ASSERT_TRUE(odin3_wire_live(module, port_wire));
    TEST_ASSERT_EQUAL_UINT32(port_wire.v, odin3_module_find_wire(module, intern("p")).v);
    for (uint32_t k = 0; k < 2; k++) {
        assert_primary(odin3_wire_net(module, port_wire, k), port_wire, k);
    }
    errors_logged = 0;
    assert_full_check_clean();
}

/* --- merge --------------------------------------------------------------------------------- */

/* Review Focus 2: drop is a named $port_out sink net; keep is a named, driven net. */
static void test_merge_port_out_drop(void) {
    odin3_node_id port = add_port("y", ODIN3_DIR_OUT, 1);
    odin3_wire_id wire = odin3_module_port_wire(module, 0);
    odin3_net_id drop = odin3_wire_net(module, wire, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, drop, intern("d")));
    odin3_net_id keep = net_named("k");
    odin3_node_id one = node_of("$_CONST1_");
    odin3_node_id buf = node_of("$_BUF_");
    connect(pin_of(one, 0), keep);
    connect(pin_of(buf, 0), keep); /* A sinks keep */
    connect(pin_of(buf, 1), drop); /* Y drives drop */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
    TEST_ASSERT_FALSE(odin3_net_live(module, drop));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, drop).count);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_net_pins(module, keep).count);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_driver_count(module, keep));
    assert_partition(keep);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_pin_net(module, pin_of(port, 0)).v); /* port stays */
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_pin_net(module, pin_of(buf, 1)).v);
    TEST_ASSERT_TRUE(has_alias(keep, name_alias(intern("d"))));
    TEST_ASSERT_TRUE(has_alias(keep, wb_alias(wire, 0)));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_alias_count(module, keep));
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_module_find_net(module, intern("d")).v);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_module_find_net(module, intern("k")).v);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_wire_net(module, wire, 0).v);
    TEST_ASSERT_EQUAL_UINT32(intern("k"), odin3_net_name(module, keep));
    TEST_ASSERT_EQUAL_UINT32(intern("d"), odin3_net_name(module, drop)); /* history */
    assert_primary(drop, (odin3_wire_id){0}, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, keep));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
}

static void assert_wire_holds(odin3_wire_id wire, odin3_net_id net) {
    for (uint32_t k = 0; k < odin3_wire_width(module, wire); k++) {
        TEST_ASSERT_EQUAL_UINT32(net.v, odin3_wire_net(module, wire, k).v);
    }
}

static void test_merge_chains_names(void) {
    odin3_wire_id wire = wire_range("w", 2, 0, NULL);
    odin3_net_id n0 = odin3_wire_net(module, wire, 0);
    odin3_net_id n1 = odin3_wire_net(module, wire, 1);
    odin3_net_id n2 = odin3_wire_net(module, wire, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, n0, intern("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, n1, intern("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, n2, intern("c")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){n1, n0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){n2, n1}));
    TEST_ASSERT_EQUAL_UINT32(n2.v, odin3_module_find_net(module, intern("a")).v);
    TEST_ASSERT_EQUAL_UINT32(n2.v, odin3_module_find_net(module, intern("b")).v);
    TEST_ASSERT_EQUAL_UINT32(n2.v, odin3_module_find_net(module, intern("c")).v);
    assert_wire_holds(wire, n2);
    assert_primary(n2, wire, 2);
    /* n1 got [a, (w,0)]; n2 gets n1's name and primary, then n1's aliases */
    odin3_net_alias want[] = {name_alias(intern("b")), wb_alias(wire, 1), name_alias(intern("a")),
                              wb_alias(wire, 0)};
    assert_aliases(n2, want, 4);
}

/* keep's own aliases first, then drop's name, drop's primary, drop's aliases oldest first. */
static void test_merge_alias_order(void) {
    odin3_wire_id wire_k = wire_range("k", 0, 0, NULL);
    odin3_net_id keep = odin3_wire_net(module, wire_k, 0);
    odin3_net_id keep_nets[] = {keep};
    odin3_wire_id wire_x = wire_range("x", 0, 0, keep_nets);
    odin3_wire_id wire_y = wire_range("y", 0, 0, NULL);
    odin3_net_id drop = odin3_wire_net(module, wire_y, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, drop, intern("d")));
    odin3_net_id drop_nets[] = {drop, drop};
    odin3_wire_id wire_z = wire_range("z", 1, 0, drop_nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
    odin3_net_alias want[] = {wb_alias(wire_x, 0), name_alias(intern("d")), wb_alias(wire_y, 0),
                              wb_alias(wire_z, 0), wb_alias(wire_z, 1)};
    assert_aliases(keep, want, 5);
    assert_primary(keep, wire_k, 0);
}

/* Many merges into one net: each appends in O(1); the names stay in merge order. */
static void test_merge_many_into_one(void) {
    enum { MERGES = 200 };
    odin3_net_id keep = net_named("keep");
    static odin3_net_alias want[MERGES];
    for (uint32_t i = 0; i < MERGES; i++) {
        uint32_t name = numbered_name("m", i);
        odin3_net_id drop = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, name, (odin3_prov_id){0}, &drop));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
        want[i] = name_alias(name);
    }
    assert_aliases(keep, want, MERGES);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_module_find_net(module, numbered_name("m", 0)).v);
}

/* IR-14: alias names count for uniqueness; renaming keep leaves its aliases in the map. */
static void test_alias_names_stay_unique(void) {
    odin3_net_id n0 = net_named("a");
    odin3_net_id n2 = net_named("c");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){n2, n0}));
    odin3_net_id other = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_rename(module, other, intern("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_create(module, intern("a"), (odin3_prov_id){0}, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_rename(module, n2, intern("a")));
    /* renaming keep leaves its aliases in the map */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, n2, intern("z")));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_net(module, intern("c")).v);
    TEST_ASSERT_EQUAL_UINT32(n2.v, odin3_module_find_net(module, intern("a")).v);
    TEST_ASSERT_EQUAL_UINT32(n2.v, odin3_module_find_net(module, intern("z")).v);
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
}

static void test_merge_unnamed_moves_pins(void) {
    odin3_net_id keep = net_named(NULL);
    odin3_net_id drop = net_named(NULL);
    odin3_node_id out = node_of("$_AND_");
    connect(pin_of(out, 0), drop);
    connect(pin_of(out, 1), drop);
    connect(pin_of(out, 2), keep);
    const odin3_net_rec *drop_rec = odin3_net_rec_cat(module, drop);
    const void *block = drop_rec->pins;
    uint8_t cls = drop_rec->cls;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
    TEST_ASSERT_NULL(drop_rec->pins); /* drop's block is back on the pool's free list */
    TEST_ASSERT_EQUAL_PTR(block, module->pinpool.free_heads[cls]);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_net_pins(module, keep).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(module, keep));
    assert_partition(keep);
    /* merging an empty net */
    odin3_net_id empty = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, empty}));
    TEST_ASSERT_FALSE(odin3_net_live(module, empty));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_net_pins(module, keep).count);
}

static void test_merge_rejects(void) {
    odin3_net_id net = net_named("n");
    odin3_net_id dead = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, dead));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_merge(module, (odin3_net_pair){net, net}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_merge(module, (odin3_net_pair){net, dead}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_merge(module, (odin3_net_pair){dead, net}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_merge(module, (odin3_net_pair){net, {99}}));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
    TEST_ASSERT_TRUE(odin3_net_live(module, net));
}

/* --- create-connected ---------------------------------------------------------------------- */

typedef struct quad_nets {
    odin3_net_id a[QUAD_W], b[QUAD_W], y[QUAD_W], c[1];
    odin3_netvec ports[4];
} quad_nets;

static void quad_nets_init(quad_nets *qn) {
    for (uint32_t k = 0; k < QUAD_W; k++) {
        qn->a[k] = net_named(NULL);
        qn->b[k] = net_named(NULL);
        qn->y[k] = net_named(NULL);
    }
    qn->c[0] = net_named(NULL);
    qn->ports[0] = (odin3_netvec){qn->a, QUAD_W};
    qn->ports[1] = (odin3_netvec){qn->b, QUAD_W};
    qn->ports[2] = (odin3_netvec){qn->y, QUAD_W};
    qn->ports[3] = (odin3_netvec){qn->c, 1};
}

static odin3_node_spec quad_spec(uint32_t name) {
    static odin3_value width;
    width = odin3_value_int(QUAD_W);
    return (odin3_node_spec){type_id("test_t4_quad"), name, {0}, &width, 1};
}

static void test_create_connected_wires_all_pins(void) {
    quad_nets qn;
    quad_nets_init(&qn);
    odin3_node_spec spec = quad_spec(intern("q"));
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, qn.ports, &node));
    TEST_ASSERT_EQUAL_UINT32(QUAD_PINS, odin3_node_pins(module, node).count);
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_module_find_node(module, intern("q")).v);
    for (uint32_t port = 0; port < 4; port++) {
        odin3_pinslice slice = odin3_node_port(module, node, port);
        TEST_ASSERT_EQUAL_UINT32(qn.ports[port].count, slice.count);
        for (uint32_t k = 0; k < slice.count; k++) {
            odin3_pin_id pin = {slice.first.v + k};
            TEST_ASSERT_EQUAL_UINT32(qn.ports[port].nets[k].v, odin3_pin_net(module, pin).v);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(pin_of(node, 2 * QUAD_W).v, odin3_net_driver(module, qn.y[0]).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_sinks(module, qn.c[0]).count);
}

static void test_create_connected_open_and_shared(void) {
    odin3_net_id shared = net_named("s");
    odin3_net_id ins[] = {shared, shared, {0}, shared};
    odin3_net_id outs[] = {{0}, {0}, {0}, {0}};
    odin3_net_id carry[] = {shared};
    odin3_netvec ports[] = {{ins, QUAD_W}, {ins, QUAD_W}, {outs, QUAD_W}, {carry, 1}};
    odin3_node_spec spec = quad_spec(0);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    TEST_ASSERT_EQUAL_UINT32(7, odin3_net_pins(module, shared).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_pin_net(module, pin_of(node, 2)).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_pin_net(module, pin_of(node, 2 * QUAD_W)).v);
    /* a type without ports needs no vectors */
    odin3_node_spec one = {type_id("$_CONST1_"), 0, {0}, NULL, 0};
    odin3_net_id out[] = {shared};
    odin3_netvec one_ports[] = {{out, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &one, one_ports, &node));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_pin_node(module, odin3_net_driver(module, shared)).v);
}

static void test_create_connected_rejects(void) {
    quad_nets qn;
    quad_nets_init(&qn);
    odin3_net_id dead = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, dead));
    uint32_t node_end = odin3_module_node_end(module);
    uint32_t pin_end = odin3_module_pin_end(module);
    odin3_node_spec spec = quad_spec(intern("q"));
    odin3_celltype_id quad = spec.type;
    qn.ports[1].count = 3; /* wrong per-port count */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(module, &spec, qn.ports, NULL));
    qn.ports[1].count = QUAD_W;
    qn.b[2] = dead;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(module, &spec, qn.ports, NULL));
    qn.b[2] = (odin3_net_id){999};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(module, &spec, qn.ports, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(module, &spec, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(node_end, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(pin_end, odin3_module_pin_end(module));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_node(module, intern("q")).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, quad));
    for (uint32_t k = 0; k < QUAD_W; k++) {
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, qn.a[k]).count);
    }
}

/* --- replace ------------------------------------------------------------------------------- */

static void test_replace_reattaches_by_port_and_bit(void) {
    odin3_net_id in_a = net_named("a");
    odin3_net_id in_b = net_named("b");
    odin3_net_id out = net_named("y");
    odin3_node_id driver = node_of("$_CONST0_");
    connect(pin_of(driver, 0), in_a);
    odin3_node_id old_node = node_named("$_AND_", intern("g"), NULL);
    connect(pin_of(old_node, 0), in_a);
    connect(pin_of(old_node, 1), in_b);
    connect(pin_of(old_node, 2), out);
    odin3_node_id sink = node_of("$_NOT_");
    connect(pin_of(sink, 0), out);
    odin3_node_id new_node = node_of("$_OR_");
    uint32_t slot = odin3_pin_rec_cat(module, pin_of(old_node, 0))->slot;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_replace(module, (odin3_node_pair){old_node, new_node}));
    TEST_ASSERT_FALSE(odin3_node_live(module, old_node));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_node(module, intern("g")).v);
    TEST_ASSERT_EQUAL_UINT32(in_a.v, odin3_pin_net(module, pin_of(new_node, 0)).v);
    TEST_ASSERT_EQUAL_UINT32(in_b.v, odin3_pin_net(module, pin_of(new_node, 1)).v);
    TEST_ASSERT_EQUAL_UINT32(out.v, odin3_pin_net(module, pin_of(new_node, 2)).v);
    TEST_ASSERT_EQUAL_UINT32(slot, odin3_pin_rec_cat(module, pin_of(new_node, 0))->slot);
    TEST_ASSERT_EQUAL_UINT32(pin_of(new_node, 2).v, odin3_net_driver(module, out).v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_pins(module, in_a).count);
    assert_partition(in_a);
    assert_partition(out);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, type_id("$_AND_")));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, type_id("$_OR_")));
}

static void test_replace_rejects(void) {
    odin3_node_id and_node = node_of("$_AND_");
    odin3_node_id or_node = node_of("$_OR_");
    odin3_node_id not_node = node_of("$_NOT_");
    odin3_node_id port = add_port("p", ODIN3_DIR_IN, 1);
    odin3_node_id other_port = add_port("q", ODIN3_DIR_IN, 1);
    odin3_node_id driver = node_of("$_CONST0_"); /* one OUT pin, like a $port_in */
    odin3_net_id net = net_named("n");
    connect(pin_of(or_node, 0), net);
    odin3_node_pair bad[] = {
        {and_node, not_node}, /* different signature */
        {and_node, or_node},  /* new node has a connected pin */
        {and_node, and_node}, /* same node */
        {and_node, {99}},     /* not a node */
        {port, other_port},   /* port nodes */
        {port, driver},       /* port by non-port, same pins */
        {driver, port},       /* non-port by port */
    };
    size_t count = sizeof bad / sizeof bad[0];
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_replace(module, bad[i]));
    }
    TEST_ASSERT_EQUAL_size_t(count, errors_logged);
    TEST_ASSERT_TRUE(odin3_node_live(module, and_node));
    TEST_ASSERT_TRUE(odin3_node_live(module, port));
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(module, pin_of(or_node, 0)).v);
}

/* Port nodes exist only through module_add_port and are never deleted (IR-7). */
static void test_port_nodes_only_via_add_port(void) {
    odin3_node_id port = add_port("p", ODIN3_DIR_OUT, 1);
    odin3_value one = odin3_value_int(1);
    odin3_node_spec in_spec = {type_id("$port_in"), 0, {0}, &one, 1};
    odin3_node_spec out_spec = {type_id("$port_out"), 0, {0}, &one, 1};
    odin3_net_id nets[] = {net_named("n")};
    odin3_netvec ports[] = {{nets, 1}};
    uint32_t node_end = odin3_module_node_end(module);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(module, &in_spec, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(module, &out_spec, ports, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_delete(module, port));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(node_end, odin3_module_node_end(module));
    TEST_ASSERT_TRUE(odin3_node_live(module, port));
    TEST_ASSERT_EQUAL_UINT32(port.v, odin3_module_port(module, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, nets[0]).count);
}

/* --- attributes ---------------------------------------------------------------------------- */

static void test_attr_set_get_overwrite(void) {
    odin3_node_id node = node_of("$_AND_");
    odin3_net_id net = net_named("n");
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    odin3_objref node_ref = {ODIN3_OBJ_NODE, node.v};
    odin3_objref net_ref = {ODIN3_OBJ_NET, net.v};
    uint32_t key = intern("keep");
    uint32_t other = intern("src");
    odin3_value one = odin3_value_int(1);
    odin3_value two = odin3_value_int(2);
    TEST_ASSERT_NULL(odin3_attr_get(module, node_ref, key));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, node_ref, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, net_ref, key, &two));
    TEST_ASSERT_EQUAL_INT64(1, odin3_attr_get(module, node_ref, key)->i);
    TEST_ASSERT_EQUAL_INT64(2, odin3_attr_get(module, net_ref, key)->i);
    TEST_ASSERT_NULL(odin3_attr_get(module, node_ref, other));
    const odin3_value *before = odin3_attr_get(module, node_ref, key);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, node_ref, key, &two)); /* overwrite */
    TEST_ASSERT_EQUAL_INT64(2, odin3_attr_get(module, node_ref, key)->i);
    TEST_ASSERT_EQUAL_INT64(1, before->i); /* an earlier pointer keeps its value */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, node_ref, other, &one));
    TEST_ASSERT_EQUAL_INT64(2, odin3_attr_get(module, node_ref, key)->i);
    TEST_ASSERT_EQUAL_INT64(1, odin3_attr_get(module, node_ref, other)->i);
    odin3_objref wire_ref = {ODIN3_OBJ_WIRE, wire.v};
    odin3_objref mod_ref = {ODIN3_OBJ_MODULE, odin3_module_id_of(module).v};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, wire_ref, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, mod_ref, key, &two));
    TEST_ASSERT_EQUAL_INT64(1, odin3_attr_get(module, wire_ref, key)->i);
    TEST_ASSERT_EQUAL_INT64(2, odin3_attr_get(module, mod_ref, key)->i);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_attr_payload_copied(void) {
    odin3_net_id net = net_named("n");
    odin3_objref ref = {ODIN3_OBJ_NET, net.v};
    uint8_t bits[] = {ODIN3_BIT_1, ODIN3_BIT_X};
    odin3_value val = {ODIN3_VAL_BITS, 0, bits, 2, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, ref, intern("init"), &val));
    bits[0] = ODIN3_BIT_0;
    const odin3_value *got = odin3_attr_get(module, ref, intern("init"));
    TEST_ASSERT_EQUAL_UINT32(2, got->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, got->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, got->bits[1]);
}

static void test_attr_rejects(void) {
    odin3_node_id node = node_of("$_AND_");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, node));
    odin3_value one = odin3_value_int(1);
    odin3_value no_payload = {ODIN3_VAL_BITS, 0, NULL, 3, 0, 0};
    odin3_value bad_kind = {(odin3_value_kind)9, 0, NULL, 0, 0, 0};
    static const uint8_t k_rows[3] = {'1', '1', '0'};
    odin3_value ragged = {ODIN3_VAL_COVER, 0, k_rows, 3, 0, 1}; /* rows of 2 bytes */
    uint32_t key = intern("k");
    odin3_net_id net = net_named("n");
    odin3_objref net_ref = {ODIN3_OBJ_NET, net.v};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_WIRE, 5}, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_MODULE, 99}, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_attr_set(module, (odin3_objref){(odin3_objkind)9, 1}, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, 0, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, UINT32_MAX, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, key, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, key, &no_payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, key, &bad_kind));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, net_ref, key, &ragged));
    TEST_ASSERT_EQUAL_size_t(10, errors_logged);
    TEST_ASSERT_NULL(odin3_attr_get(module, net_ref, key));
    TEST_ASSERT_NULL(odin3_attr_get(module, (odin3_objref){(odin3_objkind)9, 1}, key));
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Observable state: store ends, port count, the first SNAP_MAX nets, a name in every map. */
typedef struct snapshot {
    uint32_t node_end, pin_end, net_end, wire_end, ports, type_ports;
    uint32_t net_count[SNAP_MAX];
    uint32_t net_drivers[SNAP_MAX];
    uint32_t net_aliases[SNAP_MAX];
    uint32_t net_live[SNAP_MAX];
    uint32_t pin_net[SNAP_MAX];
    uint32_t found_node, found_net, found_wire;
} snapshot;

static uint32_t min_u32(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

static snapshot take_snapshot(uint32_t name) {
    snapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.node_end = odin3_module_node_end(module);
    snap.pin_end = odin3_module_pin_end(module);
    snap.net_end = odin3_module_net_end(module);
    snap.wire_end = odin3_module_wire_end(module);
    snap.ports = odin3_module_port_count(module);
    snap.type_ports = odin3_celltype_get(design, odin3_module_celltype(module))->n_ports;
    for (uint32_t i = 1; i < min_u32(snap.net_end, SNAP_MAX); i++) {
        odin3_net_id net = {i};
        snap.net_count[i] = odin3_net_pins(module, net).count;
        snap.net_drivers[i] = odin3_net_driver_count(module, net);
        snap.net_aliases[i] = odin3_net_alias_count(module, net);
        snap.net_live[i] = odin3_net_live(module, net);
    }
    for (uint32_t i = 1; i < min_u32(snap.pin_end, SNAP_MAX); i++) {
        snap.pin_net[i] = odin3_pin_net(module, (odin3_pin_id){i}).v;
    }
    snap.found_node = odin3_module_find_node(module, name).v;
    snap.found_net = odin3_module_find_net(module, name).v;
    snap.found_wire = odin3_module_find_wire(module, name).v;
    return snap;
}

static void assert_same(const snapshot *want, const snapshot *have) {
    TEST_ASSERT_EQUAL_MEMORY(want, have, sizeof *want);
}

/* Makes the arena's current chunk too small for `need` bytes. */
static void exhaust_arena(odin3_arena *arena, size_t need) {
    while (odin3_arena_bytes_reserved(arena) - odin3_arena_bytes_used(arena) >= need) {
        TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, 1));
    }
}

/* Fills the node and pin stores to a page boundary with one-pin nodes. */
static void fill_nodes(void) {
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    while (odin3_module_node_end(module) < page) {
        (void)node_of("$_CONST0_");
    }
    TEST_ASSERT_EQUAL_UINT32(page, odin3_module_pin_end(module));
}

/* Fills the net store to a page boundary. */
static void fill_nets(void) {
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    while (odin3_module_net_end(module) < page) {
        (void)net_named(NULL);
    }
}

/* Fills the wire store to a page boundary and its name map to the growth threshold. */
static void fill_wires(odin3_net_id net) {
    odin3_net_id nets[] = {net};
    for (uint32_t i = 1; odin3_module_wire_end(module) < WIRE_PAGE; i++) {
        odin3_wire_spec spec = {i <= NAME_MAP_FULL ? numbered_name("fw", i) : 0, 0, 0, false, {0}};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, nets, NULL));
    }
}

/*
 * Fresh module where one more 2-bit port must allocate at nine points: the port and port-def
 * vectors (first port), the node, pin, net and wire stores (page boundaries), the module arena,
 * the pin pool and the wire name map (growth threshold).
 */
static odin3_port_spec prepare_add_port_oom(void) {
    fresh_design();
    fill_nets();
    fill_wires((odin3_net_id){1});
    fill_nodes();
    exhaust_arena(module->arena, sizeof(odin3_value));
    exhaust_arena(module->pinpool.arena, 2 * sizeof(odin3_pin_id));
    odin3_port_spec spec = {intern("oom_port"), ODIN3_DIR_OUT, 2, false, {0}};
    return spec;
}

static void test_add_port_oom_sweep(void) {
    odin3_port_spec spec = {0};
    snapshot before;
    odin3_node_id node = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        spec = prepare_add_port_oom();
        before = take_snapshot(spec.name);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_module_add_port(module, &spec, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        snapshot now = take_snapshot(spec.name);
        assert_same(&before, &now);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, type_id("$port_out")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(ADD_PORT_ALLOCS, failures);
    TEST_ASSERT_EQUAL_UINT32(before.node_end, node.v);
    odin3_wire_id wire = odin3_module_find_wire(module, spec.name);
    TEST_ASSERT_EQUAL_UINT32(before.wire_end, wire.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_port_count(module));
    assert_port_bits(node, wire, 2);
}

/* Fresh module where a named 1-bit wire with new nets must allocate at four points. */
static uint32_t prepare_wire_oom(void) {
    fresh_design();
    fill_nets();
    fill_wires((odin3_net_id){1});
    exhaust_arena(module->arena, sizeof(odin3_net_id));
    return intern("oom_wire");
}

static void test_wire_create_oom_sweep(void) {
    uint32_t name = 0;
    snapshot before;
    odin3_wire_id wire = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        name = prepare_wire_oom();
        before = take_snapshot(name);
        odin3_wire_spec spec = {name, 0, 0, false, {0}};
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_wire_create(module, &spec, NULL, &wire);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        snapshot now = take_snapshot(name);
        assert_same(&before, &now);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(WIRE_CREATE_ALLOCS, failures);
    TEST_ASSERT_EQUAL_UINT32(wire.v, odin3_module_find_wire(module, name).v);
    assert_primary(odin3_wire_net(module, wire, 0), wire, 0);
}

/* Fresh module: keep (named, 2 pins) and drop (named, primary of a wire, 1 pin); merging them
 * needs a bigger pin block for keep (pool exhausted) and the first alias records. */
static odin3_net_pair prepare_merge_oom(void) {
    fresh_design();
    odin3_net_id keep = net_named("k");
    odin3_wire_id wire = wire_range("w", 0, 0, NULL);
    odin3_net_id drop = odin3_wire_net(module, wire, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, drop, intern("d")));
    odin3_node_id gate = node_of("$_AND_");
    connect(pin_of(gate, 0), keep);
    connect(pin_of(gate, 1), keep);
    connect(pin_of(gate, 2), drop);
    exhaust_arena(module->pinpool.arena, 4 * sizeof(odin3_pin_id));
    return (odin3_net_pair){keep, drop};
}

static void test_merge_oom_sweep(void) {
    odin3_net_pair pair = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        pair = prepare_merge_oom();
        snapshot before = take_snapshot(intern("d"));
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_net_merge(module, pair);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        snapshot now = take_snapshot(intern("d"));
        assert_same(&before, &now);
        assert_partition(pair.keep);
        assert_partition(pair.drop);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(MERGE_ALLOCS, failures);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_net_pins(module, pair.keep).count);
    TEST_ASSERT_EQUAL_UINT32(pair.keep.v, odin3_module_find_net(module, intern("d")).v);
    assert_partition(pair.keep);
}

/*
 * Fresh module where a named quad node created connected must allocate at five points: the arena
 * (parameters), the node and pin stores (page boundaries), the node name map (growth threshold)
 * and, at its 9th pin (Y[0], after eight connections to undo), a pin block from an exhausted pool.
 */
static odin3_node_spec prepare_connected_oom(quad_nets *qn) {
    fresh_design();
    quad_nets_init(qn);
    odin3_value width = odin3_value_int((int64_t)2 * QUAD_W);
    odin3_node_id spare = node_named("test_t4_sink", 0, &width);
    for (uint32_t k = 0; k < QUAD_W; k++) { /* A and B nets: one pin, room for one more */
        connect(pin_of(spare, k), qn->a[k]);
        connect(pin_of(spare, QUAD_W + k), qn->b[k]);
    }
    /* a filler node so that one-pin nodes fill the node store and pin store to page ends */
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    uint32_t singles = page - odin3_module_node_end(module) - 1;
    width = odin3_value_int(2 * page - odin3_module_pin_end(module) - singles);
    (void)node_named("test_t4_sink", 0, &width);
    for (uint32_t i = 1; i <= singles; i++) {
        (void)node_named("$_CONST0_", i <= NAME_MAP_FULL ? numbered_name("g", i) : 0, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(page, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(2 * page, odin3_module_pin_end(module));
    exhaust_arena(module->arena, sizeof(odin3_value));
    exhaust_arena(module->pinpool.arena, 2 * sizeof(odin3_pin_id));
    return quad_spec(intern("oom_q"));
}

static void test_create_connected_oom_sweep(void) {
    quad_nets qn;
    odin3_node_spec spec = {0};
    snapshot before;
    odin3_node_id node = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        spec = prepare_connected_oom(&qn);
        before = take_snapshot(spec.name);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_node_create_connected(module, &spec, qn.ports, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        snapshot now = take_snapshot(spec.name);
        assert_same(&before, &now);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, spec.type));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(CONNECTED_ALLOCS, failures);
    TEST_ASSERT_EQUAL_UINT32(before.node_end, node.v);
    TEST_ASSERT_EQUAL_UINT32(qn.y[3].v, odin3_pin_net(module, pin_of(node, 3 * QUAD_W - 1)).v);
}

/* Fresh module where a first attribute on a net must allocate at three points. */
static odin3_net_id prepare_attr_oom(void) {
    fresh_design();
    odin3_net_id net = net_named("n");
    odin3_value val = odin3_value_int(1);
    for (uint32_t i = 1; i <= NAME_MAP_FULL; i++) { /* attribute map at its growth threshold */
        odin3_node_id node = node_of("$_CONST0_");
        odin3_objref ref = {ODIN3_OBJ_NODE, node.v};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, ref, intern("a"), &val));
    }
    while (module->attrs.len < module->attrs.cap) { /* attribute table full */
        odin3_objref ref = {ODIN3_OBJ_NODE, 1};
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_attr_set(module, ref, numbered_name("x", (uint32_t)module->attrs.len), &val));
    }
    exhaust_arena(module->arena, sizeof(odin3_value));
    return net;
}

/* The attribute table's observable state: map size, record count, node 1's attribute "a". */
typedef struct attr_snapshot {
    size_t heads;
    size_t records;
    const odin3_value *node_attr;
    const odin3_value *net_attr;
} attr_snapshot;

static attr_snapshot take_attr_snapshot(odin3_objref net_ref, uint32_t key) {
    attr_snapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.heads = odin3_u64map_count(module->attr_heads);
    snap.records = module->attrs.len;
    snap.node_attr = odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_NODE, 1}, intern("a"));
    snap.net_attr = odin3_attr_get(module, net_ref, key);
    return snap;
}

static void test_attr_set_oom_sweep(void) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    odin3_objref ref = {ODIN3_OBJ_NET, 0};
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        ref.id = prepare_attr_oom().v;
        uint32_t key = intern("oom_key");
        odin3_value val = odin3_value_int(7);
        attr_snapshot before = take_attr_snapshot(ref, key);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_attr_set(module, ref, key, &val);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        attr_snapshot now = take_attr_snapshot(ref, key);
        TEST_ASSERT_EQUAL_MEMORY(&before, &now, sizeof before);
        TEST_ASSERT_NULL(now.net_attr);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(ATTR_ALLOCS, failures);
    TEST_ASSERT_EQUAL_INT64(7, odin3_attr_get(module, ref, intern("oom_key"))->i);
}

static void test_wire_add_alias_oom_sweep(void) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    odin3_wire_id wire = {0};
    odin3_net_id net = {0};
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        fresh_design();
        wire = wire_range("w", 0, 0, NULL);
        net = net_with_primary("v"); /* so wb becomes an alias: a new record */
        snapshot before = take_snapshot(0);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_wire_add_alias(module, (odin3_wirebit){wire, 0}, net); /* first alias record */
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        snapshot now = take_snapshot(0);
        assert_same(&before, &now);
        assert_primary(odin3_wire_net(module, wire, 0), wire, 0);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(1, failures);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_wire_net(module, wire, 0).v);
}

int main(void) {
    if (odin3_celltype_register_global(&k_quad) != ODIN3_OK ||
        odin3_celltype_register_global(&k_sink) != ODIN3_OK) {
        return EXIT_FAILURE;
    }
    UNITY_BEGIN();
    RUN_TEST(test_add_port_creates_node_wire_and_type_port);
    RUN_TEST(test_add_port_inout);
    RUN_TEST(test_instance_pins_follow_ports);
    RUN_TEST(test_add_port_refused_once_instantiated);
    RUN_TEST(test_add_port_rejects_bad_specs);
    RUN_TEST(test_small_module_stays_small);
    RUN_TEST(test_add_port_memory_linear);
    RUN_TEST(test_wire_downto_creates_nets);
    RUN_TEST(test_wire_upto_bit0_is_lsb);
    RUN_TEST(test_wire_over_existing_nets);
    RUN_TEST(test_wire_create_rejects);
    RUN_TEST(test_wire_add_alias_rebinds);
    RUN_TEST(test_wire_add_alias_rejects);
    RUN_TEST(test_net_delete_refuses_wire_membership);
    RUN_TEST(test_wire_delete_releases_memberships);
    RUN_TEST(test_wire_delete_rejects);
    RUN_TEST(test_merge_port_out_drop);
    RUN_TEST(test_merge_chains_names);
    RUN_TEST(test_merge_alias_order);
    RUN_TEST(test_merge_many_into_one);
    RUN_TEST(test_alias_names_stay_unique);
    RUN_TEST(test_merge_unnamed_moves_pins);
    RUN_TEST(test_merge_rejects);
    RUN_TEST(test_create_connected_wires_all_pins);
    RUN_TEST(test_create_connected_open_and_shared);
    RUN_TEST(test_create_connected_rejects);
    RUN_TEST(test_replace_reattaches_by_port_and_bit);
    RUN_TEST(test_replace_rejects);
    RUN_TEST(test_port_nodes_only_via_add_port);
    RUN_TEST(test_attr_set_get_overwrite);
    RUN_TEST(test_attr_payload_copied);
    RUN_TEST(test_attr_rejects);
    RUN_TEST(test_add_port_oom_sweep);
    RUN_TEST(test_wire_create_oom_sweep);
    RUN_TEST(test_merge_oom_sweep);
    RUN_TEST(test_create_connected_oom_sweep);
    RUN_TEST(test_attr_set_oom_sweep);
    RUN_TEST(test_wire_add_alias_oom_sweep);
    return UNITY_END();
}
