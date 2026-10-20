/*
 * mul_split.c — the oversize `$mul` split (4.2 spec X-4, X-8, X-10). A `$mul` whose wider operand
 * is above the arch's multiplier width W is rewritten, one level at a time, into smaller `$mul`
 * cells and `$add` cells (the cases and piece layout of Parmys's iterate_multipliers):
 *   both operands above W: four products and two adders (add0 the cross terms, add1 the total),
 *   only A above W:       A0*B and A1*B and one adder,
 *   only B above W:       A*B0 and A*B1 and one adder (B0 the low b - W bits, as Parmys).
 * A piece that is still above W is a new `$mul` the driver reaches later (a worklist in node-ID
 * order, no recursion). The adders are `$add` cells marked "no top stage" so that the adder rule
 * builds Parmys's chain of (size + 1) cells; the probe table of
 * tests/partial_map/probes/README.md fixes the sizes. Operands are zero-extended (D1), where
 * Parmys sign-extends the low product of the B split (a Parmys defect not copied).
 */
#include "passes/partial_map/hm_kit.h"

#include <string.h>

enum { PORT_A, PORT_B, PORT_Y };
enum { WORD_PARAMS = 5 };

typedef struct split {
    hm_ctx *hc;
    odin3_node_id src;
    odin3_celltype_id mul;
    odin3_celltype_id add;
    odin3_net_id gnd;
    uint32_t width;   /* W */
    hm_mul_dims dims; /* the source */
    odin3_vec nets;   /* odin3_net_id: operands, then the fresh nets of every piece */
    odin3_vec ties;   /* hm_tie */
    odin3_net_id *an; /* the operand nets */
    odin3_net_id *bn;
    odin3_net_id *cursor; /* the next unused entry of nets */
} split;

/* A port of a node to build: the nets and how many of them the port takes. */
typedef struct port_in {
    const odin3_net_id *nets;
    uint32_t count;
} port_in;

/* nets[0 .. n) zeroed and stable (no growth after this call). */
static odin3_status scratch(odin3_vec *vec, size_t count) {
    odin3_status st = odin3_vec_reserve(vec, count);
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = odin3_vec_push(vec) != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

/* `count` entries of the scratch; the first `fresh` of them fresh nets. */
typedef struct take_req {
    uint32_t count;
    uint32_t fresh;
} take_req;

static odin3_status take(split *sp, take_req req, odin3_net_id **out) {
    odin3_net_id *base = sp->cursor;
    odin3_status st = hm_fresh_nets(sp->hc, req.fresh, base);
    sp->cursor += req.count;
    *out = base;
    return st;
}

typedef struct gather_req {
    uint32_t port;
    uint32_t count;
} gather_req;

static void gather(const split *sp, gather_req req, odin3_net_id *dst) {
    odin3_pinslice pins = odin3_node_port(sp->hc->module, sp->src, req.port);
    for (uint32_t i = 0; i < req.count; i++) {
        dst[i] = i < pins.count ? odin3_pin_net(sp->hc->module, (odin3_pin_id){pins.first.v + i})
                                : (odin3_net_id){0};
    }
}

static odin3_status new_mul(split *sp, const char *role, port_in left, port_in right,
                            const odin3_net_id *out) {
    odin3_value params[WORD_PARAMS] = {odin3_value_int(0), odin3_value_int(0),
                                       odin3_value_int(left.count), odin3_value_int(right.count),
                                       odin3_value_int(left.count + right.count)};
    odin3_netvec ports[3] = {
        {left.nets, left.count}, {right.nets, right.count}, {out, left.count + right.count}};
    odin3_node_id node = {0};
    return hm_new_node(sp->hc, &(hm_node_req){sp->mul, params, WORD_PARAMS, ports, role}, &node);
}

/* An `$add` of the two ports with `size` output bits (the larger port), marked carry-less. */
static odin3_status new_add(split *sp, const char *role, port_in left, port_in right,
                            const odin3_net_id *out) {
    uint32_t size = left.count > right.count ? left.count : right.count;
    odin3_value params[WORD_PARAMS] = {odin3_value_int(0), odin3_value_int(0),
                                       odin3_value_int(left.count), odin3_value_int(right.count),
                                       odin3_value_int(size)};
    odin3_netvec ports[3] = {{left.nets, left.count}, {right.nets, right.count}, {out, size}};
    odin3_node_id node = {0};
    odin3_status st =
        hm_new_node(sp->hc, &(hm_node_req){sp->add, params, WORD_PARAMS, ports, role}, &node);
    return st == ODIN3_OK ? hm_mark_nocarry(sp->hc, node) : st;
}

/* Y[k] <- net for k below Y_WIDTH (the ties are reserved by the caller). */
static void tie_y(split *sp, uint32_t first_bit, const odin3_net_id *nets, uint32_t count) {
    odin3_pinslice ypins = odin3_node_port(sp->hc->module, sp->src, PORT_Y);
    for (uint32_t i = 0; i < count && first_bit + i < sp->dims.y; i++) {
        odin3_net_id ynet =
            odin3_pin_net(sp->hc->module, (odin3_pin_id){ypins.first.v + first_bit + i});
        hm_tie *tie = odin3_vec_push(&sp->ties); /* reserved: never NULL */
        *tie = (hm_tie){ynet, nets[i]};
    }
}

/* Only A above W: m00 = A[0,W) * B, m10 = A[W,a) * B, add0 = m00.out[W,..) + m10.out. */
static odin3_status split_a(split *sp) {
    uint32_t wid = sp->width;
    uint32_t a1 = sp->dims.a - wid;
    uint32_t bwid = sp->dims.b;
    odin3_net_id *lo = NULL;
    odin3_net_id *hi = NULL;
    odin3_net_id *sum = NULL;
    odin3_status st = take(sp, (take_req){wid + bwid, wid + bwid}, &lo);
    st = st == ODIN3_OK ? take(sp, (take_req){a1 + bwid, a1 + bwid}, &hi) : st;
    st = st == ODIN3_OK ? take(sp, (take_req){a1 + bwid, a1 + bwid}, &sum) : st;
    st = st == ODIN3_OK ? new_mul(sp, "m00", (port_in){sp->an, wid}, (port_in){sp->bn, bwid}, lo)
                        : st;
    st = st == ODIN3_OK
             ? new_mul(sp, "m10", (port_in){sp->an + wid, a1}, (port_in){sp->bn, bwid}, hi)
             : st;
    st = st == ODIN3_OK
             ? new_add(sp, "add0", (port_in){lo + wid, bwid}, (port_in){hi, a1 + bwid}, sum)
             : st;
    if (st == ODIN3_OK) {
        tie_y(sp, 0, lo, wid);
        tie_y(sp, wid, sum, a1 + bwid);
    }
    return st;
}

/* The port of add0 that takes the low product's upper bits (`awid` of them at `bits`). With
 * `--parmys-split-ext` (D19) Parmys's sign extension: the top bit repeated up to `awid + wid`. */
static odin3_status low_port(split *sp, const odin3_net_id *bits, port_in *out) {
    uint32_t awid = sp->dims.a;
    *out = (port_in){bits, awid};
    if (!sp->hc->parmys_split_ext) {
        return ODIN3_OK;
    }
    odin3_net_id *ext = NULL;
    odin3_status st = take(sp, (take_req){awid + sp->width, 0}, &ext);
    for (uint32_t i = 0; st == ODIN3_OK && i < awid + sp->width; i++) {
        ext[i] = bits[i < awid ? i : awid - 1U];
    }
    *out = (port_in){ext, awid + sp->width};
    return st;
}

/* Only B above W: m00 = A * B[0,b0), m01 = A * B[b0,b), add0 = m00.out[b0,..) + m01.out. With
 * `--parmys-split-ext` (D19) the low product is extended by its top bit, as Parmys does. */
static odin3_status split_b(split *sp) {
    uint32_t wid = sp->width;
    uint32_t awid = sp->dims.a;
    uint32_t b0 = sp->dims.b - wid;
    odin3_net_id *lo = NULL;
    odin3_net_id *hi = NULL;
    odin3_net_id *sum = NULL;
    odin3_status st = take(sp, (take_req){awid + b0, awid + b0}, &lo);
    st = st == ODIN3_OK ? take(sp, (take_req){awid + wid, awid + wid}, &hi) : st;
    st = st == ODIN3_OK ? take(sp, (take_req){awid + wid, awid + wid}, &sum) : st;
    st = st == ODIN3_OK ? new_mul(sp, "m00", (port_in){sp->an, awid}, (port_in){sp->bn, b0}, lo)
                        : st;
    st = st == ODIN3_OK
             ? new_mul(sp, "m01", (port_in){sp->an, awid}, (port_in){sp->bn + b0, wid}, hi)
             : st;
    port_in low = {NULL, 0};
    st = st == ODIN3_OK ? low_port(sp, lo + b0, &low) : st;
    st = st == ODIN3_OK ? new_add(sp, "add0", low, (port_in){hi, awid + wid}, sum) : st;
    if (st == ODIN3_OK) {
        tie_y(sp, 0, lo, b0);
        tie_y(sp, b0, sum, awid + wid);
    }
    return st;
}

/* The products of the two-sided split, then its adders; see split_ab. */
typedef struct both {
    odin3_net_id *m00; /* 2W then a1 + b1: the total's high word follows m00[W, 2W) */
    odin3_net_id *m11;
    odin3_net_id *m01; /* W + b1, plus one slot for the carry bit of add0 (gnd) */
    odin3_net_id *m10; /* a1 + W, plus one slot (gnd) */
    odin3_net_id *sum0;
    odin3_net_id *sum1;
} both;

static odin3_status both_products(split *sp, both *bt) {
    uint32_t wid = sp->width;
    uint32_t a1 = sp->dims.a - wid;
    uint32_t b1 = sp->dims.b - wid;
    odin3_status st =
        take(sp, (take_req){2U * wid, 2U * wid}, &bt->m00); /* followed by m11: one array */
    st = st == ODIN3_OK ? take(sp, (take_req){a1 + b1, a1 + b1}, &bt->m11) : st;
    st = st == ODIN3_OK ? take(sp, (take_req){wid + b1 + 1U, wid + b1}, &bt->m01) : st;
    st = st == ODIN3_OK ? take(sp, (take_req){a1 + wid + 1U, a1 + wid}, &bt->m10) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    bt->m01[wid + b1] = sp->gnd;
    bt->m10[a1 + wid] = sp->gnd;
    st = new_mul(sp, "m00", (port_in){sp->an, wid}, (port_in){sp->bn, wid}, bt->m00);
    st = st == ODIN3_OK
             ? new_mul(sp, "m11", (port_in){sp->an + wid, a1}, (port_in){sp->bn + wid, b1}, bt->m11)
             : st;
    st = st == ODIN3_OK
             ? new_mul(sp, "m01", (port_in){sp->an, wid}, (port_in){sp->bn + wid, b1}, bt->m01)
             : st;
    return st == ODIN3_OK
               ? new_mul(sp, "m10", (port_in){sp->an + wid, a1}, (port_in){sp->bn, wid}, bt->m10)
               : st;
}

/* Both above W (a0 = b0 = W): add0 = m10 + m01 (one carry bit each), add1 = add0 plus
 * {m11, m00[W, 2W)}; Y = m00[0, W) then add1. Parmys's `a0 == b0` branch of split_multiplier. */
static odin3_status split_ab(split *sp) {
    uint32_t wid = sp->width;
    uint32_t a1 = sp->dims.a - wid;
    uint32_t b1 = sp->dims.b - wid;
    uint32_t size0 = (a1 > b1 ? a1 : b1) + wid + 1U;
    uint32_t size1 = wid + a1 + b1;
    both bt;
    odin3_status st = both_products(sp, &bt);
    st = st == ODIN3_OK ? take(sp, (take_req){size0, size0}, &bt.sum0) : st;
    st = st == ODIN3_OK ? take(sp, (take_req){size1, size1}, &bt.sum1) : st;
    st = st == ODIN3_OK ? new_add(sp, "add0", (port_in){bt.m10, a1 + wid + 1U},
                                  (port_in){bt.m01, wid + b1 + 1U}, bt.sum0)
                        : st;
    st = st == ODIN3_OK ? new_add(sp, "add1", (port_in){bt.sum0, size0},
                                  (port_in){bt.m00 + wid, size1}, bt.sum1)
                        : st;
    if (st == ODIN3_OK) {
        tie_y(sp, 0, bt.m00, wid);
        tie_y(sp, wid, bt.sum1, size1);
    }
    return st;
}

/* An upper bound of the scratch: the operand copies and the fresh nets of the pieces. The
 * both-sided case uses 6W + 3(a1 + b1) + max(a1, b1) + 3 entries at most, the one-sided cases less;
 * each is below 4 (a + b) + 4, as W is below a and b. */
enum { SCRATCH_PER_BIT = 5, SCRATCH_SLACK = 8 };
static size_t scratch_size(const split *sp) {
    return SCRATCH_PER_BIT * ((size_t)sp->dims.a + sp->dims.b) + SCRATCH_SLACK;
}

static odin3_status build(split *sp) {
    odin3_status st = odin3_vec_reserve(&sp->ties, sp->dims.y);
    st = st == ODIN3_OK ? scratch(&sp->nets, scratch_size(sp)) : st;
    st = st == ODIN3_OK ? hm_begin_cell(sp->hc, sp->src) : st;
    st = st == ODIN3_OK ? hm_const(sp->hc, HM_GND, &sp->gnd) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    sp->an = odin3_vec_at(&sp->nets, 0);
    sp->bn = sp->an + sp->dims.a;
    sp->cursor = sp->bn + sp->dims.b;
    gather(sp, (gather_req){PORT_A, sp->dims.a}, sp->an);
    gather(sp, (gather_req){PORT_B, sp->dims.b}, sp->bn);
    if (sp->dims.a > sp->width && sp->dims.b > sp->width) {
        return split_ab(sp);
    }
    return sp->dims.a > sp->width ? split_a(sp) : split_b(sp);
}

odin3_status hm_split_mul(hm_ctx *hc, odin3_node_id src, hm_mul_dims dims) {
    split sp;
    memset(&sp, 0, sizeof sp);
    sp.hc = hc;
    sp.src = src;
    sp.width = hc->rules.mul_width;
    sp.dims = dims;
    odin3_vec_init(&sp.nets, sizeof(odin3_net_id));
    odin3_vec_init(&sp.ties, sizeof(hm_tie));
    odin3_status st =
        hm_find_type(hc->design, "$mul", &sp.mul) && hm_find_type(hc->design, "$add", &sp.add)
            ? ODIN3_OK
            : ODIN3_ERR_INVALID_ARG;
    st = st == ODIN3_OK ? build(&sp) : st;
    if (st != ODIN3_OK) {
        hm_abort(hc);
    } else {
        st = hm_commit(hc, odin3_vec_cat(&sp.ties, 0), sp.ties.len);
        hc->b_splits += st == ODIN3_OK && dims.a <= sp.width && dims.b > sp.width ? 1U : 0U;
    }
    odin3_vec_free(&sp.ties);
    odin3_vec_free(&sp.nets);
    return st;
}
