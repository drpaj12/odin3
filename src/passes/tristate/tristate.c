/*
 * tristate.c — the `tristate` pass: helpers, the run over the modules and the conversion of
 * tri-state nets that reach no port into logic (see tristate.h).
 */
#include "passes/tristate/tristate.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The class of a conversion: the same function written as other word cells. */
static const odin3_opclass ts_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                       0};

enum { TS_LABEL_CAP = 160, TS_MSG_CAP = 512, TS_CONST_ZERO = 0, TS_CONST_X = 1 };

/* --- helpers -------------------------------------------------------------------------------- */

bool ts_is_tribuf(const ts_ctx *tc, odin3_node_id node) {
    return odin3_node_live(tc->module, node) &&
           odin3_node_type(tc->module, node).v == tc->types.tribuf.v;
}

static uint32_t port_index_of(const odin3_module *module, odin3_node_id node) {
    for (uint32_t i = 0; i < odin3_module_port_count(module); i++) {
        if (odin3_module_port(module, i).v == node.v) {
            return i;
        }
    }
    return 0;
}

/* Folds one pin of the net into the counts; false when the net is not ours to touch. */
typedef struct ts_scan {
    unsigned inouts, outs;
    uint32_t port;
} ts_scan;

static bool scan_pin(const ts_ctx *tc, odin3_pin_id pin, bool driver, ts_scan *scan) {
    const odin3_module *mod = tc->module;
    odin3_node_id node = odin3_pin_node(mod, pin);
    const odin3_celltype_def *def = odin3_celltype_get(tc->design, odin3_node_type(mod, node));
    if (def != NULL && def->gran == ODIN3_GRAN_PORT) {
        odin3_dir dir = odin3_pin_dir(mod, pin);
        if (dir == ODIN3_DIR_INOUT) {
            scan->inouts++;
            scan->port = port_index_of(mod, node);
        } else if (dir == ODIN3_DIR_IN) {
            scan->outs++;
        } else {
            return false;
        }
        return true;
    }
    return !driver || ts_is_tribuf(tc, node);
}

ts_net_info ts_classify(const ts_ctx *tc, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(tc->module, net);
    uint32_t drivers = odin3_net_driver_count(tc->module, net);
    ts_scan scan = {0, 0, 0};
    ts_net_info leave = {TS_NET_LEAVE, 0, false};
    ts_net_info foreign = {TS_NET_LEAVE, 0, true};
    for (uint32_t i = 0; i < pins.count; i++) {
        if (!scan_pin(tc, pins.pins[i], i < drivers, &scan)) {
            return foreign;
        }
    }
    if (scan.inouts == 0 && scan.outs == 0) {
        return (ts_net_info){TS_NET_INTERNAL, 0, false};
    }
    if (!tc->is_top || (scan.outs != 0 && scan.inouts != 0) || scan.inouts > 1U) {
        return leave;
    }
    if (scan.inouts == 0) {
        return (ts_net_info){TS_NET_OUT, 0, false};
    }
    return (ts_net_info){TS_NET_INOUT, scan.port, false};
}

void ts_net_label(const ts_ctx *tc, odin3_net_id net, char *out, size_t size) {
    const odin3_module *mod = tc->module;
    odin3_wirebit wb = odin3_net_primary(mod, net);
    if (wb.wire.v == 0) {
        (void)snprintf(out, size, "$n%" PRIu32, net.v);
        return;
    }
    const char *name =
        odin3_strtab_get(odin3_design_strtab(tc->design), odin3_wire_name(mod, wb.wire));
    if (odin3_wire_width(mod, wb.wire) == 1U) {
        (void)snprintf(out, size, "%s", name);
    } else {
        (void)snprintf(out, size, "%s[%d]", name, (int)odin3_wire_index(mod, wb.wire, wb.bit));
    }
}

static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *slot = user;
    if (slot->v == 0) {
        *slot = leaf;
    }
}

/* "file:line" of the node's first source leaf, else "no location". */
static void where_text(const ts_ctx *tc, odin3_node_id node, char *out, size_t size) {
    odin3_prov_id leaf = {0};
    odin3_prov_id prov = odin3_node_prov(tc->module, node);
    if (prov.v != 0) {
        (void)odin3_prov_sources(tc->design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(tc->design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        (void)snprintf(out, size, "%s:%" PRIu32,
                       odin3_strtab_get(odin3_design_strtab(tc->design), rec->locs[0].file),
                       rec->locs[0].line);
    } else {
        (void)snprintf(out, size, "no location");
    }
}

void ts_warn(const ts_ctx *tc, odin3_node_id at, const char *kind, const char *what,
             const char *text) {
    char place[TS_LABEL_CAP];
    where_text(tc, at, place, sizeof place);
    odin3_log(ODIN3_LOG_WARN, "%s: %s (%s): %s", kind, what, place, text);
}

odin3_status ts_const(ts_ctx *tc, unsigned which, odin3_net_id *out) {
    odin3_net_id *slot = &tc->konst[which];
    if (slot->v != 0 && odin3_net_live(tc->module, *slot)) {
        *out = *slot;
        return ODIN3_OK;
    }
    odin3_net_id net = {0};
    odin3_status st = odin3_net_create(tc->module, 0, odin3_module_prov(tc->module), &net);
    odin3_node_spec spec = {which == TS_CONST_ZERO ? tc->types.const0 : tc->types.constx, 0,
                            odin3_module_prov(tc->module), NULL, 0};
    odin3_netvec port = {&net, 1};
    st = st == ODIN3_OK ? odin3_node_create_connected(tc->module, &spec, &port, NULL) : st;
    if (st == ODIN3_OK) {
        *slot = net;
        *out = net;
    }
    return st;
}

odin3_status ts_make_node(ts_ctx *tc, const ts_make *mk, const odin3_node_id *from, size_t n_from) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < n_from; i++) {
        odin3_prov_id prov = odin3_node_prov(tc->module, from[i]);
        odin3_prov_id *slot = prov.v != 0 ? odin3_vec_push(&parents) : NULL;
        st = (prov.v != 0 && slot == NULL) ? ODIN3_ERR_NO_MEMORY : st;
        if (slot != NULL) {
            *slot = prov;
        }
    }
    odin3_prov_id derived = {0};
    if (st == ODIN3_OK && parents.len > 0) {
        odin3_prov_begin_op_class(tc->pass, &ts_class);
        st = odin3_prov_derive(tc->pass, (odin3_prov_list){parents.data, (uint32_t)parents.len},
                               &derived);
    }
    odin3_vec_free(&parents);
    odin3_node_spec spec = {mk->type, 0, derived, mk->params, mk->n_params};
    return st == ODIN3_OK ? odin3_node_create_connected(tc->module, &spec, mk->ports, NULL) : st;
}

/* --- internal nets -------------------------------------------------------------------------- */

typedef struct ts_work {
    odin3_vec recs;   /* ts_rec */
    odin3_vec nodes;  /* odin3_node_id: the cells to delete */
    odin3_vec merges; /* odin3_net_pair */
    odin3_vec buf;    /* odin3_net_id scratch */
    odin3_u64map *ynets;
} ts_work;

static int rec_cmp(const void *lhs, const void *rhs) {
    const ts_rec *left = lhs;
    const ts_rec *right = rhs;
    if (left->net.v != right->net.v) {
        return left->net.v < right->net.v ? -1 : 1;
    }
    if (left->node.v == right->node.v) {
        return 0;
    }
    return left->node.v < right->node.v ? -1 : 1;
}

static void work_free(ts_work *wk) {
    odin3_vec_free(&wk->recs);
    odin3_vec_free(&wk->nodes);
    odin3_vec_free(&wk->merges);
    odin3_vec_free(&wk->buf);
    odin3_u64map_destroy(wk->ynets);
}

/* Notes the Y nets of every live $tribuf (they must not feed another converted cell). */
static odin3_status note_ynets(const ts_ctx *tc, ts_work *wk) {
    uint32_t end = odin3_module_node_end(tc->module);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        if (!ts_is_tribuf(tc, node)) {
            continue;
        }
        odin3_pinslice ys = odin3_node_port(tc->module, node, (uint32_t)TS_SLOT_Y);
        for (uint32_t bit = 0; bit < ys.count; bit++) {
            odin3_net_id net = odin3_pin_net(tc->module, (odin3_pin_id){ys.first.v + bit});
            odin3_status st = odin3_u64map_put(wk->ynets, (odin3_kv){net.v, 1});
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_net_id ts_slot_net(const ts_ctx *tc, odin3_node_id node, ts_slot_at at) {
    odin3_pinslice pins = odin3_node_port(tc->module, node, (uint32_t)at.slot);
    return odin3_pin_net(tc->module, (odin3_pin_id){pins.first.v + at.bit});
}

/* An internal net, or a top output with at least two drivers (a single driver there is `lower`'s:
 * it drops the enable with its own WARNING). */
static bool net_converts(const ts_ctx *tc, odin3_net_id net) {
    ts_net_info info = ts_classify(tc, net);
    return info.cls == TS_NET_INTERNAL ||
           (info.cls == TS_NET_OUT && odin3_net_driver_count(tc->module, net) >= 2U);
}

/* True when every bit of the cell is on an internal net and its inputs are no tri-state output. */
static bool cell_converts(const ts_ctx *tc, const ts_work *wk, odin3_node_id node) {
    uint32_t width = odin3_node_port(tc->module, node, (uint32_t)TS_SLOT_Y).count;
    odin3_net_id en = ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_EN, 0});
    if (!odin3_net_valid(en) || odin3_u64map_get(wk->ynets, en.v, NULL)) {
        return false; /* a tri-state fed by a tri-state (a bidirectional switch) makes a loop */
    }
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id net = ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_Y, bit});
        odin3_net_id data = ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_A, bit});
        if (!odin3_net_valid(net) || !odin3_net_valid(data) || net.v == data.v ||
            odin3_u64map_get(wk->ynets, data.v, NULL) || !net_converts(tc, net)) {
            return false;
        }
    }
    return true;
}

/* A WARNING for a $tribuf left alone because another driver shares its net. */
static void say_left(const ts_ctx *tc, const ts_work *wk, odin3_node_id node) {
    uint32_t width = odin3_node_port(tc->module, node, (uint32_t)TS_SLOT_Y).count;
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id net = ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_Y, bit});
        odin3_net_id data = ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_A, bit});
        if (odin3_net_valid(net) && odin3_net_valid(data) &&
            odin3_u64map_get(wk->ynets, data.v, NULL)) {
            char label[TS_LABEL_CAP];
            ts_net_label(tc, net, label, sizeof label);
            ts_warn(tc, node, "tristate", label,
                    "not converted: its data is the output of another tri-state (a bidirectional "
                    "switch); converting would make a combinational loop");
            return;
        }
        if (odin3_net_valid(net) && ts_classify(tc, net).foreign) {
            char label[TS_LABEL_CAP];
            ts_net_label(tc, net, label, sizeof label);
            ts_warn(tc, node, "tristate", label,
                    "not converted: another driver that is not a tri-state (an instance output, a "
                    "gate or a port) shares the net; `lower` will refuse it");
            return;
        }
    }
}

static odin3_status add_cell(const ts_ctx *tc, ts_work *wk, odin3_node_id node) {
    uint32_t width = odin3_node_port(tc->module, node, (uint32_t)TS_SLOT_Y).count;
    odin3_node_id *dead = odin3_vec_push(&wk->nodes);
    if (dead == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *dead = node;
    for (uint32_t bit = 0; bit < width; bit++) {
        ts_rec *rec = odin3_vec_push(&wk->recs);
        if (rec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *rec = (ts_rec){ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_Y, bit}),
                        ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_A, bit}),
                        ts_slot_net(tc, node, (ts_slot_at){TS_SLOT_EN, 0}), node};
    }
    return ODIN3_OK;
}

static odin3_status collect(const ts_ctx *tc, ts_work *wk) {
    odin3_status st = note_ynets(tc, wk);
    uint32_t end = odin3_module_node_end(tc->module);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (ts_is_tribuf(tc, node) && cell_converts(tc, wk, node)) {
            st = add_cell(tc, wk, node);
        } else if (ts_is_tribuf(tc, node)) {
            say_left(tc, wk, node);
        }
    }
    if (st == ODIN3_OK && wk->recs.len > 1) {
        qsort(wk->recs.data, wk->recs.len, sizeof(ts_rec), rec_cmp);
    }
    return st;
}

/* Fills buf with the data nets of the drivers, then their enables, the last driver first, and
 * nodes with the driver cells. */
static odin3_status pmux_inputs(const ts_rec *recs, uint32_t ndrv, odin3_vec *buf,
                                odin3_vec *nodes) {
    for (uint32_t pass = 0; pass < 2U; pass++) {
        for (uint32_t i = ndrv; i > 0; i--) {
            odin3_net_id *slot = odin3_vec_push(buf);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = pass == 0 ? recs[i - 1U].data : recs[i - 1U].enable;
        }
    }
    for (uint32_t i = 0; i < ndrv; i++) {
        odin3_node_id *slot = odin3_vec_push(nodes);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = recs[i].node;
    }
    return ODIN3_OK;
}

/* ndrv >= 2 drivers: a $pmux, A = x, the last driver first (as elab2_trilogic.c). */
odin3_status ts_pmux(ts_ctx *tc, const ts_rec *recs, uint32_t ndrv, odin3_net_id ynet) {
    odin3_net_id xnet = {0};
    odin3_status st = ts_const(tc, TS_CONST_X, &xnet);
    odin3_vec buf;
    odin3_vec nodes;
    odin3_vec_init(&buf, sizeof(odin3_net_id));
    odin3_vec_init(&nodes, sizeof(odin3_node_id));
    st = st == ODIN3_OK ? pmux_inputs(recs, ndrv, &buf, &nodes) : st;
    odin3_value params[2] = {odin3_value_int(1), odin3_value_int((int64_t)ndrv)};
    const odin3_net_id *nets = buf.data;
    odin3_netvec ports[4] = {{&xnet, 1}, {nets, ndrv}, {nets + ndrv, ndrv}, {&ynet, 1}};
    ts_make mk = {tc->types.pmux, params, 2, ports};
    st = st == ODIN3_OK ? ts_make_node(tc, &mk, nodes.data, nodes.len) : st;
    odin3_vec_free(&buf);
    odin3_vec_free(&nodes);
    return st;
}

/* *out = the OR of the enables of ndrv >= 1 drivers: a chain of $_OR_ gates. */
odin3_status ts_or_enables(ts_ctx *tc, const ts_rec *recs, uint32_t ndrv, odin3_net_id *out) {
    odin3_net_id acc = recs[0].enable;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < ndrv; i++) {
        odin3_net_id next = {0};
        st = odin3_net_create(tc->module, 0, odin3_module_prov(tc->module), &next);
        odin3_net_id pair[2] = {acc, recs[i].enable};
        odin3_netvec ports[3] = {{&pair[0], 1}, {&pair[1], 1}, {&next, 1}};
        ts_make mk = {tc->types.or_gate, NULL, 0, ports};
        st = st == ODIN3_OK ? ts_make_node(tc, &mk, &recs[i].node, 1) : st;
        acc = next;
    }
    *out = acc;
    return st;
}

static void warn_net(const ts_ctx *tc, const ts_rec *rec, uint32_t ndrv) {
    char label[TS_LABEL_CAP];
    char text[TS_MSG_CAP];
    ts_net_label(tc, rec->net, label, sizeof label);
    if (ndrv == 1U) {
        (void)snprintf(text, sizeof text,
                       "1 driver converted to logic (the enable is dropped and the net follows the "
                       "data, as Yosys opt_expr -mux_undef and Parmys do)");
    } else {
        (void)snprintf(text, sizeof text,
                       "%" PRIu32 " drivers converted to logic (no driver enabled: x; the enables "
                       "are not proved exclusive: two enabled at once is a bus conflict and gives "
                       "the OR of the enabled data)",
                       ndrv);
    }
    ts_warn(tc, rec->node, "tristate", label, text);
}

static odin3_status emit_run(ts_ctx *tc, ts_work *wk, const ts_rec *recs, uint32_t ndrv) {
    warn_net(tc, &recs[0], ndrv);
    tc->converted++;
    if (ndrv >= 2U) {
        return ts_pmux(tc, recs, ndrv, recs[0].net);
    }
    odin3_net_pair *pair = odin3_vec_push(&wk->merges);
    if (pair == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *pair = (odin3_net_pair){recs[0].data, recs[0].net};
    return ODIN3_OK;
}

static odin3_status apply(ts_ctx *tc, ts_work *wk) {
    odin3_status st = ODIN3_OK;
    const odin3_node_id *dead = wk->nodes.data;
    for (size_t i = 0; st == ODIN3_OK && i < wk->nodes.len; i++) {
        st = odin3_node_delete(tc->module, dead[i]);
    }
    const odin3_net_pair *pairs = wk->merges.data;
    for (size_t i = 0; st == ODIN3_OK && i < wk->merges.len; i++) {
        st = odin3_net_merge(tc->module, pairs[i]);
    }
    return st;
}

odin3_status ts_convert_internal(ts_ctx *tc) {
    ts_work wk = {.ynets = odin3_u64map_create(0)};
    odin3_vec_init(&wk.recs, sizeof(ts_rec));
    odin3_vec_init(&wk.nodes, sizeof(odin3_node_id));
    odin3_vec_init(&wk.merges, sizeof(odin3_net_pair));
    odin3_vec_init(&wk.buf, sizeof(odin3_net_id));
    odin3_status st = wk.ynets != NULL ? collect(tc, &wk) : ODIN3_ERR_NO_MEMORY;
    const ts_rec *recs = wk.recs.data;
    if (st == ODIN3_OK && tc->opts->strict && wk.recs.len > 0) {
        char label[TS_LABEL_CAP];
        ts_net_label(tc, recs[0].net, label, sizeof label);
        odin3_log(
            ODIN3_LOG_ERROR,
            "tristate: net '%s': internal tri-state nets are not supported with --strict (D3)",
            label);
        st = ODIN3_ERR_INVALID_ARG;
    }
    for (size_t i = 0; st == ODIN3_OK && i < wk.recs.len;) {
        size_t j = i + 1U;
        while (j < wk.recs.len && recs[j].net.v == recs[i].net.v) {
            j++;
        }
        st = emit_run(tc, &wk, recs + i, (uint32_t)(j - i));
        i = j;
    }
    st = st == ODIN3_OK ? apply(tc, &wk) : st;
    work_free(&wk);
    return st;
}

/* --- the run -------------------------------------------------------------------------------- */

static odin3_celltype_id type_named(const odin3_design *design, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, &id)) {
        return (odin3_celltype_id){0};
    }
    return id;
}

static odin3_status find_types(const odin3_design *design, ts_types *types) {
    types->tribuf = type_named(design, "$tribuf");
    types->pmux = type_named(design, "$pmux");
    types->or_gate = type_named(design, "$_OR_");
    types->const0 = type_named(design, "$_CONST0_");
    types->constx = type_named(design, "$_CONSTX_");
    if (types->tribuf.v == 0 || types->pmux.v == 0 || types->or_gate.v == 0 ||
        types->const0.v == 0 || types->constx.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "tristate: a cell type it needs is not registered");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static bool flattened_away(const odin3_design *design, const odin3_module *module) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(ODIN3_ATTR_FLATTENED_AWAY),
                           &key)) {
        return false;
    }
    odin3_objref self = {ODIN3_OBJ_MODULE, odin3_module_id_of(module).v};
    return odin3_attr_get(module, self, key) != NULL;
}

static bool has_tribuf(const ts_ctx *tc) {
    uint32_t end = odin3_module_node_end(tc->module);
    for (uint32_t id = 1; id < end; id++) {
        if (ts_is_tribuf(tc, (odin3_node_id){id})) {
            return true;
        }
    }
    return false;
}

odin3_status ts_run(odin3_pass_ctx *pass, odin3_design *design, const ts_opts *opts) {
    ts_ctx tc = {.pass = pass, .design = design, .opts = opts};
    odin3_status st = find_types(design, &tc.types);
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        tc.module = odin3_module_get(design, (odin3_module_id){mid});
        tc.konst[0] = tc.konst[1] = (odin3_net_id){0};
        tc.is_top = odin3_design_top(design).v == mid;
        if (flattened_away(design, tc.module)) {
            continue;
        }
        st = ts_split_inouts(&tc);
        st = st == ODIN3_OK && has_tribuf(&tc) ? ts_convert_internal(&tc) : st;
    }
    if (st == ODIN3_OK && tc.converted + tc.split > 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "tristate: %" PRIu64 " tri-state nets converted to logic, %" PRIu64
                  " inout ports split",
                  tc.converted, tc.split);
    }
    return st;
}
