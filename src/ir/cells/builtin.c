/* builtin.c — the static table of built-in cell types, in registration (and ID) order. */
#include "cells.h"

#include <stdint.h>

const odin3_celltype_def *const odin3_builtin_celltypes[] = {
    &odin3_cell_port_in, &odin3_cell_port_out,   &odin3_cell_port_inout, &odin3_cell_const0,
    &odin3_cell_const1,  &odin3_cell_constx,     &odin3_cell_constz,     &odin3_cell_add,
    &odin3_cell_sub,     &odin3_cell_mul,        &odin3_cell_div,        &odin3_cell_mod,
    &odin3_cell_and,     &odin3_cell_or,         &odin3_cell_xor,        &odin3_cell_not,
    &odin3_cell_shl,     &odin3_cell_shr,        &odin3_cell_sshr,       &odin3_cell_eq,
    &odin3_cell_ne,      &odin3_cell_lt,         &odin3_cell_le,         &odin3_cell_gt,
    &odin3_cell_ge,      &odin3_cell_reduce_and, &odin3_cell_reduce_or,  &odin3_cell_reduce_xor,
    &odin3_cell_mux,     &odin3_cell_pmux,       &odin3_cell_dff,        &odin3_cell_dffe,
    &odin3_cell_adff,    &odin3_cell_sdff,       &odin3_cell_mem,        &odin3_cell_memrd,
    &odin3_cell_memwr,   &odin3_cell_tribuf,     &odin3_cell_g_buf,      &odin3_cell_g_not,
    &odin3_cell_g_and,   &odin3_cell_g_or,       &odin3_cell_g_xor,      &odin3_cell_g_nand,
    &odin3_cell_g_nor,   &odin3_cell_g_xnor,     &odin3_cell_g_mux,      &odin3_cell_dff_p,
    &odin3_cell_dff_n,   &odin3_cell_dlatch_p,   &odin3_cell_dlatch_n,   &odin3_cell_ff,
    &odin3_cell_sop,
};

const uint32_t odin3_builtin_celltype_count =
    (uint32_t)(sizeof odin3_builtin_celltypes / sizeof odin3_builtin_celltypes[0]);
