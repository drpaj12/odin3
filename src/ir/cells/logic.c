/* logic.c — word-level bitwise cells $and, $or, $xor, $not. */
#include "cells.h"

/* Simulate hooks (cells.h): operands extended to Y_WIDTH by their signedness, then bitwise. */
static void and_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_and);
}
static void or_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_or);
}
static void xor_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_xor);
}
static void not_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_not);
}

ODIN3_BINARY_SIM(odin3_cell_and, "$and", and_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_or, "$or", or_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_xor, "$xor", xor_sim, odin3_cells_y_scratch);
ODIN3_UNARY_SIM(odin3_cell_not, "$not", not_sim, odin3_cells_y_scratch);
