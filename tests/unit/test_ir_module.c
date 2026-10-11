/*
 * test_ir_module.c — unit tests for module ports with a declared range and sign (IR-7; 2D IR
 * change C, EL-24, spec §6.2): the port wire's range and sign, the derived width, the module cell
 * type's port record, the two refusals and out of memory.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    OOM_LIMIT = 1000,
    RANGE_PORT_ALLOCS = 6, /* port and port-def vectors, node/pin/net/wire pages */
};

static odin3_design *design;
static odin3_module *module;
static odin3_prov_id prov;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR ? 1 : 0;
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &id));
    return odin3_module_get(design, id);
}

/* A fresh design with a SOURCE record (so check FULL has nothing to warn about) and module top. */
static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_pass_ctx run = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("test"), &run));
    odin3_srcloc loc = {intern("t.v"), 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run, &origin, &prov));
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

/* The declared shape of a port: width (0 = derive from the range), range and flags. */
typedef struct port_shape {
    uint32_t width;
    bool scalar;
    bool has_range;
    int32_t msb, lsb;
    bool is_signed;
} port_shape;

static odin3_port_spec spec_of(const char *name, odin3_dir dir, port_shape shape) {
    odin3_port_spec spec = {intern(name),    dir,       shape.width, shape.scalar,   prov,
                            shape.has_range, shape.msb, shape.lsb,   shape.is_signed};
    return spec;
}

static odin3_node_id add_port_ok(const char *name, odin3_dir dir, port_shape shape) {
    odin3_port_spec spec = spec_of(name, dir, shape);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, &node));
    TEST_ASSERT_TRUE(odin3_node_live(module, node));
    return node;
}

/* The module's cell-type record of port `index`. */
static const odin3_port_def *type_port(uint32_t index) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(module));
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_TRUE(index < def->n_ports);
    return &def->ports[index];
}

static odin3_status check_full(void) {
    return odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE});
}

/* Port `index` has a live wire [msb:lsb] of the given sign and width, pin k on wire bit k's net. */
static void assert_port_wire(uint32_t index, port_shape want, uint32_t width) {
    odin3_node_id node = odin3_module_port(module, index);
    odin3_wire_id wire = odin3_module_port_wire(module, index);
    TEST_ASSERT_TRUE(odin3_wire_live(module, wire));
    TEST_ASSERT_EQUAL_INT32(want.msb, odin3_wire_msb(module, wire));
    TEST_ASSERT_EQUAL_INT32(want.lsb, odin3_wire_lsb(module, wire));
    TEST_ASSERT_EQUAL(want.is_signed, odin3_wire_signed(module, wire));
    TEST_ASSERT_EQUAL_UINT32(width, odin3_wire_width(module, wire));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_wire_port_node(module, wire).v);
    odin3_pinslice pins = odin3_node_pins(module, node);
    TEST_ASSERT_EQUAL_UINT32(width, pins.count);
    for (uint32_t k = 0; k < width; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(module, wire, k).v, odin3_pin_net(module, pin).v);
    }
    const odin3_value *wparam = odin3_node_param(module, node, 0);
    TEST_ASSERT_NOT_NULL(wparam);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, wparam->kind);
    TEST_ASSERT_EQUAL_INT64(width, wparam->i);
}

/* input signed [0:7] a: width 0, the range gives it. */
static void test_signed_ascending_range(void) {
    port_shape shape = {0, false, true, 0, 7, true};
    odin3_node_id node = add_port_ok("a", ODIN3_DIR_IN, shape);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_port_count(module));
    TEST_ASSERT_EQUAL_UINT32(node.v, odin3_module_port(module, 0).v);
    assert_port_wire(0, shape, 8);
    odin3_wire_id wire = odin3_module_port_wire(module, 0);
    TEST_ASSERT_EQUAL_INT32(7, odin3_wire_index(module, wire, 0));
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_index(module, wire, 7));
    TEST_ASSERT_EQUAL_INT64(8, odin3_node_param(module, node, 0)->i); /* the port node's WIDTH */
    const odin3_port_def *pdef = type_port(0);
    TEST_ASSERT_EQUAL_STRING("a", pdef->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, pdef->dir);
    TEST_ASSERT_EQUAL_UINT32(8, pdef->width);
    TEST_ASSERT_TRUE(pdef->is_signed);
    TEST_ASSERT_FALSE(pdef->scalar);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

/* [-1:2]: four bits, bit 0 is index 2 and bit 3 (the MSB) is index -1. */
static void test_negative_bound(void) {
    port_shape shape = {0, false, true, -1, 2, false};
    (void)add_port_ok("n", ODIN3_DIR_OUT, shape);
    assert_port_wire(0, shape, 4);
    odin3_wire_id wire = odin3_module_port_wire(module, 0);
    TEST_ASSERT_EQUAL_INT32(2, odin3_wire_index(module, wire, 0));
    TEST_ASSERT_EQUAL_INT32(-1, odin3_wire_index(module, wire, 3));
    TEST_ASSERT_EQUAL_UINT32(4, type_port(0)->width);
    TEST_ASSERT_FALSE(type_port(0)->is_signed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full()); /* undriven output nets only warn */
}

/* Counts that must not move on a refused add_port. */
typedef struct counts {
    uint32_t ports, type_ports, node_end, pin_end, net_end, wire_end, found_wire;
} counts;

static counts take_counts(uint32_t name) {
    counts now = {odin3_module_port_count(module),
                  odin3_celltype_get(design, odin3_module_celltype(module))->n_ports,
                  odin3_module_node_end(module),
                  odin3_module_pin_end(module),
                  odin3_module_net_end(module),
                  odin3_module_wire_end(module),
                  odin3_module_find_wire(module, name).v};
    return now;
}

static void assert_counts(const counts *want, const counts *have) {
    TEST_ASSERT_EQUAL_MEMORY(want, have, sizeof *want);
}

/* Refused with INVALID_ARG (one error logged) and nothing changed. */
static void expect_refused(const char *name, port_shape shape) {
    odin3_port_spec spec = spec_of(name, ODIN3_DIR_IN, shape);
    counts before = take_counts(spec.name);
    size_t errors = errors_logged;
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_add_port(module, &spec, &node));
    TEST_ASSERT_EQUAL_UINT32(0, node.v);
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
    counts after = take_counts(spec.name);
    assert_counts(&before, &after);
}

/* has_range [7:0] with width 4 is refused; with width 8 (or 0) it is accepted. */
static void test_width_must_match_range(void) {
    (void)add_port_ok("first", ODIN3_DIR_IN, (port_shape){1, true, false, 0, 0, false});
    expect_refused("b", (port_shape){4, false, true, 7, 0, false});
    expect_refused("b", (port_shape){9, false, true, 7, 0, false});
    port_shape eight = {8, false, true, 7, 0, false};
    (void)add_port_ok("b", ODIN3_DIR_IN, eight);
    assert_port_wire(1, eight, 8);
    TEST_ASSERT_EQUAL_UINT32(8, type_port(1)->width);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

/* scalar with [3:0] is refused; scalar with [5:5] is a one-bit port. */
static void test_scalar_needs_one_bit_range(void) {
    expect_refused("s", (port_shape){0, true, true, 3, 0, false});
    expect_refused("s", (port_shape){4, true, true, 3, 0, false});
    port_shape one = {0, true, true, 5, 5, false};
    (void)add_port_ok("s", ODIN3_DIR_IN, one);
    assert_port_wire(0, one, 1);
    odin3_wire_id wire = odin3_module_port_wire(module, 0);
    TEST_ASSERT_EQUAL_INT32(5, odin3_wire_index(module, wire, 0));
    TEST_ASSERT_EQUAL_UINT32(1, type_port(0)->width);
    TEST_ASSERT_TRUE(type_port(0)->scalar);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

/* No range: [width-1:0] as before, msb and lsb ignored, is_signed applied. */
static void test_no_range_applies_sign(void) {
    port_shape shape = {3, false, false, 40, -40, true};
    (void)add_port_ok("u", ODIN3_DIR_IN, shape);
    port_shape want = {3, false, false, 2, 0, true};
    assert_port_wire(0, want, 3);
    TEST_ASSERT_EQUAL_UINT32(3, type_port(0)->width);
    TEST_ASSERT_TRUE(type_port(0)->is_signed);
    port_shape plain = {2, false, false, 0, 0, false};
    (void)add_port_ok("v", ODIN3_DIR_IN, plain);
    assert_port_wire(1, (port_shape){2, false, false, 1, 0, false}, 2);
    TEST_ASSERT_FALSE(type_port(1)->is_signed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

/* A range of more than 2^31 bits is refused like a width above 2^31, before any allocation. */
static void test_range_width_cap(void) {
    expect_refused("w", (port_shape){0, false, true, INT32_MAX, INT32_MIN, false});
    expect_refused("w", (port_shape){0, false, true, INT32_MIN, INT32_MAX, false});
    expect_refused("w", (port_shape){0, false, true, INT32_MAX, -1, false});
    expect_refused("w", (port_shape){0, false, false, 0, 0, false}); /* no range, width 0 */
}

/* An instance of the module gets the derived width on its pins (IR-7). */
static void test_instance_sees_the_derived_width(void) {
    (void)add_port_ok("a", ODIN3_DIR_IN, (port_shape){0, false, true, 0, 7, true});
    (void)add_port_ok("y", ODIN3_DIR_OUT, (port_shape){0, false, true, -1, 2, false});
    odin3_module *parent = new_module("parent");
    odin3_node_spec spec = {odin3_module_celltype(module), intern("u1"), prov, NULL, 0};
    odin3_node_id inst = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(parent, &spec, &inst));
    TEST_ASSERT_EQUAL_UINT32(8, odin3_node_port(parent, inst, 0).count);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_node_port(parent, inst, 1).count);
    TEST_ASSERT_EQUAL_UINT32(12, odin3_node_pins(parent, inst).count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full()); /* dangling instance pins only warn */
    /* instantiated: a further port is refused with nothing changed (IR-7) */
    expect_refused("late", (port_shape){0, false, true, 1, 0, true});
}

/* Fills the node, pin, net and wire stores to a page boundary through the API (IR-18). */
static void fill_to_page_boundaries(void) {
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    uint32_t wire_page = UINT32_C(1) << ODIN3_WIRE_PAGE_SHIFT;
    while (odin3_module_net_end(module) < page) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, NULL));
    }
    odin3_net_id nets[] = {{1}};
    while (odin3_module_wire_end(module) < wire_page) {
        odin3_wire_spec spec = {0, 0, 0, false, prov};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, nets, NULL));
    }
    odin3_node_spec spec = {{0}, 0, prov, NULL, 0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_CONST0_"), &spec.type));
    while (odin3_module_node_end(module) < page) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(page, odin3_module_pin_end(module));
}

/*
 * Every allocation of add_port with a range fails cleanly: NO_MEMORY, module unchanged. The
 * module's first port, with every store at a page boundary, allocates exactly six times: the
 * port and port-def vectors and the node, pin, net and wire stores.
 */
static void test_add_port_with_range_oom_sweep(void) {
    port_shape shape = {0, false, true, -2, 5, true};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT; fail_at++) {
        fresh_design();
        fill_to_page_boundaries();
        odin3_port_spec spec = spec_of("r", ODIN3_DIR_IN, shape);
        counts before = take_counts(spec.name);
        odin3_node_id node = {0};
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_module_add_port(module, &spec, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        counts after = take_counts(spec.name);
        assert_counts(&before, &after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(RANGE_PORT_ALLOCS, failures);
    assert_port_wire(0, shape, 8);
    TEST_ASSERT_TRUE(type_port(0)->is_signed);
    TEST_ASSERT_EQUAL_UINT32(8, type_port(0)->width);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_signed_ascending_range);
    RUN_TEST(test_negative_bound);
    RUN_TEST(test_width_must_match_range);
    RUN_TEST(test_scalar_needs_one_bit_range);
    RUN_TEST(test_no_range_applies_sign);
    RUN_TEST(test_range_width_cap);
    RUN_TEST(test_instance_sees_the_derived_width);
    RUN_TEST(test_add_port_with_range_oom_sweep);
    return UNITY_END();
}
