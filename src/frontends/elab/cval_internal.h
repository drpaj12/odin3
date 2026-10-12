/*
 * cval_internal.h — plane helpers shared by cval.c and cval_arith.c (not for other users): the
 * top-word mask that keeps a value's bits at and above its width 0, and filling a bit range.
 */
#ifndef ODIN3_FRONTENDS_ELAB_CVAL_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_CVAL_INTERNAL_H

#include "frontends/elab/cval.h"
#include "ir/value.h"

#include <stdbool.h>
#include <stdint.h>

/* The low `count` bits of a word (all 64 when count is 64 or more). */
static inline uint64_t cval_low_mask(uint32_t count) {
    return count >= ODIN3_CVAL_WORD_BITS ? UINT64_MAX : ((uint64_t)1 << count) - 1;
}

/* The used bits of the top word of a width-bit value. */
static inline uint64_t cval_top_mask(uint32_t width) {
    uint32_t used = width % ODIN3_CVAL_WORD_BITS;
    return used == 0 ? UINT64_MAX : cval_low_mask(used);
}

/* Clears both planes' bits at and above the width. */
static inline void cval_mask_top(const odin3_cval *val) {
    uint32_t top = odin3_cval_words(val->width) - 1;
    val->aval[top] &= cval_top_mask(val->width);
    val->bval[top] &= cval_top_mask(val->width);
}

/* Bits [from, width) become `bit`; nothing when from is at or above the width. */
static inline void cval_fill_from(const odin3_cval *val, uint32_t from, odin3_bit bit) {
    if (from >= val->width) {
        return;
    }
    uint64_t fill_a = bit == ODIN3_BIT_1 || bit == ODIN3_BIT_X ? UINT64_MAX : 0;
    uint64_t fill_b = bit == ODIN3_BIT_X || bit == ODIN3_BIT_Z ? UINT64_MAX : 0;
    uint32_t first = from / ODIN3_CVAL_WORD_BITS;
    uint32_t words = odin3_cval_words(val->width);
    for (uint32_t word = first; word < words; word++) {
        uint64_t keep = word == first ? cval_low_mask(from % ODIN3_CVAL_WORD_BITS) : 0;
        val->aval[word] = (val->aval[word] & keep) | (fill_a & ~keep);
        val->bval[word] = (val->bval[word] & keep) | (fill_b & ~keep);
    }
    cval_mask_top(val);
}

#endif
