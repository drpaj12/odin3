/* builtin.c — the static table of built-in cell types, in registration (and ID) order. */
#include "ir/ir_internal.h"

#include <stdint.h>

const odin3_celltype_def *const odin3_builtin_celltypes[] = {
    &odin3_cell_port_in, &odin3_cell_port_out, &odin3_cell_port_inout, &odin3_cell_const0,
    &odin3_cell_const1,  &odin3_cell_constx,   &odin3_cell_constz,
};

const uint32_t odin3_builtin_celltype_count =
    (uint32_t)(sizeof odin3_builtin_celltypes / sizeof odin3_builtin_celltypes[0]);
