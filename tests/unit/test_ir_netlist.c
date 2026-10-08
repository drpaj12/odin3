/*
 * test_ir_netlist.c — unit tests for modules, nodes, pins and nets (IR §3, IR-14..IR-16).
 */
#include "ir/celltype.h"
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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    MANY_SINKS = 1000,
    FEW_DRIVERS = 10,
    CYCLES = 10000,
    GROW_PINS = 100,
    GROW_ROUNDS = 50,
    SNAP_MAX = 64,
    MIXED_A = 2,
    MIXED_Y = 3,
    OOM_LIMIT = 10000,
    SMALL_MODULE_BYTES = 64 * 1024,
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

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(name), &id));
    return id;
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("top"), (odin3_prov_id){0}, &mid));
    module = odin3_module_get(design, mid);
    TEST_ASSERT_NOT_NULL(module);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
}

/* A test type with mixed ports: A in (A_WIDTH), B in (width 0), Y out (3), T inout (1). */
static const odin3_param_def k_mixed_params[] = {
    {"A_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, MIXED_A, NULL, 0, 0, 0}},
};
static const odin3_port_def k_mixed_ports[] = {
    {"A", ODIN3_DIR_IN, false, 0, "A_WIDTH", NULL},
    {"B", ODIN3_DIR_IN, false, 0, NULL, NULL},
    {"Y", ODIN3_DIR_OUT, false, MIXED_Y, NULL, NULL},
    {"T", ODIN3_DIR_INOUT, true, 1, NULL, NULL},
};
static const odin3_celltype_def k_mixed = {
    "test_t3_mixed", ODIN3_GRAN_WORD, 0, k_mixed_ports, 4, k_mixed_params, 1, NULL, NULL};

static odin3_node_id node_named(const char *type, uint32_t name, const odin3_value *params) {
    odin3_node_spec spec = {type_id(type), name, {0}, params, params != NULL ? 1 : 0};
    odin3_node_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &id));
    TEST_ASSERT_TRUE(odin3_node_live(module, id));
    return id;
}

static odin3_node_id node_of(const char *type) {
    return node_named(type, 0, NULL);
}

static odin3_node_id port_node(const char *type, int64_t width) {
    odin3_value param = odin3_value_int(width);
    return node_named(type, 0, &param);
}

static odin3_net_id net_named(const char *name) {
    odin3_net_id id = {0};
    uint32_t str = name != NULL ? intern(name) : 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, str, (odin3_prov_id){0}, &id));
    TEST_ASSERT_TRUE(odin3_net_live(module, id));
    return id;
}

static odin3_pin_id pin_of(odin3_node_id node, uint32_t index) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    TEST_ASSERT_TRUE(index < pins.count);
    return (odin3_pin_id){pins.first.v + index};
}

static void connect(odin3_pin_id pin, odin3_net_id net) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(module, pin, net));
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(module, pin).v);
}

/* Partition invariant and pin <-> net agreement for one net. */
static void assert_partition(odin3_net_id net) {
    odin3_pinlist all = odin3_net_pins(module, net);
    uint32_t drivers = odin3_net_driver_count(module, net);
    TEST_ASSERT_TRUE(drivers <= all.count);
    for (uint32_t i = 0; i < all.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(module, all.pins[i]).v);
        TEST_ASSERT_EQUAL(i < drivers, odin3_pin_drives(module, all.pins[i]));
    }
    odin3_pinlist sinks = odin3_net_sinks(module, net);
    TEST_ASSERT_EQUAL_UINT32(all.count - drivers, sinks.count);
    if (sinks.count > 0) {
        TEST_ASSERT_EQUAL_PTR(all.pins + drivers, sinks.pins);
    }
    odin3_pin_id first = odin3_net_driver(module, net);
    TEST_ASSERT_EQUAL_UINT32(drivers > 0 ? all.pins[0].v : 0, first.v);
}

static bool net_has(odin3_net_id net, odin3_pin_id pin) {
    odin3_pinlist all = odin3_net_pins(module, net);
    for (uint32_t i = 0; i < all.count; i++) {
        if (all.pins[i].v == pin.v) {
            return true;
        }
    }
    return false;
}

/* --- modules ------------------------------------------------------------------------------- */

static void test_module_create_registers_type(void) {
    odin3_celltype_id type = odin3_module_type(module);
    TEST_ASSERT_EQUAL_UINT32(type_id("top").v, type.v);
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_MODULE, def->gran);
    TEST_ASSERT_EQUAL_UINT32(0, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("top", def->name);
    TEST_ASSERT_EQUAL_PTR(design, odin3_module_design(module));
    TEST_ASSERT_EQUAL_UINT32(intern("top"), odin3_module_name(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_id_of(module).v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_NULL(odin3_module_get(design, (odin3_module_id){0}));
    TEST_ASSERT_NULL(odin3_module_get(design, (odin3_module_id){2}));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_pin_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_net_end(module));
}

static void test_module_create_rejects_bad_names(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_module_create(design, intern("top"), (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_create(design, intern("$port_in"),
                                                                     (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_module_create(design, 0, (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_module_create(design, UINT32_MAX, (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_TRUE(errors_logged >= 4);
}

static void test_module_instance_counts(void) {
    odin3_module_id sub = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("sub"), (odin3_prov_id){0}, &sub));
    odin3_celltype_id sub_type = odin3_module_type(odin3_module_get(design, sub));
    odin3_node_id inst = node_of("sub");
    TEST_ASSERT_EQUAL_UINT32(sub_type.v, odin3_node_type(module, inst).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_pins(module, inst).count);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, sub_type));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, inst));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, sub_type));
}

/* --- nodes and pins ------------------------------------------------------------------------ */

static void assert_slice(odin3_pinslice have, odin3_pinslice want) {
    TEST_ASSERT_EQUAL_UINT32(want.count, have.count);
    TEST_ASSERT_EQUAL_UINT32(want.first.v, have.first.v);
}

/* A test_t3_mixed node named u1 with A_WIDTH 4: pins A0..A3, Y0..Y2, T0. */
static odin3_node_id mixed_node(void) {
    odin3_value width = odin3_value_int(4);
    return node_named("test_t3_mixed", intern("u1"), &width);
}

static void test_node_port_slices(void) {
    odin3_node_id mixed = mixed_node();
    odin3_pinslice pins = odin3_node_pins(module, mixed);
    uint32_t first = pins.first.v;
    TEST_ASSERT_EQUAL_UINT32(4 + 0 + MIXED_Y + 1, pins.count);
    assert_slice(odin3_node_port(module, mixed, 0), (odin3_pinslice){{first}, 4});
    assert_slice(odin3_node_port(module, mixed, 1), (odin3_pinslice){{0}, 0});
    assert_slice(odin3_node_port(module, mixed, 2), (odin3_pinslice){{first + 4}, MIXED_Y});
    assert_slice(odin3_node_port(module, mixed, 3), (odin3_pinslice){{first + 4 + MIXED_Y}, 1});
    assert_slice(odin3_node_port(module, mixed, 4), (odin3_pinslice){{0}, 0});
    assert_slice(odin3_node_port(module, mixed, UINT32_MAX), (odin3_pinslice){{0}, 0});
}

/* Direction, and the drives/reads predicates that follow from it. */
static void assert_pin_dir(odin3_pin_id pin, odin3_dir dir) {
    TEST_ASSERT_EQUAL_INT(dir, odin3_pin_dir(module, pin));
    TEST_ASSERT_EQUAL(dir != ODIN3_DIR_IN, odin3_pin_drives(module, pin));
    TEST_ASSERT_EQUAL(dir != ODIN3_DIR_OUT, odin3_pin_reads(module, pin));
}

static void assert_y_pin(odin3_node_id node, odin3_pin_id pin, uint32_t bit) {
    TEST_ASSERT_TRUE(odin3_pin_live(module, pin));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_pin_node(module, pin).v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_pin_port(module, pin));
    TEST_ASSERT_EQUAL_UINT32(bit, odin3_pin_bit(module, pin));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_pin_net(module, pin).v);
    assert_pin_dir(pin, ODIN3_DIR_OUT);
}

static void test_node_pin_fields(void) {
    odin3_node_id mixed = mixed_node();
    odin3_pinslice port_y = odin3_node_port(module, mixed, 2);
    for (uint32_t k = 0; k < MIXED_Y; k++) {
        assert_y_pin(mixed, (odin3_pin_id){port_y.first.v + k}, k);
    }
    assert_pin_dir(odin3_node_port(module, mixed, 0).first, ODIN3_DIR_IN);
    assert_pin_dir(odin3_node_port(module, mixed, 3).first, ODIN3_DIR_INOUT);
}

static void test_node_param_and_name(void) {
    odin3_node_id mixed = mixed_node();
    const odin3_value *param = odin3_node_param(module, mixed, 0);
    TEST_ASSERT_NOT_NULL(param);
    TEST_ASSERT_EQUAL_INT64(4, param->i);
    TEST_ASSERT_NULL(odin3_node_param(module, mixed, 1));
    TEST_ASSERT_EQUAL_UINT32(type_id("test_t3_mixed").v, odin3_node_type(module, mixed).v);
    TEST_ASSERT_EQUAL_UINT32(intern("u1"), odin3_node_name(module, mixed));
    TEST_ASSERT_EQUAL_UINT32(mixed.v, odin3_module_find_node(module, intern("u1")).v);
}

static void test_node_default_params_and_prov(void) {
    odin3_node_spec spec = {type_id("test_t3_mixed"), 0, {7}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_INT64(MIXED_A, odin3_node_param(module, node, 0)->i);
    TEST_ASSERT_EQUAL_UINT32(MIXED_A + MIXED_Y + 1, odin3_node_pins(module, node).count);
    TEST_ASSERT_EQUAL_UINT32(7, odin3_node_prov(module, node).v);
    TEST_ASSERT_EQUAL_UINT32(7, odin3_pin_prov(module, pin_of(node, 1)).v);
}

static void test_node_create_rejects_bad_specs(void) {
    odin3_node_id node = {0};
    odin3_value wrong_kind = {0};
    wrong_kind.kind = ODIN3_VAL_STRING;
    odin3_value negative = odin3_value_int(-1);
    odin3_value zero = odin3_value_int(0);
    odin3_node_spec bad_type = {{UINT32_MAX}, 0, {0}, NULL, 0};
    odin3_node_spec no_type = {{0}, 0, {0}, NULL, 0};
    odin3_node_spec bad_count = {type_id("test_t3_mixed"), 0, {0}, &zero, 2};
    odin3_node_spec bad_kind = {type_id("test_t3_mixed"), 0, {0}, &wrong_kind, 1};
    odin3_node_spec bad_width = {type_id("test_t3_mixed"), 0, {0}, &negative, 1};
    odin3_node_spec verify_fails = {type_id("$port_in"), 0, {0}, &zero, 1};
    odin3_node_spec bad_name = {type_id("$_CONST0_"), UINT32_MAX, {0}, NULL, 0};
    const odin3_node_spec *specs[] = {&bad_type,  &no_type,      &bad_count, &bad_kind,
                                      &bad_width, &verify_fails, &bad_name};
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(module, specs[i], &node));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(module, NULL, &node));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_pin_end(module));
    TEST_ASSERT_TRUE(errors_logged >= sizeof specs / sizeof specs[0]);
}

static void test_node_names_unique(void) {
    uint32_t name = intern("g1");
    (void)node_named("$_CONST0_", name, NULL);
    odin3_node_spec dup = {type_id("$_CONST1_"), name, {0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(module, &dup, &node));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_node_end(module));
    /* A net may share a node's name (names are unique per kind). */
    odin3_net_id net = net_named("g1");
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_module_find_net(module, name).v);
}

static void test_node_rename(void) {
    odin3_node_id first = node_named("$_CONST0_", intern("a"), NULL);
    (void)node_named("$_CONST1_", intern("b"), NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_rename(module, first, intern("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_rename(module, first, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(intern("a"), odin3_node_name(module, first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(module, first, intern("c")));
    TEST_ASSERT_FALSE(odin3_node_valid(odin3_module_find_node(module, intern("a"))));
    TEST_ASSERT_EQUAL_UINT32(first.v, odin3_module_find_node(module, intern("c")).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(module, first, intern("c"))); /* same */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(module, first, 0));
    TEST_ASSERT_FALSE(odin3_node_valid(odin3_module_find_node(module, intern("c"))));
    TEST_ASSERT_FALSE(odin3_node_valid(odin3_module_find_node(module, 0)));
}

static void test_node_delete_frees_name(void) {
    odin3_node_id first = node_named("$_CONST0_", intern("a"), NULL);
    odin3_node_id second = node_named("$_CONST1_", intern("b"), NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, second));
    TEST_ASSERT_FALSE(odin3_node_valid(odin3_module_find_node(module, intern("b"))));
    TEST_ASSERT_EQUAL_UINT32(intern("b"), odin3_node_name(module, second)); /* tombstone data */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(module, first, intern("b")));
    TEST_ASSERT_EQUAL_UINT32(first.v, odin3_module_find_node(module, intern("b")).v);
}

static void test_node_delete_counts_instances(void) {
    odin3_celltype_id const0 = type_id("$_CONST0_");
    uint32_t before = odin3_celltype_instances(design, const0);
    odin3_node_id node = node_of("$_CONST0_");
    TEST_ASSERT_EQUAL_UINT32(before + 1, odin3_celltype_instances(design, const0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, node));
    TEST_ASSERT_EQUAL_UINT32(before, odin3_celltype_instances(design, const0));
    TEST_ASSERT_FALSE(odin3_node_live(module, node));
    TEST_ASSERT_FALSE(odin3_pin_live(module, pin_of(node, 0)));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_node_end(module)); /* IDs never reused */
    odin3_node_id next = node_of("$_CONST0_");
    TEST_ASSERT_EQUAL_UINT32(node.v + 1, next.v);
}

/* --- connectivity -------------------------------------------------------------------------- */

static void test_const_driver_and_sink(void) {
    odin3_node_id one = node_of("$_CONST1_");
    odin3_node_id out = port_node("$port_out", 1);
    odin3_net_id net = net_named("n1");
    odin3_pin_id driver = pin_of(one, 0);
    odin3_pin_id sink = pin_of(out, 0);
    connect(sink, net); /* sink first: the driver must still land at index 0 */
    connect(driver, net);
    TEST_ASSERT_EQUAL_UINT32(driver.v, odin3_net_driver(module, net).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(module, net));
    odin3_pinlist sinks = odin3_net_sinks(module, net);
    TEST_ASSERT_EQUAL_UINT32(1, sinks.count);
    TEST_ASSERT_EQUAL_UINT32(sink.v, sinks.pins[0].v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(module, net));
    assert_partition(net);

    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, driver));
    TEST_ASSERT_FALSE(odin3_pin_valid(odin3_net_driver(module, net)));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(module, net));
    assert_partition(net);
    connect(driver, net);
    assert_partition(net);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, sink));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(module, net).count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, sink)); /* already: no-op */
    connect(sink, net);
    connect(sink, net); /* same net: no-op */
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_pins(module, net).count);
    assert_partition(net);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(module, net));
}

static void test_const_value_needs_one_driver(void) {
    odin3_net_id net = net_named(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(module, net));
    connect(pin_of(port_node("$port_in", 1), 0), net);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(module, net));
    connect(pin_of(node_of("$_CONSTZ_"), 0), net);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_driver_count(module, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(module, net));
}

/* The net holds all `count` pins of node out as sinks, and the partition is sound. */
static void assert_sinks_intact(odin3_net_id net, odin3_node_id out, uint32_t count) {
    TEST_ASSERT_EQUAL_UINT32(count, odin3_net_sinks(module, net).count);
    for (uint32_t k = 0; k < count; k++) {
        TEST_ASSERT_TRUE(net_has(net, pin_of(out, k)));
    }
    assert_partition(net);
}

static void test_delete_only_driver_keeps_sinks(void) {
    odin3_node_id zero = node_of("$_CONST0_");
    odin3_node_id out = port_node("$port_out", 3);
    odin3_net_id net = net_named("n");
    connect(pin_of(zero, 0), net);
    for (uint32_t k = 0; k < 3; k++) {
        connect(pin_of(out, k), net);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, zero));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(module, net));
    TEST_ASSERT_FALSE(odin3_pin_valid(odin3_net_driver(module, net)));
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_pin_net(module, pin_of(zero, 0))));
    assert_sinks_intact(net, out, 3);

    odin3_node_id in = port_node("$port_in", 1);
    connect(pin_of(in, 0), net);
    TEST_ASSERT_EQUAL_UINT32(pin_of(in, 0).v, odin3_net_driver(module, net).v);
    assert_sinks_intact(net, out, 3);
}

static void test_many_pins_remove_every_other(void) {
    odin3_node_id sinks = port_node("$port_out", MANY_SINKS);
    odin3_node_id drivers = port_node("$port_inout", FEW_DRIVERS);
    odin3_net_id net = net_named("wide");
    /* Interleave: one inout driver after every 100 sinks, so the boundary moves often. */
    static odin3_pin_id order[MANY_SINKS + FEW_DRIVERS];
    uint32_t total = 0;
    uint32_t next_driver = 0;
    for (uint32_t k = 0; k < MANY_SINKS; k++) {
        order[total++] = pin_of(sinks, k);
        if (k % (MANY_SINKS / FEW_DRIVERS) == 0) {
            order[total++] = pin_of(drivers, next_driver++);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(MANY_SINKS + FEW_DRIVERS, total);
    for (uint32_t i = 0; i < total; i++) {
        connect(order[i], net);
    }
    assert_partition(net);
    TEST_ASSERT_EQUAL_UINT32(FEW_DRIVERS, odin3_net_driver_count(module, net));
    for (uint32_t i = 0; i < total; i += 2) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, order[i]));
    }
    assert_partition(net);
    /* Set compare: exactly the odd-indexed pins remain. */
    static bool expected[MANY_SINKS * 2];
    memset(expected, 0, sizeof expected);
    uint32_t want_drivers = 0;
    for (uint32_t i = 1; i < total; i += 2) {
        expected[order[i].v] = true;
        want_drivers += odin3_pin_drives(module, order[i]) ? 1U : 0U;
    }
    odin3_pinlist all = odin3_net_pins(module, net);
    TEST_ASSERT_EQUAL_UINT32(total / 2, all.count);
    TEST_ASSERT_EQUAL_UINT32(want_drivers, odin3_net_driver_count(module, net));
    for (uint32_t i = 0; i < all.count; i++) {
        TEST_ASSERT_TRUE(expected[all.pins[i].v]);
        expected[all.pins[i].v] = false; /* each at most once */
    }
}

static void test_connect_refuses_other_net(void) {
    odin3_node_id in = port_node("$port_in", 1);
    odin3_node_id out = port_node("$port_out", 1);
    odin3_net_id left = net_named("left");
    odin3_net_id right = net_named("right");
    connect(pin_of(in, 0), left);
    connect(pin_of(out, 0), left);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_connect(module, pin_of(in, 0), right));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(left.v, odin3_pin_net(module, pin_of(in, 0)).v);
    TEST_ASSERT_EQUAL_UINT32(pin_of(in, 0).v, odin3_net_driver(module, left).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, right).count);
    assert_partition(left);
    /* Explicit disconnect, then the connect succeeds. */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(in, 0)));
    connect(pin_of(in, 0), right);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(module, left));
    TEST_ASSERT_EQUAL_UINT32(pin_of(in, 0).v, odin3_net_driver(module, right).v);
    assert_partition(left);
    assert_partition(right);
}

/* --- nets ---------------------------------------------------------------------------------- */

static void test_net_names(void) {
    odin3_net_id net = net_named("x");
    odin3_net_id other = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_create(module, intern("x"), (odin3_prov_id){0}, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_net_create(module, UINT32_MAX, (odin3_prov_id){0}, &other));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_net_end(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, net, intern("y")));
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_module_find_net(module, intern("y")).v);
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_module_find_net(module, intern("x"))));
    odin3_net_id x2 = net_named("x");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_rename(module, x2, intern("y")));
    TEST_ASSERT_EQUAL_UINT32(intern("x"), odin3_net_name(module, x2));
}

static void test_net_delete(void) {
    odin3_net_id net = net_named("d");
    odin3_node_id one = node_of("$_CONST1_");
    connect(pin_of(one, 0), net);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, net));
    TEST_ASSERT_TRUE(odin3_net_live(module, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(one, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, net));
    TEST_ASSERT_FALSE(odin3_net_live(module, net));
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_module_find_net(module, intern("d"))));
    odin3_net_id again = net_named("d"); /* the name is free again */
    TEST_ASSERT_EQUAL_UINT32(net.v + 1, again.v);
}

static void test_dead_ids_rejected(void) {
    odin3_node_id node = node_of("$_CONST1_");
    odin3_pin_id pin = pin_of(node, 0);
    odin3_net_id net = net_named("live");
    odin3_net_id dead_net = net_named("dead");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, dead_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_connect(module, pin, dead_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, dead_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_rename(module, dead_net, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_delete(module, node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_rename(module, node, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_connect(module, pin, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_disconnect(module, pin));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, net).count);
    TEST_ASSERT_EQUAL_size_t(7, errors_logged);
}

static void test_out_of_range_ids(void) {
    odin3_net_id net = net_named("live");
    odin3_pin_id far_pin = {UINT32_MAX};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_connect(module, far_pin, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pin_connect(module, (odin3_pin_id){0}, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_delete(module, (odin3_node_id){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_delete(module, (odin3_net_id){99}));
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
}

static void test_out_of_range_accessors(void) {
    odin3_pin_id far_pin = {UINT32_MAX};
    odin3_node_id far_node = {99};
    odin3_net_id far_net = {99};
    TEST_ASSERT_EQUAL_UINT32(0, odin3_pin_node(module, far_pin).v);
    TEST_ASSERT_EQUAL(false, odin3_pin_live(module, far_pin));
    TEST_ASSERT_EQUAL(false, odin3_node_live(module, far_node));
    TEST_ASSERT_EQUAL(false, odin3_net_live(module, far_net));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_pins(module, far_node).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, far_net).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver(module, far_net).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(module, far_net));
    TEST_ASSERT_NULL(odin3_node_param(module, far_node, 0));
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

/* --- memory ------------------------------------------------------------------------------- */

static size_t store_bytes(void) {
    return odin3_pagevec_bytes_reserved(module->nodes) +
           odin3_pagevec_bytes_reserved(module->pins) + odin3_pagevec_bytes_reserved(module->nets);
}

static void test_small_module_stores_are_small(void) {
    odin3_node_id one = node_of("$_CONST1_");
    connect(pin_of(one, 0), net_named("n"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_net_end(module));
    TEST_ASSERT_TRUE(store_bytes() > 0);
    TEST_ASSERT_TRUE(store_bytes() < SMALL_MODULE_BYTES);
}

/* --- pin pool ------------------------------------------------------------------------------ */

static void test_pinpool_classes(void) {
    TEST_ASSERT_EQUAL_UINT32(2, odin3_pinpool_capacity(0));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_pinpool_capacity(1));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 31, odin3_pinpool_capacity(ODIN3_PINPOOL_CLASSES - 1));
}

static void test_pinpool_reuses_blocks(void) {
    odin3_node_id one = node_of("$_CONST1_");
    odin3_node_id out = port_node("$port_out", GROW_PINS);
    odin3_net_id net = net_named(NULL);
    connect(pin_of(one, 0), net);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(one, 0)));
    for (uint32_t k = 0; k < GROW_PINS; k++) {
        connect(pin_of(out, k), net);
    }
    for (uint32_t k = 0; k < GROW_PINS; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(out, k)));
    }
    size_t reserved = odin3_pinpool_bytes_reserved(&module->pinpool);
    for (uint32_t i = 0; i < CYCLES; i++) {
        connect(pin_of(one, 0), net);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(one, 0)));
    }
    for (uint32_t round = 0; round < GROW_ROUNDS; round++) {
        for (uint32_t k = 0; k < GROW_PINS; k++) {
            connect(pin_of(out, k), net);
        }
        for (uint32_t k = 0; k < GROW_PINS; k++) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin_of(out, k)));
        }
    }
    TEST_ASSERT_EQUAL_size_t(reserved, odin3_pinpool_bytes_reserved(&module->pinpool));
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Observable state of the module: counts, the first SNAP_MAX pins and nets, a type, a name. */
typedef struct snapshot {
    uint32_t node_end, pin_end, net_end;
    uint32_t pin_net[SNAP_MAX];
    uint32_t net_count[SNAP_MAX];
    uint32_t net_drivers[SNAP_MAX];
    uint32_t instances;
    uint32_t found;
} snapshot;

static uint32_t min_u32(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

static snapshot take_snapshot(odin3_celltype_id type, uint32_t name) {
    snapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.node_end = odin3_module_node_end(module);
    snap.pin_end = odin3_module_pin_end(module);
    snap.net_end = odin3_module_net_end(module);
    for (uint32_t i = 1; i < min_u32(snap.pin_end, SNAP_MAX); i++) {
        snap.pin_net[i] = odin3_pin_net(module, (odin3_pin_id){i}).v;
    }
    for (uint32_t i = 1; i < min_u32(snap.net_end, SNAP_MAX); i++) {
        snap.net_count[i] = odin3_net_pins(module, (odin3_net_id){i}).count;
        snap.net_drivers[i] = odin3_net_driver_count(module, (odin3_net_id){i});
    }
    snap.instances = odin3_celltype_instances(design, type);
    snap.found = odin3_module_find_node(module, name).v;
    return snap;
}

static void assert_same(const snapshot *want, const snapshot *have) {
    TEST_ASSERT_EQUAL_MEMORY(want, have, sizeof *want);
}

static void build_small_netlist(odin3_net_id *net) {
    *net = net_named("n");
    odin3_node_id out = port_node("$port_out", 4);
    for (uint32_t k = 0; k < 4; k++) {
        connect(pin_of(out, k), *net);
    }
}

/* The swept node's pins straddle a pagevec page boundary, so its pin reservation allocates. */
static void test_node_create_oom_sweep(void) {
    odin3_net_id net = {0};
    build_small_netlist(&net);
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    uint32_t filler = page - 1 - odin3_module_pin_end(module);
    (void)port_node("$port_out", filler);
    TEST_ASSERT_EQUAL_UINT32(page - 1, odin3_module_pin_end(module));
    odin3_value width = odin3_value_int(3);
    odin3_node_spec spec = {type_id("test_t3_mixed"), intern("oom"), {0}, &width, 1};
    snapshot before = take_snapshot(spec.type, spec.name);
    odin3_node_id node = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fail_at = 0;
    for (; fail_at < OOM_LIMIT; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_node_create(module, &spec, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        snapshot now = take_snapshot(spec.type, spec.name);
        assert_same(&before, &now);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(fail_at > 0); /* at least one injected failure was exercised */
    TEST_ASSERT_EQUAL_UINT32(before.node_end, node.v);
    TEST_ASSERT_EQUAL_UINT32(before.pin_end, odin3_node_pins(module, node).first.v);
    TEST_ASSERT_EQUAL_UINT32(before.pin_end + 3 + MIXED_Y + 1, odin3_module_pin_end(module));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_module_find_node(module, spec.name).v);
    TEST_ASSERT_EQUAL_UINT32(before.instances + 1, odin3_celltype_instances(design, spec.type));
}

/* Uses up the pool arena's current chunk so the next block of `cls` needs a new chunk. */
static void exhaust_pool_chunk(uint32_t cls) {
    odin3_pinpool *pool = &module->pinpool;
    size_t need = odin3_pinpool_capacity(cls) * sizeof(odin3_pin_id);
    while (odin3_arena_bytes_reserved(pool->arena) - odin3_arena_bytes_used(pool->arena) >= need) {
        TEST_ASSERT_NOT_NULL(odin3_pinpool_alloc(pool, 0));
    }
}

static void test_pin_connect_oom_sweep(void) {
    odin3_net_id net = {0};
    build_small_netlist(&net);
    odin3_node_id in = port_node("$port_in", 1);
    exhaust_pool_chunk(2);
    snapshot before = take_snapshot(type_id("$port_in"), 0);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fail_at = 0;
    for (; fail_at < OOM_LIMIT; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_pin_connect(module, pin_of(in, 0), net); /* 5th pin: needs class 2 */
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        snapshot now = take_snapshot(type_id("$port_in"), 0);
        assert_same(&before, &now);
        assert_partition(net);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(fail_at > 0);
    TEST_ASSERT_EQUAL_UINT32(pin_of(in, 0).v, odin3_net_driver(module, net).v);
    TEST_ASSERT_EQUAL_UINT32(5, odin3_net_pins(module, net).count);
    assert_partition(net);
}

static void test_net_create_oom_sweep(void) {
    snapshot before = take_snapshot(type_id("$port_in"), 0);
    odin3_net_id net = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_net_create(module, intern("oom_net"), (odin3_prov_id){0}, &net);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        snapshot now = take_snapshot(type_id("$port_in"), 0);
        assert_same(&before, &now);
        TEST_ASSERT_FALSE(odin3_net_valid(odin3_module_find_net(module, intern("oom_net"))));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_module_find_net(module, intern("oom_net")).v);
}

static void test_module_create_oom_sweep(void) {
    uint32_t name = intern("oom_mod");
    odin3_module_id mid = {0};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_module_create(design, name, (odin3_prov_id){0}, &mid);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
        TEST_ASSERT_FALSE(odin3_celltype_find(design, name, NULL));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(2, mid.v);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_design_module_end(design));
}

int main(void) {
    if (odin3_celltype_register_global(&k_mixed) != ODIN3_OK) {
        return EXIT_FAILURE;
    }
    UNITY_BEGIN();
    RUN_TEST(test_module_create_registers_type);
    RUN_TEST(test_module_create_rejects_bad_names);
    RUN_TEST(test_module_instance_counts);
    RUN_TEST(test_node_port_slices);
    RUN_TEST(test_node_pin_fields);
    RUN_TEST(test_node_param_and_name);
    RUN_TEST(test_node_default_params_and_prov);
    RUN_TEST(test_node_create_rejects_bad_specs);
    RUN_TEST(test_node_names_unique);
    RUN_TEST(test_node_rename);
    RUN_TEST(test_node_delete_frees_name);
    RUN_TEST(test_node_delete_counts_instances);
    RUN_TEST(test_const_driver_and_sink);
    RUN_TEST(test_const_value_needs_one_driver);
    RUN_TEST(test_delete_only_driver_keeps_sinks);
    RUN_TEST(test_many_pins_remove_every_other);
    RUN_TEST(test_connect_refuses_other_net);
    RUN_TEST(test_net_names);
    RUN_TEST(test_net_delete);
    RUN_TEST(test_dead_ids_rejected);
    RUN_TEST(test_out_of_range_ids);
    RUN_TEST(test_out_of_range_accessors);
    RUN_TEST(test_small_module_stores_are_small);
    RUN_TEST(test_pinpool_classes);
    RUN_TEST(test_pinpool_reuses_blocks);
    RUN_TEST(test_node_create_oom_sweep);
    RUN_TEST(test_pin_connect_oom_sweep);
    RUN_TEST(test_net_create_oom_sweep);
    RUN_TEST(test_module_create_oom_sweep);
    return UNITY_END();
}
