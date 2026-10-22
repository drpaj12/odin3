/*
 * mul_group.c — regroup of the multiplier pieces partial_map's split made (M-4 pattern P1, M-5,
 * M-6; pattern P2 follows in the same file). Every merge is one um_replace.
 */
#include "passes/unmap/mul_group.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    UM_BIN_PARAMS = 5,
    UM_PIECE_A = 0,
    UM_PIECE_B = 1,
    UM_PIECE_Y = 2,
    UM_PARAM_Y = 4,
    UM_P2_CELLS = 6,
    UM_P2_MULS = 3,
    UM_P2_ADDS = 2
};

static bool is_root(const um_ctx *uc, odin3_node_id node);

typedef enum um_match { UM_NO_MATCH, UM_SHARED, UM_MATCH } um_match;

/* One port of one node. */
typedef struct um_ref {
    odin3_node_id node;
    uint32_t port;
} um_ref;

static uint32_t ref_len(const um_ctx *uc, um_ref ref) {
    return odin3_node_port(uc->module, ref.node, ref.port).count;
}

static odin3_net_id ref_net(const um_ctx *uc, um_ref ref, uint32_t index) {
    return um_port_net(uc->module, ref.node, (um_pos){ref.port, index});
}

/* The two ports hold the same nets in the same order. */
static bool same_vec(const um_ctx *uc, um_ref left, um_ref right) {
    uint32_t len = ref_len(uc, left);
    if (len != ref_len(uc, right)) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (ref_net(uc, left, i).v != ref_net(uc, right, i).v) {
            return false;
        }
    }
    return true;
}

static bool param_zero(const um_ctx *uc, odin3_node_id node, uint32_t index) {
    const odin3_value *param = odin3_node_param(uc->module, node, index);
    return param != NULL && param->kind == ODIN3_VAL_INT && param->i == 0;
}

static bool is_unsigned(const um_ctx *uc, odin3_node_id node) {
    return param_zero(uc, node, 0) && param_zero(uc, node, 1);
}

/* A live unsigned `$mul` that carries `unmap_w`; *width receives it. */
static bool load_mul(const um_ctx *uc, odin3_node_id node, int64_t *width) {
    return odin3_node_valid(node) && odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.mul.v && is_unsigned(uc, node) &&
           um_attr_int(uc, node, "unmap_w", width);
}

/* The `$mul` piece that drives `net`, none when it is anything else. */
static odin3_node_id piece_driver(const um_ctx *uc, odin3_net_id net, int64_t *width) {
    odin3_pin_id driver = odin3_net_driver(uc->module, net);
    if (!odin3_pin_valid(driver)) {
        return (odin3_node_id){0};
    }
    odin3_node_id node = odin3_pin_node(uc->module, driver);
    return load_mul(uc, node, width) ? node : (odin3_node_id){0};
}

/* --- pattern P1 (M-4) ---------------------------------------------------------------------- */

typedef struct um_p1 {
    um_ctx *uc;
    odin3_node_id root;
    odin3_node_id lo;
    odin3_node_id hi;
    uint32_t k;      /* Y_WIDTH of the root */
    uint32_t op_lo;  /* the root's operand port that LO feeds */
    uint32_t op_hi;  /* and the one HI feeds */
    uint32_t shared; /* the piece port both read */
    uint32_t split;  /* the piece port that holds the cut operand */
    uint32_t cut_lo; /* s: bits of the split port of LO */
    uint32_t cut_hi; /* h: and of HI */
    uint32_t keep;   /* q: bits of the shared port */
    int64_t w;
} um_p1;

static um_ref lo_ref(const um_p1 *p1, uint32_t port) {
    return (um_ref){p1->lo, port};
}

static um_ref hi_ref(const um_p1 *p1, uint32_t port) {
    return (um_ref){p1->hi, port};
}

/* M-4 step 4: every net of the Y port of `src` from `first` on has exactly one sink, the port
 * `dst`. */
static bool only_to(const um_ctx *uc, um_ref src, uint32_t first, um_ref dst) {
    uint32_t len = ref_len(uc, src);
    for (uint32_t i = first; i < len; i++) {
        odin3_pinlist sinks = odin3_net_sinks(uc->module, ref_net(uc, src, i));
        if (sinks.count != 1 || odin3_pin_node(uc->module, sinks.pins[0]).v != dst.node.v ||
            odin3_pin_port(uc->module, sinks.pins[0]) != dst.port) {
            return false;
        }
    }
    return true;
}

/* M-4 P1 steps 3 and 5: the root's operands are the product bits of the pieces. */
static bool operands_match(const um_p1 *p1) {
    const um_ctx *uc = p1->uc;
    uint32_t la = ref_len(uc, (um_ref){p1->root, p1->op_lo});
    uint32_t lb = ref_len(uc, (um_ref){p1->root, p1->op_hi});
    uint32_t want_a = p1->k < p1->keep ? p1->k : p1->keep;
    uint32_t want_b = p1->k < p1->cut_hi + p1->keep ? p1->k : p1->cut_hi + p1->keep;
    if (la != want_a || lb != want_b || ref_len(uc, lo_ref(p1, UM_PIECE_Y)) != p1->cut_lo + la ||
        ref_len(uc, hi_ref(p1, UM_PIECE_Y)) != lb) {
        return false;
    }
    for (uint32_t i = 0; i < la; i++) {
        if (ref_net(uc, (um_ref){p1->root, p1->op_lo}, i).v !=
            ref_net(uc, lo_ref(p1, UM_PIECE_Y), p1->cut_lo + i).v) {
            return false;
        }
    }
    for (uint32_t i = 0; i < lb; i++) {
        if (ref_net(uc, (um_ref){p1->root, p1->op_hi}, i).v !=
            ref_net(uc, hi_ref(p1, UM_PIECE_Y), i).v) {
            return false;
        }
    }
    return true;
}

/* The shared and split ports chosen: same shared nets, a width that is the arch width. */
static um_match check_ports(um_p1 *p1) {
    const um_ctx *uc = p1->uc;
    if (!same_vec(uc, lo_ref(p1, p1->shared), hi_ref(p1, p1->shared))) {
        return UM_NO_MATCH;
    }
    p1->cut_lo = ref_len(uc, lo_ref(p1, p1->split));
    p1->cut_hi = ref_len(uc, hi_ref(p1, p1->split));
    p1->keep = ref_len(uc, lo_ref(p1, p1->shared));
    if (p1->w != p1->cut_lo && p1->w != p1->cut_hi) {
        return UM_NO_MATCH;
    }
    if (!operands_match(p1)) {
        return UM_NO_MATCH;
    }
    bool sinks_ok =
        only_to(uc, lo_ref(p1, UM_PIECE_Y), p1->cut_lo, (um_ref){p1->root, p1->op_lo}) &&
        only_to(uc, hi_ref(p1, UM_PIECE_Y), 0, (um_ref){p1->root, p1->op_hi});
    return sinks_ok ? UM_MATCH : UM_SHARED;
}

static odin3_status push_ref(odin3_vec *vec, const um_ctx *uc, um_ref ref, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        odin3_net_id *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = ref_net(uc, ref, i);
    }
    return ODIN3_OK;
}

typedef struct um_nets {
    odin3_vec cut;
    odin3_vec keep;
    odin3_vec out;
} um_nets;

static void nets_free(um_nets *nets) {
    odin3_vec_free(&nets->cut);
    odin3_vec_free(&nets->keep);
    odin3_vec_free(&nets->out);
}

/* The nets of the merged cell: split operand LO ++ HI, shared operand, Y(LO)[0..s) ++ Y(root). */
static odin3_status collect_nets(const um_p1 *p1, um_nets *nets) {
    const um_ctx *uc = p1->uc;
    odin3_vec_init(&nets->cut, sizeof(odin3_net_id));
    odin3_vec_init(&nets->keep, sizeof(odin3_net_id));
    odin3_vec_init(&nets->out, sizeof(odin3_net_id));
    odin3_status st = push_ref(&nets->cut, uc, lo_ref(p1, p1->split), p1->cut_lo);
    st = st == ODIN3_OK ? push_ref(&nets->cut, uc, hi_ref(p1, p1->split), p1->cut_hi) : st;
    st = st == ODIN3_OK ? push_ref(&nets->keep, uc, lo_ref(p1, p1->shared), p1->keep) : st;
    st = st == ODIN3_OK ? push_ref(&nets->out, uc, lo_ref(p1, UM_PIECE_Y), p1->cut_lo) : st;
    return st == ODIN3_OK ? push_ref(&nets->out, uc, (um_ref){p1->root, UM_PIECE_Y}, p1->k) : st;
}

static odin3_status emit_p1(um_p1 *p1, const um_nets *nets) {
    bool cut_is_a = p1->split == UM_PIECE_A;
    const odin3_vec *vec_a = cut_is_a ? &nets->cut : &nets->keep;
    const odin3_vec *vec_b = cut_is_a ? &nets->keep : &nets->cut;
    odin3_value params[UM_BIN_PARAMS] = {
        odin3_value_int(0), odin3_value_int(0), odin3_value_int((int64_t)vec_a->len),
        odin3_value_int((int64_t)vec_b->len), odin3_value_int((int64_t)nets->out.len)};
    odin3_netvec ports[3] = {{vec_a->data, (uint32_t)vec_a->len},
                             {vec_b->data, (uint32_t)vec_b->len},
                             {nets->out.data, (uint32_t)nets->out.len}};
    um_group group;
    um_group_init(&group);
    odin3_status st = um_group_add(&group, p1->lo);
    st = st == ODIN3_OK ? um_group_add(&group, p1->hi) : st;
    st = st == ODIN3_OK ? um_group_add(&group, p1->root) : st;
    um_new req = {p1->uc->types.mul, params, UM_BIN_PARAMS, ports, p1->lo, "mul.p1", 0, p1->w};
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? um_replace(p1->uc, &group, &req, &made) : st;
    um_group_free(&group);
    return st;
}

static odin3_status merge_p1(um_p1 *p1) {
    um_nets nets;
    odin3_status st = collect_nets(p1, &nets);
    st = st == ODIN3_OK ? emit_p1(p1, &nets) : st;
    nets_free(&nets);
    if (st == ODIN3_OK) {
        um_stats *stats = &p1->uc->stats;
        stats->regrouped_mul++;
        stats->mul -= stats->mul > 0 ? 1 : 0;
        stats->add -= stats->add > 0 ? 1 : 0;
    }
    return st;
}

/* One choice of which root operand LO feeds; tries B shared, then A shared. */
static um_match p1_choice(um_p1 *p1, uint32_t op_lo) {
    const um_ctx *uc = p1->uc;
    p1->op_lo = op_lo;
    p1->op_hi = 1 - op_lo;
    int64_t w_lo = 0;
    int64_t w_hi = 0;
    p1->lo = piece_driver(uc, ref_net(uc, (um_ref){p1->root, p1->op_lo}, 0), &w_lo);
    p1->hi = piece_driver(uc, ref_net(uc, (um_ref){p1->root, p1->op_hi}, 0), &w_hi);
    if (p1->lo.v == 0 || p1->hi.v == 0 || p1->lo.v == p1->hi.v || w_lo != w_hi) {
        return UM_NO_MATCH;
    }
    p1->w = w_lo;
    um_match best = UM_NO_MATCH;
    for (uint32_t shared = UM_PIECE_B + 1; shared > 0 && best != UM_MATCH; shared--) {
        p1->shared = shared - 1;
        p1->split = 1 - p1->shared;
        um_match got = check_ports(p1);
        best = got > best ? got : best;
    }
    return best;
}

/* P1 on one root: *match says what was found; a match is merged. */
static odin3_status try_p1(um_ctx *uc, odin3_node_id root, um_match *match) {
    um_p1 p1;
    memset(&p1, 0, sizeof p1);
    p1.uc = uc;
    p1.root = root;
    *match = UM_NO_MATCH;
    const odin3_value *yw = odin3_node_param(uc->module, root, UM_PARAM_Y);
    if (yw == NULL || ref_len(uc, (um_ref){root, UM_PIECE_A}) == 0 ||
        ref_len(uc, (um_ref){root, UM_PIECE_B}) == 0) {
        return ODIN3_OK;
    }
    p1.k = (uint32_t)yw->i;
    for (uint32_t op = 0; op < 2 && *match != UM_MATCH; op++) {
        um_match got = p1_choice(&p1, op);
        *match = got > *match ? got : *match;
    }
    return *match == UM_MATCH ? merge_p1(&p1) : ODIN3_OK;
}

/* --- pattern P2 (M-4) ---------------------------------------------------------------------- */

/* A run of nets: `first` onward in the port. */
typedef struct um_span {
    um_ref ref;
    uint32_t first;
} um_span;

static bool same_run(const um_ctx *uc, um_span left, um_span right, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (ref_net(uc, left.ref, left.first + i).v != ref_net(uc, right.ref, right.first + i).v) {
            return false;
        }
    }
    return true;
}

typedef struct um_p2 {
    um_ctx *uc;
    odin3_node_id r1;      /* the total adder */
    odin3_node_id r0;      /* the adder of the two cross products */
    odin3_node_id m00;     /* low x low */
    odin3_node_id m11;     /* high x high */
    odin3_node_id cross_a; /* the cross product that feeds R0.A */
    odin3_node_id cross_b; /* and R0.B */
    uint32_t k1;
    uint32_t k0;
    uint32_t cell_w; /* W as a count of bits */
    int64_t w;
} um_p2;

static um_ref p2_ref(odin3_node_id node, uint32_t port) {
    return (um_ref){node, port};
}

/* A live unsigned phase A `$add`. */
static bool is_chain_add(const um_ctx *uc, odin3_node_id node) {
    return odin3_node_valid(node) && is_root(uc, node);
}

/* Step 1 and the choice of the five other cells from the nets of the root's operands. */
static bool p2_load(um_p2 *p2) {
    const um_ctx *uc = p2->uc;
    um_ref r1b = p2_ref(p2->r1, UM_PIECE_B);
    uint32_t len_b = ref_len(uc, r1b);
    int64_t w_a = 0;
    int64_t w_b = 0;
    p2->m00 = piece_driver(uc, ref_net(uc, r1b, 0), &p2->w);
    p2->cell_w = (uint32_t)p2->w;
    if (p2->m00.v == 0 || p2->w < 1 || len_b <= p2->cell_w) {
        return false;
    }
    p2->m11 = piece_driver(uc, ref_net(uc, r1b, p2->cell_w), &w_a);
    odin3_pin_id driver = odin3_net_driver(uc->module, ref_net(uc, p2_ref(p2->r1, UM_PIECE_A), 0));
    p2->r0 = odin3_pin_valid(driver) ? odin3_pin_node(uc->module, driver) : (odin3_node_id){0};
    if (p2->m11.v == 0 || w_a != p2->w || !is_chain_add(uc, p2->r0)) {
        return false;
    }
    const odin3_value *yw0 = odin3_node_param(uc->module, p2->r0, UM_PARAM_Y);
    p2->k0 = yw0 != NULL && yw0->i > 0 ? (uint32_t)yw0->i : 0;
    p2->cross_a = piece_driver(uc, ref_net(uc, p2_ref(p2->r0, UM_PIECE_A), 0), &w_a);
    p2->cross_b = piece_driver(uc, ref_net(uc, p2_ref(p2->r0, UM_PIECE_B), 0), &w_b);
    return p2->cross_a.v != 0 && p2->cross_b.v != 0 && w_a == p2->w && w_b == p2->w;
}

/* Step 1 of M-4 P2: the low product is W x W, the high product has at least a bit per side. */
static bool p2_shapes(const um_p2 *p2) {
    const um_ctx *uc = p2->uc;
    return ref_len(uc, p2_ref(p2->m00, UM_PIECE_A)) == p2->cell_w &&
           ref_len(uc, p2_ref(p2->m00, UM_PIECE_B)) == p2->cell_w &&
           ref_len(uc, p2_ref(p2->m00, UM_PIECE_Y)) == 2 * p2->cell_w &&
           ref_len(uc, p2_ref(p2->m11, UM_PIECE_A)) >= 1 &&
           ref_len(uc, p2_ref(p2->m11, UM_PIECE_B)) >= 1 && p2->cross_a.v != p2->cross_b.v &&
           p2->cross_a.v != p2->m00.v && p2->cross_a.v != p2->m11.v && p2->cross_b.v != p2->m00.v &&
           p2->cross_b.v != p2->m11.v && p2->m00.v != p2->m11.v;
}

/* True when `cell` multiplies the given A and B operand ports. */
static bool crosses(const um_ctx *uc, odin3_node_id cell, um_ref want_a, um_ref want_b) {
    return same_vec(uc, p2_ref(cell, UM_PIECE_A), want_a) &&
           same_vec(uc, p2_ref(cell, UM_PIECE_B), want_b);
}

/* Step 2: the cross products take (M00.A, M11.B) and (M11.A, M00.B), in either order. */
static bool p2_cross(const um_p2 *p2) {
    const um_ctx *uc = p2->uc;
    um_ref low_a = p2_ref(p2->m00, UM_PIECE_A);
    um_ref low_b = p2_ref(p2->m00, UM_PIECE_B);
    um_ref high_a = p2_ref(p2->m11, UM_PIECE_A);
    um_ref high_b = p2_ref(p2->m11, UM_PIECE_B);
    return (crosses(uc, p2->cross_a, low_a, high_b) && crosses(uc, p2->cross_b, high_a, low_b)) ||
           (crosses(uc, p2->cross_a, high_a, low_b) && crosses(uc, p2->cross_b, low_a, high_b));
}

/* The operand `port` of `adder` is the first `want` bits of the Y of `cell`, and the cell shows
 * nothing more. */
static bool feeds_prefix(const um_ctx *uc, um_ref operand, um_ref product, uint32_t k_width) {
    uint32_t len_y = ref_len(uc, product);
    uint32_t want = k_width < len_y ? k_width : len_y;
    return ref_len(uc, operand) == want && len_y == want &&
           same_run(uc, (um_span){operand, 0}, (um_span){product, 0}, want);
}

/* Step 3. */
static bool p2_operands(const um_p2 *p2) {
    const um_ctx *uc = p2->uc;
    uint32_t len_b = ref_len(uc, p2_ref(p2->r1, UM_PIECE_B));
    uint32_t a1 = ref_len(uc, p2_ref(p2->m11, UM_PIECE_A));
    uint32_t b1 = ref_len(uc, p2_ref(p2->m11, UM_PIECE_B));
    uint32_t total = p2->cell_w + a1 + b1;
    um_ref r1b = p2_ref(p2->r1, UM_PIECE_B);
    um_ref high_y = p2_ref(p2->m11, UM_PIECE_Y);
    return len_b == (p2->k1 < total ? p2->k1 : total) &&
           same_run(uc, (um_span){r1b, 0}, (um_span){p2_ref(p2->m00, UM_PIECE_Y), p2->cell_w},
                    p2->cell_w) &&
           same_run(uc, (um_span){r1b, p2->cell_w}, (um_span){high_y, 0}, len_b - p2->cell_w) &&
           ref_len(uc, high_y) == len_b - p2->cell_w &&
           ref_len(uc, p2_ref(p2->r1, UM_PIECE_A)) == ref_len(uc, p2_ref(p2->r0, UM_PIECE_Y)) &&
           same_run(uc, (um_span){p2_ref(p2->r1, UM_PIECE_A), 0},
                    (um_span){p2_ref(p2->r0, UM_PIECE_Y), 0},
                    ref_len(uc, p2_ref(p2->r0, UM_PIECE_Y))) &&
           feeds_prefix(uc, p2_ref(p2->r0, UM_PIECE_A), p2_ref(p2->cross_a, UM_PIECE_Y), p2->k0) &&
           feeds_prefix(uc, p2_ref(p2->r0, UM_PIECE_B), p2_ref(p2->cross_b, UM_PIECE_Y), p2->k0);
}

/* Step 4: the intermediate nets have no other reader. */
static bool p2_sinks(const um_p2 *p2) {
    const um_ctx *uc = p2->uc;
    um_ref r1b = p2_ref(p2->r1, UM_PIECE_B);
    return only_to(uc, p2_ref(p2->m00, UM_PIECE_Y), p2->cell_w, r1b) &&
           only_to(uc, p2_ref(p2->m11, UM_PIECE_Y), 0, r1b) &&
           only_to(uc, p2_ref(p2->cross_a, UM_PIECE_Y), 0, p2_ref(p2->r0, UM_PIECE_A)) &&
           only_to(uc, p2_ref(p2->cross_b, UM_PIECE_Y), 0, p2_ref(p2->r0, UM_PIECE_B)) &&
           only_to(uc, p2_ref(p2->r0, UM_PIECE_Y), 0, p2_ref(p2->r1, UM_PIECE_A));
}

/* The nets of the merged cell: A = M00.A ++ M11.A, B = M00.B ++ M11.B, Y = Y(M00)[0..W) ++ Y(R1).
 */
static odin3_status p2_collect(const um_p2 *p2, um_nets *nets) {
    const um_ctx *uc = p2->uc;
    odin3_vec_init(&nets->cut, sizeof(odin3_net_id));
    odin3_vec_init(&nets->keep, sizeof(odin3_net_id));
    odin3_vec_init(&nets->out, sizeof(odin3_net_id));
    um_ref low_a = p2_ref(p2->m00, UM_PIECE_A);
    um_ref high_a = p2_ref(p2->m11, UM_PIECE_A);
    um_ref low_b = p2_ref(p2->m00, UM_PIECE_B);
    um_ref high_b = p2_ref(p2->m11, UM_PIECE_B);
    odin3_status st = push_ref(&nets->cut, uc, low_a, ref_len(uc, low_a));
    st = st == ODIN3_OK ? push_ref(&nets->cut, uc, high_a, ref_len(uc, high_a)) : st;
    st = st == ODIN3_OK ? push_ref(&nets->keep, uc, low_b, ref_len(uc, low_b)) : st;
    st = st == ODIN3_OK ? push_ref(&nets->keep, uc, high_b, ref_len(uc, high_b)) : st;
    st = st == ODIN3_OK ? push_ref(&nets->out, uc, p2_ref(p2->m00, UM_PIECE_Y), p2->cell_w) : st;
    return st == ODIN3_OK ? push_ref(&nets->out, uc, p2_ref(p2->r1, UM_PIECE_Y), p2->k1) : st;
}

static odin3_status p2_emit(um_p2 *p2, const um_nets *nets) {
    odin3_value params[UM_BIN_PARAMS] = {
        odin3_value_int(0), odin3_value_int(0), odin3_value_int((int64_t)nets->cut.len),
        odin3_value_int((int64_t)nets->keep.len), odin3_value_int((int64_t)nets->out.len)};
    odin3_netvec ports[3] = {{nets->cut.data, (uint32_t)nets->cut.len},
                             {nets->keep.data, (uint32_t)nets->keep.len},
                             {nets->out.data, (uint32_t)nets->out.len}};
    odin3_node_id cells[UM_P2_CELLS] = {p2->m00, p2->m11, p2->cross_a, p2->cross_b, p2->r0, p2->r1};
    um_group group;
    um_group_init(&group);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < UM_P2_CELLS; i++) {
        st = um_group_add(&group, cells[i]);
    }
    um_new req = {p2->uc->types.mul, params, UM_BIN_PARAMS, ports, p2->m00, "mul.p2", 0, p2->w};
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? um_replace(p2->uc, &group, &req, &made) : st;
    um_group_free(&group);
    return st;
}

static odin3_status merge_p2(um_p2 *p2) {
    um_nets nets;
    odin3_status st = p2_collect(p2, &nets);
    st = st == ODIN3_OK ? p2_emit(p2, &nets) : st;
    nets_free(&nets);
    if (st == ODIN3_OK) {
        um_stats *stats = &p2->uc->stats;
        stats->regrouped_mul += UM_P2_MULS;
        stats->mul -= stats->mul >= UM_P2_MULS ? UM_P2_MULS : stats->mul;
        stats->add -= stats->add >= UM_P2_ADDS ? UM_P2_ADDS : stats->add;
    }
    return st;
}

/* P2 on one root: the match, or a match that only the sink test refuses. */
static um_match p2_match(um_p2 *p2) {
    if (!p2_load(p2) || !p2_shapes(p2) || !p2_cross(p2) || !p2_operands(p2)) {
        return UM_NO_MATCH;
    }
    return p2_sinks(p2) ? UM_MATCH : UM_SHARED;
}

static odin3_status try_p2(um_ctx *uc, odin3_node_id root, um_match *match) {
    um_p2 p2;
    memset(&p2, 0, sizeof p2);
    p2.uc = uc;
    p2.r1 = root;
    *match = UM_NO_MATCH;
    const odin3_value *yw = odin3_node_param(uc->module, root, UM_PARAM_Y);
    if (yw == NULL || ref_len(uc, p2_ref(root, UM_PIECE_A)) == 0 ||
        ref_len(uc, p2_ref(root, UM_PIECE_B)) == 0) {
        return ODIN3_OK;
    }
    p2.k1 = (uint32_t)yw->i;
    *match = p2_match(&p2);
    return *match == UM_MATCH ? merge_p2(&p2) : ODIN3_OK;
}

/* --- the scan (M-5) ------------------------------------------------------------------------ */

/* A candidate root: an unsigned `$add` phase A made from a chain. */
static bool is_root(const um_ctx *uc, odin3_node_id node) {
    const char *rule = um_attr_text(uc, node, "unmap");
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.add.v && is_unsigned(uc, node) &&
           rule != NULL && strcmp(rule, "chain") == 0;
}

/* One root: P2, then P1; a refusal on the sink test alone notes the root (M-6) once. */
static odin3_status try_root(um_ctx *uc, odin3_node_id root, bool *merged) {
    um_match second = UM_NO_MATCH;
    um_match first = UM_NO_MATCH;
    odin3_status st = try_p2(uc, root, &second);
    if (st == ODIN3_OK && second != UM_MATCH) {
        st = try_p1(uc, root, &first);
    }
    *merged = second == UM_MATCH || first == UM_MATCH;
    bool shared = second == UM_SHARED || first == UM_SHARED;
    if (st == ODIN3_OK && !*merged && shared && um_attr_text(uc, root, "unmap_note") == NULL) {
        st = um_decline(uc, &root, 1, (um_why){"mul", "shared"});
    }
    return st;
}

static odin3_status scan_once(um_ctx *uc, bool *changed) {
    uint32_t end = odin3_module_node_end(uc->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        odin3_node_id node = {nid};
        bool merged = false;
        if (is_root(uc, node)) {
            st = try_root(uc, node, &merged);
        }
        *changed = *changed || merged;
    }
    return st;
}

odin3_status um_mul_group_phase(um_ctx *uc) {
    uint32_t limit = odin3_module_node_end(uc->module) + 1;
    for (uint32_t round = 0; round < limit; round++) {
        bool changed = false;
        odin3_status st = scan_once(uc, &changed);
        if (st != ODIN3_OK || !changed) {
            return st;
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "unmap: multiplier regroup did not converge");
    return ODIN3_ERR_INVALID_ARG;
}
