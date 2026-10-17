/*
 * cmp.c — the `techmap.v` family of `lower`, part 3 (3B spec §5.4, LW-6; plan Task 4): the
 * comparisons $lt $le $gt $ge, built on the subtraction of alu.c as alumacc.cc does (lines 44 to
 * 143 and 478 to 530): the flags come from the carry and propagate bits of A - B and the sum bits
 * are not built. Every builder appends bit nodes on fresh nets and puts the net of each output
 * bit into `out`; Y[k > 0] is the constant-0 net.
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Port positions: A B Y. */
enum { PORT_A = 0, PORT_B = 1 };

typedef enum cmp_kind { CMP_LT, CMP_LE, CMP_GT, CMP_GE } cmp_kind;

/*
 * The "less than" flag of the subtraction A - B (alumacc.cc get_lt/get_cf/get_of/get_sf): unsigned
 * it is the inverted carry out of the top bit; signed it is overflow ^ sign, where overflow is
 * CO[W-1] ^ CO[W-2] and the sign is Y[W-1] = P[W-1] ^ CO[W-2] (CO[W-2] is CI when W = 1).
 */
static odin3_status less_than(lw_ctx *lw, const lw_alu_out *res, odin3_net_id carry_in,
                              bool is_signed, odin3_net_id *flag) {
    uint32_t width = lw_bits_len(&res->carry);
    odin3_net_id top = lw_bits_at(&res->carry, width - 1);
    if (!is_signed) {
        return lw_not(lw, top, flag);
    }
    odin3_net_id below = width >= 2 ? lw_bits_at(&res->carry, width - 2) : carry_in;
    odin3_net_id overflow = {0};
    odin3_net_id sign = {0};
    odin3_status st = lw_xor(lw, top, below, &overflow);
    st = st == ODIN3_OK ? lw_xor(lw, lw_bits_at(&res->prop, width - 1), below, &sign) : st;
    return st == ODIN3_OK ? lw_xor(lw, overflow, sign, flag) : st;
}

/* $le = lt | eq; $gt = ~(lt | eq); $ge = gt | eq, where eq is the AND of every P bit. */
static odin3_status compare_flag(lw_ctx *lw, cmp_kind kind, const lw_alu_out *res,
                                 odin3_net_id less, odin3_net_id *flag) {
    *flag = less;
    if (kind == CMP_LT) {
        return ODIN3_OK;
    }
    odin3_net_id equal = {0};
    odin3_status st = lw_tree(lw, LW_G_AND, lw_bits_span(&res->prop), &equal);
    st = st == ODIN3_OK ? lw_or(lw, less, equal, flag) : st;
    if (st == ODIN3_OK && kind != CMP_LE) {
        st = lw_not(lw, *flag, flag);
    }
    if (st == ODIN3_OK && kind == CMP_GE) {
        st = lw_or(lw, *flag, equal, flag);
    }
    return st;
}

static odin3_status build_cmp(lw_ctx *lw, const lw_cell *cell, cmp_kind kind, lw_bits *out) {
    uint32_t wa = cell->port[PORT_A].n;
    uint32_t wb = cell->port[PORT_B].n;
    lw_alu_args args = {cell->port[PORT_A], cell->port[PORT_B], {0}, true, false, false,
                        wa > wb ? wa : wb};
    args.is_signed = lw_both_signed(lw, cell);
    odin3_status st = lw_const(lw, ODIN3_BIT_1, &args.carry_in);
    lw_alu_out res;
    lw_alu_out_init(&res);
    st = st == ODIN3_OK ? lw_alu(lw, &args, &res) : st;
    odin3_net_id less = {0};
    odin3_net_id flag = {0};
    st = st == ODIN3_OK ? less_than(lw, &res, args.carry_in, args.is_signed, &less) : st;
    st = st == ODIN3_OK ? compare_flag(lw, kind, &res, less, &flag) : st;
    st = st == ODIN3_OK ? lw_finish_flag(lw, cell, flag, out) : st;
    lw_alu_out_free(&res);
    return st;
}

odin3_status lw_build_lt(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return build_cmp(lw, cell, CMP_LT, out);
}

odin3_status lw_build_le(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return build_cmp(lw, cell, CMP_LE, out);
}

odin3_status lw_build_gt(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return build_cmp(lw, cell, CMP_GT, out);
}

odin3_status lw_build_ge(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return build_cmp(lw, cell, CMP_GE, out);
}

/* The subtraction without sum bits, the flag(s) of `kind`, and the AND tree of P for eq. */
static uint64_t cmp_estimate(const lw_cell *cell, cmp_kind kind) {
    uint64_t wa = cell->port[PORT_A].n;
    uint64_t wb = cell->port[PORT_B].n;
    uint64_t width = wa > wb ? wa : wb;
    bool is_signed = cell->params[LW_P_BIN_ASIGNED].i != 0 && cell->params[LW_P_BIN_BSIGNED].i != 0;
    uint64_t gates = lw_alu_gates(width, true, false);
    gates = lw_sat_add(gates, is_signed ? 3 : 1);
    if (kind != CMP_LT) {
        gates = lw_sat_add(gates, width - 1);
        gates = lw_sat_add(gates, (uint64_t)kind); /* le: 1 (OR), gt: 2 (OR, NOT), ge: 3 */
    }
    return gates;
}

uint64_t lw_est_lt(const lw_cell *cell) {
    return cmp_estimate(cell, CMP_LT);
}

uint64_t lw_est_le(const lw_cell *cell) {
    return cmp_estimate(cell, CMP_LE);
}

uint64_t lw_est_gt(const lw_cell *cell) {
    return cmp_estimate(cell, CMP_GT);
}

uint64_t lw_est_ge(const lw_cell *cell) {
    return cmp_estimate(cell, CMP_GE);
}
