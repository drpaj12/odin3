/*
 * wreduce_op.c — wreduce's pre-step (3G spec 3, step 1: one-bit results, $div/$mod/$pow operand
 * trimming, $mem address bits) and the operator rules of Yosys's run_cell (spec 4.3).
 *
 * Signedness (WR-6, PHASE2 #25). A binary cell of the ruled types is signed only when both flags
 * are set; wherever Yosys reads one port's flag of such a cell, this file reads that. $shl, $shr
 * and $sshr read A_SIGNED for A and treat B as unsigned; $shiftx and $pow read B_SIGNED as the
 * amount's own flag; $not and $neg read A_SIGNED.
 *
 * The rule order is Yosys's, in one pass (wr_plan_op): the port limits, the free choice of
 * signedness, the trims of A and B, signed to unsigned, the trim and the cap of Y. The plan reads
 * the IR and changes nothing; wr_run_op applies it and plans again until it changes nothing, the
 * loop that replaces Yosys's recursive run_cell call.
 */
#include "passes/opt/wreduce.h"

#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    PORT_A = 0,
    PORT_B = 1,
    PRM_A_SIGNED = 0,
    PRM_B_SIGNED = 1,
    PRM_A_WIDTH = 2,
    PRM_B_WIDTH = 3,
    PRM_UN_A_WIDTH = 1,
    N_BIN_PORTS = 3,
    N_UN_PORTS = 2,
    MEM_PRM_SIZE = 1,
    MEM_PRM_OFFSET = 2,
    MEM_PRM_ABITS = 3,
    MEM_PRM_RD_PORTS = 5,
    MEM_PRM_WR_PORTS = 6,
    MEM_PORT_RD_ADDR = 2,
    MEM_PORT_WR_ADDR = 6,
    MEM_N_PORTS = 8
};

/* A port that is empty after a narrowing still needs a non-NULL pointer (NULL = "the old nets"). */
static const odin3_net_id k_no_nets[1] = {{0}};

/* --- type classes ---------------------------------------------------------------------------- */

static bool is_unary(wr_type type) {
    return type == WT_NOT || type == WT_NEG;
}

/* The 15 types of PHASE2 #25: signed only when both flags are set. */
#define WT_BIT(type) ((uint64_t)1 << (unsigned)(type))
static const uint64_t k_ruled_mask =
    WT_BIT(WT_AND) | WT_BIT(WT_OR) | WT_BIT(WT_XOR) | WT_BIT(WT_XNOR) | WT_BIT(WT_LT) |
    WT_BIT(WT_LE) | WT_BIT(WT_EQ) | WT_BIT(WT_NE) | WT_BIT(WT_GE) | WT_BIT(WT_GT) | WT_BIT(WT_ADD) |
    WT_BIT(WT_SUB) | WT_BIT(WT_MUL) | WT_BIT(WT_DIV) | WT_BIT(WT_MOD);

static bool is_ruled(wr_type type) {
    return (k_ruled_mask & WT_BIT(type)) != 0;
}

/* Types whose operands are limited to the width of Y. */
static bool in_limit_group(wr_type type) {
    return is_unary(type) || type == WT_AND || type == WT_OR || type == WT_XOR || type == WT_ADD ||
           type == WT_SUB;
}

static bool in_cap_group(wr_type type) {
    return type == WT_ADD || type == WT_SUB || type == WT_MUL || type == WT_AND || type == WT_OR ||
           type == WT_XOR;
}

static bool is_one_bit_type(wr_type type) {
    return (type >= WT_LT && type <= WT_GT) || (type >= WT_REDUCE_AND && type <= WT_LOGIC_OR);
}

/* Whether port A (port_b false) or port B of a cell with these flags is read as signed. */
static bool port_signed(wr_type type, bool fa, bool fb, bool port_b) {
    switch (type) {
    case WT_SHL:
    case WT_SHR:
    case WT_SSHR:
        return port_b ? false : fa;
    case WT_SHIFTX:
    case WT_POW:
        return port_b ? fb : fa;
    case WT_NOT:
    case WT_NEG:
        return fa;
    default:
        return fa && fb;
    }
}

static void warn_mixed(wr_state *ws, odin3_node_id node, wr_type type, bool fa, bool fb) {
    if (is_ruled(type) && !is_unary(type) && fa != fb) {
        odin3_opt_warn_mixed_sign(ws->oc, node, wr_type_name(ws, node));
    }
}

/* --- bits of a port --------------------------------------------------------------------------- */

/* Yosys's run_reduce_inport loop: drop the top bit while it repeats the one below (signed) or is
 * the constant 0 (unsigned); one bit always stays. */
static uint32_t reduce_port(const wr_state *ws, const odin3_net_id *sig, uint32_t len,
                            bool is_signed) {
    while (len > 1) {
        const bool drop = is_signed ? wr_same(ws, sig[len - 1U], sig[len - 2U])
                                    : wr_is_const(ws, sig[len - 1U], ODIN3_CONST_0);
        if (!drop) {
            break;
        }
        len--;
    }
    return len;
}

/* Yosys's reduced_opsize (the size may reach 0 when unsigned). */
static uint32_t reduced_opsize(const wr_state *ws, const odin3_net_id *sig, uint32_t size,
                               bool is_signed) {
    while (is_signed ? size >= 2U && wr_same(ws, sig[size - 1U], sig[size - 2U])
                     : size >= 1U && wr_is_const(ws, sig[size - 1U], ODIN3_CONST_0)) {
        size--;
    }
    return size;
}

/* --- the cell's view -------------------------------------------------------------------------- */

typedef struct wr_op {
    odin3_node_id node;
    wr_type type;
    bool binary, fa, fb;
    uint32_t na, nb, ny;
    const odin3_net_id *a, *b, *y;
} wr_op;

typedef struct wr_plan {
    uint32_t la, lb, ly; /* the new widths */
    uint32_t cap;        /* Y bits above ly that are connected to the pad bit */
    bool fa, fb, pad_signed, to_signed, to_unsigned;
} wr_plan;

static int64_t param_int(const wr_state *ws, odin3_node_id node, uint32_t index) {
    return odin3_node_param(ws->m, node, index)->i;
}

/* Loads node into op (its nets in ws->nets); false when the cell is to be left alone: an open pin
 * or a constant Y bit (Yosys's sig.has_const()). *st is the status. */
static bool load_op(wr_state *ws, odin3_node_id node, wr_type type, wr_op *op, odin3_status *st) {
    bool complete = false;
    *st = wr_gather(ws, node, &complete);
    if (*st != ODIN3_OK || !complete) {
        return false;
    }
    op->node = node;
    op->type = type;
    op->binary = !is_unary(type);
    const uint32_t out = op->binary ? 2U : 1U;
    op->na = odin3_node_port(ws->m, node, PORT_A).count;
    op->nb = op->binary ? odin3_node_port(ws->m, node, PORT_B).count : 0U;
    op->ny = odin3_node_port(ws->m, node, out).count;
    op->fa = param_int(ws, node, PRM_A_SIGNED) != 0;
    op->fb = op->binary && param_int(ws, node, PRM_B_SIGNED) != 0;
    const odin3_net_id *nets = ws->nets.data;
    op->a = nets;
    op->b = nets + op->na;
    op->y = nets + op->na + op->nb;
    for (uint32_t k = 0; k < op->ny; k++) {
        if (wr_const(ws, op->y[k]) != ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

/* --- the plan ----------------------------------------------------------------------------------
 */

static uint64_t max_u64(uint64_t lhs, uint64_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

/* Rule 2: with A, B and Y of one width the signedness is free; the cheaper reading wins. */
static void plan_free_choice(const wr_state *ws, const wr_op *op, wr_plan *pl, uint32_t ma,
                             uint32_t mb) {
    if (!(op->type == WT_MUL || op->type == WT_ADD || op->type == WT_SUB) || ma != op->ny ||
        mb != op->ny) {
        return;
    }
    const uint64_t sa = reduced_opsize(ws, op->a, ma, true);
    const uint64_t sb = reduced_opsize(ws, op->b, mb, true);
    const uint64_t ua = reduced_opsize(ws, op->a, ma, false);
    const uint64_t ub = reduced_opsize(ws, op->b, mb, false);
    const uint64_t signed_cost = op->type == WT_MUL ? sa * sb : max_u64(sa, sb);
    const uint64_t unsigned_cost = op->type == WT_MUL ? ua * ub : max_u64(ua, ub);
    if (signed_cost < unsigned_cost) {
        pl->to_signed = !(op->fa && op->fb);
        pl->fa = true;
        pl->fb = true;
    }
}

/* Rule 3: A (except on $shiftx), then B, cut to their limits and trimmed. */
static void plan_ports(const wr_state *ws, const wr_op *op, wr_plan *pl, uint32_t ma, uint32_t mb) {
    pl->la = ma;
    pl->lb = mb;
    if (op->type != WT_SHIFTX) {
        pl->la = reduce_port(ws, op->a, ma, port_signed(op->type, pl->fa, pl->fb, false));
    }
    if (op->binary) {
        pl->lb = reduce_port(ws, op->b, mb, port_signed(op->type, pl->fa, pl->fb, true));
    }
}

/* Rule 4: signed with both top bits the constant 0 is unsigned. Returns the A signedness left. */
static bool plan_to_unsigned(const wr_state *ws, const wr_op *op, wr_plan *pl, bool pa, bool pb) {
    const bool a0 = pl->la > 0 && wr_is_const(ws, op->a[pl->la - 1U], ODIN3_CONST_0);
    if (op->binary) {
        const bool b0 = pl->lb > 0 && wr_is_const(ws, op->b[pl->lb - 1U], ODIN3_CONST_0);
        if (pa && pb && a0 && b0) {
            pl->fa = pl->fb = false;
            pl->to_unsigned = true;
            return false;
        }
    } else if (pa && a0) {
        pl->fa = false;
        pl->to_unsigned = true;
        return false;
    }
    return pa;
}

/* Rules 5 and 6: Y loses its unused top bits (not on a signed-A $shr), then the bits no result
 * can use. */
static void plan_y(const wr_state *ws, const wr_op *op, wr_plan *pl, bool pa) {
    pl->ly = op->ny;
    if (!(pa && op->type == WT_SHR)) {
        while (pl->ly > 0 && wr_unused(ws, op->y[pl->ly - 1U])) {
            pl->ly--;
        }
    }
    if (!in_cap_group(op->type)) {
        return;
    }
    pl->pad_signed = port_signed(op->type, pl->fa, pl->fb, false) || op->type == WT_SUB;
    uint32_t max_y = pl->la > pl->lb ? pl->la : pl->lb;
    max_y += op->type == WT_ADD || op->type == WT_SUB ? 1U : 0U;
    max_y = op->type == WT_MUL ? pl->la + pl->lb : max_y;
    max_y = max_y < 1U ? 1U : max_y;
    if (pl->ly > max_y) {
        pl->cap = pl->ly - max_y;
        pl->ly = max_y;
    }
}

static void plan_op(const wr_state *ws, const wr_op *op, wr_plan *pl) {
    memset(pl, 0, sizeof *pl);
    pl->fa = op->fa;
    pl->fb = op->fb;
    uint32_t ma = op->na;
    uint32_t mb = op->binary ? op->nb : 0U;
    if (in_limit_group(op->type)) {
        ma = ma < op->ny ? ma : op->ny;
        mb = mb < op->ny ? mb : op->ny;
    }
    plan_free_choice(ws, op, pl, ma, mb);
    plan_ports(ws, op, pl, ma, mb);
    const bool pa = op->type != WT_SHIFTX && port_signed(op->type, pl->fa, pl->fb, false);
    const bool pb = op->binary && port_signed(op->type, pl->fa, pl->fb, true);
    const bool pa_left = plan_to_unsigned(ws, op, pl, pa, pb);
    plan_y(ws, op, pl, pa_left);
}

static bool plan_changed(const wr_op *op, const wr_plan *pl) {
    return pl->la != op->na || pl->lb != op->nb || pl->ly != op->ny || pl->fa != op->fa ||
           pl->fb != op->fb;
}

/* --- applying a plan ---------------------------------------------------------------------------
 */

static void log_flags(const wr_state *ws, odin3_node_id node, const char *how) {
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "wreduce: module '%s': converting %s '%s' from %s",
              wr_module_name(ws), wr_type_name(ws, node), label, how);
}

/* The nets Yosys queues: the A and B bits that went. */
static odin3_status mark_ports(wr_state *ws, const wr_op *op, const wr_plan *pl) {
    odin3_status st = wr_mark(ws, op->a + pl->la, op->na - pl->la);
    return st == ODIN3_OK && op->binary ? wr_mark(ws, op->b + pl->lb, op->nb - pl->lb) : st;
}

/* The DEBUG lines and counters of a plan that was applied (op still names the old cell). */
static void note_plan(wr_state *ws, const wr_op *op, const wr_plan *pl, odin3_node_id node) {
    const uint32_t removed_y = op->ny - pl->ly;
    if (pl->la != op->na) {
        wr_log_removed(ws, node, op->na - pl->la, op->na, "port A of");
        wr_count(ws, WR_RULE_PORT_A);
    }
    if (pl->lb != op->nb) {
        wr_log_removed(ws, node, op->nb - pl->lb, op->nb, "port B of");
        wr_count(ws, WR_RULE_PORT_B);
    }
    if (removed_y > 0) {
        wr_log_removed(ws, node, removed_y, op->ny, "port Y of");
        ws->cnt->rule[WR_RULE_Y_UNUSED] += removed_y > pl->cap ? 1U : 0U;
        ws->cnt->rule[WR_RULE_Y_CAP] += pl->cap > 0 ? 1U : 0U;
    }
    if (pl->to_signed) {
        log_flags(ws, node, "unsigned to signed");
        wr_count(ws, WR_RULE_TO_SIGNED);
        ws->cnt->to_signed++;
    }
    if (pl->to_unsigned) {
        log_flags(ws, node, "signed to unsigned");
        wr_count(ws, WR_RULE_TO_UNSIGNED);
        ws->cnt->to_unsigned++;
    }
    ws->visited_bits += (op->na - pl->la) + (op->nb - pl->lb) + removed_y;
}

/* The cell's rebuilt form: same type, new flags and widths. */
typedef struct built {
    odin3_value params[WR_MAX_PARAMS];
    odin3_netvec ports[N_BIN_PORTS];
    odin3_kit_cell cell;
} built;

static void build_op(const wr_state *ws, const wr_op *op, const wr_plan *pl, built *out) {
    const uint32_t count = wr_params(ws, op->node, out->params);
    out->params[PRM_A_SIGNED] = odin3_value_int(pl->fa ? 1 : 0);
    if (op->binary) {
        out->params[PRM_B_SIGNED] = odin3_value_int(pl->fb ? 1 : 0);
        out->params[PRM_A_WIDTH] = odin3_value_int(pl->la);
        out->params[PRM_B_WIDTH] = odin3_value_int(pl->lb);
    } else {
        out->params[PRM_UN_A_WIDTH] = odin3_value_int(pl->la);
    }
    out->params[count - 1U] = odin3_value_int(pl->ly);
    out->ports[PORT_A] = (odin3_netvec){op->a, pl->la};
    if (op->binary) {
        out->ports[PORT_B] = (odin3_netvec){op->b, pl->lb};
    }
    out->ports[op->binary ? 2 : 1] = (odin3_netvec){NULL, 0};
    out->cell = (odin3_kit_cell){odin3_node_type(ws->m, op->node), out->params, count, out->ports,
                                 (odin3_prov_id){0}};
}

/* The cell is applied: Y keeps its width (kit_rebuild_node), or loses bits (wr_commit). */
static odin3_status apply_plan(wr_state *ws, const wr_op *op, const wr_plan *pl,
                               odin3_node_id *node) {
    built bt;
    build_op(ws, op, pl, &bt);
    if (pl->ly == op->ny) {
        const odin3_kit_rebuild req = {op->node, bt.cell};
        return wr_rebuild(ws, &req, node);
    }
    const uint32_t out_port = op->binary ? 2U : 1U;
    odin3_net_id zero = {0};
    odin3_status st = wr_ysrc_reset(ws, op->node, out_port);
    st = st == ODIN3_OK && pl->cap > 0 && !pl->pad_signed
             ? odin3_opt_const_net(ws->oc, ODIN3_BIT_0, &zero)
             : st;
    for (uint32_t k = 0; st == ODIN3_OK && !pl->pad_signed && k < pl->cap; k++) {
        *(odin3_net_id *)odin3_vec_at(&ws->ysrc, pl->ly + k) = zero;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    wr_rewrite rw = {op->node,          &bt.cell, out_port, pl->ly, pl->pad_signed ? pl->cap : 0U,
                     (odin3_node_id){0}};
    st = wr_commit(ws, &rw);
    *node = rw.made;
    return st;
}

/* Rule 7: nothing of Y is left, the cell goes. */
static odin3_status remove_op(wr_state *ws, const wr_op *op) {
    char label[WR_LABEL_MAX];
    wr_label(ws, op->node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "wreduce: module '%s': removed %s '%s'", wr_module_name(ws),
              wr_type_name(ws, op->node), label);
    const uint32_t out_port = op->binary ? 2U : 1U;
    odin3_status st = wr_ysrc_reset(ws, op->node, out_port);
    wr_rewrite rw = {op->node, NULL, out_port, 0, 0, (odin3_node_id){0}};
    st = st == ODIN3_OK ? wr_commit(ws, &rw) : st;
    if (st == ODIN3_OK) {
        wr_count(ws, WR_RULE_OP_REMOVED);
        ws->cnt->removed++;
    }
    return st;
}

odin3_status wr_run_op(wr_state *ws, odin3_node_id node) {
    const wr_type type = wr_type_of(ws, node);
    ws->visited_bits = 0;
    for (;;) {
        wr_op op;
        wr_plan pl;
        odin3_status st = ODIN3_OK;
        if (!load_op(ws, node, type, &op, &st)) {
            return st;
        }
        warn_mixed(ws, node, type, op.fa, op.fb);
        plan_op(ws, &op, &pl);
        if (!plan_changed(&op, &pl)) {
            break;
        }
        st = mark_ports(ws, &op, &pl);
        if (st != ODIN3_OK) {
            return st;
        }
        if (pl.ly == 0) {
            note_plan(ws, &op, &pl, node);
            ws->cnt->bits += ws->visited_bits;
            ws->cnt->narrowed += ws->visited_bits > 0 ? 1U : 0U;
            return remove_op(ws, &op);
        }
        odin3_node_id made = {0};
        st = apply_plan(ws, &op, &pl, &made);
        if (st != ODIN3_OK) {
            return st;
        }
        note_plan(ws, &op, &pl, made);
        node = made;
    }
    ws->cnt->bits += ws->visited_bits;
    ws->cnt->narrowed += ws->visited_bits > 0 ? 1U : 0U;
    return ODIN3_OK;
}

/* --- the pre-step ------------------------------------------------------------------------------
 */

/* Yosys's one-bit-result rule: Y keeps bit 0, the rest of Y reads 0. */
static odin3_status prestep_one_bit(wr_state *ws, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    const uint32_t out = def->n_ports - 1U;
    const uint32_t ny = odin3_node_port(ws->m, node, out).count;
    bool complete = false;
    odin3_status st = wr_gather(ws, node, &complete);
    if (st != ODIN3_OK || !complete || ny <= 1U) {
        return st;
    }
    const odin3_net_id *nets = ws->nets.data;
    const uint32_t first_y = (uint32_t)ws->nets.len - ny;
    for (uint32_t k = 0; k < ny; k++) {
        if (wr_const(ws, nets[first_y + k]) != ODIN3_CONST_NONE) {
            return ODIN3_OK;
        }
    }
    odin3_value params[WR_MAX_PARAMS];
    odin3_netvec ports[N_BIN_PORTS] = {{NULL, 0}, {NULL, 0}, {NULL, 0}};
    const uint32_t count = wr_params(ws, node, params);
    params[count - 1U] = odin3_value_int(1);
    uint32_t at = 0;
    for (uint32_t i = 0; i < out; i++) {
        const uint32_t width = odin3_node_port(ws->m, node, i).count;
        ports[i] = (odin3_netvec){nets + at, width};
        at += width;
    }
    odin3_net_id zero = {0};
    st = odin3_opt_const_net(ws->oc, ODIN3_BIT_0, &zero);
    st = st == ODIN3_OK ? wr_ysrc_reset(ws, node, out) : st;
    for (uint32_t k = 1; st == ODIN3_OK && k < ny; k++) {
        *(odin3_net_id *)odin3_vec_at(&ws->ysrc, k) = zero;
    }
    const odin3_kit_cell cell = {odin3_node_type(ws->m, node), params, count, ports, {0}};
    wr_rewrite rw = {node, &cell, out, 1, 0, {0}};
    wr_log_removed(ws, node, ny - 1U, ny, "port Y of");
    st = st == ODIN3_OK ? wr_commit(ws, &rw) : st;
    if (st == ODIN3_OK) {
        wr_count(ws, WR_RULE_ONE_BIT);
        ws->cnt->narrowed++;
        ws->cnt->bits += ny - 1U;
    }
    return st;
}

/* $div, $mod and $pow: A, then B, lose their top constant-0 bits (a signed port keeps one zero). */
static uint32_t trim_zeros(const wr_state *ws, const odin3_net_id *sig, uint32_t len,
                           bool is_signed) {
    while (len > 1U && wr_is_const(ws, sig[len - 1U], ODIN3_CONST_0) &&
           (!is_signed || wr_is_const(ws, sig[len - 2U], ODIN3_CONST_0))) {
        len--;
    }
    return len;
}

static odin3_status prestep_divmod(wr_state *ws, odin3_node_id node, wr_type type) {
    bool complete = false;
    odin3_status st = wr_gather(ws, node, &complete);
    if (st != ODIN3_OK || !complete) {
        return st;
    }
    const bool fa = param_int(ws, node, PRM_A_SIGNED) != 0;
    const bool fb = param_int(ws, node, PRM_B_SIGNED) != 0;
    warn_mixed(ws, node, type, fa, fb);
    const uint32_t na = odin3_node_port(ws->m, node, PORT_A).count;
    const uint32_t nb = odin3_node_port(ws->m, node, PORT_B).count;
    const odin3_net_id *nets = ws->nets.data;
    const uint32_t la = trim_zeros(ws, nets, na, port_signed(type, fa, fb, false));
    const uint32_t lb = trim_zeros(ws, nets + na, nb, port_signed(type, fa, fb, true));
    if (la == na && lb == nb) {
        return ODIN3_OK;
    }
    odin3_value params[WR_MAX_PARAMS];
    const uint32_t count = wr_params(ws, node, params);
    params[PRM_A_WIDTH] = odin3_value_int(la);
    params[PRM_B_WIDTH] = odin3_value_int(lb);
    odin3_netvec ports[N_BIN_PORTS] = {{nets, la}, {nets + na, lb}, {NULL, 0}};
    const odin3_kit_rebuild req = {node, {odin3_node_type(ws->m, node), params, count, ports, {0}}};
    if (la != na) {
        wr_log_removed(ws, node, na - la, na, "port A of");
        wr_count(ws, WR_RULE_DIVMOD_TRIM);
    }
    if (lb != nb) {
        wr_log_removed(ws, node, nb - lb, nb, "port B of");
        wr_count(ws, WR_RULE_DIVMOD_TRIM);
    }
    odin3_node_id made = {0};
    st = wr_rebuild(ws, &req, &made);
    if (st == ODIN3_OK) {
        ws->cnt->narrowed++;
        ws->cnt->bits += (na - la) + (nb - lb);
    }
    return st;
}

static uint32_t ceil_log2(uint64_t value) {
    uint32_t bits = 0;
    while (((uint64_t)1 << bits) < value) {
        bits++;
    }
    return bits;
}

/* The low `keep` address bits of each of `ports` addresses of `abits` bits, into out. */
typedef struct addr_shape {
    uint32_t ports, abits, keep;
} addr_shape;

static odin3_status trim_addrs(const odin3_net_id *addr, addr_shape shape, odin3_vec *out) {
    odin3_vec_clear(out);
    for (uint32_t port = 0; port < shape.ports; port++) {
        for (uint32_t bit = 0; bit < shape.keep; bit++) {
            odin3_status st = odin3_kit_push_net(out, addr[(size_t)port * shape.abits + bit]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* $mem: the address ports keep ceil_log2(OFFSET + SIZE) bits (not with -memx). */
static odin3_status prestep_mem(wr_state *ws, odin3_node_id node) {
    const int64_t size = param_int(ws, node, MEM_PRM_SIZE);
    const int64_t offset = param_int(ws, node, MEM_PRM_OFFSET);
    const uint32_t abits = (uint32_t)param_int(ws, node, MEM_PRM_ABITS);
    const uint32_t keep = offset >= 0 ? ceil_log2((uint64_t)offset + (uint64_t)size) : abits;
    bool complete = false;
    odin3_status st = wr_gather(ws, node, &complete);
    if (st != ODIN3_OK || !complete || keep >= abits) {
        return st;
    }
    const uint32_t rd = (uint32_t)param_int(ws, node, MEM_PRM_RD_PORTS);
    const uint32_t wr = (uint32_t)param_int(ws, node, MEM_PRM_WR_PORTS);
    const odin3_net_id *nets = ws->nets.data;
    uint32_t at = 0;
    const odin3_net_id *rd_addr = NULL;
    const odin3_net_id *wr_addr = NULL;
    for (uint32_t i = 0; i < MEM_N_PORTS; i++) {
        rd_addr = i == MEM_PORT_RD_ADDR ? nets + at : rd_addr;
        wr_addr = i == MEM_PORT_WR_ADDR ? nets + at : wr_addr;
        at += odin3_node_port(ws->m, node, i).count;
    }
    st = trim_addrs(rd_addr, (addr_shape){rd, abits, keep}, &ws->nets2);
    st = st == ODIN3_OK ? trim_addrs(wr_addr, (addr_shape){wr, abits, keep}, &ws->ysrc) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[WR_MAX_PARAMS];
    const uint32_t count = wr_params(ws, node, params);
    params[MEM_PRM_ABITS] = odin3_value_int(keep);
    odin3_netvec ports[MEM_N_PORTS];
    memset(ports, 0, sizeof ports);
    ports[MEM_PORT_RD_ADDR] =
        (odin3_netvec){ws->nets2.len > 0 ? ws->nets2.data : k_no_nets, (uint32_t)ws->nets2.len};
    ports[MEM_PORT_WR_ADDR] =
        (odin3_netvec){ws->ysrc.len > 0 ? ws->ysrc.data : k_no_nets, (uint32_t)ws->ysrc.len};
    const odin3_kit_rebuild req = {node, {odin3_node_type(ws->m, node), params, count, ports, {0}}};
    odin3_node_id made = {0};
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    st = wr_rebuild(ws, &req, &made);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_DEBUG,
                  "wreduce: module '%s': removed top %u address bits (of %u) from memory '%s'",
                  wr_module_name(ws), (unsigned)(abits - keep), (unsigned)abits, label);
        wr_count(ws, WR_RULE_MEM_ADDR);
        ws->cnt->narrowed++;
        ws->cnt->bits += (abits - keep) * (rd + wr);
    }
    return st;
}

odin3_status wr_prestep(wr_state *ws, odin3_node_id node) {
    const wr_type type = wr_type_of(ws, node);
    if (is_one_bit_type(type)) {
        return prestep_one_bit(ws, node);
    }
    if (type == WT_DIV || type == WT_MOD || type == WT_POW) {
        return prestep_divmod(ws, node, type);
    }
    return type == WT_MEM && !ws->opts->memx ? prestep_mem(ws, node) : ODIN3_OK;
}
