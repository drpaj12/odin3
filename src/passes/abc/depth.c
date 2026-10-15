/*
 * depth.c — the LUT depth of the top module (3D spec §8.1, AB-25; plan Task 18): the number of
 * $lut levels on the longest path between the top module's CIs and COs (ports, flip-flops and cut
 * cells bound a path), by Kahn's order with an explicit worklist.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "passes/abc/prov.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct dp {
    const odin3_module *mod;
    uint32_t lut_type;
    odin3_vec indeg; /* uint32_t per node: $lut drivers among its inputs (0 for a non-$lut) */
    odin3_vec level; /* uint32_t per node: its depth once ordered */
    odin3_vec ready; /* uint32_t: the worklist */
} dp;

static uint32_t *at(odin3_vec *vec, uint32_t idx) {
    return (uint32_t *)vec->data + idx;
}

static bool is_lut(const dp *d, odin3_node_id node) {
    return odin3_node_live(d->mod, node) && odin3_node_type(d->mod, node).v == d->lut_type;
}

/* The $lut node driving net, or 0. */
static uint32_t lut_driver(const dp *d, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(d->mod, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(d->mod, odin3_net_driver(d->mod, net));
    return is_lut(d, node) ? node.v : 0;
}

static odin3_status vec_zeros(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(vec, count == 0 ? 1 : count);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, sizeof(uint32_t) * (count == 0 ? 1 : count));
        vec->len = count;
    }
    return st;
}

static odin3_status push(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* In-degrees, and the first worklist: the $luts none of whose inputs a $lut drives. */
static odin3_status dp_start(dp *d) {
    uint32_t end = odin3_module_node_end(d->mod);
    odin3_status st = vec_zeros(&d->indeg, end);
    st = st == ODIN3_OK ? vec_zeros(&d->level, end) : st;
    odin3_vec_init(&d->ready, sizeof(uint32_t));
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!is_lut(d, node)) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(d->mod, node);
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            *at(&d->indeg, id) += odin3_pin_reads(d->mod, pin) &&
                                          lut_driver(d, odin3_pin_net(d->mod, pin)) != 0
                                      ? 1U
                                      : 0U;
        }
        st = *at(&d->indeg, id) == 0 ? push(&d->ready, id) : st;
    }
    return st;
}

/* Finishes node: its level is final; every $lut reading its output is one input closer. */
static odin3_status dp_finish(dp *d, uint32_t node) {
    uint32_t level = *at(&d->level, node) + 1U;
    *at(&d->level, node) = level;
    odin3_pinslice pins = odin3_node_pins(d->mod, (odin3_node_id){node});
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (!odin3_pin_drives(d->mod, pin) || !odin3_net_valid(odin3_pin_net(d->mod, pin))) {
            continue;
        }
        odin3_pinlist sinks = odin3_net_sinks(d->mod, odin3_pin_net(d->mod, pin));
        for (uint32_t s = 0; s < sinks.count; s++) {
            uint32_t reader = odin3_pin_node(d->mod, sinks.pins[s]).v;
            if (!is_lut(d, (odin3_node_id){reader}) || *at(&d->indeg, reader) == 0) {
                continue;
            }
            *at(&d->level, reader) = *at(&d->level, reader) > level ? *at(&d->level, reader) : level;
            *at(&d->indeg, reader) -= 1U;
            if (*at(&d->indeg, reader) == 0) {
                odin3_status st = push(&d->ready, reader);
                if (st != ODIN3_OK) {
                    return st;
                }
            }
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_abc_depth(const odin3_design *design, uint32_t *depth) {
    *depth = 0;
    odin3_module_id top = odin3_design_top(design);
    if (top.v == 0) {
        return ODIN3_OK;
    }
    dp d = {0};
    d.mod = odin3_module_get((odin3_design *)design, top);
    d.lut_type = odin3_lut_builtin_index();
    odin3_status st = dp_start(&d);
    while (st == ODIN3_OK && d.ready.len > 0) {
        uint32_t node = *at(&d.ready, (uint32_t)d.ready.len - 1U);
        odin3_vec_pop(&d.ready);
        st = dp_finish(&d, node);
        *depth = *at(&d.level, node) > *depth ? *at(&d.level, node) : *depth;
    }
    odin3_vec_free(&d.indeg);
    odin3_vec_free(&d.level);
    odin3_vec_free(&d.ready);
    return st;
}
