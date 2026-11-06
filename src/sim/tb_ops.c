/* tb_ops.c — the testbench operators on 4-state values (design note
 * docs/specs/2026-11-05-sim-4state-design.md section 5, IEEE 1364-2005 5.1): arithmetic, bitwise,
 * comparisons, shifts, power and the ?: merge, over the word4 kernels. A value of 64 bits or fewer
 * is one limb, and a known one runs the 2-state arithmetic directly. */
#include "sim/tb_internal.h"

#include <string.h>

enum { DIV_LIMBS = TB_LIMBS + 1 };

static bool op_is(const char *op, const char *text) {
    return strcmp(op, text) == 0;
}

static odin3_w4 w4_of(tbv *val) {
    return (odin3_w4){val->v, val->u};
}

static odin3_w4c w4c_of(const tbv *val) {
    return (odin3_w4c){val->v, val->u};
}

/* The sign bit of a known value of width bits. */
static bool negative(const uint64_t *plane, uint32_t width) {
    return width > 0 && odin3_word_bit(plane, width - 1U);
}

/* --- division --- */

/* quo, rem = num / den, num % den, unsigned, width bits (den not 0). Shift-subtract over width + 1
 * bits, so the partial remainder cannot overflow. */
typedef struct divop {
    const uint64_t *num;
    const uint64_t *den;
    uint64_t *quo;
    uint64_t *rem;
    uint32_t width;
} divop;

static void udivmod(const divop *op) {
    uint64_t rem[DIV_LIMBS] = {0};
    uint64_t den[DIV_LIMBS] = {0};
    uint32_t wide = op->width + 1U;
    memcpy(den, op->den, (size_t)odin3_word_limbs(op->width) * sizeof *den);
    odin3_word_zero(op->quo, op->width);
    for (uint32_t bit = op->width; bit-- > 0;) {
        odin3_word_bin shl = {rem, rem, NULL, wide};
        odin3_word_shl(&shl, 1);
        if (odin3_word_bit(op->num, bit)) {
            rem[0] |= 1U;
        }
        odin3_word_bin cmp = {rem, rem, den, wide};
        if (odin3_word_ucmp(&cmp) >= 0) {
            odin3_word_sub(&cmp);
            odin3_word_set_bit(op->quo, bit);
        }
    }
    memcpy(op->rem, rem, (size_t)odin3_word_limbs(op->width) * sizeof *rem);
}

/* lhs = lhs / rhs or lhs % rhs (truncating toward zero; the remainder has the dividend's sign),
 * known operands, rhs not 0. */
static void divide(const tb_binop *op, bool is_div) {
    uint64_t num[TB_LIMBS];
    uint64_t den[TB_LIMBS];
    uint64_t quo[TB_LIMBS];
    uint64_t rem[TB_LIMBS];
    size_t bytes = (size_t)odin3_word_limbs(op->width) * sizeof *num;
    memcpy(num, op->lhs->v, bytes);
    memcpy(den, op->rhs->v, bytes);
    bool neg_num = op->sgn && negative(num, op->width);
    bool neg_den = op->sgn && negative(den, op->width);
    odin3_word_bin neg_n = {num, num, NULL, op->width};
    odin3_word_bin neg_d = {den, den, NULL, op->width};
    if (neg_num) {
        odin3_word_neg(&neg_n);
    }
    if (neg_den) {
        odin3_word_neg(&neg_d);
    }
    udivmod(&(divop){num, den, quo, rem, op->width});
    uint64_t *res = is_div ? quo : rem;
    if ((is_div && neg_num != neg_den) || (!is_div && neg_num)) {
        odin3_word_bin neg_r = {res, res, NULL, op->width};
        odin3_word_neg(&neg_r);
    }
    memcpy(op->lhs->v, res, bytes);
}

/* --- power --- */

/* base ** exp for a negative exponent (IEEE 1364-2005 table 5-6): 0 gives x, 1 gives 1, -1 gives
 * -1 or 1 by the exponent's parity, anything else 0. */
static void pow_negative(const tb_powop *op) {
    tbv *base = op->base;
    uint32_t width = op->width;
    tbv one;
    tbv_set_u64(&one, width, 1);
    tbv ones;
    tbv_fill(&ones, width, ODIN3_W4_1);
    if (odin3_word_is_zero(base->v, width)) {
        tbv_fill(base, width, ODIN3_W4_X);
    } else if (tbv_same(base, &one, width)) {
        return;
    } else if (op->base_sgn && tbv_same(base, &ones, width)) {
        if (!odin3_word_bit(op->exp->v, 0)) {
            tbv_copy(base, &one, width);
        }
    } else {
        tbv_zero(base, width);
    }
}

void tb_op_pow(const tb_powop *op) {
    uint32_t width = op->width;
    if (!tbv_known(op->base, width) || !tbv_known(op->exp, op->exp_width)) {
        tbv_fill(op->base, width, ODIN3_W4_X);
        return;
    }
    if (op->exp_sgn && negative(op->exp->v, op->exp_width)) {
        pow_negative(op);
        return;
    }
    uint64_t acc[TB_LIMBS];
    uint64_t sq[TB_LIMBS];
    uint64_t tmp[TB_LIMBS];
    size_t bytes = (size_t)odin3_word_limbs(width) * sizeof *acc;
    odin3_word_zero(acc, width);
    acc[0] = 1;
    odin3_word_mask(acc, width);
    memcpy(sq, op->base->v, bytes);
    for (uint32_t bit = 0; bit < op->exp_width; bit++) {
        if (odin3_word_bit(op->exp->v, bit)) {
            odin3_word_bin mul = {tmp, acc, sq, width};
            odin3_word_mul(&mul);
            memcpy(acc, tmp, bytes);
        }
        odin3_word_bin square = {tmp, sq, sq, width};
        odin3_word_mul(&square);
        memcpy(sq, tmp, bytes);
    }
    memcpy(op->base->v, acc, bytes);
}

/* --- arithmetic and bitwise --- */

/* + - * / %: any unknown operand bit gives all x, division or modulus by 0 too. */
static void arith(const char *op, const tb_binop *bo) {
    uint32_t width = bo->width;
    bool div = op_is(op, "/") || op_is(op, "%");
    if (!tbv_known(bo->lhs, width) || !tbv_known(bo->rhs, width) ||
        (div && odin3_word_is_zero(bo->rhs->v, width))) {
        tbv_fill(bo->lhs, width, ODIN3_W4_X);
        return;
    }
    odin3_word_bin word = {bo->lhs->v, bo->lhs->v, bo->rhs->v, width};
    if (op_is(op, "+")) {
        odin3_word_add(&word);
    } else if (op_is(op, "-")) {
        odin3_word_sub(&word);
    } else if (op_is(op, "*")) {
        uint64_t prod[TB_LIMBS];
        word.dst = prod;
        odin3_word_mul(&word);
        memcpy(bo->lhs->v, prod, (size_t)odin3_word_limbs(width) * sizeof *prod);
    } else {
        divide(bo, op_is(op, "/"));
    }
}

void tb_op_arith(const char *op, const tb_binop *bo) {
    odin3_w4_bin w4 = {w4_of(bo->lhs), w4c_of(bo->lhs), w4c_of(bo->rhs), bo->width};
    if (op_is(op, "&")) {
        odin3_w4_and(&w4);
    } else if (op_is(op, "|")) {
        odin3_w4_or(&w4);
    } else if (op_is(op, "^")) {
        odin3_w4_xor(&w4);
    } else if (op_is(op, "~^")) {
        odin3_w4_xor(&w4);
        w4.lhs = w4c_of(bo->lhs);
        odin3_w4_not(&w4);
    } else {
        arith(op, bo);
    }
}

void tb_op_unary(const char *op, tbv *val, uint32_t width) {
    odin3_w4_bin w4 = {w4_of(val), w4c_of(val), w4c_of(val), width};
    if (op_is(op, "-")) {
        odin3_w4_neg(&w4);
    } else if (op_is(op, "~")) {
        odin3_w4_not(&w4);
    }
}

uint32_t tb_code_not(uint32_t code) {
    if (code == ODIN3_W4_0) {
        return ODIN3_W4_1;
    }
    return code == ODIN3_W4_1 ? (uint32_t)ODIN3_W4_0 : (uint32_t)ODIN3_W4_X;
}

/* --- comparisons --- */

uint32_t tb_op_compare(const char *op, const tb_binop *bo) {
    odin3_w4_bin fwd = {w4_of(bo->lhs), w4c_of(bo->lhs), w4c_of(bo->rhs), bo->width};
    odin3_w4_bin rev = {w4_of(bo->lhs), w4c_of(bo->rhs), w4c_of(bo->lhs), bo->width};
    if (op_is(op, "==")) {
        return odin3_w4_eq(&fwd);
    }
    if (op_is(op, "!=")) {
        return tb_code_not(odin3_w4_eq(&fwd));
    }
    if (op_is(op, "===")) {
        return odin3_w4_case_eq(&fwd);
    }
    if (op_is(op, "!==")) {
        return tb_code_not(odin3_w4_case_eq(&fwd));
    }
    if (op_is(op, "<")) {
        return odin3_w4_lt(&fwd, bo->sgn);
    }
    if (op_is(op, "<=")) {
        return odin3_w4_le(&fwd, bo->sgn);
    }
    if (op_is(op, ">")) {
        return odin3_w4_lt(&rev, bo->sgn);
    }
    return odin3_w4_le(&rev, bo->sgn);
}

/* --- shifts --- */

/* lhs >>> amount of a signed value: the vacated bits take the sign bit's code (an x or z sign bit
 * fills with x or z, as iverilog). */
static void arith_right(const tb_shiftop *so) {
    uint32_t width = so->width;
    uint64_t amount = 0;
    if (!tbv_u64(so->amount, so->amount_width, &amount)) {
        tbv_fill(so->lhs, width, ODIN3_W4_X);
        return;
    }
    uint32_t sign = width > 0 ? tbv_code(so->lhs, width - 1U) : 0U;
    uint32_t fill = amount < width ? (uint32_t)amount : width;
    odin3_word_bin vp = {so->lhs->v, so->lhs->v, NULL, width};
    odin3_word_bin up = {so->lhs->u, so->lhs->u, NULL, width};
    odin3_word_shr(&vp, amount);
    odin3_word_shr(&up, amount);
    if (sign == ODIN3_W4_0 || fill == 0) {
        return;
    }
    tbv ones;
    tbv_fill(&ones, fill, sign);
    tb_field_put((tb_planes){so->lhs->v, so->lhs->u, width}, (tb_field){width - fill, fill},
                 &ones);
}

void tb_op_shift(const char *op, const tb_shiftop *so) {
    if (op_is(op, ">>>") && so->sgn) {
        arith_right(so);
        return;
    }
    odin3_w4_shift w4 = {w4_of(so->lhs), w4c_of(so->lhs), w4c_of(so->amount), so->width,
                         so->amount_width};
    if (op[0] == '<') {
        odin3_w4_shl(&w4);
    } else {
        odin3_w4_shr(&w4);
    }
}

/* --- ?: with an unknown condition --- */

void tb_op_merge(tbv *lhs, const tbv *rhs, uint32_t width) {
    uint32_t count = odin3_word_limbs(width);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t same = ~(lhs->v[i] ^ rhs->v[i]) & ~(lhs->u[i] ^ rhs->u[i]);
        lhs->v[i] &= same;
        lhs->u[i] = (lhs->u[i] & same) | ~same;
    }
    tbv_extend(lhs, &(tb_resize){width, width, false, false});
}
