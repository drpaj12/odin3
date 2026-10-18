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

static uint32_t clog2(uint32_t value) {
    uint32_t bits = 0;
    while (((uint64_t)1 << bits) < value) {
        bits++;
    }
    return bits;
}

static uint32_t max_of(uint32_t lhs, uint32_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

static uint32_t min_of(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? lhs : rhs;
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

/* --- the shifts ----------------------------------------------------------------------------- */

typedef struct sh_case {
    const char *type;
    uint32_t aw, bw, yw;
    bool a_signed, b_signed;
} sh_case;

enum { AMOUNT_HUGE = 1 << 24, AMOUNT_BITS = 24 };

/* The value of bits[0 .. n) as an unsigned number, saturated at AMOUNT_HUGE. */
static uint64_t amount_of(const uint8_t *bits, uint32_t count) {
    uint64_t amount = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (bits[i] != 0) {
            amount = i >= AMOUNT_BITS ? AMOUNT_HUGE : amount | ((uint64_t)1 << i);
        }
    }
    return amount;
}

/* The value of bits[0 .. n) as a two's complement number (n <= 32). */
static int64_t signed_of(const uint8_t *bits, uint32_t count) {
    int64_t value = (int64_t)amount_of(bits, count);
    if (bits[count - 1] != 0) {
        value -= (int64_t)1 << count;
    }
    return value;
}

static odin3_node_id build_shift(lf *fix, const sh_case *cs, odin3_module **mod) {
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {cs->type, params, 5, "dut", {0}};
    return lf_one_cell(fix, &cell, mod);
}

static void shift_params(const sh_case *cs, odin3_value params[5]) {
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
}

/*
 * The Verilog operator, not the gates: A widened to max(A, Y) bits (sign-extended when signed),
 * shifted by the unsigned value of B; $shr and $shl fill with 0, $sshr of a signed A with A's top.
 */
static void shift_buffer(const sh_case *cs, const uint8_t *in, uint8_t *buf) {
    uint32_t width = max_of(cs->aw, cs->yw);
    for (uint32_t k = 0; k < width; k++) {
        buf[k] = k < cs->aw ? in[k] : 0U;
        buf[k] = k >= cs->aw && cs->a_signed ? in[cs->aw - 1] : buf[k];
    }
}

static void shift_model(const lf_model_io *io, const void *user) {
    const sh_case *cs = user;
    uint32_t width = max_of(cs->aw, cs->yw);
    uint8_t buf[MAX_BITS];
    shift_buffer(cs, io->in, buf);
    uint64_t amount = amount_of(io->in + cs->aw, cs->bw);
    bool left = strcmp(cs->type, "$shl") == 0;
    bool arithmetic = strcmp(cs->type, "$sshr") == 0 && cs->a_signed;
    uint8_t fill = arithmetic ? buf[width - 1] : 0U;
    for (uint32_t k = 0; k < cs->yw; k++) {
        if (left) {
            io->out[k] = amount <= k ? buf[k - amount] : 0U;
        } else {
            io->out[k] = amount + k < width ? buf[k + amount] : fill;
        }
    }
}

/* The gate count of techmap.v's shift map, from the widths. */
static uint32_t shift_gates(const sh_case *cs) {
    uint32_t width = max_of(cs->aw, cs->yw);
    uint32_t span = cs->aw;
    if (cs->a_signed) {
        span = width;
    }
    if (strcmp(cs->type, "$shl") == 0) {
        span = cs->yw;
    }
    uint32_t bits = min_of(clog2(span) + 1, cs->bw);
    uint32_t tree = cs->bw > bits ? cs->bw - bits - 1 : 0;
    return tree + width * (bits + 1);
}

/* What a lowered cell must satisfy: its gate count and the estimate that bounds it. */
typedef struct expected {
    uint32_t gates;
    uint64_t estimate;
} expected;

static void check_gates_and_view(lf *fix, odin3_module *mod, const expected *want,
                                 const char *msg) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(fix, mod), msg);
    check_netlist_view(mod);
    uint32_t gates = lf_gate_nodes(fix, mod);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want->gates, gates, msg);
    TEST_ASSERT_TRUE_MESSAGE(gates <= want->estimate, msg);
}

static void check_shift_case(const sh_case *cs) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "%s a=%u b=%u y=%u signed=%d", cs->type, cs->aw, cs->bw, cs->yw,
                   cs->a_signed ? 1 : 0);
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_shift(&fix, cs, &mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(&fix), msg);
    odin3_value params[5];
    shift_params(cs, params);
    uint32_t widths[3] = {cs->aw, cs->bw, cs->yw};
    expected want = {shift_gates(cs), lf_estimate(cs->type, params, widths, 3)};
    check_gates_and_view(&fix, mod, &want, msg);
    lf_sim_opts opts = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 5};
    lf_sim_result res = lf_sim_model(fix.design, odin3_module_id_of(mod), shift_model, cs, &opts);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    TEST_ASSERT_TRUE_MESSAGE(res.vectors > 0, msg);
    lf_end(&fix);
}

static void test_shifts_match_the_operator_model_and_the_closed_form(void) {
    static const char *const k_types[] = {"$shl", "$shr", "$sshr"};
    static const uint32_t k_a[] = {1, 2, 3, 5, 8, 17};
    static const uint32_t k_b[] = {1, 2, 3, 4, 6};
    for (size_t ti = 0; ti < 3; ti++) {
        for (size_t ai = 0; ai < sizeof k_a / sizeof k_a[0]; ai++) {
            for (size_t bi = 0; bi < sizeof k_b / sizeof k_b[0]; bi++) {
                for (int sign = 0; sign < 2; sign++) {
                    sh_case cs = {k_types[ti], k_a[ai], k_b[bi], k_a[ai], sign != 0, sign != 0};
                    check_shift_case(&cs);
                    cs.yw = k_a[ai] + 3; /* a result wider than A */
                    check_shift_case(&cs);
                    cs.yw = k_a[ai] > 2 ? k_a[ai] - 1 : 1; /* narrower */
                    check_shift_case(&cs);
                }
            }
        }
    }
}

static void test_shift_node_counts_are_pinned(void) {
    static const struct {
        sh_case cs;
        uint32_t gates;
    } k_pins[] = {
        {{"$shl", 8, 3, 8, false, false}, 32}, /* BB 3, no overflow: 8 + 3 * 8 */
        {{"$shr", 8, 4, 8, false, false}, 40}, /* BB 4 */
        {{"$sshr", 5, 6, 5, true, true}, 26},  /* BB 4, overflow OR of 2 bits: 1 + 5 + 4 * 5 */
        {{"$shl", 1, 1, 1, false, false}, 2},  /* BB 1: one overflow mux, one stage mux */
    };
    for (size_t i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(k_pins[i].gates, shift_gates(&k_pins[i].cs));
        lf fix;
        odin3_module *mod = NULL;
        lf_begin(&fix);
        (void)build_shift(&fix, &k_pins[i].cs, &mod);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
        TEST_ASSERT_EQUAL_UINT32(k_pins[i].gates, lf_gate_nodes(&fix, mod));
        lf_end(&fix);
    }
}

static void test_a_shift_by_a_wide_b_ors_the_high_bits_into_one_overflow_net(void) {
    sh_case cs = {"$shr", 3, 6, 3, false, false}; /* BB = clog2(3) + 1 = 3: B[3..5] overflow */
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_shift(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(2, lf_count_type(&fix, mod, "$_OR_")); /* B[3] | B[4] | B[5] */
    TEST_ASSERT_EQUAL_UINT32(3 + 3 * 3, lf_count_type(&fix, mod, "$_MUX_"));
    lf_end(&fix);
}

/* --- $shiftx -------------------------------------------------------------------------------- */

/* Y[k] = A[k + B]: B signed when B_SIGNED; a bit outside A is open (x). */
static void shiftx_model(const lf_model_io *io, const void *user) {
    const sh_case *cs = user;
    int64_t offset = cs->b_signed ? signed_of(io->in + cs->aw, cs->bw)
                                  : (int64_t)amount_of(io->in + cs->aw, cs->bw);
    for (uint32_t k = 0; k < cs->yw; k++) {
        int64_t idx = (int64_t)k + offset;
        bool inside = idx >= 0 && idx < (int64_t)cs->aw;
        io->out[k] = inside ? io->in[idx] : 0U;
        io->care[k] = inside ? 1U : 0U;
    }
}

static uint32_t shiftx_gates(const sh_case *cs) {
    uint32_t wide = max_of(cs->aw, cs->yw);
    uint32_t bits = min_of(clog2(wide) + (cs->b_signed ? 2U : 1U), cs->bw);
    uint32_t width = wide + (cs->b_signed ? 1U << (bits - 1) : 0U);
    uint32_t extra = cs->bw > bits ? cs->bw - bits : 0;
    uint32_t tree = 0;
    uint32_t layers = bits;
    if (extra != 0) {
        tree = cs->b_signed ? 2 * extra - 1 : extra - 1;
        layers++;
    }
    return tree + width * layers;
}

static void check_shiftx_case(const sh_case *cs) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "$shiftx a=%u b=%u y=%u bsigned=%d", cs->aw, cs->bw, cs->yw,
                   cs->b_signed ? 1 : 0);
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_shift(&fix, cs, &mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(&fix), msg);
    odin3_value params[5];
    shift_params(cs, params);
    uint32_t widths[3] = {cs->aw, cs->bw, cs->yw};
    expected want = {shiftx_gates(cs), lf_estimate("$shiftx", params, widths, 3)};
    check_gates_and_view(&fix, mod, &want, msg);
    lf_sim_opts opts = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 9};
    lf_sim_result res = lf_sim_model(fix.design, odin3_module_id_of(mod), shiftx_model, cs, &opts);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    lf_end(&fix);
}

static void test_shiftx_matches_the_offset_model_and_the_closed_form(void) {
    static const uint32_t k_a[] = {1, 2, 3, 5, 8};
    static const uint32_t k_b[] = {1, 2, 3, 5, 6};
    for (size_t ai = 0; ai < sizeof k_a / sizeof k_a[0]; ai++) {
        for (size_t bi = 0; bi < sizeof k_b / sizeof k_b[0]; bi++) {
            for (int sign = 0; sign < 2; sign++) {
                sh_case cs = {"$shiftx", k_a[ai], k_b[bi], 1, false, sign != 0};
                check_shiftx_case(&cs);
                cs.yw = k_a[ai];
                check_shiftx_case(&cs);
                cs.yw = k_a[ai] + 2;
                check_shiftx_case(&cs);
            }
        }
    }
}

static void test_shiftx_writes_x_constants_and_only_this_rule_does(void) {
    sh_case cs = {"$shiftx", 4, 3, 2, false, true};
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_shift(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_CONSTX_"));
    lf_end(&fix);
    sh_case plain = {"$shr", 4, 3, 2, false, false};
    lf_begin(&fix);
    (void)build_shift(&fix, &plain, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&fix, mod, "$_CONSTX_"));
    lf_end(&fix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pmux_matches_the_closed_form_the_hook_and_the_one_hot_model);
    RUN_TEST(test_pmux_of_one_word_and_one_select_is_an_and_and_a_mux);
    RUN_TEST(test_pmux_has_one_select_or_tree_and_a_mux_per_bit);
    RUN_TEST(test_shifts_match_the_operator_model_and_the_closed_form);
    RUN_TEST(test_shift_node_counts_are_pinned);
    RUN_TEST(test_a_shift_by_a_wide_b_ors_the_high_bits_into_one_overflow_net);
    RUN_TEST(test_shiftx_matches_the_offset_model_and_the_closed_form);
    RUN_TEST(test_shiftx_writes_x_constants_and_only_this_rule_does);
    return UNITY_END();
}
