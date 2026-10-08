/* reduce.c — word-level reduction cells $reduce_and, $reduce_or, $reduce_xor. */
#include "cells.h"

#define ODIN3_UNARY(var, label)                                                                    \
    const odin3_celltype_def var = {label, ODIN3_GRAN_WORD,          0, odin3_cells_unary_ports,   \
                                    2,     odin3_cells_unary_params, 3, odin3_cells_unary_verify,  \
                                    NULL}

ODIN3_UNARY(odin3_cell_reduce_and, "$reduce_and");
ODIN3_UNARY(odin3_cell_reduce_or, "$reduce_or");
ODIN3_UNARY(odin3_cell_reduce_xor, "$reduce_xor");
