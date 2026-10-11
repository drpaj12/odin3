/*
 * reduce.c — word-level reduction cells $reduce_and, $reduce_or, $reduce_xor, and (2D, EL-31)
 * $reduce_xnor and $reduce_bool.
 */
#include "cells.h"

/*
 * Simulate hooks (sim/cell.h): one result bit, zero-extended to Y_WIDTH; signedness is moot
 * (simlib's `~^$signed(A)` and `!(!$signed(A))` reduce A's own bits, a self-determined operand).
 */
static void reduce_and_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_port_ones(cell, 0) == cell->ports[0].width);
}
static void reduce_or_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_port_ones(cell, 0) != 0);
}
static void reduce_xor_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, (odin3_cells_port_ones(cell, 0) & 1U) != 0);
}
/* ~^A: 1 for an even number of ones. */
static void reduce_xnor_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, (odin3_cells_port_ones(cell, 0) & 1U) == 0);
}

ODIN3_UNARY_SIM(odin3_cell_reduce_and, "$reduce_and", reduce_and_sim, NULL);
ODIN3_UNARY_SIM(odin3_cell_reduce_or, "$reduce_or", reduce_or_sim, NULL);
ODIN3_UNARY_SIM(odin3_cell_reduce_xor, "$reduce_xor", reduce_xor_sim, NULL);
ODIN3_UNARY_SIM(odin3_cell_reduce_xnor, "$reduce_xnor", reduce_xnor_sim, NULL);
/* simlib: Y = !(!A), which is |A (the same hook as $reduce_or). */
ODIN3_UNARY_SIM(odin3_cell_reduce_bool, "$reduce_bool", reduce_or_sim, NULL);
