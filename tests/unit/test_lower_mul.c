/*
 * test_lower_mul.c — 3B Task 5, part 1: the full adder, the bit heap and $mul (spec §5.5).
 * `lw_fa` against its truth table; `lw_heap` cancel-and-carry; then $mul through the pass for
 * widths 1 to 64, signed and unsigned, equal and unequal operands, Y narrower, equal and wider:
 * the gate count against a model of maccmap's summand and reduction order (column depths and
 * occupancy masks, no nets), the size estimate, the netlist view, and simulator-equal with the
 * registry hook (exhaustive up to 14 input bits, seeded random above). Also the literal-x rule
 * and the per-cell cap.
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { EXHAUSTIVE_BITS = 14, RANDOM_VECTORS = 1024, LOG_TEXT = 4096, MAX_W = 64, MAX_ROWS = 140 };

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
    odin3_lower_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
}

static odin3_status run_lower(lf *fix) {
    return odin3_pass_run(fix->design, "lower", (odin3_bytes){NULL, 0});
}

static uint32_t min_of(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

/* --- lw_fa ---------------------------------------------------------------------------------- */

static uint8_t eval_gate(const char *type, const uint8_t *in) {
    if (strcmp(type, "$_AND_") == 0) {
        return in[0] & in[1];
    }
    if (strcmp(type, "$_OR_") == 0) {
        return in[0] | in[1];
    }
    TEST_ASSERT_EQUAL_STRING("$_XOR_", type);
    return in[0] ^ in[1];
}

static void test_full_adder_truth_table_and_five_gates(void) {
    lf fix;
    lf_begin(&fix);
    lf_binary spec = {false, false, 1, 1, 1};
    odin3_value params[5];
    lf_binary_params(&spec, params);
    lf_cell_spec cs = {"$and", params, 5, "host", {0}};
    odin3_module *mod = NULL;
    odin3_node_id host = lf_one_cell(&fix, &cs, &mod);
    odin3_net_id inputs[3];
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, (odin3_prov_id){0}, &inputs[i]));
    }
    lw_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_ctx_init(&ctx, &fix.ctx, fix.design, NULL));
    lw_ctx_enter(&ctx, mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_begin(&ctx, host));
    odin3_net_id out[2] = {{0}, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_fa(&ctx, inputs, out));
    TEST_ASSERT_EQUAL_UINT64(5, ctx.gates);
    for (unsigned vec = 0; vec < 8; vec++) {
        uint8_t value[4096] = {0};
        for (unsigned i = 0; i < 3; i++) {
            value[inputs[i].v] = (vec >> i) & 1U;
        }
        for (size_t i = 0; i < ctx.created.len; i++) {
            odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&ctx.created, i);
            odin3_pinslice pins = odin3_node_pins(mod, node);
            uint8_t in[2] = {value[odin3_pin_net(mod, pins.first).v],
                             value[odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 1}).v]};
            odin3_net_id dst = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 2});
            value[dst.v] =
                eval_gate(odin3_celltype_get(fix.design, odin3_node_type(mod, node))->name, in);
        }
        unsigned total = (vec & 1U) + ((vec >> 1) & 1U) + ((vec >> 2) & 1U);
        TEST_ASSERT_EQUAL_UINT8(total & 1U, value[out[0].v]);
        TEST_ASSERT_EQUAL_UINT8(total >> 1, value[out[1].v]);
    }
    lw_rollback(&ctx);
    lw_ctx_free(&ctx);
    lf_end(&fix);
}

/* --- lw_heap -------------------------------------------------------------------------------- */

static odin3_net_id net_of(uint32_t id) {
    return (odin3_net_id){id};
}

static void test_heap_cancel_and_carry(void) {
    lw_heap heap;
    lw_heap_init(&heap);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_reset(&heap, 4, net_of(1)));
    TEST_ASSERT_EQUAL_UINT32(4, lw_heap_width(&heap));
    /* one copy stays; two cancel and carry; three leave one in the column and one above */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(7), 0));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(7), 0));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 0));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(7), 0));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 0));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 1));
    /* a fourth: column 0 empties and carries; column 1 holds it so it cancels and carries on */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(7), 0));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 1));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 2));
    lw_heap_free(&heap);
}

static void test_heap_skips_the_zero_net_and_columns_past_the_width(void) {
    lw_heap heap;
    lw_heap_init(&heap);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_reset(&heap, 2, net_of(1)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(1), 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(5), 2));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 1));
    /* a carry out of the last column is dropped */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(5), 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(5), 1));
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 1));
    lw_heap_free(&heap);
}

static void test_heap_pops_the_smallest_net_first(void) {
    lw_heap heap;
    lw_heap_init(&heap);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_reset(&heap, 1, net_of(1)));
    static const uint32_t k_ids[] = {9, 4, 12, 6, 30, 5};
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(k_ids[i]), 0));
    }
    static const uint32_t k_sorted[] = {4, 5, 6, 9, 12, 30};
    for (size_t i = 0; i < sizeof k_sorted / sizeof k_sorted[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(k_sorted[i], lw_heap_pop(&heap, 0).v);
    }
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 0));
    lw_heap_free(&heap);
}

static void test_heap_add_is_atomic_under_out_of_memory(void) {
    lw_heap heap;
    lw_heap_init(&heap);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_reset(&heap, 3, net_of(1)));
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, lw_heap_add(&heap, net_of(8), 1));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT32(0, lw_heap_depth(&heap, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_heap_add(&heap, net_of(8), 1));
    TEST_ASSERT_EQUAL_UINT32(1, lw_heap_depth(&heap, 1));
    lw_heap_free(&heap);
}

/* --- the model of maccmap's order ------------------------------------------------------------ */

typedef struct mul_case {
    uint32_t aw, bw, yw;
    bool a_signed, b_signed;
} mul_case;

/* The state of the model: column depths, the summands as occupancy masks, the packed bit count. */
typedef struct mul_model {
    uint32_t width;
    uint32_t depth[MAX_W];
    uint64_t rows[MAX_ROWS];
    uint32_t count;
    uint32_t tree;
    uint64_t slices; /* full-adder bit slices */
} mul_model;

/* Columns [low, high] as a bit mask. */
typedef struct bit_range {
    uint32_t low;
    uint32_t high;
} bit_range;

static uint64_t range_mask(bit_range range) {
    uint64_t upto = range.high >= MAX_W - 1 ? UINT64_MAX : (((uint64_t)1 << (range.high + 1)) - 1);
    return upto & ~(((uint64_t)1 << range.low) - 1);
}

static uint32_t product_rows(const mul_case *cs) {
    return min_of(min_of(cs->aw, cs->bw), cs->yw);
}

static void model_fill(const mul_case *cs, bool is_signed, mul_model *model) {
    uint32_t rows = product_rows(cs);
    model->width = cs->yw;
    for (uint32_t col = 0; col < model->width; col++) {
        model->depth[col] = min_of(col + 1, rows) + (is_signed && col + 1 == rows ? 1U : 0U);
    }
}

static bool model_any(const mul_model *model) {
    for (uint32_t col = 0; col < model->width; col++) {
        if (model->depth[col] != 0) {
            return true;
        }
    }
    return false;
}

/* The deepest column (the last one holding the maximum depth): depth in [0], position in [1]. */
static void model_peak(const mul_model *model, uint32_t peak[2]) {
    peak[0] = 0;
    peak[1] = 0;
    for (uint32_t col = 0; col < model->width; col++) {
        if (peak[0] <= model->depth[col]) {
            peak[0] = model->depth[col];
            peak[1] = col;
        }
    }
}

/* The tree slots the columns up to `maxp` holding `maxd` bits would need. */
static uint32_t model_required(const mul_model *model, uint32_t maxp, uint32_t maxd) {
    uint32_t required = 0;
    for (uint32_t col = 0; col <= maxp; col++) {
        required += model->depth[col] == maxd ? 1U << col : 0U;
    }
    return required;
}

/* synth()'s packing loop after a summand was taken. */
static void model_pack(mul_model *model) {
    for (;;) {
        uint32_t peak[2];
        model_peak(model, peak);
        if (peak[0] == 0 || peak[1] > 4 ||
            model_required(model, peak[1], peak[0]) > model->count - 1 - model->tree) {
            return;
        }
        for (uint32_t col = 0; col <= peak[1]; col++) {
            if (model->depth[col] == peak[0]) {
                model->depth[col]--;
                model->tree += 1U << col;
            }
        }
    }
}

static void model_take(mul_model *model) {
    model->count = 0;
    model->tree = 0;
    while (model_any(model)) {
        uint64_t mask = 0;
        for (uint32_t col = 0; col < model->width; col++) {
            if (model->depth[col] != 0) {
                mask |= (uint64_t)1 << col;
                model->depth[col]--;
            }
        }
        model->rows[model->count++] = mask;
        model_pack(model);
    }
}

/* One triple of summands: the columns where any has data get a full adder. */
static void model_triple(mul_model *model, uint64_t union_mask, uint64_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (union_mask != 0) {
        uint32_t low = 0;
        while (((union_mask >> low) & 1U) == 0) {
            low++;
        }
        uint32_t high = MAX_W - 1;
        while (((union_mask >> high) & 1U) == 0) {
            high--;
        }
        model->slices += high - low + 1;
        out[0] = range_mask((bit_range){low, high});
        out[1] = (range_mask((bit_range){low, high}) << 1) &
                 range_mask((bit_range){0, model->width - 1});
    }
    if (model->tree > 0) {
        model->tree--;
        out[1] |= 1U;
    }
}

static void model_reduce(mul_model *model) {
    while (model->count > 2) {
        uint64_t next[MAX_ROWS];
        uint32_t used = 0;
        uint32_t row = 0;
        for (; row + 2 < model->count; row += 3) {
            uint64_t out[2];
            model_triple(model, model->rows[row] | model->rows[row + 1] | model->rows[row + 2],
                         out);
            next[used++] = out[0];
            next[used++] = out[1];
        }
        for (; row < model->count; row++) {
            next[used++] = model->rows[row];
        }
        memcpy(model->rows, next, used * sizeof next[0]);
        model->count = used;
    }
}

/* Gates by the closed forms of §5.5: ANDs, the signed row's NOTs, 5 per slice, the final adder. */
static uint64_t model_gates(const mul_case *cs, uint64_t *nots) {
    bool is_signed = cs->a_signed && cs->b_signed;
    mul_model model;
    memset(&model, 0, sizeof model);
    model_fill(cs, is_signed, &model);
    model_take(&model);
    model_reduce(&model);
    uint64_t rows = product_rows(cs);
    uint64_t width = cs->yw;
    uint64_t ands = rows * width - rows * (rows - 1) / 2;
    *nots = is_signed ? width - rows + 1 : 0;
    uint64_t alu = model.count >= 2 ? 3 * width + lw_lcu_gates(width) : 0;
    return ands + *nots + 5 * model.slices + alu;
}

/* --- $mul through the pass -------------------------------------------------------------------- */

static odin3_node_id build_mul(lf *fix, const mul_case *cs, odin3_module **mod) {
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {"$mul", params, 5, "dut", {0}};
    return lf_one_cell(fix, &cell, mod);
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

static void check_mul_case(const mul_case *cs) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "$mul a=%u b=%u y=%u signed=%d/%d", cs->aw, cs->bw, cs->yw,
                   cs->a_signed ? 1 : 0, cs->b_signed ? 1 : 0);
    lf after;
    odin3_module *after_mod = NULL;
    lf_begin(&after);
    (void)build_mul(&after, cs, &after_mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(&after), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(&after, after_mod), msg);
    check_netlist_view(after_mod);
    uint64_t nots = 0;
    uint32_t gates = lf_gate_nodes(&after, after_mod);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(model_gates(cs, &nots), gates, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)nots, lf_count_type(&after, after_mod, "$_NOT_"),
                                     msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_count_type(&after, after_mod, "$_MUX_"), msg);
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    uint32_t widths[3] = {cs->aw, cs->bw, cs->yw};
    TEST_ASSERT_TRUE_MESSAGE(gates <= lf_estimate("$mul", params, widths, 3), msg);
    lf before;
    odin3_module *before_mod = NULL;
    lf_begin(&before);
    (void)build_mul(&before, cs, &before_mod);
    lf_sim_opts sim = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 17};
    lf_sim_result res = lf_sim_equal(before.design, odin3_module_id_of(before_mod), after.design,
                                     odin3_module_id_of(after_mod), &sim);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    if (cs->aw + cs->bw <= EXHAUSTIVE_BITS) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)1 << (cs->aw + cs->bw), res.vectors, msg);
    }
    lf_end(&before);
    lf_end(&after);
}

/* Equal operand widths, each sign; Y equal to, twice and below the operand width. */
static void equal_widths(uint32_t width) {
    for (int sign = 0; sign < 2; sign++) {
        mul_case cs = {width, width, width, sign != 0, sign != 0};
        check_mul_case(&cs);
        cs.yw = width > 1 ? width - 1 : 1;
        check_mul_case(&cs);
        if (2 * width <= MAX_W / 2) {
            cs.yw = 2 * width;
            check_mul_case(&cs);
        }
    }
}

static void test_mul_matches_the_model_and_the_hook_for_widths_1_to_16(void) {
    for (uint32_t width = 1; width <= 16; width++) {
        equal_widths(width);
    }
}

static void test_mul_matches_the_model_and_the_hook_for_wide_operands(void) {
    static const uint32_t k_wide[] = {17, 24, 33, 64};
    for (size_t i = 0; i < sizeof k_wide / sizeof k_wide[0]; i++) {
        mul_case cs = {k_wide[i], k_wide[i], k_wide[i], false, false};
        check_mul_case(&cs);
        cs.a_signed = true;
        cs.b_signed = true;
        check_mul_case(&cs);
    }
}

static void test_mul_with_unequal_operands_and_every_sign_mix(void) {
    static const uint32_t k_pairs[][2] = {{1, 5}, {5, 1}, {3, 7}, {7, 3}, {2, 9}, {9, 2}, {16, 3}};
    for (size_t i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++) {
        uint32_t sum = k_pairs[i][0] + k_pairs[i][1];
        for (int sign = 0; sign < 4; sign++) {
            mul_case cs = {k_pairs[i][0], k_pairs[i][1], sum, sign == 1 || sign == 2,
                           sign == 1 || sign == 3};
            check_mul_case(&cs);
            cs.yw = sum + 3; /* Y wider than A + B */
            check_mul_case(&cs);
            cs.yw = 2;
            check_mul_case(&cs);
        }
    }
}

static void test_a_mixed_sign_mul_warns_once_and_is_unsigned(void) {
    mul_case cs = {5, 3, 8, true, false};
    check_mul_case(&cs);
    const char *first = strstr(log_text, "has A_SIGNED != B_SIGNED");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "has A_SIGNED != B_SIGNED"));
}

static void test_node_counts_are_pinned_for_small_multipliers(void) {
    static const struct {
        mul_case cs;
        uint32_t gates;
    } k_pins[] = {
        {{1, 1, 1, false, false}, 1},  /* one AND, one summand */
        {{2, 2, 2, false, false}, 14}, /* 3 ANDs + the 2-bit adder (6 + 5) */
        {{3, 3, 3, false, false}, 38}, {{4, 4, 8, false, false}, 165},
        {{4, 4, 4, true, true}, 97}, /* the signed top row: 4 NOTs more */
    };
    for (size_t i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
        lf one;
        odin3_module *one_mod = NULL;
        lf_begin(&one);
        (void)build_mul(&one, &k_pins[i].cs, &one_mod);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&one));
        uint64_t nots = 0;
        uint32_t gates = lf_gate_nodes(&one, one_mod);
        TEST_ASSERT_EQUAL_UINT64(model_gates(&k_pins[i].cs, &nots), gates);
        TEST_ASSERT_EQUAL_UINT32(k_pins[i].gates, gates);
        lf_end(&one);
    }
}

/* --- literal x, the cap ----------------------------------------------------------------------- */

static void test_a_literal_x_operand_bit_makes_every_output_bit_x(void) {
    mul_case cs = {3, 3, 4, false, false};
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    odin3_node_id mul_node = build_mul(&fix, &cs, &mod);
    odin3_celltype_id x_type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix.design, lf_str(&fix, "$_CONSTX_"), &x_type));
    odin3_net_id x_net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, (odin3_prov_id){0}, &x_net));
    odin3_netvec x_port = {&x_net, 1};
    odin3_node_spec x_spec = {x_type, 0, {0}, NULL, 0};
    odin3_node_id x_node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &x_spec, &x_port, &x_node));
    odin3_pin_id a_pin = odin3_node_pins(mod, mul_node).first; /* A[0] */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(mod, a_pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(mod, a_pin, x_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&fix));
    TEST_ASSERT_EQUAL_UINT32(0, lf_gate_nodes(&fix, mod));
    TEST_ASSERT_EQUAL_UINT32(0, lf_word_nodes(&fix, mod));
    for (uint32_t bit = 0; bit < 4; bit++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X,
                              odin3_net_const_value(mod, lf_wnet(&fix, mod, "Y", bit)));
    }
    lf_end(&fix);
}

static void test_a_mul_over_the_cell_cap_is_a_located_error_and_nothing_is_built(void) {
    mul_case cs = {8, 8, 16, false, false};
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_mul(&fix, &cs, &mod);
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    odin3_lower_limits caps = {100, 0, 0};
    odin3_lower_test_set_limits(&caps);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
    odin3_lower_test_set_limits(NULL);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: module 'm': '$mul' 'dut' would create "));
    TEST_ASSERT_NOT_NULL(strstr(log_text, " nodes (cap 100)"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    TEST_ASSERT_EQUAL_UINT32(1, lf_word_nodes(&fix, mod));
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&fix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_adder_truth_table_and_five_gates);
    RUN_TEST(test_heap_cancel_and_carry);
    RUN_TEST(test_heap_skips_the_zero_net_and_columns_past_the_width);
    RUN_TEST(test_heap_pops_the_smallest_net_first);
    RUN_TEST(test_heap_add_is_atomic_under_out_of_memory);
    RUN_TEST(test_mul_matches_the_model_and_the_hook_for_widths_1_to_16);
    RUN_TEST(test_mul_matches_the_model_and_the_hook_for_wide_operands);
    RUN_TEST(test_mul_with_unequal_operands_and_every_sign_mix);
    RUN_TEST(test_a_mixed_sign_mul_warns_once_and_is_unsigned);
    RUN_TEST(test_node_counts_are_pinned_for_small_multipliers);
    RUN_TEST(test_a_literal_x_operand_bit_makes_every_output_bit_x);
    RUN_TEST(test_a_mul_over_the_cell_cap_is_a_located_error_and_nothing_is_built);
    return UNITY_END();
}
