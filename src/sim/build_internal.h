/*
 * build_internal.h — the builder state shared by build.c, build_hier.c and build_names.c.
 */
#ifndef ODIN3_SIM_BUILD_INTERNAL_H
#define ODIN3_SIM_BUILD_INTERNAL_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "sim/sim_internal.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stddef.h>
#include <stdint.h>

/* A net-map entry not yet given a slot, and "no frame" (the top's parent). */
#define ODIN3_SIM_UNSET UINT32_MAX

/* Appends val to vec (of uint32_t); ODIN3_ERR_NO_MEMORY on out of memory. */
static inline odin3_status odin3_sim_push_u32(odin3_vec *vec, uint32_t val) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = val;
    return ODIN3_OK;
}

/*
 * One expanded module instance: its module, its parent frame and instance node there (the top
 * frame has parent ODIN3_SIM_UNSET), and the offset of its net map in the builder's netmap (one
 * provisional slot per net ID of the module).
 */
typedef struct odin3_sim_frame {
    odin3_module_id module;
    uint32_t parent;
    odin3_node_id inst;
    size_t map;
} odin3_sim_frame;

/*
 * Build state. Slots are provisional while instances are expanded: an instance's port can join
 * two slots (a child that wires an input straight to an output), so slots form a union-find (uf:
 * parent per slot, the parent always the smaller index); odin3_sim_build renumbers them densely at
 * the end, after which uf[s] is the final slot of provisional slot s.
 */
typedef struct odin3_sim_builder {
    odin3_design *design;
    odin3_u64map *types; /* module cell type ID -> module ID */
    odin3_vec frames;    /* odin3_sim_frame, in expansion order (frame 0 is the top) */
    odin3_vec netmap;    /* uint32_t provisional slots, per frame */
    odin3_vec uf;        /* uint32_t */
    odin3_vec cells;     /* odin3_sim_flat */
    odin3_vec spans;     /* odin3_sim_span, idx set when the build is finalized */
    odin3_vec idx;       /* uint32_t slots of every pin bit of every cell */
    odin3_vec in_ports;  /* odin3_sim_port */
    odin3_vec out_ports; /* odin3_sim_port */
    odin3_vec in_bits;   /* uint32_t */
    odin3_vec out_bits;  /* uint32_t */
    uint32_t n_state;
    uint32_t n_scratch;
} odin3_sim_builder;

/*
 * Rejects a recursive module hierarchy below top (a module that instantiates itself through any
 * chain of instances): ODIN3_ERR_INVALID_ARG, logged, naming a module on the cycle. Fills
 * bld->types first. ODIN3_ERR_NO_MEMORY on out of memory. Linear in the reachable modules' nodes.
 */
odin3_status odin3_sim_check_hierarchy(odin3_sim_builder *bld, odin3_module_id top);

/* The module ID instantiated by node of module, 0 when node is not a live module instance. */
uint32_t odin3_sim_child_module(const odin3_sim_builder *bld, const odin3_module *module,
                                odin3_node_id node);

/* Messages (build_names.c). Each logs one located error. */

/* "cannot simulate `<type>`" for node of frame. */
void odin3_sim_err_unsupported(const odin3_sim_builder *bld, uint32_t frame, odin3_node_id node);

/* "cannot simulate `<type>`" for node of frame, whose sim_scratch_bytes hook refused its size. */
void odin3_sim_err_too_large(const odin3_sim_builder *bld, uint32_t frame, odin3_node_id node);

/*
 * The clock pin of flat cell, on final slot `slot`, is not a primary input (slots renumbered); for
 * the ZERO slot, the clock pin is not connected.
 */
void odin3_sim_err_clock(const odin3_sim_builder *bld, const odin3_sim_flat *cell, uint32_t slot);

/* A combinational loop through final slot `slot` (slots renumbered). */
void odin3_sim_err_loop(const odin3_sim_builder *bld, uint32_t slot);

/* More than one ordinary (non-inout, non-tristate) driver on final slot `slot`. */
void odin3_sim_err_drivers(const odin3_sim_builder *bld, uint32_t slot);

/* An inout port of the top module. */
void odin3_sim_err_inout(const odin3_sim_builder *bld, uint32_t port_name);

#endif
