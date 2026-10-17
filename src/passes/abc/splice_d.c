/*
 * splice_d.c — phase D of the splice (3D spec §6.5, AB-18; plan Task 17): a worklist removes the
 * new LUTs and flip-flops nothing reads, then the nets left without pins, wire memberships or
 * aliases, then the non-port wires whose nets have no pins. A failed delete is skipped: what is
 * left is dead but valid.
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "passes/abc/region.h"
#include "passes/abc/splice_int.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- unread new cells ---------------------------------------------------------------------- */

/* True when node is a new LUT or edge-triggered flip-flop (what phase A made). */
static bool removable_kind(const splice *sp, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(sp->run->design, odin3_node_type(sp->mod, node));
    return def != NULL && (strcmp(def->name, "$lut") == 0 || (def->flags & ODIN3_CT_SEQ_EDGE) != 0);
}

/* The net on node's driving pin; none when it drives nothing. */
static odin3_net_id out_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

/* A new cell is dead when its output net is open, or has no reader and no `keep`. */
static bool node_unread(const splice *sp, odin3_node_id node) {
    odin3_net_id out = out_net(sp->mod, node);
    return !odin3_net_valid(out) ||
           (odin3_net_sinks(sp->mod, out).count == 0 && !odin3_abc_net_keep(sp->mod, out));
}

static bool remove_node(splice *sp, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(sp->mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        (void)odin3_pin_disconnect(sp->mod, (odin3_pin_id){pins.first.v + i});
    }
    return odin3_node_delete(sp->mod, node) == ODIN3_OK;
}

/* Creation order is topological (LUTs, then flip-flops), so one backward sweep removes a dead
 * cone; the loop repeats until nothing changes. */
static void remove_unread(splice *sp) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t at = sp->created.len; at > 0; at--) {
            const sp_created *item = odin3_vec_cat(&sp->created, at - 1);
            odin3_node_id node = {item->id};
            if (item->kind == SP_NODE && odin3_node_live(sp->mod, node) &&
                removable_kind(sp, node) && node_unread(sp, node) && remove_node(sp, node)) {
                changed = true;
            }
        }
    }
}

/* --- nets and wires ------------------------------------------------------------------------ */

/* A live net with no pins, no wire membership and no alias (the conditions of odin3_net_delete). */
static bool net_deletable(const odin3_module *mod, odin3_net_id net) {
    return odin3_net_live(mod, net) && odin3_net_pins(mod, net).count == 0 &&
           !odin3_wire_valid(odin3_net_primary(mod, net).wire) &&
           odin3_net_alias_count(mod, net) == 0;
}

static void delete_net_if_dead(splice *sp, uint32_t id) {
    odin3_net_id net = {id};
    if (net_deletable(sp->mod, net) && odin3_net_delete(sp->mod, net) == ODIN3_OK) {
        sp->stats.deleted_nets++;
    }
}

static void delete_dead_nets(splice *sp) {
    for (size_t at = 0; at < sp->old_nets.len; at++) {
        delete_net_if_dead(sp, sp_u32(&sp->old_nets, at));
    }
    for (size_t at = 0; at < sp->created.len; at++) {
        const sp_created *item = odin3_vec_cat(&sp->created, at);
        if (item->kind == SP_NET) {
            delete_net_if_dead(sp, item->id);
        }
    }
}

/* Every wire bit of a wire is on a net without pins. */
static bool wire_pinless(const odin3_module *mod, odin3_wire_id wire) {
    for (uint32_t bit = 0; bit < odin3_wire_width(mod, wire); bit++) {
        odin3_net_id net = odin3_wire_net(mod, wire, bit);
        if (odin3_net_valid(net) && odin3_net_pins(mod, net).count != 0) {
            return false;
        }
    }
    return true;
}

/* Deletes wire when it is a live non-port wire whose nets have no pins; its nets are queued in
 * extra for another round of net deletion (best effort). */
static void delete_wire_if_dead(splice *sp, odin3_wire_id wire, odin3_vec *extra) {
    const odin3_module *mod = sp->mod;
    if (!odin3_wire_valid(wire) || !odin3_wire_live(mod, wire) ||
        odin3_node_valid(odin3_wire_port_node(mod, wire)) || !wire_pinless(mod, wire)) {
        return;
    }
    uint32_t width = odin3_wire_width(mod, wire);
    if (odin3_vec_reserve(extra, extra->len + width) != ODIN3_OK) {
        return;
    }
    for (uint32_t bit = 0; bit < width; bit++) {
        (void)sp_push(extra, odin3_wire_net(mod, wire, bit).v);
    }
    if (odin3_wire_delete(sp->mod, wire) == ODIN3_OK) {
        sp->stats.deleted_wires++;
    }
}

/* The wires that hold net: its primary and its aliases'. Best effort: out of memory skips. */
static void collect_wires(const splice *sp, odin3_net_id net, odin3_vec *wires) {
    odin3_wire_id primary = odin3_net_primary(sp->mod, net).wire;
    if (odin3_wire_valid(primary)) {
        (void)sp_push(wires, primary.v);
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(sp->mod, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire)) {
            (void)sp_push(wires, alias.wb.wire.v);
        }
    }
}

static void delete_dead_wires(splice *sp) {
    odin3_vec wires;
    odin3_vec extra;
    odin3_vec_init(&wires, sizeof(uint32_t));
    odin3_vec_init(&extra, sizeof(uint32_t));
    for (size_t at = 0; at < sp->old_nets.len; at++) {
        odin3_net_id net = {sp_u32(&sp->old_nets, at)};
        if (odin3_net_live(sp->mod, net) && odin3_net_pins(sp->mod, net).count == 0) {
            collect_wires(sp, net, &wires);
        }
    }
    for (size_t at = 0; at < wires.len; at++) {
        delete_wire_if_dead(sp, (odin3_wire_id){sp_u32(&wires, at)}, &extra);
    }
    for (size_t at = 0; at < extra.len; at++) {
        delete_net_if_dead(sp, sp_u32(&extra, at));
    }
    odin3_vec_free(&wires);
    odin3_vec_free(&extra);
}

void sp_phase_d(splice *sp) {
    remove_unread(sp);
    delete_dead_nets(sp);
    delete_dead_wires(sp);
}
