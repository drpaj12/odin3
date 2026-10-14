/*
 * elab2_trivtr.c — tri-state drivers of an internal net under `--vtr-compat`: the resolution
 * Parmys gives (PHASE2 decision 27; Peter 2026-10-14; spec 2D 6.7, 17 #79).
 *
 * Yosys 0.55 (and so Parmys) never builds a bus from `assign d = en ? x : 'bz`. Its `opt_expr
 * -mux_undef` replaces a mux with an undefined input (z counts) by its other input, so every
 * driver becomes the plain connection `d = x` and the enables are gone. `opt_clean` then puts d
 * and all the x into one class of signals and keeps one representative; the others are shorted
 * to it. When every x is an input port bit that leaves `d` wired to one input and the other
 * inputs unused (micro `syntax/multi_assignment`: `busout = enable ? bus0 : 1`, `s` unused).
 *
 * The representative is the one `compare_signals` (opt_clean.cc) ranks first, for two input
 * ports: the bit another cell reads (`connected_signals`), else the wire with the lowest name
 * (strcmp), else the highest bit of that wire (a later bit of an equal wire replaces the
 * earlier). The order of the assigns does not matter.
 *
 * Only a bit whose every driver is a distinct or repeated input port bit is resolved here. A
 * driver that is a cell output, a constant or a bit of a child instance is left to the IEEE
 * resolution (`$pmux`, elab2_trilogic.c): Parmys leaves several drivers on one net there, which
 * the IR cannot hold, and the netlist is not usable in VTR either.
 */
#include "ast/ast.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab2_drive.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The ports of a $tribuf, in the cell type's order. */
enum { VT_A = 0, VT_Y = 2 };

/* An input port bit that drives one data bit of a driver. */
typedef struct vtr_cand {
    odin3_net_id net;
    odin3_wire_id wire;
    uint32_t bit;
    bool read; /* a cell, not a $tribuf or a port, reads the net */
} vtr_cand;

typedef struct vtr_run {
    odin3_elab2 *e2;
    const odin3_elab2_tri_rec *recs;
    const odin3_u64map *y_owner; /* the nets that are the Y of a group of $tribuf cells */
    uint32_t count, width;
    vtr_cand *rep;       /* the winner of each bit */
    odin3_vec cands;     /* vtr_cand: every distinct candidate of every bit */
    odin3_u64map *known; /* net -> 1: the candidates in cands */
} vtr_run;

static const char *wire_str(const odin3_elab2 *e2, odin3_wire_id wire) {
    return odin3_strtab_get(odin3_design_strtab(e2->design), odin3_wire_name(e2->mod, wire));
}

static odin3_net_id vt_net(const odin3_elab2 *e2, odin3_pinslice slice, uint32_t bit) {
    return odin3_pin_net(e2->mod, (odin3_pin_id){slice.first.v + bit});
}

/* Bit `bit` of the A (data) or Y port of a driver's $tribuf. */
static odin3_net_id vt_data(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, uint32_t bit) {
    return vt_net(e2, odin3_node_port(e2->mod, rec.node, VT_A), bit);
}

static odin3_net_id vt_out(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, uint32_t bit) {
    return vt_net(e2, odin3_node_port(e2->mod, rec.node, VT_Y), bit);
}

/* The wire of the port node of a module's port list; none when node is no port of it. */
static odin3_wire_id port_wire_of(const odin3_elab2 *e2, odin3_node_id node) {
    uint32_t ports = odin3_module_port_count(e2->mod);
    for (uint32_t i = 0; i < ports; i++) {
        if (odin3_module_port(e2->mod, i).v == node.v) {
            return odin3_module_port_wire(e2->mod, i);
        }
    }
    return (odin3_wire_id){0};
}

/* True when a cell other than a $tribuf, a port node or a dead one reads the net. */
static bool read_by_cell(const odin3_elab2 *e2, odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(e2->mod, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        odin3_node_id node = odin3_pin_node(e2->mod, sinks.pins[i]);
        odin3_celltype_id type = odin3_node_type(e2->mod, node);
        const odin3_celltype_def *def = odin3_celltype_get(e2->design, type);
        if (type.v != e2->types[ODIN3_E2T_TRIBUF].v && def != NULL &&
            def->gran != ODIN3_GRAN_PORT) {
            return true;
        }
    }
    return false;
}

/* The candidate of a net: its one driver must be the pin of a $port_in node of this module. */
static bool input_source(const odin3_elab2 *e2, odin3_net_id net, vtr_cand *out) {
    if (net.v == 0 || odin3_net_driver_count(e2->mod, net) != 1U) {
        return false;
    }
    odin3_pin_id pin = odin3_net_driver(e2->mod, net);
    odin3_node_id node = odin3_pin_node(e2->mod, pin);
    const odin3_celltype_def *def = odin3_celltype_get(e2->design, odin3_node_type(e2->mod, node));
    if (def == NULL || def->gran != ODIN3_GRAN_PORT || strcmp(def->name, "$port_in") != 0) {
        return false;
    }
    odin3_wire_id wire = port_wire_of(e2, node);
    if (wire.v == 0) {
        return false;
    }
    *out = (vtr_cand){net, wire, odin3_pin_bit(e2->mod, pin), read_by_cell(e2, net)};
    return true;
}

/* compare_signals for two input port bits: true when a is the representative over b. */
static bool beats(const odin3_elab2 *e2, const vtr_cand *lhs, const vtr_cand *rhs) {
    if (lhs->read != rhs->read) {
        return lhs->read;
    }
    int order = strcmp(wire_str(e2, lhs->wire), wire_str(e2, rhs->wire));
    if (order != 0) {
        return order < 0;
    }
    return lhs->bit > rhs->bit;
}

/* Remembers a candidate once. */
static odin3_status remember(vtr_run *run, const vtr_cand *cand) {
    if (odin3_u64map_get(run->known, cand->net.v, NULL)) {
        return ODIN3_OK;
    }
    vtr_cand *slot = odin3_vec_push(&run->cands);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *cand;
    return odin3_u64map_put(run->known, (odin3_kv){cand->net.v, 1});
}

/* One data bit: the winner among the drivers' bits. *ok false when a driver is not an input. */
static odin3_status pick_bit(vtr_run *run, uint32_t bit, bool *ok) {
    odin3_net_id ynet = vt_out(run->e2, run->recs[0], bit);
    for (uint32_t i = 0; *ok && i < run->count; i++) {
        vtr_cand cand;
        odin3_net_id net = vt_data(run->e2, run->recs[i], bit);
        *ok = net.v != ynet.v && !odin3_u64map_get(run->y_owner, net.v, NULL) &&
              input_source(run->e2, net, &cand);
        if (!*ok) {
            return ODIN3_OK;
        }
        odin3_status st = remember(run, &cand);
        if (st != ODIN3_OK) {
            return st;
        }
        if (i == 0 || beats(run->e2, &cand, &run->rep[bit])) {
            run->rep[bit] = cand;
        }
    }
    return ODIN3_OK;
}

/* --- the message ----------------------------------------------------------------------------- */

/* Appends the wire name once to list, comma separated; seen holds the wires listed. */
static odin3_status list_wire(const odin3_elab2 *e2, odin3_u64map *seen, odin3_wire_id wire,
                              odin3_strbuf *list) {
    if (odin3_u64map_get(seen, wire.v, NULL)) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_u64map_put(seen, (odin3_kv){wire.v, 1});
    st = st != ODIN3_OK || list->len == 0 ? st : odin3_strbuf_append(list, odin3_bytes_cstr(", "));
    return st != ODIN3_OK ? st : odin3_strbuf_appendf(list, "'%s'", wire_str(e2, wire));
}

/* The wires of the winners in kept, then those of the other candidates in dropped. */
static odin3_status name_wires(const vtr_run *run, odin3_strbuf *kept, odin3_strbuf *dropped) {
    odin3_u64map *seen = odin3_u64map_create(0);
    if (seen == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < run->width; bit++) {
        st = list_wire(run->e2, seen, run->rep[bit].wire, kept);
    }
    const vtr_cand *all = run->cands.data;
    for (size_t i = 0; st == ODIN3_OK && i < run->cands.len; i++) {
        st = list_wire(run->e2, seen, all[i].wire, dropped);
    }
    odin3_u64map_destroy(seen);
    return st;
}

#define VTR_RULE                                                                                   \
    "the input Yosys keeps as the net's representative: one other logic reads, else the lowest "   \
    "name"

/* The warning: the net, the drivers, the one kept, the ones dropped and the IEEE reading. */
static odin3_status say(const vtr_run *run) {
    odin3_elab2 *e2 = run->e2;
    odin3_strbuf net;
    odin3_strbuf kept;
    odin3_strbuf dropped;
    odin3_strbuf_init(&net);
    odin3_strbuf_init(&kept);
    odin3_strbuf_init(&dropped);
    odin3_status st = odin3_elab2_tri_bus_label(e2, run->recs[0], &net);
    st = st != ODIN3_OK ? st : name_wires(run, &kept, &dropped);
    if (st == ODIN3_OK) {
        odin3_elab2_warn(
            e2, odin3_ast_loc(e2->elab, run->recs[0].at),
            "net '%s' has %u drivers; --vtr-compat keeps only the data of %s as Parmys does "
            "(" VTR_RULE
            "), ignores every enable and drops %s (IEEE resolution would be a bus selected by the "
            "enables: the data of the driver that is enabled, undefined for several)",
            net.data != NULL ? net.data : "net", (unsigned)run->count,
            kept.data != NULL ? kept.data : "?", dropped.data != NULL ? dropped.data : "none");
    }
    odin3_strbuf_free(&net);
    odin3_strbuf_free(&kept);
    odin3_strbuf_free(&dropped);
    return st;
}

/* --- the change ------------------------------------------------------------------------------ */

/* Deletes the $tribuf cells and shorts every Y net into its winner. */
static odin3_status short_nets(vtr_run *run) {
    odin3_elab2 *e2 = run->e2;
    odin3_net_id *ynets = odin3_arena_alloc(e2->scratch, (size_t)run->width * sizeof *ynets);
    if (ynets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t bit = 0; bit < run->width; bit++) {
        ynets[bit] = vt_out(e2, run->recs[0], bit);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < run->count; i++) {
        st = odin3_node_delete(e2->mod, run->recs[i].node);
    }
    for (uint32_t bit = 0; st == ODIN3_OK && bit < run->width; bit++) {
        st = odin3_elab2_merge(e2, (odin3_net_pair){run->rep[bit].net, ynets[bit]});
    }
    return st;
}

odin3_status odin3_elab2_tri_vtr(odin3_elab2 *e2, const odin3_elab2_tri_vtr_req *req, bool *done) {
    *done = false;
    vtr_run run = {.e2 = e2,
                   .recs = req->recs,
                   .y_owner = req->y_owner,
                   .count = req->count,
                   .width = odin3_node_port(e2->mod, req->recs[0].node, VT_Y).count};
    odin3_vec_init(&run.cands, sizeof(vtr_cand));
    run.known = odin3_u64map_create(0);
    run.rep = odin3_arena_alloc(e2->scratch, (size_t)run.width * sizeof *run.rep);
    odin3_status st = run.rep != NULL && run.known != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    bool ok = true;
    for (uint32_t bit = 0; st == ODIN3_OK && ok && bit < run.width; bit++) {
        st = pick_bit(&run, bit, &ok);
    }
    if (st == ODIN3_OK && ok) {
        st = say(&run);
        st = st != ODIN3_OK ? st : short_nets(&run);
        *done = st == ODIN3_OK;
    }
    odin3_vec_free(&run.cands);
    odin3_u64map_destroy(run.known);
    return st;
}
