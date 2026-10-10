/* compare.c — word-level comparison cells $eq, $ne, $lt, $le, $gt, $ge. */
#include "cells.h"

/*
 * Simulate hooks (cells.h): as Yosys simlib, a signed comparison only when both A_SIGNED and
 * B_SIGNED are set, else both operands are zero-extended and compared unsigned.
 */
static void eq_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) == 0);
}
static void ne_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) != 0);
}
static void lt_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) < 0);
}
static void le_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) <= 0);
}
static void gt_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) > 0);
}
static void ge_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, odin3_cells_word_compare(cell) >= 0);
}

ODIN3_BINARY_SIM(odin3_cell_eq, "$eq", eq_sim, odin3_cells_cmp_scratch);
ODIN3_BINARY_SIM(odin3_cell_ne, "$ne", ne_sim, odin3_cells_cmp_scratch);
ODIN3_BINARY_SIM(odin3_cell_lt, "$lt", lt_sim, odin3_cells_cmp_scratch);
ODIN3_BINARY_SIM(odin3_cell_le, "$le", le_sim, odin3_cells_cmp_scratch);
ODIN3_BINARY_SIM(odin3_cell_gt, "$gt", gt_sim, odin3_cells_cmp_scratch);
ODIN3_BINARY_SIM(odin3_cell_ge, "$ge", ge_sim, odin3_cells_cmp_scratch);
