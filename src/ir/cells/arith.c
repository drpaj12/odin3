/*
 * arith.c — word-level arithmetic cells $add, $sub, $mul, $div, $mod, and (2D, EL-31) $neg and
 * $pow.
 */
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
/* simlib `-A` / `-$signed(A)`: A extended to Y_WIDTH by A_SIGNED, negated modulo 2^Y_WIDTH. */
static void neg_sim(const odin3_sim_cell *cell) {
    odin3_cells_word_sim(cell, odin3_word_neg);
}

ODIN3_BINARY_SIM(odin3_cell_add, "$add", add_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_sub, "$sub", sub_sim, odin3_cells_y_scratch);
ODIN3_BINARY_SIM(odin3_cell_mul, "$mul", mul_sim, odin3_cells_y_scratch);
ODIN3_BINARY(odin3_cell_div, "$div");
ODIN3_BINARY(odin3_cell_mod, "$mod");
ODIN3_UNARY_SIM(odin3_cell_neg, "$neg", neg_sim, odin3_cells_y_scratch);
/*
 * Y = A ** B (simlib: `$signed(A) ** $signed(B)` when both are signed, else each operand signed
 * by its own flag, so a negative signed exponent follows IEEE 1364-2005 5.1.5). No simulate hook
 * until a pass needs one (2D spec §7.2).
 */
ODIN3_BINARY(odin3_cell_pow, "$pow");
