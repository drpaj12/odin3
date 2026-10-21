/*
 * levelize_int.h — the private state of the levelize pass (Phase 6 spec 6.4, V-1 to V-8), shared by
 * levelize.c (options, checks, attributes) and sweep.c (the Kahn sweep and the loop cut).
 */
#ifndef ODIN3_PASSES_LEVELIZE_LEVELIZE_INT_H
#define ODIN3_PASSES_LEVELIZE_LEVELIZE_INT_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stddef.h>
#include <stdint.h>

/* The role of a node in the sweep. */
enum {
    LV_SKIP = 0,  /* dead node or module port node: not a cell */
    LV_COMB = 1,  /* combinational: level 1 + max of its input nets */
    LV_SOURCE = 2 /* starts a path: sequential, memory or a black box that states a register */
};

/* Per-node state bits. */
enum { LV_DONE = 1, LV_CUT = 2, LV_SELF = 4, LV_CAND = 16 };

/* One module under the sweep. All arrays are indexed by the module-local node or net ID. */
typedef struct lv_mod {
    const odin3_design *design;
    odin3_module *module;
    uint32_t n_nodes; /* odin3_module_node_end */
    uint32_t n_nets;  /* odin3_module_net_end */
    uint8_t *role;    /* LV_SKIP, LV_COMB or LV_SOURCE, per node */
    uint8_t *state;   /* LV_DONE | LV_CUT | LV_SELF, per node */
    uint32_t *indeg;  /* unresolved (input pin, comb driver pin) pairs, per node */
    uint32_t *clevel; /* level of the cell, per node */
    uint32_t *nlevel; /* level of the net, per net */
    uint32_t *ndrv;   /* comb driver pins not yet done, per net */
    uint32_t *stack;  /* the ready list of the Kahn sweep */
    size_t n_stack;
    uint32_t n_comb;     /* comb nodes */
    uint32_t n_done;     /* comb nodes finished */
    uint32_t n_cells;    /* nodes that are cells (comb + source) */
    uint32_t n_inst;     /* instances of design modules among them */
    uint32_t n_cut;      /* loops cut */
    uint32_t max_level;  /* over cells and nets */
    odin3_vec frames;    /* Tarjan DFS frames (sweep.c) */
    odin3_vec members;   /* Tarjan component stack */
    uint32_t *tj_index;  /* Tarjan index, per node */
    uint32_t tj_counter; /* Tarjan: indices handed out */
    uint32_t tj_cands;   /* Tarjan: cyclic components found (their lowest cell has LV_CAND) */
    uint32_t *tj_low;    /* Tarjan lowlink, per node */
} lv_mod;

/* sweep.c: classifies the nodes, then runs the sweep, cutting loops. NO_MEMORY on failure. */
odin3_status lv_sweep(lv_mod *lv);

/* Releases every array of lv (safe on a partly built one). */
void lv_free(lv_mod *lv);

/* Allocates the arrays of lv for module and zeroes the counters. NO_MEMORY on failure (lv then
 * still safe to lv_free). */
odin3_status lv_open(lv_mod *lv, const odin3_design *design, odin3_module *module);

#endif
