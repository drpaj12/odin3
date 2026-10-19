/*
 * splice_bc.c — phases B and C of the splice (3D spec §6.5, AB-18; plan Task 17): re-stitching the
 * COs one by one (each step atomic and function-keeping), then deleting the old cells and moving
 * the register names and Q nets.
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "passes/abc/prov.h"
#include "passes/abc/splice_int.h"

#include <stdbool.h>
#include <stdint.h>

enum { PORT_Y = 1, /* the Y port of a $lut */ FIRST_OBJ = 2 };

/* --- phase B ------------------------------------------------------------------------------- */

/* The step's change of the net c, whose old driver has just been disconnected. */
static odin3_status apply_step(splice *sp, const sp_step *step, odin3_net_id net) {
    odin3_module *mod = sp->mod;
    odin3_status st = ODIN3_OK;
    switch (step->act) {
    case SP_TAKE:
        st = odin3_net_merge(mod, (odin3_net_pair){net, {step->take}});
        break;
    case SP_FOLLOW:
    case SP_CONST:
        st = odin3_net_merge(mod, (odin3_net_pair){{step->keep}, net});
        break;
    default: {
        odin3_pinslice out = odin3_node_port(mod, (odin3_node_id){step->buffer}, PORT_Y);
        st = odin3_pin_connect(mod, out.first, net);
        sp->stats.buffers += st == ODIN3_OK ? 1U : 0U;
        return st;
    }
    }
    sp->stats.merges += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* One CO: its old driver pin leaves, the step runs, and a failure puts the pin back. */
static odin3_status stitch_one(splice *sp, uint32_t idx) {
    odin3_net_id net = {sp_u32(&sp->region->cos, idx)};
    const sp_step *step = odin3_vec_cat(&sp->steps, idx);
    odin3_pin_id old = {0};
    if (odin3_net_driver_count(sp->mod, net) > 0) {
        old = odin3_net_driver(sp->mod, net);
        (void)odin3_pin_disconnect(sp->mod, old);
    }
    odin3_status st = apply_step(sp, step, net);
    if (st != ODIN3_OK && old.v != 0) {
        (void)odin3_pin_connect(sp->mod, old, net);
    }
    return st;
}

odin3_status sp_phase_b(splice *sp) {
    for (uint32_t idx = 0; idx < sp->res->n_cos; idx++) {
        odin3_status st = stitch_one(sp, idx);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- phase C ------------------------------------------------------------------------------- */

/* Deletes the old soft cells and flip-flops. A delete that fails (out of memory) leaves a dead
 * cell that drives only its own old nets, so the function is complete; the first failure is
 * returned after the rest were tried. */
static odin3_status delete_old(splice *sp) {
    odin3_status first = ODIN3_OK;
    for (size_t at = 0; at < sp->region->soft.len + sp->region->regs.len; at++) {
        bool soft = at < sp->region->soft.len;
        odin3_node_id node = {sp_u32(soft ? &sp->region->soft : &sp->region->regs,
                                     soft ? at : at - sp->region->soft.len)};
        if (!odin3_node_live(sp->mod, node)) {
            continue;
        }
        odin3_status st = odin3_node_delete(sp->mod, node);
        if (st == ODIN3_OK) {
            sp->stats.deleted_cells++;
        } else if (first == ODIN3_OK) {
            first = st;
        }
    }
    return first;
}

/* The net on a node's driving pin; none when it drives nothing. */
static odin3_net_id drive_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

/* Register j: the old Q net (when nothing drives it any more) joins the new one, and the new
 * flip-flop takes the representative's name. */
static odin3_status move_register(splice *sp, uint32_t reg) {
    odin3_node_id ff = {sp_u32(&sp->ffnode, reg)};
    odin3_net_id oldq = {sp_u32(&sp->oldq, reg)};
    odin3_net_id newq = drive_net(sp->mod, ff);
    odin3_status st = ODIN3_OK;
    if (odin3_net_valid(oldq) && odin3_net_live(sp->mod, oldq) && oldq.v != newq.v &&
        odin3_net_driver_count(sp->mod, oldq) == 0) {
        st = odin3_net_merge(sp->mod, (odin3_net_pair){newq, oldq});
        sp->stats.merges += st == ODIN3_OK ? 1U : 0U;
    }
    uint32_t name = sp_u32(&sp->repname, reg);
    if (st == ODIN3_OK && name != 0 && !odin3_node_valid(odin3_module_find_node(sp->mod, name))) {
        st = odin3_node_rename(sp->mod, ff, name);
    }
    return st;
}

/* The net object obj drives now: a LUT's output, a flip-flop's Q, or the net of a CI or constant.
 */
static odin3_net_id object_net_now(const splice *sp, uint32_t obj) {
    uint32_t base = FIRST_OBJ + sp->res->n_cis;
    if (obj >= odin3_abc_result_first_lut(sp->res)) {
        return drive_net(sp->mod, (odin3_node_id){sp_u32(&sp->objnode, obj)});
    }
    if (obj >= base) {
        return drive_net(sp->mod, (odin3_node_id){sp_u32(&sp->ffnode, obj - base)});
    }
    return (odin3_net_id){sp_u32(&sp->objnet, obj)};
}

/* An old internal net (pinless now, neither a CI nor a CO) that is a register input's net: merged
 * into the net its anchor's object drives, so its names and wire memberships survive (§6.5). */
static bool movable(const splice *sp, odin3_net_id old) {
    return odin3_net_valid(old) && odin3_net_live(sp->mod, old) &&
           sp_u32(&sp->region->co_of, old.v) == 0 && sp_u32(&sp->region->ci_of, old.v) == 0 &&
           odin3_net_driver_count(sp->mod, old) == 0;
}

static odin3_status move_anchored_nets(splice *sp) {
    odin3_status first = ODIN3_OK;
    for (size_t at = 0; at < sp->anchors.len; at++) {
        const odin3_abc_anchor *an = odin3_vec_cat(&sp->anchors, at);
        bool names = an->kind == ODIN3_ABC_ANCHOR_REG_IN || an->kind == ODIN3_ABC_ANCHOR_NAMEMAP;
        if (!names || an->complemented || !movable(sp, an->old_net)) {
            continue;
        }
        odin3_net_id target = object_net_now(sp, an->obj);
        if (!odin3_net_valid(target) || target.v == an->old_net.v ||
            !odin3_net_live(sp->mod, target)) {
            continue;
        }
        odin3_status st = odin3_net_merge(sp->mod, (odin3_net_pair){target, an->old_net});
        sp->stats.merges += st == ODIN3_OK ? 1U : 0U;
        first = first == ODIN3_OK ? st : first;
    }
    return first;
}

odin3_status sp_phase_c(splice *sp) {
    odin3_status st = delete_old(sp);
    odin3_status moved = move_anchored_nets(sp);
    st = st == ODIN3_OK ? moved : st;
    for (uint32_t reg = 0; reg < sp->res->n_regs_out; reg++) {
        moved = move_register(sp, reg);
        st = st == ODIN3_OK ? moved : st;
    }
    return st;
}
