/*
 * clean.c — opt_clean (2F spec OP-10, §10): Yosys 0.55 passes/opt/opt_clean.cc rmunused_module over
 * Odin III's nets. The order per module is: buffers become wiring; mark-and-sweep liveness and
 * the deletion of dead nodes (a dead FF's init goes with it); wires, then the nets they leave
 * (one sweep, where §10's numbering would leave the nets of a deleted wire for the next call);
 * init cleanup last, as §10 step 6, so a wire kept for its init loses it in the same call and
 * goes in the next iteration.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LABEL_MAX = 256, CONST_PREFIX_LEN = 7 };

/* --- cell type -> module (instances), the keep cache ----------------------------------------- */

/* module[t] is the ID of the module whose cell type has ID t (0: none). */
typedef struct type_map {
    uint32_t *module;
    uint32_t n;
} type_map;

static void type_map_free(type_map *tm) {
    odin3_util_free(tm->module);
    tm->module = NULL;
    tm->n = 0;
}

static odin3_status type_map_build(odin3_design *design, type_map *tm) {
    uint32_t end = odin3_design_module_end(design);
    uint32_t size = 1;
    for (uint32_t id = 1; id < end; id++) {
        odin3_celltype_id type =
            odin3_module_celltype(odin3_module_get(design, (odin3_module_id){id}));
        size = type.v >= size ? type.v + 1 : size;
    }
    tm->module = odin3_util_calloc(sizeof *tm->module * (size_t)size);
    if (tm->module == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    tm->n = size;
    for (uint32_t id = 1; id < end; id++) {
        odin3_celltype_id type =
            odin3_module_celltype(odin3_module_get(design, (odin3_module_id){id}));
        tm->module[type.v] = id;
    }
    return ODIN3_OK;
}

/* The module a node instantiates (0: none, or a registry or black-box type). */
static uint32_t instance_of(const type_map *tm, odin3_celltype_id type) {
    return type.v < tm->n ? tm->module[type.v] : 0;
}

/* A kit view of module m: odin3_kit_keep reads only the design and the module. */
static odin3_opt_ctx keep_view(odin3_design *design, odin3_module *module) {
    odin3_opt_ctx oc;
    memset(&oc, 0, sizeof oc);
    oc.design = design;
    oc.module = module;
    return oc;
}

/* The module has `keep`, or a node or wire of it has. */
static bool module_own_keep(odin3_design *design, odin3_module *module) {
    odin3_opt_ctx oc = keep_view(design, module);
    odin3_objref self = {ODIN3_OBJ_MODULE, odin3_module_id_of(module).v};
    if (odin3_kit_keep(&oc, self)) {
        return true;
    }
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_objref obj = {ODIN3_OBJ_NODE, id};
        if (odin3_node_live(module, (odin3_node_id){id}) && odin3_kit_keep(&oc, obj)) {
            return true;
        }
    }
    for (uint32_t id = 1; id < odin3_module_wire_end(module); id++) {
        odin3_objref obj = {ODIN3_OBJ_WIRE, id};
        if (odin3_wire_live(module, (odin3_wire_id){id}) && odin3_kit_keep(&oc, obj)) {
            return true;
        }
    }
    return false;
}

typedef struct inst_edge {
    uint32_t child, parent;
} inst_edge;

/* One edge per instance in module parent: child module -> parent (repeats in a row dropped). */
static odin3_status collect_module_edges(const odin3_module *module, uint32_t parent,
                                         const type_map *tm, odin3_vec *edges) {
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        uint32_t child =
            odin3_node_live(module, node) ? instance_of(tm, odin3_node_type(module, node)) : 0;
        const inst_edge *last = edges->len > 0 ? odin3_vec_cat(edges, edges->len - 1) : NULL;
        bool repeat = last != NULL && last->child == child && last->parent == parent;
        if (child == 0 || child == parent || repeat) {
            continue;
        }
        inst_edge *edge = odin3_vec_push(edges);
        if (edge == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        edge->child = child;
        edge->parent = parent;
    }
    return ODIN3_OK;
}

static odin3_status collect_edges(odin3_design *design, const type_map *tm, odin3_vec *edges) {
    for (uint32_t parent = 1; parent < odin3_design_module_end(design); parent++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){parent});
        odin3_status st = collect_module_edges(module, parent, tm, edges);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Spreads keep from children to parents until nothing changes (no recursion). */
static void spread_keep(bool *flags, const odin3_vec *edges) {
    bool again = true;
    while (again) {
        again = false;
        for (size_t i = 0; i < edges->len; i++) {
            const inst_edge *edge = odin3_vec_cat(edges, i);
            if (flags[edge->child] && !flags[edge->parent]) {
                flags[edge->parent] = true;
                again = true;
            }
        }
    }
}

odin3_status odin3_opt_keep_modules(odin3_design *design, odin3_vec *keep) {
    if (design == NULL || keep == NULL || keep->elem_size != sizeof(bool)) {
        odin3_log(ODIN3_LOG_ERROR, "opt_clean: keep_modules: no design or no bool vector");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec_clear(keep);
    uint32_t end = odin3_design_module_end(design);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 0; id < end && st == ODIN3_OK; id++) {
        bool *flag = odin3_vec_push(keep);
        st = flag == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (st == ODIN3_OK && id > 0) {
            *flag = module_own_keep(design, odin3_module_get(design, (odin3_module_id){id}));
        }
    }
    type_map tm = {NULL, 0};
    odin3_vec edges;
    odin3_vec_init(&edges, sizeof(inst_edge));
    st = st == ODIN3_OK ? type_map_build(design, &tm) : st;
    st = st == ODIN3_OK ? collect_edges(design, &tm, &edges) : st;
    if (st == ODIN3_OK) {
        spread_keep(keep->data, &edges);
    } else {
        odin3_vec_clear(keep);
    }
    odin3_vec_free(&edges);
    type_map_free(&tm);
    return st;
}

/* --- one module --------------------------------------------------------------------------------
 */

/* A $memwr node of the module, chained to the previous one of its memory (1-based; 0 ends). */
typedef struct memwr_link {
    odin3_node_id node;
    uint32_t next;
} memwr_link;

typedef struct clean_shared {
    const bool *keep_module;
    const type_map *tm;
    uint32_t buf_type, memrd_type, memwr_type; /* 0: the design has no such type */
} clean_shared;

/* What one module (or the whole call) removed. */
typedef struct clean_tally {
    uint32_t cells, nets, wires;
} clean_tally;

typedef struct clean_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    const clean_shared *sh;
    uint8_t *node_live, *net_live;
    uint8_t *net_const; /* nets that were constant-driven before the sweep */
    odin3_node_id *stack;
    uint32_t sp;
    odin3_u64map *memwr_head; /* MEMID string -> index+1 of the newest $memwr; 0 once marked */
    odin3_vec memwr;          /* memwr_link */
    clean_tally t;
} clean_state;

static void note_change(clean_state *cs) {
    cs->oc->changed = true;
    odin3_kit_note_change(cs->oc);
    cs->oc->counts->clean_changes++;
}

static const char *name_text(const clean_state *cs, uint32_t str) {
    return str == 0 ? NULL : odin3_strtab_get(odin3_design_strtab(cs->oc->design), str);
}

static bool node_keep(const clean_state *cs, odin3_node_id node) {
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    return odin3_kit_keep(cs->oc, obj);
}

/* Step 1: `$_BUF_` nodes without `keep` become wiring (Yosys removes $pos/$_BUF_ first). */
static odin3_status unbuffer(clean_state *cs) {
    if (cs->sh->buf_type == 0) {
        return ODIN3_OK;
    }
    enum { PORT_A = 0, PORT_Y = 1 };
    for (uint32_t id = 1; id < odin3_module_node_end(cs->m); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(cs->m, node) || odin3_node_type(cs->m, node).v != cs->sh->buf_type ||
            node_keep(cs, node)) {
            continue;
        }
        odin3_net_id src = odin3_pin_net(cs->m, odin3_node_port(cs->m, node, PORT_A).first);
        if (!odin3_net_valid(src)) {
            continue; /* nothing to wire Y to: a normal cell, removed when dead */
        }
        odin3_kit_conn conn = {node, PORT_Y, &src, 1};
        odin3_status st = odin3_kit_connect(cs->oc, &conn);
        if (st != ODIN3_OK) {
            return st;
        }
        cs->oc->counts->clean_changes++;
    }
    return ODIN3_OK;
}

/* --- step 2: liveness ---------------------------------------------------------------------------
 */

static void mark_node(clean_state *cs, odin3_node_id node) {
    if (cs->node_live[node.v] == 0) {
        cs->node_live[node.v] = 1;
        cs->stack[cs->sp++] = node;
    }
}

/* A live net makes the nodes that drive it live. */
static void mark_net(clean_state *cs, odin3_net_id net) {
    if (!odin3_net_valid(net) || cs->net_live[net.v] != 0) {
        return;
    }
    cs->net_live[net.v] = 1;
    odin3_pinlist pins = odin3_net_pins(cs->m, net);
    uint32_t drivers = odin3_net_driver_count(cs->m, net);
    for (uint32_t i = 0; i < drivers && i < pins.count; i++) {
        mark_node(cs, odin3_pin_node(cs->m, pins.pins[i]));
    }
}

static bool memid_of(const clean_state *cs, odin3_node_id node, uint32_t *memid) {
    const odin3_value *val = odin3_node_param(cs->m, node, 0);
    if (val == NULL || val->kind != ODIN3_VAL_STRING) {
        return false;
    }
    *memid = val->str;
    return true;
}

static odin3_status add_memwr(clean_state *cs, odin3_node_id node) {
    uint32_t memid = 0;
    uint64_t head = 0;
    if (!memid_of(cs, node, &memid)) {
        return ODIN3_OK;
    }
    (void)odin3_u64map_get(cs->memwr_head, memid, &head);
    memwr_link *link = odin3_vec_push(&cs->memwr);
    if (link == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    link->node = node;
    link->next = (uint32_t)head;
    odin3_kv entry = {memid, cs->memwr.len};
    return odin3_u64map_put(cs->memwr_head, entry);
}

/* Yosys mem2cells: a live $memrd keeps every $memwr of its memory. */
static odin3_status mark_memwr(clean_state *cs, odin3_node_id memrd) {
    uint32_t memid = 0;
    uint64_t head = 0;
    if (!memid_of(cs, memrd, &memid) || !odin3_u64map_get(cs->memwr_head, memid, &head)) {
        return ODIN3_OK;
    }
    for (uint64_t at = head; at != 0;) {
        const memwr_link *link = odin3_vec_cat(&cs->memwr, (size_t)at - 1);
        mark_node(cs, link->node);
        at = link->next;
    }
    odin3_kv done = {memid, 0};
    return odin3_u64map_put(cs->memwr_head, done);
}

/* The roots among the nodes: ports, `keep` nodes, instances of `keep` modules. */
static odin3_status scan_node_roots(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_node_end(cs->m); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(cs->m, node)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(cs->m, node);
        const odin3_celltype_def *def = odin3_celltype_get(cs->oc->design, type);
        uint32_t inst = instance_of(cs->sh->tm, type);
        if ((def != NULL && def->gran == ODIN3_GRAN_PORT) || node_keep(cs, node) ||
            (inst != 0 && cs->sh->keep_module[inst])) {
            mark_node(cs, node);
        } else if (type.v == cs->sh->memwr_type && add_memwr(cs, node) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* Marks the nets driven by a constant node; the sweep is about to delete the dead ones. */
static void mark_const_nets(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_net_end(cs->m); id++) {
        odin3_net_id net = {id};
        if (odin3_net_live(cs->m, net) && odin3_net_const_value(cs->m, net) != ODIN3_CONST_NONE) {
            cs->net_const[id] = 1;
        }
    }
}

/* A public wire that names a constant stays without -purge (Yosys keeps `connect c 1'0`), so its
 * constant driver stays too: a later run then keeps the wire again. */
static bool names_constant(const clean_state *cs, odin3_wire_id wire, uint32_t bit) {
    const char *text = name_text(cs, odin3_wire_name(cs->m, wire));
    odin3_net_id net = odin3_wire_net(cs->m, wire, bit);
    return !cs->oc->opts->purge && text != NULL && text[0] != '$' && odin3_net_valid(net) &&
           cs->net_const[net.v] != 0;
}

/* The roots among the wires: every net of a `keep` wire, and the constants public wires name. */
static void scan_wire_roots(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_wire_end(cs->m); id++) {
        odin3_wire_id wire = {id};
        odin3_objref obj = {ODIN3_OBJ_WIRE, id};
        if (!odin3_wire_live(cs->m, wire)) {
            continue;
        }
        bool keep = odin3_kit_keep(cs->oc, obj);
        for (uint32_t bit = 0; bit < odin3_wire_width(cs->m, wire); bit++) {
            if (keep || names_constant(cs, wire, bit)) {
                mark_net(cs, odin3_wire_net(cs->m, wire, bit));
            }
        }
    }
}

/* A live node makes the nets on its input pins live; a $memrd also its memory's writes. */
static odin3_status drain(clean_state *cs) {
    while (cs->sp > 0) {
        odin3_node_id node = cs->stack[--cs->sp];
        odin3_pinslice pins = odin3_node_pins(cs->m, node);
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_pin_id pin = {pins.first.v + k};
            if (odin3_pin_reads(cs->m, pin)) {
                mark_net(cs, odin3_pin_net(cs->m, pin));
            }
        }
        if (odin3_node_type(cs->m, node).v == cs->sh->memrd_type &&
            mark_memwr(cs, node) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* --- step 3: deleting dead nodes ------------------------------------------------------------- */

/* A dead FF's Q init bits go with it (Yosys ffinit.remove_init). */
static odin3_status drop_ff_init(clean_state *cs, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(cs->m, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_net_id net = odin3_pin_net(cs->m, pin);
        if (odin3_pin_drives(cs->m, pin) && odin3_net_valid(net)) {
            odin3_status st = odin3_ffinit_remove(cs->m, net, NULL);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static void log_removed(const clean_state *cs, odin3_node_id node, const odin3_celltype_def *def) {
    char label[LABEL_MAX];
    odin3_kit_node_label(cs->oc, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "opt_clean: module '%s': removing unused %s '%s'",
              name_text(cs, odin3_module_name(cs->m)), def->name, label);
}

static odin3_status remove_dead(clean_state *cs, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(cs->m, node);
    const odin3_celltype_def *def = odin3_celltype_get(cs->oc->design, type);
    /* Constants are not cells in Yosys: their removal is no removal there (carry M-d). */
    bool constant = def != NULL && strncmp(def->name, "$_CONST", CONST_PREFIX_LEN) == 0;
    if (def == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!constant && odin3_log_get_level() >= ODIN3_LOG_DEBUG) {
        log_removed(cs, node, def);
    }
    odin3_status st = odin3_ff_is_ff(cs->oc, type) ? drop_ff_init(cs, node) : ODIN3_OK;
    st = st == ODIN3_OK ? odin3_node_delete(cs->m, node) : st;
    if (st == ODIN3_OK && !constant) {
        cs->t.cells++;
        note_change(cs);
    }
    return st;
}

static odin3_status sweep(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_node_end(cs->m); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(cs->m, node) && cs->node_live[id] == 0) {
            odin3_status st = remove_dead(cs, node);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* --- step 6: init bits --------------------------------------------------------------------------
 */

/*
 * Yosys 0.55 rmunused_module_signals: an init on a net that was constant-driven goes, whatever its
 * value; a net with several defined init memberships keeps one (primary first). Both silent: no
 * counter, no change.
 */
static odin3_status clean_init(clean_state *cs) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(cs->oc->design), odin3_bytes_cstr("init"), &key)) {
        return ODIN3_OK; /* "init" never interned: no net has an init, ffinit_get is X for all */
    }
    for (uint32_t id = 1; id < odin3_module_net_end(cs->m); id++) {
        odin3_net_id net = {id};
        odin3_bit init = odin3_ffinit_get_key(cs->m, net, key);
        if (init != ODIN3_BIT_0 && init != ODIN3_BIT_1) {
            continue;
        }
        odin3_status st = cs->net_const[id] != 0 ? odin3_ffinit_remove(cs->m, net, NULL)
                                                 : odin3_ffinit_set(cs->m, net, init);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- step 5: wires
 * --------------------------------------------------------------------------------- */

static bool wire_has_init(const clean_state *cs, odin3_wire_id wire) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(cs->oc->design), odin3_bytes_cstr("init"), &key)) {
        return false;
    }
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    const odin3_value *val = odin3_attr_get(cs->m, obj, key);
    if (val == NULL || val->kind != ODIN3_VAL_BITS) {
        return false;
    }
    for (uint32_t k = 0; k < val->len; k++) {
        if (val->bits[k] == ODIN3_BIT_0 || val->bits[k] == ODIN3_BIT_1) {
            return true;
        }
    }
    return false;
}

static bool wire_unused(const clean_state *cs, odin3_wire_id wire) {
    for (uint32_t bit = 0; bit < odin3_wire_width(cs->m, wire); bit++) {
        odin3_net_id net = odin3_wire_net(cs->m, wire, bit);
        if (odin3_net_valid(net) && odin3_net_pins(cs->m, net).count > 0) {
            return false;
        }
    }
    return true;
}

/* Yosys's public names, as 2F §10 has them: named, and not starting with `$`. */
static bool wire_public(const clean_state *cs, odin3_wire_id wire) {
    const char *text = name_text(cs, odin3_wire_name(cs->m, wire));
    return text != NULL && text[0] != '$';
}

/* -purge: a public wire may go unless it is the primary name of a net that still has pins. */
static bool wire_purgeable(const clean_state *cs, odin3_wire_id wire) {
    if (!wire_public(cs, wire)) {
        return false;
    }
    for (uint32_t bit = 0; bit < odin3_wire_width(cs->m, wire); bit++) {
        odin3_net_id net = odin3_wire_net(cs->m, wire, bit);
        odin3_wirebit prim =
            odin3_net_valid(net) ? odin3_net_primary(cs->m, net) : (odin3_wirebit){{0}, 0};
        if (odin3_net_valid(net) && odin3_net_pins(cs->m, net).count > 0 && prim.wire.v == wire.v &&
            prim.bit == bit) {
            return false;
        }
    }
    return true;
}

/* True when the net has a wire membership other than bit `bit` of `wire`. */
static bool net_has_other_wire(const clean_state *cs, odin3_net_id net, odin3_wire_id wire,
                               uint32_t bit) {
    odin3_wirebit prim = odin3_net_primary(cs->m, net);
    if (odin3_wire_valid(prim.wire) && (prim.wire.v != wire.v || prim.bit != bit)) {
        return true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(cs->m, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire) && (alias.wb.wire.v != wire.v || alias.wb.bit != bit)) {
            return true;
        }
    }
    return false;
}

/* Without -purge (Yosys `s1 != s2`): a public wire that aliases a wire or was a constant stays. */
static bool wire_keeps_name(const clean_state *cs, odin3_wire_id wire) {
    if (!wire_public(cs, wire)) {
        return false;
    }
    for (uint32_t bit = 0; bit < odin3_wire_width(cs->m, wire); bit++) {
        odin3_net_id net = odin3_wire_net(cs->m, wire, bit);
        if (odin3_net_valid(net) &&
            (cs->net_const[net.v] != 0 || net_has_other_wire(cs, net, wire, bit))) {
            return true;
        }
    }
    return false;
}

static bool wire_deletable(const clean_state *cs, odin3_wire_id wire) {
    if (cs->oc->opts->purge) {
        return wire_unused(cs, wire) || wire_purgeable(cs, wire);
    }
    return wire_unused(cs, wire) && !wire_keeps_name(cs, wire);
}

static odin3_status clean_wires(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_wire_end(cs->m); id++) {
        odin3_wire_id wire = {id};
        odin3_objref obj = {ODIN3_OBJ_WIRE, id};
        if (!odin3_wire_live(cs->m, wire) || odin3_node_valid(odin3_wire_port_node(cs->m, wire)) ||
            odin3_kit_keep(cs->oc, obj) || wire_has_init(cs, wire)) {
            continue;
        }
        if (!wire_deletable(cs, wire)) {
            continue;
        }
        odin3_status st = odin3_wire_delete(cs->m, wire);
        if (st != ODIN3_OK) {
            return st;
        }
        cs->t.wires++;
        note_change(cs);
    }
    return ODIN3_OK;
}

/* --- step 4: nets --------------------------------------------------------------------------------
 */

/* Nets with no pins, no wire membership and no aliases; no Yosys counterpart, so no change. */
static odin3_status clean_nets(clean_state *cs) {
    for (uint32_t id = 1; id < odin3_module_net_end(cs->m); id++) {
        odin3_net_id net = {id};
        if (!odin3_net_live(cs->m, net) || odin3_net_pins(cs->m, net).count > 0 ||
            odin3_wire_valid(odin3_net_primary(cs->m, net).wire) ||
            odin3_net_alias_count(cs->m, net) > 0) {
            continue;
        }
        odin3_status st = odin3_net_delete(cs->m, net);
        if (st != ODIN3_OK) {
            return st;
        }
        cs->t.nets++;
    }
    return ODIN3_OK;
}

/* --- the driver
 * ------------------------------------------------------------------------------------ */

static odin3_status stages(clean_state *cs) {
    mark_const_nets(cs);
    odin3_status st = scan_node_roots(cs);
    if (st == ODIN3_OK) {
        scan_wire_roots(cs);
        st = drain(cs);
    }
    st = st == ODIN3_OK ? sweep(cs) : st;
    st = st == ODIN3_OK ? clean_init(cs) : st;
    st = st == ODIN3_OK ? clean_wires(cs) : st;
    return st == ODIN3_OK ? clean_nets(cs) : st;
}

static odin3_status clean_module(odin3_opt_ctx *oc, const clean_shared *sh, clean_tally *out) {
    clean_state cs;
    memset(&cs, 0, sizeof cs);
    cs.oc = oc;
    cs.m = oc->module;
    cs.sh = sh;
    odin3_status st = unbuffer(&cs);
    uint32_t node_end = odin3_module_node_end(cs.m);
    odin3_vec_init(&cs.memwr, sizeof(memwr_link));
    cs.node_live = odin3_util_calloc((size_t)node_end);
    cs.net_live = odin3_util_calloc((size_t)odin3_module_net_end(cs.m));
    cs.net_const = odin3_util_calloc((size_t)odin3_module_net_end(cs.m));
    cs.stack = odin3_util_calloc(sizeof *cs.stack * (size_t)node_end);
    cs.memwr_head = odin3_u64map_create(0);
    if (st == ODIN3_OK && (cs.node_live == NULL || cs.net_live == NULL || cs.net_const == NULL ||
                           cs.stack == NULL || cs.memwr_head == NULL)) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    st = st == ODIN3_OK ? stages(&cs) : st;
    *out = cs.t;
    odin3_util_free(cs.node_live);
    odin3_util_free(cs.net_live);
    odin3_util_free(cs.net_const);
    odin3_util_free(cs.stack);
    odin3_u64map_destroy(cs.memwr_head);
    odin3_vec_free(&cs.memwr);
    return st;
}

typedef struct clean_call {
    odin3_pass_ctx *ctx;
    const odin3_opt_opts *opts;
    odin3_opt_counts *counts;
    clean_shared shared;
} clean_call;

/* Cleans every module without processes; adds this call's removals into total. */
static odin3_status clean_modules(odin3_design *design, const clean_call *call,
                                  clean_tally *total) {
    const odin3_strtab *strtab = odin3_design_strtab(design);
    for (uint32_t id = 1; id < odin3_design_module_end(design); id++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        if (odin3_module_proc_count(module) > 0) {
            odin3_log(ODIN3_LOG_INFO, "opt_clean: module '%s' has processes; skipped",
                      odin3_strtab_get(strtab, odin3_module_name(module)));
            continue;
        }
        odin3_opt_ctx oc;
        odin3_status st = odin3_opt_ctx_init(&oc, call->ctx, module, call->opts);
        if (st != ODIN3_OK) {
            return st;
        }
        oc.counts = call->counts;
        clean_tally tally = {0, 0, 0};
        st = clean_module(&oc, &call->shared, &tally);
        odin3_opt_ctx_free(&oc);
        total->cells += tally.cells;
        total->nets += tally.nets;
        total->wires += tally.wires;
        call->counts->clean_cells += tally.cells;
        call->counts->clean_nets += tally.nets;
        call->counts->clean_wires += tally.wires;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_opt_clean_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_clean: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(bool));
    type_map tm = {NULL, 0};
    odin3_status st = odin3_opt_keep_modules(design, &keep);
    st = st == ODIN3_OK ? type_map_build(design, &tm) : st;
    clean_tally total = {0, 0, 0};
    if (st == ODIN3_OK) {
        clean_call call = {ctx, opts, counts, {keep.data, &tm, 0, 0, 0}};
        call.shared.buf_type = odin3_kit_type_named(design, "$_BUF_").v;
        call.shared.memrd_type = odin3_kit_type_named(design, "$memrd").v;
        call.shared.memwr_type = odin3_kit_type_named(design, "$memwr").v;
        st = clean_modules(design, &call, &total);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "opt_clean: removed %" PRIu32 " unused cells, %" PRIu32
                  " unused nets and %" PRIu32 " unused wires",
                  total.cells, total.nets, total.wires);
    }
    odin3_vec_free(&keep);
    type_map_free(&tm);
    return st;
}
