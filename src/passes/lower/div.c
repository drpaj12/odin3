/*
 * div.c — the divider of `lower` (3B spec §5.6, LW-6; plan Task 5): $div and
 * $mod as techmap.v builds them, `$__div_mod_u` (lines 315 to 343, the
 * restoring divider) inside `$__div_mod_trunc` (lines 346 to 385, the sign
 * handling). With WIDTH = max(A_WIDTH, B_WIDTH, Y_WIDTH):
 *
 *   - A and B are extended to WIDTH; when both are signed a negative one is
 * replaced by its absolute value (`-X` is a `$neg`, the choice a `$_MUX_` row
 * on the sign bit);
 *   - WIDTH stages: stage i (shift s = WIDTH - 1 - i) compares the running
 * remainder (the dividend at stage 0) with {B, s zeros}, quotient bit s is the
 * result of `>=`, and the difference is kept when it is set (a `$_MUX_` row
 * over the low WIDTH bits of the subtraction);
 *   - $div: the quotient is negated when both are signed and the sign bits
 * differ; $mod: the remainder is negated when both are signed and A is
 * negative; the low Y_WIDTH bits are Y.
 *
 * B = 0 gives quotient all ones and remainder A (so the signed results follow
 * the same rules); simlib leaves them x, a refinement (spec §16 row 16). The
 * "both signed" flag is the registry rule of spec §3.3. Logic Yosys builds and
 * `opt_clean` removes (the last stage's difference for $div) is not built;
 * constants are left in for `opt -fast` (spec §2.2).
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Port positions: A B Y. */
enum { PORT_A = 0, PORT_B = 1, PORT_Y = 2 };

/* Estimate loops stop once the sum is far beyond any cap (2^40). */
#define DIV_ESTIMATE_LIMIT ((uint64_t)1 << 40)

void lw_divmod_out_init(lw_divmod_out *res) {
    lw_bits_init(&res->quot);
    lw_bits_init(&res->rem);
}

void lw_divmod_out_free(lw_divmod_out *res) {
    lw_bits_free(&res->quot);
    lw_bits_free(&res->rem);
}

/* {B, shift zeros}: `shift` zero nets, then the bits of B (the subtrahend of
 * one stage). */
static odin3_status shifted_divisor(lw_span divisor, uint32_t shift, odin3_net_id zero,
                                    lw_bits *out) {
    lw_bits_clear(out);
    odin3_status st = odin3_vec_reserve(&out->v, (size_t)divisor.n + shift);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < shift; pos++) {
        st = lw_bits_push(out, zero);
    }
    for (uint32_t pos = 0; st == ODIN3_OK && pos < divisor.n; pos++) {
        st = lw_bits_push(out, divisor.nets[pos]);
    }
    return st;
}

/* The scratch of the divider: the running remainder, the next one and the
 * shifted divisor. */
typedef struct div_stage {
    lw_bits cur;
    lw_bits next;
    lw_bits rhs;
} div_stage;

/* next[k] = flag ? (cur - rhs)[k] : cur[k] for the low `cur` bits (the kept difference). */
static odin3_status keep_difference(lw_ctx *lw, div_stage *stage, odin3_net_id flag) {
    const lw_bits *cur = &stage->cur;
    lw_alu_args args = {lw_bits_span(cur),       lw_bits_span(&stage->rhs), {0}, true, false, true,
                        lw_bits_len(&stage->rhs)};
    odin3_status st = lw_const(lw, ODIN3_BIT_1, &args.carry_in);
    lw_alu_out diff;
    lw_alu_out_init(&diff);
    st = st == ODIN3_OK ? lw_alu(lw, &args, &diff) : st;
    lw_bits_clear(&stage->next);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < lw_bits_len(cur); pos++) {
        odin3_net_id kept = {0};
        st = lw_mux(lw, lw_bits_at(cur, pos), lw_bits_at(&diff.sum, pos), flag, &kept);
        st = st == ODIN3_OK ? lw_bits_push(&stage->next, kept) : st;
    }
    lw_alu_out_free(&diff);
    return st;
}

/* Stage `step`: the quotient bit, and (unless the last stage of a $div) the next remainder. */
static odin3_status divide_step(lw_ctx *lw, const lw_divmod_args *args, uint32_t step,
                                div_stage *stage, lw_divmod_out *res) {
    uint32_t width = args->dividend.n;
    uint32_t shift = width - 1 - step;
    odin3_net_id flag = {0};
    odin3_net_id zero = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    st = st == ODIN3_OK ? shifted_divisor(args->divisor, shift, zero, &stage->rhs) : st;
    lw_cmp_args cmp = {lw_bits_span(&stage->cur), lw_bits_span(&stage->rhs), LW_CMP_GE, false};
    st = st == ODIN3_OK ? lw_compare(lw, &cmp, &flag) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    lw_bits_set(&res->quot, shift, flag);
    if (shift > 0 || args->want_rem) {
        st = keep_difference(lw, stage, flag);
        lw_bits swap = stage->cur;
        stage->cur = stage->next;
        stage->next = swap;
    }
    return st;
}

odin3_status lw_divmod_u(lw_ctx *lw, const lw_divmod_args *args, lw_divmod_out *res) {
    uint32_t width = args->dividend.n;
    div_stage stage;
    lw_bits_init(&stage.cur);
    lw_bits_init(&stage.next);
    lw_bits_init(&stage.rhs);
    lw_bits_clear(&res->quot);
    lw_bits_clear(&res->rem);
    odin3_net_id zero = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < width; pos++) {
        st = lw_bits_push(&stage.cur, args->dividend.nets[pos]);
        st = st == ODIN3_OK ? lw_bits_push(&res->quot, zero) : st;
    }
    for (uint32_t step = 0; st == ODIN3_OK && step < width; step++) {
        st = divide_step(lw, args, step, &stage, res);
    }
    for (uint32_t pos = 0; st == ODIN3_OK && args->want_rem && pos < width; pos++) {
        st = lw_bits_push(&res->rem, lw_bits_at(&stage.cur, pos));
    }
    lw_bits_free(&stage.cur);
    lw_bits_free(&stage.next);
    lw_bits_free(&stage.rhs);
    return st;
}

/* Stage `shift` costs a comparison and, unless it is the last stage of a $div,
 * the difference (W + shift bits) and W multiplexers. */
uint64_t lw_divmod_u_gates(uint64_t width, bool want_rem) {
    uint64_t gates = 0;
    for (uint64_t shift = 0; shift < width && gates < DIV_ESTIMATE_LIMIT; shift++) {
        uint64_t wide = width + shift;
        lw_cmp_shape shape = {wide, LW_CMP_GE, false};
        gates = lw_sat_add(gates, lw_compare_gates(&shape));
        if (shift > 0 || want_rem) {
            gates = lw_sat_add(gates, lw_sat_add(lw_alu_gates(wide, true, true), width));
        }
    }
    return gates;
}

/* --- $div and $mod
 * -------------------------------------------------------------------------- */

/* out = sel ? -in : in over the bits of `in`: a $neg and a multiplexer row. */
static odin3_status negate_if(lw_ctx *lw, const lw_bits *in, odin3_net_id sel, lw_bits *out) {
    lw_bits neg;
    lw_bits_init(&neg);
    odin3_status st = lw_neg_bits(lw, lw_bits_span(in), lw_bits_len(in), false, &neg);
    lw_bits_clear(out);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < lw_bits_len(in); pos++) {
        odin3_net_id picked = {0};
        st = lw_mux(lw, lw_bits_at(in, pos), lw_bits_at(&neg, pos), sel, &picked);
        st = st == ODIN3_OK ? lw_bits_push(out, picked) : st;
    }
    lw_bits_free(&neg);
    return st;
}

/* WIDTH of techmap.v's $__div_mod_trunc: the widest of A, B and Y. */
static uint32_t work_width(const lw_cell *cell) {
    uint32_t top =
        cell->port[PORT_A].n > cell->port[PORT_B].n ? cell->port[PORT_A].n : cell->port[PORT_B].n;
    return top > cell->port[PORT_Y].n ? top : cell->port[PORT_Y].n;
}

/* The scratch of one $div or $mod. */
typedef struct divmod_work {
    lw_bits a;   /* A extended to WIDTH, then |A| */
    lw_bits b;   /* B likewise */
    lw_bits tmp; /* operand sign fix-up */
    lw_divmod_out res;
    lw_bits fixed;
    odin3_net_id sign_a; /* the sign bits of the extended operands, before the
                            absolute values */
    odin3_net_id sign_b;
} divmod_work;

/* The extended operands, absolute values when both are signed. */
static odin3_status prepare(lw_ctx *lw, const lw_cell *cell, bool is_signed, divmod_work *work,
                            uint32_t width) {
    odin3_status st = lw_extend(lw, cell->port[PORT_A], width, is_signed, &work->a);
    st = st == ODIN3_OK ? lw_extend(lw, cell->port[PORT_B], width, is_signed, &work->b) : st;
    if (st == ODIN3_OK) {
        work->sign_a = lw_bits_at(&work->a, width - 1);
        work->sign_b = lw_bits_at(&work->b, width - 1);
    }
    if (st == ODIN3_OK && is_signed) {
        st = negate_if(lw, &work->a, lw_bits_at(&work->a, width - 1), &work->tmp);
        lw_bits swap = work->a;
        work->a = work->tmp;
        work->tmp = swap;
        st = st == ODIN3_OK ? negate_if(lw, &work->b, lw_bits_at(&work->b, width - 1), &work->tmp)
                            : st;
        swap = work->b;
        work->b = work->tmp;
        work->tmp = swap;
    }
    return st;
}

static odin3_status divmod(lw_ctx *lw, const lw_cell *cell, bool want_rem, lw_bits *out) {
    divmod_work work;
    lw_bits_init(&work.a);
    lw_bits_init(&work.b);
    lw_bits_init(&work.tmp);
    lw_bits_init(&work.fixed);
    lw_divmod_out_init(&work.res);
    uint32_t width = work_width(cell);
    bool is_signed = lw_both_signed(lw, cell);
    odin3_status st = prepare(lw, cell, is_signed, &work, width);
    lw_divmod_args args = {lw_bits_span(&work.a), lw_bits_span(&work.b), want_rem};
    st = st == ODIN3_OK ? lw_divmod_u(lw, &args, &work.res) : st;
    const lw_bits *raw = want_rem ? &work.res.rem : &work.res.quot;
    if (st == ODIN3_OK && is_signed) {
        odin3_net_id sel = work.sign_a;
        st = want_rem ? ODIN3_OK : lw_xor(lw, work.sign_a, work.sign_b, &sel);
        st = st == ODIN3_OK ? negate_if(lw, raw, sel, &work.fixed) : st;
        raw = &work.fixed;
    }
    lw_bits_clear(out);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < cell->port[PORT_Y].n; pos++) {
        st = lw_bits_push(out, lw_bits_at(raw, pos));
    }
    lw_bits_free(&work.a);
    lw_bits_free(&work.b);
    lw_bits_free(&work.tmp);
    lw_bits_free(&work.fixed);
    lw_divmod_out_free(&work.res);
    return st;
}

odin3_status lw_build_div(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return divmod(lw, cell, false, out);
}

odin3_status lw_build_mod(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return divmod(lw, cell, true, out);
}

/* An upper bound: the divider, both absolute values and, signed, the result's
 * negation (and for $div the sign XOR), each a $neg row and W multiplexers. */
static uint64_t divmod_estimate(const lw_cell *cell, bool want_rem) {
    uint64_t width = work_width(cell);
    uint64_t gates = lw_divmod_u_gates(width, want_rem);
    bool is_signed = cell->params[LW_P_BIN_ASIGNED].i != 0 && cell->params[LW_P_BIN_BSIGNED].i != 0;
    if (is_signed) {
        uint64_t negation = lw_sat_add(lw_alu_gates(width, true, true), width);
        gates = lw_sat_add(gates, lw_sat_mul(negation, 3));
        gates = lw_sat_add(gates, 1);
    }
    return gates;
}

uint64_t lw_est_div(const lw_cell *cell) {
    return divmod_estimate(cell, false);
}

uint64_t lw_est_mod(const lw_cell *cell) {
    return divmod_estimate(cell, true);
}
