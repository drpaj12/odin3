/*
 * word.h — fixed-width two's-complement words for simulate hooks: little-endian 64-bit limbs.
 *
 * A word of width W lives in odin3_word_limbs(W) limbs (at least one), bit k in limb k / 64, and
 * is kept normalized: bits at W and above are 0. A word of 64 bits or fewer is one limb, so every
 * operation is then a single machine operation (the fast path); wider words loop over the limbs.
 * Nothing here allocates: the caller supplies every limb array (a hook takes them from its
 * scratch, sized with odin3_word_reserve).
 */
#ifndef ODIN3_SIM_WORD_H
#define ODIN3_SIM_WORD_H

#include "odin3/odin3.h"
#include "sim/cell.h"

#include <stdbool.h>
#include <stdint.h>

enum { ODIN3_WORD_LIMB_BITS = 64 };

/* Limbs holding a width-bit word: ceil(width / 64), at least 1. */
static inline uint32_t odin3_word_limbs(uint32_t width) {
    uint32_t limbs = width / ODIN3_WORD_LIMB_BITS + (width % ODIN3_WORD_LIMB_BITS != 0 ? 1U : 0U);
    return limbs > 0 ? limbs : 1;
}

/*
 * *bytes += the bytes of one width-bit word; false (*bytes unchanged) when the sum passes
 * UINT32_MAX. Sizing helper for sim_scratch_bytes hooks.
 */
bool odin3_word_reserve(uint32_t width, uint32_t *bytes);

static inline bool odin3_word_bit(const uint64_t *word, uint32_t pos) {
    return ((word[pos / ODIN3_WORD_LIMB_BITS] >> (pos % ODIN3_WORD_LIMB_BITS)) & 1U) != 0;
}

static inline void odin3_word_set_bit(uint64_t *word, uint32_t pos) {
    word[pos / ODIN3_WORD_LIMB_BITS] |= (uint64_t)1 << (pos % ODIN3_WORD_LIMB_BITS);
}

/* Clears every limb of a width-bit word. */
void odin3_word_zero(uint64_t *word, uint32_t width);

/* Clears the bits at width and above in the top limb (normalizes after a raw limb write). */
void odin3_word_mask(uint64_t *word, uint32_t width);

/* A word's width change in place, from a normalized `from`-bit value to `to` bits. */
typedef struct odin3_word_resize {
    uint32_t from;
    uint32_t to;
    bool is_signed; /* extend with bit from-1 (0 when from is 0); else with 0 */
} odin3_word_resize;

/* Extends (or truncates, when to < from) word, which has room for odin3_word_limbs(to) limbs. */
void odin3_word_extend(uint64_t *word, const odin3_word_resize *resize);

/* A port of a cell as a word source or sink: its span in the value array. */
typedef struct odin3_word_port {
    uint8_t *values;
    odin3_sim_span span;
    bool is_signed; /* extension of a load past the span, or of a store past the word */
} odin3_word_port;

/* word (width bits) = the port's bits, extended past the span by its signedness, truncated. */
void odin3_word_load(uint64_t *word, uint32_t width, const odin3_word_port *port);

/* The port's bits = word (width bits), extended past width by the port's signedness. */
void odin3_word_store(const uint64_t *word, uint32_t width, const odin3_word_port *port);

/*
 * A binary operation dst = lhs OP rhs on three words of one width (rhs is ignored by the unary
 * operations). dst may be lhs or rhs, except for multiplication.
 */
typedef struct odin3_word_bin {
    uint64_t *dst;
    const uint64_t *lhs;
    const uint64_t *rhs;
    uint32_t width;
} odin3_word_bin;

/* Any of the operations below that takes only the operands. */
typedef void (*odin3_word_fn)(const odin3_word_bin *op);

void odin3_word_add(const odin3_word_bin *op); /* modulo 2^width */
void odin3_word_sub(const odin3_word_bin *op); /* modulo 2^width */
void odin3_word_mul(const odin3_word_bin *op); /* modulo 2^width; dst aliases neither operand */
void odin3_word_and(const odin3_word_bin *op);
void odin3_word_or(const odin3_word_bin *op);
void odin3_word_xor(const odin3_word_bin *op);
void odin3_word_not(const odin3_word_bin *op); /* dst = ~lhs */
void odin3_word_neg(const odin3_word_bin *op); /* dst = -lhs modulo 2^width */

/* dst = lhs shifted by amount bits (logical: zeros shift in); amount >= width gives 0. */
void odin3_word_shl(const odin3_word_bin *op, uint64_t amount);
void odin3_word_shr(const odin3_word_bin *op, uint64_t amount);

/* -1, 0 or 1: lhs against rhs as unsigned, or as two's-complement (bit width-1 the sign). */
int odin3_word_ucmp(const odin3_word_bin *op);
int odin3_word_scmp(const odin3_word_bin *op);

/* True when every bit of the width-bit word is 0. */
bool odin3_word_is_zero(const uint64_t *word, uint32_t width);

/* The word's value as a shift amount: its low 64 bits, or UINT64_MAX when a higher bit is set. */
uint64_t odin3_word_amount(const uint64_t *word, uint32_t width);

#endif
