/* logic.c — word-level bitwise cells $and, $or, $xor, $not. */
#include "cells.h"

#define ODIN3_BINARY(var, label)                                                                   \
    const odin3_celltype_def var = {                                                               \
        label, ODIN3_GRAN_WORD,           0, odin3_cells_binary_ports,                             \
        3,     odin3_cells_binary_params, 5, odin3_cells_binary_verify,                            \
        NULL}
#define ODIN3_UNARY(var, label)                                                                    \
    const odin3_celltype_def var = {label, ODIN3_GRAN_WORD,          0, odin3_cells_unary_ports,   \
                                    2,     odin3_cells_unary_params, 3, odin3_cells_unary_verify,  \
                                    NULL}

ODIN3_BINARY(odin3_cell_and, "$and");
ODIN3_BINARY(odin3_cell_or, "$or");
ODIN3_BINARY(odin3_cell_xor, "$xor");
ODIN3_UNARY(odin3_cell_not, "$not");
