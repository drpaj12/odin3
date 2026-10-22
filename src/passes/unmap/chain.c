/*
 * chain.c — carry chain discovery (C-1) and the conversion of chains to `$add` / `$sub` (C-2 to
 * C-8).
 */
#include "passes/unmap/chain.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- discovery (C-1) ----------------------------------------------------------------------- */

typedef struct um_links {
    odin3_vec succ;    /* uint32_t per node ID: the next adder of the chain, 0 none */
    odin3_vec pred;    /* the previous one */
    odin3_vec visited; /* uint32_t per node ID */
} um_links;

static bool is_adder(const um_ctx *uc, odin3_node_id node) {
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.adder.v;
}

static void links_free(um_links *links) {
    odin3_vec_free(&links->succ);
    odin3_vec_free(&links->pred);
    odin3_vec_free(&links->visited);
}

static odin3_status vec_zero(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(vec, count);
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        void *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memset(slot, 0, sizeof(uint32_t));
    }
    return st;
}

static uint32_t *u32_at(odin3_vec *vec, uint32_t index) {
    return odin3_vec_at(vec, index);
}

/* The adder whose `cin` is the only reader of the `cout` of `node` and nothing else reads it;
 * 0 when there is none (no sink, a tap or a branch). */
static uint32_t next_adder(const um_ctx *uc, odin3_node_id node) {
    odin3_net_id net = um_port_net(uc->module, node, (um_pos){UM_ADDER_COUT, 0});
    if (!odin3_net_valid(net)) {
        return 0;
    }
    odin3_pinlist sinks = odin3_net_sinks(uc->module, net);
    uint32_t cins = 0;
    uint32_t others = 0;
    uint32_t next = 0;
    for (uint32_t i = 0; i < sinks.count; i++) {
        odin3_node_id reader = odin3_pin_node(uc->module, sinks.pins[i]);
        if (is_adder(uc, reader) && odin3_pin_port(uc->module, sinks.pins[i]) == UM_ADDER_CIN) {
            cins++;
            next = reader.v;
        } else {
            others++;
        }
    }
    return cins == 1 && others == 0 ? next : 0;
}

static odin3_status make_links(const um_ctx *uc, um_links *links) {
    size_t end = odin3_module_node_end(uc->module);
    odin3_status st = vec_zero(&links->succ, end);
    st = st == ODIN3_OK ? vec_zero(&links->pred, end) : st;
    st = st == ODIN3_OK ? vec_zero(&links->visited, end) : st;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (!is_adder(uc, (odin3_node_id){nid})) {
            continue;
        }
        uint32_t next = next_adder(uc, (odin3_node_id){nid});
        if (next != 0 && next != nid && *u32_at(&links->pred, next) == 0) {
            *u32_at(&links->succ, nid) = next;
            *u32_at(&links->pred, next) = nid;
        }
    }
    return st;
}

static odin3_status add_cell(um_walk *walk, uint32_t node) {
    odin3_node_id *slot = odin3_vec_push(&walk->cells);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->v = node;
    return ODIN3_OK;
}

/* Walks succ from `start` into a new walk (ending at a cell with no successor or back at start,
 * a cycle); the walk is pushed on `walks` whatever the status. */
static odin3_status walk_from(um_links *links, uint32_t start, odin3_vec *walks) {
    um_walk *walk = odin3_vec_push(walks);
    if (walk == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    walk->decline = NULL;
    odin3_vec_init(&walk->cells, sizeof(odin3_node_id));
    odin3_status st = ODIN3_OK;
    uint32_t node = start;
    while (st == ODIN3_OK && node != 0 && *u32_at(&links->visited, node) == 0) {
        *u32_at(&links->visited, node) = 1;
        st = add_cell(walk, node);
        node = *u32_at(&links->succ, node);
    }
    if (node != 0) {
        walk->decline = "cycle";
    }
    return st;
}

odin3_status um_chain_walks(const um_ctx *uc, odin3_vec *walks) {
    um_links links;
    odin3_vec_init(walks, sizeof(um_walk));
    odin3_status st = make_links(uc, &links);
    uint32_t end = odin3_module_node_end(uc->module);
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (is_adder(uc, (odin3_node_id){nid}) && *u32_at(&links.pred, nid) == 0) {
            st = walk_from(&links, nid, walks);
        }
    }
    /* cells never visited lie on a cycle */
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (is_adder(uc, (odin3_node_id){nid}) && *u32_at(&links.visited, nid) == 0) {
            st = walk_from(&links, nid, walks);
        }
    }
    links_free(&links);
    return st;
}

void um_walks_free(odin3_vec *walks) {
    um_walk *items = walks->data;
    for (size_t i = 0; i < walks->len; i++) {
        odin3_vec_free(&items[i].cells);
    }
    odin3_vec_free(walks);
}

odin3_status um_adder_phase(um_ctx *uc) {
    odin3_vec walks;
    odin3_status st = um_chain_walks(uc, &walks);
    um_walks_free(&walks);
    return st;
}
