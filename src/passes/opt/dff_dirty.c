/*
 * dff_dirty.c — opt_dff's dirty set (2F spec §19, OP-12; the opt_expr counterpart is dirty.c).
 *
 * After its first sweep of a module in an opt session, opt_dff loads and runs the rules only on the
 * FFs whose inputs may have changed. What a visit of FF f reads is exactly: f's own node and pins;
 * the constants its nets carry (a net's driver); the init of its Q nets (wire attributes and
 * memberships); and, for rules 10 and 11, the single-user mux cone below D (the mux nodes' pins and
 * their nets, and the readers of each mux output, which the pass snapshots). A change to any of
 * these is a journal entry of the IR (module.h): a pin connected, disconnected or handed over, a
 * net merged, a wire membership or attribute changed. The journal entries since the previous sweep
 * began are walked here, forwards from the changed object: an FF reached directly is dirty, a mux
 * reached is followed through the readers of its outputs (it may sit in the cone of the FFs
 * below), anything else stops the walk. Everything journaled during a sweep stays for the next
 * one, and is also walked before the constant-bits sweep (rule 12's rewrites of the sweep's own
 * FFs). A visit of an FF that is not dirty would change nothing, so it is skipped; to keep the
 * provenance operation numbers of a full sweep it still counts as an operation.
 *
 * Fallbacks, all exact: an overflowed journal, or a module not yet swept once, is a full sweep.
 * The mux walks stop after as many steps as the module has mux bits (a mux loop): an FF that hit
 * that cap journals itself and is visited again each sweep. Debug builds visit the skipped FFs
 * after the sweep and abort if one of them changes anything.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/opt/dff.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MARK_FF = 1U, MARK_MUX = 2U, N_MUX_NAMES = 3, JOURNAL_SHIFT = 32 };

typedef struct walker {
    const odin3_dff_state *ds;
    odin3_dff_dirty *dd;
    odin3_celltype_id mux_types[N_MUX_NAMES];
    odin3_vec stack; /* uint32_t node IDs of muxes whose outputs are to be followed */
    uint8_t *seen;   /* per net ID: bit 0 DRIVE entry walked, bit 1 SINK entry walked */
    uint32_t n_seen;
} walker;

static bool is_mux(const walker *wk, odin3_celltype_id type) {
    for (size_t i = 0; i < N_MUX_NAMES; i++) {
        if (wk->mux_types[i].v == type.v && type.v != 0) {
            return true;
        }
    }
    return false;
}

/* Marks node: an FF is dirty, a mux not yet followed goes on the stack, anything else is nothing.
 */
static odin3_status touch_node(walker *wk, odin3_node_id node) {
    const odin3_opt_ctx *oc = wk->ds->oc;
    if (node.v == 0 || node.v >= wk->dd->n_marks || !odin3_node_live(oc->module, node)) {
        return ODIN3_OK;
    }
    odin3_celltype_id type = odin3_node_type(oc->module, node);
    uint8_t *mark = &wk->dd->marks[node.v];
    if (odin3_ff_is_ff(oc, type)) {
        *mark |= MARK_FF;
    } else if (is_mux(wk, type) && (*mark & MARK_MUX) == 0) {
        *mark |= MARK_MUX;
        return odin3_kit_push_u32(&wk->stack, node.v);
    }
    return ODIN3_OK;
}

/* What touch_net reaches. */
typedef enum reach {
    REACH_READERS,     /* the readers of the net */
    REACH_ALL,         /* its readers and drivers */
    REACH_MUX_DRIVERS, /* its drivers that are muxes (their output readers are a snapshot) */
} reach;

/* The nodes on net that `how` selects, touched. */
static odin3_status touch_net(walker *wk, odin3_net_id net, reach how) {
    const odin3_module *module = wk->ds->oc->module;
    if (!odin3_net_valid(net) || !odin3_net_live(module, net)) {
        return ODIN3_OK;
    }
    odin3_pinlist pins = odin3_net_pins(module, net);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < pins.count; i++) {
        bool drives = odin3_pin_drives(module, pins.pins[i]);
        odin3_node_id node = odin3_pin_node(module, pins.pins[i]);
        bool take = how == REACH_ALL || (how == REACH_READERS && !drives) ||
                    (how == REACH_MUX_DRIVERS && drives && node.v != 0 &&
                     is_mux(wk, odin3_node_type(module, node)));
        st = take ? touch_node(wk, node) : st;
    }
    return st;
}

/* The mux popped from the stack: the readers of its outputs. */
static odin3_status follow_mux(walker *wk, odin3_node_id node) {
    const odin3_module *module = wk->ds->oc->module;
    odin3_pinslice pins = odin3_node_pins(module, node);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (odin3_pin_drives(module, pin)) {
            st = touch_net(wk, odin3_pin_net(module, pin), REACH_READERS);
        }
    }
    return st;
}

/* One journal entry. */
static odin3_status entry_walk(walker *wk, uint64_t entry) {
    uint32_t kind = (uint32_t)(entry >> JOURNAL_SHIFT);
    uint32_t id = (uint32_t)entry;
    if (kind == ODIN3_JOURNAL_NODE) {
        return touch_node(wk, (odin3_node_id){id});
    }
    if (id <
        wk->n_seen) { /* a net is walked once per kind (clock, reset and constant nets recur) */
        uint8_t bit = kind == ODIN3_JOURNAL_DRIVE ? 1U : 2U;
        if ((wk->seen[id] & bit) != 0) {
            return ODIN3_OK;
        }
        wk->seen[id] = (uint8_t)(wk->seen[id] | bit);
    }
    if (kind == ODIN3_JOURNAL_DRIVE) {
        return touch_net(wk, (odin3_net_id){id}, REACH_ALL);
    }
    /* SINK: the readers of the net changed, which a mux driving it snapshotted */
    return touch_net(wk, (odin3_net_id){id}, REACH_MUX_DRIVERS);
}

/* Walks entries [0, n) of the journal (plus what the muxes they reach lead to). */
static odin3_status walk_entries(walker *wk, const uint64_t *entries, size_t n) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = entry_walk(wk, entries[i]);
        while (st == ODIN3_OK && wk->stack.len > 0) {
            uint32_t node = *(const uint32_t *)odin3_vec_cat(&wk->stack, wk->stack.len - 1U);
            wk->stack.len--;
            st = follow_mux(wk, (odin3_node_id){node});
        }
    }
    return st;
}

static void walker_init(walker *wk, const odin3_dff_state *ds, odin3_dff_dirty *dd) {
    static const char *const k_names[N_MUX_NAMES] = {"$mux", "$pmux", "$_MUX_"};
    wk->ds = ds;
    wk->dd = dd;
    for (size_t i = 0; i < N_MUX_NAMES; i++) {
        wk->mux_types[i] = odin3_kit_type_named(ds->oc->design, k_names[i]);
    }
    odin3_vec_init(&wk->stack, sizeof(uint32_t));
    wk->n_seen = odin3_module_net_end(ds->oc->module);
    wk->seen = odin3_util_calloc((size_t)wk->n_seen + 1U); /* NULL: walked again, still right */
    if (wk->seen == NULL) {
        wk->n_seen = 0;
    }
}

static void walker_free(walker *wk) {
    odin3_vec_free(&wk->stack);
    odin3_util_free(wk->seen);
}

/* Clears the mux-followed bits (so a later walk follows again) and keeps the FF bits. */
static void clear_followed(odin3_dff_dirty *dd) {
    for (uint32_t i = 0; i < dd->n_marks; i++) {
        dd->marks[i] = (uint8_t)(dd->marks[i] & MARK_FF);
    }
}

void odin3_dff_dirty_init(odin3_dff_dirty *dd) {
    memset(dd, 0, sizeof *dd);
}

void odin3_dff_dirty_free(odin3_dff_dirty *dd) {
    odin3_util_free(dd->marks);
    dd->marks = NULL;
    dd->n_marks = 0;
    dd->active = false;
}

odin3_status odin3_dff_dirty_begin(odin3_dff_state *ds) {
    odin3_dff_dirty *dd = &ds->dirty;
    odin3_opt_ctx *oc = ds->oc;
    odin3_dff_dirty_free(dd);
    if (!odin3_opt_dff_primed(oc)) {
        (void)odin3_opt_dff_prime(
            oc); /* the first sweep is full; changes from here are journaled */
        return ODIN3_OK;
    }
    const uint64_t *entries = NULL;
    size_t count = 0;
    if (!odin3_module_journal_get(oc->module, &entries, &count)) {
        (void)odin3_opt_dff_prime(oc); /* overflowed: a full sweep, a fresh journal */
        return ODIN3_OK;
    }
    dd->n_marks = odin3_module_node_end(oc->module);
    dd->marks = odin3_util_calloc((size_t)dd->n_marks + 1U);
    if (dd->marks == NULL) {
        dd->n_marks = 0;
        (void)odin3_opt_dff_prime(oc);
        return ODIN3_OK; /* out of memory: a full sweep is always right */
    }
    walker wk;
    walker_init(&wk, ds, dd);
    odin3_status st = walk_entries(&wk, entries, count);
    walker_free(&wk);
    if (st != ODIN3_OK) {
        odin3_dff_dirty_free(dd);
        (void)odin3_opt_dff_prime(oc);
        return ODIN3_OK; /* a failed walk (out of memory) is no reason to fail: sweep in full */
    }
    odin3_module_journal_drop(oc->module, count);
    clear_followed(dd);
    dd->first_new = dd->n_marks;
    dd->active = true;
    return ODIN3_OK;
}

bool odin3_dff_dirty_visit(const odin3_dff_state *ds, odin3_node_id node) {
    const odin3_dff_dirty *dd = &ds->dirty;
    return !dd->active || node.v >= dd->first_new || (dd->marks[node.v] & MARK_FF) != 0;
}

odin3_status odin3_dff_dirty_extend(odin3_dff_state *ds) {
    odin3_dff_dirty *dd = &ds->dirty;
    const odin3_opt_ctx *oc = ds->oc;
    if (!dd->active) {
        return ODIN3_OK;
    }
    const uint64_t *entries = NULL;
    size_t count = 0;
    bool valid = odin3_module_journal_get(oc->module, &entries, &count);
    uint32_t end = odin3_module_node_end(oc->module);
    uint8_t *grown = valid ? odin3_util_realloc(dd->marks, (size_t)end + 1U) : NULL;
    if (grown == NULL) {
        dd->active = false; /* every FF is visited */
        return ODIN3_OK;
    }
    memset(grown + dd->n_marks + 1U, 0, (size_t)end - dd->n_marks);
    dd->marks = grown;
    dd->n_marks = end;
    clear_followed(dd);
    walker wk;
    walker_init(&wk, ds, dd);
    odin3_status st = walk_entries(&wk, entries, count);
    walker_free(&wk);
    dd->active = dd->active && st == ODIN3_OK;
    return ODIN3_OK;
}
