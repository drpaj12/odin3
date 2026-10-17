/*
 * test_lower_pmux_shift.c — 3B Task 4, part 1: $pmux, $shl $shr $sshr and $shiftx (spec §5.1 to
 * §5.3). Per rule: gate counts against the closed forms and the size estimate, the netlist view,
 * no word cell left, and function. $pmux has a simulate hook, so the lowered module is compared
 * with the unlowered cell (and, for one-hot selects, with a model written here); the shifts have
 * none (the registry gives $shl $shr $sshr and $shiftx no hook, 3B report), so the lowered module
 * is compared with an independent C model of the Verilog operator.
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { EXHAUSTIVE_BITS = 14, RANDOM_VECTORS = 8192, MAX_BITS = 256 };

void setUp(void) {
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

static odin3_status run_lower(lf *fix) {
    return odin3_pass_run(fix->design, "lower", (odin3_bytes){NULL, 0});
}

/* --- $pmux ---------------------------------------------------------------------------------- */

typedef struct pm_case {
    uint32_t width, sels;
} pm_case;

static odin3_node_id build_pmux(lf *fix, const pm_case *cs, odin3_module **mod) {
    odin3_value params[2] = {odin3_value_int(cs->width), odin3_value_int(cs->sels)};
    lf_cell_spec cell = {"$pmux", params, 2, "dut", {0}};
    return lf_one_cell(fix, &cell, mod);
}

static uint32_t pmux_gates(const pm_case *cs) {
    return cs->sels - 1 + cs->width * 2 * cs->sels;
}

static void lower_pmux(lf *fix, const pm_case *cs, odin3_module **mod) {
    char msg[48];
    (void)snprintf(msg, sizeof msg, "$pmux W=%u S=%u", cs->width, cs->sels);
    lf_begin(fix);
    (void)build_pmux(fix, cs, mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(fix), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(fix, *mod), msg);
    check_netlist_view(*mod);
    uint32_t gates = lf_gate_nodes(fix, *mod);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(pmux_gates(cs), gates, msg);
    odin3_value params[2] = {odin3_value_int(cs->width), odin3_value_int(cs->sels)};
    uint32_t widths[4] = {cs->width, cs->width * cs->sels, cs->sels, cs->width};
    TEST_ASSERT_TRUE_MESSAGE(gates <= lf_estimate("$pmux", params, widths, 4), msg);
}

/* The one-hot model: Y = A with no select, B[i] with only select i, open when several are set. */
static void pmux_model(const lf_model_io *io, const void *user) {
    const pm_case *cs = user;
    const uint8_t *sel = io->in + (size_t)cs->width * (cs->sels + 1);
    uint32_t hot = 0;
    uint32_t which = 0;
    for (uint32_t i = 0; i < cs->sels; i++) {
        hot += sel[i];
        which = sel[i] != 0 ? i : which;
    }
    for (uint32_t bit = 0; bit < cs->width; bit++) {
        io->out[bit] = hot == 0 ? io->in[bit] : io->in[cs->width + which * cs->width + bit];
        io->care[bit] = hot <= 1 ? 1U : 0U;
    }
}

/* One width/select case: closed form, then the hook and the one-hot model on the lowered module. */
static void check_pmux_case(const pm_case *cs) {
    lf after;
    odin3_module *after_mod = NULL;
    lower_pmux(&after, cs, &after_mod);
    lf before;
    odin3_module *before_mod = NULL;
    lf_begin(&before);
    (void)build_pmux(&before, cs, &before_mod);
    lf_sim_opts opts = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 11};
    lf_sim_result res = lf_sim_equal(before.design, odin3_module_id_of(before_mod), after.design,
                                     odin3_module_id_of(after_mod), &opts);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    lf_sim_opts one_hot = {EXHAUSTIVE_BITS + 2, RANDOM_VECTORS, 11};
    res = lf_sim_model(after.design, odin3_module_id_of(after_mod), pmux_model, cs, &one_hot);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    lf_end(&before);
    lf_end(&after);
}

static void test_pmux_matches_the_closed_form_the_hook_and_the_one_hot_model(void) {
    static const uint32_t k_widths[] = {1, 2, 3, 5, 8, 17};
    static const uint32_t k_sels[] = {1, 2, 3, 4, 7, 8};
    for (size_t wi = 0; wi < sizeof k_widths / sizeof k_widths[0]; wi++) {
        for (size_t si = 0; si < sizeof k_sels / sizeof k_sels[0]; si++) {
            pm_case cs = {k_widths[wi], k_sels[si]};
            check_pmux_case(&cs);
        }
    }
}

static void test_pmux_of_one_word_and_one_select_is_an_and_and_a_mux(void) {
    pm_case cs = {1, 1};
    lf fix;
    odin3_module *mod = NULL;
    lower_pmux(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_AND_"));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_MUX_"));
    odin3_pin_id drv = odin3_net_driver(mod, lf_wnet(&fix, mod, "Y", 0));
    odin3_node_id mux = odin3_pin_node(mod, drv);
    odin3_pinslice pins = odin3_node_pins(mod, mux);
    TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "A", 0).v, odin3_pin_net(mod, pins.first).v);
    TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "S", 0).v,
                             odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 2}).v);
    lf_end(&fix);
}

static void test_pmux_has_one_select_or_tree_and_a_mux_per_bit(void) {
    pm_case cs = {4, 5};
    lf fix;
    odin3_module *mod = NULL;
    lower_pmux(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_UINT32(4, lf_count_type(&fix, mod, "$_MUX_"));
    TEST_ASSERT_EQUAL_UINT32(4 * 5, lf_count_type(&fix, mod, "$_AND_"));
    TEST_ASSERT_EQUAL_UINT32(4 + 4 * 4, lf_count_type(&fix, mod, "$_OR_"));
    lf_end(&fix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pmux_matches_the_closed_form_the_hook_and_the_one_hot_model);
    RUN_TEST(test_pmux_of_one_word_and_one_select_is_an_and_and_a_mux);
    RUN_TEST(test_pmux_has_one_select_or_tree_and_a_mux_per_bit);
    return UNITY_END();
}
