/*
 * connect.c — the per-cell transaction of `lower` (3B spec §3.2, §9, §12.2; plan Task 1): begin
 * (provenance), lw_connect (kit_connect's merge rule), commit (delete the word node, merge its
 * outputs) and rollback (remove what a failed builder made). It mirrors the 2F kit's rule and does
 * not call it: the kit is private to src/passes/opt (spec §16 row 22).
 */
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_int.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

odin3_status lw_begin(lw_ctx *lw, odin3_node_id node) {
    odin3_vec_clear(&lw->created);
    odin3_vec_clear(&lw->created_nets);
    odin3_vec_clear(&lw->merged);
    lw->cur = node;
    lw->gates = 0;
    lw->prefix_set = false;
    lw->prov = (odin3_prov_id){0};
    lw->keep_value = NULL;
    odin3_prov_begin_op(lw->pass);
    odin3_prov_id parent = odin3_node_prov(lw->module, node);
    if (odin3_prov_valid(parent)) {
        odin3_prov_list parents = {&parent, 1};
        odin3_status st = odin3_prov_derive(lw->pass, parents, &lw->prov);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (lw->keep_key != 0) {
        lw->keep_value =
            odin3_attr_get(lw->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, lw->keep_key);
    }
    return ODIN3_OK;
}

odin3_status lw_connect(lw_ctx *lw, odin3_net_id keep, odin3_net_id drop) {
    if (!odin3_net_valid(keep) || !odin3_net_valid(drop) || keep.v == drop.v) {
        return ODIN3_OK;
    }
    return odin3_net_merge(lw->module, (odin3_net_pair){keep, drop});
}

/* The live net that `net` stands for after the merges in lw->merged (none if it was lost). */
static odin3_net_id resolve(const lw_ctx *lw, odin3_net_id net) {
    while (odin3_net_valid(net) && !odin3_net_live(lw->module, net)) {
        odin3_net_id next = {0};
        for (size_t i = 0; i < lw->merged.len && !odin3_net_valid(next); i++) {
            const odin3_net_pair *pair = odin3_vec_cat(&lw->merged, i);
            next = pair->drop.v == net.v ? pair->keep : next;
        }
        net = next;
    }
    return net;
}

/* The merges of a commit after the delete: ynets[k] into outs[k]. */
static odin3_status merge_outputs(lw_ctx *lw, lw_span ynets, const lw_bits *outs) {
    for (uint32_t k = 0; k < ynets.n; k++) {
        odin3_net_id drop = resolve(lw, ynets.nets[k]);
        odin3_net_id keep = resolve(lw, lw_bits_at(outs, k));
        if (!odin3_net_valid(drop) || !odin3_net_valid(keep) || drop.v == keep.v) {
            continue; /* unconnected, or fed back to itself: left undriven */
        }
        odin3_status st = lw_connect(lw, keep, drop);
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_net_pair *slot = odin3_vec_push(&lw->merged); /* reserved: never NULL */
        if (slot != NULL) {
            *slot = (odin3_net_pair){keep, drop};
        }
    }
    return ODIN3_OK;
}

odin3_status lw_commit(lw_ctx *lw, const lw_cell *cell, const lw_bits *outs) {
    lw_span ynets = cell->port[cell->out_port];
    if (lw_bits_len(outs) != ynets.n) {
        odin3_log(ODIN3_LOG_ERROR, "lower: '%s': builder returned %u bits for a %u-bit output",
                  cell->def->name, lw_bits_len(outs), ynets.n);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_vec_reserve(&lw->merged, (size_t)ynets.n + 1);
    st = st == ODIN3_OK ? odin3_node_delete(lw->module, cell->node) : st;
    if (st != ODIN3_OK) {
        return st; /* nothing changed: the caller rolls the builder back */
    }
    lw->live_nodes--;
    st = merge_outputs(lw, ynets, outs);
    if (st != ODIN3_OK) {
        char label[LW_LABEL_LEN];
        lw_node_label(lw, cell->node, label, sizeof label);
        odin3_log(ODIN3_LOG_ERROR,
                  "lower: module '%s': out of memory while connecting the outputs of '%s' '%s': "
                  "some of its outputs are left undriven",
                  odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module)),
                  cell->def->name, label);
    }
    return st;
}

/* Deletes a node; when that cannot allocate its tombstone record, disconnects its pins instead. */
static void remove_node(lw_ctx *lw, odin3_node_id node) {
    if (odin3_node_delete(lw->module, node) == ODIN3_OK) {
        return;
    }
    odin3_pinslice pins = odin3_node_pins(lw->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        (void)odin3_pin_disconnect(lw->module, (odin3_pin_id){pins.first.v + i});
    }
}

void lw_rollback(lw_ctx *lw) {
    for (size_t i = lw->created.len; i > 0; i--) {
        remove_node(lw, *(const odin3_node_id *)odin3_vec_cat(&lw->created, i - 1));
    }
    if (lw->live_nodes >= lw->created.len) {
        lw->live_nodes -= lw->created.len;
    }
    for (size_t i = lw->created_nets.len; i > 0; i--) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&lw->created_nets, i - 1);
        (void)odin3_net_delete(lw->module, net); /* fails only for a floating node's net */
    }
    for (uint32_t i = 0; i < LW_N_CONST; i++) {
        if (lw->const_slot[i].fresh) {
            lw->const_slot[i] = (lw_const_slot){{0}, false};
        }
    }
    odin3_vec_clear(&lw->created);
    odin3_vec_clear(&lw->created_nets);
}

void lw_end(lw_ctx *lw) {
    for (uint32_t i = 0; i < LW_N_CONST; i++) {
        lw->const_slot[i].fresh = false;
    }
    lw->totals.nodes += lw->created.len;
    odin3_vec_clear(&lw->created);
    odin3_vec_clear(&lw->created_nets);
}

/* Appends the nets of one port of node to lw->portnets (room reserved); open input pins get a fresh
 * net each. */
static odin3_status load_port(lw_ctx *lw, const odin3_celltype_def *def, odin3_node_id node,
                              uint32_t port) {
    odin3_pinslice pins = odin3_node_port(lw->module, node, port);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_net_id net = odin3_pin_net(lw->module, (odin3_pin_id){pins.first.v + i});
        if (!odin3_net_valid(net) && def->ports[port].dir == ODIN3_DIR_IN) {
            odin3_status st = lw_net(lw, &net);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        odin3_net_id *slot = odin3_vec_push(&lw->portnets); /* reserved: never NULL */
        if (slot != NULL) {
            *slot = net;
        }
    }
    return ODIN3_OK;
}

odin3_status lw_load_cell(lw_ctx *lw, odin3_node_id node, lw_cell *cell) {
    const odin3_celltype_def *def =
        odin3_celltype_get(lw->design, odin3_node_type(lw->module, node));
    memset(cell, 0, sizeof *cell);
    cell->node = node;
    cell->def = def;
    cell->params = odin3_node_param(lw->module, node, 0);
    cell->n_ports = def->n_ports;
    uint32_t n_out = 0;
    size_t total = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        total += odin3_node_port(lw->module, node, port).count;
        if (def->ports[port].dir != ODIN3_DIR_IN) {
            cell->out_port = port;
            n_out++;
        }
    }
    if (def->n_ports > LW_MAX_PORTS || n_out != 1) {
        odin3_log(ODIN3_LOG_ERROR, "lower: cell type '%s' has no single output port", def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec_clear(&lw->portnets);
    odin3_status st = odin3_vec_reserve(&lw->portnets, total);
    size_t first[LW_MAX_PORTS + 1] = {0};
    for (uint32_t port = 0; st == ODIN3_OK && port < def->n_ports; port++) {
        first[port] = lw->portnets.len;
        st = load_port(lw, def, node, port);
        first[port + 1] = lw->portnets.len;
    }
    /* The vector never grows past `total`, so these pointers stay valid. */
    const odin3_net_id *base = lw->portnets.data;
    for (uint32_t port = 0; st == ODIN3_OK && port < def->n_ports; port++) {
        cell->port[port] = (lw_span){base + first[port], (uint32_t)(first[port + 1] - first[port])};
    }
    return st;
}
