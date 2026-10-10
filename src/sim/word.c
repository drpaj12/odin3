/* word.c — fixed-width two's-complement words over little-endian 64-bit limbs (word.h). */
#include "sim/word.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { HALF_BITS = 32, LIMB_BYTES = 8 };
static const uint64_t k_low_half = 0xffffffffU;

bool odin3_word_reserve(uint32_t width, uint32_t *bytes) {
    uint64_t total = (uint64_t)*bytes + (uint64_t)odin3_word_limbs(width) * LIMB_BYTES;
    if (total > UINT32_MAX) {
        return false;
    }
    *bytes = (uint32_t)total;
    return true;
}

void odin3_word_zero(uint64_t *word, uint32_t width) {
    memset(word, 0, (size_t)odin3_word_limbs(width) * sizeof *word);
}

void odin3_word_mask(uint64_t *word, uint32_t width) {
    uint32_t top = odin3_word_limbs(width) - 1;
    uint32_t used = width - top * ODIN3_WORD_LIMB_BITS; /* 0..64 bits in the top limb */
    if (used < ODIN3_WORD_LIMB_BITS) {
        word[top] &= ((uint64_t)1 << used) - 1;
    }
}

void odin3_word_extend(uint64_t *word, const odin3_word_resize *resize) {
    uint32_t from = resize->from;
    uint32_t to = resize->to;
    if (from >= to) {
        odin3_word_mask(word, to);
        return;
    }
    bool fill = resize->is_signed && from > 0 && odin3_word_bit(word, from - 1);
    uint32_t limb = from / ODIN3_WORD_LIMB_BITS;
    uint32_t part = from % ODIN3_WORD_LIMB_BITS;
    uint32_t n_limbs = odin3_word_limbs(to);
    if (limb < n_limbs && part != 0) {
        uint64_t keep = ((uint64_t)1 << part) - 1;
        word[limb] = fill ? word[limb] | ~keep : word[limb] & keep;
        limb++;
    }
    for (; limb < n_limbs; limb++) {
        word[limb] = fill ? UINT64_MAX : 0;
    }
    odin3_word_mask(word, to);
}

void odin3_word_load(uint64_t *word, uint32_t width, const odin3_word_port *port) {
    odin3_word_zero(word, width);
    uint32_t have = port->span.width < width ? port->span.width : width;
    for (uint32_t k = 0; k < have; k++) {
        if (port->values[port->span.idx[k]] != 0) {
            odin3_word_set_bit(word, k);
        }
    }
    const odin3_word_resize resize = {port->span.width, width, port->is_signed};
    odin3_word_extend(word, &resize);
}

void odin3_word_store(const uint64_t *word, uint32_t width, const odin3_word_port *port) {
    uint8_t fill = (uint8_t)(port->is_signed && width > 0 && odin3_word_bit(word, width - 1));
    for (uint32_t k = 0; k < port->span.width; k++) {
        port->values[port->span.idx[k]] = k < width ? (uint8_t)odin3_word_bit(word, k) : fill;
    }
}

/* --- arithmetic ------------------------------------------------------------------------------- */

void odin3_word_add(const odin3_word_bin *op) {
    uint32_t n_limbs = odin3_word_limbs(op->width);
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n_limbs; i++) {
        uint64_t lhs = op->lhs[i];
        uint64_t sum = lhs + op->rhs[i];
        uint64_t out = sum + carry;
        carry = (uint64_t)(sum < lhs) + (uint64_t)(out < sum);
        op->dst[i] = out;
    }
    odin3_word_mask(op->dst, op->width);
}

void odin3_word_sub(const odin3_word_bin *op) {
    uint32_t n_limbs = odin3_word_limbs(op->width);
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < n_limbs; i++) {
        uint64_t lhs = op->lhs[i];
        uint64_t diff = lhs - op->rhs[i];
        uint64_t out = diff - borrow;
        borrow = (uint64_t)(diff > lhs) + (uint64_t)(out > diff);
        op->dst[i] = out;
    }
    odin3_word_mask(op->dst, op->width);
}

/* The 128-bit product of two limbs, from 32-bit halves. */
typedef struct wide_product {
    uint64_t lo;
    uint64_t hi;
} wide_product;

static wide_product mul_limbs(uint64_t lhs, uint64_t rhs) {
    uint64_t l_lo = lhs & k_low_half;
    uint64_t l_hi = lhs >> HALF_BITS;
    uint64_t r_lo = rhs & k_low_half;
    uint64_t r_hi = rhs >> HALF_BITS;
    uint64_t p00 = l_lo * r_lo;
    uint64_t p01 = l_lo * r_hi;
    uint64_t p10 = l_hi * r_lo;
    uint64_t mid = (p00 >> HALF_BITS) + (p01 & k_low_half) + (p10 & k_low_half);
    wide_product out = {(mid << HALF_BITS) | (p00 & k_low_half),
                        l_hi * r_hi + (p01 >> HALF_BITS) + (p10 >> HALF_BITS) + (mid >> HALF_BITS)};
    return out;
}

/*
 * Schoolbook multiplication keeping the low n limbs. Each step adds a limb, a 128-bit product and
 * a carry, which together stay below 2^128, so the carry out fits one limb.
 */
void odin3_word_mul(const odin3_word_bin *op) {
    uint32_t n_limbs = odin3_word_limbs(op->width);
    if (n_limbs == 1) {
        op->dst[0] = op->lhs[0] * op->rhs[0];
        odin3_word_mask(op->dst, op->width);
        return;
    }
    memset(op->dst, 0, (size_t)n_limbs * sizeof *op->dst);
    for (uint32_t i = 0; i < n_limbs; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; i + j < n_limbs; j++) {
            wide_product prod = mul_limbs(op->lhs[i], op->rhs[j]);
            uint64_t acc = op->dst[i + j];
            uint64_t sum = acc + prod.lo;
            uint64_t out = sum + carry;
            carry = prod.hi + (uint64_t)(sum < acc) + (uint64_t)(out < sum);
            op->dst[i + j] = out;
        }
    }
    odin3_word_mask(op->dst, op->width);
}

void odin3_word_and(const odin3_word_bin *op) {
    for (uint32_t i = 0; i < odin3_word_limbs(op->width); i++) {
        op->dst[i] = op->lhs[i] & op->rhs[i];
    }
}

void odin3_word_or(const odin3_word_bin *op) {
    for (uint32_t i = 0; i < odin3_word_limbs(op->width); i++) {
        op->dst[i] = op->lhs[i] | op->rhs[i];
    }
}

void odin3_word_xor(const odin3_word_bin *op) {
    for (uint32_t i = 0; i < odin3_word_limbs(op->width); i++) {
        op->dst[i] = op->lhs[i] ^ op->rhs[i];
    }
}

void odin3_word_not(const odin3_word_bin *op) {
    for (uint32_t i = 0; i < odin3_word_limbs(op->width); i++) {
        op->dst[i] = ~op->lhs[i];
    }
    odin3_word_mask(op->dst, op->width);
}

void odin3_word_neg(const odin3_word_bin *op) {
    uint64_t carry = 1;
    for (uint32_t i = 0; i < odin3_word_limbs(op->width); i++) {
        uint64_t out = ~op->lhs[i] + carry;
        carry = (uint64_t)(carry != 0 && out == 0);
        op->dst[i] = out;
    }
    odin3_word_mask(op->dst, op->width);
}

/* --- shifts and comparisons ------------------------------------------------------------------- */

void odin3_word_shl(const odin3_word_bin *op, uint64_t amount) {
    uint32_t n_limbs = odin3_word_limbs(op->width);
    if (amount >= op->width) {
        memset(op->dst, 0, (size_t)n_limbs * sizeof *op->dst);
        return;
    }
    uint32_t skip = (uint32_t)(amount / ODIN3_WORD_LIMB_BITS);
    uint32_t part = (uint32_t)(amount % ODIN3_WORD_LIMB_BITS);
    for (uint32_t i = n_limbs; i-- > 0;) { /* high to low: dst may be lhs */
        uint64_t out = 0;
        if (i >= skip) {
            out = op->lhs[i - skip] << part;
            if (part != 0 && i > skip) {
                out |= op->lhs[i - skip - 1] >> (ODIN3_WORD_LIMB_BITS - part);
            }
        }
        op->dst[i] = out;
    }
    odin3_word_mask(op->dst, op->width);
}

void odin3_word_shr(const odin3_word_bin *op, uint64_t amount) {
    uint32_t n_limbs = odin3_word_limbs(op->width);
    if (amount >= op->width) {
        memset(op->dst, 0, (size_t)n_limbs * sizeof *op->dst);
        return;
    }
    uint32_t skip = (uint32_t)(amount / ODIN3_WORD_LIMB_BITS);
    uint32_t part = (uint32_t)(amount % ODIN3_WORD_LIMB_BITS);
    for (uint32_t i = 0; i < n_limbs; i++) { /* low to high: dst may be lhs */
        uint64_t out = 0;
        if (i + skip < n_limbs) {
            out = op->lhs[i + skip] >> part;
            if (part != 0 && i + skip + 1 < n_limbs) {
                out |= op->lhs[i + skip + 1] << (ODIN3_WORD_LIMB_BITS - part);
            }
        }
        op->dst[i] = out;
    }
}

int odin3_word_ucmp(const odin3_word_bin *op) {
    for (uint32_t i = odin3_word_limbs(op->width); i-- > 0;) {
        if (op->lhs[i] != op->rhs[i]) {
            return op->lhs[i] > op->rhs[i] ? 1 : -1;
        }
    }
    return 0;
}

int odin3_word_scmp(const odin3_word_bin *op) {
    if (op->width == 0) {
        return 0;
    }
    bool lhs_neg = odin3_word_bit(op->lhs, op->width - 1);
    bool rhs_neg = odin3_word_bit(op->rhs, op->width - 1);
    if (lhs_neg != rhs_neg) {
        return lhs_neg ? -1 : 1;
    }
    return odin3_word_ucmp(op); /* same sign: two's complement orders like unsigned */
}

bool odin3_word_is_zero(const uint64_t *word, uint32_t width) {
    for (uint32_t i = 0; i < odin3_word_limbs(width); i++) {
        if (word[i] != 0) {
            return false;
        }
    }
    return true;
}

uint64_t odin3_word_amount(const uint64_t *word, uint32_t width) {
    for (uint32_t i = 1; i < odin3_word_limbs(width); i++) {
        if (word[i] != 0) {
            return UINT64_MAX;
        }
    }
    return word[0];
}
