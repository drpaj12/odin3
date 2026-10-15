/*
 * bitwise.c — the `simplemap` family of `lower` (3B spec §4, LW-5; plan Task 3): $not, the bitwise
 * binary cells, $mux, the reductions, the logical cells, $eq and $ne, transcribed from Yosys 0.55
 * passes/techmap/simplemap.cc. The gate-for-gate structure (types, pairing order, count) is the
 * contract (tier S, spec §14.2). Every builder appends bit nodes on fresh nets and puts the net of
 * each output bit into `out`; Y[k > 0] of a one-bit result is the constant-0 net.
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Port positions: unary A Y; binary A B Y; $mux A B S Y. */
enum { PORT_A = 0, PORT_B = 1, PORT_S = 2, PORT_Y_UNARY = 1, PORT_Y_BINARY = 2, PORT_Y_MUX = 3 };

/* Pushes the constant-0 net into `out` until it holds `width` bits (simplemap's `connect Y[k] 0`).
 */
static odin3_status pad_zero(lw_ctx *lw, lw_bits *out, uint32_t width) {
    if (lw_bits_len(out) >= width) {
        return ODIN3_OK;
    }
    odin3_net_id zero = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    while (st == ODIN3_OK && lw_bits_len(out) < width) {
        st = lw_bits_push(out, zero);
    }
    return st;
}

/* Y[0] = root, Y[k > 0] = 0: the result shape of every one-bit cell (reductions, logic, eq/ne). */
static odin3_status finish_flag(lw_ctx *lw, const lw_cell *cell, odin3_net_id root, lw_bits *out) {
    lw_bits_clear(out);
    odin3_status st = lw_bits_push(out, root);
    return st == ODIN3_OK ? pad_zero(lw, out, cell->port[cell->out_port].n) : st;
}

/* --- $not and the bitwise binary cells ------------------------------------------------------ */

odin3_status lw_build_not(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    uint32_t width = cell->port[PORT_Y_UNARY].n;
    lw_bits ext;
    lw_bits_init(&ext);
    bool is_signed = lw_both_signed(lw, cell);
    odin3_status st = lw_extend(lw, cell->port[PORT_A], width, is_signed, &ext);
    lw_bits_clear(out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        odin3_net_id result = {0};
        st = lw_not(lw, lw_bits_at(&ext, i), &result);
        st = st == ODIN3_OK ? lw_bits_push(out, result) : st;
    }
    lw_bits_free(&ext);
    return st;
}

/* Both operands of a binary cell extended to `width` by the both-signed flag. */
static odin3_status extend_both(lw_ctx *lw, const lw_cell *cell, uint32_t width, lw_bits *lhs,
                                lw_bits *rhs) {
    bool is_signed = lw_both_signed(lw, cell);
    odin3_status st = lw_extend(lw, cell->port[PORT_A], width, is_signed, lhs);
    return st == ODIN3_OK ? lw_extend(lw, cell->port[PORT_B], width, is_signed, rhs) : st;
}

/* Y[i] = gate(ext(A)[i], ext(B)[i]) over `width` bits, into `out` (cleared first). */
static odin3_status bitop(lw_ctx *lw, const lw_cell *cell, lw_kind kind, lw_bits *out,
                          uint32_t width) {
    lw_bits lhs;
    lw_bits rhs;
    lw_bits_init(&lhs);
    lw_bits_init(&rhs);
    odin3_status st = extend_both(lw, cell, width, &lhs, &rhs);
    lw_bits_clear(out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        const odin3_net_id in[2] = {lw_bits_at(&lhs, i), lw_bits_at(&rhs, i)};
        odin3_net_id result = {0};
        st = lw_gate(lw, kind, in, 2, &result);
        st = st == ODIN3_OK ? lw_bits_push(out, result) : st;
    }
    lw_bits_free(&lhs);
    lw_bits_free(&rhs);
    return st;
}

odin3_status lw_build_and(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return bitop(lw, cell, LW_G_AND, out, cell->port[PORT_Y_BINARY].n);
}

odin3_status lw_build_or(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return bitop(lw, cell, LW_G_OR, out, cell->port[PORT_Y_BINARY].n);
}

odin3_status lw_build_xor(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return bitop(lw, cell, LW_G_XOR, out, cell->port[PORT_Y_BINARY].n);
}

odin3_status lw_build_xnor(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return bitop(lw, cell, LW_G_XNOR, out, cell->port[PORT_Y_BINARY].n);
}

/* --- $mux ----------------------------------------------------------------------------------- */

odin3_status lw_build_mux(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    uint32_t width = cell->port[PORT_Y_MUX].n;
    odin3_net_id sel = cell->port[PORT_S].nets[0];
    lw_bits_clear(out);
    odin3_status st = odin3_vec_reserve(&out->v, width);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        odin3_net_id result = {0};
        st = lw_mux(lw, cell->port[PORT_A].nets[i], cell->port[PORT_B].nets[i], sel, &result);
        st = st == ODIN3_OK ? lw_bits_push(out, result) : st;
    }
    return st;
}

/* --- reductions and logical cells ----------------------------------------------------------- */

/* $reduce_*: the pairing tree of the kind; $reduce_xnor adds one $_NOT_ as the root. */
static odin3_status reduce(lw_ctx *lw, const lw_cell *cell, lw_kind kind, bool invert,
                           lw_bits *out) {
    odin3_net_id root = {0};
    odin3_status st = lw_tree(lw, kind, cell->port[PORT_A], &root);
    st = st == ODIN3_OK && invert ? lw_not(lw, root, &root) : st;
    return st == ODIN3_OK ? finish_flag(lw, cell, root, out) : st;
}

odin3_status lw_build_reduce_and(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return reduce(lw, cell, LW_G_AND, false, out);
}

/* $reduce_or and $reduce_bool are one rule (simplemap: both are $_OR_ trees). */
odin3_status lw_build_reduce_or(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return reduce(lw, cell, LW_G_OR, false, out);
}

odin3_status lw_build_reduce_xor(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return reduce(lw, cell, LW_G_XOR, false, out);
}

odin3_status lw_build_reduce_xnor(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return reduce(lw, cell, LW_G_XOR, true, out);
}

odin3_status lw_build_logic_not(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    odin3_net_id root = {0};
    odin3_status st = lw_tree(lw, LW_G_OR, cell->port[PORT_A], &root);
    st = st == ODIN3_OK ? lw_not(lw, root, &root) : st;
    return st == ODIN3_OK ? finish_flag(lw, cell, root, out) : st;
}

/* $logic_and / $logic_or: the $_OR_ tree of each operand, then the gate of the cell. */
static odin3_status logic_bin(lw_ctx *lw, const lw_cell *cell, lw_kind kind, lw_bits *out) {
    odin3_net_id lhs = {0};
    odin3_net_id rhs = {0};
    odin3_status st = lw_tree(lw, LW_G_OR, cell->port[PORT_A], &lhs);
    st = st == ODIN3_OK ? lw_tree(lw, LW_G_OR, cell->port[PORT_B], &rhs) : st;
    odin3_net_id root = {0};
    const odin3_net_id in[2] = {lhs, rhs};
    st = st == ODIN3_OK ? lw_gate(lw, kind, in, 2, &root) : st;
    return st == ODIN3_OK ? finish_flag(lw, cell, root, out) : st;
}

odin3_status lw_build_logic_and(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return logic_bin(lw, cell, LW_G_AND, out);
}

odin3_status lw_build_logic_or(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return logic_bin(lw, cell, LW_G_OR, out);
}

/* --- $eq and $ne ---------------------------------------------------------------------------- */

/* simplemap_eqne: W = max widths; per bit $_XOR_ of the extended operands; the $_OR_ tree of
 * those; $ne: the root; $eq: one more $_NOT_. */
static odin3_status eqne(lw_ctx *lw, const lw_cell *cell, bool invert, lw_bits *out) {
    uint32_t wa = cell->port[PORT_A].n;
    uint32_t wb = cell->port[PORT_B].n;
    uint32_t width = wa > wb ? wa : wb;
    lw_bits diff;
    lw_bits_init(&diff);
    odin3_status st = bitop(lw, cell, LW_G_XOR, &diff, width);
    odin3_net_id root = {0};
    st = st == ODIN3_OK ? lw_tree(lw, LW_G_OR, lw_bits_span(&diff), &root) : st;
    st = st == ODIN3_OK && invert ? lw_not(lw, root, &root) : st;
    lw_bits_free(&diff);
    return st == ODIN3_OK ? finish_flag(lw, cell, root, out) : st;
}

odin3_status lw_build_eq(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return eqne(lw, cell, true, out);
}

odin3_status lw_build_ne(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return eqne(lw, cell, false, out);
}
