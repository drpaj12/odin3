/* arith.c — word-level arithmetic cells $add, $sub, $mul, $div, $mod. */
#include "cells.h"

#define ODIN3_BINARY(var, label)                                                                   \
    const odin3_celltype_def var = {                                                               \
        label, ODIN3_GRAN_WORD,           0, odin3_cells_binary_ports,                             \
        3,     odin3_cells_binary_params, 5, odin3_cells_binary_verify,                            \
        NULL}

ODIN3_BINARY(odin3_cell_add, "$add");
ODIN3_BINARY(odin3_cell_sub, "$sub");
ODIN3_BINARY(odin3_cell_mul, "$mul");
ODIN3_BINARY(odin3_cell_div, "$div");
ODIN3_BINARY(odin3_cell_mod, "$mod");
