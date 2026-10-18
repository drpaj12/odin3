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

/* Overwrites dst with the nets of src (the same length). */
static void replace_bits(lw_bits *dst, const lw_bits *src) {
    odin3_net_id *nets = dst->v.data;
    for (uint32_t k = 0; k < lw_bits_len(src); k++) {
        nets[k] = lw_bits_at(src, k);
    }
}

/* --- $shl, $shr, $sshr ---------------------------------------------------------------------- */

typedef enum shift_kind { SHIFT_SHL, SHIFT_SHR, SHIFT_SSHR } shift_kind;

/* The sizes techmap.v's _90_shift_ops_shr_shl_sshl_sshr derives from the widths (lines 77 to 99).
 */
typedef struct shift_dims {
    uint32_t width; /* WIDTH = max(A_WIDTH, Y_WIDTH) */
    uint32_t bb;    /* BB_WIDTH: the bits of B that select a stage */
    bool left;
    bool sign_extend; /* $sshr of a signed A */
} shift_dims;

/* The width whose log2 bounds the useful bits of B: Y for $shl, else WIDTH or A_WIDTH. */
static uint64_t shift_span(const lw_cell *cell, const shift_dims *dims, bool a_signed) {
    if (dims->left) {
        return cell->port[PORT_Y_SHIFT].n;
    }
    return a_signed ? dims->width : cell->port[PORT_A].n;
}

static shift_dims shift_dims_of(const lw_cell *cell, shift_kind kind) {
    uint32_t aw = cell->port[PORT_A].n;
    uint32_t yw = cell->port[PORT_Y_SHIFT].n;
    bool a_signed = cell->params[LW_P_BIN_ASIGNED].i != 0;
    shift_dims dims = {aw > yw ? aw : yw, 0, kind == SHIFT_SHL, kind == SHIFT_SSHR && a_signed};
    uint64_t span = shift_span(cell, &dims, a_signed);
    uint32_t want = lw_clog2(span) + 1;
    uint32_t bw = cell->port[PORT_B].n;
    dims.bb = want < bw ? want : bw;
    return dims;
}

/* The overflow fill: A's top bit for a sign-extending shift, else the constant 0. */
static odin3_status shift_fill(lw_ctx *lw, const lw_cell *cell, const shift_dims *dims,
                               odin3_net_id *fill) {
    if (dims->sign_extend) {
        *fill = cell->port[PORT_A].nets[cell->port[PORT_A].n - 1];
        return ODIN3_OK;
    }
    return lw_const(lw, ODIN3_BIT_0, fill);
}

/* buffer = overflow ? fill : ext(A), bitwise; `fill` is A's top bit for a sign-extending shift. */
static odin3_status shift_start(lw_ctx *lw, const lw_cell *cell, const shift_dims *dims,
                                odin3_net_id fill, lw_bits *buf) {
    uint32_t bw = cell->port[PORT_B].n;
    odin3_net_id overflow = {0};
    odin3_status st = ODIN3_OK;
    if (bw > dims->bb) {
        lw_span high = {cell->port[PORT_B].nets + dims->bb, bw - dims->bb};
        st = lw_tree(lw, LW_G_OR, high, &overflow);
    } else {
        st = lw_const(lw, ODIN3_BIT_0, &overflow);
    }
    lw_bits ext;
    lw_bits_init(&ext);
    bool a_signed = cell->params[LW_P_BIN_ASIGNED].i != 0;
    st = st == ODIN3_OK ? lw_extend(lw, cell->port[PORT_A], dims->width, a_signed, &ext) : st;
    lw_bits_clear(buf);
    for (uint32_t k = 0; st == ODIN3_OK && k < dims->width; k++) {
        odin3_net_id bit = {0};
        st = lw_mux(lw, lw_bits_at(&ext, k), fill, overflow, &bit);
        st = st == ODIN3_OK ? lw_bits_push(buf, bit) : st;
    }
    lw_bits_free(&ext);
    return st;
}

/* The net that bit k of the stage by 2^i reads when its select is 1. */
static odin3_net_id shifted_bit(const shift_dims *dims, const lw_bits *buf, uint32_t pos,
                                uint64_t amount, odin3_net_id zero) {
    if (dims->left) {
        return pos >= amount ? lw_bits_at(buf, (uint32_t)(pos - amount)) : zero;
    }
    if (pos + amount < dims->width) {
        return lw_bits_at(buf, (uint32_t)(pos + amount));
    }
    return dims->sign_extend ? lw_bits_at(buf, dims->width - 1) : zero;
}

/* One stage: next[k] = B[stage] ? shifted(buf)[k] : buf[k]; then buf = next. */
static odin3_status shift_stage(lw_ctx *lw, const shift_dims *dims, odin3_net_id sel,
                                uint32_t stage, lw_bits *buf) {
    odin3_net_id zero = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    lw_bits next;
    lw_bits_init(&next);
    for (uint32_t k = 0; st == ODIN3_OK && k < dims->width; k++) {
        odin3_net_id bit = {0};
        st = lw_mux(lw, lw_bits_at(buf, k), shifted_bit(dims, buf, k, (uint64_t)1 << stage, zero),
                    sel, &bit);
        st = st == ODIN3_OK ? lw_bits_push(&next, bit) : st;
    }
    if (st == ODIN3_OK) {
        replace_bits(buf, &next);
    }
    lw_bits_free(&next);
    return st;
}

static odin3_status shift_build(lw_ctx *lw, const lw_cell *cell, shift_kind kind, lw_bits *out) {
    shift_dims dims = shift_dims_of(cell, kind);
    odin3_net_id fill = {0};
    odin3_status st = shift_fill(lw, cell, &dims, &fill);
    lw_bits buf;
    lw_bits_init(&buf);
    st = st == ODIN3_OK ? shift_start(lw, cell, &dims, fill, &buf) : st;
    for (uint32_t stage = 0; st == ODIN3_OK && stage < dims.bb; stage++) {
        st = shift_stage(lw, &dims, cell->port[PORT_B].nets[stage], stage, &buf);
    }
    lw_bits_clear(out);
    for (uint32_t k = 0; st == ODIN3_OK && k < cell->port[PORT_Y_SHIFT].n; k++) {
        st = lw_bits_push(out, lw_bits_at(&buf, k));
    }
    lw_bits_free(&buf);
    return st;
}

odin3_status lw_build_shl(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return shift_build(lw, cell, SHIFT_SHL, out);
}

odin3_status lw_build_shr(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return shift_build(lw, cell, SHIFT_SHR, out);
}

odin3_status lw_build_sshr(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    return shift_build(lw, cell, SHIFT_SSHR, out);
}

/* The overflow tree, WIDTH muxes for the overflow, WIDTH muxes for each of the BB stages. */
static uint64_t shift_estimate(const lw_cell *cell, shift_kind kind) {
    shift_dims dims = shift_dims_of(cell, kind);
    uint32_t bw = cell->port[PORT_B].n;
    uint64_t tree = bw > dims.bb ? bw - dims.bb - 1 : 0;
    return lw_sat_add(tree, lw_sat_mul(dims.width, (uint64_t)dims.bb + 1));
}

uint64_t lw_est_shl(const lw_cell *cell) {
    return shift_estimate(cell, SHIFT_SHL);
}

uint64_t lw_est_shr(const lw_cell *cell) {
    return shift_estimate(cell, SHIFT_SHR);
}

uint64_t lw_est_sshr(const lw_cell *cell) {
    return shift_estimate(cell, SHIFT_SSHR);
}

/* --- $shiftx -------------------------------------------------------------------------------- */

/* The sizes of techmap.v's _90_shift_shiftx (lines 140 to 148). */
typedef struct shiftx_dims {
    uint32_t width; /* WIDTH: max(A, Y) plus the room a negative B shifts into */
    uint32_t bb;    /* BB_WIDTH */
    bool b_signed;
} shiftx_dims;

static shiftx_dims shiftx_dims_of(const lw_cell *cell) {
    uint32_t aw = cell->port[PORT_A].n;
    uint32_t yw = cell->port[PORT_Y_SHIFT].n;
    uint32_t wide = aw > yw ? aw : yw;
    shiftx_dims dims = {wide, 0, cell->params[LW_P_BIN_BSIGNED].i != 0};
    uint32_t want = lw_clog2(wide) + (dims.b_signed ? 2U : 1U);
    uint32_t bw = cell->port[PORT_B].n;
    dims.bb = want < bw ? want : bw;
    if (dims.b_signed) {
        dims.width = (uint32_t)((uint64_t)wide + ((uint64_t)1 << (dims.bb - 1)));
    }
    return dims;
}

/* overflow: B[BB ..] is not all zero (unsigned) or not all equal to B[BB - 1] (signed). */
static odin3_status shiftx_overflow(lw_ctx *lw, const lw_cell *cell, const shiftx_dims *dims,
                                    odin3_net_id *overflow) {
    uint32_t bw = cell->port[PORT_B].n;
    const odin3_net_id *nets = cell->port[PORT_B].nets;
    lw_bits diff;
    lw_bits_init(&diff);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = dims->bb; st == ODIN3_OK && i < bw; i++) {
        odin3_net_id bit = nets[i];
        st = dims->b_signed ? lw_xor(lw, nets[i], nets[dims->bb - 1], &bit) : ODIN3_OK;
        st = st == ODIN3_OK ? lw_bits_push(&diff, bit) : st;
    }
    st = st == ODIN3_OK ? lw_tree(lw, LW_G_OR, lw_bits_span(&diff), overflow) : st;
    lw_bits_free(&diff);
    return st;
}

/* The net bit `pos` reads from when the stage is on: `amount` places toward the high end (`high`)
 * or the low end of the buffer, x beyond it. */
static odin3_net_id shiftx_source(const lw_bits *buf, uint32_t pos, uint64_t amount, bool high,
                                  odin3_net_id unknown) {
    if (high) {
        return pos >= amount ? lw_bits_at(buf, (uint32_t)(pos - amount)) : unknown;
    }
    return pos + amount < lw_bits_len(buf) ? lw_bits_at(buf, (uint32_t)(pos + amount)) : unknown;
}

/* One stage of the buffer: buf[k] = sel ? source : buf[k]. */
typedef struct shiftx_stage_spec {
    odin3_net_id sel;
    uint64_t amount; /* places to move; a stage of the buffer's width or more blanks it */
    bool high;
} shiftx_stage_spec;

static odin3_status shiftx_stage(lw_ctx *lw, odin3_net_id unknown, const shiftx_stage_spec *spec,
                                 lw_bits *buf) {
    uint32_t width = lw_bits_len(buf);
    lw_bits next;
    lw_bits_init(&next);
    odin3_status st = ODIN3_OK;
    for (uint32_t pos = 0; st == ODIN3_OK && pos < width; pos++) {
        odin3_net_id moved = shiftx_source(buf, pos, spec->amount, spec->high, unknown);
        odin3_net_id bit = {0};
        st = lw_mux(lw, lw_bits_at(buf, pos), moved, spec->sel, &bit);
        st = st == ODIN3_OK ? lw_bits_push(&next, bit) : st;
    }
    if (st == ODIN3_OK) {
        replace_bits(buf, &next);
    }
    lw_bits_free(&next);
    return st;
}

/* buffer = {WIDTH{x}} with A in the low bits. */
static odin3_status shiftx_start(lw_ctx *lw, const lw_cell *cell, const shiftx_dims *dims,
                                 odin3_net_id unknown, lw_bits *buf) {
    (void)lw;
    lw_bits_clear(buf);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < dims->width; k++) {
        st = lw_bits_push(buf, k < cell->port[PORT_A].n ? cell->port[PORT_A].nets[k] : unknown);
    }
    return st;
}

static odin3_status shiftx_stages(lw_ctx *lw, const lw_cell *cell, const shiftx_dims *dims,
                                  odin3_net_id unknown, lw_bits *buf) {
    const odin3_net_id *sel = cell->port[PORT_B].nets;
    odin3_status st = ODIN3_OK;
    if (cell->port[PORT_B].n > dims->bb) {
        odin3_net_id overflow = {0};
        st = shiftx_overflow(lw, cell, dims, &overflow);
        shiftx_stage_spec blank = {overflow, dims->width, false}; /* all of it moves out: x */
        st = st == ODIN3_OK ? shiftx_stage(lw, unknown, &blank, buf) : st;
    }
    if (st == ODIN3_OK && dims->b_signed) {
        shiftx_stage_spec sign = {sel[dims->bb - 1], (uint64_t)1 << (dims->bb - 1), true};
        st = shiftx_stage(lw, unknown, &sign, buf);
    }
    uint32_t stages = dims->b_signed ? dims->bb - 1 : dims->bb;
    for (uint32_t i = 0; st == ODIN3_OK && i < stages; i++) {
        shiftx_stage_spec down = {sel[i], (uint64_t)1 << i, false};
        st = shiftx_stage(lw, unknown, &down, buf);
    }
    return st;
}

odin3_status lw_build_shiftx(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    shiftx_dims dims = shiftx_dims_of(cell);
    odin3_net_id unknown = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_X, &unknown);
    lw_bits buf;
    lw_bits_init(&buf);
    st = st == ODIN3_OK ? shiftx_start(lw, cell, &dims, unknown, &buf) : st;
    st = st == ODIN3_OK ? shiftx_stages(lw, cell, &dims, unknown, &buf) : st;
    lw_bits_clear(out);
    for (uint32_t k = 0; st == ODIN3_OK && k < cell->port[PORT_Y_SHIFT].n; k++) {
        st = lw_bits_push(out, lw_bits_at(&buf, k));
    }
    lw_bits_free(&buf);
    return st;
}

/* The overflow gates, WIDTH muxes for them, and WIDTH muxes for the sign stage and each stage. */
uint64_t lw_est_shiftx(const lw_cell *cell) {
    shiftx_dims dims = shiftx_dims_of(cell);
    uint64_t extra = cell->port[PORT_B].n > dims.bb ? cell->port[PORT_B].n - dims.bb : 0;
    uint64_t gates = 0;
    uint64_t layers = dims.bb;
    if (extra != 0) {
        gates = dims.b_signed ? 2 * extra - 1 : extra - 1;
        layers++;
    }
    return lw_sat_add(gates, lw_sat_mul(dims.width, layers));
}
