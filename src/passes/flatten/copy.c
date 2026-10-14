/*
 * copy.c — copying a child's nodes and wires into the root (3A spec §3.3 steps 5-7, §3.5, §9) and
 * the checks after the joins (§3.7).
 */
#include "passes/flatten/flatten_int.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "passes/budget.h"
#include "util/alloc.h"
#include "util/log.h"

#include <string.h>

/* "blif_name" names a port bit; the copy is no longer a port (spec §3.5). */
#define FLAT_SKIP_ATTR "blif_name"

typedef struct attr_copy {
    odin3_flatten_ctx *fc;
    odin3_objref dst;
} attr_copy;

static odin3_status copy_one_attr(void *user, uint32_t key, const odin3_value *value) {
    attr_copy *job = user;
    const char *text = odin3_strtab_get(odin3_design_strtab(job->fc->design), key);
    if (strcmp(text, FLAT_SKIP_ATTR) == 0) {
        return ODIN3_OK;
    }
    return odin3_attr_set(job->fc->root, job->dst, key, value);
}

typedef struct attr_move {
    odin3_objref from, to;
} attr_move;

static odin3_status copy_attrs(odin3_flatten_ctx *fc, attr_move move) {
    attr_copy job = {fc, move.to};
    return odin3_attr_foreach(fc->child, move.from, copy_one_attr, &job);
}

/* The index of the MEMID parameter of def, -1 when it has none. */
static int memid_index(const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, "MEMID") == 0 && def->params[i].kind == ODIN3_VAL_STRING) {
            return (int)i;
        }
    }
    return -1;
}

/* The parameters of node, MEMID rewritten, in fc->tmp_vals. */
static odin3_status copy_params(odin3_flatten_ctx *fc, odin3_node_id node,
                                const odin3_celltype_def *def) {
    odin3_vec_clear(&fc->tmp_vals);
    int memid = memid_index(def);
    for (uint32_t i = 0; i < def->n_params; i++) {
        odin3_value *val = odin3_vec_push(&fc->tmp_vals);
        if (val == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *val = *odin3_node_param(fc->child, node, i);
        if (memid >= 0 && i == (uint32_t)memid) {
            odin3_status st = odin3_flatten_memid(fc, val->str, &val->str);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* The mapped nets of every port of node, in fc->tmp_nets and (as slices) fc->tmp_ports. */
static odin3_status gather_ports(odin3_flatten_ctx *fc, odin3_node_id node,
                                 const odin3_celltype_def *def) {
    odin3_vec_clear(&fc->tmp_nets);
    odin3_vec_clear(&fc->tmp_ports);
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice slice = odin3_node_port(fc->child, node, port);
        for (uint32_t bit = 0; bit < slice.count; bit++) {
            odin3_net_id *net = odin3_vec_push(&fc->tmp_nets);
            if (net == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            odin3_pin_id pin = {slice.first.v + bit};
            *net = odin3_flatten_mapped(fc, odin3_pin_net(fc->child, pin));
        }
    }
    const odin3_net_id *nets = fc->tmp_nets.data;
    uint32_t at = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_netvec *vec = odin3_vec_push(&fc->tmp_ports);
        if (vec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *vec = (odin3_netvec){nets + at, odin3_node_port(fc->child, node, port).count};
        at += vec->count;
    }
    return ODIN3_OK;
}

static odin3_status budget_tick(odin3_flatten_ctx *fc) {
    if (++fc->created % FLAT_BUDGET_EVERY != 0) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_budget_check(fc->pass, fc->design, 0);
    if (st != ODIN3_OK) {
        odin3_strbuf msg;
        odin3_strbuf_init(&msg);
        if (odin3_budget_message(fc->pass, &msg) == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "flatten: %s", msg.data);
        }
        odin3_strbuf_free(&msg);
    }
    return st;
}

/* A new module-type node joins the worklist, with the path data its expansion needs. */
static odin3_status queue_instance(odin3_flatten_ctx *fc, odin3_node_id copy, odin3_node_id orig) {
    flat_work *item = odin3_vec_push(&fc->work);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *item = (flat_work){copy.v, fc->path_str, odin3_node_name(fc->child, orig),
                        odin3_node_prov(fc->child, orig)};
    return ODIN3_OK;
}

static odin3_status copy_node(odin3_flatten_ctx *fc, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fc->design, odin3_node_type(fc->child, node));
    odin3_node_spec spec = {odin3_node_type(fc->child, node), 0, {0}, NULL, def->n_params};
    odin3_status st =
        odin3_flatten_name(fc, (flat_key){FLAT_NODE, odin3_node_name(fc->child, node)}, &spec.name);
    st = st == ODIN3_OK ? copy_params(fc, node, def) : st;
    st = st == ODIN3_OK ? gather_ports(fc, node, def) : st;
    st = st == ODIN3_OK ? odin3_flatten_derive(fc, odin3_node_prov(fc->child, node), &spec.prov)
                        : st;
    spec.params = fc->tmp_vals.data;
    odin3_node_id copy = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(fc->root, &spec, fc->tmp_ports.data, &copy)
                        : st;
    st = st == ODIN3_OK
             ? copy_attrs(fc, (attr_move){{ODIN3_OBJ_NODE, node.v}, {ODIN3_OBJ_NODE, copy.v}})
             : st;
    if (st == ODIN3_OK && def->gran == ODIN3_GRAN_MODULE) {
        st = queue_instance(fc, copy, node);
    }
    fc->nodes++;
    return st == ODIN3_OK ? budget_tick(fc) : st;
}

odin3_status odin3_flatten_copy_nodes(odin3_flatten_ctx *fc) {
    for (uint32_t nid = 1; nid < odin3_module_node_end(fc->child); nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(fc->child, node)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(fc->child, node);
        const odin3_celltype_def *def = odin3_celltype_get(fc->design, type);
        if (def->gran == ODIN3_GRAN_PORT || odin3_flatten_graph_is_const(fc->graph, type)) {
            continue;
        }
        odin3_status st = copy_node(fc, node);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status copy_wire(odin3_flatten_ctx *fc, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(fc->child, wire);
    odin3_vec_clear(&fc->tmp_nets);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id *net = odin3_vec_push(&fc->tmp_nets);
        if (net == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *net = odin3_flatten_mapped(fc, odin3_wire_net(fc->child, wire, bit));
    }
    odin3_wire_spec spec = {0,
                            odin3_wire_msb(fc->child, wire),
                            odin3_wire_lsb(fc->child, wire),
                            odin3_wire_signed(fc->child, wire),
                            {0}};
    odin3_status st =
        odin3_flatten_name(fc, (flat_key){FLAT_WIRE, odin3_wire_name(fc->child, wire)}, &spec.name);
    st = st == ODIN3_OK ? odin3_flatten_derive(fc, odin3_wire_prov(fc->child, wire), &spec.prov)
                        : st;
    odin3_wire_id copy = {0};
    st = st == ODIN3_OK ? odin3_wire_create(fc->root, &spec, fc->tmp_nets.data, &copy) : st;
    return st == ODIN3_OK
               ? copy_attrs(fc, (attr_move){{ODIN3_OBJ_WIRE, wire.v}, {ODIN3_OBJ_WIRE, copy.v}})
               : st;
}

odin3_status odin3_flatten_copy_wires(odin3_flatten_ctx *fc) {
    for (uint32_t wid = 1; wid < odin3_module_wire_end(fc->child); wid++) {
        if (!odin3_wire_live(fc->child, (odin3_wire_id){wid})) {
            continue;
        }
        odin3_status st = copy_wire(fc, (odin3_wire_id){wid});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the checks of spec §3.7 ------------------------------------------------------------------ */

/* The drivers of net that are not bus drivers (tristate or inout pins), and whether any of the
 * drivers is a constant node. */
static uint32_t hard_drivers(const odin3_flatten_ctx *fc, odin3_net_id net, bool *constant) {
    uint32_t count = 0;
    odin3_pinlist pins = odin3_net_pins(fc->root, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = pins.pins[i];
        if (!odin3_pin_drives(fc->root, pin)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(fc->root, odin3_pin_node(fc->root, pin));
        const odin3_celltype_def *def = odin3_celltype_get(fc->design, type);
        *constant = *constant || odin3_flatten_graph_is_const(fc->graph, type);
        count +=
            (def->flags & ODIN3_CT_TRISTATE) != 0 || odin3_pin_dir(fc->root, pin) == ODIN3_DIR_INOUT
                ? 0
                : 1;
    }
    return count;
}

static const char *net_label(const odin3_flatten_ctx *fc, odin3_net_id net, odin3_strbuf *buf) {
    odin3_wirebit primary = odin3_net_primary(fc->root, net);
    const odin3_strtab *tab = odin3_design_strtab(fc->design);
    uint32_t name = odin3_wire_valid(primary.wire) ? odin3_wire_name(fc->root, primary.wire)
                                                   : odin3_net_name(fc->root, net);
    if (name != 0) {
        return odin3_strtab_get(tab, name);
    }
    (void)odin3_strbuf_appendf(buf, "$n%u", net.v);
    return buf->data;
}

static odin3_status join_error(const odin3_flatten_ctx *fc, flat_joined bit, odin3_net_id net,
                               uint32_t drivers, bool constant) {
    odin3_strbuf label;
    odin3_strbuf_init(&label);
    const char *port =
        odin3_celltype_get(fc->design, odin3_module_celltype(fc->child))->ports[bit.port].name;
    const odin3_strtab *tab = odin3_design_strtab(fc->design);
    const char *mod = odin3_strtab_get(tab, odin3_module_name(fc->child));
    const char *path = odin3_strtab_get(tab, fc->path_str);
    const char *net_name = net_label(fc, net, &label);
    odin3_status st =
        constant ? odin3_flatten_error(fc,
                                       "instance '%s' of module '%s': output port '%s' drives net "
                                       "'%s', which is driven by a constant",
                                       path, mod, port, net_name)
                 : odin3_flatten_error(fc,
                                       "instance '%s': net '%s' has %u drivers after joining port "
                                       "'%s'",
                                       path, net_name, drivers, port);
    odin3_strbuf_free(&label);
    return st;
}

odin3_status odin3_flatten_check_joins(odin3_flatten_ctx *fc) {
    const flat_joined *bits = fc->bits.data;
    for (size_t i = 0; i < fc->bits.len; i++) {
        odin3_net_id net = odin3_flatten_mapped(fc, (odin3_net_id){bits[i].net});
        bool constant = false;
        uint32_t drivers = hard_drivers(fc, net, &constant);
        if (drivers > 1) {
            return join_error(fc, bits[i], net, drivers, constant);
        }
    }
    return ODIN3_OK;
}
