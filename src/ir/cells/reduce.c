/* reduce.c — word-level reduction cells $reduce_and, $reduce_or, $reduce_xor. */
#include "cells.h"

/* Number of 1 bits of A (port 0). */
static uint32_t ones_of_a(const odin3_sim_cell *cell) {
    const odin3_sim_span *a_span = &cell->ports[0];
    uint32_t ones = 0;
    for (uint32_t k = 0; k < a_span->width; k++) {
        ones += cell->values[a_span->idx[k]];
    }
    return ones;
}

/* Simulate hooks (sim/cell.h): one result bit, zero-extended to Y_WIDTH; signedness is moot. */
static void reduce_and_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, ones_of_a(cell) == cell->ports[0].width);
}
static void reduce_or_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, ones_of_a(cell) != 0);
}
static void reduce_xor_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, (ones_of_a(cell) & 1U) != 0);
}

ODIN3_UNARY_SIM(odin3_cell_reduce_and, "$reduce_and", reduce_and_sim, NULL);
ODIN3_UNARY_SIM(odin3_cell_reduce_or, "$reduce_or", reduce_or_sim, NULL);
ODIN3_UNARY_SIM(odin3_cell_reduce_xor, "$reduce_xor", reduce_xor_sim, NULL);
