/*
 * sim_internal.h — the simulator's flat program, shared by build, levelize and the cycle engine.
 */
#ifndef ODIN3_SIM_SIM_INTERNAL_H
#define ODIN3_SIM_SIM_INTERNAL_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "sim/cell.h"
#include "sim/sim.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Reserved value slots: an unconnected input pin reads ZERO (never written), an unconnected
 * output pin writes DISCARD (never read). Net bits start at FIRST.
 */
enum { ODIN3_SIM_SLOT_ZERO = 0, ODIN3_SIM_SLOT_DISCARD = 1, ODIN3_SIM_SLOT_FIRST = 2 };

/*
 * One flat cell. view is the hook's argument, precomputed at build (values, ports, params, state,
 * type data, scratch); the engine sets view.event before each call. The IR node it came from
 * (module, node) and the instance expansion it belongs to (frame, 0 for the top module) give names
 * for messages. span_first / idx_first / state_first are the cell's offsets into the simulator's
 * span, index and state arrays.
 */
typedef struct odin3_sim_flat {
    odin3_sim_cell view;
    const odin3_celltype_def *def;
    odin3_module_id module;
    odin3_node_id node;
    uint32_t frame;
    uint32_t span_first;
    uint32_t idx_first;
    uint32_t state_first;
} odin3_sim_flat;

/* A port of the top module: its name (strtab ID) and its bits in in_bits / out_bits. */
typedef struct odin3_sim_port {
    uint32_t name;
    uint32_t first;
    uint32_t width;
} odin3_sim_port;

/*
 * The program. values: n_values bytes, one per slot (0 or 1). cells: n_cells flat cells in
 * instance-expansion order. order: the n_cells cell indices in settle order (a topological order
 * of driver -> reader edges in which edge-triggered cells have no inputs, so they come first).
 * spans / idx / state: the storage the views point into. Primary inputs (in_ports, n_in_ports)
 * and outputs, in port order; in_bits / out_bits hold the slot of each port bit, in port then bit
 * order; in_clock[k] is 1 when input bit k drives the clock pin of an edge-triggered cell (the
 * engine toggles it instead of driving it at random).
 */
struct odin3_sim {
    odin3_design *design;
    uint8_t *values;
    uint32_t n_values;
    odin3_sim_flat *cells;
    uint32_t n_cells;
    uint32_t *order;
    odin3_sim_span *spans;
    uint32_t *idx;
    uint8_t *state;
    uint32_t n_state;
    uint8_t *scratch;
    uint32_t n_scratch;
    odin3_sim_port *in_ports;
    uint32_t n_in_ports;
    odin3_sim_port *out_ports;
    uint32_t n_out_ports;
    uint32_t *in_bits;
    uint8_t *in_clock;
    uint32_t n_in_bits;
    uint32_t *out_bits;
    uint32_t n_out_bits;
};

/* True when the cell type is edge-triggered storage (cuts the combinational graph). */
static inline bool odin3_sim_is_edge(const odin3_celltype_def *def) {
    return (def->flags & ODIN3_CT_SEQ_EDGE) != 0;
}

/* True when the type keeps state (edge-triggered or level-sensitive). */
static inline bool odin3_sim_is_seq(const odin3_celltype_def *def) {
    return (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0;
}

/*
 * Levelizes sim's cells (Kahn's algorithm over driver -> reader edges; edge-triggered cells read
 * nothing): fills sim->order. On a combinational loop returns ODIN3_ERR_INVALID_ARG, unlogged,
 * with *loop_slot a value slot on the loop; ODIN3_ERR_NO_MEMORY on out of memory. Linear in cells,
 * pin bits and slots. Needs the views' port spans final.
 */
odin3_status odin3_sim_levelize(odin3_sim *sim, uint32_t *loop_slot);

#endif
