/*
 * gl_net.c — the netlist adapter of the graphlets pass (spec section 2 and 3): the directed edges
 * of one module as driver node -> reader node, constants and (optionally) global nets left out.
 */
#include "ir/celltype.h"
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"

#include <ctype.h>
#include <string.h>

enum { LOW_MAX = 8 };

/*
 * A constant driver: a cell with at least one pin, no reading pin, and every pin on a constant net
 * (odin3_net_const_value): $_CONST0_ / $_CONST1_, a zero-input $sop or $lut. The same test the JSON
 * writer uses to leave such a cell out (backends/json/cells.c), so the graph matches
 * plugins/python/odin3_graph.py (E-4). A live $sop, $lut or gate is not constant even though its
 * type defines a const_value hook: the hook answers per parameters, and the net test decides.
 */
static bool node_is_constant(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    if (pins.count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(mod, pin);
        if (odin3_pin_reads(mod, pin) || !odin3_net_valid(net) ||
            odin3_net_const_value(mod, net) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

static bool node_eligible(const odin3_module *mod, odin3_node_id node) {
    if (!odin3_node_live(mod, node)) {
        return false;
    }
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
    return def != NULL && !node_is_constant(mod, node);
}

static bool name_is_clock(const char *name) {
    char low[LOW_MAX] = {0};
    size_t len = strlen(name);
    if (len == 0 || len >= sizeof low) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        low[i] = (char)tolower((unsigned char)name[i]);
    }
    return strcmp(low, "c") == 0 || strcmp(low, "clk") == 0 || strcmp(low, "clock") == 0;
}

/* A reader pin that is a clock: port 0 of a CLOCK_PIN0 cell, or a port named C / CLK / clock. */
static bool pin_is_clock(const odin3_module *mod, odin3_pin_id pin) {
    odin3_node_id node = odin3_pin_node(mod, pin);
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
    uint32_t port = odin3_pin_port(mod, pin);
    if (def == NULL || def->gran == ODIN3_GRAN_PORT || port >= def->n_ports) {
        return false;
    }
    if ((def->flags & ODIN3_CT_CLOCK_PIN0) != 0 && port == 0) {
        return true;
    }
    return name_is_clock(def->ports[port].name);
}

static bool net_is_global(const odin3_module *mod, odin3_net_id net, const gl_opts *opts) {
    odin3_pinlist pins = odin3_net_pins(mod, net);
    uint32_t readers = 0;
    for (uint32_t i = 0; i < pins.count; i++) {
        if (!odin3_pin_reads(mod, pins.pins[i])) {
            continue;
        }
        readers++;
        if (pin_is_clock(mod, pins.pins[i])) {
            return true;
        }
    }
    return readers >= opts->global_fanout;
}

/* One edge from node from to node to; none unless both are eligible (not constant drivers). */
static odin3_status add_edge(gl_mod *gm, uint32_t from, uint32_t to) {
    if (gm->eligible[from] == 0 || gm->eligible[to] == 0) {
        return ODIN3_OK;
    }
    glet_edge *slot = odin3_vec_push(&gm->edges);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (glet_edge){from, to};
    return ODIN3_OK;
}

static odin3_status add_net(gl_mod *gm, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(gm->mod, net);
    uint32_t drivers = odin3_net_driver_count(gm->mod, net);
    for (uint32_t dd = 0; dd < drivers; dd++) {
        uint32_t from = odin3_pin_node(gm->mod, pins.pins[dd]).v;
        for (uint32_t rr = 0; rr < pins.count; rr++) {
            /* an inout pin is not its own sink (E-4: driver == sink is skipped) */
            if (rr == dd || !odin3_pin_reads(gm->mod, pins.pins[rr])) {
                continue;
            }
            odin3_status st = add_edge(gm, from, odin3_pin_node(gm->mod, pins.pins[rr]).v);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status gl_mod_build(gl_mod *gm, odin3_module *mod, const gl_opts *opts) {
    memset(gm, 0, sizeof *gm);
    gm->mod = mod;
    odin3_vec_init(&gm->edges, sizeof(glet_edge));
    uint32_t nodes = odin3_module_node_end(mod);
    gm->eligible = odin3_util_calloc((size_t)nodes + 1);
    if (gm->eligible == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 1; id < nodes; id++) {
        gm->eligible[id] = node_eligible(mod, (odin3_node_id){id}) ? 1 : 0;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_net_end(mod); id++) {
        odin3_net_id net = {id};
        if (!odin3_net_live(mod, net) || odin3_net_driver_count(mod, net) == 0 ||
            odin3_net_const_value(mod, net) != ODIN3_CONST_NONE) {
            continue;
        }
        if (opts->exclude_global && net_is_global(mod, net, opts)) {
            gm->global_nets++;
            continue;
        }
        st = add_net(gm, net);
    }
    gm->built = true;
    return st;
}

void gl_mod_free(gl_mod *gm) {
    odin3_vec_free(&gm->edges);
    odin3_util_free(gm->eligible);
    memset(gm, 0, sizeof *gm);
}
