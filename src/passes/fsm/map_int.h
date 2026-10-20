/*
 * map_int.h — what map.c and map_pattern.c share (private to src/passes/fsm): the mapper of one
 * FSM. Every new object is recorded so that a failure before the first destructive step can undo
 * the build.
 */
#ifndef ODIN3_PASSES_FSM_MAP_INT_H
#define ODIN3_PASSES_FSM_MAP_INT_H

#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* The number of elements of an array. */
#define ODIN3_FSM_COUNT_OF(arr) ((uint32_t)(sizeof(arr) / sizeof((arr)[0])))

typedef struct odin3_fsm_mapper {
    const odin3_fsm_mod *fm;
    const odin3_fsm_inst *inst;
    odin3_opt_opts opts;
    odin3_opt_ctx oc;
    odin3_prov_id prov;
    odin3_vec nodes;         /* odin3_node_id created */
    odin3_vec nets;          /* odin3_net_id created */
    odin3_vec pending;       /* uint32_t pairs: output pin of a new cell, ctrl_out net to drive */
    odin3_net_id *state_sig; /* per state: the net that is true in that state */
    uint32_t n_states, n_cells;
} odin3_fsm_mapper;

/* A fresh unnamed net with the mapper's record, remembered for undo. */
odin3_status odin3_fsm_map_net(odin3_fsm_mapper *mp, odin3_net_id *out);

/* The net of a signal bit: itself, or the canonical constant net of a constant. */
odin3_status odin3_fsm_map_sig_net(odin3_fsm_mapper *mp, odin3_fsm_sig sig, odin3_net_id *out);

/* A cell of the named type: its parameters and one netvec per port; remembered for undo. */
typedef struct odin3_fsm_cell_req {
    const char *type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
} odin3_fsm_cell_req;

odin3_status odin3_fsm_map_cell(odin3_fsm_mapper *mp, const odin3_fsm_cell_req *req,
                                odin3_node_id *out);

/* The cases-vector builder of fsm_map.cc:45-155 (implement_pattern_cache) for one output. */
typedef enum odin3_fsm_target_kind {
    ODIN3_FSM_TARGET_NEXT_STATE, /* transitions into state `index` */
    ODIN3_FSM_TARGET_CTRL_OUT    /* transitions whose control output `index` is 1 */
} odin3_fsm_target_kind;

typedef struct odin3_fsm_target {
    odin3_fsm_target_kind kind;
    uint32_t index;
    odin3_net_id out_net;   /* the net to drive; none: leave the output pin open (pending) */
    odin3_fsm_sig out_ctrl; /* the control output signal the open pin will drive */
} odin3_fsm_target;

odin3_status odin3_fsm_map_target(odin3_fsm_mapper *mp, const odin3_fsm_target *target);

#endif
