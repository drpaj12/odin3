/*
 * value.h — typed parameter values (IR-10): int, 4-state bits, string, SOP cover.
 */
#ifndef ODIN3_IR_VALUE_H
#define ODIN3_IR_VALUE_H

#include "odin3/odin3.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>

/* odin3_value_kind is public: see odin3.h. */

typedef enum odin3_bit { ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_Z } odin3_bit;

typedef struct odin3_value {
    odin3_value_kind kind;
    int64_t i;             /* INT */
    const uint8_t *bits;   /* BITS: one odin3_bit per byte, LSB first; COVER: rows */
    uint32_t len;          /* BITS: bit count; COVER: byte length (STRING uses str) */
    uint32_t str;          /* STRING: strtab ID */
    uint32_t cover_inputs; /* COVER: inputs per row */
} odin3_value;

/*
 * Cover encoding: rows*(cover_inputs+1) bytes; input chars '0','1','-', then output '0'/'1'.
 * A zero-input cover has cover_inputs 0 and one byte per row.
 */

/*
 * True when val is well formed: a known kind; for BITS and COVER a payload (bits non-NULL) whenever
 * len > 0; for COVER a len that is a multiple of cover_inputs + 1 (whole rows). False for NULL.
 * Every API that stores a value (node parameters, cell-type defaults, attributes) checks it first.
 */
bool odin3_value_valid(const odin3_value *val);

/*
 * Deep-copies src into dst, placing any byte payload in the arena. ODIN3_ERR_NO_MEMORY on
 * out of memory; dst is unchanged on failure.
 */
odin3_status odin3_value_copy(odin3_arena *arena, const odin3_value *src, odin3_value *dst);

/* Structural equality: same kind and same contents (payload bytes compared, not pointers). */
bool odin3_value_equal(const odin3_value *lhs, const odin3_value *rhs);

odin3_value odin3_value_int(int64_t num);

#endif
