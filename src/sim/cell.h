/*
 * cell.h — the simulator's view of one cell, passed to a cell type's simulate hook (IR-8, 1E).
 *
 * Values are 2-state (Phase 1): the simulator keeps one byte per net bit, always 0 or 1, in one
 * value array; a cell sees its pins as spans of indices into that array. X and Z do not exist in
 * the simulator: constant X/Z drivers read as 0, and latch INIT 2 (don't care) and 3 (unknown)
 * start at 0, as tools/equiv-check and the reference testbench do (1E spec, Semantics).
 *
 * Contract of a simulate hook (celltype.h): it reads input-pin values and writes output-pin
 * values (0 or 1 only) through the view, and a sequential type also reads and writes its state.
 * It never allocates, logs or fails. The simulator guarantees every pin bit has a valid index
 * (unconnected inputs read a constant-0 slot, unconnected outputs write a discard slot), every
 * value is 0 or 1, and the parameters are the node's (they passed the type's verify).
 *
 * Events. Combinational types (no ODIN3_CT_SEQ_* flag) only ever receive ODIN3_SIM_COMB and may
 * ignore the event. Sequential types receive:
 *   ODIN3_SIM_INIT     once, before the first cycle, with the state zeroed: set the initial state
 *                      from the parameters (write no outputs);
 *   ODIN3_SIM_COMB     during every settle, in level order: write the outputs (an edge-triggered
 *                      type from its state only; a level latch also from its inputs, updating its
 *                      state while transparent);
 *   ODIN3_SIM_POSEDGE  at the rising edge of the clocks, after a settle: a type triggered on it
 *                      copies its inputs into its state and writes no outputs (so the order in
 *                      which the simulator visits the cells does not matter);
 *   ODIN3_SIM_NEGEDGE  likewise at the falling edge.
 */
#ifndef ODIN3_SIM_CELL_H
#define ODIN3_SIM_CELL_H

#include "ir/value.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum odin3_sim_event {
    ODIN3_SIM_COMB,
    ODIN3_SIM_POSEDGE,
    ODIN3_SIM_NEGEDGE,
    ODIN3_SIM_INIT
} odin3_sim_event;

/* The bits of one port: idx[b] is the value index of bit b (LSB first); idx may be NULL when
 * width is 0. */
typedef struct odin3_sim_span {
    const uint32_t *idx;
    uint32_t width;
} odin3_sim_span;

/*
 * One cell as its simulate hook sees it. ports has one span per port definition of the type, in
 * definition order, each as wide as the port's width for these parameters. state is the cell's
 * storage: for a sequential type one byte (0 or 1) per bit of its output ports, in port then bit
 * order (n_state bytes); NULL and 0 for a combinational type.
 */
typedef struct odin3_sim_cell {
    uint8_t *values;             /* the simulator's value array */
    const odin3_sim_span *ports; /* n_ports spans */
    uint32_t n_ports;
    const odin3_value *params; /* one value per parameter definition, in order */
    uint8_t *state;
    uint32_t n_state;
    odin3_sim_event event;
} odin3_sim_cell;

/*
 * The initial 2-state value of a latch or flip-flop INIT parameter (IR-10, BLIF .latch): 1 for
 * INIT 1, 0 for INIT 0, 2 (don't care) and 3 (unknown), and 0 for anything else.
 */
static inline uint8_t odin3_sim_init_bit(const odin3_value *init) {
    return (uint8_t)(init->kind == ODIN3_VAL_INT && init->i == 1 ? 1 : 0);
}

#endif
