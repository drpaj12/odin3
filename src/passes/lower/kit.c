/*
 * kit.c — the builders' toolkit (3B spec §3, §4; plan Task 1): net vectors, the context, fresh
 * nets, the gate helpers, constants, extension and the pairing tree. Every node `lower` creates
 * is made here, so these are the allocation points an out-of-memory test hooks.
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- net vectors ---------------------------------------------------------------------------- */

void lw_bits_init(lw_bits *bits) {
    odin3_vec_init(&bits->v, sizeof(odin3_net_id));
}

void lw_bits_free(lw_bits *bits) {
    odin3_vec_free(&bits->v);
}

void lw_bits_clear(lw_bits *bits) {
    odin3_vec_clear(&bits->v);
}

odin3_status lw_bits_push(lw_bits *bits, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(&bits->v);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

uint32_t lw_bits_len(const lw_bits *bits) {
    return (uint32_t)bits->v.len;
}

odin3_net_id lw_bits_at(const lw_bits *bits, uint32_t idx) {
    return *(const odin3_net_id *)odin3_vec_cat(&bits->v, idx);
}

lw_span lw_bits_span(const lw_bits *bits) {
    return (lw_span){(const odin3_net_id *)bits->v.data, (uint32_t)bits->v.len};
}

/* --- the context ---------------------------------------------------------------------------- */

static const char *const k_gate_names[LW_G_COUNT] = {
    "$_NOT_", "$_AND_",   "$_OR_",    "$_XOR_",      "$_XNOR_",
    "$_MUX_", "$_DFF_P_", "$_DFF_N_", "$_DLATCH_P_", "$_DLATCH_N_"};
/* The <op> of a bit node's name (PHASE3 #38): the gate type without "$_" and the trailing "_". */
static const char *const k_op_names[LW_G_COUNT] = {"not", "and",   "or",    "xor",      "xnor",
                                                   "mux", "dff_p", "dff_n", "dlatch_p", "dlatch_n"};
static const char *const k_const_names[LW_N_CONST] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_"};
static const odin3_const k_const_values[LW_N_CONST] = {ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_X};

static odin3_celltype_id type_named(const odin3_design *design, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, &id)) {
        return (odin3_celltype_id){0};
    }
    return id;
}

odin3_status lw_ctx_init(lw_ctx *lw, odin3_pass_ctx *pass, odin3_design *design,
                         const odin3_lower_opts *opts) {
    memset(lw, 0, sizeof *lw);
    lw->pass = pass;
    lw->design = design;
    lw->opts = opts;
    odin3_vec_init(&lw->created, sizeof(odin3_node_id));
    odin3_vec_init(&lw->created_nets, sizeof(odin3_net_id));
    odin3_vec_init(&lw->portnets, sizeof(odin3_net_id));
    odin3_vec_init(&lw->merged, sizeof(odin3_net_pair));
    for (uint32_t i = 0; i < LW_REP_COUNT; i++) {
        odin3_strbuf_init(&lw->report[i]);
    }
    odin3_strbuf_init(&lw->namebuf);
    for (uint32_t i = 0; i < LW_G_COUNT; i++) {
        lw->gate[i] = type_named(design, k_gate_names[i]);
        if (!odin3_celltype_valid(lw->gate[i])) {
            odin3_log(ODIN3_LOG_ERROR, "lower: cell type '%s' is not registered", k_gate_names[i]);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    for (uint32_t i = 0; i < LW_N_CONST; i++) {
        lw->constant[i] = type_named(design, k_const_names[i]);
        if (!odin3_celltype_valid(lw->constant[i])) {
            odin3_log(ODIN3_LOG_ERROR, "lower: cell type '%s' is not registered", k_const_names[i]);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    uint32_t key = 0;
    lw->keep_key =
        odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("keep"), &key) ? key : 0;
    lw->cap_cell = odin3_lower_limits_now().max_cell_nodes;
    lw->next_budget = ODIN3_LOWER_BUDGET_NODES;
    return ODIN3_OK;
}

void lw_ctx_free(lw_ctx *lw) {
    odin3_vec_free(&lw->created);
    odin3_vec_free(&lw->created_nets);
    odin3_vec_free(&lw->portnets);
    odin3_vec_free(&lw->merged);
    odin3_strbuf_free(&lw->namebuf);
    for (uint32_t i = 0; i < LW_REP_COUNT; i++) {
        odin3_strbuf_free(&lw->report[i]);
    }
}

/* The net on pin 0 (Y) of a constant node, none when it is open. */
static odin3_net_id const_node_net(const odin3_module *module, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    return pins.count > 0 ? odin3_pin_net(module, pins.first) : (odin3_net_id){0};
}

void lw_ctx_enter(lw_ctx *lw, odin3_module *module) {
    lw->module = module;
    lw->live_nodes = 0;
    memset(lw->const_slot, 0, sizeof lw->const_slot);
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        lw->live_nodes++;
        odin3_celltype_id type = odin3_node_type(module, node);
        for (uint32_t i = 0; i < LW_N_CONST; i++) {
            if (type.v == lw->constant[i].v && !odin3_net_valid(lw->const_slot[i].net)) {
                lw->const_slot[i].net = const_node_net(module, node);
            }
        }
    }
}

void lw_node_label(const lw_ctx *lw, odin3_node_id node, char *out, size_t size) {
    uint32_t name = odin3_node_name(lw->module, node);
    const char *text = name != 0 ? odin3_strtab_get(odin3_design_strtab(lw->design), name) : NULL;
    if (text != NULL) {
        (void)snprintf(out, size, "%s", text);
    } else {
        (void)snprintf(out, size, "$c%" PRIu32, node.v);
    }
}

/* --- fresh nets, gates, constants ----------------------------------------------------------- */

/* Room for one more node and one more net in the open cell's lists, so a push cannot fail. */
static odin3_status reserve_one(lw_ctx *lw) {
    odin3_status st = odin3_vec_reserve(&lw->created, lw->created.len + 1);
    return st == ODIN3_OK ? odin3_vec_reserve(&lw->created_nets, lw->created_nets.len + 1) : st;
}

odin3_status lw_net(lw_ctx *lw, odin3_net_id *out) {
    odin3_status st = reserve_one(lw);
    odin3_net_id net = {0};
    st = st == ODIN3_OK ? odin3_net_create(lw->module, 0, lw->prov, &net) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id *slot = odin3_vec_push(&lw->created_nets); /* reserved: never NULL */
    if (slot != NULL) {
        *slot = net;
    }
    *out = net;
    return ODIN3_OK;
}

/* The design-wide byte budget, every ODIN3_LOWER_BUDGET_NODES created nodes (spec §11). */
static odin3_status budget_tick(lw_ctx *lw) {
    lw->total_created++;
    if (lw->total_created < lw->next_budget) {
        return ODIN3_OK;
    }
    lw->next_budget = lw->total_created + ODIN3_LOWER_BUDGET_NODES;
    odin3_status st = odin3_budget_check(lw->pass, lw->design, 0);
    if (st != ODIN3_ERR_PARSE) {
        return st;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    odin3_status built = odin3_budget_message(lw->pass, &msg);
    if (built == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return built == ODIN3_OK ? ODIN3_ERR_PARSE : built;
}

/* Copies the word cell's `keep` onto a created node. */
static odin3_status copy_keep(lw_ctx *lw, odin3_node_id node) {
    if (lw->keep_value == NULL) {
        return ODIN3_OK;
    }
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    return odin3_attr_set(lw->module, obj, lw->keep_key, lw->keep_value);
}

/*
 * The name of the next gate of the open cell (PHASE3 #38): "<cell>.<op><i>", i counting the cell's
 * gates from 0, then "_1", "_2", ... until no node of the module has it. 0 (unnamed) when the word
 * cell has no name.
 */
static odin3_status gate_name(lw_ctx *lw, lw_kind kind, uint32_t *out) {
    *out = 0;
    uint32_t cell_name = odin3_node_name(lw->module, lw->cur);
    if (cell_name == 0) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(&lw->namebuf);
    odin3_status st = odin3_strbuf_appendf(
        &lw->namebuf, "%s.%s%" PRIu64, odin3_strtab_get(odin3_design_strtab(lw->design), cell_name),
        k_op_names[kind], lw->gates);
    size_t base_len = lw->namebuf.len;
    for (uint32_t tries = 1; st == ODIN3_OK; tries++) {
        st = odin3_design_intern(lw->design, odin3_bytes_cstr(lw->namebuf.data), out);
        if (st != ODIN3_OK || !odin3_node_valid(odin3_module_find_node(lw->module, *out))) {
            break;
        }
        lw->namebuf.len = base_len;
        lw->namebuf.data[base_len] = '\0';
        st = odin3_strbuf_appendf(&lw->namebuf, "_%" PRIu32, tries);
    }
    return st;
}

/* Creates the node of `type` named `name` (0: unnamed) on `ports` (inputs then the output). */
static odin3_status make_node(lw_ctx *lw, odin3_celltype_id type, uint32_t name,
                              const odin3_value *params, const odin3_netvec *ports) {
    odin3_node_spec spec = {type, name, lw->prov, params, params != NULL ? 1U : 0U};
    odin3_node_id node = {0};
    odin3_status st = odin3_node_create_connected(lw->module, &spec, ports, &node);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id *slot = odin3_vec_push(&lw->created); /* reserved: never NULL */
    if (slot != NULL) {
        *slot = node;
    }
    lw->live_nodes++;
    st = copy_keep(lw, node);
    return st == ODIN3_OK ? budget_tick(lw) : st;
}

odin3_status lw_gate(lw_ctx *lw, lw_kind kind, const odin3_net_id *in, uint32_t n_in,
                     odin3_net_id *out_net) {
    if (lw->gates >= lw->cap_cell) {
        char label[LW_LABEL_LEN];
        lw_node_label(lw, lw->cur, label, sizeof label);
        odin3_log(ODIN3_LOG_ERROR,
                  "lower: module '%s': '%s' would create more than %" PRIu64 " nodes (cap %" PRIu64
                  ")",
                  odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module)),
                  label, lw->cap_cell, lw->cap_cell);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id nets[4];
    for (uint32_t i = 0; i < n_in; i++) {
        nets[i] = in[i];
    }
    odin3_status st = lw_net(lw, &nets[n_in]);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_netvec ports[4];
    for (uint32_t i = 0; i <= n_in; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    uint32_t name = 0;
    st = gate_name(lw, kind, &name);
    st = st == ODIN3_OK ? make_node(lw, lw->gate[kind], name, NULL, ports) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    lw->gates++;
    *out_net = nets[n_in];
    return ODIN3_OK;
}

odin3_status lw_not(lw_ctx *lw, odin3_net_id src, odin3_net_id *out_net) {
    return lw_gate(lw, LW_G_NOT, &src, 1, out_net);
}

odin3_status lw_and(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net) {
    const odin3_net_id in[2] = {lhs, rhs};
    return lw_gate(lw, LW_G_AND, in, 2, out_net);
}

odin3_status lw_or(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net) {
    const odin3_net_id in[2] = {lhs, rhs};
    return lw_gate(lw, LW_G_OR, in, 2, out_net);
}

odin3_status lw_xor(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net) {
    const odin3_net_id in[2] = {lhs, rhs};
    return lw_gate(lw, LW_G_XOR, in, 2, out_net);
}

odin3_status lw_xnor(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net) {
    const odin3_net_id in[2] = {lhs, rhs};
    return lw_gate(lw, LW_G_XNOR, in, 2, out_net);
}

odin3_status lw_mux(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id sel,
                    odin3_net_id *out_net) {
    const odin3_net_id in[3] = {lhs, rhs, sel};
    return lw_gate(lw, LW_G_MUX, in, 3, out_net);
}

odin3_status lw_const(lw_ctx *lw, odin3_bit bit, odin3_net_id *out) {
    uint32_t idx = (uint32_t)bit;
    if (idx >= LW_N_CONST) {
        odin3_log(ODIN3_LOG_ERROR, "lower: const: %u is not 0, 1 or x", idx);
        return ODIN3_ERR_INVALID_ARG;
    }
    lw_const_slot *slot = &lw->const_slot[idx];
    if (odin3_net_valid(slot->net) && odin3_net_live(lw->module, slot->net) &&
        odin3_net_const_value(lw->module, slot->net) == k_const_values[idx]) {
        *out = slot->net;
        return ODIN3_OK;
    }
    odin3_net_id net = {0};
    odin3_status st = lw_net(lw, &net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_netvec port = {&net, 1};
    st = make_node(lw, lw->constant[idx], 0, NULL, &port);
    if (st != ODIN3_OK) {
        return st;
    }
    slot->net = net;
    slot->fresh = true;
    *out = net;
    return ODIN3_OK;
}

/* --- extension and the pairing tree --------------------------------------------------------- */

odin3_status lw_extend(lw_ctx *lw, lw_span in, uint32_t width, bool is_signed, lw_bits *out) {
    lw_bits_clear(out);
    odin3_status st = odin3_vec_reserve(&out->v, width);
    odin3_net_id fill = in.nets[in.n - 1];
    if (st == ODIN3_OK && !is_signed && in.n < width) {
        st = lw_const(lw, ODIN3_BIT_0, &fill);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        st = lw_bits_push(out, i < in.n ? in.nets[i] : fill);
    }
    return st;
}

odin3_status lw_tree(lw_ctx *lw, lw_kind kind, lw_span in, odin3_net_id *root) {
    if (in.n == 1) {
        *root = in.nets[0];
        return ODIN3_OK;
    }
    lw_bits level;
    lw_bits_init(&level);
    odin3_status st = odin3_vec_reserve(&level.v, in.n);
    for (uint32_t i = 0; st == ODIN3_OK && i < in.n; i++) {
        st = lw_bits_push(&level, in.nets[i]);
    }
    uint32_t count = in.n;
    while (st == ODIN3_OK && count > 1) {
        /* In place: the pair results land in front of the operands they read. */
        odin3_net_id *nets = level.v.data;
        uint32_t paired = count / 2;
        for (uint32_t i = 0; st == ODIN3_OK && i < paired; i++) {
            size_t pos = (size_t)i * 2;
            const odin3_net_id pair[2] = {nets[pos], nets[pos + 1]};
            st = lw_gate(lw, kind, pair, 2, &nets[i]);
        }
        if (st == ODIN3_OK && (count & 1U) != 0) {
            nets[paired] = nets[count - 1];
            paired++;
        }
        count = paired;
    }
    if (st == ODIN3_OK) {
        *root = ((const odin3_net_id *)level.v.data)[0];
    }
    lw_bits_free(&level);
    return st;
}

/* --- the both-signed rule ------------------------------------------------------------------- */

static bool has_param(const odin3_celltype_def *def, uint32_t idx, const char *name) {
    return def->n_params > idx && strcmp(def->params[idx].name, name) == 0;
}

/* WARN once per mixed cell (spec §3.3, §10) and the report entry. */
static void warn_mixed(lw_ctx *lw, const lw_cell *cell) {
    char label[LW_LABEL_LEN];
    lw_node_label(lw, cell->node, label, sizeof label);
    const char *module =
        odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module));
    odin3_log(ODIN3_LOG_WARN,
              "lower: module '%s': cell '%s' '%s' has A_SIGNED != B_SIGNED: treated as unsigned "
              "(both-signed rule, Yosys simlib; PHASE2 #25)",
              module, cell->def->name, label);
    lw->totals.mixed_sign++;
    lw_report_entry entry = {module, label, cell->def->name, 0, NULL};
    (void)lw_report_add(lw, LW_REP_MIXED_SIGN, &entry);
}

bool lw_both_signed(lw_ctx *lw, const lw_cell *cell) {
    const odin3_celltype_def *def = cell->def;
    if (!has_param(def, 0, "A_SIGNED")) {
        return false;
    }
    bool a_signed = cell->params[0].i != 0;
    if (!has_param(def, 1, "B_SIGNED")) {
        return a_signed; /* unary: A_SIGNED alone */
    }
    bool b_signed = cell->params[1].i != 0;
    if (a_signed != b_signed) {
        warn_mixed(lw, cell);
    }
    return a_signed && b_signed;
}
