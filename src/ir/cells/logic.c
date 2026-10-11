/*
 * logic.c — word-level bitwise cells $and, $or, $xor, $not, and (2D, EL-31) $xnor and the logical
 * cells $logic_not, $logic_and, $logic_or.
 */
#include "cells.h"

enum { PORT_A, PORT_B };

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

/* ~(A ^ B) at Y_WIDTH: simlib's `A ~^ B`, both extended to Y_WIDTH (signed only if both are). */
static void xnor_sim(const odin3_sim_cell *cell) {
    uint32_t width = cell->ports[cell->n_ports - 1].width;
    odin3_word_bin op = odin3_cells_word_load(cell, width);
    odin3_word_xor(&op);
    const odin3_word_bin inv = {op.dst, op.dst, op.rhs, width};
    odin3_word_not(&inv);
    odin3_cells_word_store(cell, op.dst, width);
}

/*
 * The logical cells: each operand is true when some bit is 1 (sign extension never changes
 * that), the result is one bit zero-extended to Y_WIDTH (simlib `!A`, `A && B`, `A || B`).
 */
static bool truth(const odin3_sim_cell *cell, uint32_t port) {
    return odin3_cells_port_ones(cell, port) != 0;
}
static void logic_not_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, !truth(cell, PORT_A));
}
static void logic_and_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, truth(cell, PORT_A) && truth(cell, PORT_B));
}
static void logic_or_sim(const odin3_sim_cell *cell) {
    odin3_cells_flag_store(cell, truth(cell, PORT_A) || truth(cell, PORT_B));
}

ODIN3_BINARY_SIM(odin3_cell_and, "$and", and_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_or, "$or", or_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_xor, "$xor", xor_sim, odin3_cells_y_scratch);
ODIN3_UNARY_SIM(odin3_cell_not, "$not", not_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_xnor, "$xnor", xnor_sim, odin3_cells_y_scratch);
ODIN3_UNARY_SIM(odin3_cell_logic_not, "$logic_not", logic_not_sim, NULL);
ODIN3_BINARY_SIM(odin3_cell_logic_and, "$logic_and", logic_and_sim, NULL);
ODIN3_BINARY_SIM(odin3_cell_logic_or, "$logic_or", logic_or_sim, NULL);
