/*
 * pmux_shift.c — the `techmap.v` family of `lower`, part 1 (3B spec §5.1 to §5.3, LW-6; plan
 * Task 4): $pmux, $shl $shr $sshr and $shiftx. Each builder follows Yosys 0.55's map (the source
 * lines are cited at the builder); the contract is tier T (the same algorithm; constants are left
 * in for `opt -fast`, spec §2.2). Every builder appends bit nodes on fresh nets and puts the net of
 * each output bit into `out`.
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Port positions: $pmux A B S Y; shifts A B Y. */
enum { PORT_A = 0, PORT_B = 1, PORT_S = 2, PORT_Y_PMUX = 3, PORT_Y_SHIFT = 2 };

/* --- $pmux ---------------------------------------------------------------------------------- */

/* Scratch of the $pmux builder: the net of |S| and the AND terms of the bit being built. */
typedef struct pmux_scratch {
    odin3_net_id any;
    lw_bits terms;
} pmux_scratch;

/*
 * Output bit `bit` of a $pmux (simplemap.cc simplemap_pmux, lines 441 to 480; techmap.v
 * _90_pmux is the same structure): Y_B = OR over the selects of AND(B[i * W + bit], S[i]), then
 * Y = MUX(A[bit], Y_B, any).
 */
static odin3_status pmux_bit(lw_ctx *lw, const lw_cell *cell, uint32_t bit, pmux_scratch *work,
                             odin3_net_id *out_net) {
    uint32_t width = cell->port[PORT_Y_PMUX].n;
    lw_bits_clear(&work->terms);
    odin3_status st = ODIN3_OK;
    for (uint32_t sel = 0; st == ODIN3_OK && sel < cell->port[PORT_S].n; sel++) {
        odin3_net_id term = {0};
        st = lw_and(lw, cell->port[PORT_B].nets[(size_t)sel * width + bit],
                    cell->port[PORT_S].nets[sel], &term);
        st = st == ODIN3_OK ? lw_bits_push(&work->terms, term) : st;
    }
    odin3_net_id picked = {0};
    st = st == ODIN3_OK ? lw_tree(lw, LW_G_OR, lw_bits_span(&work->terms), &picked) : st;
    return st == ODIN3_OK ? lw_mux(lw, cell->port[PORT_A].nets[bit], picked, work->any, out_net)
                          : st;
}

odin3_status lw_build_pmux(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    pmux_scratch work = {{0}, {{0}}};
    odin3_status st = lw_tree(lw, LW_G_OR, cell->port[PORT_S], &work.any);
    lw_bits_init(&work.terms);
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < cell->port[PORT_Y_PMUX].n; bit++) {
        odin3_net_id result = {0};
        st = pmux_bit(lw, cell, bit, &work, &result);
        st = st == ODIN3_OK ? lw_bits_push(out, result) : st;
    }
    lw_bits_free(&work.terms);
    return st;
}

/* |S| - 1 gates for `any`; per bit |S| ANDs, |S| - 1 ORs and one MUX. */
uint64_t lw_est_pmux(const lw_cell *cell) {
    uint64_t sels = cell->port[PORT_S].n;
    return lw_sat_add(sels - 1, lw_sat_mul(cell->port[PORT_Y_PMUX].n, 2 * sels));
}
