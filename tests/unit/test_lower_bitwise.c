/*
 * test_lower_bitwise.c — 3B Task 3: the simplemap family (spec §4): $not, $and $or $xor $xnor,
 * $mux, $reduce_*, $logic_*, $eq, $ne. For every type, widths {1, 2, 3, 4, 5, 8, 17, 33, 64, 65},
 * unequal operand widths, signed, unsigned and mixed operands: node counts against the closed
 * forms and the size estimate, the pairing-tree shape, Y[k > 0] on the constant-0 net, the netlist
 * view, names and wire membership of Y, provenance, the mixed-sign warning and simulator-equal
 * before and after (exhaustive for total input width up to 12, 4,096 seeded random vectors above).
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/lower/lower.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 16384,
    N_WIDTHS = 10,
    EXHAUSTIVE_BITS = 12,
    RANDOM_VECTORS = 4096,
    MAX_WIDTH = 65
};

static const uint32_t k_widths[N_WIDTHS] = {1, 2, 3, 4, 5, 8, 17, 33, 64, 65};

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* --- the cases ------------------------------------------------------------------------------ */

typedef enum shape {
    SH_UNARY,
    SH_BINARY,
    SH_MUX,
    SH_REDUCE,
    SH_LOGIC_NOT,
    SH_LOGIC_BIN,
    SH_CMP
} shape;

typedef struct bw_case {
    const char *type;
    shape shape;
    uint32_t aw, bw, yw;
    bool a_signed, b_signed;
} bw_case;

/* The gate count the spec §4 closed form gives. */
static uint64_t gates_of(const bw_case *cs) {
    uint64_t wide = cs->aw > cs->bw ? cs->aw : cs->bw;
    switch (cs->shape) {
    case SH_UNARY:
    case SH_BINARY:
        return cs->yw;
    case SH_MUX:
        return cs->aw;
    case SH_REDUCE:
        return cs->aw - 1 + (strcmp(cs->type, "$reduce_xnor") == 0 ? 1U : 0U);
    case SH_LOGIC_NOT:
        return cs->aw;
    case SH_LOGIC_BIN:
        return (uint64_t)cs->aw + cs->bw - 1;
    case SH_CMP:
        return 2 * wide - 1 + (strcmp(cs->type, "$eq") == 0 ? 1U : 0U);
    }
    return 0;
}

/* Builds the one-cell module of cs in fix; returns the cell. */
static odin3_node_id build(lf *fix, const bw_case *cs, odin3_module **mod) {
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_cell_spec cell = {cs->type, params, 5, "dut", {0}};
    switch (cs->shape) {
    case SH_UNARY:
    case SH_REDUCE:
    case SH_LOGIC_NOT:
        lf_unary_params(&spec, params);
        cell.n_params = 3;
        break;
    case SH_MUX:
        params[0] = odin3_value_int(cs->aw);
        cell.n_params = 1;
        break;
    default:
        lf_binary_params(&spec, params);
        break;
    }
    return lf_one_cell(fix, &cell, mod);
}

static uint32_t input_bits(const bw_case *cs) {
    switch (cs->shape) {
    case SH_UNARY:
    case SH_REDUCE:
    case SH_LOGIC_NOT:
        return cs->aw;
    case SH_MUX:
        return 2 * cs->aw + 1;
    default:
        return cs->aw + cs->bw;
    }
}

static uint32_t gate_nodes(lf *fix, odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node)) {
            continue;
        }
        const odin3_celltype_def *def = odin3_celltype_get(fix->design, odin3_node_type(mod, node));
        if (def->gran == ODIN3_GRAN_BIT && strncmp(def->name, "$_CONST", 7) != 0) {
            count++;
        }
    }
    return count;
}

static uint32_t word_nodes(lf *fix, odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(fix->design, odin3_node_type(mod, node))->gran == ODIN3_GRAN_WORD) {
            count++;
        }
    }
    return count;
}

static uint32_t port_count(shape sh) {
    switch (sh) {
    case SH_MUX:
        return 4;
    case SH_UNARY:
    case SH_REDUCE:
    case SH_LOGIC_NOT:
        return 2;
    default:
        return 3;
    }
}

/* The estimate of the rule for cs, from the widths alone. */
static uint64_t estimate_of(const bw_case *cs) {
    static const odin3_net_id none[MAX_WIDTH * 2 + 4];
    lw_cell cell;
    memset(&cell, 0, sizeof cell);
    uint32_t widths[4] = {cs->aw, cs->bw, cs->yw, 0};
    if (cs->shape == SH_MUX) {
        widths[1] = cs->aw;
        widths[2] = 1;
        widths[3] = cs->aw;
    }
    uint32_t n_ports = port_count(cs->shape);
    if (n_ports == 2) {
        widths[1] = cs->yw;
    }
    for (uint32_t pidx = 0; pidx < n_ports; pidx++) {
        cell.port[pidx] = (lw_span){none, widths[pidx]};
    }
    cell.n_ports = n_ports;
    cell.out_port = n_ports - 1;
    const lw_rule *rule = lw_rule_find(cs->type);
    TEST_ASSERT_NOT_NULL_MESSAGE(rule, cs->type);
    return rule->estimate(&cell);
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

/* Runs `lower` on the design of fix through the pass manager (check before and after). */
static odin3_status run_lower(lf *fix) {
    return odin3_pass_run(fix->design, "lower", (odin3_bytes){NULL, 0});
}

static const char *describe(const bw_case *cs, char *buf, size_t size) {
    (void)snprintf(buf, size, "%s aw=%u bw=%u yw=%u signed=%d/%d", cs->type, cs->aw, cs->bw, cs->yw,
                   cs->a_signed ? 1 : 0, cs->b_signed ? 1 : 0);
    return buf;
}

/* Y[k] for k >= 1 is the constant-0 net, for the one-bit result cells. */
static void check_upper_bits_are_zero(lf *fix, odin3_module *mod, const bw_case *cs,
                                      const char *msg) {
    for (uint32_t k = 1; k < cs->yw; k++) {
        odin3_net_id net = lf_wnet(fix, mod, "Y", k);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_CONST_0, odin3_net_const_value(mod, net), msg);
    }
}

static bool is_flag_shape(shape sh) {
    return sh == SH_REDUCE || sh == SH_LOGIC_NOT || sh == SH_LOGIC_BIN || sh == SH_CMP;
}

/* Y keeps its nets, and Y[k > 0] of a one-bit result is the constant 0. */
static void check_outputs(lf *fix, odin3_module *mod, const bw_case *cs, const char *msg) {
    for (uint32_t bit = 0; bit < cs->yw; bit++) {
        TEST_ASSERT_TRUE_MESSAGE(odin3_net_live(mod, lf_wnet(fix, mod, "Y", bit)), msg);
    }
    if (is_flag_shape(cs->shape)) {
        check_upper_bits_are_zero(fix, mod, cs, msg);
    }
}

/* Lowers cs in a fresh design and checks everything but the simulation; returns the fixture. */
static void lower_and_check(lf *fix, const bw_case *cs, odin3_module **mod) {
    char msg[96];
    describe(cs, msg, sizeof msg);
    lf_begin(fix);
    (void)build(fix, cs, mod);
    uint32_t before_nodes = lf_live_nodes(*mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(fix), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, word_nodes(fix, *mod), msg);
    check_netlist_view(*mod);
    uint64_t gates = gate_nodes(fix, *mod);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(gates_of(cs), gates, msg);
    TEST_ASSERT_TRUE_MESSAGE(gates <= estimate_of(cs), msg);
    TEST_ASSERT_TRUE_MESSAGE(lf_live_nodes(*mod) <= before_nodes - 1 + gates + 3, msg);
    check_outputs(fix, *mod, cs, msg);
}

/* Simulator-equal: the same cell unlowered (the registry hook) against the lowered module. */
static void sim_equal(lf *after_fix, odin3_module *after_mod, const bw_case *cs) {
    char msg[96];
    describe(cs, msg, sizeof msg);
    lf before_fix;
    odin3_module *before_mod = NULL;
    lf_begin(&before_fix);
    (void)build(&before_fix, cs, &before_mod);
    lf_sim_opts opts = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 7};
    lf_sim_result res = lf_sim_equal(before_fix.design, odin3_module_id_of(before_mod),
                                     after_fix->design, odin3_module_id_of(after_mod), &opts);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    TEST_ASSERT_TRUE_MESSAGE(res.vectors > 0, msg);
    if (input_bits(cs) <= EXHAUSTIVE_BITS) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)1 << input_bits(cs), res.vectors, msg);
    }
    lf_end(&before_fix);
}

static void run_case(const bw_case *cs) {
    lf fix;
    odin3_module *mod = NULL;
    lower_and_check(&fix, cs, &mod);
    sim_equal(&fix, mod, cs);
    lf_end(&fix);
}

/* --- the sweeps ----------------------------------------------------------------------------- */

static void unary_sweep(const char *type, shape sh) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        uint32_t width = k_widths[i];
        for (int sign = 0; sign < 2; sign++) {
            bw_case cs = {type, sh, width, 0, sh == SH_UNARY ? width : 1, sign != 0, false};
            run_case(&cs);
            if (sh == SH_UNARY) { /* the result wider than the operand: extension */
                cs.yw = width + 3;
                run_case(&cs);
                if (width > 2) { /* narrower: truncation */
                    cs.yw = width - 1;
                    run_case(&cs);
                }
            } else {
                cs.yw = 4; /* a result wider than the one bit: Y[k > 0] = 0 */
                run_case(&cs);
            }
        }
    }
}

static void binary_sweep(const char *type, shape sh, uint32_t y_one) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        uint32_t width = k_widths[i];
        bw_case cs = {type, sh, width, width, y_one != 0 ? y_one : width, false, false};
        run_case(&cs);
        cs.a_signed = cs.b_signed = true;
        run_case(&cs);
    }
    static const uint32_t k_pairs[][2] = {{3, 5}, {5, 3}, {1, 4}, {8, 17}, {33, 2}, {65, 64}};
    for (size_t i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++) {
        uint32_t wa = k_pairs[i][0];
        uint32_t wb = k_pairs[i][1];
        uint32_t wide = wa > wb ? wa : wb;
        for (int sign = 0; sign < 4; sign++) { /* none, both, A only, B only */
            bw_case cs = {type,
                          sh,
                          wa,
                          wb,
                          y_one != 0 ? y_one : wide,
                          sign == 1 || sign == 2,
                          sign == 1 || sign == 3};
            run_case(&cs);
        }
    }
    if (y_one == 0) { /* results wider and narrower than the operands */
        bw_case wide_y = {type, sh, 4, 4, 9, true, true};
        run_case(&wide_y);
        bw_case narrow_y = {type, sh, 6, 6, 3, true, true};
        run_case(&narrow_y);
        bw_case mixed_y = {type, sh, 3, 6, 9, true, false};
        run_case(&mixed_y);
    }
}

static void test_not_sweep(void) {
    unary_sweep("$not", SH_UNARY);
}
static void test_and_sweep(void) {
    binary_sweep("$and", SH_BINARY, 0);
}
static void test_or_sweep(void) {
    binary_sweep("$or", SH_BINARY, 0);
}
static void test_xor_sweep(void) {
    binary_sweep("$xor", SH_BINARY, 0);
}
static void test_xnor_sweep(void) {
    binary_sweep("$xnor", SH_BINARY, 0);
}

static void test_mux_sweep(void) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        bw_case cs = {"$mux", SH_MUX, k_widths[i], k_widths[i], k_widths[i], false, false};
        run_case(&cs);
    }
}

static void test_reduce_sweeps(void) {
    static const char *const k_types[] = {"$reduce_and", "$reduce_or", "$reduce_xor",
                                          "$reduce_xnor", "$reduce_bool"};
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        unary_sweep(k_types[i], SH_REDUCE);
    }
}

static void test_logic_not_sweep(void) {
    unary_sweep("$logic_not", SH_LOGIC_NOT);
}

static void test_logic_binary_sweeps(void) {
    binary_sweep("$logic_and", SH_LOGIC_BIN, 1);
    binary_sweep("$logic_or", SH_LOGIC_BIN, 1);
}

static void test_eq_ne_sweeps(void) {
    binary_sweep("$eq", SH_CMP, 1);
    binary_sweep("$ne", SH_CMP, 1);
}

/* --- structure ------------------------------------------------------------------------------ */

/* The gate driving the net of Y[0], and the types and operands of the tree below it. */
static const char *driver_type(lf *fix, odin3_module *mod, odin3_net_id net) {
    odin3_pin_id pin = odin3_net_driver(mod, net);
    if (!odin3_pin_valid(pin)) {
        return "none";
    }
    return odin3_celltype_get(fix->design, odin3_node_type(mod, odin3_pin_node(mod, pin)))->name;
}

static void test_a_five_bit_reduce_or_is_the_carried_odd_bit_tree(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$reduce_or", SH_REDUCE, 5, 0, 1, false, false};
    lower_and_check(&fix, &cs, &mod);
    odin3_net_id root = lf_wnet(&fix, mod, "Y", 0);
    TEST_ASSERT_EQUAL_STRING("$_OR_", driver_type(&fix, mod, root));
    odin3_pin_id drv = odin3_net_driver(mod, root);
    odin3_pinslice pins = odin3_node_pins(mod, odin3_pin_node(mod, drv));
    odin3_net_id left = odin3_pin_net(mod, pins.first);
    odin3_net_id right = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 1});
    /* root = OR(OR(OR(a0,a1), OR(a2,a3)), a4): the odd bit a4 is carried to the root */
    TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "A", 4).v, right.v);
    TEST_ASSERT_EQUAL_STRING("$_OR_", driver_type(&fix, mod, left));
    lf_end(&fix);
}

static void test_a_one_bit_reduction_creates_no_node_and_y_is_a(void) {
    static const char *const k_types[] = {"$reduce_and", "$reduce_or", "$reduce_xor",
                                          "$reduce_bool"};
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        lf fix;
        odin3_module *mod = NULL;
        bw_case cs = {k_types[i], SH_REDUCE, 1, 0, 1, false, false};
        lower_and_check(&fix, &cs, &mod);
        TEST_ASSERT_EQUAL_UINT32(0, gate_nodes(&fix, mod));
        TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "A", 0).v, lf_wnet(&fix, mod, "Y", 0).v);
        lf_end(&fix);
    }
}

static void test_a_one_bit_reduce_xnor_is_one_inverter(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$reduce_xnor", SH_REDUCE, 1, 0, 1, false, false};
    lower_and_check(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_UINT32(1, gate_nodes(&fix, mod));
    TEST_ASSERT_EQUAL_STRING("$_NOT_", driver_type(&fix, mod, lf_wnet(&fix, mod, "Y", 0)));
    lf_end(&fix);
}

static void test_eq_is_xors_then_an_or_tree_then_a_not_and_ne_has_no_not(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case eq = {"$eq", SH_CMP, 5, 5, 1, false, false};
    lower_and_check(&fix, &eq, &mod);
    TEST_ASSERT_EQUAL_UINT32(5, lf_count_type(&fix, mod, "$_XOR_"));
    TEST_ASSERT_EQUAL_UINT32(4, lf_count_type(&fix, mod, "$_OR_"));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_STRING("$_NOT_", driver_type(&fix, mod, lf_wnet(&fix, mod, "Y", 0)));
    lf_end(&fix);
    bw_case ne = {"$ne", SH_CMP, 5, 5, 1, false, false};
    lower_and_check(&fix, &ne, &mod);
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&fix, mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_STRING("$_OR_", driver_type(&fix, mod, lf_wnet(&fix, mod, "Y", 0)));
    lf_end(&fix);
}

static void test_logic_and_ors_both_operands_then_ands(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$logic_and", SH_LOGIC_BIN, 3, 5, 1, false, false};
    lower_and_check(&fix, &cs, &mod);
    TEST_ASSERT_EQUAL_UINT32(2 + 4, lf_count_type(&fix, mod, "$_OR_"));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_AND_"));
    TEST_ASSERT_EQUAL_STRING("$_AND_", driver_type(&fix, mod, lf_wnet(&fix, mod, "Y", 0)));
    lf_end(&fix);
}

static void test_bitwise_cell_makes_one_gate_of_its_type_per_bit(void) {
    static const struct {
        const char *type, *gate;
    } k_map[] = {{"$and", "$_AND_"}, {"$or", "$_OR_"}, {"$xor", "$_XOR_"}, {"$xnor", "$_XNOR_"}};
    for (size_t i = 0; i < sizeof k_map / sizeof k_map[0]; i++) {
        lf fix;
        odin3_module *mod = NULL;
        bw_case cs = {k_map[i].type, SH_BINARY, 7, 7, 7, false, false};
        lower_and_check(&fix, &cs, &mod);
        TEST_ASSERT_EQUAL_UINT32(7, lf_count_type(&fix, mod, k_map[i].gate));
        TEST_ASSERT_EQUAL_STRING(k_map[i].gate, driver_type(&fix, mod, lf_wnet(&fix, mod, "Y", 3)));
        lf_end(&fix);
    }
}

static void test_mux_has_the_select_on_every_bit(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$mux", SH_MUX, 4, 4, 4, false, false};
    lower_and_check(&fix, &cs, &mod);
    odin3_net_id sel = lf_wnet(&fix, mod, "S", 0);
    for (uint32_t k = 0; k < 4; k++) {
        odin3_pin_id drv = odin3_net_driver(mod, lf_wnet(&fix, mod, "Y", k));
        odin3_pinslice pins = odin3_node_pins(mod, odin3_pin_node(mod, drv));
        TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "A", k).v, odin3_pin_net(mod, pins.first).v);
        TEST_ASSERT_EQUAL_UINT32(lf_wnet(&fix, mod, "B", k).v,
                                 odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 1}).v);
        TEST_ASSERT_EQUAL_UINT32(sel.v, odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 2}).v);
    }
    lf_end(&fix);
}

/* --- extension, signs ----------------------------------------------------------------------- */

static void test_not_extends_a_signed_operand_with_its_top_bit(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$not", SH_UNARY, 3, 0, 5, true, false};
    lower_and_check(&fix, &cs, &mod);
    odin3_net_id top = lf_wnet(&fix, mod, "A", 2);
    for (uint32_t k = 2; k < 5; k++) { /* bits 2..4 are NOT(A[2]) */
        odin3_pin_id drv = odin3_net_driver(mod, lf_wnet(&fix, mod, "Y", k));
        odin3_pinslice pins = odin3_node_pins(mod, odin3_pin_node(mod, drv));
        TEST_ASSERT_EQUAL_UINT32(top.v, odin3_pin_net(mod, pins.first).v);
    }
    lf_end(&fix);
}

static void test_not_zero_extends_an_unsigned_operand_with_the_constant_zero(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$not", SH_UNARY, 3, 0, 5, false, false};
    lower_and_check(&fix, &cs, &mod);
    odin3_pin_id drv = odin3_net_driver(mod, lf_wnet(&fix, mod, "Y", 4));
    odin3_pinslice pins = odin3_node_pins(mod, odin3_pin_node(mod, drv));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0,
                          odin3_net_const_value(mod, odin3_pin_net(mod, pins.first)));
    lf_end(&fix);
}

/* The mixed-sign warning of `type` is in the log exactly once. */
static void expect_warned_once(const char *type) {
    char want[160];
    (void)snprintf(want, sizeof want,
                   "cell '%s' 'dut' has A_SIGNED != B_SIGNED: treated as "
                   "unsigned (both-signed rule, Yosys simlib; PHASE2 #25)",
                   type);
    const char *first = strstr(log_text, want);
    TEST_ASSERT_NOT_NULL_MESSAGE(first, log_text);
    TEST_ASSERT_NULL_MESSAGE(strstr(first + 1, want), "warned twice");
}

static void test_a_mixed_sign_cell_is_lowered_as_unsigned_and_warns_once(void) {
    static const char *const k_types[] = {"$and", "$or", "$xor", "$xnor", "$eq", "$ne"};
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        lf fix;
        odin3_module *mod = NULL;
        bool cmp = i >= 4;
        bw_case cs = {k_types[i], cmp ? SH_CMP : SH_BINARY, 3, 5, cmp ? 1U : 6U, true, false};
        log_text[0] = '\0';
        lower_and_check(&fix, &cs, &mod);
        expect_warned_once(k_types[i]);
        lf_end(&fix);
    }
}

static void test_same_sign_cells_do_not_warn(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 3, 5, 5, true, true};
    lower_and_check(&fix, &cs, &mod);
    TEST_ASSERT_NULL(strstr(log_text, "A_SIGNED != B_SIGNED"));
    lf_end(&fix);
}

/* --- names, provenance, keep, counts -------------------------------------------------------- */

/* Net holds bit `bit` of `wire` as its primary or as an alias. */
static bool holds_wire_bit(odin3_module *mod, odin3_net_id net, odin3_wire_id wire, uint32_t bit) {
    odin3_wirebit prim = odin3_net_primary(mod, net);
    if (prim.wire.v == wire.v && prim.bit == bit) {
        return true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(mod, net, &cursor, &alias)) {
        if (alias.wb.wire.v == wire.v && alias.wb.bit == bit) {
            return true;
        }
    }
    return false;
}

static void test_output_nets_keep_their_names_and_wire_membership(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 4, 4, 4, false, false};
    lower_and_check(&fix, &cs, &mod);
    odin3_wire_id wire = odin3_module_find_wire(mod, lf_str(&fix, "Y"));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_wire_width(mod, wire));
    TEST_ASSERT_TRUE(odin3_node_valid(odin3_wire_port_node(mod, wire)));
    for (uint32_t bit = 0; bit < 4; bit++) {
        odin3_net_id net = odin3_wire_net(mod, wire, bit);
        TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, net));
        TEST_ASSERT_EQUAL_STRING("$_AND_", driver_type(&fix, mod, net));
        TEST_ASSERT_TRUE(holds_wire_bit(mod, net, wire, bit));
    }
    lf_end(&fix);
}

/* Name of live node `id` of mod ("" when unnamed). */
static const char *node_text(lf *fix, odin3_module *mod, uint32_t id) {
    uint32_t name = odin3_node_name(mod, (odin3_node_id){id});
    return name != 0 ? odin3_strtab_get(odin3_design_strtab(fix->design), name) : "";
}

/* Number of live bit nodes of mod named exactly `text`. */
static uint32_t gates_named(lf *fix, odin3_module *mod, const char *text) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        found +=
            odin3_node_live(mod, (odin3_node_id){id}) && strcmp(node_text(fix, mod, id), text) == 0
                ? 1U
                : 0U;
    }
    return found;
}

static void test_bit_nodes_are_named_cell_dot_op_index(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 3, 3, 3, false, false};
    lower_and_check(&fix, &cs, &mod);
    const char *want[3] = {"dut.and0", "dut.and1", "dut.and2"};
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, gates_named(&fix, mod, want[i]), want[i]);
    }
    lf_end(&fix);
}

static void test_eq_names_use_each_gates_own_op(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$eq", SH_CMP, 2, 2, 1, false, false};
    lower_and_check(&fix, &cs, &mod);
    /* two xors, one or, one not: gate index counts every gate of the cell in creation order */
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.xor0"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.xor1"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.or2"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.not3"));
    lf_end(&fix);
}

/* Constants are not counted: $and 2 -> 4 bits unsigned makes constants before its first gate. */
static void test_the_name_index_counts_gates_not_constants(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 2, 4, 4, false, false};
    lower_and_check(&fix, &cs, &mod);
    TEST_ASSERT_TRUE(lf_count_type(&fix, mod, "$_CONST0_") > 0);
    const char *want[4] = {"dut.and0", "dut.and1", "dut.and2", "dut.and3"};
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, gates_named(&fix, mod, want[i]), want[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, gates_named(&fix, mod, "dut.and4"));
    lf_end(&fix);
}

static void test_a_signed_not_has_no_constant_and_the_same_indices(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$not", SH_UNARY, 2, 0, 4, true, false};
    lower_and_check(&fix, &cs, &mod);
    for (uint32_t i = 0; i < 4; i++) {
        char want[16];
        (void)snprintf(want, sizeof want, "dut.not%u", i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, gates_named(&fix, mod, want), want);
    }
    lf_end(&fix);
}

/* An unnamed $or on 2-bit ports: its name is that of the output wire "Y". */
static odin3_node_id unnamed_or(lf *fix, odin3_module **mod) {
    lf_begin(fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$or", params, 5, NULL, {0}};
    return lf_one_cell(fix, &cell, mod);
}

static void test_an_unnamed_word_cell_takes_the_name_of_its_output_wire(void) {
    lf fix;
    odin3_module *mod = NULL;
    (void)unnamed_or(&fix, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "Y.or0"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "Y.or1"));
    lf_end(&fix);
}

static void test_an_unnamed_word_cell_prefers_the_name_of_its_output_net(void) {
    lf fix;
    odin3_module *mod = NULL;
    (void)unnamed_or(&fix, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_rename(mod, lf_wnet(&fix, mod, "Y", 0), lf_str(&fix, "ynet")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "ynet.or0"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "ynet.or1"));
    lf_end(&fix);
}

static void test_a_cell_named_neither_way_falls_back_to_dollar_c_id(void) {
    lf fix;
    odin3_module *mod = NULL;
    odin3_node_id cell = unnamed_or(&fix, &mod);
    odin3_pinslice ypins = odin3_node_port(mod, cell, 2);
    for (uint32_t i = 0; i < ypins.count; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_pin_disconnect(mod, (odin3_pin_id){ypins.first.v + i}));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    char want[32];
    (void)snprintf(want, sizeof want, "$c%u.or0", cell.v);
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, want));
    lf_end(&fix);
}

static void test_a_taken_derived_name_gets_the_underscore_n_suffix(void) {
    lf fix;
    odin3_module *mod = NULL;
    (void)unnamed_or(&fix, &mod);
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix.design, lf_str(&fix, "$_CONST0_"), &tid));
    odin3_node_spec taken = {tid, lf_str(&fix, "Y.or0"), {0}, NULL, 0};
    odin3_node_id squatter = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &taken, &squatter));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "Y.or0"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "Y.or0_1"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "Y.or1"));
    lf_end(&fix);
}

static void test_a_taken_bit_node_name_gets_the_underscore_n_suffix(void) {
    lf fix;
    lf_begin(&fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$and", params, 5, "dut", {0}};
    odin3_module *mod = NULL;
    (void)lf_one_cell(&fix, &cell, &mod);
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix.design, lf_str(&fix, "$_CONST0_"), &tid));
    odin3_node_spec taken = {tid, lf_str(&fix, "dut.and0"), {0}, NULL, 0};
    odin3_node_id squatter = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &taken, &squatter));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.and0"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.and0_1"));
    TEST_ASSERT_EQUAL_UINT32(1, gates_named(&fix, mod, "dut.and1"));
    lf_end(&fix);
}

/* Every live bit node of mod has a DERIVED record whose only parent is src; returns how many. */
static uint32_t count_derived_gates(lf *fix, odin3_module *mod, odin3_prov_id src) {
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def = odin3_celltype_get(fix->design, odin3_node_type(mod, node));
        if (!odin3_node_live(mod, node) || def->gran != ODIN3_GRAN_BIT) {
            continue;
        }
        const odin3_prov_record *rec = odin3_prov_get(fix->design, odin3_node_prov(mod, node));
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
        TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
        TEST_ASSERT_EQUAL_UINT32(src.v, rec->parents.ids[0].v);
        seen++;
    }
    return seen;
}

typedef struct line_hits {
    uint32_t live, dead;
} line_hits;

/* The nodes the source line reaches, live and dead. */
static line_hits count_line_hits(lf *fix, const char *file, uint32_t line) {
    odin3_prov_index *index = odin3_prov_index_build(fix->design);
    TEST_ASSERT_NOT_NULL(index);
    odin3_srcloc loc = {lf_str(fix, file), line, 1, line, 4, 0};
    odin3_prov_hits hits = odin3_prov_index_by_loc(index, loc);
    line_hits found = {0, 0};
    for (uint32_t i = 0; i < hits.count; i++) {
        if (hits.hits[i].obj.kind != ODIN3_OBJ_NODE) {
            continue;
        }
        found.live += hits.hits[i].live ? 1U : 0U;
        found.dead += hits.hits[i].live ? 0U : 1U;
    }
    odin3_prov_index_destroy(index);
    return found;
}

static void test_every_created_node_has_a_derived_record_of_the_word_cell(void) {
    lf fix;
    lf_begin(&fix);
    odin3_prov_id src = lf_source(&fix, "dut.v", 42);
    odin3_value params[5];
    lf_binary spec = {false, false, 4, 4, 4};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$xor", params, 5, "dut", src};
    odin3_module *mod = NULL;
    (void)lf_one_cell(&fix, &cell, &mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(4, count_derived_gates(&fix, mod, src));
    /* the source line reaches the dead word cell and every gate made from it */
    line_hits hits = count_line_hits(&fix, "dut.v", 42);
    TEST_ASSERT_EQUAL_UINT32(4, hits.live);
    TEST_ASSERT_EQUAL_UINT32(1, hits.dead);
    lf_end(&fix);
}

/* The record of the last live bit node of mod (0 when it has none). */
static odin3_prov_id last_gate_record(lf *fix, odin3_module *mod) {
    odin3_prov_id rec = {0};
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(fix->design, odin3_node_type(mod, node))->gran == ODIN3_GRAN_BIT) {
            rec = odin3_node_prov(mod, node);
        }
    }
    return rec;
}

static void test_each_lowered_cell_gets_its_own_provenance_operation(void) {
    lf fix;
    lf_begin(&fix);
    odin3_module *mod = lf_module(&fix, "m");
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    lf_cell_spec first = {"$and", params, 5, "c1", lf_source(&fix, "a.v", 1)};
    (void)lf_cell(&fix, mod, &first);
    odin3_module *mod2 = lf_module(&fix, "m2");
    lf_cell_spec second = {"$or", params, 5, "c2", lf_source(&fix, "a.v", 2)};
    (void)lf_cell(&fix, mod2, &second);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    odin3_prov_id rec0 = last_gate_record(&fix, mod);
    odin3_prov_id rec1 = last_gate_record(&fix, mod2);
    TEST_ASSERT_TRUE(rec0.v != 0 && rec1.v != 0);
    TEST_ASSERT_TRUE(rec0.v != rec1.v);
    const odin3_prov_record *r0 = odin3_prov_get(fix.design, rec0);
    const odin3_prov_record *r1 = odin3_prov_get(fix.design, rec1);
    TEST_ASSERT_TRUE(r0->op != r1->op || r0->run.v != r1->run.v);
    lf_end(&fix);
}

static void test_keep_reaches_every_created_node(void) {
    lf fix;
    lf_begin(&fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 3, 3, 3};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$or", params, 5, "dut", {0}};
    odin3_module *mod = NULL;
    odin3_node_id node = lf_one_cell(&fix, &cell, &mod);
    odin3_value one = odin3_value_int(1);
    uint32_t key = lf_str(&fix, "keep");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    uint32_t kept = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id gate = {id};
        if (odin3_node_live(mod, gate) &&
            strcmp(odin3_celltype_get(fix.design, odin3_node_type(mod, gate))->name, "$_OR_") ==
                0) {
            const odin3_value *val = odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, id}, key);
            TEST_ASSERT_NOT_NULL(val);
            kept++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, kept);
    lf_end(&fix);
}

static void test_the_pass_logs_its_counts_and_they_match_the_node_counts(void) {
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 5, 5, 5};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$and", params, 5, "dut", {0}};
    (void)lf_one_cell(&fix, &cell, &mod);
    uint32_t before = lf_live_nodes(mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    uint32_t after = lf_live_nodes(mod);
    char want[96];
    (void)snprintf(want, sizeof want, "lower: module 'm': lowered 1 word cells to %u bit cells",
                   after - before + 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, want), log_text);
    TEST_ASSERT_NULL(strstr(log_text, "nothing to lower"));
    lf_end(&fix);
}

static void test_the_modules_of_a_design_are_lowered_in_id_order_and_all_of_them(void) {
    lf fix;
    lf_begin(&fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    odin3_module *mods[3];
    const char *names[3] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        mods[i] = lf_module(&fix, names[i]);
        lf_cell_spec cell = {"$and", params, 5, "dut", {0}};
        (void)lf_cell(&fix, mods[i], &cell);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    const char *pa = strstr(log_text, "module 'a'");
    const char *pb = strstr(log_text, "module 'b'");
    const char *pc = strstr(log_text, "module 'c'");
    TEST_ASSERT_TRUE(pa != NULL && pb != NULL && pc != NULL);
    TEST_ASSERT_TRUE(pa < pb && pb < pc);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, word_nodes(&fix, mods[i]));
    }
    lf_end(&fix);
}

static void test_a_cell_with_an_open_input_pin_lowers_to_a_gate_with_an_open_pin(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 2, 2, 2, false, false};
    lf_begin(&fix);
    odin3_node_id node = build(&fix, &cs, &mod);
    /* open B[1]: disconnect its pin */
    odin3_pinslice pins = odin3_node_port(mod, node, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(mod, (odin3_pin_id){pins.first.v + 1}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(2, lf_count_type(&fix, mod, "$_AND_"));
    check_netlist_view(mod);
    lf_end(&fix);
}

/* --- out of memory -------------------------------------------------------------------------- */

/* The fixture of an OOM run: module "m" with the case's cell; with `second`, module "n" after it
 * holds a 40-bit $and, lowered after it. */
typedef struct oom_fix {
    odin3_module *mod, *other;
    odin3_node_id cell, other_cell;
    uint32_t other_live; /* live nodes of "n" in the input */
} oom_fix;

static void oom_setup(lf *fix, const bw_case *cs, bool second, oom_fix *of) {
    lf_begin(fix);
    of->cell = build(fix, cs, &of->mod);
    of->other = NULL;
    of->other_cell = (odin3_node_id){0};
    if (second) {
        odin3_value params[5];
        lf_binary spec = {false, false, 40, 40, 40};
        lf_binary_params(&spec, params);
        lf_cell_spec cell = {"$and", params, 5, "big", {0}};
        of->other = lf_module(fix, "n");
        of->other_cell = lf_cell(fix, of->other, &cell);
        of->other_live = lf_live_nodes(of->other);
    }
}

/*
 * After a failed run, decide the phase from the IR (spec 12.2). While the first word cell is live
 * the run failed before its commit: the design must equal the input byte for byte. Once it
 * is dead the failure is in its commit (a partial state, check FULL clean) or in a later cell;
 * a later cell that is still live must have been rolled back to exactly its input.
 */
static bool oom_judge(lf *fix, const oom_fix *of, const odin3_strbuf *input, const char *msg) {
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(fix, &after));
    bool partial = !odin3_node_live(of->mod, of->cell);
    if (!partial) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(input->data, after.data, msg);
    } else if (of->other != NULL && odin3_node_live(of->other, of->other_cell)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(of->other_live, lf_live_nodes(of->other), msg);
    }
    odin3_strbuf_free(&after);
    return partial;
}

static void oom_sweep(const bw_case *cs, bool second) {
    char msg[96];
    describe(cs, msg, sizeof msg);
    odin3_strbuf input;
    odin3_strbuf_init(&input);
    lf fix;
    oom_fix of;
    oom_setup(&fix, cs, second, &of);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &input));
    bool done = false;
    uint32_t built = 0;
    for (long step = 0; !done && step < 4000; step++) {
        odin3_util_set_alloc_fail_after(step);
        odin3_status st = odin3_lower_run(&fix.ctx, fix.design, NULL);
        odin3_util_set_alloc_fail_after(-1);
        odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_module(of.mod, opts), msg);
        if (st == ODIN3_OK) {
            done = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, msg);
        if (!oom_judge(&fix, &of, &input, msg)) {
            built++;
            continue;
        }
        lf_end(&fix); /* a commit-phase partial state: start again from the input */
        oom_setup(&fix, cs, second, &of);
    }
    TEST_ASSERT_TRUE_MESSAGE(done, msg);
    TEST_ASSERT_TRUE_MESSAGE(built > 0, msg); /* the sweep did hit the build phase */
    check_netlist_view(of.mod);
    odin3_strbuf_free(&input);
    lf_end(&fix);
}

static void test_oom_at_every_allocation_leaves_a_valid_design_per_rule_family(void) {
    static const bw_case k_cases[] = {
        {"$and", SH_BINARY, 4, 4, 4, false, false},
        {"$not", SH_UNARY, 3, 0, 5, true, false},
        {"$mux", SH_MUX, 3, 3, 3, false, false},
        {"$reduce_xnor", SH_REDUCE, 5, 0, 3, false, false},
        {"$logic_or", SH_LOGIC_BIN, 3, 4, 1, false, false},
        {"$eq", SH_CMP, 4, 3, 1, true, true},
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        oom_sweep(&k_cases[i], false);
        oom_sweep(&k_cases[i], true);
    }
}

/* --- caps ----------------------------------------------------------------------------------- */

static void test_a_cell_over_the_node_cap_is_a_located_error_and_nothing_is_built(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 40, 40, 40, false, false};
    lf_begin(&fix);
    (void)build(&fix, &cs, &mod);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &before));
    odin3_lower_limits caps = {20, 0, 0};
    odin3_lower_test_set_limits(&caps);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
    odin3_lower_test_set_limits(NULL);
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "lower: module 'm': '$and' 'dut' would create 43 nodes (cap 20)"));
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&fix);
}

static void test_the_module_cap_counts_live_nodes(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 8, 8, 8, false, false};
    lf_begin(&fix);
    (void)build(&fix, &cs, &mod);
    odin3_lower_limits caps = {0, 12, 0}; /* the module holds 4 nodes; 8 + 3 more would pass 12 */
    odin3_lower_test_set_limits(&caps);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
    odin3_lower_test_set_limits(NULL);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "would create"));
    TEST_ASSERT_EQUAL_UINT32(1, word_nodes(&fix, mod));
    lf_end(&fix);
}

static void lower_with_caps(lf *fix, odin3_lower_limits caps, odin3_status want) {
    odin3_lower_test_set_limits(&caps);
    odin3_status st = run_lower(fix);
    odin3_lower_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_INT(want, st);
}

/* $and 8 bits: 8 gates + 3 constants of slack = 11 nodes. The cap is inclusive. */
static void test_the_cell_cap_is_inclusive(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 8, 8, 8, false, false};
    lf_begin(&fix);
    (void)build(&fix, &cs, &mod);
    lower_with_caps(&fix, (odin3_lower_limits){10, 0, 0}, ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_UINT32(1, word_nodes(&fix, mod));
    /* a cell exactly at its cap still counts against the module cap */
    lower_with_caps(&fix, (odin3_lower_limits){11, lf_live_nodes(mod) + 10, 0},
                    ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_UINT32(1, word_nodes(&fix, mod));
    lower_with_caps(&fix, (odin3_lower_limits){11, 0, 0}, ODIN3_OK);
    TEST_ASSERT_EQUAL_UINT32(0, word_nodes(&fix, mod));
    lf_end(&fix);
}

static void test_the_module_cap_is_inclusive(void) {
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$and", SH_BINARY, 8, 8, 8, false, false};
    lf_begin(&fix);
    (void)build(&fix, &cs, &mod);
    uint64_t live = lf_live_nodes(mod);
    lower_with_caps(&fix, (odin3_lower_limits){0, live + 10, 0}, ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_UINT32(1, word_nodes(&fix, mod));
    lower_with_caps(&fix, (odin3_lower_limits){0, live + 11, 0}, ODIN3_OK);
    TEST_ASSERT_EQUAL_UINT32(0, word_nodes(&fix, mod));
    lf_end(&fix);
}

/* Three 2-bit $and cells in one module: each lowering nets +1 live node (2 gates, the word cell
 * gone), so the third needs live0 + 2 + 5. Neither a missed decrement nor a missed increment of
 * the live count passes both bounds. */
static void three_ands(lf *fix, odin3_module **mod) {
    lf_begin(fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    *mod = lf_module(fix, "m");
    static const char *const k_prefix[3] = {"p0_", "p1_", "p2_"};
    static const char *const k_name[3] = {"c0", "c1", "c2"};
    for (int i = 0; i < 3; i++) {
        lf_cell_spec cell = {"$and", params, 5, k_name[i], {0}};
        (void)lf_cell_prefixed(fix, *mod, &cell, k_prefix[i]);
    }
}

static void test_the_module_cap_follows_the_live_count_over_several_cells(void) {
    lf fix;
    odin3_module *mod = NULL;
    three_ands(&fix, &mod);
    uint64_t live = lf_live_nodes(mod);
    lower_with_caps(&fix, (odin3_lower_limits){0, live + 6, 0}, ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_UINT32(1, word_nodes(&fix, mod)); /* the first two lowered, the third not */
    lf_end(&fix);
    three_ands(&fix, &mod);
    lower_with_caps(&fix, (odin3_lower_limits){0, live + 7, 0}, ODIN3_OK);
    TEST_ASSERT_EQUAL_UINT32(0, word_nodes(&fix, mod));
    lf_end(&fix);
}

/* --- the post-condition (spec 12.3) --------------------------------------------------------- */

/* A rule for $not that builds like the real one and also leaves a live word node behind. */
static odin3_status leaky_build(lw_ctx *lw, const lw_cell *cell, lw_bits *outs) {
    odin3_status st = lw_build_not(lw, cell, outs);
    odin3_celltype_id tid = {0};
    uint32_t type_name = 0;
    st =
        st == ODIN3_OK ? odin3_design_intern(lw->design, odin3_bytes_cstr("$and"), &type_name) : st;
    if (st != ODIN3_OK || !odin3_celltype_find(lw->design, type_name, &tid)) {
        return st != ODIN3_OK ? st : ODIN3_ERR_INVALID_ARG;
    }
    odin3_value params[5];
    lf_binary spec = {false, false, 1, 1, 1};
    lf_binary_params(&spec, params);
    odin3_node_spec leak = {tid, 0, {0}, params, 5};
    odin3_node_id node = {0};
    return odin3_node_create(lw->module, &leak, &node);
}

static void test_a_word_cell_left_live_is_a_check_error(void) {
    static const lw_rule k_leaky = {"$not", leaky_build, lw_est_not};
    lf fix;
    odin3_module *mod = NULL;
    bw_case cs = {"$not", SH_UNARY, 2, 0, 2, false, false};
    lf_begin(&fix);
    (void)build(&fix, &cs, &mod);
    odin3_lower_test_override_rule(&k_leaky);
    odin3_status st = run_lower(&fix);
    odin3_lower_test_override_rule(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, st);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "is not in the netlist view after lowering"));
    lf_end(&fix);
}

/* L1: a cell without a rule is known before anything changes: nothing is lowered, all are listed.
 */
static void test_ruleless_cells_are_reported_before_any_cell_is_lowered(void) {
    lf fix;
    lf_begin(&fix);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    odin3_module *mod = lf_module(&fix, "m");
    lf_cell_spec first = {"$and", params, 5, "first", {0}};
    lf_cell_spec add1 = {"$dlatch", NULL, 0, "sum1", {0}};
    lf_cell_spec add2 = {"$dlatch", NULL, 0, "sum2", {0}};
    (void)lf_cell_prefixed(&fix, mod, &first, "a_");
    (void)lf_cell_prefixed(&fix, mod, &add1, "b_");
    (void)lf_cell_prefixed(&fix, mod, &add2, "c_");
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "no lowering rule for cell type '$dlatch' ('sum1')"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "('sum2')"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "not implemented yet"));
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&fix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_not_sweep);
    RUN_TEST(test_and_sweep);
    RUN_TEST(test_or_sweep);
    RUN_TEST(test_xor_sweep);
    RUN_TEST(test_xnor_sweep);
    RUN_TEST(test_mux_sweep);
    RUN_TEST(test_reduce_sweeps);
    RUN_TEST(test_logic_not_sweep);
    RUN_TEST(test_logic_binary_sweeps);
    RUN_TEST(test_eq_ne_sweeps);
    RUN_TEST(test_a_five_bit_reduce_or_is_the_carried_odd_bit_tree);
    RUN_TEST(test_a_one_bit_reduction_creates_no_node_and_y_is_a);
    RUN_TEST(test_a_one_bit_reduce_xnor_is_one_inverter);
    RUN_TEST(test_eq_is_xors_then_an_or_tree_then_a_not_and_ne_has_no_not);
    RUN_TEST(test_logic_and_ors_both_operands_then_ands);
    RUN_TEST(test_bitwise_cell_makes_one_gate_of_its_type_per_bit);
    RUN_TEST(test_mux_has_the_select_on_every_bit);
    RUN_TEST(test_not_extends_a_signed_operand_with_its_top_bit);
    RUN_TEST(test_not_zero_extends_an_unsigned_operand_with_the_constant_zero);
    RUN_TEST(test_a_mixed_sign_cell_is_lowered_as_unsigned_and_warns_once);
    RUN_TEST(test_same_sign_cells_do_not_warn);
    RUN_TEST(test_output_nets_keep_their_names_and_wire_membership);
    RUN_TEST(test_bit_nodes_are_named_cell_dot_op_index);
    RUN_TEST(test_eq_names_use_each_gates_own_op);
    RUN_TEST(test_the_name_index_counts_gates_not_constants);
    RUN_TEST(test_a_signed_not_has_no_constant_and_the_same_indices);
    RUN_TEST(test_an_unnamed_word_cell_takes_the_name_of_its_output_wire);
    RUN_TEST(test_an_unnamed_word_cell_prefers_the_name_of_its_output_net);
    RUN_TEST(test_a_cell_named_neither_way_falls_back_to_dollar_c_id);
    RUN_TEST(test_a_taken_derived_name_gets_the_underscore_n_suffix);
    RUN_TEST(test_a_taken_bit_node_name_gets_the_underscore_n_suffix);
    RUN_TEST(test_every_created_node_has_a_derived_record_of_the_word_cell);
    RUN_TEST(test_each_lowered_cell_gets_its_own_provenance_operation);
    RUN_TEST(test_keep_reaches_every_created_node);
    RUN_TEST(test_the_pass_logs_its_counts_and_they_match_the_node_counts);
    RUN_TEST(test_the_modules_of_a_design_are_lowered_in_id_order_and_all_of_them);
    RUN_TEST(test_a_cell_with_an_open_input_pin_lowers_to_a_gate_with_an_open_pin);
    RUN_TEST(test_oom_at_every_allocation_leaves_a_valid_design_per_rule_family);
    RUN_TEST(test_a_cell_over_the_node_cap_is_a_located_error_and_nothing_is_built);
    RUN_TEST(test_the_module_cap_counts_live_nodes);
    RUN_TEST(test_the_cell_cap_is_inclusive);
    RUN_TEST(test_the_module_cap_is_inclusive);
    RUN_TEST(test_the_module_cap_follows_the_live_count_over_several_cells);
    RUN_TEST(test_a_word_cell_left_live_is_a_check_error);
    RUN_TEST(test_ruleless_cells_are_reported_before_any_cell_is_lowered);
    return UNITY_END();
}
