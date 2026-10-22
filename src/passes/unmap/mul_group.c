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

enum { UM_BIN_PARAMS = 5, UM_PIECE_A = 0, UM_PIECE_B = 1, UM_PIECE_Y = 2, UM_PARAM_Y = 4 };

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

/* M-4 P1 step 4: every net of `piece` Y from `first` on has one sink, the given root port. */
static bool only_to_root(const um_p1 *p1, odin3_node_id piece, uint32_t first, uint32_t port) {
    uint32_t len = ref_len(p1->uc, (um_ref){piece, UM_PIECE_Y});
    for (uint32_t i = first; i < len; i++) {
        odin3_net_id net = ref_net(p1->uc, (um_ref){piece, UM_PIECE_Y}, i);
        odin3_pinlist sinks = odin3_net_sinks(p1->uc->module, net);
        if (sinks.count != 1 || odin3_pin_node(p1->uc->module, sinks.pins[0]).v != p1->root.v ||
            odin3_pin_port(p1->uc->module, sinks.pins[0]) != port) {
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
        only_to_root(p1, p1->lo, p1->cut_lo, p1->op_lo) && only_to_root(p1, p1->hi, 0, p1->op_hi);
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

/* P1 on one root: *merged says a merge was made; a refusal on sinks only notes the root. */
static odin3_status try_p1(um_ctx *uc, odin3_node_id root, bool *merged) {
    um_p1 p1;
    memset(&p1, 0, sizeof p1);
    p1.uc = uc;
    p1.root = root;
    const odin3_value *yw = odin3_node_param(uc->module, root, UM_PARAM_Y);
    if (yw == NULL || ref_len(uc, (um_ref){root, UM_PIECE_A}) == 0 ||
        ref_len(uc, (um_ref){root, UM_PIECE_B}) == 0) {
        return ODIN3_OK;
    }
    p1.k = (uint32_t)yw->i;
    um_match best = UM_NO_MATCH;
    odin3_status st = ODIN3_OK;
    for (uint32_t op = 0; op < 2 && best != UM_MATCH; op++) {
        um_match got = p1_choice(&p1, op);
        if (got == UM_MATCH) {
            best = got;
            st = merge_p1(&p1);
            *merged = st == ODIN3_OK;
        } else {
            best = got > best ? got : best;
        }
    }
    if (st == ODIN3_OK && best == UM_SHARED && um_attr_text(uc, root, "unmap_note") == NULL) {
        st = um_decline(uc, &root, 1, (um_why){"mul", "shared"});
    }
    return st;
}

/* --- the scan (M-5) ------------------------------------------------------------------------ */

/* A candidate root: an unsigned `$add` phase A made from a chain. */
static bool is_root(const um_ctx *uc, odin3_node_id node) {
    const char *rule = um_attr_text(uc, node, "unmap");
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.add.v && is_unsigned(uc, node) &&
           rule != NULL && strcmp(rule, "chain") == 0;
}

static odin3_status scan_once(um_ctx *uc, bool *changed) {
    uint32_t end = odin3_module_node_end(uc->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        odin3_node_id node = {nid};
        bool merged = false;
        if (is_root(uc, node)) {
            st = try_p1(uc, node, &merged);
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
