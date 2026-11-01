/*
 * test_edit_oom.c — out of memory inside an edit call leaves no half-made object and no
 * half-moved pin (docs/specs/2026-10-31-edit-gaps-design.md, fix round item 6). Each test fails
 * every allocation of one call in turn: a failed call must change nothing, and the sweep must
 * reach a success.
 */
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"

#include <stdint.h>
#include <stdio.h>

enum { OOM_LIMIT = 400 };

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
    odin3_util_set_alloc_fail_after(-1);
    odin3_design_destroy(design);
    design = NULL;
}

static uint32_t count_of(odin3_status (*get)(const odin3_design *, uint32_t, uint32_t *)) {
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, get(design, top, &count));
    return count;
}

static void test_add_net_oom_makes_no_net(void) {
    int failures = 0;
    for (long nth = 0; nth < OOM_LIMIT; nth++) {
        uint32_t before = count_of(odin3_module_get_net_count);
        odin3_edit_new what = {sizeof what, 0, .name = "oomn", .tag = "t"};
        odin3_ref net;
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_edit_add_net(design, top, &what, &net);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_TRUE(failures > 0);
            return;
        }
        failures++;
        TEST_ASSERT_EQUAL_UINT32(before, count_of(odin3_module_get_net_count));
    }
    TEST_FAIL_MESSAGE("the sweep never reached a success");
}

static void test_add_sop_oom_makes_no_node(void) {
    int failures = 0;
    for (long nth = 0; nth < OOM_LIMIT; nth++) {
        uint32_t before = count_of(odin3_module_get_node_count);
        odin3_edit_new what = {sizeof what,  0, .name = "ooms", .tag = "t", .cover = "11 1\n",
                               .n_inputs = 2};
        odin3_ref node;
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_edit_add_sop(design, top, &what, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_TRUE(failures > 0);
            return;
        }
        failures++;
        TEST_ASSERT_EQUAL_UINT32(before, count_of(odin3_module_get_node_count));
    }
    TEST_FAIL_MESSAGE("the sweep never reached a success");
}

static odin3_ref net_named(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, name, &id));
    return (odin3_ref){top, id};
}

/* The first input pin of the .names cell, the driver of y. */
static odin3_ref names_pin(void) {
    uint32_t drv_pin = 0;
    uint32_t drv_node = 0;
    odin3_span pins;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver(design, net_named("y"), &drv_pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_get_node(design, (odin3_ref){top, drv_pin}, &drv_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, (odin3_ref){top, drv_node}, &pins));
    return (odin3_ref){top, pins.first};
}

static const char *node_events(odin3_ref pin) {
    uint32_t node = 0;
    const char *text = "unset";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_node(design, pin, &node));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_events(design, (odin3_obj){top, ODIN3_OBJ_NODE, node}, &text));
    return text;
}

/* The pin must be where it was and the node unmarked after a failed call. */
static void assert_untouched(odin3_ref pin, uint32_t net) {
    uint32_t now = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_get_net(design, pin, &now));
    TEST_ASSERT_EQUAL_UINT32(net, now);
    TEST_ASSERT_NULL(node_events(pin));
}

static void test_rewire_oom_keeps_the_pin_and_marks_nothing(void) {
    odin3_ref pin = names_pin();
    uint32_t net_a = net_named("a").id;
    for (long nth = 0; nth < OOM_LIMIT; nth++) {
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_edit_rewire_tagged(design, pin, net_named("b"), "r");
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_TRUE(nth > 0);
            TEST_ASSERT_EQUAL_STRING("rewired r\n", node_events(pin));
            return;
        }
        assert_untouched(pin, net_a);
    }
    TEST_FAIL_MESSAGE("the sweep never reached a success");
}

static void test_disconnect_oom_keeps_the_pin_and_marks_nothing(void) {
    odin3_ref pin = names_pin();
    uint32_t net_a = net_named("a").id;
    for (long nth = 0; nth < OOM_LIMIT; nth++) {
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_edit_disconnect_tagged(design, pin, "d");
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_TRUE(nth > 0);
            return;
        }
        assert_untouched(pin, net_a);
    }
    TEST_FAIL_MESSAGE("the sweep never reached a success");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_net_oom_makes_no_net);
    RUN_TEST(test_add_sop_oom_makes_no_node);
    RUN_TEST(test_rewire_oom_keeps_the_pin_and_marks_nothing);
    RUN_TEST(test_disconnect_oom_keeps_the_pin_and_marks_nothing);
    return UNITY_END();
}
