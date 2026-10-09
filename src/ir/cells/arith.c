/* arith.c — word-level arithmetic cells $add, $sub, $mul, $div, $mod. */
#include "cells.h"

/* Simulate hooks (cells.h): results modulo 2^Y_WIDTH; $div and $mod have none yet. */
static void add_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_add);
}
static void sub_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_sub);
}
static void mul_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_mul);
}

ODIN3_BINARY_SIM(odin3_cell_add, "$add", add_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_sub, "$sub", sub_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_mul, "$mul", mul_sim, odin3_cells_y_scratch);
ODIN3_BINARY(odin3_cell_div, "$div");
ODIN3_BINARY(odin3_cell_mod, "$mod");
