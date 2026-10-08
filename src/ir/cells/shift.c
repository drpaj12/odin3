/* shift.c — word-level shift cells $shl, $shr, $sshr (B is the shift amount). */
#include "cells.h"

#define ODIN3_BINARY(var, label)                                                                   \
    const odin3_celltype_def var = {                                                               \
        label, ODIN3_GRAN_WORD,           0, odin3_cells_binary_ports,                             \
        3,     odin3_cells_binary_params, 5, odin3_cells_binary_verify,                            \
        NULL}

ODIN3_BINARY(odin3_cell_shl, "$shl");
ODIN3_BINARY(odin3_cell_shr, "$shr");
ODIN3_BINARY(odin3_cell_sshr, "$sshr");
