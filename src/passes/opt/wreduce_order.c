/*
 * wreduce_order.c — the work order of wreduce (3G spec WR-7, amended 2026-10-19; PHASE3 #51).
 *
 * Yosys visits cells in the order its front end created them, and the front end builds an
 * expression bottom-up: an operand's cell exists before the cell that consumes it. Odin III's
 * elaborator numbers a consumer first, so node-ID order is not that order. The rank of a node is
 * therefore its position in a depth-first post-order of the net graph: nodes are entered in ID
 * order, and the driver of each input pin (pin order, so port A before port B) is placed before
 * the node. The outputs of sequential nodes (flip-flops, latches, memories) are not followed:
 * they cut every feedback loop, and Yosys's front end creates the flip-flop after the logic of
 * its D input. A combinational loop is cut where the walk meets it. The walk uses an explicit
 * stack (the IR can be 2M nodes deep) and no hash order, so two runs give the same ranks.
 */
#include "passes/opt/wreduce.h"

#include "util/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FRAME_SHIFT = 32 };

static bool is_source(const wr_state *ws, odin3_node_id node) {
    const wr_type type = wr_type_of(ws, node);
    return (type >= WT_DFF && type <= WT_ADLATCH) || type == WT_MEM;
}

/* The next unvisited combinational driver of node's input pins from *cursor on; 0 for none. */
static uint32_t next_dep(const wr_state *ws, odin3_node_id node, const uint8_t *seen,
                         uint32_t *cursor) {
    const odin3_pinslice pins = odin3_node_pins(ws->m, node);
    while (*cursor < pins.count) {
        const odin3_pin_id pin = {pins.first.v + (*cursor)++};
        const odin3_net_id net = odin3_pin_net(ws->m, pin);
        if (!odin3_pin_reads(ws->m, pin) || !odin3_net_valid(net)) {
            continue;
        }
        const odin3_pin_id drv = odin3_net_driver(ws->m, net);
        if (!odin3_pin_valid(drv)) {
            continue;
        }
        const odin3_node_id dn = odin3_pin_node(ws->m, drv);
        if (dn.v != node.v && !seen[dn.v] && !is_source(ws, dn)) {
            return dn.v;
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
    if (end > 1U && ws->rank.len >= end) {
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
