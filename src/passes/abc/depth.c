/*
 * depth.c — the LUT depth of the top module (3D spec §8.1, AB-25; plan Task 18): the number of
 * $lut levels on the longest path between the top module's CIs and COs (ports, flip-flops and cut
 * cells bound a path), by Kahn's order with an explicit worklist. After abc --gates the levels are
 * the library gate cells (gatemap review: a library cell that is gate-like by the gate_truth rule
 * counts as a $lut does), so the depth agrees with the pass report.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/abc/prov.h"
#include "sim/gate_truth.h"

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

/* A level: a $lut, or a library cell that is gate-like by the gate_truth rule. */
static bool is_lut(const dp *dpt, odin3_node_id node) {
    if (!odin3_node_live(dpt->mod, node)) {
        return false;
    }
    odin3_celltype_id type = odin3_node_type(dpt->mod, node);
    if (type.v == dpt->lut_type) {
        return true;
    }
    const odin3_design *design = odin3_module_design(dpt->mod);
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    return def != NULL && odin3_celltype_lib(design, type) != NULL &&
           odin3_gate_truth_eligible(def);
}

/* The $lut node driving net, or 0. */
static uint32_t lut_driver(const dp *dpt, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(dpt->mod, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(dpt->mod, odin3_net_driver(dpt->mod, net));
    return is_lut(dpt, node) ? node.v : 0;
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
static odin3_status dp_start(dp *dpt) {
    uint32_t end = odin3_module_node_end(dpt->mod);
    odin3_status st = vec_zeros(&dpt->indeg, end);
    st = st == ODIN3_OK ? vec_zeros(&dpt->level, end) : st;
    odin3_vec_init(&dpt->ready, sizeof(uint32_t));
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!is_lut(dpt, node)) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(dpt->mod, node);
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            *at(&dpt->indeg, id) +=
                odin3_pin_reads(dpt->mod, pin) && lut_driver(dpt, odin3_pin_net(dpt->mod, pin)) != 0
                    ? 1U
                    : 0U;
        }
        st = *at(&dpt->indeg, id) == 0 ? push(&dpt->ready, id) : st;
    }
    return st;
}

/* The net on the output pin of node (a $lut has one); none when it drives nothing. */
static odin3_net_id out_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

/* One reader of a finished node's output: one input closer; ready when it has none left. */
static odin3_status dp_release(dp *dpt, odin3_node_id node, uint32_t level) {
    uint32_t reader = node.v;
    if (!is_lut(dpt, node) || *at(&dpt->indeg, reader) == 0) {
        return ODIN3_OK;
    }
    uint32_t *held = at(&dpt->level, reader);
    *held = *held > level ? *held : level;
    *at(&dpt->indeg, reader) -= 1U;
    return *at(&dpt->indeg, reader) == 0 ? push(&dpt->ready, reader) : ODIN3_OK;
}

/* Finishes node: its level is final; every $lut reading its output is one input closer. */
static odin3_status dp_finish(dp *dpt, uint32_t node) {
    uint32_t level = *at(&dpt->level, node) + 1U;
    *at(&dpt->level, node) = level;
    odin3_net_id out = out_net(dpt->mod, (odin3_node_id){node});
    if (!odin3_net_valid(out)) {
        return ODIN3_OK;
    }
    odin3_pinlist sinks = odin3_net_sinks(dpt->mod, out);
    for (uint32_t snk = 0; snk < sinks.count; snk++) {
        odin3_status st = dp_release(dpt, odin3_pin_node(dpt->mod, sinks.pins[snk]), level);
        if (st != ODIN3_OK) {
            return st;
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
    dp dpt = {0};
    dpt.mod = odin3_module_get((odin3_design *)design, top);
    dpt.lut_type = odin3_abc_lut_type(design).v;
    odin3_status st = dp_start(&dpt);
    while (st == ODIN3_OK && dpt.ready.len > 0) {
        uint32_t node = *at(&dpt.ready, (uint32_t)dpt.ready.len - 1U);
        odin3_vec_pop(&dpt.ready);
        st = dp_finish(&dpt, node);
        *depth = *at(&dpt.level, node) > *depth ? *at(&dpt.level, node) : *depth;
    }
    odin3_vec_free(&dpt.indeg);
    odin3_vec_free(&dpt.level);
    odin3_vec_free(&dpt.ready);
    return st;
}
