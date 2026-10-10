/*
 * test_abi_core.c — ABI paths that need the core's internals: the simulation hooks a cell type
 * registered through odin3_celltype_register may set reach the 1E simulator (the hooks see the
 * simulator's view, sim/cell.h, whose layout is not part of the ABI), and out-of-memory sweeps
 * over cell-type and pass registration and plugin pass runs (util/alloc.h failure injection).
 */
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "sim/cell.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif
#ifndef ODIN3_CLI_FIXTURES
#error "ODIN3_CLI_FIXTURES must name tests/cli"
#endif

enum { PATH_BUF = 512, AND_SCRATCH = 3 };
enum { PORT_A, PORT_B, PORT_Y };

static uint32_t g_sim_calls;
static uint32_t g_scratch_calls;

/* example_and: Y[k] = A[k] & B[k]. */
static void and_simulate(const odin3_sim_cell *cell) {
    g_sim_calls++;
    const odin3_sim_span *out = &cell->ports[PORT_Y];
    for (uint32_t k = 0; k < out->width; k++) {
        uint8_t lhs = cell->values[cell->ports[PORT_A].idx[k]];
        uint8_t rhs = cell->values[cell->ports[PORT_B].idx[k]];
        cell->values[out->idx[k]] = (uint8_t)(lhs & rhs);
    }
}

static odin3_status and_scratch(const odin3_sim_cell *cell, uint32_t *bytes) {
    g_scratch_calls++;
    TEST_ASSERT_NULL(cell->values); /* the sizing view */
    *bytes = AND_SCRATCH;
    return ODIN3_OK;
}

static const odin3_plugin_port k_ports[] = {{"A", ODIN3_DIR_IN, 0, "WIDTH", false},
                                            {"B", ODIN3_DIR_IN, 0, "WIDTH", false},
                                            {"Y", ODIN3_DIR_OUT, 0, "WIDTH", false}};
static const odin3_plugin_param k_params[] = {{"WIDTH", ODIN3_VAL_INT, 1}};

void setUp(void) {
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void set_bit(odin3_sim *sim, uint32_t port, uint32_t bit, bool value) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(sim, (odin3_sim_bit){port, bit}, value));
}

static bool out_bit(const odin3_sim *sim, uint32_t port, uint32_t bit) {
    bool value = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &value));
    return value;
}

/* Registers example_and with both hooks through the ABI, reads the fixture into a new design
 * and builds its simulator (the scratch hook runs once per flat cell). */
static odin3_sim *build_example(odin3_design **design) {
    odin3_plugin_celltype def = {"example_and", ODIN3_GRAN_HARD, 0,     k_ports, k_params, 3, 1,
                                 and_simulate,  and_scratch,     {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&def));
    *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(*design);
    char args[PATH_BUF];
    (void)snprintf(args, sizeof args, "\"%s/example_plugin.blif\"", ODIN3_CLI_FIXTURES);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(*design, "read_blif", args));
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_build(*design, odin3_design_top(*design), &sim));
    TEST_ASSERT_EQUAL_UINT32(2, g_scratch_calls);
    return sim;
}

/* Drives a = 0b11, b = 0b01 and c, then runs one cycle. */
static void drive(odin3_sim *sim, bool c_value) {
    set_bit(sim, 0, 0, true);
    set_bit(sim, 0, 1, true);
    set_bit(sim, 1, 0, true);
    set_bit(sim, 1, 1, false);
    set_bit(sim, 2, 0, c_value);
    odin3_sim_cycle(sim);
}

/* The outputs packed as y[0] | y[1] << 1 | z << 2. */
static uint32_t outputs(const odin3_sim *sim) {
    return (out_bit(sim, 0, 0) ? 1U : 0U) | (out_bit(sim, 0, 1) ? 2U : 0U) |
           (out_bit(sim, 1, 0) ? 4U : 0U);
}

/* Inputs a[2], b[2], c; outputs y[2] = a & b and z = c & a[0]. */
static void test_registered_hooks_simulate(void) {
    odin3_design *design = NULL;
    odin3_sim *sim = build_example(&design);
    drive(sim, true);
    TEST_ASSERT_TRUE(g_sim_calls > 0);
    TEST_ASSERT_EQUAL_UINT32(5, outputs(sim)); /* y = 01, z = 1 */
    drive(sim, false);
    TEST_ASSERT_EQUAL_UINT32(1, outputs(sim)); /* y = 01, z = 0 */
    odin3_sim_destroy(sim);
    odin3_design_destroy(design);
}

/* Registration under every allocation failure: NO_MEMORY changes nothing, so the same
 * definition registers once allocation succeeds, and a second time is a duplicate. */
static void test_celltype_register_oom_sweep(void) {
    odin3_plugin_celltype def = {"oom_and", ODIN3_GRAN_HARD, 0, k_ports, k_params, 3, 1, NULL,
                                 NULL,      {NULL}};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fail_after = 0;
    for (; st == ODIN3_ERR_NO_MEMORY; fail_after++) {
        odin3_util_set_alloc_fail_after(fail_after);
        st = odin3_celltype_register(&def);
        odin3_util_set_alloc_fail_after(-1);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(fail_after > 1); /* the copy and the registry slot both failed once */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&def));
}

static uint32_t g_oom_runs;

static odin3_status oom_run(odin3_design *design, const char *args, void *user) {
    (void)design;
    (void)user;
    g_oom_runs++;
    TEST_ASSERT_EQUAL_STRING("a b", args);
    return ODIN3_OK;
}

/* Pass registration, then a run of the pass, under every allocation failure. */
static void test_pass_register_and_run_oom_sweep(void) {
    odin3_plugin_pass pass = {"oom_pass", "oom_pass: counts its runs", oom_run, NULL, {NULL}};
    uint32_t before = odin3_pass_get_count();
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_after = 0; st == ODIN3_ERR_NO_MEMORY; fail_after++) {
        odin3_util_set_alloc_fail_after(fail_after);
        st = odin3_pass_register(&pass);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_UINT32(st == ODIN3_OK ? before + 1 : before, odin3_pass_get_count());
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    st = ODIN3_ERR_NO_MEMORY;
    for (long fail_after = 0; st == ODIN3_ERR_NO_MEMORY; fail_after++) {
        odin3_design *design = odin3_design_create(); /* a fresh design per try */
        TEST_ASSERT_NOT_NULL(design);
        uint32_t runs = g_oom_runs;
        odin3_util_set_alloc_fail_after(fail_after);
        st = odin3_design_run_pass(design, "oom_pass", "a b");
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_UINT32(st == ODIN3_OK ? runs + 1 : runs, g_oom_runs);
        odin3_design_destroy(design);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

/* --- odin3_prov_visit_objects on dead objects and tombstones ------------------------------- */

enum { HAND_SUB_LINE = 18, MAX_FOUND = 32 };

typedef struct found_list {
    uint32_t count;
    odin3_prov_object found[MAX_FOUND];
} found_list;

static void collect(const odin3_prov_object *found, void *user) {
    found_list *list = user;
    if (list->count < MAX_FOUND) {
        list->found[list->count++] = *found;
    }
}

/* The node hits of hand_body.blif line 18 (the cell u_sub's line). */
static found_list sub_line_nodes(const odin3_design *design) {
    found_list all = {0};
    found_list nodes = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_visit_objects(design, ODIN3_BLIF_FIXTURES "/hand_body.blif",
                                                   HAND_SUB_LINE, collect, &all));
    for (uint32_t i = 0; i < all.count; i++) {
        if (all.found[i].obj.kind == (uint32_t)ODIN3_OBJ_NODE) {
            nodes.found[nodes.count++] = all.found[i];
        }
    }
    return nodes;
}

/* list holds exactly one hit: object ID id, live as given. */
static void expect_one(const found_list *list, uint32_t id, bool live) {
    TEST_ASSERT_EQUAL_UINT32(1, list->count);
    TEST_ASSERT_EQUAL_UINT32(id, list->found[0].obj.id);
    TEST_ASSERT_EQUAL(live, list->found[0].live);
}

/* A deleted node is found with its own ID and live false; after compact, as its tombstone (ID 0,
 * live false). */
static void test_visit_objects_dead_and_tombstone(void) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_blif",
                                                          ODIN3_BLIF_FIXTURES "/hand_body.blif"));
    odin3_module_id top = odin3_design_top(design);
    uint32_t sub = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(design, top.v, "u_sub", &sub));
    found_list live = sub_line_nodes(design);
    expect_one(&live, sub, true);
    odin3_module *module = odin3_module_get(design, top);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, (odin3_node_id){sub}));
    found_list dead = sub_line_nodes(design);
    expect_one(&dead, sub, false);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "compact", NULL));
    found_list tomb = sub_line_nodes(design);
    expect_one(&tomb, 0, false); /* the tombstone */
    TEST_ASSERT_EQUAL_UINT32(top.v, tomb.found[0].obj.module);
    odin3_design_destroy(design);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_registered_hooks_simulate);
    RUN_TEST(test_celltype_register_oom_sweep);
    RUN_TEST(test_pass_register_and_run_oom_sweep);
    RUN_TEST(test_visit_objects_dead_and_tombstone);
    return UNITY_END();
}
