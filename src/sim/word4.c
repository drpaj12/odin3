/* word4.c — 4-state words over (v, u) limb planes (word4.h). */
#include "sim/word4.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t limbs_of(uint32_t width) {
    return odin3_word_limbs(width);
}

void odin3_w4_set_bit(odin3_w4 word, uint32_t pos, uint32_t code) {
    if ((code & 1U) != 0) {
        odin3_word_set_bit(word.v, pos);
    }
    if ((code & 2U) != 0) {
        odin3_word_set_bit(word.u, pos);
    }
}

bool odin3_w4_known(const uint64_t *u, uint32_t width) {
    return odin3_word_is_zero(u, width);
}

void odin3_w4_zero(odin3_w4 word, uint32_t width) {
    odin3_word_zero(word.v, width);
    odin3_word_zero(word.u, width);
}

void odin3_w4_fill_x(odin3_w4 word, uint32_t width) {
    odin3_word_zero(word.v, width);
    for (uint32_t i = 0; i < limbs_of(width); i++) {
        word.u[i] = UINT64_MAX;
    }
    odin3_word_mask(word.u, width);
}

void odin3_w4_fill_z(odin3_w4 word, uint32_t width) {
    odin3_w4_fill_x(word, width);
    memcpy(word.v, word.u, (size_t)limbs_of(width) * sizeof *word.v);
}

void odin3_w4_copy(odin3_w4 dst, odin3_w4c src, uint32_t width) {
    memmove(dst.v, src.v, (size_t)limbs_of(width) * sizeof *dst.v);
    memmove(dst.u, src.u, (size_t)limbs_of(width) * sizeof *dst.u);
}

void odin3_w4_extend(odin3_w4 word, const odin3_word_resize *resize) {
    /* each plane extends with its own copy of the sign bit, so an x or z sign bit extends as x
     * or z */
    odin3_word_extend(word.v, resize);
    odin3_word_extend(word.u, resize);
}

void odin3_w4_load(odin3_w4 word, uint32_t width, const odin3_word_port *port) {
    odin3_w4_zero(word, width);
    uint32_t have = port->span.width < width ? port->span.width : width;
    for (uint32_t k = 0; k < have; k++) {
        odin3_w4_set_bit(word, k, port->values[port->span.idx[k]] & 3U);
    }
    odin3_w4_extend(word, &(odin3_word_resize){port->span.width, width, port->is_signed});
}

void odin3_w4_store(odin3_w4c word, uint32_t width, const odin3_word_port *port) {
    uint8_t fill = port->is_signed && width > 0 ? (uint8_t)odin3_w4_bit(word, width - 1) : 0;
    for (uint32_t k = 0; k < port->span.width; k++) {
        port->values[port->span.idx[k]] = k < width ? (uint8_t)odin3_w4_bit(word, k) : fill;
    }
}

/* --- bitwise ---------------------------------------------------------------------------------- */

void odin3_w4_and(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t one_a = op->lhs.v[i] & ~op->lhs.u[i];
        uint64_t one_b = op->rhs.v[i] & ~op->rhs.u[i];
        uint64_t zero_a = ~op->lhs.v[i] & ~op->lhs.u[i];
        uint64_t zero_b = ~op->rhs.v[i] & ~op->rhs.u[i];
        uint64_t one = one_a & one_b;
        op->dst.v[i] = one;
        op->dst.u[i] = ~(one | zero_a | zero_b);
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}

void odin3_w4_or(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t one_a = op->lhs.v[i] & ~op->lhs.u[i];
        uint64_t one_b = op->rhs.v[i] & ~op->rhs.u[i];
        uint64_t zero_a = ~op->lhs.v[i] & ~op->lhs.u[i];
        uint64_t zero_b = ~op->rhs.v[i] & ~op->rhs.u[i];
        uint64_t one = one_a | one_b;
        op->dst.v[i] = one;
        op->dst.u[i] = ~(one | (zero_a & zero_b));
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}

void odin3_w4_xor(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t unk = op->lhs.u[i] | op->rhs.u[i];
        op->dst.v[i] = (op->lhs.v[i] ^ op->rhs.v[i]) & ~unk;
        op->dst.u[i] = unk;
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}

void odin3_w4_not(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t unk = op->lhs.u[i];
        op->dst.v[i] = ~op->lhs.v[i] & ~unk;
        op->dst.u[i] = unk;
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}

/* --- arithmetic ------------------------------------------------------------------------------- */

static bool operands_known(const odin3_w4_bin *op) {
    return odin3_w4_known(op->lhs.u, op->width) && odin3_w4_known(op->rhs.u, op->width);
}

static odin3_word_bin plain(const odin3_w4_bin *op) {
    return (odin3_word_bin){op->dst.v, op->lhs.v, op->rhs.v, op->width};
}

/* Runs the 2-state operation on the v planes when every operand bit is known, else all x. */
static void arith(const odin3_w4_bin *op, odin3_word_fn fn) {
    if (!operands_known(op)) {
        odin3_w4_fill_x(op->dst, op->width);
        return;
    }
    odin3_word_bin word_op = plain(op);
    fn(&word_op);
    odin3_word_zero(op->dst.u, op->width);
}

void odin3_w4_add(const odin3_w4_bin *op) {
    arith(op, odin3_word_add);
}

void odin3_w4_sub(const odin3_w4_bin *op) {
    arith(op, odin3_word_sub);
}

void odin3_w4_mul(const odin3_w4_bin *op) {
    arith(op, odin3_word_mul);
}

void odin3_w4_neg(const odin3_w4_bin *op) {
    if (!odin3_w4_known(op->lhs.u, op->width)) {
        odin3_w4_fill_x(op->dst, op->width);
        return;
    }
    odin3_word_bin word_op = plain(op);
    odin3_word_neg(&word_op);
    odin3_word_zero(op->dst.u, op->width);
}

/* --- shifts ----------------------------------------------------------------------------------- */

typedef void (*shift_fn)(const odin3_word_bin *op, uint64_t amount);

/* Both planes move; the amount must be known. */
static void shift_planes(const odin3_w4_shift *op, shift_fn fn) {
    if (!odin3_w4_known(op->amount.u, op->amount_width)) {
        odin3_w4_fill_x(op->dst, op->width);
        return;
    }
    uint64_t amount = odin3_word_amount(op->amount.v, op->amount_width);
    odin3_word_bin vp = {op->dst.v, op->lhs.v, NULL, op->width};
    odin3_word_bin up = {op->dst.u, op->lhs.u, NULL, op->width};
    fn(&vp, amount);
    fn(&up, amount);
}

void odin3_w4_shl(const odin3_w4_shift *op) {
    shift_planes(op, odin3_word_shl);
}

void odin3_w4_shr(const odin3_w4_shift *op) {
    shift_planes(op, odin3_word_shr);
}

void odin3_w4_sshr(const odin3_w4_shift *op) {
    if (!odin3_w4_known(op->amount.u, op->amount_width)) {
        odin3_w4_fill_x(op->dst, op->width);
        return;
    }
    uint32_t sign = op->width > 0 ? odin3_w4_bit(op->lhs, op->width - 1) : 0;
    uint64_t amount = odin3_word_amount(op->amount.v, op->amount_width);
    if (amount > op->width) {
        amount = op->width;
    }
    odin3_w4_shift logical = *op;
    odin3_w4_shr(&logical);
    uint32_t fill = (sign & 2U) != 0 ? 2U : sign; /* an x or z sign bit shifts in x */
    for (uint64_t k = 0; k < amount && fill != 0; k++) {
        uint32_t pos = op->width - 1U - (uint32_t)k;
        if ((fill & 1U) != 0) {
            odin3_word_set_bit(op->dst.v, pos);
        }
        if ((fill & 2U) != 0) {
            odin3_word_set_bit(op->dst.u, pos);
        }
    }
}

/* --- comparisons ------------------------------------------------------------------------------ */

static uint32_t compare(const odin3_w4_bin *op, bool is_signed) {
    odin3_word_bin word_op = plain(op);
    return (uint32_t)(is_signed ? odin3_word_scmp(&word_op) : odin3_word_ucmp(&word_op)) + 1U;
}

uint32_t odin3_w4_lt(const odin3_w4_bin *op, bool is_signed) {
    if (!operands_known(op)) {
        return ODIN3_W4_X;
    }
    return compare(op, is_signed) == 0 ? 1U : 0U;
}

uint32_t odin3_w4_le(const odin3_w4_bin *op, bool is_signed) {
    if (!operands_known(op)) {
        return ODIN3_W4_X;
    }
    return compare(op, is_signed) <= 1U ? 1U : 0U;
}

uint32_t odin3_w4_eq(const odin3_w4_bin *op) {
    bool unknown = false;
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t both_known = ~op->lhs.u[i] & ~op->rhs.u[i];
        if (((op->lhs.v[i] ^ op->rhs.v[i]) & both_known) != 0) {
            return ODIN3_W4_0;
        }
        unknown = unknown || (op->lhs.u[i] | op->rhs.u[i]) != 0;
    }
    return unknown ? ODIN3_W4_X : ODIN3_W4_1;
}

uint32_t odin3_w4_case_eq(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        if (op->lhs.v[i] != op->rhs.v[i] || op->lhs.u[i] != op->rhs.u[i]) {
            return ODIN3_W4_0;
        }
    }
    return ODIN3_W4_1;
}

/* --- conditions and reductions ---------------------------------------------------------------- */

uint32_t odin3_w4_truth(odin3_w4c word, uint32_t width) {
    bool unknown = false;
    for (uint32_t i = 0; i < limbs_of(width); i++) {
        if ((word.v[i] & ~word.u[i]) != 0) {
            return ODIN3_W4_1;
        }
        unknown = unknown || word.u[i] != 0;
    }
    return unknown ? ODIN3_W4_X : ODIN3_W4_0;
}

uint32_t odin3_w4_reduce_or(odin3_w4c word, uint32_t width) {
    return odin3_w4_truth(word, width);
}

uint32_t odin3_w4_reduce_and(odin3_w4c word, uint32_t width) {
    bool unknown = false;
    for (uint32_t i = 0; i < limbs_of(width); i++) {
        uint64_t mask = UINT64_MAX;
        if (i + 1U == limbs_of(width) && width % ODIN3_WORD_LIMB_BITS != 0) {
            mask = ((uint64_t)1 << (width % ODIN3_WORD_LIMB_BITS)) - 1U;
        }
        if ((~word.v[i] & ~word.u[i] & mask) != 0) {
            return ODIN3_W4_0;
        }
        unknown = unknown || word.u[i] != 0;
    }
    return unknown ? ODIN3_W4_X : ODIN3_W4_1;
}

uint32_t odin3_w4_reduce_xor(odin3_w4c word, uint32_t width) {
    if (!odin3_w4_known(word.u, width)) {
        return ODIN3_W4_X;
    }
    uint32_t parity = 0;
    for (uint32_t i = 0; i < limbs_of(width); i++) {
        parity ^= (uint32_t)__builtin_parityll(word.v[i]);
    }
    return parity;
}

/* --- merge and resolution --------------------------------------------------------------------- */

void odin3_w4_merge(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t same = ~(op->lhs.v[i] ^ op->rhs.v[i]) & ~op->lhs.u[i] & ~op->rhs.u[i];
        op->dst.v[i] = op->lhs.v[i] & same;
        op->dst.u[i] = ~same;
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}

void odin3_w4_resolve(const odin3_w4_bin *op) {
    for (uint32_t i = 0; i < limbs_of(op->width); i++) {
        uint64_t z_a = op->lhs.v[i] & op->lhs.u[i];
        uint64_t z_b = op->rhs.v[i] & op->rhs.u[i];
        uint64_t x_a = ~op->lhs.v[i] & op->lhs.u[i];
        uint64_t x_b = ~op->rhs.v[i] & op->rhs.u[i];
        uint64_t both_val = ~z_a & ~z_b; /* neither is z */
        uint64_t clash = (op->lhs.v[i] ^ op->rhs.v[i]) | x_a | x_b;
        uint64_t unk_x = both_val & clash;
        uint64_t pick_b = z_a & ~z_b; /* a is z: b's code */
        uint64_t pick_a = z_b & ~z_a;
        uint64_t both_z = z_a & z_b;
        uint64_t v = (op->lhs.v[i] & ~clash & both_val) | (op->rhs.v[i] & pick_b) |
                     (op->lhs.v[i] & pick_a) | both_z;
        uint64_t u = unk_x | (op->rhs.u[i] & pick_b) | (op->lhs.u[i] & pick_a) | both_z;
        op->dst.v[i] = v;
        op->dst.u[i] = u;
    }
    odin3_word_mask(op->dst.v, op->width);
    odin3_word_mask(op->dst.u, op->width);
}
