/*
 * test_lower_alu.c — 3B Task 4, part 2: the carry-lookahead unit, the adder and the cells built on
 * it (spec §5.4): lw_lcu against a software lookahead model for W = 1..70, lw_alu sums, carries
 * and propagate bits against integer arithmetic, then $add $sub $neg and $lt $le $gt $ge through
 * the pass: node counts against the closed forms and the size estimate, the netlist view, no word
 * cell left, and simulator-equal before and after (these cells have simulate hooks).
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_int.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LCU_MAX = 70,
    MAX_NETS = 4096,
    EXHAUSTIVE_BITS = 12,
    RANDOM_VECTORS = 4096,
    ALU_VECTORS = 64,
    ALU_MAX = 32,
    LOG_TEXT = 4096
};

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

/* --- the builder harness: nets, an open cell, an evaluator of the nodes it created ----------- */

static lf fix;
static lw_ctx ctx;
static odin3_module *mod;
static odin3_net_id inputs[MAX_NETS];
static uint32_t n_inputs;
static uint8_t value[MAX_NETS * 8];

static void harness_begin(uint32_t count) {
    lf_begin(&fix);
    lf_binary spec = {false, false, 1, 1, 1};
    odin3_value params[5];
    lf_binary_params(&spec, params);
    lf_cell_spec cs = {"$and", params, 5, "host", {0}};
    odin3_node_id host = lf_one_cell(&fix, &cs, &mod);
    TEST_ASSERT_TRUE(count <= MAX_NETS);
    n_inputs = count;
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, (odin3_prov_id){0}, &inputs[i]));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_ctx_init(&ctx, &fix.ctx, fix.design, NULL));
    lw_ctx_enter(&ctx, mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_begin(&ctx, host));
}

static void harness_end(void) {
    lw_rollback(&ctx);
    lw_ctx_free(&ctx);
    lf_end(&fix);
}

static const char *node_type(odin3_node_id node) {
    return odin3_celltype_get(fix.design, odin3_node_type(mod, node))->name;
}

static uint8_t gate_value(const char *type, const uint8_t *in) {
    if (strcmp(type, "$_NOT_") == 0) {
        return in[0] == 0 ? 1U : 0U;
    }
    if (strcmp(type, "$_AND_") == 0) {
        return in[0] & in[1];
    }
    if (strcmp(type, "$_OR_") == 0) {
        return in[0] | in[1];
    }
    if (strcmp(type, "$_XOR_") == 0) {
        return in[0] ^ in[1];
    }
    if (strcmp(type, "$_XNOR_") == 0) {
        return (in[0] ^ in[1]) == 0 ? 1U : 0U;
    }
    if (strcmp(type, "$_MUX_") == 0) {
        return in[2] != 0 ? in[1] : in[0];
    }
    return strcmp(type, "$_CONST1_") == 0 ? 1U : 0U;
}

/* Evaluates one created node from the values of its input nets. */
static void evaluate_node(odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    uint8_t in[3] = {0, 0, 0};
    TEST_ASSERT_TRUE(pins.count >= 1 && pins.count <= 4);
    for (uint32_t pin = 0; pin + 1 < pins.count; pin++) {
        in[pin] = value[odin3_pin_net(mod, (odin3_pin_id){pins.first.v + pin}).v];
    }
    odin3_net_id out = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + pins.count - 1});
    TEST_ASSERT_TRUE(out.v < sizeof value);
    value[out.v] = gate_value(node_type(node), in);
}

/* Evaluates the nodes the open cell created, in creation order, on the values set in `value`. */
static void evaluate_created(void) {
    for (size_t i = 0; i < ctx.created.len; i++) {
        evaluate_node(*(const odin3_node_id *)odin3_vec_cat(&ctx.created, i));
    }
}

static uint8_t net_value(odin3_net_id net) {
    TEST_ASSERT_TRUE(net.v < sizeof value);
    return value[net.v];
}

static void set_inputs_random(odin3_prng *prng) {
    for (uint32_t i = 0; i < n_inputs; i++) {
        value[inputs[i].v] = odin3_prng_bit(prng) ? 1U : 0U;
    }
}

/* --- lw_lcu --------------------------------------------------------------------------------- */

/* The software model: CO[i] = G[i] | (P[i] & CO[i - 1]) with CO[-1] = CI. */
typedef struct lcu_vector {
    uint8_t prop[LCU_MAX];
    uint8_t gen[LCU_MAX];
    uint8_t carry_in;
} lcu_vector;

static void lcu_model(const lcu_vector *vec, uint32_t width, uint8_t *carry) {
    uint8_t below = vec->carry_in;
    for (uint32_t i = 0; i < width; i++) {
        carry[i] = (uint8_t)(vec->gen[i] | (vec->prop[i] & below));
        below = carry[i];
    }
}

/* Gates of lw_lcu from the structure: g[0] (2 gates), and 3 per update of the two trees. */
static uint64_t lcu_pinned(uint32_t width) {
    static const uint64_t k_pins[] = {0, 2, 5, 8, 14};
    return width < sizeof k_pins / sizeof k_pins[0] ? k_pins[width] : 0;
}

static void check_lcu(uint32_t width, odin3_prng *prng) {
    harness_begin(2 * width + 1);
    const odin3_net_id *prop = inputs;
    const odin3_net_id *gen = inputs + (size_t)width;
    odin3_net_id carry_in = inputs[(size_t)width * 2];
    lw_bits carry;
    lw_bits_init(&carry);
    uint64_t before = ctx.gates;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, lw_lcu(&ctx, (lw_span){prop, width}, (lw_span){gen, width}, carry_in, &carry));
    TEST_ASSERT_EQUAL_UINT32(width, lw_bits_len(&carry));
    TEST_ASSERT_EQUAL_UINT64(lw_lcu_gates(width), ctx.gates - before);
    if (lcu_pinned(width) != 0) {
        TEST_ASSERT_EQUAL_UINT64(lcu_pinned(width), ctx.gates - before);
    }
    for (uint32_t trial = 0; trial < ALU_VECTORS; trial++) {
        set_inputs_random(prng);
        if (trial % 4 == 1) { /* long carry chains: every P high, one G low in the chain */
            for (uint32_t i = 0; i < width; i++) {
                value[prop[i].v] = 1U;
            }
        }
        evaluate_created();
        lcu_vector vec = {{0}, {0}, value[carry_in.v]};
        uint8_t want[LCU_MAX];
        for (uint32_t i = 0; i < width; i++) {
            vec.prop[i] = value[prop[i].v];
            vec.gen[i] = value[gen[i].v];
        }
        lcu_model(&vec, width, want);
        for (uint32_t i = 0; i < width; i++) {
            char msg[40];
            (void)snprintf(msg, sizeof msg, "lcu W=%u CO[%u]", width, i);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[i], net_value(lw_bits_at(&carry, i)), msg);
        }
    }
    lw_bits_free(&carry);
    harness_end();
}

static void test_lcu_matches_the_lookahead_model_for_1_to_70_bits(void) {
    odin3_prng prng;
    odin3_prng_seed(&prng, 3);
    for (uint32_t width = 1; width <= LCU_MAX; width++) {
        check_lcu(width, &prng);
    }
}

/* Independent of lw_lcu_gates: the update counts of the two trees, level by level, by listing. */
static uint64_t listed_updates(uint32_t width) {
    uint64_t count = 0;
    for (uint32_t level = 1; ((uint64_t)1 << (level - 1)) < width; level++) {
        uint64_t step = (uint64_t)1 << level;
        for (uint64_t bit = step - 1; bit < width; bit += step) {
            count++;
        }
        for (uint64_t bit = step + step / 2 - 1; bit < width; bit += step) {
            count++;
        }
    }
    return count;
}

static void test_lcu_gate_count_is_2_plus_3_per_tree_update(void) {
    for (uint32_t width = 1; width <= 200; width++) {
        TEST_ASSERT_EQUAL_UINT64(2 + 3 * listed_updates(width), lw_lcu_gates(width));
    }
}

/* --- lw_alu --------------------------------------------------------------------------------- */

typedef struct alu_case {
    uint32_t aw, bw, width;
    bool invert, is_signed, carry_in_one;
} alu_case;

static uint64_t mask_of(uint32_t width) {
    return width >= 64 ? UINT64_MAX : ((uint64_t)1 << width) - 1;
}

/* ext(v, from -> width): the low `from` bits of v, sign- or zero-extended, as `width` bits. */
static uint64_t extend_value(uint64_t val, uint32_t from, uint32_t width, bool is_signed) {
    uint64_t low = val & mask_of(from);
    bool negative = is_signed && ((low >> (from - 1)) & 1U) != 0;
    if (width <= from) {
        return low & mask_of(width);
    }
    return negative ? (low | (mask_of(width) & ~mask_of(from))) : low;
}

static uint64_t read_bits(const odin3_net_id *nets, uint32_t count) {
    uint64_t val = 0;
    for (uint32_t i = 0; i < count; i++) {
        val |= (uint64_t)value[nets[i].v] << i;
    }
    return val;
}

static uint64_t read_span(const lw_bits *bits) {
    uint64_t val = 0;
    for (uint32_t i = 0; i < lw_bits_len(bits); i++) {
        val |= (uint64_t)net_value(lw_bits_at(bits, i)) << i;
    }
    return val;
}

/* The carry out of every bit position of AA + BB + CI, as a bit mask. */
static uint64_t carries_of(uint64_t lhs, uint64_t rhs, uint32_t width, bool carry_in) {
    uint64_t carries = 0;
    for (uint32_t i = 0; i < width; i++) {
        uint64_t low = mask_of(i + 1);
        uint64_t total = (lhs & low) + (rhs & low) + (carry_in ? 1U : 0U);
        carries |= ((total >> (i + 1)) & 1U) << i;
    }
    return carries;
}

static void check_alu_vector(const alu_case *cs, odin3_net_id carry_in, const lw_alu_out *res,
                             const char *msg) {
    uint64_t lhs = extend_value(read_bits(inputs, cs->aw), cs->aw, cs->width, cs->is_signed);
    uint64_t rhs =
        extend_value(read_bits(inputs + cs->aw, cs->bw), cs->bw, cs->width, cs->is_signed);
    uint64_t rhs_used = cs->invert ? ~rhs & mask_of(cs->width) : rhs;
    uint32_t cin = cs->carry_in_one ? 1U : 0U;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)cin, net_value(carry_in), msg);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((lhs + rhs_used + cin) & mask_of(cs->width),
                                     read_span(&res->sum), msg);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(lhs ^ rhs_used, read_span(&res->prop), msg);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(carries_of(lhs, rhs_used, cs->width, cs->carry_in_one),
                                     read_span(&res->carry), msg);
}

static void check_alu(const alu_case *cs, odin3_prng *prng) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "alu a=%u b=%u W=%u inv=%d sgn=%d ci=%d", cs->aw, cs->bw,
                   cs->width, cs->invert ? 1 : 0, cs->is_signed ? 1 : 0, cs->carry_in_one ? 1 : 0);
    harness_begin(cs->aw + cs->bw);
    lw_alu_args args = {
        {inputs, cs->aw}, {inputs + cs->aw, cs->bw}, {0}, cs->invert, cs->is_signed, true,
        cs->width};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, lw_const(&ctx, cs->carry_in_one ? ODIN3_BIT_1 : ODIN3_BIT_0, &args.carry_in));
    lw_alu_out res;
    lw_alu_out_init(&res);
    uint64_t before = ctx.gates;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, lw_alu(&ctx, &args, &res), msg);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(lw_alu_gates(cs->width, cs->invert, true), ctx.gates - before,
                                     msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(cs->width, lw_bits_len(&res.sum), msg);
    for (uint32_t trial = 0; trial < ALU_VECTORS; trial++) {
        set_inputs_random(prng);
        evaluate_created();
        check_alu_vector(cs, args.carry_in, &res, msg);
    }
    lw_alu_out_free(&res);
    harness_end();
}

static void test_alu_sum_propagate_and_carries_match_integer_arithmetic(void) {
    static const uint32_t k_widths[] = {1, 2, 3, 5, 8, 17, ALU_MAX};
    odin3_prng prng;
    odin3_prng_seed(&prng, 5);
    for (size_t wi = 0; wi < sizeof k_widths / sizeof k_widths[0]; wi++) {
        uint32_t width = k_widths[wi];
        for (int flags = 0; flags < 8; flags++) {
            alu_case cs = {width,           width, width, (flags & 1) != 0, (flags & 2) != 0,
                           (flags & 4) != 0};
            check_alu(&cs, &prng);
            cs.aw = width > 1 ? width - 1 : 1; /* a narrower A is extended */
            check_alu(&cs, &prng);
            cs.aw = width;
            cs.bw = width + 3 > ALU_MAX ? ALU_MAX : width + 3; /* a wider B is truncated */
            check_alu(&cs, &prng);
        }
    }
}

static void test_alu_without_sum_bits_builds_only_the_flags_inputs(void) {
    harness_begin(8);
    lw_alu_args args = {{inputs, 4}, {inputs + 4, 4}, {0}, true, false, false, 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_1, &args.carry_in));
    lw_alu_out res;
    lw_alu_out_init(&res);
    uint64_t before = ctx.gates;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_alu(&ctx, &args, &res));
    TEST_ASSERT_EQUAL_UINT32(0, lw_bits_len(&res.sum));
    TEST_ASSERT_EQUAL_UINT32(4, lw_bits_len(&res.prop));
    TEST_ASSERT_EQUAL_UINT32(4, lw_bits_len(&res.carry));
    TEST_ASSERT_EQUAL_UINT64(lw_alu_gates(4, true, false), ctx.gates - before);
    TEST_ASSERT_EQUAL_UINT64(lw_alu_gates(4, true, true) - 4, ctx.gates - before);
    lw_alu_out_free(&res);
    harness_end();
}

/* --- the cells through the pass --------------------------------------------------------------- */

typedef struct ar_case {
    const char *type;
    uint32_t aw, bw, yw;
    bool a_signed, b_signed;
} ar_case;

static bool is_unary(const char *type) {
    return strcmp(type, "$neg") == 0;
}

static odin3_node_id build_cell(lf *target, const ar_case *cs, odin3_module **out_mod) {
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_cell_spec cell = {cs->type, params, 5, "dut", {0}};
    if (is_unary(cs->type)) {
        lf_unary_params(&spec, params);
        cell.n_params = 3;
    } else {
        lf_binary_params(&spec, params);
    }
    return lf_one_cell(target, &cell, out_mod);
}

/* Gates the closed form gives (alumacc.cc/techmap.v structure; lw_lcu_gates for the lookahead). */
static uint64_t cell_gates(const ar_case *cs) {
    uint64_t wide = cs->aw > cs->bw ? cs->aw : cs->bw;
    if (strcmp(cs->type, "$add") == 0) {
        return 3 * (uint64_t)cs->yw + lw_lcu_gates(cs->yw);
    }
    if (strcmp(cs->type, "$sub") == 0) {
        return 4 * (uint64_t)cs->yw + lw_lcu_gates(cs->yw);
    }
    if (strcmp(cs->type, "$neg") == 0) {
        return 4 * (uint64_t)cs->yw + lw_lcu_gates(cs->yw);
    }
    bool is_signed = cs->a_signed && cs->b_signed;
    uint64_t gates = 3 * wide + lw_lcu_gates(wide) + (is_signed ? 3U : 1U);
    if (strcmp(cs->type, "$lt") == 0) {
        return gates;
    }
    gates += wide - 1;
    if (strcmp(cs->type, "$le") == 0) {
        return gates + 1;
    }
    return gates + (strcmp(cs->type, "$gt") == 0 ? 2U : 3U);
}

static odin3_status run_lower(lf *target) {
    return odin3_pass_run(target->design, "lower", (odin3_bytes){NULL, 0});
}

static uint32_t input_bits(const ar_case *cs) {
    return is_unary(cs->type) ? cs->aw : cs->aw + cs->bw;
}

static void run_cell_case(const ar_case *cs) {
    char msg[80];
    (void)snprintf(msg, sizeof msg, "%s a=%u b=%u y=%u signed=%d/%d", cs->type, cs->aw, cs->bw,
                   cs->yw, cs->a_signed ? 1 : 0, cs->b_signed ? 1 : 0);
    lf after;
    odin3_module *after_mod = NULL;
    lf_begin(&after);
    (void)build_cell(&after, cs, &after_mod);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_lower(&after), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(&after, after_mod), msg);
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_module(after_mod, opts), msg);
    uint32_t gates = lf_gate_nodes(&after, after_mod);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(cell_gates(cs), gates, msg);
    odin3_value params[5];
    lf_binary spec = {cs->a_signed, cs->b_signed, cs->aw, cs->bw, cs->yw};
    lf_binary_params(&spec, params);
    uint32_t widths[3] = {cs->aw, is_unary(cs->type) ? cs->yw : cs->bw, cs->yw};
    uint32_t n_ports = 3;
    if (is_unary(cs->type)) {
        lf_unary_params(&spec, params);
        widths[1] = cs->yw;
        n_ports = 2;
    }
    TEST_ASSERT_TRUE_MESSAGE(gates <= lf_estimate(cs->type, params, widths, n_ports), msg);
    lf before;
    odin3_module *before_mod = NULL;
    lf_begin(&before);
    (void)build_cell(&before, cs, &before_mod);
    lf_sim_opts sim = {EXHAUSTIVE_BITS, RANDOM_VECTORS, 13};
    lf_sim_result res = lf_sim_equal(before.design, odin3_module_id_of(before_mod), after.design,
                                     odin3_module_id_of(after_mod), &sim);
    TEST_ASSERT_TRUE_MESSAGE(res.equal, res.why);
    if (input_bits(cs) <= EXHAUSTIVE_BITS) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)1 << input_bits(cs), res.vectors, msg);
    }
    lf_end(&before);
    lf_end(&after);
}

static const uint32_t k_widths[] = {1, 2, 3, 4, 5, 8, 17, 33, 64, 65};
static const uint32_t k_pairs[][2] = {{3, 5}, {5, 3}, {1, 4}, {8, 17}, {33, 2}, {65, 64}};

/* Equal operand widths, each sign, the result as wide as, wider and narrower than the operands. */
static void equal_width_sweep(const char *type, bool compare) {
    for (size_t wi = 0; wi < sizeof k_widths / sizeof k_widths[0]; wi++) {
        uint32_t width = k_widths[wi];
        for (int sign = 0; sign < 2; sign++) {
            ar_case cs = {type, width, width, compare ? 1U : width, sign != 0, sign != 0};
            run_cell_case(&cs);
            cs.yw = compare ? 4U : width + 3; /* wider than the operands */
            run_cell_case(&cs);
            if (!compare && width > 2) {
                cs.yw = width - 1; /* narrower */
                run_cell_case(&cs);
            }
        }
    }
}

/* Unequal operand widths, all four sign mixes (none, both, A only, B only). */
static void unequal_width_sweep(const char *type, bool compare) {
    for (size_t pi = 0; pi < sizeof k_pairs / sizeof k_pairs[0]; pi++) {
        uint32_t wide = k_pairs[pi][0] > k_pairs[pi][1] ? k_pairs[pi][0] : k_pairs[pi][1];
        for (int sign = 0; sign < 4; sign++) {
            ar_case cs = {type,
                          k_pairs[pi][0],
                          k_pairs[pi][1],
                          compare ? 1U : wide,
                          sign == 1 || sign == 2,
                          sign == 1 || sign == 3};
            run_cell_case(&cs);
        }
    }
}

static void binary_sweep(const char *type, bool compare) {
    equal_width_sweep(type, compare);
    unequal_width_sweep(type, compare);
}

static void test_add_matches_the_closed_form_and_the_hook(void) {
    binary_sweep("$add", false);
}

static void test_sub_matches_the_closed_form_and_the_hook(void) {
    binary_sweep("$sub", false);
}

static void test_neg_matches_the_closed_form_and_the_hook(void) {
    for (size_t wi = 0; wi < sizeof k_widths / sizeof k_widths[0]; wi++) {
        uint32_t width = k_widths[wi];
        for (int sign = 0; sign < 2; sign++) {
            ar_case cs = {"$neg", width, 0, width, sign != 0, false};
            run_cell_case(&cs);
            cs.yw = width + 3;
            run_cell_case(&cs);
            cs.yw = width > 2 ? width - 1 : 1;
            run_cell_case(&cs);
        }
    }
}

static void test_node_counts_are_pinned_for_small_widths(void) {
    static const struct {
        ar_case cs;
        uint32_t gates;
    } k_pins[] = {
        {{"$add", 1, 1, 1, false, false}, 5},  /* 3 + lcu(1) = 3 + 2 */
        {{"$add", 3, 3, 3, false, false}, 17}, /* 9 + 8 */
        {{"$add", 4, 4, 4, false, false}, 26}, /* 12 + 14 */
        {{"$sub", 4, 4, 4, false, false}, 30}, /* 16 + 14 */
        {{"$neg", 2, 0, 2, false, false}, 13}, /* 8 + 5 */
    };
    for (size_t i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
        TEST_ASSERT_EQUAL_UINT64(k_pins[i].gates, cell_gates(&k_pins[i].cs));
        lf one;
        odin3_module *one_mod = NULL;
        lf_begin(&one);
        (void)build_cell(&one, &k_pins[i].cs, &one_mod);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_lower(&one));
        TEST_ASSERT_EQUAL_UINT32(k_pins[i].gates, lf_gate_nodes(&one, one_mod));
        lf_end(&one);
    }
}

static void test_a_mixed_sign_cell_is_lowered_as_unsigned_and_warns_once(void) {
    static const char *const k_types[] = {"$add", "$sub"};
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        log_text[0] = '\0';
        ar_case cs = {k_types[i], 5, 3, 5U, true, false};
        run_cell_case(&cs);
        const char *first = strstr(log_text, "has A_SIGNED != B_SIGNED");
        TEST_ASSERT_NOT_NULL_MESSAGE(first, k_types[i]);
        TEST_ASSERT_NULL_MESSAGE(strstr(first + 1, "has A_SIGNED != B_SIGNED"), k_types[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lcu_matches_the_lookahead_model_for_1_to_70_bits);
    RUN_TEST(test_lcu_gate_count_is_2_plus_3_per_tree_update);
    RUN_TEST(test_alu_sum_propagate_and_carries_match_integer_arithmetic);
    RUN_TEST(test_alu_without_sum_bits_builds_only_the_flags_inputs);
    RUN_TEST(test_add_matches_the_closed_form_and_the_hook);
    RUN_TEST(test_sub_matches_the_closed_form_and_the_hook);
    RUN_TEST(test_neg_matches_the_closed_form_and_the_hook);
    RUN_TEST(test_node_counts_are_pinned_for_small_widths);
    RUN_TEST(test_a_mixed_sign_cell_is_lowered_as_unsigned_and_warns_once);
    return UNITY_END();
}
