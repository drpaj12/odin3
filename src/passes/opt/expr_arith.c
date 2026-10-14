/*
 * expr_arith.c — opt_expr's rules on arithmetic (2F spec §4.3; Yosys 0.55 opt_expr.cc:
 * identwrt.a/b, the $pow strength reduction, mul_shift.*, mul_low_zeros, divmod_zero, div_shift,
 * mod_mask).
 */
#include "passes/opt/expr.h"

#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_A ODIN3_EXPR_PORT_A
#define PORT_B ODIN3_EXPR_PORT_B

enum {
    SHIFT_AMOUNT_BITS = 32, /* Yosys's Const(int) */
    N_BIN_PARAMS = ODIN3_EXPR_P_BIN_YW + 1,
    N_UN_PARAMS = ODIN3_EXPR_P_UN_YW + 1
};

/* An unsigned number and the width it is written in. */
typedef struct number {
    uint32_t value, bits;
} number;

/* A constant holding the number, LSB first (bits above the value's own are 0). */
static odin3_status const_value(odin3_expr_ctx *ec, number num, odin3_expr_sig *out) {
    odin3_net_id zero = {0};
    odin3_net_id one = {0};
    odin3_status st = odin3_opt_const_net(ec->oc, ODIN3_BIT_0, &zero);
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, ODIN3_BIT_1, &one) : st;
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, num.bits, out) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < num.bits; k++) {
        out->net[k] = k < SHIFT_AMOUNT_BITS && (num.value >> k & 1U) != 0 ? one : zero;
    }
    return st;
}

/* `ones` ones, then zeros up to width (the mask of a power of two minus one). */
static odin3_status ones_mask(odin3_expr_ctx *ec, uint32_t ones, uint32_t width,
                              odin3_expr_sig *out) {
    odin3_expr_sig low;
    odin3_expr_sig high;
    odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_1, ones, &low);
    st = st == ODIN3_OK ? odin3_expr_fill(ec, ODIN3_BIT_0, width - ones, &high) : st;
    return st == ODIN3_OK ? odin3_expr_join(ec, &low, &high, out) : st;
}

static bool const_is(const odin3_expr_ctx *ec, odin3_expr_port port, uint32_t bit,
                     odin3_const want) {
    return odin3_expr_cval(ec, odin3_expr_net(ec, port, bit)) == want;
}

/* Yosys is_onehot: every bit 0 or 1, exactly one of them 1, at *pos. */
static bool onehot(const odin3_expr_ctx *ec, odin3_expr_port port, uint32_t *pos) {
    uint32_t width = odin3_expr_width(ec, port);
    bool found = false;
    for (uint32_t k = 0; k < width; k++) {
        if (const_is(ec, port, k, ODIN3_CONST_1)) {
            if (found) {
                return false;
            }
            found = true;
            *pos = k;
        } else if (!const_is(ec, port, k, ODIN3_CONST_0)) {
            return false;
        }
    }
    return found;
}

/* --- identwrt.a, identwrt.b (consume_x only) -------------------------------------------------- */

typedef enum iw_side { IW_NONE, IW_A, IW_B } iw_side;

/* Constant and without a 1 bit (Yosys: is_fully_const() && as_bool() == false; x and z read 0). */
static bool const_false(const odin3_expr_ctx *ec, odin3_expr_port port) {
    odin3_expr_census census = odin3_expr_port_census(ec, port);
    return odin3_expr_fully_const(census, false) && census.one == 0;
}

/* Yosys is_one_or_minus_one on the constant at port. */
static bool one_or_minus_one(const odin3_expr_ctx *ec, odin3_expr_port port, bool is_signed,
                             bool *negative) {
    odin3_expr_census census = odin3_expr_port_census(ec, port);
    uint32_t width = census.n;
    if (!odin3_expr_fully_const(census, false)) {
        return false;
    }
    if (width == 1) {
        *negative = census.one == 1 && is_signed;
        return census.one == 1;
    }
    bool last_bit_one = const_is(ec, port, 0, ODIN3_CONST_1) && census.zero == width - 1;
    if (census.one == width && is_signed) {
        *negative = true;
        return true;
    }
    return last_bit_one;
}

/* Yosys: b.size() <= 32 && b.as_int() == 1 (a bit that is not 1 reads as 0). */
static bool is_int_one(const odin3_expr_ctx *ec, odin3_expr_port port) {
    odin3_expr_census census = odin3_expr_port_census(ec, port);
    return odin3_expr_fully_const(census, false) && census.n <= SHIFT_AMOUNT_BITS &&
           census.one == 1 && const_is(ec, port, 0, ODIN3_CONST_1);
}

/* Which operand the cell reduces to; *negative for a multiplication by -1. (Shifts by 0 are not
 * here: a constant amount is constshift's, which runs first.) */
static iw_side identity_of(const odin3_expr_ctx *ec, bool *negative) {
    bool wrt_a = false;
    bool wrt_b = false;
    switch (ec->kind) {
    case ODIN3_EXPR_T_ADD:
    case ODIN3_EXPR_T_OR:
    case ODIN3_EXPR_T_XOR:
        wrt_b = const_false(ec, PORT_A);
        wrt_a = const_false(ec, PORT_B);
        break;
    case ODIN3_EXPR_T_SUB:
        wrt_a = const_false(ec, PORT_B);
        break;
    case ODIN3_EXPR_T_MUL:
        /* a constant is -1 only when the cell is signed (both flags: odin3_expr_both_signed) */
        if (one_or_minus_one(ec, PORT_A, odin3_expr_both_signed(ec), negative)) {
            wrt_b = true;
        } else {
            wrt_a = one_or_minus_one(ec, PORT_B, odin3_expr_both_signed(ec), negative);
        }
        break;
    case ODIN3_EXPR_T_DIV:
        wrt_a = is_int_one(ec, PORT_B);
        /* a 1-bit signed 1 is -1 (Yosys's `as_int` reads it as +1: §17 #30) */
        *negative = wrt_a && odin3_expr_both_signed(ec) && odin3_expr_width(ec, PORT_B) == 1;
        break;
    default:
        break;
    }
    /* -1 only when both operands are signed (§17 #30); on a mixed cell 1'1 is +1, as simlib, the
     * registry evaluator and the fold read it (§17 #57), so `negative` is never set there */
    if (wrt_a) {
        return IW_A;
    }
    return wrt_b ? IW_B : IW_NONE;
}

/* Y = the operand (Yosys's $pos: wiring here) or its negation ($neg). */
odin3_status odin3_expr_rule_identwrt(odin3_expr_ctx *ec) {
    if (!ec->consume_x) {
        return ODIN3_OK;
    }
    bool negative = false;
    iw_side side = identity_of(ec, &negative);
    if (side == IW_NONE) {
        return ODIN3_OK;
    }
    odin3_expr_port port = side == IW_A ? PORT_A : PORT_B;
    /* The kept operand reaches Y extended as the cell extends it: by sign only when both operands
     * are signed. Yosys 0.55 uses the operand's own flag (a `$pos` with it); the ruling of §17 #57
     * is simlib's, so a mixed-sign A - 0 with A signed zero-extends A. */
    bool is_signed = odin3_expr_both_signed(ec);
    odin3_opt_rule rule = side == IW_A ? ODIN3_OPT_RULE_IDENTWRT_A : ODIN3_OPT_RULE_IDENTWRT_B;
    uint32_t y_width = odin3_expr_y_width(ec);
    odin3_expr_sig operand;
    odin3_status st = odin3_expr_read(ec, port, &operand);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!negative) {
        odin3_expr_sig whole;
        st = odin3_expr_extend(ec, &operand, y_width, is_signed, &whole);
        return st == ODIN3_OK ? odin3_expr_wire(ec, rule, &whole) : st;
    }
    odin3_expr_cell spec = {.type = ODIN3_EXPR_T_NEG, .n_params = N_UN_PARAMS, .in = {&operand}};
    spec.params[ODIN3_EXPR_P_UN_SIGNED] = is_signed ? 1 : 0;
    spec.params[ODIN3_EXPR_P_UN_AW] = operand.n;
    spec.params[ODIN3_EXPR_P_UN_YW] = y_width;
    return odin3_expr_rebuild(ec, rule, &spec);
}

/* --- $pow ------------------------------------------------------------------------------------- */

/* The number of bits in value (0 for 0). */
static uint32_t bit_length(uint32_t value) {
    uint32_t bits = 0;
    for (; value != 0; value >>= 1) {
        bits++;
    }
    return bits;
}

/*
 * A constant one-hot base and an unsigned exponent: 2 ** b is 1 << b; 2^k ** b is 1 << (k * b), the
 * product in a new $mul. Yosys 0.55 forgets to clear A_SIGNED in the first case and to exclude a
 * signed base whose one-hot bit is its sign; both would change the value, so this follows the later
 * Yosys (external/yosys, passes/opt/opt_expr.cc): A_SIGNED 0 and no signed MSB. Both Yosys versions
 * also make the product k * b only Y_WIDTH bits wide, so it wraps (4 ** 128 in 8 bits gives 1, not
 * 0); here it is B_WIDTH + bit_length(k) bits, which holds it, and a $shl by an amount of Y_WIDTH
 * or more gives 0 (§17 #29).
 */
odin3_status odin3_expr_rule_pow(odin3_expr_ctx *ec) {
    uint32_t index = 0;
    uint32_t a_width = odin3_expr_width(ec, PORT_A);
    if (ec->kind != ODIN3_EXPR_T_POW || odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED) != 0 ||
        !onehot(ec, PORT_A, &index) ||
        (odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED) != 0 && index == a_width - 1)) {
        return ODIN3_OK;
    }
    uint32_t y_width = odin3_expr_y_width(ec);
    odin3_expr_sig one;
    odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_1, 1, &one);
    odin3_expr_cell shl = {.type = ODIN3_EXPR_T_SHL, .n_params = N_BIN_PARAMS, .in = {&one}};
    shl.params[ODIN3_EXPR_P_BIN_AW] = 1;
    shl.params[ODIN3_EXPR_P_BIN_BW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BW);
    shl.params[ODIN3_EXPR_P_BIN_YW] = y_width;
    odin3_expr_sig exponent = {NULL, 0};
    odin3_expr_sig amount = {NULL, 0};
    odin3_expr_sig product = {NULL, 0};
    if (st == ODIN3_OK && index != 1) { /* 2^k: the product k * b feeds the shift */
        st = odin3_expr_read(ec, PORT_B, &exponent);
        st = st == ODIN3_OK ? const_value(ec, (number){index, a_width}, &amount) : st;
        odin3_expr_cell mul = {
            .type = ODIN3_EXPR_T_MUL, .n_params = N_BIN_PARAMS, .in = {&amount, &exponent}};
        mul.params[ODIN3_EXPR_P_BIN_AW] = a_width;
        mul.params[ODIN3_EXPR_P_BIN_BW] = exponent.n;
        uint32_t product_width = exponent.n + bit_length(index);
        mul.params[ODIN3_EXPR_P_BIN_YW] = product_width;
        st = st == ODIN3_OK ? odin3_expr_new_cell(ec, &mul, NULL, &product) : st;
        shl.in[1] = &product;
        shl.params[ODIN3_EXPR_P_BIN_BW] = product_width;
    }
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_POW_SHIFT, &shl) : st;
}

/* --- $mul ----------------------------------------------------------------------------------- */

static uint32_t low_zeros(const odin3_expr_ctx *ec, odin3_expr_port port) {
    uint32_t width = odin3_expr_width(ec, port);
    uint32_t zeros = 0;
    while (zeros < width && const_is(ec, port, zeros, ODIN3_CONST_0)) {
        zeros++;
    }
    return zeros;
}

/* The constant operand is a power of two: a $shl of the other by its index. */
static odin3_status mul_to_shift(odin3_expr_ctx *ec, odin3_expr_port var_port, bool swapped,
                                 uint32_t exp) {
    odin3_expr_sig operand;
    odin3_expr_sig amount;
    odin3_status st = odin3_expr_read(ec, var_port, &operand);
    st = st == ODIN3_OK ? const_value(ec, (number){exp, SHIFT_AMOUNT_BITS}, &amount) : st;
    odin3_expr_cell spec = {
        .type = ODIN3_EXPR_T_SHL, .n_params = N_BIN_PARAMS, .in = {&operand, &amount}};
    /* the variable is sign-extended only when the cell is signed (both flags) */
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = odin3_expr_both_signed(ec) ? 1 : 0;
    spec.params[ODIN3_EXPR_P_BIN_AW] = operand.n;
    spec.params[ODIN3_EXPR_P_BIN_BW] = SHIFT_AMOUNT_BITS;
    spec.params[ODIN3_EXPR_P_BIN_YW] = odin3_expr_y_width(ec);
    return st == ODIN3_OK ? odin3_expr_rebuild(ec,
                                               swapped ? ODIN3_OPT_RULE_MUL_SHIFT_SWAPPED
                                                       : ODIN3_OPT_RULE_MUL_SHIFT_UNSWAPPED,
                                               &spec)
                          : st;
}

/* Operands with low zero bits: a narrower $mul on the rest, Y's low bits 0. */
static odin3_status mul_low_zeros(odin3_expr_ctx *ec) {
    uint32_t a_zeros = low_zeros(ec, PORT_A);
    uint32_t b_zeros = low_zeros(ec, PORT_B);
    uint32_t a_width = odin3_expr_width(ec, PORT_A);
    uint32_t b_width = odin3_expr_width(ec, PORT_B);
    uint32_t y_width = odin3_expr_y_width(ec);
    uint32_t y_zeros = a_zeros + b_zeros;
    if (y_zeros == 0) {
        return ODIN3_OK;
    }
    odin3_expr_sig zeros;
    odin3_status st =
        odin3_expr_fill(ec, ODIN3_BIT_0, y_zeros < y_width ? y_zeros : y_width, &zeros);
    if (st != ODIN3_OK || y_zeros >= y_width || a_zeros == a_width || b_zeros == b_width) {
        /* every Y bit is 0 (an all-zero operand too, where Yosys would leave a 0-bit port) */
        return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_MUL_LOW_ZEROS, &zeros) : st;
    }
    odin3_expr_sig lhs;
    odin3_expr_sig rhs;
    st = odin3_expr_read(ec, PORT_A, &lhs);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &rhs) : st;
    odin3_expr_sig lhs_rest = odin3_expr_slice(&lhs, a_zeros, a_width - a_zeros);
    odin3_expr_sig rhs_rest = odin3_expr_slice(&rhs, b_zeros, b_width - b_zeros);
    odin3_expr_cell spec = {
        .type = ODIN3_EXPR_T_MUL, .n_params = N_BIN_PARAMS, .in = {&lhs_rest, &rhs_rest}};
    /* equal flags, the cell's own signedness (a mixed-sign cell is unsigned: §17 #57) */
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = odin3_expr_both_signed(ec) ? 1 : 0;
    spec.params[ODIN3_EXPR_P_BIN_BSIGNED] = odin3_expr_both_signed(ec) ? 1 : 0;
    spec.params[ODIN3_EXPR_P_BIN_AW] = lhs_rest.n;
    spec.params[ODIN3_EXPR_P_BIN_BW] = rhs_rest.n;
    spec.params[ODIN3_EXPR_P_BIN_YW] = y_width - y_zeros;
    odin3_node_id fresh = {0};
    odin3_expr_sig narrow;
    odin3_expr_sig whole;
    st = st == ODIN3_OK ? odin3_expr_new_cell(ec, &spec, &fresh, &narrow) : st;
    st = st == ODIN3_OK ? odin3_expr_join(ec, &zeros, &narrow, &whole) : st;
    return st == ODIN3_OK ? odin3_expr_wire_keep(ec, ODIN3_OPT_RULE_MUL_LOW_ZEROS, &whole, fresh)
                          : st;
}

odin3_status odin3_expr_rule_mul(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_MUL || ec->keepdc) {
        return ODIN3_OK;
    }
    bool swapped = odin3_expr_fully_const(odin3_expr_port_census(ec, PORT_B), false);
    odin3_expr_port cst_port = swapped ? PORT_B : PORT_A;
    odin3_expr_port var_port = swapped ? PORT_A : PORT_B;
    odin3_expr_census census = odin3_expr_port_census(ec, cst_port);
    uint32_t exp = 0;
    if (odin3_expr_fully_zero(census)) {
        odin3_expr_sig zeros;
        odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_0, odin3_expr_y_width(ec), &zeros);
        return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_MUL_SHIFT_ZERO, &zeros) : st;
    }
    /* a one bit that is the constant's sign bit is negative only when the cell is signed */
    bool cst_signed = odin3_expr_both_signed(ec);
    if (odin3_expr_fully_def(census) && onehot(ec, cst_port, &exp) &&
        !(cst_signed && exp == census.n - 1)) {
        return mul_to_shift(ec, var_port, swapped, exp);
    }
    return mul_low_zeros(ec);
}

/* --- $div, $mod ------------------------------------------------------------------------------- */

/* The truncation fix of a signed division or remainder by 2^exp: A[msb] & |A[exp-1:0]. */
static odin3_status trunc_fix(odin3_expr_ctx *ec, uint32_t exp, const odin3_expr_sig *data,
                              odin3_net_id *fix) {
    uint32_t low = exp < data->n ? exp : data->n;
    odin3_expr_sig low_bits = odin3_expr_slice(data, 0, low);
    odin3_expr_sig sign = odin3_expr_slice(data, data->n - 1, 1);
    odin3_expr_sig nonzero;
    odin3_expr_sig both;
    odin3_status st = odin3_expr_new_un(ec, ODIN3_EXPR_T_REDUCE_OR, &low_bits, 1, &nonzero);
    st = st == ODIN3_OK ? odin3_expr_new_bin(ec, ODIN3_EXPR_T_AND, &sign, &nonzero, &both) : st;
    if (st == ODIN3_OK) {
        *fix = both.net[0];
    }
    return st;
}

/* $div by 2^exp: an arithmetic shift right; signed and truncating, plus the fix on the flooring. */
static odin3_status div_to_shift(odin3_expr_ctx *ec, uint32_t exp) {
    bool a_signed = odin3_expr_both_signed(ec); /* a mixed-sign division is unsigned (logical shift) */
    uint32_t y_width = odin3_expr_y_width(ec);
    odin3_expr_sig dividend;
    odin3_expr_sig amount;
    odin3_status st = odin3_expr_read(ec, PORT_A, &dividend);
    st = st == ODIN3_OK ? const_value(ec, (number){exp, SHIFT_AMOUNT_BITS}, &amount) : st;
    odin3_expr_cell shift = {
        .type = ODIN3_EXPR_T_SSHR, .n_params = N_BIN_PARAMS, .in = {&dividend, &amount}};
    shift.params[ODIN3_EXPR_P_BIN_ASIGNED] = a_signed ? 1 : 0;
    shift.params[ODIN3_EXPR_P_BIN_AW] = dividend.n;
    shift.params[ODIN3_EXPR_P_BIN_BW] = SHIFT_AMOUNT_BITS;
    shift.params[ODIN3_EXPR_P_BIN_YW] = y_width;
    if (st != ODIN3_OK || !a_signed || exp == 0) {
        return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_DIV_SHIFT, &shift) : st;
    }
    /* truncating = flooring + (A < 0 && the shifted-out bits are not all 0) */
    odin3_expr_sig flooring;
    odin3_expr_sig add_bit;
    odin3_net_id fix = {0};
    st = odin3_expr_new_cell(ec, &shift, NULL, &flooring);
    st = st == ODIN3_OK ? trunc_fix(ec, exp, &dividend, &fix) : st;
    st = st == ODIN3_OK ? odin3_expr_one(ec, fix, &add_bit) : st;
    odin3_expr_cell sum = {
        .type = ODIN3_EXPR_T_ADD, .n_params = N_BIN_PARAMS, .in = {&flooring, &add_bit}};
    sum.params[ODIN3_EXPR_P_BIN_AW] = y_width;
    sum.params[ODIN3_EXPR_P_BIN_BW] = 1;
    sum.params[ODIN3_EXPR_P_BIN_YW] = y_width;
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_DIV_SHIFT, &sum) : st;
}

/* $mod by 2^exp, signed and truncating: {A[msb] & |A[exp-1:0], A[exp-1:0]} sign-extended (wiring).
 */
static odin3_status mod_signed(odin3_expr_ctx *ec, uint32_t exp) {
    uint32_t y_width = odin3_expr_y_width(ec);
    odin3_expr_sig dividend;
    odin3_net_id fix = {0};
    odin3_expr_sig sign_sig;
    odin3_expr_sig whole;
    odin3_status st = odin3_expr_read(ec, PORT_A, &dividend);
    st = st == ODIN3_OK ? trunc_fix(ec, exp, &dividend, &fix) : st;
    st = st == ODIN3_OK ? odin3_expr_one(ec, fix, &sign_sig) : st;
    odin3_expr_sig low = odin3_expr_slice(&dividend, 0, exp < dividend.n ? exp : dividend.n);
    odin3_expr_sig value;
    st = st == ODIN3_OK ? odin3_expr_join(ec, &low, &sign_sig, &value) : st;
    st = st == ODIN3_OK ? odin3_expr_extend(ec, &value, y_width, true, &whole) : st;
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_MOD_MASK, &whole) : st;
}

/* $mod by 2^exp otherwise: an $and with the mask of exp ones (and a 0 above when B is signed). */
static odin3_status mod_to_mask(odin3_expr_ctx *ec, uint32_t exp) {
    bool both = odin3_expr_both_signed(ec);
    uint32_t mask_width = exp + (both || exp == 0 ? 1U : 0U);
    odin3_expr_sig mask;
    odin3_status st = ones_mask(ec, exp, mask_width, &mask);
    odin3_expr_cell spec = {
        .type = ODIN3_EXPR_T_AND, .n_params = N_BIN_PARAMS, .in = {NULL, &mask}};
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = both ? 1 : 0;
    spec.params[ODIN3_EXPR_P_BIN_BSIGNED] = both ? 1 : 0;
    spec.params[ODIN3_EXPR_P_BIN_AW] = odin3_expr_width(ec, PORT_A);
    spec.params[ODIN3_EXPR_P_BIN_BW] = mask_width;
    spec.params[ODIN3_EXPR_P_BIN_YW] = odin3_expr_y_width(ec);
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_MOD_MASK, &spec) : st;
}

odin3_status odin3_expr_rule_divmod(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_DIV && ec->kind != ODIN3_EXPR_T_MOD) {
        return ODIN3_OK;
    }
    odin3_expr_census census = odin3_expr_port_census(ec, PORT_B);
    if (!odin3_expr_fully_def(census)) {
        return ODIN3_OK;
    }
    if (odin3_expr_fully_zero(census)) {
        odin3_expr_sig unknown;
        odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_X, odin3_expr_y_width(ec), &unknown);
        return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_DIVMOD_ZERO, &unknown) : st;
    }
    uint32_t exp = 0;
    bool is_signed = odin3_expr_both_signed(ec);
    if (ec->keepdc || !onehot(ec, PORT_B, &exp) || (is_signed && exp == census.n - 1)) {
        return ODIN3_OK;
    }
    if (ec->kind == ODIN3_EXPR_T_DIV) {
        return div_to_shift(ec, exp);
    }
    return is_signed && exp != 0 ? mod_signed(ec, exp) : mod_to_mask(ec, exp);
}
