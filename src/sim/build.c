/* build.c — odin3_sim_build: expands module instances into flat cells over shared value slots. */
#include "build_internal.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- value slots: a union-find while instances are expanded ----------------------------------- */

static odin3_status new_slot(odin3_sim_builder *bld, uint32_t *slot) {
    if (bld->uf.len >= ODIN3_SIM_UNSET) {
        return ODIN3_ERR_NO_MEMORY; /* the slot space (32 bits) is exhausted */
    }
    *slot = (uint32_t)bld->uf.len;
    return odin3_sim_push_u32(&bld->uf, *slot);
}

static uint32_t uf_find(uint32_t *uf, uint32_t slot) {
    while (uf[slot] != slot) {
        uf[slot] = uf[uf[slot]];
        slot = uf[slot];
    }
    return slot;
}

/* Joins two slots; the smaller root stays the root (so every parent precedes its children). */
static void uf_union(odin3_sim_builder *bld, uint32_t one, uint32_t two) {
    uint32_t *uf = bld->uf.data;
    one = uf_find(uf, one);
    two = uf_find(uf, two);
    if (one < two) {
        uf[two] = one;
    } else {
        uf[one] = two;
    }
}

/* Renumbers the roots densely, in order: afterwards uf[s] is the final slot of s. */
static uint32_t uf_renumber(odin3_sim_builder *bld) {
    uint32_t *uf = bld->uf.data;
    uint32_t next = 0;
    for (uint32_t i = 0; i < bld->uf.len; i++) {
        uf[i] = uf[i] == i ? next++ : uf[uf[i]]; /* uf[i] < i is final already */
    }
    return next;
}

static uint32_t *map_at(odin3_sim_builder *bld, uint32_t frame, odin3_net_id net) {
    const odin3_sim_frame *fr = odin3_vec_cat(&bld->frames, frame);
    return (uint32_t *)bld->netmap.data + fr->map + net.v;
}

static const odin3_module *frame_module(const odin3_sim_builder *bld, uint32_t frame) {
    const odin3_sim_frame *fr = odin3_vec_cat(&bld->frames, frame);
    return odin3_module_get(bld->design, fr->module);
}

/* --- the budget: checked before the flattened design grows -------------------------------------
 */

/* What one step of expansion adds: cells/frames, and net-map entries, slots or pin bits. */
typedef struct odin3_sim_growth {
    uint64_t units;
    uint64_t bits;
} odin3_sim_growth;

/*
 * False (and logged) when adding units cells/frames and bits net-map entries, slots or pin bits
 * for node of frame would pass the budget.
 */
static bool within_budget(const odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                          odin3_sim_growth grow) {
    uint64_t units = (uint64_t)bld->cells.len + bld->frames.len + grow.units;
    uint64_t bits = (uint64_t)bld->netmap.len + bld->uf.len + bld->idx.len + grow.bits;
    if (units > bld->max_units) {
        odin3_sim_err_budget(bld, frame, node, "cells and instances", bld->max_units);
        return false;
    }
    if (bits > bld->max_bits) {
        odin3_sim_err_budget(bld, frame, node, "net and pin bits", bld->max_bits);
        return false;
    }
    return true;
}

/* --- frames: one per expanded module instance ------------------------------------------------- */

static odin3_status add_frame(odin3_sim_builder *bld, odin3_module_id module, uint32_t parent,
                              odin3_node_id inst) {
    uint32_t end = odin3_module_net_end(odin3_module_get(bld->design, module));
    /* Each net of the frame takes a net-map entry and, at most, one slot. */
    if (!within_budget(bld, parent, inst, (odin3_sim_growth){1, 2 * (uint64_t)end})) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (bld->frames.len >= ODIN3_SIM_UNSET ||
        odin3_vec_reserve(&bld->netmap, bld->netmap.len + end) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_sim_frame *fr = odin3_vec_push(&bld->frames);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *fr = (odin3_sim_frame){module, parent, inst, bld->netmap.len};
    for (uint32_t nid = 0; nid < end; nid++) {
        (void)odin3_sim_push_u32(&bld->netmap, ODIN3_SIM_UNSET); /* reserved: cannot fail */
    }
    return ODIN3_OK;
}

/*
 * Gives each port net of child frame the slot of the parent net on the instance pin (a fresh slot
 * for an open pin); a child net on two port bits joins both parent slots.
 */
static odin3_status bind_ports(odin3_sim_builder *bld, uint32_t child) {
    const odin3_sim_frame *fr = odin3_vec_cat(&bld->frames, child);
    uint32_t parent = fr->parent;
    odin3_node_id inst = fr->inst;
    const odin3_module *cmod = frame_module(bld, child);
    const odin3_module *pmod = frame_module(bld, parent);
    for (uint32_t port = 0; port < odin3_module_port_count(cmod); port++) {
        odin3_pinslice cpins = odin3_node_pins(cmod, odin3_module_port(cmod, port));
        odin3_pinslice ipins = odin3_node_port(pmod, inst, port);
        for (uint32_t k = 0; k < cpins.count && k < ipins.count; k++) {
            odin3_net_id cnet = odin3_pin_net(cmod, (odin3_pin_id){cpins.first.v + k});
            odin3_net_id pnet = odin3_pin_net(pmod, (odin3_pin_id){ipins.first.v + k});
            uint32_t slot = 0;
            if (!odin3_net_valid(cnet)) {
                continue;
            }
            if (odin3_net_valid(pnet)) {
                slot = *map_at(bld, parent, pnet);
            } else if (new_slot(bld, &slot) != ODIN3_OK) {
                return ODIN3_ERR_NO_MEMORY;
            }
            uint32_t *entry = map_at(bld, child, cnet);
            if (*entry == ODIN3_SIM_UNSET) {
                *entry = slot;
            } else {
                uf_union(bld, *entry, slot);
            }
        }
    }
    return ODIN3_OK;
}

/* Gives every live net of frame that has no slot yet (all but the port nets) a fresh one. */
static odin3_status fill_frame(odin3_sim_builder *bld, uint32_t frame) {
    const odin3_module *mod = frame_module(bld, frame);
    uint32_t end = odin3_module_net_end(mod);
    for (uint32_t nid = 1; nid < end; nid++) {
        odin3_net_id net = {nid};
        uint32_t slot = 0;
        if (!odin3_net_live(mod, net) || *map_at(bld, frame, net) != ODIN3_SIM_UNSET) {
            continue;
        }
        if (new_slot(bld, &slot) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *map_at(bld, frame, net) = slot;
    }
    return ODIN3_OK;
}

/* --- flat cells ------------------------------------------------------------------------------- */

/*
 * Appends the span and pin slots of port `port` of flat (an open input pin reads ZERO, an open
 * output pin writes DISCARD); *width gets the port's width.
 */
static odin3_status add_port_pins(odin3_sim_builder *bld, const odin3_sim_flat *flat, uint32_t port,
                                  uint32_t *width) {
    const odin3_module *mod = frame_module(bld, flat->frame);
    odin3_pinslice pins = odin3_node_port(mod, flat->node, port);
    uint32_t open =
        flat->def->ports[port].dir == ODIN3_DIR_IN ? ODIN3_SIM_SLOT_ZERO : ODIN3_SIM_SLOT_DISCARD;
    odin3_sim_span *span = odin3_vec_push(&bld->spans);
    if (span == NULL || bld->idx.len + pins.count >= ODIN3_SIM_UNSET ||
        odin3_vec_reserve(&bld->idx, bld->idx.len + pins.count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    span->idx = NULL; /* pointed at sim->idx when the build is finalized */
    span->width = pins.count;
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k});
        (void)odin3_sim_push_u32(&bld->idx,
                                 odin3_net_valid(net) ? *map_at(bld, flat->frame, net) : open);
    }
    *width = pins.count;
    return ODIN3_OK;
}

/* Appends the spans and pin slots of flat; *n_state gets its state size (output bits if seq). */
static odin3_status add_pins(odin3_sim_builder *bld, const odin3_sim_flat *flat,
                             uint32_t *n_state) {
    uint64_t outputs = 0;
    for (uint32_t port = 0; port < flat->def->n_ports; port++) {
        uint32_t width = 0;
        if (add_port_pins(bld, flat, port, &width) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        outputs += flat->def->ports[port].dir == ODIN3_DIR_IN ? 0 : width;
    }
    if (outputs > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *n_state = odin3_sim_is_seq(flat->def) ? (uint32_t)outputs : 0;
    return ODIN3_OK;
}

/*
 * Grows the shared scratch to what flat's type asks for (its sizing view: the port widths, the
 * parameters and the type data). A cell too large to simulate is reported.
 */
static odin3_status size_scratch(odin3_sim_builder *bld, const odin3_sim_flat *flat) {
    if (flat->def->sim_scratch_bytes == NULL) {
        return ODIN3_OK;
    }
    odin3_sim_cell sizing = flat->view;
    sizing.ports = (const odin3_sim_span *)bld->spans.data + flat->span_first;
    uint32_t bytes = 0;
    odin3_status st = flat->def->sim_scratch_bytes(&sizing, &bytes);
    if (st == ODIN3_ERR_INVALID_ARG) {
        odin3_sim_err_too_large(bld, flat->frame, flat->node);
    }
    if (st == ODIN3_OK && bytes > bld->n_scratch) {
        bld->n_scratch = bytes;
    }
    return st;
}

/* A black box is simulated like any other type when it has a simulate hook. */
static odin3_status add_flat(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             const odin3_celltype_def *def) {
    const odin3_module *mod = frame_module(bld, frame);
    if (def->simulate == NULL) {
        odin3_sim_err_unsupported(bld, frame, node);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!within_budget(bld, frame, node, (odin3_sim_growth){1, odin3_node_pins(mod, node).count})) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (bld->cells.len >= ODIN3_SIM_UNSET || bld->spans.len >= ODIN3_SIM_UNSET - def->n_ports ||
        bld->idx.len >= ODIN3_SIM_UNSET) {
        return ODIN3_ERR_NO_MEMORY; /* indices are 32 bits */
    }
    odin3_sim_flat *flat = odin3_vec_push(&bld->cells);
    if (flat == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *flat = (odin3_sim_flat){.def = def,
                             .module = odin3_module_id_of(mod),
                             .node = node,
                             .frame = frame,
                             .span_first = (uint32_t)bld->spans.len,
                             .idx_first = (uint32_t)bld->idx.len,
                             .state_first = bld->n_state};
    flat->view.n_ports = def->n_ports;
    flat->view.params = odin3_node_param(mod, node, 0);
    flat->view.type_data = odin3_celltype_lib(bld->design, odin3_node_type(mod, node));
    uint32_t n_state = 0;
    if (add_pins(bld, flat, &n_state) != ODIN3_OK || UINT32_MAX - bld->n_state < n_state) {
        return ODIN3_ERR_NO_MEMORY;
    }
    flat->view.n_state = n_state;
    bld->n_state += n_state;
    return size_scratch(bld, flat);
}

/* A node of frame: port nodes are the frame's boundary; an instance opens a frame. */
static odin3_status expand_node(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node) {
    const odin3_module *mod = frame_module(bld, frame);
    if (!odin3_node_live(mod, node)) {
        return ODIN3_OK;
    }
    const odin3_celltype_def *def = odin3_celltype_get(bld->design, odin3_node_type(mod, node));
    if (def->gran == ODIN3_GRAN_PORT) {
        return ODIN3_OK;
    }
    uint32_t child = odin3_sim_child_module(bld, mod, node);
    if (child == 0) {
        return add_flat(bld, frame, node, def);
    }
    odin3_status st = add_frame(bld, (odin3_module_id){child}, frame, node);
    return st != ODIN3_OK ? st : bind_ports(bld, (uint32_t)bld->frames.len - 1);
}

/* Expands every frame in creation order (an explicit worklist: children are appended). */
static odin3_status expand(odin3_sim_builder *bld) {
    for (uint32_t fi = 0; fi < bld->frames.len; fi++) {
        odin3_status st = fill_frame(bld, fi);
        uint32_t end = odin3_module_node_end(frame_module(bld, fi));
        for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
            st = expand_node(bld, fi, (odin3_node_id){nid});
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- primary inputs and outputs --------------------------------------------------------------- */

static odin3_status add_top_port(odin3_sim_builder *bld, uint32_t index) {
    const odin3_module *top = frame_module(bld, 0);
    odin3_node_id pnode = odin3_module_port(top, index);
    const odin3_celltype_def *def = odin3_celltype_get(bld->design, odin3_node_type(top, pnode));
    uint32_t name = odin3_wire_name(top, odin3_module_port_wire(top, index));
    if (def->ports[0].dir == ODIN3_DIR_INOUT) {
        odin3_sim_err_inout(bld, name);
        return ODIN3_ERR_INVALID_ARG;
    }
    bool input = def->ports[0].dir == ODIN3_DIR_OUT; /* a $port_in drives the module's net */
    odin3_vec *bits = input ? &bld->in_bits : &bld->out_bits;
    odin3_pinslice pins = odin3_node_pins(top, pnode);
    odin3_sim_port *port = odin3_vec_push(input ? &bld->in_ports : &bld->out_ports);
    if (port == NULL || bits->len > UINT32_MAX - pins.count) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *port = (odin3_sim_port){name, (uint32_t)bits->len, pins.count};
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(top, (odin3_pin_id){pins.first.v + k});
        uint32_t slot = 0;
        if (odin3_net_valid(net)) {
            slot = *map_at(bld, 0, net);
        } else if (new_slot(bld, &slot) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (odin3_sim_push_u32(bits, slot) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

static odin3_status collect_ports(odin3_sim_builder *bld) {
    uint32_t count = odin3_module_port_count(frame_module(bld, 0));
    for (uint32_t i = 0; i < count; i++) {
        odin3_status st = add_top_port(bld, i);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- finalize: dense slots, owned arrays, the views' pointers --------------------------------- */

static void remap(const odin3_sim_builder *bld, const odin3_vec *vec) {
    const uint32_t *uf = bld->uf.data;
    uint32_t *slots = vec->data;
    for (size_t i = 0; i < vec->len; i++) {
        slots[i] = uf[slots[i]];
    }
}

/* Takes the data of vec (its length to *len when given), leaving vec empty. */
static void *take(odin3_vec *vec, uint32_t *len) {
    void *data = vec->data;
    if (len != NULL) {
        *len = (uint32_t)vec->len;
    }
    odin3_vec_init(vec, vec->elem_size);
    return data;
}

static odin3_status allocate(odin3_sim_builder *bld, odin3_sim *sim) {
    sim->n_values = uf_renumber(bld);
    remap(bld, &bld->idx);
    remap(bld, &bld->in_bits);
    remap(bld, &bld->out_bits);
    sim->values = odin3_util_calloc(sim->n_values);
    sim->n_state = bld->n_state;
    sim->n_scratch = bld->n_scratch;
    sim->state = bld->n_state > 0 ? odin3_util_calloc(bld->n_state) : NULL;
    sim->scratch = bld->n_scratch > 0 ? odin3_util_calloc(bld->n_scratch) : NULL;
    sim->in_clock = odin3_util_calloc(bld->in_bits.len > 0 ? bld->in_bits.len : 1);
    if (sim->values == NULL || (bld->n_state > 0 && sim->state == NULL) ||
        (bld->n_scratch > 0 && sim->scratch == NULL) || sim->in_clock == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sim->cells = take(&bld->cells, &sim->n_cells);
    sim->spans = take(&bld->spans, NULL);
    sim->idx = take(&bld->idx, NULL);
    sim->in_ports = take(&bld->in_ports, &sim->n_in_ports);
    sim->out_ports = take(&bld->out_ports, &sim->n_out_ports);
    sim->in_bits = take(&bld->in_bits, &sim->n_in_bits);
    sim->out_bits = take(&bld->out_bits, &sim->n_out_bits);
    return ODIN3_OK;
}

static void point_views(odin3_sim *sim) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        odin3_sim_flat *flat = &sim->cells[i];
        odin3_sim_span *spans = sim->spans + flat->span_first;
        uint32_t cursor = flat->idx_first;
        for (uint32_t port = 0; port < flat->view.n_ports; port++) {
            spans[port].idx = spans[port].width > 0 ? sim->idx + cursor : NULL;
            cursor += spans[port].width;
        }
        flat->view.values = sim->values;
        flat->view.ports = spans;
        flat->view.state = flat->view.n_state > 0 ? sim->state + flat->state_first : NULL;
        flat->view.scratch = sim->scratch;
        flat->view.n_scratch = sim->n_scratch;
    }
}

/* --- clocks ----------------------------------------------------------------------------------- */

/* Flags the inputs that clock edge-triggered cells; a clock that is not an input is an error. */
static odin3_status find_clocks(const odin3_sim_builder *bld, odin3_sim *sim) {
    uint32_t *input_of = odin3_util_malloc((size_t)sim->n_values * sizeof *input_of);
    if (input_of == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t slot_ix = 0; slot_ix < sim->n_values; slot_ix++) {
        input_of[slot_ix] = ODIN3_SIM_UNSET;
    }
    for (uint32_t k = sim->n_in_bits; k-- > 0;) {
        input_of[sim->in_bits[k]] = k; /* the first input bit on the slot */
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < sim->n_cells && st == ODIN3_OK; i++) {
        const odin3_sim_flat *flat = &sim->cells[i];
        if (!odin3_sim_is_edge(flat->def) || (flat->def->flags & ODIN3_CT_CLOCK_PIN0) == 0 ||
            flat->view.ports[0].width == 0) {
            continue;
        }
        uint32_t slot = flat->view.ports[0].idx[0];
        if (input_of[slot] == ODIN3_SIM_UNSET) {
            odin3_sim_err_clock(bld, flat, slot);
            st = ODIN3_ERR_INVALID_ARG;
        } else {
            sim->in_clock[input_of[slot]] = 1;
        }
    }
    odin3_util_free(input_of);
    return st;
}

/* --- drivers ---------------------------------------------------------------------------------- */

/* True for the pins of port `port` of flat that drive their slots as ordinary drivers. */
static bool ordinary_driver(const odin3_sim_flat *flat, uint32_t port) {
    return flat->def->ports[port].dir == ODIN3_DIR_OUT &&
           (flat->def->flags & ODIN3_CT_TRISTATE) == 0;
}

/* Counts one more driver of slot (saturating at 2); true when slot now has two. */
static bool add_driver(uint8_t *count, uint32_t slot) {
    if (slot == ODIN3_SIM_SLOT_DISCARD) {
        return false; /* every open output pin writes here; nothing reads it */
    }
    count[slot] = count[slot] < 2 ? (uint8_t)(count[slot] + 1) : count[slot];
    return count[slot] == 2;
}

/* Counts the ordinary drivers of flat; the first slot that reaches two, or ODIN3_SIM_UNSET. */
static uint32_t cell_drivers(const odin3_sim_flat *flat, uint8_t *count) {
    for (uint32_t port = 0; port < flat->view.n_ports; port++) {
        const odin3_sim_span *span = &flat->view.ports[port];
        for (uint32_t k = 0; ordinary_driver(flat, port) && k < span->width; k++) {
            if (add_driver(count, span->idx[k])) {
                return span->idx[k];
            }
        }
    }
    return ODIN3_SIM_UNSET;
}

/*
 * Rejects a slot with two ordinary drivers: outputs of non-tristate cells and primary inputs
 * (inout pins and tristate outputs form buses, which the IR allows to share a net).
 */
static odin3_status check_drivers(const odin3_sim_builder *bld, const odin3_sim *sim) {
    uint8_t *count = odin3_util_calloc(sim->n_values);
    if (count == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t clash = ODIN3_SIM_UNSET;
    for (uint32_t k = 0; k < sim->n_in_bits && clash == ODIN3_SIM_UNSET; k++) {
        clash = add_driver(count, sim->in_bits[k]) ? sim->in_bits[k] : clash;
    }
    for (uint32_t i = 0; i < sim->n_cells && clash == ODIN3_SIM_UNSET; i++) {
        clash = cell_drivers(&sim->cells[i], count);
    }
    odin3_util_free(count);
    if (clash != ODIN3_SIM_UNSET) {
        odin3_sim_err_drivers(bld, clash);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* --- build ------------------------------------------------------------------------------------ */

static odin3_status run(odin3_sim_builder *bld, odin3_module_id top, odin3_sim *sim) {
    uint32_t reserved = 0;
    odin3_status st = odin3_sim_check_hierarchy(bld, top);
    for (uint32_t i = 0; st == ODIN3_OK && i < ODIN3_SIM_SLOT_FIRST; i++) {
        st = new_slot(bld, &reserved);
    }
    st = st != ODIN3_OK ? st : add_frame(bld, top, ODIN3_SIM_UNSET, (odin3_node_id){0});
    st = st != ODIN3_OK ? st : expand(bld);
    st = st != ODIN3_OK ? st : collect_ports(bld);
    st = st != ODIN3_OK ? st : allocate(bld, sim);
    if (st != ODIN3_OK) {
        return st;
    }
    point_views(sim);
    st = check_drivers(bld, sim);
    st = st != ODIN3_OK ? st : find_clocks(bld, sim);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t loop_slot = 0;
    st = odin3_sim_levelize(sim, &loop_slot);
    if (st == ODIN3_ERR_INVALID_ARG) {
        odin3_sim_err_loop(bld, loop_slot);
    }
    if (st == ODIN3_OK) {
        odin3_sim_start(sim);
    }
    return st;
}

static void builder_init(odin3_sim_builder *bld, odin3_design *design) {
    *bld = (odin3_sim_builder){.design = design};
    odin3_vec_init(&bld->frames, sizeof(odin3_sim_frame));
    odin3_vec_init(&bld->netmap, sizeof(uint32_t));
    odin3_vec_init(&bld->uf, sizeof(uint32_t));
    odin3_vec_init(&bld->cells, sizeof(odin3_sim_flat));
    odin3_vec_init(&bld->spans, sizeof(odin3_sim_span));
    odin3_vec_init(&bld->idx, sizeof(uint32_t));
    odin3_vec_init(&bld->in_ports, sizeof(odin3_sim_port));
    odin3_vec_init(&bld->out_ports, sizeof(odin3_sim_port));
    odin3_vec_init(&bld->in_bits, sizeof(uint32_t));
    odin3_vec_init(&bld->out_bits, sizeof(uint32_t));
}

static void builder_free(odin3_sim_builder *bld) {
    odin3_u64map_destroy(bld->types);
    odin3_vec_free(&bld->frames);
    odin3_vec_free(&bld->netmap);
    odin3_vec_free(&bld->uf);
    odin3_vec_free(&bld->cells);
    odin3_vec_free(&bld->spans);
    odin3_vec_free(&bld->idx);
    odin3_vec_free(&bld->in_ports);
    odin3_vec_free(&bld->out_ports);
    odin3_vec_free(&bld->in_bits);
    odin3_vec_free(&bld->out_bits);
}

odin3_status odin3_sim_build(odin3_design *design, odin3_module_id top, odin3_sim **out) {
    return odin3_sim_build_opts(design, top, NULL, out);
}

odin3_status odin3_sim_build_opts(odin3_design *design, odin3_module_id top,
                                  const odin3_sim_options *opts, odin3_sim **out) {
    if (out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_sim_build: out is NULL");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (design == NULL || odin3_module_get(design, top) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_sim_build: no module %u to simulate", top.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_sim_builder bld;
    builder_init(&bld, design);
    uint32_t max_cells =
        opts != NULL && opts->max_cells != 0 ? opts->max_cells : ODIN3_SIM_DEFAULT_MAX_CELLS;
    bld.max_units = max_cells;
    bld.max_bits = (uint64_t)max_cells * ODIN3_SIM_BITS_PER_CELL;
    odin3_sim *sim = odin3_util_calloc(sizeof *sim);
    odin3_status st = sim == NULL ? ODIN3_ERR_NO_MEMORY : run(&bld, top, sim);
    builder_free(&bld);
    if (st != ODIN3_OK) {
        odin3_sim_destroy(sim);
        return st;
    }
    sim->design = design;
    *out = sim;
    return ODIN3_OK;
}

void odin3_sim_destroy(odin3_sim *sim) {
    if (sim == NULL) {
        return;
    }
    odin3_util_free(sim->values);
    odin3_util_free(sim->cells);
    odin3_util_free(sim->order);
    odin3_util_free(sim->spans);
    odin3_util_free(sim->idx);
    odin3_util_free(sim->state);
    odin3_util_free(sim->scratch);
    odin3_util_free(sim->in_ports);
    odin3_util_free(sim->out_ports);
    odin3_util_free(sim->in_bits);
    odin3_util_free(sim->in_clock);
    odin3_util_free(sim->out_bits);
    odin3_util_free(sim);
}
