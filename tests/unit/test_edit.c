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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "read_blif", ODIN3_TEST_TMP ".in.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_status add_net(uint32_t module, const char *name, const char *tag, odin3_ref *out) {
    odin3_edit_new what = {sizeof what, 0, .name = name, .tag = tag};
    return odin3_edit_add_net(design, module, &what, out);
}

static odin3_status add_node(const char *type, const char *name, const char *tag, odin3_ref *out) {
    odin3_edit_new what = {sizeof what, 0, .name = name, .tag = tag, .cell_type = type};
    return odin3_edit_add_node(design, top, &what, out);
}

static odin3_status add_sop(uint32_t n_inputs, const char *cover, const char *name, const char *tag,
                            odin3_ref *out) {
    odin3_edit_new what = {sizeof what,         0, .name = name, .tag = tag, .cover = cover,
                           .n_inputs = n_inputs};
    return odin3_edit_add_sop(design, top, &what, out);
}

static odin3_ref last_twin;

static odin3_status set_cover(odin3_ref node, const char *cover, const char *tag) {
    odin3_edit_new what = {sizeof what, 0, .tag = tag, .cover = cover};
    return odin3_edit_set_cover(design, node, &what, &last_twin);
}

static odin3_ref net_named(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, name, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id);
    return (odin3_ref){top, id};
}

/* Pin `index` of the node, in port order then bit order. */
static odin3_ref pin_of(odin3_ref node, uint32_t index) {
    odin3_span pins;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, node, &pins));
    TEST_ASSERT_TRUE(index < pins.count);
    return (odin3_ref){top, pins.first + index};
}

static void test_add_net_and_sop_carry_the_tag(void) {
    odin3_ref net;
    odin3_ref sop;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_net(top, "t_q", "trojan:t1", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_sop(2, "11 1\n", "t_and", "trojan:t1", &sop));
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_net(top, "n1", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_net(top, "n1", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_net(top, "n2", "", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_net(999, "n3", "t", &net));
    odin3_ref node;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_node("$nosuch", "x", "t", &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_sop(2, "1 1\n", "bad", "t", &node));
}

static void test_connect_rewire_and_driver_rule(void) {
    odin3_ref sop;
    odin3_ref out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_net(top, "t_q", "t", &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_sop(2, "10 1\n", "g", "t", &sop));
    odin3_ref in0 = pin_of(sop, 0);
    odin3_ref in1 = pin_of(sop, 1);
    odin3_ref drv = pin_of(sop, 2);
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_sop(2, "11 1\n", "g", "t", &sop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(sop, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_cover(sop, "00 1\n", "t2"));
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(design, top, "g", &node));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node);
    odin3_ref fresh = {top, node};
    uint32_t net = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, pin_of(fresh, 0), &net));
    TEST_ASSERT_EQUAL_UINT32(net_named("a").id, net);
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NODE, node}, &tag));
    TEST_ASSERT_EQUAL_STRING("t2", tag);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, set_cover(fresh, "0 1\n", "t"));
}

static void test_remove_and_noop_sequence_round_trips(void) {
    odin3_ref sop;
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_net(top, "tmp", "t", &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_sop(1, "1 1\n", "tmpg", "t", &sop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_remove_node(design, sop, 0));
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
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, add_net(top, "x", "t", &net));
    odin3_simulation_close(sim);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_net(top, "x", "t", &net));
}

static const char K_SEQ[] = ".model s\n.inputs clk en\n.outputs q\n.names en q d\n10 1\n01 1\n"
                            ".latch d q re clk 0\n.end\n";

/* Net visibility without keep, run_vectors and toggle counts on a BLIF with a latch. */
static void test_sequences_net_visibility_and_toggles(void) {
    FILE *file = fopen(ODIN3_TEST_TMP ".seq.blif", "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fputs(K_SEQ, file);
    (void)fclose(file);
    odin3_design *seq = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(seq, "read_blif", ODIN3_TEST_TMP ".seq.blif"));
    odin3_simulation *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_open(seq, 0, &sim));
    uint32_t en = 0;
    uint32_t sig_q = 0;
    uint32_t sig_d = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "en", &en));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "q", &sig_q));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "d", &sig_d));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sig_d);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_set_toggle_counting(sim, 1));
    const uint64_t in_values[] = {1, 1, 0, 1};
    const uint32_t outs[] = {sig_q, sig_d};
    uint64_t out_values[8] = {0};
    odin3_simulation_run run = {.size = sizeof run,
                                .in_signals = &en,
                                .n_in = 1,
                                .in_values = in_values,
                                .n_vectors = 4,
                                .out_signals = outs,
                                .n_out = 2,
                                .out_values = out_values};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_run_vectors(sim, &run));
    const uint64_t want[] = {1, 0, 0, 1, 0, 0, 1, 0};
    TEST_ASSERT_EQUAL_UINT64_ARRAY(want, out_values, 8);
    uint64_t toggles = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_q, &toggles));
    TEST_ASSERT_EQUAL_UINT64(3, toggles);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_d, &toggles));
    TEST_ASSERT_EQUAL_UINT64(2, toggles);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_reset(sim));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_q, &toggles));
    TEST_ASSERT_EQUAL_UINT64(0, toggles);
    odin3_simulation_close(sim);
    odin3_design_destroy(seq);
}

/* The node driving the net `name` of the top module. */
static odin3_ref driver_node(const char *name) {
    uint32_t pin = 0;
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver(design, net_named(name), &pin));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, pin);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_node(design, (odin3_ref){top, pin}, &node));
    return (odin3_ref){top, node};
}

typedef struct first_src {
    uint32_t n;
    uint32_t line;
    char file[256];
} first_src;

static void note_src(const odin3_source *src, void *user) {
    first_src *got = user;
    if (got->n++ == 0) {
        got->line = src->line;
        (void)snprintf(got->file, sizeof got->file, "%s", src->file);
    }
}

static first_src first_source(odin3_ref node) {
    first_src got = {0, 0, ""};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_visit_sources(design, (odin3_obj){top, ODIN3_OBJ_NODE, node.id},
                                           note_src, &got));
    return got;
}

typedef struct parents {
    uint32_t n;
    uint32_t ids[4];
} parents;

static void note_parent(uint32_t id, void *user) {
    parents *got = user;
    if (got->n < 4) {
        got->ids[got->n] = id;
    }
    got->n++;
}

/* A twin keeps the old node's HDL ancestry: a DERIVED record over the old record and the edit. */
static void test_twin_provenance_reaches_original_source(void) {
    odin3_ref old = driver_node("y");
    first_src before = first_source(old);
    TEST_ASSERT_TRUE(before.n > 0);
    TEST_ASSERT_TRUE(before.line > 0);
    uint32_t old_rec = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_obj_get_prov(design, (odin3_obj){top, ODIN3_OBJ_NODE, old.id}, &old_rec));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_attr_set_string(design, (odin3_obj){top, ODIN3_OBJ_NODE, old.id}, "note", "kept"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_cover(old, "00 1\n", "trojan:t9"));
    odin3_ref twin = last_twin;
    TEST_ASSERT_NOT_EQUAL_UINT32(old.id, twin.id);
    first_src after = first_source(twin);
    TEST_ASSERT_EQUAL_UINT32(before.n, after.n);
    TEST_ASSERT_EQUAL_UINT32(before.line, after.line);
    TEST_ASSERT_EQUAL_STRING(before.file, after.file);
    uint32_t rec = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_obj_get_prov(design, (odin3_obj){top, ODIN3_OBJ_NODE, twin.id}, &rec));
    odin3_prov_info info;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_get_info(design, rec, &info));
    TEST_ASSERT_EQUAL_UINT32(2, info.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_REWRITE, info.dir);
    parents par = {0, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_parents(design, rec, note_parent, &par));
    TEST_ASSERT_EQUAL_UINT32(2, par.n);
    TEST_ASSERT_EQUAL_UINT32(old_rec, par.ids[0]);
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_tag(design, (odin3_obj){top, ODIN3_OBJ_NODE, twin.id}, &tag));
    TEST_ASSERT_EQUAL_STRING("trojan:t9", tag);
    const char *note = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_attr_get_string(design, (odin3_obj){top, ODIN3_OBJ_NODE, twin.id}, "note", &note));
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_STRING("kept", note);
}

/* set_param_int returns the twin through the out parameter. */
static void test_set_param_int_returns_twin(void) {
    odin3_ref sop;
    odin3_ref twin = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, add_sop(2, "11 1\n", "p", "t", &sop));
    odin3_edit_new what = {sizeof what, 0, .tag = "t3", .index = 1, .value = 5};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_set_param_int(design, sop, &what, &twin));
    TEST_ASSERT_EQUAL_UINT32(0, twin.id);
    what.index = 0;
    what.value = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_set_param_int(design, sop, &what, &twin));
    TEST_ASSERT_NOT_EQUAL_UINT32(sop.id, twin.id);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, twin.id);
}

static void test_struct_size_and_reserved_are_checked(void) {
    odin3_ref net;
    odin3_edit_new what = {sizeof what, 0, .name = "q1", .tag = "t"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, &what, &net));
    what.name = "q2";
    what.reserved = 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, &what, &net));
    what.reserved = 0;
    what.size = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, &what, &net));
    what.size = 63; /* below the first release's 64-byte struct */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, &what, &net));
    uint32_t found = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, "q1", &found));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, "q2", &found));
    TEST_ASSERT_EQUAL_UINT32(0, found);
}

/* Removing a driver of a net with other pins is refused unless the caller allows it. */
static void test_remove_driver_needs_the_flag(void) {
    odin3_ref drv = driver_node("y");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_remove_node(design, drv, 0));
    uint32_t drivers = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver_count(design, net_named("y"), &drivers));
    TEST_ASSERT_EQUAL_UINT32(1, drivers);
    odin3_status st = odin3_edit_remove_node(design, drv, 1);
#ifdef NDEBUG
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st); /* release: no check, the net is simply undriven */
#else
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_CHECK); /* debug: the check may object */
#endif
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver_count(design, net_named("y"), &drivers));
    TEST_ASSERT_EQUAL_UINT32(0, drivers); /* applied in both builds */
}

/* run_vectors rejects a bad size or reserved; hand-computed toggles of a combinational net. */
static void test_run_vectors_struct_and_hand_toggles(void) {
    odin3_simulation *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_open(design, 0, &sim));
    uint32_t sig_a = 0;
    uint32_t sig_y = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "a", &sig_a));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "y", &sig_y));
    uint32_t sig_b = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(sim, "b", &sig_b));
    const uint32_t ins[] = {sig_a, sig_b};
    const uint64_t vals[] = {1, 1, 1, 0, 1, 1, 0, 1};
    uint64_t outv[4] = {9, 9, 9, 9};
    odin3_simulation_run run = {.size = sizeof run,
                                .in_signals = ins,
                                .n_in = 2,
                                .in_values = vals,
                                .n_vectors = 4,
                                .out_signals = &sig_y,
                                .n_out = 1,
                                .out_values = outv};
    run.reserved = 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_run_vectors(sim, &run));
    run.reserved = 0;
    run.size = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_run_vectors(sim, &run));
    run.size = sizeof run;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_set_toggle_counting(sim, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_run_vectors(sim, &run));
    const uint64_t want[] = {1, 0, 1, 0}; /* y = a & b */
    TEST_ASSERT_EQUAL_UINT64_ARRAY(want, outv, 4);
    uint64_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_y, &count));
    TEST_ASSERT_EQUAL_UINT64(4, count); /* 0 -> 1 -> 0 -> 1 -> 0 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_a, &count));
    TEST_ASSERT_EQUAL_UINT64(2, count); /* 0 -> 1 -> 1 -> 1 -> 0 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_toggles(sim, sig_b, &count));
    TEST_ASSERT_EQUAL_UINT64(3, count); /* 0 -> 1 -> 0 -> 1 -> 1 */
    odin3_simulation_close(sim);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_net_and_sop_carry_the_tag);
    RUN_TEST(test_duplicate_name_and_bad_args_are_refused);
    RUN_TEST(test_connect_rewire_and_driver_rule);
    RUN_TEST(test_set_cover_keeps_pins_and_name);
    RUN_TEST(test_remove_and_noop_sequence_round_trips);
    RUN_TEST(test_edits_are_refused_while_simulating);
    RUN_TEST(test_sequences_net_visibility_and_toggles);
    RUN_TEST(test_twin_provenance_reaches_original_source);
    RUN_TEST(test_set_param_int_returns_twin);
    RUN_TEST(test_struct_size_and_reserved_are_checked);
    RUN_TEST(test_remove_driver_needs_the_flag);
    RUN_TEST(test_run_vectors_struct_and_hand_toggles);
    return UNITY_END();
}
