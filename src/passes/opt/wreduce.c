/*
 * wreduce.c — wreduce (3G spec WR-1..WR-11; Yosys 0.55 passes/opt/wreduce.cc): the pass, the
 * worker's rounds, the bit helpers and the rewrite machinery shared with wreduce_op.c (the
 * pre-step and the operator rules) and wreduce_ff.c (muxes, flip-flops, latches).
 *
 * Order (WR-7). Yosys visits the cells of a round in the order their names were first interned,
 * and a rewritten cell keeps its name. A rebuilt Odin III node has a new ID, so each node carries
 * a rank, the ID of the node it descends from, and a round is sorted by rank: the same order as
 * Yosys's on an RTLIL file written in node-ID order. No hash order reaches the IR.
 *
 * Nets. Yosys's work_queue_bits are SigBits, which a later connect redirects; here a changed net
 * that is merged away is followed through `fwd` (a net that wreduce merged into another) to the
 * live net whose readers the next round visits.
 */
#include "passes/opt/wreduce.h"

#include "ir/design.h"
#include "ir/proc.h"
#include "passes/budget.h"
#include "passes/ffinit.h"
#include "passes/opt/opt_test.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { RANK_SHIFT = 32 };

static const char *const k_type_names[WT_COUNT] = {
    "$not",        "$neg",       "$and",        "$or",          "$xor",         "$xnor",
    "$shl",        "$shr",       "$sshr",       "$shiftx",      "$lt",          "$le",
    "$eq",         "$ne",        "$ge",         "$gt",          "$add",         "$sub",
    "$mul",        "$mux",       "$pmux",       "$dff",         "$dffe",        "$adff",
    "$adffe",      "$sdff",      "$sdffe",      "$sdffce",      "$dlatch",      "$adlatch",
    "$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor", "$reduce_bool", "$logic_not",
    "$logic_and",  "$logic_or",  "$div",        "$mod",         "$pow",         "$mem"};

static const char *const k_rule_names[WR_N_RULES] = {
    [WR_RULE_ONE_BIT] = "one_bit",
    [WR_RULE_DIVMOD_TRIM] = "divmod_trim",
    [WR_RULE_MEM_ADDR] = "mem_addr",
    [WR_RULE_PORT_A] = "port_a",
    [WR_RULE_PORT_B] = "port_b",
    [WR_RULE_TO_SIGNED] = "to_signed",
    [WR_RULE_TO_UNSIGNED] = "to_unsigned",
    [WR_RULE_Y_UNUSED] = "y_unused",
    [WR_RULE_Y_CAP] = "y_cap",
    [WR_RULE_OP_REMOVED] = "op_removed",
    [WR_RULE_MUX_NARROW] = "mux_narrow",
    [WR_RULE_MUX_REMOVED] = "mux_removed",
    [WR_RULE_FF_ZERO_EXT] = "ff_zero_ext",
    [WR_RULE_FF_SIGN_EXT] = "ff_sign_ext",
    [WR_RULE_FF_UNUSED] = "ff_unused",
    [WR_RULE_FF_REMOVED] = "ff_removed",
};

const char *odin3_wr_rule_name(uint32_t rule) {
    return rule < (uint32_t)WR_N_RULES ? k_rule_names[rule] : NULL;
}

void wr_count(wr_state *ws, odin3_wr_rule rule) {
    ws->cnt->rule[rule]++;
}

/* --- bits ------------------------------------------------------------------------------------ */

odin3_const wr_const(const wr_state *ws, odin3_net_id net) {
    return odin3_net_const_value(ws->m, net);
}

bool wr_is_const(const wr_state *ws, odin3_net_id net, odin3_const value) {
    return wr_const(ws, net) == value;
}

bool wr_same(const wr_state *ws, odin3_net_id lhs, odin3_net_id rhs) {
    if (lhs.v == rhs.v) {
        return true;
    }
    odin3_const cl = wr_const(ws, lhs);
    return cl != ODIN3_CONST_NONE && cl == wr_const(ws, rhs);
}

/* A net on a wire with `keep`: Yosys's keep_bits. */
static bool net_keep(const wr_state *ws, odin3_net_id net) {
    odin3_wirebit prim = odin3_net_primary(ws->m, net);
    if (odin3_wire_valid(prim.wire) &&
        odin3_kit_keep(ws->oc, (odin3_objref){ODIN3_OBJ_WIRE, prim.wire.v})) {
        return true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(ws->m, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire) &&
            odin3_kit_keep(ws->oc, (odin3_objref){ODIN3_OBJ_WIRE, alias.wb.wire.v})) {
            return true;
        }
    }
    return false;
}

bool wr_unused(const wr_state *ws, odin3_net_id net) {
    return odin3_net_pins(ws->m, net).count <= 1U && !net_keep(ws, net);
}

/* --- names and types ----------------------------------------------------------------------------
 */

wr_type wr_type_of(const wr_state *ws, odin3_node_id node) {
    const uint32_t id = odin3_node_type(ws->m, node).v;
    for (uint32_t i = 0; i < (uint32_t)WT_COUNT; i++) {
        if (ws->types[i].v != 0 && ws->types[i].v == id) {
            return (wr_type)i;
        }
    }
    return WT_NONE;
}

const char *wr_module_name(const wr_state *ws) {
    return odin3_strtab_get(odin3_design_strtab(ws->oc->design), odin3_module_name(ws->m));
}

void wr_label(const wr_state *ws, odin3_node_id node, char *out, size_t size) {
    odin3_kit_node_label(ws->oc, node, out, size);
}

const char *wr_type_name(const wr_state *ws, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    return def != NULL ? def->name : "?";
}

void wr_log_removed(const wr_state *ws, odin3_node_id node, uint32_t count, uint32_t of,
                    const char *what) {
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "wreduce: module '%s': removed top %u bits (of %u) from %s %s '%s'",
              wr_module_name(ws), (unsigned)count, (unsigned)of, what, wr_type_name(ws, node),
              label);
}

uint32_t wr_params(const wr_state *ws, odin3_node_id node, odin3_value *out) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    uint32_t count = def->n_params < WR_MAX_PARAMS ? def->n_params : WR_MAX_PARAMS;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = *odin3_node_param(ws->m, node, i);
    }
    return count;
}

/* --- side tables ------------------------------------------------------------------------------ */

/* vec (uint32_t elements) grown to `want` entries: 0, or the entry's own index when identity. */
static odin3_status grow_u32(odin3_vec *vec, size_t want, bool identity) {
    odin3_status st = odin3_vec_reserve(vec, want);
    while (st == ODIN3_OK && vec->len < want) {
        uint32_t *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = identity ? (uint32_t)(vec->len - 1U) : 0U;
    }
    return st;
}

/* Room for the rewrite about to happen: the merged-into table, the new nodes' ranks. */
static odin3_status prepare(wr_state *ws) {
    odin3_status st = grow_u32(&ws->fwd, (size_t)odin3_module_net_end(ws->m) + 1U, false);
    return st == ODIN3_OK ? grow_u32(&ws->rank, (size_t)odin3_module_node_end(ws->m) + 2U, true)
                          : st;
}

static uint32_t rank_of(const wr_state *ws, uint32_t node) {
    return node < ws->rank.len ? *(const uint32_t *)odin3_vec_cat(&ws->rank, node) : node;
}

static void inherit_rank(wr_state *ws, odin3_node_id from, odin3_node_id to) {
    if (to.v < ws->rank.len && from.v < ws->rank.len) {
        *(uint32_t *)odin3_vec_at(&ws->rank, to.v) = rank_of(ws, from.v);
    }
}

odin3_status wr_mark(wr_state *ws, const odin3_net_id *nets, uint32_t count) {
    odin3_status st = odin3_vec_reserve(&ws->marks, ws->marks.len + count);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = odin3_kit_push_u64(&ws->marks, nets[i].v);
    }
    return st;
}

odin3_status wr_gather(wr_state *ws, odin3_node_id node, bool *complete) {
    const odin3_pinslice pins = odin3_node_pins(ws->m, node);
    odin3_vec_clear(&ws->nets);
    odin3_status st = odin3_vec_reserve(&ws->nets, pins.count);
    *complete = true;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(ws->m, (odin3_pin_id){pins.first.v + k});
        *complete = *complete && odin3_net_valid(net);
        st = odin3_kit_push_net(&ws->nets, net);
    }
    return st;
}

odin3_status wr_ysrc_reset(wr_state *ws, odin3_node_id node, uint32_t out_port) {
    const odin3_pinslice pins = odin3_node_port(ws->m, node, out_port);
    odin3_vec_clear(&ws->ysrc);
    odin3_vec_clear(&ws->olds);
    odin3_status st = odin3_vec_reserve(&ws->ysrc, pins.count);
    st = st == ODIN3_OK ? odin3_vec_reserve(&ws->olds, pins.count) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(ws->m, (odin3_pin_id){pins.first.v + k});
        st = odin3_kit_push_net(&ws->ysrc, net);
        st = st == ODIN3_OK ? odin3_kit_push_net(&ws->olds, net) : st;
    }
    return st;
}

/* --- the rewrite ------------------------------------------------------------------------------ */

static odin3_net_id *ysrc_at(wr_state *ws, uint32_t idx) {
    return odin3_vec_at(&ws->ysrc, idx);
}

/* Undoes commit_build: the new node and its (pinless) fresh nets go again. */
static void undo_build(wr_state *ws, const wr_rewrite *rw) {
    if (!odin3_node_valid(rw->made)) {
        return;
    }
    (void)odin3_node_delete(ws->m, rw->made);
    for (uint32_t k = 0; k < rw->n_keep; k++) {
        odin3_net_id net = *ysrc_at(ws, k);
        if (odin3_net_live(ws->m, net) && odin3_net_pins(ws->m, net).count == 0) {
            (void)odin3_net_delete(ws->m, net);
        }
    }
}

/* The replacement node on fresh output nets; ysrc[0 .. n_keep) = those nets, the pad entries the
 * last of them. */
static odin3_status commit_build(wr_state *ws, wr_rewrite *rw) {
    odin3_kit_cell cell = *rw->cell;
    odin3_status st = odin3_kit_prov(ws->oc, &rw->node, 1, &cell.prov);
    st = st == ODIN3_OK ? odin3_kit_new_cell(ws->oc, &cell, &rw->made) : st;
    if (st != ODIN3_OK) {
        rw->made = (odin3_node_id){0};
        return st;
    }
    const odin3_pinslice pins = odin3_node_port(ws->m, rw->made, rw->out_port);
    for (uint32_t k = 0; k < rw->n_keep && k < pins.count; k++) {
        *ysrc_at(ws, k) = odin3_pin_net(ws->m, (odin3_pin_id){pins.first.v + k});
    }
    for (uint32_t k = 0; rw->n_keep > 0 && k < rw->n_pad; k++) {
        *ysrc_at(ws, rw->n_keep + k) = *ysrc_at(ws, rw->n_keep - 1U);
    }
    st = odin3_kit_copy_attrs(ws->m, rw->node, rw->made);
    if (st != ODIN3_OK) {
        undo_build(ws, rw);
        rw->made = (odin3_node_id){0};
    }
    return st;
}

/* The `init` of the dropped Q bits goes while their nets are still apart (the ties are queued),
 * then the ties are made. */
static odin3_status drop_inits(wr_state *ws) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ws->init.len; i++) {
        const odin3_net_id *net = odin3_vec_cat(&ws->init, i);
        st = odin3_ffinit_remove(ws->m, *net, NULL);
    }
    st = st == ODIN3_OK ? odin3_kit_flush_ties(ws->oc) : st;
    return odin3_kit_half_done(ws->oc, st);
}

/* The destructive step: kit_connect (the old node goes, its outputs are merged). */
static odin3_status commit_connect(wr_state *ws, wr_rewrite *rw) {
    const odin3_kit_conn conn = {rw->node, rw->out_port, ws->ysrc.data, (uint32_t)ws->ysrc.len};
    const bool defer = ws->init.len > 0;
    ws->oc->defer_ties = defer;
    odin3_status st = odin3_kit_connect(ws->oc, &conn);
    ws->oc->defer_ties = false;
    if (st != ODIN3_OK) {
        if (odin3_node_live(ws->m, rw->node)) {
            undo_build(ws, rw); /* nothing was destroyed */
            rw->made = (odin3_node_id){0};
        }
        return st;
    }
    return defer ? drop_inits(ws) : ODIN3_OK;
}

static odin3_status commit_inner(wr_state *ws, wr_rewrite *rw) {
    odin3_status st = prepare(ws);
    rw->made = (odin3_node_id){0};
    odin3_kit_begin_rewrite(ws->oc);
    st = st == ODIN3_OK && rw->cell != NULL ? commit_build(ws, rw) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    const uint32_t name = odin3_node_name(ws->m, rw->node);
    st = commit_connect(ws, rw);
    if (st != ODIN3_OK) {
        return st;
    }
    for (size_t k = 0; k < ws->olds.len; k++) { /* the table was sized by prepare */
        const odin3_net_id old = *(const odin3_net_id *)odin3_vec_cat(&ws->olds, k);
        const odin3_net_id now = *ysrc_at(ws, (uint32_t)k);
        if (old.v != now.v && odin3_net_valid(old) && old.v < ws->fwd.len) {
            *(uint32_t *)odin3_vec_at(&ws->fwd, old.v) = now.v;
        }
    }
    if (odin3_node_valid(rw->made)) {
        inherit_rank(ws, rw->node, rw->made);
        st = name != 0 ? odin3_node_rename(ws->m, rw->made, name) : st;
        st = odin3_kit_half_done(ws->oc, st);
    }
    return st;
}

odin3_status wr_commit(wr_state *ws, wr_rewrite *rw) {
    odin3_status st = commit_inner(ws, rw);
    odin3_vec_clear(&ws->init);
    return st;
}

odin3_status wr_rebuild(wr_state *ws, const odin3_kit_rebuild *req, odin3_node_id *out) {
    odin3_status st = prepare(ws);
    odin3_kit_begin_rewrite(ws->oc);
    st = st == ODIN3_OK ? odin3_kit_rebuild_node(ws->oc, req, out) : st;
    if (st == ODIN3_OK) {
        inherit_rank(ws, req->old_node, *out);
    }
    return st;
}

/* --- the rounds ------------------------------------------------------------------------------- */

static odin3_status visit(wr_state *ws, odin3_node_id node) {
    if (!odin3_node_live(ws->m, node)) {
        return ODIN3_OK;
    }
    const wr_type type = wr_type_of(ws, node);
    odin3_vec_clear(&ws->init);
    if (type == WT_MUX || type == WT_PMUX) {
        return wr_run_mux(ws, node);
    }
    if (type >= WT_DFF && type <= WT_ADLATCH) {
        return wr_run_ff(ws, node);
    }
    return type <= WT_MUL ? wr_run_op(ws, node) : ODIN3_OK;
}

static odin3_status push_key(wr_state *ws, odin3_node_id node) {
    return odin3_kit_push_u64(&ws->work, ((uint64_t)rank_of(ws, node.v) << RANK_SHIFT) | node.v);
}

/* The first round: every live node. */
static odin3_status initial_work(wr_state *ws) {
    odin3_vec_clear(&ws->work);
    for (uint32_t id = 1; id < odin3_module_node_end(ws->m); id++) {
        if (odin3_node_live(ws->m, (odin3_node_id){id})) {
            odin3_status st = push_key(ws, (odin3_node_id){id});
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    odin3_kit_sort_unify(&ws->work, 0);
    return ODIN3_OK;
}

/* The net a changed net stands for now (it may have been merged into another). */
static odin3_net_id resolve(const wr_state *ws, uint32_t raw) {
    odin3_net_id net = {raw};
    while (odin3_net_valid(net) && !odin3_net_live(ws->m, net)) {
        net = (odin3_net_id){net.v < ws->fwd.len ? *(const uint32_t *)odin3_vec_cat(&ws->fwd, net.v)
                                                 : 0U};
    }
    return net;
}

/* The next round: every node with a pin on a net the round changed, in rank order. */
static odin3_status next_work(wr_state *ws) {
    odin3_kit_sort_unify(&ws->marks, 0);
    odin3_vec_clear(&ws->work);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ws->marks.len; i++) {
        const odin3_net_id net =
            resolve(ws, (uint32_t) * (const uint64_t *)odin3_vec_cat(&ws->marks, i));
        if (!odin3_net_valid(net) || !odin3_net_live(ws->m, net)) {
            continue;
        }
        const odin3_pinlist pins = odin3_net_pins(ws->m, net);
        for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
            st = push_key(ws, odin3_pin_node(ws->m, pins.pins[k]));
        }
    }
    odin3_kit_sort_unify(&ws->work, 0);
    return st;
}

static odin3_status rounds(wr_state *ws) {
    const uint32_t cap = odin3_opt_wreduce_rounds_now();
    uint32_t done = 0;
    odin3_status st = initial_work(ws);
    while (st == ODIN3_OK && ws->work.len > 0) {
        if (done++ >= cap) {
            odin3_log(ODIN3_LOG_WARN,
                      "wreduce: module '%s': no fixed point after %u rounds; stopping",
                      wr_module_name(ws), (unsigned)cap);
            break;
        }
        odin3_vec_clear(&ws->marks);
        for (size_t i = 0; st == ODIN3_OK && i < ws->work.len; i++) {
            const uint64_t key = *(const uint64_t *)odin3_vec_cat(&ws->work, i);
            st = visit(ws, (odin3_node_id){(uint32_t)(key & UINT32_MAX)});
        }
        st = st == ODIN3_OK ? next_work(ws) : st;
    }
    return st;
}

/* --- one module ------------------------------------------------------------------------------- */

static void state_init(wr_state *ws, odin3_opt_ctx *oc, odin3_wreduce_counts *cnt) {
    memset(ws, 0, sizeof *ws);
    ws->oc = oc;
    ws->m = oc->module;
    ws->opts = oc->opts;
    ws->cnt = cnt;
    for (uint32_t i = 0; i < (uint32_t)WT_COUNT; i++) {
        ws->types[i] = odin3_kit_type_named(oc->design, k_type_names[i]);
    }
    odin3_vec_init(&ws->marks, sizeof(uint64_t));
    odin3_vec_init(&ws->fwd, sizeof(uint32_t));
    odin3_vec_init(&ws->rank, sizeof(uint32_t));
    odin3_vec_init(&ws->work, sizeof(uint64_t));
    odin3_vec_init(&ws->nets, sizeof(odin3_net_id));
    odin3_vec_init(&ws->nets2, sizeof(odin3_net_id));
    odin3_vec_init(&ws->ysrc, sizeof(odin3_net_id));
    odin3_vec_init(&ws->olds, sizeof(odin3_net_id));
    odin3_vec_init(&ws->init, sizeof(odin3_net_id));
    odin3_vec_init(&ws->bits, sizeof(uint8_t));
}

static void state_free(wr_state *ws) {
    odin3_vec_free(&ws->marks);
    odin3_vec_free(&ws->fwd);
    odin3_vec_free(&ws->rank);
    odin3_vec_free(&ws->work);
    odin3_vec_free(&ws->nets);
    odin3_vec_free(&ws->nets2);
    odin3_vec_free(&ws->ysrc);
    odin3_vec_free(&ws->olds);
    odin3_vec_free(&ws->init);
    odin3_vec_free(&ws->bits);
}

/* The design budget at the start of a module: the side tables are a few words per net and node. */
static odin3_status module_budget(const wr_state *ws) {
    const uint64_t extra =
        ((uint64_t)odin3_module_net_end(ws->m) * 2U + (uint64_t)odin3_module_node_end(ws->m) * 3U) *
        sizeof(uint32_t);
    if (odin3_budget_check(ws->oc->pass, ws->oc->design, extra) == ODIN3_OK) {
        return ODIN3_OK;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    if (odin3_budget_message(ws->oc->pass, &msg) == ODIN3_OK && msg.data != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return ODIN3_ERR_PARSE;
}

static odin3_status module_run(wr_state *ws) {
    odin3_status st = module_budget(ws);
    st = st == ODIN3_OK ? odin3_opt_canon_consts(ws->oc) : st;
    st = st == ODIN3_OK ? prepare(ws) : st;
    const uint32_t end = odin3_module_node_end(ws->m);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        if (odin3_node_live(ws->m, (odin3_node_id){id})) {
            st = wr_prestep(ws, (odin3_node_id){id});
        }
    }
    return st == ODIN3_OK ? rounds(ws) : st;
}

static void log_totals(const odin3_wreduce_counts *now, const odin3_wreduce_counts *before) {
    odin3_log(ODIN3_LOG_INFO,
              "wreduce: %u cells narrowed, %u bits removed, %u cells removed, %u cells made "
              "signed, %u cells made unsigned",
              (unsigned)(now->narrowed - before->narrowed), (unsigned)(now->bits - before->bits),
              (unsigned)(now->removed - before->removed),
              (unsigned)(now->to_signed - before->to_signed),
              (unsigned)(now->to_unsigned - before->to_unsigned));
}

odin3_status odin3_opt_wreduce_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "wreduce: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_wreduce_counts before = counts->wreduce;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        if (odin3_module_proc_count(module) > 0) {
            odin3_log(ODIN3_LOG_INFO, "wreduce: module '%s' has processes; skipped",
                      odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(module)));
            continue;
        }
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, module, opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        wr_state ws;
        state_init(&ws, &oc, &counts->wreduce);
        st = module_run(&ws);
        state_free(&ws);
        odin3_opt_ctx_free(&oc);
    }
    if (st == ODIN3_OK) {
        log_totals(&counts->wreduce, &before);
    }
    return st;
}
