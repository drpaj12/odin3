/*
 * gl_net.c — the netlist adapter of the graphlets pass (spec section 2 and 3): the directed edges
 * of one module as driver node -> reader node, constants and (optionally) global nets left out.
 */
#include "ir/celltype.h"
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"

#include <ctype.h>
#include <string.h>

static bool node_eligible(const odin3_module *mod, odin3_node_id node) {
    if (!odin3_node_live(mod, node)) {
        return false;
    }
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
    return def != NULL && def->const_value == NULL;
}

static bool name_is_clock(const char *name) {
    char low[8] = {0};
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

static odin3_status add_net(gl_mod *gm, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(gm->mod, net);
    uint32_t drivers = odin3_net_driver_count(gm->mod, net);
    for (uint32_t d = 0; d < drivers; d++) {
        uint32_t from = odin3_pin_node(gm->mod, pins.pins[d]).v;
        for (uint32_t r = 0; r < pins.count; r++) {
            if (!odin3_pin_reads(gm->mod, pins.pins[r])) {
                continue;
            }
            uint32_t to = odin3_pin_node(gm->mod, pins.pins[r]).v;
            glet_edge *slot = gm->eligible[from] != 0 && gm->eligible[to] != 0
                                  ? odin3_vec_push(&gm->edges)
                                  : NULL;
            if (slot != NULL) {
                *slot = (glet_edge){from, to};
            } else if (gm->eligible[from] != 0 && gm->eligible[to] != 0) {
                return ODIN3_ERR_NO_MEMORY;
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
