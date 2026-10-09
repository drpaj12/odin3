/*
 * test_sim_build.c — odin3_sim_build: flattening, value-slot sharing, clocks, levelization, errors.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 4096, MAX_BITS = 8, OOM_LIMIT = 100000, BIG = 200000, NAME_BUF = 32 };

static odin3_design *design;
static char log_text[LOG_TEXT];

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- IR construction -------------------------------------------------------------------------- */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, &mid));
    return odin3_module_get(design, mid);
}

/* A one-bit port; returns its net. */
static odin3_net_id add_port(odin3_module *mod, const char *name, odin3_dir dir) {
    odin3_port_spec spec = {intern(name), dir, 1, true, (odin3_prov_id){0}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
    return odin3_pin_net(mod, odin3_node_pins(mod, node).first);
}

static odin3_net_id net(odin3_module *mod, const char *name) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, name ? intern(name) : 0, (odin3_prov_id){0}, &id));
    return id;
}

/* A node of type `type` with one-bit ports connected to nets (one per port, in port order). */
static odin3_node_id cell_prov(odin3_module *mod, odin3_celltype_id type, const odin3_net_id *nets,
                               odin3_prov_id prov) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    odin3_netvec ports[MAX_BITS];
    for (uint32_t i = 0; i < def->n_ports; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    odin3_node_spec spec = {type, 0, prov, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

static odin3_node_id cell(odin3_module *mod, const char *type, const odin3_net_id *nets) {
    return cell_prov(mod, type_id(type), nets, (odin3_prov_id){0});
}

static odin3_node_id inst(odin3_module *mod, const odin3_module *child, const char *name,
                          const odin3_net_id *nets) {
    odin3_node_id node =
        cell(mod, odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(child)), nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(mod, node, intern(name)));
    return node;
}

static odin3_sim *build_ok(const odin3_module *top) {
    odin3_sim *sim = NULL;
    odin3_status st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL(sim);
    return sim;
}

static void build_fails(const odin3_module *top, const char *needle) {
    odin3_sim *sim = (odin3_sim *)&sim; /* must be reset to NULL */
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_build(design, odin3_module_id_of(top), &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
}

/* --- a test-only settle (the cycle engine is Task 3) ------------------------------------------ */

static void send(const odin3_sim_flat *flat, odin3_sim_event event) {
    odin3_sim_cell view = flat->view;
    view.event = event;
    flat->def->simulate(&view);
}

static void init_state(const odin3_sim *sim) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        if (odin3_sim_is_seq(sim->cells[i].def)) {
            send(&sim->cells[i], ODIN3_SIM_INIT);
        }
    }
}

/* Drives input bits from the bits of pattern, settles once in level order, returns the outputs. */
static uint32_t settle(const odin3_sim *sim, uint32_t pattern) {
    for (uint32_t k = 0; k < sim->n_in_bits; k++) {
        sim->values[sim->in_bits[k]] = (uint8_t)((pattern >> k) & 1U);
    }
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        send(&sim->cells[sim->order[i]], ODIN3_SIM_COMB);
    }
    uint32_t outputs = 0;
    for (uint32_t k = 0; k < sim->n_out_bits; k++) {
        outputs |= (uint32_t)sim->values[sim->out_bits[k]] << k;
    }
    return outputs;
}

/* --- fixtures --------------------------------------------------------------------------------- */

/* half(a, b) -> s = a ^ b, c = a & b. */
static odin3_module *make_half(void) {
    odin3_module *half = new_module("half");
    odin3_net_id net_a = add_port(half, "a", ODIN3_DIR_IN);
    odin3_net_id net_b = add_port(half, "b", ODIN3_DIR_IN);
    odin3_net_id net_s = add_port(half, "s", ODIN3_DIR_OUT);
    odin3_net_id net_c = add_port(half, "c", ODIN3_DIR_OUT);
    cell(half, "$_XOR_", (odin3_net_id[]){net_a, net_b, net_s});
    cell(half, "$_AND_", (odin3_net_id[]){net_a, net_b, net_c});
    return half;
}

/* full(x, y, z) -> sum, carry from two half instances (the second level of hierarchy). */
static odin3_module *make_full(const odin3_module *half) {
    odin3_module *full = new_module("full");
    odin3_net_id net_x = add_port(full, "x", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(full, "y", ODIN3_DIR_IN);
    odin3_net_id net_z = add_port(full, "z", ODIN3_DIR_IN);
    odin3_net_id sum = add_port(full, "sum", ODIN3_DIR_OUT);
    odin3_net_id carry = add_port(full, "carry", ODIN3_DIR_OUT);
    odin3_net_id s1 = net(full, "s1");
    odin3_net_id c1 = net(full, "c1");
    odin3_net_id c2 = net(full, "c2");
    inst(full, half, "h1", (odin3_net_id[]){net_x, net_y, s1, c1});
    inst(full, half, "h2", (odin3_net_id[]){s1, net_z, sum, c2});
    cell(full, "$_OR_", (odin3_net_id[]){c1, c2, carry});
    return full;
}

/* top(p, q, r) -> o0, o1 through one full instance; flat(p, q, r) the same, written flat. */
static odin3_module *make_top(const odin3_module *full) {
    odin3_module *top = new_module("top");
    odin3_net_id net_p = add_port(top, "p", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(top, "q", ODIN3_DIR_IN);
    odin3_net_id net_r = add_port(top, "r", ODIN3_DIR_IN);
    odin3_net_id o0 = add_port(top, "o0", ODIN3_DIR_OUT);
    odin3_net_id o1 = add_port(top, "o1", ODIN3_DIR_OUT);
    inst(top, full, "u", (odin3_net_id[]){net_p, net_q, net_r, o0, o1});
    return top;
}

static odin3_module *make_flat(void) {
    odin3_module *flat = new_module("flat");
    odin3_net_id net_p = add_port(flat, "p", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(flat, "q", ODIN3_DIR_IN);
    odin3_net_id net_r = add_port(flat, "r", ODIN3_DIR_IN);
    odin3_net_id o0 = add_port(flat, "o0", ODIN3_DIR_OUT);
    odin3_net_id o1 = add_port(flat, "o1", ODIN3_DIR_OUT);
    odin3_net_id s1 = net(flat, NULL);
    odin3_net_id c1 = net(flat, NULL);
    odin3_net_id c2 = net(flat, NULL);
    /* deliberately in reverse dependency order: levelization must fix it */
    cell(flat, "$_OR_", (odin3_net_id[]){c1, c2, o1});
    cell(flat, "$_AND_", (odin3_net_id[]){s1, net_r, c2});
    cell(flat, "$_XOR_", (odin3_net_id[]){s1, net_r, o0});
    cell(flat, "$_AND_", (odin3_net_id[]){net_p, net_q, c1});
    cell(flat, "$_XOR_", (odin3_net_id[]){net_p, net_q, s1});
    return flat;
}

/* The flat cell expanded from node of module mod, the nth such (instances in expansion order). */
static const odin3_sim_flat *find_flat(const odin3_sim *sim, const odin3_module *mod, uint32_t node,
                                       uint32_t nth) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        const odin3_sim_flat *flat = &sim->cells[i];
        if (flat->module.v == odin3_module_id_of(mod).v && flat->node.v == node && nth-- == 0) {
            return flat;
        }
    }
    TEST_FAIL_MESSAGE("flat cell not found");
    return NULL;
}

static uint32_t pin_slot(const odin3_sim_flat *flat, uint32_t port) {
    return flat->view.ports[port].idx[0];
}

/* --- tests ------------------------------------------------------------------------------------ */

static void test_two_level_hierarchy_matches_flat(void) {
    odin3_module *half = make_half();
    odin3_module *top = make_top(make_full(half));
    odin3_sim *hier = build_ok(top);
    odin3_sim *flat = build_ok(make_flat());
    TEST_ASSERT_EQUAL_UINT32(5, hier->n_cells);
    TEST_ASSERT_EQUAL_UINT32(5, flat->n_cells);
    TEST_ASSERT_EQUAL_UINT32(3, hier->n_in_bits);
    TEST_ASSERT_EQUAL_UINT32(2, hier->n_out_bits);
    TEST_ASSERT_EQUAL_UINT32(2, hier->n_out_ports);
    TEST_ASSERT_EQUAL_UINT32(intern("o1"), hier->out_ports[1].name);
    /* same slot count: top nets + full's internal nets, each port net shared with its parent */
    TEST_ASSERT_EQUAL_UINT32(flat->n_values, hier->n_values);
    for (uint32_t pattern = 0; pattern < 8; pattern++) {
        uint32_t want = (uint32_t)__builtin_popcount(pattern);
        TEST_ASSERT_EQUAL_UINT32(want, settle(flat, pattern));
        TEST_ASSERT_EQUAL_UINT32(want, settle(hier, pattern));
    }
    odin3_sim_destroy(hier);
    odin3_sim_destroy(flat);
}

static void test_instance_pins_share_parent_slots(void) {
    odin3_module *half = make_half();
    odin3_module *top = make_top(make_full(half));
    odin3_sim *sim = build_ok(top);
    const uint32_t xor_node = 5; /* half: four port nodes, then the XOR, then the AND */
    const odin3_sim_flat *x1 = find_flat(sim, half, xor_node, 0);
    const odin3_sim_flat *x2 = find_flat(sim, half, xor_node, 1);
    const odin3_sim_flat *a1 = find_flat(sim, half, xor_node + 1, 0);
    TEST_ASSERT_EQUAL_UINT32(sim->in_bits[0], pin_slot(x1, 0)); /* top p = full x = h1 a */
    TEST_ASSERT_EQUAL_UINT32(sim->in_bits[1], pin_slot(x1, 1));
    TEST_ASSERT_EQUAL_UINT32(pin_slot(x1, 2), pin_slot(x2, 0)); /* full s1 = h1 s = h2 a */
    TEST_ASSERT_EQUAL_UINT32(sim->in_bits[2], pin_slot(x2, 1));
    TEST_ASSERT_EQUAL_UINT32(sim->out_bits[0], pin_slot(x2, 2)); /* h2 s = full sum = top o0 */
    TEST_ASSERT_EQUAL_UINT32(pin_slot(x1, 0), pin_slot(a1, 0));
    TEST_ASSERT_TRUE(pin_slot(x1, 2) >= ODIN3_SIM_SLOT_FIRST);
    TEST_ASSERT_EQUAL_PTR(odin3_node_param(half, (odin3_node_id){xor_node}, 0), x1->view.params);
    TEST_ASSERT_NULL(x1->view.state);
    odin3_sim_destroy(sim);
}

/* A child whose output port net is merged into its input port net: a pure feedthrough. */
static void test_feedthrough_unifies_parent_nets(void) {
    odin3_module *wire = new_module("thru");
    odin3_net_id net_a = add_port(wire, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(wire, "y", ODIN3_DIR_OUT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(wire, (odin3_net_pair){net_a, net_y}));
    odin3_module *top = new_module("top");
    odin3_net_id i = add_port(top, "i", ODIN3_DIR_IN);
    odin3_net_id net_o = add_port(top, "o", ODIN3_DIR_OUT);
    odin3_net_id mid = net(top, "mid");
    inst(top, wire, "t", (odin3_net_id[]){i, mid});
    cell(top, "$_NOT_", (odin3_net_id[]){mid, net_o});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(sim->in_bits[0], pin_slot(&sim->cells[0], 0));
    TEST_ASSERT_EQUAL_UINT32(1, settle(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(0, settle(sim, 1));
    odin3_sim_destroy(sim);
}

static void test_unconnected_pins_use_reserved_slots(void) {
    odin3_module *top = new_module("top");
    odin3_net_id net_o = add_port(top, "o", ODIN3_DIR_OUT);
    cell(top, "$_NOT_", (odin3_net_id[]){{0}, net_o});
    cell(top, "$_BUF_", (odin3_net_id[]){net_o, {0}});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_SLOT_ZERO, pin_slot(&sim->cells[0], 0));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_SLOT_DISCARD, pin_slot(&sim->cells[1], 1));
    TEST_ASSERT_EQUAL_UINT32(1, settle(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(0, sim->values[ODIN3_SIM_SLOT_ZERO]);
    odin3_sim_destroy(sim);
}

/* A register feeding back through logic is legal: the flop cuts the loop. */
static void test_flop_cuts_loop_and_clock_is_flagged(void) {
    odin3_module *top = new_module("top");
    odin3_net_id d_in = add_port(top, "d", ODIN3_DIR_IN);
    odin3_net_id clk = add_port(top, "clk", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(top, "q", ODIN3_DIR_OUT);
    odin3_net_id nxt = net(top, "nxt");
    cell(top, "$_NOT_", (odin3_net_id[]){d_in, net(top, "spare")}); /* ready from the start too */
    cell(top, "$_XOR_", (odin3_net_id[]){net_q, d_in, nxt});
    cell(top, "$_DFF_P_", (odin3_net_id[]){clk, nxt, net_q});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(2, sim->order[0]); /* the flop is a source, queued first */
    TEST_ASSERT_EQUAL_UINT32(0, sim->order[1]);
    TEST_ASSERT_EQUAL_UINT32(1, sim->order[2]);
    TEST_ASSERT_EQUAL_UINT8(0, sim->in_clock[0]);
    TEST_ASSERT_EQUAL_UINT8(1, sim->in_clock[1]);
    TEST_ASSERT_EQUAL_UINT32(1, sim->n_state);
    TEST_ASSERT_NOT_NULL(sim->cells[2].view.state);
    TEST_ASSERT_NULL(sim->cells[1].view.state);
    init_state(sim);
    TEST_ASSERT_EQUAL_UINT32(0, settle(sim, 1));
    odin3_sim_destroy(sim);
}

/* A clock reaching a child flop through instance ports is still a primary input. */
static void test_clock_through_hierarchy_is_primary(void) {
    odin3_module *reg = new_module("reg");
    odin3_net_id net_c = add_port(reg, "c", ODIN3_DIR_IN);
    odin3_net_id d_in = add_port(reg, "d", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(reg, "q", ODIN3_DIR_OUT);
    cell(reg, "$_DFF_N_", (odin3_net_id[]){net_c, d_in, net_q});
    odin3_module *top = new_module("top");
    odin3_net_id d_top = add_port(top, "d", ODIN3_DIR_IN);
    odin3_net_id clk = add_port(top, "clk", ODIN3_DIR_IN);
    odin3_net_id q_top = add_port(top, "q", ODIN3_DIR_OUT);
    inst(top, reg, "r0", (odin3_net_id[]){clk, d_top, q_top});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT8(1, sim->in_clock[1]);
    odin3_sim_destroy(sim);
}

static void test_clock_from_logic_is_rejected(void) {
    odin3_module *reg = new_module("reg");
    odin3_net_id net_a = add_port(reg, "a", ODIN3_DIR_IN);
    odin3_net_id net_b = add_port(reg, "b", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(reg, "q", ODIN3_DIR_OUT);
    odin3_net_id gated = net(reg, "gated_clk");
    cell(reg, "$_AND_", (odin3_net_id[]){net_a, net_b, gated});
    cell(reg, "$_DFF_P_", (odin3_net_id[]){gated, net_a, net_q});
    odin3_module *top = new_module("top");
    odin3_net_id net_x = add_port(top, "x", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(top, "y", ODIN3_DIR_IN);
    odin3_net_id net_o = add_port(top, "o", ODIN3_DIR_OUT);
    inst(top, reg, "r0", (odin3_net_id[]){net_x, net_y, net_o});
    build_fails(top, "r0/gated_clk");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "clock"), log_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "$_DFF_P_"), log_text);
}

/* A level latch's enable may come from logic: it settles with the combinational cells. */
static void test_latch_enable_from_logic_is_allowed(void) {
    odin3_module *top = new_module("top");
    odin3_net_id net_a = add_port(top, "a", ODIN3_DIR_IN);
    odin3_net_id net_b = add_port(top, "b", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(top, "q", ODIN3_DIR_OUT);
    odin3_net_id en = net(top, "en");
    cell(top, "$_DLATCH_P_", (odin3_net_id[]){en, net_b, net_q});
    cell(top, "$_AND_", (odin3_net_id[]){net_a, net_b, en});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(1, sim->order[0]); /* the AND before the latch it enables */
    TEST_ASSERT_EQUAL_UINT8(0, sim->in_clock[0]);
    init_state(sim);
    TEST_ASSERT_EQUAL_UINT32(1, settle(sim, 3));
    odin3_sim_destroy(sim);
}

static void test_combinational_loop_names_a_net(void) {
    odin3_module *ring = new_module("ring");
    odin3_net_id net_a = add_port(ring, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(ring, "y", ODIN3_DIR_OUT);
    odin3_net_id n1 = net(ring, "loop_n1");
    cell(ring, "$_AND_", (odin3_net_id[]){net_a, n1, net_y});
    cell(ring, "$_NOT_", (odin3_net_id[]){net_y, n1});
    odin3_module *top = new_module("top");
    odin3_net_id i = add_port(top, "i", ODIN3_DIR_IN);
    odin3_net_id net_o = add_port(top, "o", ODIN3_DIR_OUT);
    odin3_net_id tail = net(top, "tail");
    cell(top, "$_NOT_", (odin3_net_id[]){net_o, tail}); /* downstream of the loop, not on it */
    inst(top, ring, "u7", (odin3_net_id[]){i, net_o});
    build_fails(top, "combinational loop");
    bool names_loop_net = strstr(log_text, "u7/loop_n1") != NULL || strstr(log_text, "`o`") != NULL;
    TEST_ASSERT_TRUE_MESSAGE(names_loop_net, log_text);
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "tail"), log_text);
}

/* A loop entirely inside an instance is named by its hierarchical path. */
static void test_internal_loop_names_hierarchical_net(void) {
    odin3_module *ring = new_module("ring");
    odin3_net_id net_a = add_port(ring, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(ring, "y", ODIN3_DIR_OUT);
    odin3_net_id n1 = net(ring, "loop_n1");
    odin3_net_id n2 = net(ring, "loop_n2");
    cell(ring, "$_BUF_", (odin3_net_id[]){n1, net_y});
    cell(ring, "$_AND_", (odin3_net_id[]){net_a, n2, n1});
    cell(ring, "$_NOT_", (odin3_net_id[]){n1, n2});
    odin3_module *top = new_module("top");
    odin3_net_id i = add_port(top, "i", ODIN3_DIR_IN);
    odin3_net_id net_o = add_port(top, "o", ODIN3_DIR_OUT);
    inst(top, ring, "u7", (odin3_net_id[]){i, net_o});
    build_fails(top, "combinational loop through net `u7/loop_n");
}

/* Level latches do not cut the graph (progress.md ruling): a loop through one is an error. */
static void test_loop_through_latch_is_rejected(void) {
    odin3_module *top = new_module("top");
    odin3_net_id en = add_port(top, "en", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(top, "q", ODIN3_DIR_OUT);
    odin3_net_id nq = net(top, "nq");
    cell(top, "$_DLATCH_P_", (odin3_net_id[]){en, nq, net_q});
    cell(top, "$_NOT_", (odin3_net_id[]){net_q, nq});
    build_fails(top, "combinational loop");
}

static void test_black_box_is_rejected_with_location(void) {
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("reader"), &ctx));
    odin3_srcloc loc = {intern("in.blif"), 7, 1, 7, 9};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&ctx, &origin, &prov));
    static const odin3_port_def k_ports[] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                             {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};
    odin3_celltype_def def = {
        "mystery", ODIN3_GRAN_BLACKBOX, 0, k_ports, 2, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_id bb = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &bb));
    odin3_module *top = new_module("top");
    odin3_net_id net_a = add_port(top, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(top, "y", ODIN3_DIR_OUT);
    cell_prov(top, bb, (odin3_net_id[]){net_a, net_y}, prov);
    build_fails(top, "cannot simulate `mystery`");
    TEST_ASSERT_EQUAL_STRING_LEN("in.blif:7: ", log_text, strlen("in.blif:7: "));
}

/* A built-in type without a simulate hook (yet) is rejected the same way. */
static void test_type_without_hook_is_rejected(void) {
    odin3_module *top = new_module("top");
    odin3_net_id net_a = add_port(top, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(top, "y", ODIN3_DIR_OUT);
    odin3_netvec ports[] = {{&net_a, 1}, {&net_a, 1}, {&net_y, 1}};
    odin3_node_spec spec = {type_id("$add"), intern("adder0"), (odin3_prov_id){0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, &node));
    build_fails(top, "cannot simulate `$add`");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "adder0"), log_text);
}

static void test_recursive_hierarchy_is_rejected(void) {
    odin3_module *self = new_module("self");
    odin3_node_spec spec = {odin3_module_celltype(self), 0, (odin3_prov_id){0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(self, &spec, &node));
    odin3_module *top = new_module("top");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(top, &spec, &node));
    build_fails(top, "recursive");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "module `self`"), log_text);
}

/* top -> A -> B -> A: the message names a module on the cycle (A or B), not top. */
static void test_indirect_recursion_names_a_module_on_the_cycle(void) {
    odin3_module *mod_a = new_module("mod_a");
    odin3_module *mod_b = new_module("mod_b");
    odin3_module *top = new_module("top");
    odin3_node_spec spec_a = {odin3_module_celltype(mod_a), 0, (odin3_prov_id){0}, NULL, 0};
    odin3_node_spec spec_b = {odin3_module_celltype(mod_b), 0, (odin3_prov_id){0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(top, &spec_a, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod_a, &spec_b, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod_b, &spec_a, &node));
    build_fails(top, "recursive");
    bool on_cycle =
        strstr(log_text, "module `mod_a`") != NULL || strstr(log_text, "module `mod_b`") != NULL;
    TEST_ASSERT_TRUE_MESSAGE(on_cycle, log_text);
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "`top`"), log_text);
}

/* A flop whose clock pin is open: its own wording, not "not a primary input". */
static void test_unconnected_clock_is_rejected(void) {
    odin3_module *top = new_module("top");
    odin3_net_id d_in = add_port(top, "d", ODIN3_DIR_IN);
    odin3_net_id net_q = add_port(top, "q", ODIN3_DIR_OUT);
    odin3_node_id flop = cell(top, "$_DFF_P_", (odin3_net_id[]){{0}, d_in, net_q});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(top, flop, intern("reg0")));
    build_fails(top, "`$_DFF_P_` node `reg0` has no clock connected");
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "primary input"), log_text);
}

static void test_inout_top_port_is_rejected(void) {
    odin3_module *top = new_module("top");
    add_port(top, "bus", ODIN3_DIR_INOUT);
    build_fails(top, "cannot simulate inout port `bus` of the top module");
}

static void test_bad_top_is_rejected(void) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_build(design, (odin3_module_id){42}, &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "no module 42 to simulate"), log_text);
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_build(design, (odin3_module_id){1}, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "out is NULL"), log_text);
    odin3_sim_destroy(NULL);
}

/* The OOM fixture: the two-level adder plus a flop clocked by a primary input, in a fresh design.
 */
static odin3_module *make_oom_top(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module *top = make_top(make_full(make_half()));
    odin3_net_id clk = add_port(top, "clk", ODIN3_DIR_IN);
    odin3_net_id reg_q = add_port(top, "reg_q", ODIN3_DIR_OUT);
    odin3_net_id d_in = odin3_wire_net(top, odin3_module_find_wire(top, intern("p")), 0);
    cell(top, "$_DFF_P_", (odin3_net_id[]){clk, d_in, reg_q});
    return top;
}

/* One build with allocation fail_at failing: either NO_MEMORY with nothing returned, or a sim. */
static odin3_status oom_try(long fail_at) {
    odin3_module *top = make_oom_top();
    odin3_sim *sim = (odin3_sim *)&sim; /* must be reset to NULL */
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(sim);
        return st;
    }
    TEST_ASSERT_EQUAL_UINT32(2, settle(sim, 3) & 3U); /* p + q = 0b10 */
    odin3_sim_destroy(sim);
    return st;
}

/* Every allocation of a hierarchical build with a flop fails in turn (fresh design per try). */
static void test_oom_sweep(void) {
    long fail_at = 0;
    while (fail_at < OOM_LIMIT && oom_try(fail_at) != ODIN3_OK) {
        fail_at++;
    }
    TEST_ASSERT_TRUE(fail_at > 10); /* the sweep really hit allocation points */
    TEST_ASSERT_TRUE(fail_at < OOM_LIMIT);
}

/* top: in -> n/2 instances of inv2 (two NOTs each) in a chain -> out. */
static odin3_module *make_chain(uint32_t n_cells) {
    odin3_module *inv2 = new_module("inv2");
    odin3_net_id net_a = add_port(inv2, "a", ODIN3_DIR_IN);
    odin3_net_id net_y = add_port(inv2, "y", ODIN3_DIR_OUT);
    odin3_net_id net_m = net(inv2, NULL);
    cell(inv2, "$_NOT_", (odin3_net_id[]){net_m, net_y}); /* reverse order on purpose */
    cell(inv2, "$_NOT_", (odin3_net_id[]){net_a, net_m});
    odin3_module *top = new_module("top");
    odin3_net_id prev = add_port(top, "i", ODIN3_DIR_IN);
    odin3_net_id out = add_port(top, "o", ODIN3_DIR_OUT);
    odin3_node_spec spec = {odin3_module_celltype(inv2), 0, (odin3_prov_id){0}, NULL, 0};
    for (uint32_t i = 0; i < n_cells / 2; i++) {
        odin3_net_id next = i + 1 == n_cells / 2 ? out : net(top, NULL);
        odin3_net_id pins[2] = {prev, next};
        odin3_netvec ports[2] = {{&pins[0], 1}, {&pins[1], 1}};
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, &node));
        prev = next;
    }
    return top;
}

/* True when building top succeeds with at most `allowed` allocations (the rest fail). */
static bool builds_within(const odin3_module *top, long allowed) {
    odin3_sim *sim = NULL;
    odin3_util_set_alloc_fail_after(allowed);
    odin3_status st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
    odin3_util_set_alloc_fail_after(-1);
    odin3_sim_destroy(sim);
    return st == ODIN3_OK;
}

/*
 * The number of allocations a build of a chain of n_cells makes (the least `allowed` with which
 * it succeeds: exponential then binary search), after checking the chain simulates.
 */
static long build_allocations(uint32_t n_cells) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    odin3_module *top = make_chain(n_cells);
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(n_cells, sim->n_cells);
    TEST_ASSERT_EQUAL_UINT32(1, settle(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(0, settle(sim, 0));
    odin3_sim_destroy(sim);
    long low = 0; /* fails with low allocations */
    long high = 1;
    while (!builds_within(top, high)) {
        low = high;
        high *= 2;
        TEST_ASSERT_TRUE(high < OOM_LIMIT);
    }
    while (high - low > 1) {
        long mid = low + (high - low) / 2;
        *(builds_within(top, mid) ? &high : &low) = mid;
    }
    return high;
}

/*
 * Deterministic stand-in for "time is linear": 4x the cells (and 4x the instance frames) adds
 * only the few allocations of the growing arrays' doublings, so nothing is allocated per cell,
 * frame or net; the work itself is single passes over cells, pins and slots (levelize.c).
 */
static void test_200k_cells_build_linear(void) {
    long small = build_allocations(BIG / 4);
    long big = build_allocations(BIG);
    char msg[NAME_BUF * 4];
    (void)snprintf(msg, sizeof msg, "50k: %ld allocations, 200k: %ld", small, big);
    TEST_ASSERT_TRUE_MESSAGE(small < 200, msg);
    TEST_ASSERT_TRUE_MESSAGE(big >= small && big - small <= 40, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_two_level_hierarchy_matches_flat);
    RUN_TEST(test_instance_pins_share_parent_slots);
    RUN_TEST(test_feedthrough_unifies_parent_nets);
    RUN_TEST(test_unconnected_pins_use_reserved_slots);
    RUN_TEST(test_flop_cuts_loop_and_clock_is_flagged);
    RUN_TEST(test_clock_through_hierarchy_is_primary);
    RUN_TEST(test_clock_from_logic_is_rejected);
    RUN_TEST(test_latch_enable_from_logic_is_allowed);
    RUN_TEST(test_combinational_loop_names_a_net);
    RUN_TEST(test_internal_loop_names_hierarchical_net);
    RUN_TEST(test_loop_through_latch_is_rejected);
    RUN_TEST(test_black_box_is_rejected_with_location);
    RUN_TEST(test_type_without_hook_is_rejected);
    RUN_TEST(test_recursive_hierarchy_is_rejected);
    RUN_TEST(test_indirect_recursion_names_a_module_on_the_cycle);
    RUN_TEST(test_unconnected_clock_is_rejected);
    RUN_TEST(test_inout_top_port_is_rejected);
    RUN_TEST(test_bad_top_is_rejected);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_200k_cells_build_linear);
    return UNITY_END();
}
