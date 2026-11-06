/*
 * word4.h — 4-state (0, 1, x, z) words of any width, the value type shared by the netlist
 * simulator's word cells and the testbench interpreter (design note
 * docs/specs/2026-11-05-sim-4state-design.md).
 *
 * A word is two planes of odin3_word_limbs(width) limbs, `v` and `u` (unknown). Bit k is the code
 * (u_k v_k), equal to odin3_bit and to the simulator's value byte: 0 = 0, 1 = 1, 2 = x, 3 = z.
 * Bits at width and above are 0 in both planes. A word whose `u` plane is all zero is a known word:
 * its `v` plane is exactly a 2-state word of word.h, and every operation below then reduces to
 * the 2-state operation (the fast path). Operation results never contain z, except where stated
 * (a copy, a mux, a resolution): an unknown operand bit gives x.
 *
 * Nothing here allocates; the caller supplies every limb array. dst may alias an operand unless
 * stated.
 */
#ifndef ODIN3_SIM_WORD4_H
#define ODIN3_SIM_WORD4_H

#include "odin3/odin3.h"
#include "sim/word.h"

#include <stdbool.h>
#include <stdint.h>

/* A bit's code. */
enum { ODIN3_W4_0 = 0, ODIN3_W4_1 = 1, ODIN3_W4_X = 2, ODIN3_W4_Z = 3 };

typedef struct odin3_w4 {
    uint64_t *v;
    uint64_t *u;
} odin3_w4;

typedef struct odin3_w4c {
    const uint64_t *v;
    const uint64_t *u;
} odin3_w4c;

/* One binary operation dst = lhs OP rhs on words of one width (rhs ignored by unary ones). */
typedef struct odin3_w4_bin {
    odin3_w4 dst;
    odin3_w4c lhs;
    odin3_w4c rhs;
    uint32_t width;
} odin3_w4_bin;

/* The bit codes of bit `pos`. */
static inline uint32_t odin3_w4_bit(odin3_w4c word, uint32_t pos) {
    return (uint32_t)odin3_word_bit(word.v, pos) | ((uint32_t)odin3_word_bit(word.u, pos) << 1);
}

/* A bit position and the code (0..3) to give it. */
typedef struct odin3_w4_bit_at {
    uint32_t pos;
    uint32_t code;
} odin3_w4_bit_at;

/* Sets the bit to its code; the bit must be 0 in both planes before (a fresh word). */
void odin3_w4_set_bit(odin3_w4 word, odin3_w4_bit_at at);

/* True when no bit of the width-bit word is x or z. */
bool odin3_w4_known(const uint64_t *unk, uint32_t width);

/* All bits 0 / all bits x / all bits z. */
void odin3_w4_zero(odin3_w4 word, uint32_t width);
void odin3_w4_fill_x(odin3_w4 word, uint32_t width);
void odin3_w4_fill_z(odin3_w4 word, uint32_t width);

/* dst = src (same width). */
void odin3_w4_copy(odin3_w4 dst, odin3_w4c src, uint32_t width);

/* Width change in place: from a normalized `from`-bit value to `to` bits. Extension past from is
 * with the sign bit's code when is_signed (an x or z sign bit extends as x or z), else with 0. */
void odin3_w4_extend(odin3_w4 word, const odin3_word_resize *resize);

/* The planes <-> value bytes (codes 0..3) of a port, extending like odin3_word_load/store. */
void odin3_w4_load(odin3_w4 word, uint32_t width, const odin3_word_port *port);
void odin3_w4_store(odin3_w4c word, uint32_t width, const odin3_word_port *port);

/* Bitwise: per bit a 0 beats x for AND, a 1 beats x for OR; XOR and NOT of an unknown bit are x. */
void odin3_w4_and(const odin3_w4_bin *op);
void odin3_w4_or(const odin3_w4_bin *op);
void odin3_w4_xor(const odin3_w4_bin *op);
void odin3_w4_not(const odin3_w4_bin *op);

/* Arithmetic modulo 2^width: any unknown bit in an operand makes the whole result x. mul: dst
 * aliases neither operand. */
void odin3_w4_add(const odin3_w4_bin *op);
void odin3_w4_sub(const odin3_w4_bin *op);
void odin3_w4_mul(const odin3_w4_bin *op);
void odin3_w4_neg(const odin3_w4_bin *op);

/*
 * Shifts of lhs by the amount word `amount` (width amount_width): an unknown bit in the amount
 * gives all x; the state of the shifted bits moves with them; zeros shift in, except sshr
 * (arithmetic), which shifts in the sign bit's code (an x or z sign bit shifts in x).
 */
typedef struct odin3_w4_shift {
    odin3_w4 dst;
    odin3_w4c lhs;
    odin3_w4c amount;
    uint32_t width;
    uint32_t amount_width;
} odin3_w4_shift;
void odin3_w4_shl(const odin3_w4_shift *op);
void odin3_w4_shr(const odin3_w4_shift *op);
void odin3_w4_sshr(const odin3_w4_shift *op);

/* Relational: code 0, 1, or x (unknown bit in either operand); is_signed picks two's complement. */
uint32_t odin3_w4_lt(const odin3_w4_bin *op, bool is_signed);
uint32_t odin3_w4_le(const odin3_w4_bin *op, bool is_signed);

/* == : a bit position known in both and different gives 0 whatever the others are; else x when any
 * bit is unknown; else 1. */
uint32_t odin3_w4_eq(const odin3_w4_bin *op);
/* === : bitwise on all four states; always 0 or 1. */
uint32_t odin3_w4_case_eq(const odin3_w4_bin *op);

/* Truth of a word as a condition: 1 if any bit is 1, else 0 if every bit is 0, else x. */
uint32_t odin3_w4_truth(odin3_w4c word, uint32_t width);

/* Reductions of a word: &: 0 if any bit 0, else x if any unknown, else 1; |: 1 if any bit 1, else x
 * if any unknown, else 0; ^: x if any unknown, else parity. Code 0, 1 or x. */
uint32_t odin3_w4_reduce_and(odin3_w4c word, uint32_t width);
uint32_t odin3_w4_reduce_or(odin3_w4c word, uint32_t width);
uint32_t odin3_w4_reduce_xor(odin3_w4c word, uint32_t width);

/* The merge of ?: with an unknown condition: a bit equal and known in both stays, any other is x.
 */
void odin3_w4_merge(const odin3_w4_bin *op);

/* Net resolution of two drivers, per bit: z yields the other driver, equal values stay, anything
 * else is x. */
void odin3_w4_resolve(const odin3_w4_bin *op);

#endif
