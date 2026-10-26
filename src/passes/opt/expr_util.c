/*
 * expr_util.c — what the opt_expr rules share (2F plan Task 6): reading the visited cell, signals
 * in the visit's arena, helper cells, and the rewrites, each of which goes through the kit
 * (odin3_kit_connect, odin3_kit_new_cell, odin3_kit_rebuild_node) after one
 * odin3_kit_begin_rewrite.
 */
#include "passes/opt/expr.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { ARENA_CHUNK = 4096, MAX_PORTS = 8, UNARY_PARAMS = 3, BINARY_PARAMS = 5 };

/* --- reading the visited cell ----------------------------------------------------------------- */

uint32_t odin3_expr_width(const odin3_expr_ctx *ec, odin3_expr_port port) {
    return odin3_node_port(ec->module, ec->node, port.v).count;
}

uint32_t odin3_expr_y_width(const odin3_expr_ctx *ec) {
    return odin3_expr_width(ec, odin3_expr_y_port(ec));
}

odin3_net_id odin3_expr_net(const odin3_expr_ctx *ec, odin3_expr_port port, uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(ec->module, ec->node, port.v);
    if (bit >= pins.count) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(ec->module, (odin3_pin_id){pins.first.v + bit});
}

odin3_const odin3_expr_cval(const odin3_expr_ctx *ec, odin3_net_id net) {
    return odin3_net_valid(net) ? odin3_net_const_value(ec->module, net) : ODIN3_CONST_NONE;
}

int64_t odin3_expr_param(const odin3_expr_ctx *ec, uint32_t index) {
    const odin3_value *val = odin3_node_param(ec->module, ec->node, index);
    return val != NULL ? val->i : 0;
}

odin3_expr_port odin3_expr_y_port(const odin3_expr_ctx *ec) {
    /* the one output of every type opt_expr rewrites is its last port */
    return ODIN3_EXPR_PORT(ec->def->n_ports - 1);
}

bool odin3_expr_same(const odin3_expr_ctx *ec, odin3_net_id lhs, odin3_net_id rhs) {
    if (lhs.v == rhs.v) {
        return true;
    }
    odin3_const value = odin3_expr_cval(ec, lhs);
    return value != ODIN3_CONST_NONE && value == odin3_expr_cval(ec, rhs);
}

bool odin3_expr_is_undef(const odin3_expr_ctx *ec, odin3_net_id net) {
    odin3_const value = odin3_expr_cval(ec, net);
    return value == ODIN3_CONST_X || value == ODIN3_CONST_Z;
}

static void census_add(odin3_expr_census *census, odin3_const value) {
    census->n++;
    switch (value) {
    case ODIN3_CONST_0:
        census->zero++;
        break;
    case ODIN3_CONST_1:
        census->one++;
        break;
    case ODIN3_CONST_X:
        census->xs++;
        break;
    case ODIN3_CONST_Z:
        census->zs++;
        break;
    case ODIN3_CONST_NONE:
    default:
        census->other++;
        break;
    }
}

bool odin3_expr_both_signed(const odin3_expr_ctx *ec) {
    return odin3_opt_both_signed(ec->module, ec->node);
}

odin3_expr_census odin3_expr_port_census(const odin3_expr_ctx *ec, odin3_expr_port port) {
    odin3_expr_census census = {0};
    odin3_pinslice pins = odin3_node_port(ec->module, ec->node, port.v);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(ec->module, (odin3_pin_id){pins.first.v + k});
        census_add(&census, odin3_expr_cval(ec, net));
    }
    return census;
}

odin3_expr_census odin3_expr_sig_census(const odin3_expr_ctx *ec, const odin3_expr_sig *sig) {
    odin3_expr_census census = {0};
    for (uint32_t k = 0; k < sig->n; k++) {
        census_add(&census, odin3_expr_cval(ec, sig->net[k]));
    }
    return census;
}

bool odin3_expr_fully_def(odin3_expr_census census) {
    return census.n > 0 && census.other == 0 && census.xs == 0 && census.zs == 0;
}

bool odin3_expr_fully_undef(odin3_expr_census census) {
    return census.n > 0 && census.other == 0 && census.zero == 0 && census.one == 0;
}

bool odin3_expr_fully_const(odin3_expr_census census, bool keepdc) {
    return census.n > 0 && census.other == 0 && (!keepdc || !odin3_expr_fully_undef(census));
}

bool odin3_expr_fully_zero(odin3_expr_census census) {
    return census.n > 0 && census.zero == census.n;
}

/* --- signals ---------------------------------------------------------------------------------- */

void *odin3_expr_alloc(odin3_expr_ctx *ec, size_t bytes) {
    if (ec->arena == NULL) {
        ec->arena = odin3_arena_create(ARENA_CHUNK);
    }
    return ec->arena != NULL ? odin3_arena_alloc(ec->arena, bytes) : NULL;
}

odin3_status odin3_expr_sig_new(odin3_expr_ctx *ec, uint32_t n, odin3_expr_sig *out) {
    out->net = odin3_expr_alloc(ec, sizeof *out->net * ((size_t)n + 1));
    out->n = n;
    return out->net != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

odin3_status odin3_expr_read(odin3_expr_ctx *ec, odin3_expr_port port, odin3_expr_sig *out) {
    odin3_pinslice pins = odin3_node_port(ec->module, ec->node, port.v);
    odin3_status st = odin3_expr_sig_new(ec, pins.count, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        out->net[k] = odin3_pin_net(ec->module, (odin3_pin_id){pins.first.v + k});
    }
    return st;
}

odin3_status odin3_expr_one(odin3_expr_ctx *ec, odin3_net_id net, odin3_expr_sig *out) {
    odin3_status st = odin3_expr_sig_new(ec, 1, out);
    if (st == ODIN3_OK) {
        out->net[0] = net;
    }
    return st;
}

odin3_status odin3_expr_fill(odin3_expr_ctx *ec, odin3_bit bit, uint32_t n, odin3_expr_sig *out) {
    odin3_net_id net = {0};
    odin3_status st = odin3_opt_const_net(ec->oc, bit, &net);
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, n, out) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < n; k++) {
        out->net[k] = net;
    }
    return st;
}

odin3_status odin3_expr_extend(odin3_expr_ctx *ec, const odin3_expr_sig *in, uint32_t width,
                               bool is_signed, odin3_expr_sig *out) {
    odin3_net_id pad = {0};
    odin3_status st = ODIN3_OK;
    if (width > in->n) {
        if (is_signed && in->n > 0) {
            pad = in->net[in->n - 1];
        } else {
            st = odin3_opt_const_net(ec->oc, ODIN3_BIT_0, &pad);
        }
    }
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, width, out) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        out->net[k] = k < in->n ? in->net[k] : pad;
    }
    return st;
}

odin3_expr_sig odin3_expr_slice(const odin3_expr_sig *in, uint32_t from, uint32_t n) {
    /* NULL + 0 is undefined: an empty signal may have no array (UBSan). */
    return (odin3_expr_sig){in->net != NULL ? in->net + from : NULL, n};
}

odin3_status odin3_expr_join(odin3_expr_ctx *ec, const odin3_expr_sig *low,
                             const odin3_expr_sig *high, odin3_expr_sig *out) {
    odin3_status st = odin3_expr_sig_new(ec, low->n + high->n, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < low->n + high->n; k++) {
        out->net[k] = k < low->n ? low->net[k] : high->net[k - low->n];
    }
    return st;
}

/* --- marks ------------------------------------------------------------------------------------ */

odin3_status odin3_expr_mark_begin(odin3_expr_ctx *ec) {
    size_t end = odin3_module_net_end(ec->module);
    if (ec->marks.len < end) {
        odin3_status st = odin3_vec_reserve(&ec->marks, end);
        while (st == ODIN3_OK && ec->marks.len < end) {
            st = odin3_vec_push(&ec->marks) != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (++ec->gen == 0) { /* wrapped: forget every old mark */
        if (ec->marks.len > 0) {
            memset(ec->marks.data, 0, ec->marks.len * sizeof(uint32_t));
        }
        ec->gen = 1;
    }
    return ODIN3_OK;
}

void odin3_expr_mark(odin3_expr_ctx *ec, odin3_net_id net) {
    if (net.v < ec->marks.len) {
        *(uint32_t *)odin3_vec_at(&ec->marks, net.v) = ec->gen;
    }
}

bool odin3_expr_marked(const odin3_expr_ctx *ec, odin3_net_id net) {
    return net.v < ec->marks.len && *(const uint32_t *)odin3_vec_cat(&ec->marks, net.v) == ec->gen;
}

/* --- building cells ----------------------------------------------------------------------------
 */

odin3_status odin3_expr_begin(odin3_expr_ctx *ec) {
    if (ec->began) {
        return ODIN3_OK;
    }
    odin3_kit_begin_rewrite(ec->oc);
    ec->began = true;
    return odin3_kit_prov(ec->oc, &ec->node, 1, NULL, &ec->prov);
}

/* A spec turned into the kit's cell: parameters as values, one netvec per port. */
typedef struct built {
    odin3_value params[ODIN3_EXPR_MAX_PARAMS];
    odin3_netvec ports[MAX_PORTS];
    odin3_kit_cell cell;
    const odin3_celltype_def *def;
} built;

static odin3_status build_spec(const odin3_expr_ctx *ec, const odin3_expr_cell *spec, built *out) {
    odin3_celltype_id type = ec->type[spec->type];
    out->def = odin3_celltype_get(ec->oc->design, type);
    if (out->def == NULL || out->def->n_ports > MAX_PORTS || out->def->n_params != spec->n_params) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < spec->n_params; i++) {
        out->params[i] = odin3_value_int(spec->params[i]);
    }
    uint32_t input = 0;
    for (uint32_t i = 0; i < out->def->n_ports; i++) {
        out->ports[i] = (odin3_netvec){NULL, 0};
        if (out->def->ports[i].dir == ODIN3_DIR_IN) {
            const odin3_expr_sig *sig = input < ODIN3_EXPR_MAX_IN ? spec->in[input] : NULL;
            out->ports[i] = sig != NULL ? (odin3_netvec){sig->net, sig->n} : out->ports[i];
            input++;
        }
    }
    out->cell = (odin3_kit_cell){type, spec->n_params > 0 ? out->params : NULL, spec->n_params,
                                 out->ports, ec->prov};
    return ODIN3_OK;
}

/* The nets of node's last port (its output) in the visit's arena. */
static odin3_status read_output(odin3_expr_ctx *ec, odin3_node_id node, odin3_expr_sig *out) {
    uint32_t last =
        odin3_celltype_get(ec->oc->design, odin3_node_type(ec->module, node))->n_ports - 1;
    odin3_pinslice pins = odin3_node_port(ec->module, node, last);
    odin3_status st = odin3_expr_sig_new(ec, pins.count, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        out->net[k] = odin3_pin_net(ec->module, (odin3_pin_id){pins.first.v + k});
    }
    return st;
}

odin3_status odin3_expr_new_cell(odin3_expr_ctx *ec, const odin3_expr_cell *spec,
                                 odin3_node_id *node, odin3_expr_sig *out) {
    built cell;
    odin3_status st = odin3_expr_begin(ec);
    st = st == ODIN3_OK ? build_spec(ec, spec, &cell) : st;
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? odin3_kit_new_cell(ec->oc, &cell.cell, &made) : st;
    if (st == ODIN3_OK && out != NULL) {
        st = read_output(ec, made, out);
    }
    if (st == ODIN3_OK && node != NULL) {
        *node = made;
    }
    return st;
}

static bool is_gate(odin3_expr_type type) {
    return type == ODIN3_EXPR_T_G_NOT || type == ODIN3_EXPR_T_G_AND || type == ODIN3_EXPR_T_G_OR ||
           type == ODIN3_EXPR_T_G_XOR || type == ODIN3_EXPR_T_G_XNOR || type == ODIN3_EXPR_T_G_MUX;
}

odin3_status odin3_expr_new_un(odin3_expr_ctx *ec, odin3_expr_type type, const odin3_expr_sig *src,
                               uint32_t y_width, odin3_expr_sig *out) {
    odin3_expr_cell spec = {.type = type, .in = {src}};
    if (!is_gate(type)) {
        spec.n_params = UNARY_PARAMS;
        spec.params[ODIN3_EXPR_P_UN_AW] = src->n;
        spec.params[ODIN3_EXPR_P_UN_YW] = y_width;
    }
    return odin3_expr_new_cell(ec, &spec, NULL, out);
}

odin3_status odin3_expr_new_bin(odin3_expr_ctx *ec, odin3_expr_type type, const odin3_expr_sig *lhs,
                                const odin3_expr_sig *rhs, odin3_expr_sig *out) {
    odin3_expr_cell spec = {.type = type, .in = {lhs, rhs}};
    if (!is_gate(type)) {
        spec.n_params = BINARY_PARAMS;
        spec.params[ODIN3_EXPR_P_BIN_AW] = lhs->n;
        spec.params[ODIN3_EXPR_P_BIN_BW] = rhs->n;
        spec.params[ODIN3_EXPR_P_BIN_YW] = lhs->n > rhs->n ? lhs->n : rhs->n;
    }
    return odin3_expr_new_cell(ec, &spec, NULL, out);
}

/* --- the rewrites ------------------------------------------------------------------------------
 */

static void fired(odin3_expr_ctx *ec, odin3_opt_rule rule, bool all_const) {
    ec->fired = true;
    ec->rule = rule;
    ec->all_const = all_const;
}

void odin3_expr_count(odin3_expr_ctx *ec, odin3_opt_rule rule) {
    ec->oc->counts->expr.rule[rule]++;
}

/* Y = src zero-extended or cut to Y's width, through kit_connect (the cell goes). */
odin3_status odin3_expr_wire(odin3_expr_ctx *ec, odin3_opt_rule rule, const odin3_expr_sig *src) {
    odin3_expr_port port = odin3_expr_y_port(ec);
    uint32_t width = odin3_expr_width(ec, port);
    odin3_expr_sig full;
    odin3_status st = odin3_expr_extend(ec, src, width, false, &full);
    st = st == ODIN3_OK ? odin3_expr_begin(ec) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    bool all_const = true;
    for (uint32_t k = 0; k < width; k++) {
        all_const = all_const && odin3_expr_cval(ec, full.net[k]) != ODIN3_CONST_NONE;
    }
    odin3_kit_conn conn = {ec->node, port.v, full.net, width};
    st = odin3_kit_connect(ec->oc, &conn);
    if (st == ODIN3_OK) {
        fired(ec, rule, all_const);
    }
    return st;
}

odin3_status odin3_expr_wire_net(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_net_id net) {
    odin3_expr_sig sig;
    odin3_status st = odin3_expr_one(ec, net, &sig);
    return st == ODIN3_OK ? odin3_expr_wire(ec, rule, &sig) : st;
}

odin3_status odin3_expr_wire_bit(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_bit bit) {
    odin3_net_id net = {0};
    odin3_status st = odin3_opt_const_net(ec->oc, bit, &net);
    return st == ODIN3_OK ? odin3_expr_wire_net(ec, rule, net) : st;
}

odin3_status odin3_expr_rebuild(odin3_expr_ctx *ec, odin3_opt_rule rule,
                                const odin3_expr_cell *spec) {
    built cell;
    odin3_status st = odin3_expr_begin(ec);
    st = st == ODIN3_OK ? build_spec(ec, spec, &cell) : st;
    odin3_node_id made = {0};
    if (st == ODIN3_OK) {
        odin3_kit_rebuild req = {ec->node, cell.cell};
        st = odin3_kit_rebuild_node(ec->oc, &req, &made);
    }
    if (st == ODIN3_OK) {
        ec->made = made;
        fired(ec, rule, false);
    }
    return st;
}

odin3_status odin3_expr_wire_keep(odin3_expr_ctx *ec, odin3_opt_rule rule,
                                  const odin3_expr_sig *src, odin3_node_id fresh) {
    uint32_t name = odin3_node_name(ec->module, ec->node);
    odin3_status st = odin3_kit_copy_attrs(ec->module, ec->node, fresh);
    st = st == ODIN3_OK ? odin3_expr_wire(ec, rule, src) : st;
    /* the visited cell's name left the map with it, so this re-insertion does not grow the map */
    if (st == ODIN3_OK && name != 0) {
        st = odin3_node_rename(ec->module, fresh, name);
    }
    return st;
}
