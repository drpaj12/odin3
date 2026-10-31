/*
 * test_edit.c — the netlist-editing C ABI (spec docs/specs/2026-10-31-netlist-edit-sim-design.md
 * section 2) through libodin3.so only: add, connect, rewire, set cover, remove, tags, refusals,
 * and a BLIF round trip after the edits.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static odin3_design *design;
static uint32_t top;

static const char K_BLIF[] = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n";

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    FILE *file = fopen(ODIN3_TEST_TMP ".in.blif", "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fputs(K_BLIF, file);
    (void)fclose(file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_blif",
                                                          ODIN3_TEST_TMP ".in.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_ref net_named(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, name, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id);
    return (odin3_ref){top, id};
}

static odin3_ref pin_of(odin3_ref node, uint32_t port, uint32_t bit) {
    odin3_span pins;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_pins(design, node, port, &pins));
    TEST_ASSERT_TRUE(bit < pins.count);
    return (odin3_ref){top, pins.first + bit};
}

static void test_add_net_and_sop_carry_the_tag(void) {
    odin3_ref net;
    odin3_ref sop;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, "t_q", "trojan:t1", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_add_sop(design, top, 2, "11 1\n", "t_and", "trojan:t1", &sop));
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NET, net.id}, &tag));
    TEST_ASSERT_EQUAL_STRING("trojan:t1", tag);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NODE, sop.id}, &tag));
    TEST_ASSERT_EQUAL_STRING("trojan:t1", tag);
    odin3_ref original = net_named("a");
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NET, original.id}, &tag));
    TEST_ASSERT_NULL(tag);
}

static void test_duplicate_name_and_bad_args_are_refused(void) {
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, "n1", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, "n1", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, "n2", "", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, 999, "n3", "t", &net));
    odin3_ref node;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_add_node(design, top, "$nosuch", "x", "t", &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_add_sop(design, top, 2, "1 1\n", "bad", "t", &node));
}

static void test_connect_rewire_and_driver_rule(void) {
    odin3_ref sop;
    odin3_ref out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, "t_q", "t", &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_sop(design, top, 2, "10 1\n", "g", "t", &sop));
    odin3_ref in0 = pin_of(sop, 0, 0);
    odin3_ref in1 = pin_of(sop, 0, 1);
    odin3_ref drv = pin_of(sop, 1, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, in0, net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, in1, net_named("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, drv, out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_connect(design, drv, net_named("y")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire(design, in1, net_named("a")));
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, in1, &got));
    TEST_ASSERT_EQUAL_UINT32(net_named("a").id, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_disconnect(design, in1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, in1, &got));
    TEST_ASSERT_EQUAL_UINT32(0, got);
}

static void test_set_cover_keeps_pins_and_name(void) {
    odin3_ref sop;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_sop(design, top, 2, "11 1\n", "g", "t", &sop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(sop, 0, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_set_cover(design, sop, "00 1\n", "t2"));
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(design, top, "g", &node));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node);
    odin3_ref fresh = {top, node};
    uint32_t net = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, pin_of(fresh, 0, 0), &net));
    TEST_ASSERT_EQUAL_UINT32(net_named("a").id, net);
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NODE, node}, &tag));
    TEST_ASSERT_EQUAL_STRING("t2", tag);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_set_cover(design, fresh, "0 1\n", "t"));
}

static void test_remove_and_noop_sequence_round_trips(void) {
    odin3_ref sop;
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, "tmp", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_sop(design, top, 1, "1 1\n", "tmpg", "t", &sop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_remove_node(design, sop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_remove_net(design, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_remove_net(design, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_blif", ODIN3_TEST_TMP ".out.blif"));
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(again, "read_blif", ODIN3_TEST_TMP ".out.blif"));
    odin3_design_destroy(again);
}

static void test_edits_are_refused_while_simulating(void) {
    odin3_simulation *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_open(design, 0, &sim));
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, "x", "t", &net));
    odin3_simulation_close(sim);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, "x", "t", &net));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_net_and_sop_carry_the_tag);
    RUN_TEST(test_duplicate_name_and_bad_args_are_refused);
    RUN_TEST(test_connect_rewire_and_driver_rule);
    RUN_TEST(test_set_cover_keeps_pins_and_name);
    RUN_TEST(test_remove_and_noop_sequence_round_trips);
    RUN_TEST(test_edits_are_refused_while_simulating);
    return UNITY_END();
}
