/*
 * alu.c — the `techmap.v` family of `lower`, part 2 (3B spec §5.4, LW-6; plan Task 4): the
 * carry-lookahead unit `lw_lcu` (techmap.v `_90_lcu_brent_kung`, lines 210 to 256), the adder
 * `lw_alu` (`_90_alu`, lines 258 to 299, with `$fa`'s propagate and generate bits) and the cells
 * that use it: $add, $sub, $neg (alumacc.cc). Constants (CI, a constant operand) are left in
 * for `opt -fast` (spec §2.2). Every builder appends bit nodes on fresh nets and puts the net of
 * each output bit into `out`.
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Port positions: unary A Y; binary A B Y. */
enum { PORT_A = 0, PORT_B = 1, PORT_Y_UNARY = 1, PORT_Y_BINARY = 2 };

/* --- the carry-lookahead unit --------------------------------------------------------------- */

/* One tree update at bit `bit` from bit `src`: g[bit] |= p[bit] & g[src]; p[bit] &= p[src]. */
static odin3_status lcu_update(lw_ctx *lw, lw_bits *prop, lw_bits *gen, uint32_t bit,
                               uint32_t src) {
    odin3_net_id term = {0};
    odin3_net_id merged = {0};
    odin3_net_id narrowed = {0};
    odin3_status st = lw_and(lw, lw_bits_at(prop, bit), lw_bits_at(gen, src), &term);
    st = st == ODIN3_OK ? lw_or(lw, lw_bits_at(gen, bit), term, &merged) : st;
    st = st == ODIN3_OK ? lw_and(lw, lw_bits_at(prop, bit), lw_bits_at(prop, src), &narrowed) : st;
    if (st == ODIN3_OK) {
        lw_bits_set(gen, bit, merged);
        lw_bits_set(prop, bit, narrowed);
    }
    return st;
}

/* Level `level` of the tree: every bit `first + m * 2^level` below the width reads 2^(level-1)
 * places down. */
static odin3_status lcu_level(lw_ctx *lw, lw_bits *prop, lw_bits *gen, uint32_t level,
                              bool inverse) {
    uint64_t step = (uint64_t)1 << level;
    uint64_t half = step >> 1;
    uint64_t first = inverse ? step + half - 1 : step - 1;
    odin3_status st = ODIN3_OK;
    for (uint64_t bit = first; st == ODIN3_OK && bit < lw_bits_len(gen); bit += step) {
        st = lcu_update(lw, prop, gen, (uint32_t)bit, (uint32_t)(bit - half));
    }
    return st;
}

odin3_status lw_lcu(lw_ctx *lw, lw_span prop, lw_span gen, odin3_net_id carry_in, lw_bits *out) {
    lw_bits cur_p;
    lw_bits cur_g;
    lw_bits_init(&cur_p);
    lw_bits_init(&cur_g);
    odin3_status st = odin3_vec_reserve(&cur_p.v, prop.n);
    st = st == ODIN3_OK ? odin3_vec_reserve(&cur_g.v, gen.n) : st;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < gen.n; bit++) {
        st = lw_bits_push(&cur_p, prop.nets[bit]);
        st = st == ODIN3_OK ? lw_bits_push(&cur_g, gen.nets[bit]) : st;
    }
    /* g[0] |= p[0] & CI */
    odin3_net_id term = {0};
    odin3_net_id first = {0};
    st = st == ODIN3_OK ? lw_and(lw, prop.nets[0], carry_in, &term) : st;
    st = st == ODIN3_OK ? lw_or(lw, gen.nets[0], term, &first) : st;
    if (st == ODIN3_OK) {
        lw_bits_set(&cur_g, 0, first);
    }
    uint32_t levels = lw_clog2(gen.n);
    for (uint32_t level = 1; st == ODIN3_OK && level <= levels; level++) {
        st = lcu_level(lw, &cur_p, &cur_g, level, false);
    }
    for (uint32_t level = levels; st == ODIN3_OK && level > 0; level--) {
        st = lcu_level(lw, &cur_p, &cur_g, level, true);
    }
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < gen.n; bit++) {
        st = lw_bits_push(out, lw_bits_at(&cur_g, bit));
    }
    lw_bits_free(&cur_p);
    lw_bits_free(&cur_g);
    return st;
}

/* Gates of lw_lcu over `width` bits: g[0], then three per tree update. */
uint64_t lw_lcu_gates(uint64_t width) {
    uint64_t updates = 0;
    uint32_t levels = lw_clog2(width);
    for (uint32_t level = 1; level <= levels; level++) {
        uint64_t step = (uint64_t)1 << level;
        uint64_t half = step >> 1;
        updates += width / step; /* the main tree: bits step - 1, 2 * step - 1, ... */
        if (width >= step + half) {
            updates += (width - step - half) / step + 1; /* the inverse tree */
        }
    }
    return 2 + 3 * updates;
}

/* --- the adder ------------------------------------------------------------------------------ */

void lw_alu_out_init(lw_alu_out *res) {
    lw_bits_init(&res->sum);
    lw_bits_init(&res->prop);
    lw_bits_init(&res->carry);
}

void lw_alu_out_free(lw_alu_out *res) {
    lw_bits_free(&res->sum);
    lw_bits_free(&res->prop);
    lw_bits_free(&res->carry);
}

/* BB = BI ? ~ext(B) : ext(B), in place. */
static odin3_status invert_all(lw_ctx *lw, lw_bits *bits) {
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < lw_bits_len(bits); bit++) {
        odin3_net_id inverted = {0};
        st = lw_not(lw, lw_bits_at(bits, bit), &inverted);
        if (st == ODIN3_OK) {
            lw_bits_set(bits, bit, inverted);
        }
    }
    return st;
}

/* P[i] = AA[i] ^ BB[i] and G[i] = AA[i] & BB[i] (the $fa of _90_alu with C = 0). */
static odin3_status propagate_generate(lw_ctx *lw, const lw_bits *lhs, const lw_bits *rhs,
                                       lw_alu_out *res, lw_bits *gen) {
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < lw_bits_len(lhs); bit++) {
        odin3_net_id prop = {0};
        odin3_net_id carry = {0};
        st = lw_xor(lw, lw_bits_at(lhs, bit), lw_bits_at(rhs, bit), &prop);
        st = st == ODIN3_OK ? lw_and(lw, lw_bits_at(lhs, bit), lw_bits_at(rhs, bit), &carry) : st;
        st = st == ODIN3_OK ? lw_bits_push(&res->prop, prop) : st;
        st = st == ODIN3_OK ? lw_bits_push(gen, carry) : st;
    }
    return st;
}

/* Y[i] = P[i] ^ (i == 0 ? CI : CO[i - 1]). */
static odin3_status sum_bits(lw_ctx *lw, odin3_net_id carry_in, lw_alu_out *res) {
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < lw_bits_len(&res->prop); bit++) {
        odin3_net_id below = bit == 0 ? carry_in : lw_bits_at(&res->carry, bit - 1);
        odin3_net_id total = {0};
        st = lw_xor(lw, lw_bits_at(&res->prop, bit), below, &total);
        st = st == ODIN3_OK ? lw_bits_push(&res->sum, total) : st;
    }
    return st;
}

odin3_status lw_alu(lw_ctx *lw, const lw_alu_args *args, lw_alu_out *res) {
    lw_bits lhs;
    lw_bits rhs;
    lw_bits gen;
    lw_bits_init(&lhs);
    lw_bits_init(&rhs);
    lw_bits_init(&gen);
    lw_bits_clear(&res->sum);
    lw_bits_clear(&res->prop);
    lw_bits_clear(&res->carry);
    odin3_status st = lw_extend(lw, args->a, args->width, args->is_signed, &lhs);
    st = st == ODIN3_OK ? lw_extend(lw, args->b, args->width, args->is_signed, &rhs) : st;
    st = st == ODIN3_OK && args->invert_b ? invert_all(lw, &rhs) : st;
    st = st == ODIN3_OK ? propagate_generate(lw, &lhs, &rhs, res, &gen) : st;
    st = st == ODIN3_OK
             ? lw_lcu(lw, lw_bits_span(&res->prop), lw_bits_span(&gen), args->carry_in, &res->carry)
             : st;
    st = st == ODIN3_OK && args->need_sum ? sum_bits(lw, args->carry_in, res) : st;
    lw_bits_free(&lhs);
    lw_bits_free(&rhs);
    lw_bits_free(&gen);
    return st;
}

/* Gates of lw_alu: the inverters of B, P and G, the lookahead unit, the sum bits. */
uint64_t lw_alu_gates(uint64_t width, bool invert_b, bool need_sum) {
    uint64_t gates = lw_sat_add(lw_sat_mul(width, 2), lw_lcu_gates(width));
    gates = lw_sat_add(gates, invert_b ? width : 0);
    return lw_sat_add(gates, need_sum ? width : 0);
}

/* --- $add, $sub, $neg ----------------------------------------------------------------------- */

/* Y = A + B (CI = 0, BI = 0) or A - B (CI = 1, BI = 1) at Y_WIDTH (alumacc.cc, _90_alu). */
static odin3_status addsub(lw_ctx *lw, const lw_cell *cell, bool subtract, lw_bits *out) {
    lw_alu_args args = {cell->port[PORT_A],         cell->port[PORT_B], {0}, subtract, false, true,
                        cell->port[PORT_Y_BINARY].n};
    args.is_signed = lw_both_signed(lw, cell);
    odin3_status st = lw_const(lw, subtract ? ODIN3_BIT_1 : ODIN3_BIT_0, &args.carry_in);
    lw_alu_out res;
    lw_alu_out_init(&res);
    st = st == ODIN3_OK ? lw_alu(lw, &args, &res) : st;
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < lw_bits_len(&res.sum); bit++) {
        st = lw_bits_push(out, lw_bits_at(&res.sum, bit));
    }
    lw_alu_out_free(&res);
    return st;
}

odin3_status lw_build_add(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return addsub(lw, cell, false, out);
}

odin3_status lw_build_sub(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return addsub(lw, cell, true, out);
}

/* Y = 0 - A with the operands signed by A_SIGNED (alumacc turns a lone negation into 0 - A). */
odin3_status lw_build_neg(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    odin3_net_id zero = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    lw_alu_args args = {{&zero, 1}, cell->port[PORT_A],        {0}, true, false,
                        true,       cell->port[PORT_Y_UNARY].n};
    args.is_signed = lw_both_signed(lw, cell);
    st = st == ODIN3_OK ? lw_const(lw, ODIN3_BIT_1, &args.carry_in) : st;
    lw_alu_out res;
    lw_alu_out_init(&res);
    st = st == ODIN3_OK ? lw_alu(lw, &args, &res) : st;
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < lw_bits_len(&res.sum); bit++) {
        st = lw_bits_push(out, lw_bits_at(&res.sum, bit));
    }
    lw_alu_out_free(&res);
    return st;
}

uint64_t lw_est_add(const lw_cell *cell) {
    return lw_alu_gates(cell->port[PORT_Y_BINARY].n, false, true);
}

uint64_t lw_est_sub(const lw_cell *cell) {
    return lw_alu_gates(cell->port[PORT_Y_BINARY].n, true, true);
}

uint64_t lw_est_neg(const lw_cell *cell) {
    return lw_alu_gates(cell->port[PORT_Y_UNARY].n, true, true);
}
