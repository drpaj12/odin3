/*
 * test_lower_div.c — 3B Task 5, part 2: $div, $mod and $pow (spec §5.6, §5.7). The registry has no
 * simulate hook for $div and $mod, so the lowered module is compared with an independent C model
 * of the Verilog operators (truncating division with C's `/` and `%`; the divisor-0 and
 * most-negative-over-minus-one corners stated from the spec, not from the circuit): widths 1 to
 * 24, signed, unsigned, unequal operands, Y narrower and wider. Gate counts against closed forms
 * written here, the size estimate, the netlist view. $pow is always the located error of §5.7
 * and builds nothing; a $div of width 3,000 is the located cap error.
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

enum { EXHAUSTIVE_BITS = 12, RANDOM_VECTORS = 512, LOG_TEXT = 4096, MODEL_W = 64 };

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

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

typedef struct dv_case {
    const char *type; /* "$div", "$mod" or "$pow" */
    uint32_t aw, bw, yw;
    bool a_signed, b_signed;
} dv_case;

static uint32_t work_width(const dv_case *cs) {
    uint32_t top = cs->aw > cs->bw ? cs->aw : cs->bw;
    return top > cs->yw ? top : cs->yw;
}

static odin3_node_id build_dv(lf *fix, const dv_case *cs, odin3_module **mod) {
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    lf_cell_spec cell = {cs->type, params, 5, "dut", {0}};
    return lf_one_cell(fix, &cell, mod);
}

/* --- the reference model ---------------------------------------------------------------------- */

static uint64_t mask_of(uint32_t width) {
    return width >= MODEL_W ? UINT64_MAX : ((uint64_t)1 << width) - 1;
}

/* An operand: `count` input bits, extended to `width` (by sign when is_signed). */
typedef struct operand {
    const uint8_t *bits;
    uint32_t count;
    uint32_t width;
    bool is_signed;
} operand;

static uint64_t pattern_of(const operand *op) {
    uint64_t val = 0;
    for (uint32_t k = 0; k < op->width; k++) {
        uint8_t bit = 0;
        if (k < op->count) {
            bit = op->bits[k];
        } else if (op->is_signed) {
            bit = op->bits[op->count - 1];
        }
        val |= (uint64_t)bit << k;
    }
    return val;
}

static int64_t sign_extend(uint64_t pattern, uint32_t width) {
    if (width < MODEL_W && ((pattern >> (width - 1)) & 1U) != 0) {
        pattern |= ~mask_of(width);
    }
    return (int64_t)pattern;
}

/* The WIDTH-bit patterns of A and B, the width, and which result is wanted. */
typedef struct dm_op {
    uint64_t num_pat;
    uint64_t den_pat;
    uint32_t width;
    bool want_rem;
} dm_op;

/* Unsigned: C's `/` and `%`; divisor 0 gives quotient all ones and remainder A (spec 5.6). */
static uint64_t unsigned_model(const dm_op *op) {
    if (op->den_pat == 0) {
        return op->want_rem ? op->num_pat : mask_of(op->width);
    }
    return (op->want_rem ? op->num_pat % op->den_pat : op->num_pat / op->den_pat) &
           mask_of(op->width);
}

/*
 * Both signed: C's truncating `/` and `%` on the sign-extended values (the most negative over -1
 * wraps to itself at `width` bits); divisor 0 gives quotient 1 for a negative A and all ones
 * otherwise, remainder A.
 */
static uint64_t signed_model(const dm_op *op) {
    int64_t num = sign_extend(op->num_pat, op->width);
    int64_t den = sign_extend(op->den_pat, op->width);
    if (den == 0) {
        if (op->want_rem) {
            return op->num_pat;
        }
        return num < 0 ? 1U : mask_of(op->width);
    }
    if (op->width == MODEL_W && num == INT64_MIN && den == -1) {
        return op->want_rem ? 0U : op->num_pat;
    }
    return (uint64_t)(op->want_rem ? num % den : num / den) & mask_of(op->width);
}

static uint64_t divmod_model(const dm_op *op, bool is_signed) {
    return is_signed ? signed_model(op) : unsigned_model(op);
}

static void dv_model(const lf_model_io *io, const void *user) {
    const dv_case *cs = user;
    uint32_t width = work_width(cs);
    bool is_signed = cs->a_signed && cs->b_signed;
    operand lhs = {io->in, cs->aw, width, is_signed};
    operand rhs = {io->in + cs->aw, cs->bw, width, is_signed};
    dm_op op = {pattern_of(&lhs), pattern_of(&rhs), width, strcmp(cs->type, "$mod") == 0};
    uint64_t result = divmod_model(&op, is_signed);
    for (uint32_t k = 0; k < cs->yw; k++) {
        io->out[k] = (uint8_t)((result >> k) & 1U);
    }
}

/* --- closed forms ------------------------------------------------------------------------------
 */

/* Gates of the compare >= at width n (4n + lcu(n) + 3: A - B without sums, P tree, OR, NOT, OR). */
static uint64_t ge_gates(uint64_t n) {
    return 4 * n + lw_lcu_gates(n) + 3;
}

/* The kept difference at width n over W bits: A - B with sums (4n + lcu(n)) and W multiplexers. */
static uint64_t keep_gates(uint64_t n, uint64_t width) {
    return 4 * n + lw_lcu_gates(n) + width;
}

static uint64_t neg_if_gates(uint64_t width) {
    return 4 * width + lw_lcu_gates(width) + width;
}

static uint64_t dv_gates(const dv_case *cs) {
    uint64_t width = work_width(cs);
    bool want_rem = strcmp(cs->type, "$mod") == 0;
    uint64_t gates = 0;
    for (uint64_t shift = 0; shift < width; shift++) {
        gates += ge_gates(width + shift);
        if (shift > 0 || want_rem) {
            gates += keep_gates(width + shift, width);
        }
    }
    if (cs->a_signed && cs->b_signed) {
        gates += 2 * neg_if_gates(width) + neg_if_gates(width) + (want_rem ? 0U : 1U);
    }
    return gates;
}

static void check_dv_case(const dv_case *cs) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "%s a=%u b=%u y=%u signed=%d/%d", cs->type, cs->aw, cs->bw,
                   cs->yw, cs->a_signed ? 1 : 0, cs->b_signed ? 1 : 0);
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_dv(&fix, cs, &mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(&fix), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(&fix, mod), msg);
    check_netlist_view(mod);
    uint32_t gates = lf_gate_nodes(&fix, mod);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(dv_gates(cs), gates, msg);
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    uint32_t widths[3] = {cs->aw, cs->bw, cs->yw};
    TEST_ASSERT_TRUE_MESSAGE(gates <= lf_estimate(cs->type, params, widths, 3), msg);
    lf_sim_opts sim = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 23};
    lf_sim_result res = lf_sim_model(fix.design, odin3_module_id_of(mod), dv_model, cs, &sim);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    if (cs->aw + cs->bw <= EXHAUSTIVE_BITS) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)1 << (cs->aw + cs->bw), res.vectors, msg);
    }
    lf_end(&fix);
}

static const char *const k_types[] = {"$div", "$mod"};

static void test_div_and_mod_match_the_model_for_equal_widths(void) {
    for (size_t ti = 0; ti < 2; ti++) {
        for (uint32_t width = 1; width <= 12; width++) {
            for (int sign = 0; sign < 2; sign++) {
                dv_case cs = {k_types[ti], width, width, width, sign != 0, sign != 0};
                check_dv_case(&cs);
                cs.yw = width > 1 ? width - 1 : 1; /* Y narrower */
                check_dv_case(&cs);
                cs.yw = width + 3; /* Y wider: the work width grows */
                check_dv_case(&cs);
            }
        }
    }
}

static void test_div_and_mod_match_the_model_for_unequal_operands_and_sign_mixes(void) {
    static const uint32_t k_pairs[][2] = {{1, 4}, {4, 1}, {3, 5}, {5, 3}, {7, 2}, {2, 7}};
    for (size_t ti = 0; ti < 2; ti++) {
        for (size_t pi = 0; pi < sizeof k_pairs / sizeof k_pairs[0]; pi++) {
            uint32_t wide = k_pairs[pi][0] > k_pairs[pi][1] ? k_pairs[pi][0] : k_pairs[pi][1];
            for (int sign = 0; sign < 4; sign++) {
                dv_case cs = {k_types[ti], k_pairs[pi][0],         k_pairs[pi][1],
                              wide,        sign == 1 || sign == 2, sign == 1 || sign == 3};
                check_dv_case(&cs);
            }
        }
    }
}

static void test_wide_dividers_with_a_short_divisor_hit_divisor_zero(void) {
    /* B of 2 bits is 0 for a quarter of the random vectors; A is wide, signed and not. */
    static const uint32_t k_wide[] = {17, 24};
    for (size_t ti = 0; ti < 2; ti++) {
        for (size_t wi = 0; wi < sizeof k_wide / sizeof k_wide[0]; wi++) {
            for (int sign = 0; sign < 2; sign++) {
                dv_case cs = {k_types[ti], k_wide[wi], 2, k_wide[wi], sign != 0, sign != 0};
                check_dv_case(&cs);
            }
        }
    }
}

static uint64_t model4(uint64_t num, uint64_t den, bool is_signed, bool want_rem) {
    dm_op op = {num, den, 4, want_rem};
    return divmod_model(&op, is_signed);
}

static void test_the_model_states_the_corners(void) {
    /* 4 bits: -8 / -1 wraps to -8 (remainder 0); x / 0 is all ones (-1 for a positive x; 1 for a
     * negative x), remainder x; -7 / 2 truncates toward zero. */
    TEST_ASSERT_EQUAL_UINT64(8, model4(8, 15, true, false));
    TEST_ASSERT_EQUAL_UINT64(0, model4(8, 15, true, true));
    TEST_ASSERT_EQUAL_UINT64(15, model4(5, 0, true, false));
    TEST_ASSERT_EQUAL_UINT64(1, model4(9, 0, true, false));
    TEST_ASSERT_EQUAL_UINT64(9, model4(9, 0, true, true));
    TEST_ASSERT_EQUAL_UINT64(13, model4(9, 2, true, false)); /* -7 / 2 = -3 */
    TEST_ASSERT_EQUAL_UINT64(15, model4(9, 2, true, true));  /* -7 % 2 = -1 */
    TEST_ASSERT_EQUAL_UINT64(15, model4(5, 0, false, false));
    TEST_ASSERT_EQUAL_UINT64(5, model4(5, 0, false, true));
}

static void test_the_corners_through_the_circuit(void) {
    /* The most negative number over -1, and the divisor 0, at 4 bits signed: exhaustive runs
     * cover them; here the lowered module is read at the two inputs by the model comparison. */
    dv_case div = {"$div", 4, 4, 4, true, true};
    check_dv_case(&div);
    dv_case mod = {"$mod", 4, 4, 4, true, true};
    check_dv_case(&mod);
}

static void test_node_counts_are_pinned_for_small_dividers(void) {
    static const struct {
        dv_case cs;
        uint32_t gates;
    } k_pins[] = {
        {{"$div", 1, 1, 1, false, false}, 9},  /* one compare: 3 + lcu(1) + 1 + 0 + 3 */
        {{"$mod", 1, 1, 1, false, false}, 16}, /* the compare and the kept difference (7) */
        {{"$div", 2, 2, 2, false, false}, 61},
        {{"$mod", 3, 3, 3, true, true}, 261},
    };
    for (size_t i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
        lf one;
        odin3_module *one_mod = NULL;
        lf_begin(&one);
        (void)build_dv(&one, &k_pins[i].cs, &one_mod);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&one));
        uint32_t gates = lf_gate_nodes(&one, one_mod);
        TEST_ASSERT_EQUAL_UINT64(dv_gates(&k_pins[i].cs), gates);
        TEST_ASSERT_EQUAL_UINT32(k_pins[i].gates, gates);
        lf_end(&one);
    }
}

static void test_a_mixed_sign_div_warns_once_and_is_unsigned(void) {
    dv_case cs = {"$div", 5, 3, 5, true, false};
    check_dv_case(&cs);
    const char *first = strstr(log_text, "has A_SIGNED != B_SIGNED");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "has A_SIGNED != B_SIGNED"));
}

/* --- the cap and $pow --------------------------------------------------------------------------
 */

static void test_a_div_of_width_3000_is_the_located_cap_error_and_nothing_is_built(void) {
    dv_case cs = {"$div", 3000, 3000, 3000, false, false};
    lf fix;
    odin3_module *mod = NULL;
    lf_begin(&fix);
    (void)build_dv(&fix, &cs, &mod);
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: module 'm': '$div' 'dut' would create "));
    TEST_ASSERT_NOT_NULL(strstr(log_text, " nodes (cap 16777216)"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    TEST_ASSERT_EQUAL_UINT32(1, lf_word_nodes(&fix, mod));
    TEST_ASSERT_EQUAL_UINT32(0, lf_gate_nodes(&fix, mod));
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&fix);
}

static void test_pow_is_the_located_error_and_nothing_is_built(void) {
    static const dv_case k_cases[] = {{"$pow", 4, 3, 8, false, false},
                                      {"$pow", 8, 8, 8, true, true}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        log_text[0] = '\0';
        lf fix;
        odin3_module *mod = NULL;
        lf_begin(&fix);
        (void)build_dv(&fix, &k_cases[i], &mod);
        odin3_strbuf before;
        odin3_strbuf after;
        odin3_strbuf_init(&before);
        odin3_strbuf_init(&after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_lower(&fix));
        TEST_ASSERT_NOT_NULL(
            strstr(log_text, "lower: module 'm': '$pow' 'dut' has no bit-level form (run opt "
                             "first; Yosys leaves it unmapped)"));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
        TEST_ASSERT_EQUAL_STRING(before.data, after.data);
        TEST_ASSERT_EQUAL_UINT32(1, lf_word_nodes(&fix, mod));
        TEST_ASSERT_EQUAL_UINT32(0, lf_gate_nodes(&fix, mod));
        odin3_strbuf_free(&before);
        odin3_strbuf_free(&after);
        lf_end(&fix);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_model_states_the_corners);
    RUN_TEST(test_div_and_mod_match_the_model_for_equal_widths);
    RUN_TEST(test_div_and_mod_match_the_model_for_unequal_operands_and_sign_mixes);
    RUN_TEST(test_wide_dividers_with_a_short_divisor_hit_divisor_zero);
    RUN_TEST(test_the_corners_through_the_circuit);
    RUN_TEST(test_node_counts_are_pinned_for_small_dividers);
    RUN_TEST(test_a_mixed_sign_div_warns_once_and_is_unsigned);
    RUN_TEST(test_a_div_of_width_3000_is_the_located_cap_error_and_nothing_is_built);
    RUN_TEST(test_pow_is_the_located_error_and_nothing_is_built);
    return UNITY_END();
}
