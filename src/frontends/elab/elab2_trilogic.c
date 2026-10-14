/*
 * elab2_trilogic.c — tri-state drivers of internal nets become logic (2D-7b, D3; Peter
 * 2026-10-13; spec 17 #79), as Yosys 0.55's `tribuf -logic` does.
 *
 * odin3_elab2_tribuf makes a `$tribuf` for every `c ? d : 'bz` and `bufif`/`notif`, and records
 * the ones whose nets are not on a port of the top module. At the end of the module those cells
 * are grouped by their Y (the same list of nets: the drivers of one bus), each group becomes one
 * `$pmux` and the `$tribuf` cells are deleted:
 *
 *   A = x (no driver enabled: a don't care; FPGAs have no internal tri-states)
 *   B = the drivers' data, WIDTH bits each, the last driver in the source first (Yosys's order)
 *   S = the drivers' enables, in the same order
 *   Y = the nets
 *
 * Several enables active at once: `$pmux` leaves that undefined (Yosys's model says x; its gate
 * lowering and the simulator give the OR of the selected data), and so does this. Y lists that
 * overlap without being the same list cannot be one `$pmux`; that is a located error.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab2_drive.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ports of a $tribuf, in the cell type's order. */
enum { TRI_A = 0, TRI_EN = 1, TRI_Y = 2 };
#define NO_NEXT UINT32_MAX /* the end of a group's list of records */

/* A group of drivers of one list of nets: the first record (the first driver in the source), the
 * head of the list (the last driver) and how many. The list runs from the last driver to the
 * first, the order Yosys's `tribuf` makes the $pmux in (it walks the module's cell table, which
 * iterates newest first): the x of "no driver enabled" is resolved by `opt` against that order. */
typedef struct tri_group {
    uint32_t first, head, count;
} tri_group;

/* The groups of a module's records; next[i] is the record after record i in its group. */
typedef struct tri_groups {
    odin3_vec groups;    /* tri_group */
    odin3_vec next;      /* uint32_t per record */
    odin3_u64map *owner; /* net -> group index + 1 */
    odin3_vec buf;       /* odin3_net_id: the nets of the $pmux being made */
    odin3_u64map *shared; /* net -> 1: the data net of more than one group (`--vtr-compat`) */
} tri_groups;

static odin3_pinslice tri_port(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, uint32_t port) {
    return odin3_node_port(e2->mod, rec.node, port);
}

static odin3_net_id tri_net(const odin3_elab2 *e2, odin3_pinslice slice, uint32_t bit) {
    return odin3_pin_net(e2->mod, (odin3_pin_id){slice.first.v + bit});
}

/* True when two cells drive the same list of nets. */
static bool same_y(const odin3_elab2 *e2, odin3_elab2_tri_rec lhs, odin3_elab2_tri_rec rhs) {
    odin3_pinslice ys = tri_port(e2, lhs, TRI_Y);
    odin3_pinslice yt = tri_port(e2, rhs, TRI_Y);
    if (ys.count != yt.count) {
        return false;
    }
    for (uint32_t i = 0; i < ys.count; i++) {
        if (tri_net(e2, ys, i).v != tri_net(e2, yt, i).v) {
            return false;
        }
    }
    return true;
}

/* --- naming the bus -------------------------------------------------------------------------- */

odin3_status odin3_elab2_tri_bus_label(const odin3_elab2 *e2, odin3_elab2_tri_rec rec,
                                       odin3_strbuf *out) {
    const odin3_module *mod = e2->mod;
    odin3_pinslice ys = tri_port(e2, rec, TRI_Y);
    odin3_wirebit lo = odin3_net_primary(mod, tri_net(e2, ys, 0));
    const odin3_strtab *tab = odin3_design_strtab(e2->design);
    if (lo.wire.v == 0) {
        return odin3_strbuf_append(out, odin3_bytes_cstr("net"));
    }
    odin3_wirebit hi = odin3_net_primary(mod, tri_net(e2, ys, ys.count - 1U));
    odin3_status st = odin3_strbuf_append(
        out, odin3_bytes_cstr(odin3_strtab_get(tab, odin3_wire_name(mod, lo.wire))));
    uint32_t width = odin3_wire_width(mod, lo.wire);
    if (st != ODIN3_OK || width == 1U ||
        (ys.count == width && hi.wire.v == lo.wire.v && lo.bit == 0)) {
        return st;
    }
    if (ys.count == 1U || hi.wire.v != lo.wire.v) {
        return odin3_strbuf_appendf(out, "[%d]", (int)odin3_wire_index(mod, lo.wire, lo.bit));
    }
    return odin3_strbuf_appendf(out, "[%d:%d]", (int)odin3_wire_index(mod, hi.wire, hi.bit),
                                (int)odin3_wire_index(mod, lo.wire, lo.bit));
}

/* The messages about the bus of a cell. */
typedef enum tri_msg { MSG_WARN, MSG_OVERLAP, MSG_MIX, MSG_BELOW_TOP, MSG_BELOW_TOP_OUT } tri_msg;

/* One located message about the bus of rec, at its driver. */
static odin3_status bus_say(odin3_elab2 *e2, odin3_elab2_tri_rec rec, tri_msg msg) {
    odin3_strbuf name;
    odin3_strbuf_init(&name);
    odin3_status st = odin3_elab2_tri_bus_label(e2, rec, &name);
    if (st == ODIN3_OK) {
        const char *text = name.data != NULL ? name.data : "net";
        odin3_loc at = odin3_ast_loc(e2->elab, rec.at);
        switch (msg) {
        case MSG_OVERLAP:
            odin3_elab2_error(e2, at,
                              "tri-state drivers on internal net '%s' overlap without driving the "
                              "same bits (D3)",
                              text);
            break;
        case MSG_MIX:
            odin3_elab2_error(e2, at,
                              "tri-state driver on '%s' mixes a top-level port bit and an "
                              "internal net (D3)",
                              text);
            break;
        case MSG_BELOW_TOP:
            odin3_elab2_error(e2, at,
                              "tri-state driver on '%s' of an inout port of a module below the "
                              "top is not supported (D3)",
                              text);
            break;
        case MSG_BELOW_TOP_OUT:
            odin3_elab2_error(e2, at,
                              "tri-state driver on '%s' of an output port of a module below the "
                              "top is not supported: the instance may drive a top-level port, "
                              "where z must stay (D3)",
                              text);
            break;
        default:
            odin3_elab2_warn(e2, at,
                             "tri-state drivers on internal net '%s' converted to logic (no "
                             "driver enabled: x; several enabled: undefined)",
                             text);
            break;
        }
    }
    odin3_strbuf_free(&name);
    return st;
}

/* An inout pin of a child instance on a net that becomes logic: the bus is not ours to merge. */
static odin3_status child_inout_error(odin3_elab2 *e2, odin3_elab2_tri_rec rec,
                                      const odin3_elab2_inout_pin *pin) {
    odin3_strbuf name;
    odin3_strbuf place;
    odin3_strbuf_init(&name);
    odin3_strbuf_init(&place);
    odin3_status st = odin3_elab2_tri_bus_label(e2, rec, &name);
    st = st != ODIN3_OK ? st : odin3_elab2_where(e2, rec.at, &place);
    if (st == ODIN3_OK) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, rec.at),
                          "net '%s' is driven by inout port '%s' of instance '%s' and by logic at "
                          "%s; an inout pin cannot share a net with converted tri-state drivers "
                          "(D3)",
                          name.data != NULL ? name.data : "net", pin->port, pin->instance,
                          place.data != NULL ? place.data : "?");
    }
    odin3_strbuf_free(&name);
    odin3_strbuf_free(&place);
    return st;
}

/* --- which cells become logic ---------------------------------------------------------------- */

/* What the nets of a cell's Y are, counted. A net nothing reads (the cell's own pin only: a dropped
 * bit's fresh net) says nothing. */
typedef struct y_nets {
    uint32_t port, internal;
    bool below_top, below_out;
    bool child;
    odin3_elab2_inout_pin pin;
} y_nets;

static y_nets count_y(const odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    y_nets out = {0, 0, false, false, false, {"?", "?"}};
    odin3_pinslice ys = tri_port(e2, rec, TRI_Y);
    for (uint32_t bit = 0; bit < ys.count; bit++) {
        odin3_net_id net = tri_net(e2, ys, bit);
        if (odin3_net_pins(e2->mod, net).count <= 1U) {
            continue;
        }
        odin3_tri_net kind = odin3_elab2_tri_net(e2, net, &out.pin);
        out.port += kind == ODIN3_TRI_NET_PORT ? 1U : 0U;
        out.internal += kind == ODIN3_TRI_NET_PORT ? 0U : 1U;
        out.below_top = out.below_top || kind == ODIN3_TRI_NET_BELOW_TOP_INOUT;
        out.below_out = out.below_out || kind == ODIN3_TRI_NET_BELOW_TOP_OUT;
        out.child = out.child || kind == ODIN3_TRI_NET_CHILD_INOUT;
    }
    return out;
}

/* True when the cell becomes logic. A cell on a top-level port keeps its handling (Yosys's
 * `tribuf -logic` keeps the $tribuf when any bit of the net is on an output port); the other
 * cases that cannot be logic are located errors and the cell stays. *st is a hard failure. */
static bool becomes_logic(odin3_elab2 *e2, odin3_elab2_tri_rec rec, odin3_status *st) {
    y_nets nets = count_y(e2, rec);
    if (nets.below_top) {
        *st = bus_say(e2, rec, MSG_BELOW_TOP);
        return false;
    }
    if (nets.below_out) {
        *st = bus_say(e2, rec, MSG_BELOW_TOP_OUT);
        return false;
    }
    if (nets.port != 0 && nets.internal != 0) {
        *st = bus_say(e2, rec, MSG_MIX);
        return false;
    }
    if (nets.port != 0) {
        return false;
    }
    if (nets.child) {
        *st = child_inout_error(e2, rec, &nets.pin);
        return false;
    }
    return true;
}

/* --- grouping -------------------------------------------------------------------------------- */

static const odin3_elab2_tri_rec *tri_recs(const struct odin3_elab2_drive *dr) {
    return dr->tribufs.data;
}

/* The group of record i: an existing one (its index + 1) when its Y is the first net's owner and
 * the same list, 0 for a new group, UINT32_MAX when the record overlaps another group's nets. */
static uint32_t find_group(const odin3_elab2 *e2, const tri_groups *tg, uint32_t rec_no) {
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    odin3_pinslice ys = tri_port(e2, recs[rec_no], TRI_Y);
    uint64_t owner = 0;
    if (odin3_u64map_get(tg->owner, tri_net(e2, ys, 0).v, &owner)) {
        const tri_group *grp = (const tri_group *)tg->groups.data + (owner - 1U);
        return same_y(e2, recs[grp->first], recs[rec_no]) ? (uint32_t)owner : UINT32_MAX;
    }
    for (uint32_t bit = 1; bit < ys.count; bit++) {
        if (odin3_u64map_get(tg->owner, tri_net(e2, ys, bit).v, NULL)) {
            return UINT32_MAX;
        }
    }
    return 0;
}

/* Starts a group with record i: its nets belong to it from now on. */
static odin3_status new_group(const odin3_elab2 *e2, tri_groups *tg, uint32_t rec_no) {
    odin3_pinslice ys = tri_port(e2, tri_recs(e2->drive)[rec_no], TRI_Y);
    tri_group *grp = odin3_vec_push(&tg->groups);
    if (grp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *grp = (tri_group){rec_no, rec_no, 1};
    for (uint32_t bit = 0; bit < ys.count; bit++) {
        odin3_status st = odin3_u64map_put(
            tg->owner, (odin3_kv){tri_net(e2, ys, bit).v, (uint64_t)tg->groups.len});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Record rec_no joins group number `owner` (its index + 1), as its new head. */
typedef struct tri_join {
    uint32_t owner, rec_no;
} tri_join;

static void join_group(tri_groups *tg, tri_join join) {
    tri_group *grp = (tri_group *)tg->groups.data + (join.owner - 1U);
    ((uint32_t *)tg->next.data)[join.rec_no] = grp->head;
    grp->head = join.rec_no;
    grp->count++;
}

/* Files record i; an overlap is the error and the record stays a $tribuf. */
static odin3_status file_record(odin3_elab2 *e2, tri_groups *tg, uint32_t rec_no) {
    uint32_t owner = find_group(e2, tg, rec_no);
    if (owner == UINT32_MAX) {
        return bus_say(e2, tri_recs(e2->drive)[rec_no], MSG_OVERLAP);
    }
    if (owner != 0) {
        join_group(tg, (tri_join){owner, rec_no});
        return ODIN3_OK;
    }
    return new_group(e2, tg, rec_no);
}

/* --- the $pmux ------------------------------------------------------------------------------- */

/* Pushes the nets of port `port` of the cell of record rec onto the buffer. */
static odin3_status push_port(const odin3_elab2 *e2, tri_groups *tg, odin3_elab2_tri_rec rec,
                              uint32_t port) {
    odin3_pinslice slice = tri_port(e2, rec, port);
    for (uint32_t bit = 0; bit < slice.count; bit++) {
        odin3_net_id *slot = odin3_vec_push(&tg->buf);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = tri_net(e2, slice, bit);
    }
    return ODIN3_OK;
}

/* The buffer: A (width x nets), B (the drivers' data), S (their enables), Y. */
static odin3_status gather(odin3_elab2 *e2, tri_groups *tg, const tri_group *grp) {
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    odin3_net_id xnet = {0};
    uint32_t width = tri_port(e2, recs[grp->first], TRI_Y).count;
    odin3_status st = odin3_elab2_const_net(e2, ODIN3_BIT_X, &xnet);
    odin3_vec_clear(&tg->buf);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        odin3_net_id *slot = odin3_vec_push(&tg->buf);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = xnet;
        }
    }
    const uint32_t port_order[2] = {TRI_A, TRI_EN};
    for (unsigned pass = 0; st == ODIN3_OK && pass < 2U; pass++) {
        for (uint32_t i = grp->head; st == ODIN3_OK && i != NO_NEXT;
             i = ((const uint32_t *)tg->next.data)[i]) {
            st = push_port(e2, tg, recs[i], port_order[pass]);
        }
    }
    return st != ODIN3_OK ? st : push_port(e2, tg, recs[grp->first], TRI_Y);
}

/* Deletes the cells of the group, then makes the $pmux from the buffer. */
static odin3_status make_pmux(odin3_elab2 *e2, tri_groups *tg, const tri_group *grp) {
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    uint32_t width = tri_port(e2, recs[grp->first], TRI_Y).count;
    uint32_t sel = grp->count;
    odin3_value params[2] = {odin3_value_int((int64_t)width), odin3_value_int((int64_t)sel)};
    const odin3_net_id *nets = tg->buf.data;
    odin3_netvec ports[4] = {{nets, width},
                             {nets + width, width * sel},
                             {nets + width + (size_t)width * sel, sel},
                             {nets + width + (size_t)width * sel + sel, width}};
    odin3_prov_id prov = odin3_node_prov(e2->mod, recs[grp->first].node);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = grp->head; st == ODIN3_OK && i != NO_NEXT;
         i = ((const uint32_t *)tg->next.data)[i]) {
        st = odin3_node_delete(e2->mod, recs[i].node);
    }
    odin3_node_spec spec = {
        .type = e2->types[ODIN3_E2T_PMUX], .prov = prov, .params = params, .n_params = 2};
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, ports, &node);
    odin3_elab2_counts made = {1, 2U * width + width * sel + sel, 0, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* `--vtr-compat`, two drivers or more: Parmys's resolution (elab2_trivtr.c); *done when it took
 * the group. The records go in source order. */
static odin3_status try_vtr(odin3_elab2 *e2, const tri_groups *tg, const tri_group *grp,
                            bool *done) {
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    odin3_elab2_tri_rec *ordered = odin3_arena_alloc(e2->scratch, grp->count * sizeof *ordered);
    if (ordered == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t at = grp->count;
    for (uint32_t i = grp->head; i != NO_NEXT; i = ((const uint32_t *)tg->next.data)[i]) {
        ordered[--at] = recs[i];
    }
    odin3_elab2_tri_vtr_req req = {ordered, tg->owner, tg->shared, grp->count};
    return odin3_elab2_tri_vtr(e2, &req, done);
}

/* Notes the data nets of one group: seen holds net -> group + 1, tg->shared the nets two groups
 * have. */
static odin3_status note_data(const odin3_elab2 *e2, tri_groups *tg, odin3_u64map *seen,
                              const tri_group *grp, uint32_t gno) {
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = grp->head; st == ODIN3_OK && i != NO_NEXT;
         i = ((const uint32_t *)tg->next.data)[i]) {
        odin3_pinslice data = tri_port(e2, recs[i], TRI_A);
        for (uint32_t bit = 0; st == ODIN3_OK && bit < data.count; bit++) {
            uint64_t net = tri_net(e2, data, bit).v;
            uint64_t prev = 0;
            if (!odin3_u64map_get(seen, net, &prev)) {
                st = odin3_u64map_put(seen, (odin3_kv){net, (uint64_t)gno + 1U});
            } else if (prev != (uint64_t)gno + 1U) {
                st = odin3_u64map_put(tg->shared, (odin3_kv){net, 1});
            }
        }
    }
    return st;
}

/* `--vtr-compat`, top module: the nets that are the data of drivers of two groups or more. */
static odin3_status find_shared(const odin3_elab2 *e2, tri_groups *tg) {
    odin3_u64map *seen = odin3_u64map_create(0);
    if (seen == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t gno = 0; st == ODIN3_OK && gno < tg->groups.len; gno++) {
        const tri_group grp = ((const tri_group *)tg->groups.data)[gno];
        st = grp.count >= 2U ? note_data(e2, tg, seen, &grp, (uint32_t)gno) : ODIN3_OK;
    }
    odin3_u64map_destroy(seen);
    return st;
}

/* One group: the cap, the nets, the $pmux and the warning. */
static odin3_status convert_group(odin3_elab2 *e2, tri_groups *tg, const tri_group *grp) {
    if (e2->mod_vtr_compat && grp->count >= 2U) { /* below the top it declines, with a warning */
        bool done = false;
        odin3_status vst = try_vtr(e2, tg, grp, &done);
        if (vst != ODIN3_OK || done) {
            return vst;
        }
    }
    const odin3_elab2_tri_rec *recs = tri_recs(e2->drive);
    odin3_elab2_tri_rec first = recs[grp->first];
    uint64_t width = tri_port(e2, first, TRI_Y).count;
    uint64_t pins = 2U * width + width * grp->count + grp->count;
    odin3_status st = odin3_elab2_pins_cap(e2, odin3_ast_loc(e2->elab, first.at), pins);
    st = st != ODIN3_OK ? st : bus_say(e2, first, MSG_WARN);
    st = st != ODIN3_OK ? st : gather(e2, tg, grp);
    return st != ODIN3_OK ? st : make_pmux(e2, tg, grp);
}

static void groups_free(tri_groups *tg) {
    odin3_vec_free(&tg->groups);
    odin3_vec_free(&tg->next);
    odin3_vec_free(&tg->buf);
    odin3_u64map_destroy(tg->owner);
    odin3_u64map_destroy(tg->shared);
}

odin3_status odin3_elab2_tri_logic(odin3_elab2 *e2) {
    const struct odin3_elab2_drive *dr = e2->drive;
    if (dr == NULL || dr->tribufs.len == 0) {
        return ODIN3_OK;
    }
    tri_groups tg = {.owner = odin3_u64map_create(0), .shared = odin3_u64map_create(0)};
    odin3_vec_init(&tg.groups, sizeof(tri_group));
    odin3_vec_init(&tg.next, sizeof(uint32_t));
    odin3_vec_init(&tg.buf, sizeof(odin3_net_id));
    odin3_status st = tg.owner != NULL && tg.shared != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t i = 0; st == ODIN3_OK && i < dr->tribufs.len; i++) {
        uint32_t *link = odin3_vec_push(&tg.next);
        st = link != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (link != NULL) {
            *link = NO_NEXT;
            bool convert = becomes_logic(e2, tri_recs(dr)[i], &st);
            st = convert && st == ODIN3_OK ? file_record(e2, &tg, i) : st;
        }
    }
    st = st == ODIN3_OK && e2->mod_vtr_compat && dr->is_top ? find_shared(e2, &tg) : st;
    for (size_t gno = 0; st == ODIN3_OK && gno < tg.groups.len; gno++) {
        const tri_group grp = ((const tri_group *)tg.groups.data)[gno];
        st = convert_group(e2, &tg, &grp);
    }
    groups_free(&tg);
    return st;
}
