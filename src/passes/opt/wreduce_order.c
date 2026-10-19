/*
 * wreduce_order.c — the work order of wreduce (3G spec WR-7, amended 2026-10-19; PHASE3 #51).
 *
 * Yosys visits cells in the order its front end created them (the interning order of the cell
 * names), and the front end builds an expression bottom-up: an operand's cell exists before the
 * cell that consumes it. Odin III's elaborator numbers a consumer first, so node-ID order is not
 * that order. The rank of a node is its position in a depth-first post-order of the net graph:
 * nodes are entered in ID order, and the driver of each input pin is placed before the node.
 * What the walk models, and what it does not, is the amended WR-7 list:
 *  - Pin order: ports in the order of wr_order_port (registry order, except `$mux`: S, B, A, the
 *    order genrtlil.cc:1904 creates the ternary's operands: condition, true value, false value),
 *    bits LSB first, as the amended spec says. The first driver of a net is the one followed.
 *  - Only an anonymous net is followed (no wire on it): an expression temporary. A net with a wire
 *    is a statement boundary, where Yosys's order is the order of the statements, which is
 *    node-ID order (a forward reference stays consumer first).
 *  - The outputs of sequential nodes (wr_order_is_source) are not followed: Yosys's front end
 *    creates a register after the logic of its D input, and the cut keeps every loop out of the
 *    walk. A combinational loop is cut where the walk meets a node already entered.
 * Known approximations (WR-7): cells that `proc` makes come after all front-end cells in Yosys but
 * are ranked here like any other node; a shared operand is placed by the first consumer entered;
 * Yosys interns names over the whole design, which only matters to a file read by Yosys.
 * The walk uses an explicit stack (the IR can be 2M nodes deep) and no hash order, so two runs
 * give the same ranks. tools/opt-check/il_celltypes.txt is the table il_order.py uses; the unit
 * test test_wreduce_order checks it against this file and the registry.
 */
#include "passes/opt/wreduce.h"

#include "util/alloc.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { FRAME_SHIFT = 32 };

/* The types whose outputs the walk does not follow (storage), by registry name. */
static const char *const k_sources[] = {
    "$dff",   "$dffe",   "$adff",    "$adffe",   "$sdff",       "$sdffe",      "$sdffce",
    "$aldff", "$aldffe", "$dffsr",   "$dffsre",  "$dlatch",     "$adlatch",    "$dlatchsr",
    "$sr",    "$mem",    "$_DFF_P_", "$_DFF_N_", "$_DLATCH_P_", "$_DLATCH_N_", "$_FF_"};

bool wr_order_is_source(const char *type) {
    for (size_t i = 0; i < sizeof k_sources / sizeof k_sources[0]; i++) {
        if (strcmp(type, k_sources[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* `$mux` ports are A, B, S, Y: the walk takes S, B, A (the ternary's creation order). */
static const uint32_t k_mux_walk[] = {2, 1, 0};

uint32_t wr_order_port(const char *type, uint32_t n_ports, uint32_t k) {
    if (strcmp(type, "$mux") == 0 && n_ports == 4U && k < 3U) {
        return k_mux_walk[k];
    }
    return k;
}

/* The pin at flat index *cursor of node's ports in walk order; false past the last pin. */
static bool pin_at(const wr_state *ws, odin3_node_id node, const odin3_celltype_def *def,
                   uint32_t cursor, odin3_pin_id *pin) {
    uint32_t left = cursor;
    for (uint32_t k = 0; k < def->n_ports; k++) {
        const odin3_pinslice slice =
            odin3_node_port(ws->m, node, wr_order_port(def->name, def->n_ports, k));
        if (left < slice.count) {
            *pin = (odin3_pin_id){slice.first.v + left};
            return true;
        }
        left -= slice.count;
    }
    return false;
}

/* The node that drives the anonymous net of `pin`, a reading pin, and is to be placed first; 0 if
 * none. */
static uint32_t pin_dep(const wr_state *ws, odin3_node_id node, odin3_pin_id pin,
                        const uint8_t *seen) {
    const odin3_net_id net = odin3_pin_net(ws->m, pin);
    if (!odin3_pin_reads(ws->m, pin) || !odin3_net_valid(net) ||
        odin3_wire_valid(odin3_net_primary(ws->m, net).wire)) {
        return 0;
    }
    const odin3_pin_id drv = odin3_net_driver(ws->m, net);
    if (!odin3_pin_valid(drv)) {
        return 0;
    }
    const odin3_node_id dn = odin3_pin_node(ws->m, drv);
    const odin3_celltype_def *def = odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, dn));
    const bool follow =
        dn.v != node.v && !seen[dn.v] && !(def != NULL && wr_order_is_source(def->name));
    return follow ? dn.v : 0U;
}

/* The next unvisited driver to place before node, from flat pin index *cursor on; 0 for none. */
static uint32_t next_dep(const wr_state *ws, odin3_node_id node, const uint8_t *seen,
                         uint32_t *cursor) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    odin3_pin_id pin;
    while (def != NULL && pin_at(ws, node, def, *cursor, &pin)) {
        (*cursor)++;
        const uint32_t dep = pin_dep(ws, node, pin, seen);
        if (dep != 0U) {
            return dep;
        }
    }
    return 0;
}

static odin3_status push_frame(odin3_vec *stack, uint8_t *seen, uint32_t node) {
    seen[node] = 1;
    return odin3_kit_push_u64(stack, (uint64_t)node << FRAME_SHIFT);
}

/* Walks from `root`, giving each node entered a rank in post-order. */
static odin3_status walk(wr_state *ws, odin3_vec *stack, uint8_t *seen, uint32_t root,
                         uint32_t *next_rank) {
    odin3_vec_clear(stack);
    odin3_status st = push_frame(stack, seen, root);
    while (st == ODIN3_OK && stack->len > 0) {
        uint64_t *top = (uint64_t *)odin3_vec_at(stack, stack->len - 1U);
        uint32_t cursor = (uint32_t)(*top & UINT32_MAX);
        const uint32_t node = (uint32_t)(*top >> FRAME_SHIFT);
        const uint32_t dep = next_dep(ws, (odin3_node_id){node}, seen, &cursor);
        *top = ((uint64_t)node << FRAME_SHIFT) | cursor;
        if (dep != 0U) {
            st = push_frame(stack, seen, dep);
            continue;
        }
        *(uint32_t *)odin3_vec_at(&ws->rank, node) = (*next_rank)++;
        odin3_vec_pop(stack);
    }
    return st;
}

odin3_status wr_order_ranks(wr_state *ws) {
    const uint32_t end = odin3_module_node_end(ws->m);
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(uint64_t));
    odin3_status st = ODIN3_OK;
    uint8_t *seen = NULL;
    assert(end <= 1U || ws->rank.len >= end);
    if (end > 1U && ws->rank.len < end) {
        return ODIN3_ERR_INVALID_ARG; /* prepare() sizes the table: a caller bug */
    }
    if (end > 1U) {
        seen = (uint8_t *)odin3_util_calloc(end);
        st = seen != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    uint32_t next_rank = 1;
    for (uint32_t id = 1; seen != NULL && st == ODIN3_OK && id < end; id++) {
        if (!seen[id] && odin3_node_live(ws->m, (odin3_node_id){id})) {
            st = walk(ws, &stack, seen, id, &next_rank);
        }
    }
    odin3_util_free(seen);
    odin3_vec_free(&stack);
    return st;
}
