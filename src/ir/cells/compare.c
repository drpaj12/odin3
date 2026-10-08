/* compare.c — word-level comparison cells $eq, $ne, $lt, $le, $gt, $ge. */
#include "cells.h"

#define ODIN3_BINARY(var, label)                                                                   \
    const odin3_celltype_def var = {                                                               \
        label, ODIN3_GRAN_WORD,           0, odin3_cells_binary_ports,                             \
        3,     odin3_cells_binary_params, 5, odin3_cells_binary_verify,                            \
        NULL}

ODIN3_BINARY(odin3_cell_eq, "$eq");
ODIN3_BINARY(odin3_cell_ne, "$ne");
ODIN3_BINARY(odin3_cell_lt, "$lt");
ODIN3_BINARY(odin3_cell_le, "$le");
ODIN3_BINARY(odin3_cell_gt, "$gt");
ODIN3_BINARY(odin3_cell_ge, "$ge");
