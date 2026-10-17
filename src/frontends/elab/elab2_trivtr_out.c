/*
 * elab2_trivtr_out.c — a tri-state driver of a top-level output port under `--vtr-compat`: the
 * enable is dropped, as Parmys does (PHASE2 decision 28; Peter 2026-10-16; spec 2D 6.7).
 *
 * Yosys 0.55 (and so Parmys) reads `assign q = en ? d : 'bz` as a mux with an undefined input,
 * and `opt_expr -mux_undef` replaces it by `d`: the output is wired to the data and `en` is
 * unused (the golden of a 4-bit `q` is four `.names d~i q~i` buffers, `en` an unused input).
 * The default (IEEE) reading keeps the `$tribuf`, so the output is z while the enable is off.
 *
 * The rule applies to a driver that is the only one on its output bits. Several drivers of one
 * output, a driver on an inout port, a constant 0, x or z enable and a driver whose data is the
 * output of another such driver keep the `$tribuf`.
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
enum { TO_A = 0, TO_EN = 1, TO_Y = 2 };

static odin3_net_id slice_net(const odin3_elab2 *e2, odin3_pinslice slice, uint32_t bit) {
    return odin3_pin_net(e2->mod, (odin3_pin_id){slice.first.v + bit});
}

/* Bit `bit` of the A (data), EN or Y port of a $tribuf. */
static odin3_net_id to_a(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, uint32_t bit) {
    return slice_net(e2, odin3_node_port(e2->mod, rec.node, TO_A), bit);
}

static odin3_net_id to_en(const odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    return slice_net(e2, odin3_node_port(e2->mod, rec.node, TO_EN), 0);
}

static odin3_net_id to_y(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, uint32_t bit) {
    return slice_net(e2, odin3_node_port(e2->mod, rec.node, TO_Y), bit);
}

static uint32_t to_width(const odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    return odin3_node_port(e2->mod, rec.node, TO_Y).count;
}

/* A bit of Y that nothing else reads or drives: a bit the lvalue dropped. */
static bool dangling(const odin3_elab2 *e2, odin3_net_id net) {
    return odin3_net_pins(e2->mod, net).count <= 1U;
}

/* How many cells drive each Y net (dangling bits left out). */
static odin3_status count_drivers(const odin3_elab2 *e2, odin3_u64map *counts) {
    const odin3_elab2_tri_rec *recs = e2->drive->tribufs.data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < e2->drive->tribufs.len; i++) {
        for (uint32_t bit = 0, width = to_width(e2, recs[i]); st == ODIN3_OK && bit < width;
             bit++) {
            odin3_net_id net = to_y(e2, recs[i], bit);
            uint64_t seen = 0;
            if (!dangling(e2, net)) {
                (void)odin3_u64map_get(counts, net.v, &seen);
                st = odin3_u64map_put(counts, (odin3_kv){net.v, seen + 1U});
            }
        }
    }
    return st;
}

/* True when the enable is the constant 0, x or z: the cell is z or undefined for good, which the
 * enable-free reading would turn into the data. */
static bool enable_is_dead(const odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    odin3_net_id en = to_en(e2, rec);
    if (en.v == 0 || odin3_net_driver_count(e2->mod, en) != 1U) {
        return false;
    }
    odin3_celltype_id type =
        odin3_node_type(e2->mod, odin3_pin_node(e2->mod, odin3_net_driver(e2->mod, en)));
    return type.v == e2->types[ODIN3_E2T_CONST0].v || type.v == e2->types[ODIN3_E2T_CONSTX].v ||
           type.v == e2->types[ODIN3_E2T_CONSTZ].v;
}

/* True when every live Y bit of the cell is on a top-level output port that only this cell
 * drives, and one at least is; *outputs says how many bits that is. */
static bool drives_only_outputs(const odin3_elab2 *e2, const odin3_u64map *counts,
                                odin3_elab2_tri_rec rec, uint32_t *outputs) {
    *outputs = 0;
    for (uint32_t bit = 0, width = to_width(e2, rec); bit < width; bit++) {
        odin3_net_id net = to_y(e2, rec, bit);
        uint64_t drivers = 0;
        if (dangling(e2, net)) {
            continue;
        }
        (void)odin3_u64map_get(counts, net.v, &drivers);
        if (!odin3_elab2_net_on_top_output(e2, net) || drivers != 1U) {
            return false;
        }
        (*outputs)++;
    }
    return *outputs != 0;
}

/* True when no data bit is the Y net of a driver (a chain of tri-states: left alone). */
static bool data_is_free(const odin3_elab2 *e2, const odin3_u64map *counts,
                         odin3_elab2_tri_rec rec) {
    for (uint32_t bit = 0, width = to_width(e2, rec); bit < width; bit++) {
        if (odin3_u64map_get(counts, to_a(e2, rec, bit).v, NULL)) {
            return false;
        }
    }
    return true;
}

static bool eligible(const odin3_elab2 *e2, const odin3_u64map *counts, odin3_elab2_tri_rec rec) {
    uint32_t outputs = 0;
    return drives_only_outputs(e2, counts, rec, &outputs) && data_is_free(e2, counts, rec) &&
           !enable_is_dead(e2, rec);
}

/* --- the message ----------------------------------------------------------------------------- */

/* The name of the output port wire a net is on; NULL for none. */
static const char *port_name(const odin3_elab2 *e2, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(e2->mod, net);
    uint32_t ports = odin3_module_port_count(e2->mod);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_node_id node = odin3_pin_node(e2->mod, pins.pins[i]);
        for (uint32_t port = 0; port < ports; port++) {
            if (odin3_module_port(e2->mod, port).v == node.v) {
                odin3_wire_id wire = odin3_module_port_wire(e2->mod, port);
                return odin3_strtab_get(odin3_design_strtab(e2->design),
                                        odin3_wire_name(e2->mod, wire));
            }
        }
    }
    return NULL;
}

/* The port names of the cell's output bits, each once, comma separated. */
static odin3_status name_ports(const odin3_elab2 *e2, odin3_elab2_tri_rec rec, odin3_strbuf *list) {
    odin3_status st = ODIN3_OK;
    const char *last = NULL;
    for (uint32_t bit = 0, width = to_width(e2, rec); st == ODIN3_OK && bit < width; bit++) {
        odin3_net_id net = to_y(e2, rec, bit);
        const char *name = dangling(e2, net) ? NULL : port_name(e2, net);
        if (name != NULL && (last == NULL || strcmp(name, last) != 0)) {
            st = odin3_strbuf_appendf(list, "%s'%s'", last != NULL ? ", " : "", name);
            last = name;
        }
    }
    return st;
}

static odin3_status say(odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    odin3_strbuf ports;
    odin3_strbuf_init(&ports);
    odin3_status st = name_ports(e2, rec, &ports);
    if (st == ODIN3_OK) {
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, rec.at),
                         "tri-state driver of output port %s: --vtr-compat drops the enable and "
                         "wires the output to the data as Parmys does (without the option the "
                         "output is z while the enable is off)",
                         ports.data != NULL ? ports.data : "?");
    }
    odin3_strbuf_free(&ports);
    return st;
}

/* --- the change ------------------------------------------------------------------------------ */

/* Deletes the cell and shorts each live Y bit into its data bit. */
static odin3_status drop_enable(odin3_elab2 *e2, odin3_elab2_tri_rec rec) {
    uint32_t width = to_width(e2, rec);
    odin3_net_pair *pairs = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *pairs);
    if (pairs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t bit = 0; bit < width; bit++) {
        pairs[bit] = (odin3_net_pair){to_a(e2, rec, bit), to_y(e2, rec, bit)};
    }
    odin3_status st = odin3_node_delete(e2->mod, rec.node);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        bool same = pairs[bit].keep.v == pairs[bit].drop.v;
        st = same ? ODIN3_OK : odin3_elab2_merge(e2, pairs[bit]);
    }
    return st;
}

odin3_status odin3_elab2_tri_vtr_outputs(odin3_elab2 *e2) {
    const struct odin3_elab2_drive *dr = e2->drive;
    odin3_u64map *counts = odin3_u64map_create(0);
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(odin3_elab2_tri_rec));
    odin3_status st = counts != NULL ? count_drivers(e2, counts) : ODIN3_ERR_NO_MEMORY;
    const odin3_elab2_tri_rec *recs = dr->tribufs.data;
    for (size_t i = 0; st == ODIN3_OK && i < dr->tribufs.len; i++) {
        if (eligible(e2, counts, recs[i])) {
            odin3_elab2_tri_rec *slot = odin3_vec_push(&todo);
            st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
            if (slot != NULL) {
                *slot = recs[i];
            }
        }
    }
    const odin3_elab2_tri_rec *chosen = todo.data;
    for (size_t i = 0; st == ODIN3_OK && i < todo.len; i++) {
        st = say(e2, chosen[i]);
        st = st != ODIN3_OK ? st : drop_enable(e2, chosen[i]);
    }
    odin3_vec_free(&todo);
    odin3_u64map_destroy(counts);
    return st;
}
