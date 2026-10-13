/*
 * dirty.c — the opt session and opt_expr's dirty set (2F spec §19; plan Task 13): the per-module
 * marks the kit fills (odin3_kit_note_*) and opt_expr drains (odin3_opt_dirty_*). See dirty.h.
 *
 * A mark is two bits on a node: DIRTY (visit it) and RING (its readers are marked too, so marking
 * it again costs nothing). Marking never fails: when the mark array cannot grow the module falls
 * back to "all dirty", which opt_expr treats as a full sweep.
 */
#include "passes/opt/dirty.h"

#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * A mark is three bits on a node. DIRTY0 and DIRTY1 say it needs a visit in opt_expr's plain
 * rounds and in its x-consuming rounds (a rewrite marks both: the rounds of a call alternate, and
 * a node first seen in a plain round must still be seen in the x round). RING says its readers
 * are marked too, so marking it again costs nothing.
 */
enum {
    MARK_DIRTY0 = 1U,
    MARK_DIRTY1 = 2U,
    MARK_RING = 4U,
    MARK_BOTH = MARK_DIRTY0 | MARK_DIRTY1,
    MARK_ALL = MARK_BOTH | MARK_RING,
    N_PHASES = 2,
    GROW_SLACK = 64U
};

struct odin3_opt_modstate {
    uint8_t *bits;              /* one byte per node ID, bits[0] unused */
    size_t n_bits;              /* allocated length */
    uint32_t n_dirty[N_PHASES]; /* nodes with DIRTY0 / DIRTY1 set */
    bool all;                   /* every node counts as dirty (opt_expr sweeps) */
    bool changed;               /* a kit rewrite touched the module since the iteration began */
};

static struct {
    odin3_design *design;
    struct odin3_opt_modstate *mods; /* indexed by module ID */
    uint32_t n_mods;
    bool incremental;
} g_sess;

odin3_status odin3_opt_session_begin(odin3_design *design, bool incremental) {
    if (design == NULL || g_sess.design != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: session_begin: no design or a session is open");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t end = odin3_design_module_end(design);
    struct odin3_opt_modstate *mods = odin3_util_calloc(sizeof *mods * ((size_t)end + 1U));
    if (mods == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i <= end; i++) {
        mods[i].all = true;
    }
    g_sess.design = design;
    g_sess.mods = mods;
    g_sess.n_mods = end;
    g_sess.incremental = incremental;
    return ODIN3_OK;
}

void odin3_opt_session_end(void) {
    for (uint32_t i = 0; g_sess.mods != NULL && i <= g_sess.n_mods; i++) {
        odin3_util_free(g_sess.mods[i].bits);
    }
    odin3_util_free(g_sess.mods);
    memset(&g_sess, 0, sizeof g_sess);
}

bool odin3_opt_session_incremental(void) {
    return g_sess.design != NULL && g_sess.incremental;
}

void odin3_opt_session_new_iteration(void) {
    for (uint32_t i = 0; g_sess.mods != NULL && i <= g_sess.n_mods; i++) {
        g_sess.mods[i].changed = false;
    }
}

bool odin3_opt_session_module_changed(odin3_module_id module) {
    return g_sess.mods != NULL && module.v <= g_sess.n_mods && g_sess.mods[module.v].changed;
}

struct odin3_opt_modstate *odin3_opt_modstate_of(const odin3_design *design,
                                                 const odin3_module *module) {
    if (g_sess.design != design || g_sess.mods == NULL) {
        return NULL;
    }
    odin3_module_id id = odin3_module_id_of(module);
    return id.v <= g_sess.n_mods ? &g_sess.mods[id.v] : NULL;
}

/* --- marks ----------------------------------------------------------------------------------- */

/* Room for node's byte; false (and the module falls back to "all dirty") when it cannot grow. */
static bool mark_room(struct odin3_opt_modstate *mod, const odin3_module *module,
                      odin3_node_id node) {
    if ((size_t)node.v < mod->n_bits) {
        return true;
    }
    size_t want = (size_t)odin3_module_node_end(module) + GROW_SLACK;
    want = want > (size_t)node.v + 1U ? want : (size_t)node.v + 1U;
    uint8_t *grown = odin3_util_realloc(mod->bits, want);
    if (grown == NULL) {
        odin3_util_free(mod->bits);
        mod->bits = NULL;
        mod->n_bits = 0;
        mod->n_dirty[0] = mod->n_dirty[1] = 0;
        mod->all = true;
        return false;
    }
    memset(grown + mod->n_bits, 0, want - mod->n_bits);
    mod->bits = grown;
    mod->n_bits = want;
    return true;
}

/* Sets bits on node; returns the bits it had. */
static uint8_t mark_set(struct odin3_opt_modstate *mod, const odin3_module *module,
                        odin3_node_id node, uint8_t bits) {
    if (mod->all || !mark_room(mod, module, node)) {
        return MARK_ALL;
    }
    uint8_t had = mod->bits[node.v];
    for (uint32_t phase = 0; phase < N_PHASES; phase++) {
        uint8_t bit = (uint8_t)(MARK_DIRTY0 << phase);
        if ((had & bit) == 0 && (bits & bit) != 0) {
            mod->n_dirty[phase]++;
        }
    }
    mod->bits[node.v] = (uint8_t)(had | bits);
    return had;
}

/* The readers of every output net of node get DIRTY. */
static void mark_readers(struct odin3_opt_modstate *mod, const odin3_module *module,
                         odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    for (uint32_t k = 0; k < pins.count && !mod->all; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_net_id net = odin3_pin_net(module, pin);
        if (!odin3_pin_drives(module, pin) || !odin3_net_valid(net)) {
            continue;
        }
        odin3_pinlist sinks = odin3_net_sinks(module, net);
        for (uint32_t i = 0; i < sinks.count && !mod->all; i++) {
            (void)mark_set(mod, module, odin3_pin_node(module, sinks.pins[i]), MARK_BOTH);
        }
    }
}

/* node, and the readers of its outputs ("plus their readers"), unless it was marked that way. */
static void mark_node_ring(struct odin3_opt_modstate *mod, const odin3_module *module,
                           odin3_node_id node) {
    uint8_t had = mark_set(mod, module, node, MARK_ALL);
    if ((had & MARK_RING) == 0) {
        mark_readers(mod, module, node);
    }
}

void odin3_kit_note_change(odin3_opt_ctx *oc) {
    if (oc != NULL && oc->mod != NULL) {
        oc->mod->changed = true;
    }
}

void odin3_kit_note_node(odin3_opt_ctx *oc, odin3_node_id node) {
    if (oc == NULL || oc->mod == NULL) {
        return;
    }
    oc->mod->changed = true;
    if (g_sess.incremental && !oc->mod->all && odin3_node_live(oc->module, node)) {
        mark_node_ring(oc->mod, oc->module, node);
    }
}

void odin3_kit_note_net(odin3_opt_ctx *oc, odin3_net_id net) {
    if (oc == NULL || oc->mod == NULL) {
        return;
    }
    oc->mod->changed = true;
    if (!g_sess.incremental || oc->mod->all || !odin3_net_valid(net) ||
        !odin3_net_live(oc->module, net)) {
        return;
    }
    odin3_pinlist sinks = odin3_net_sinks(oc->module, net);
    for (uint32_t i = 0; i < sinks.count && !oc->mod->all; i++) {
        mark_node_ring(oc->mod, oc->module, odin3_pin_node(oc->module, sinks.pins[i]));
    }
}

void odin3_kit_note_outputs(odin3_opt_ctx *oc, odin3_node_id node) {
    if (oc == NULL || oc->mod == NULL || !odin3_node_live(oc->module, node)) {
        return;
    }
    odin3_pinslice pins = odin3_node_pins(oc->module, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (odin3_pin_drives(oc->module, pin)) {
            odin3_kit_note_net(oc, odin3_pin_net(oc->module, pin));
        }
    }
}

/* --- opt_expr's side ------------------------------------------------------------------------- */

bool odin3_opt_dirty_incremental(const odin3_opt_ctx *oc) {
    return oc != NULL && oc->mod != NULL && g_sess.incremental && !oc->mod->all;
}

bool odin3_opt_dirty_pending(const odin3_opt_ctx *oc, bool x_round) {
    return !odin3_opt_dirty_incremental(oc) || oc->mod->n_dirty[x_round ? 1 : 0] > 0;
}

bool odin3_opt_dirty_peek(const odin3_opt_ctx *oc, odin3_node_id node, bool x_round) {
    if (!odin3_opt_dirty_incremental(oc)) {
        return true;
    }
    uint8_t bit = (uint8_t)(x_round ? MARK_DIRTY1 : MARK_DIRTY0);
    return (size_t)node.v < oc->mod->n_bits && (oc->mod->bits[node.v] & bit) != 0;
}

bool odin3_opt_dirty_take(odin3_opt_ctx *oc, odin3_node_id node, bool x_round) {
    if (!odin3_opt_dirty_incremental(oc)) {
        return true;
    }
    if (!odin3_opt_dirty_peek(oc, node, x_round)) {
        return false;
    }
    struct odin3_opt_modstate *mod = oc->mod;
    uint8_t bit = (uint8_t)(x_round ? MARK_DIRTY1 : MARK_DIRTY0);
    mod->bits[node.v] = (uint8_t)(mod->bits[node.v] & ~(bit | MARK_RING));
    mod->n_dirty[x_round ? 1 : 0]--;
    return true;
}

void odin3_opt_dirty_settle(odin3_opt_ctx *oc) {
    if (oc == NULL || oc->mod == NULL) {
        return;
    }
    struct odin3_opt_modstate *mod = oc->mod;
    if (mod->bits != NULL) {
        memset(mod->bits, 0, mod->n_bits);
    }
    mod->n_dirty[0] = mod->n_dirty[1] = 0;
    mod->all = false;
}

uint64_t odin3_opt_dirty_bytes(const odin3_opt_ctx *oc) {
    return oc != NULL && oc->mod != NULL ? (uint64_t)oc->mod->n_bits : 0U;
}

void odin3_opt_dirty_sweep_all(odin3_opt_ctx *oc) {
    if (oc != NULL && oc->mod != NULL) {
        oc->mod->all = true;
    }
}
