/* builtin.c — the static table of built-in cell types, in registration (and ID) order. */
#include "cells.h"

#include <stdint.h>

/*
 * Append only: a type's index is its built-in ID. 2D IR change A (EL-31) appended the nine word
 * cells from $neg to $pow after $sop (index 52), so IDs 0..52 did not move, and IR change B
 * (EL-33) then appended the process cell $proc (index 62; 2D plan point (f)). 2E's IR change D
 * (2E spec §13) appended the eleven word flip-flops and latches $adffe … $sr (indices 63..73),
 * and 3D (3D spec §2, AB-2) then appended $lut, whose index only odin3_lut_builtin_index names
 * (ruling I5).
 */
const odin3_celltype_def *const odin3_builtin_celltypes[] = {
    &odin3_cell_port_in,    &odin3_cell_port_out,    &odin3_cell_port_inout,
    &odin3_cell_const0,     &odin3_cell_const1,      &odin3_cell_constx,
    &odin3_cell_constz,     &odin3_cell_add,         &odin3_cell_sub,
    &odin3_cell_mul,        &odin3_cell_div,         &odin3_cell_mod,
    &odin3_cell_and,        &odin3_cell_or,          &odin3_cell_xor,
    &odin3_cell_not,        &odin3_cell_shl,         &odin3_cell_shr,
    &odin3_cell_sshr,       &odin3_cell_eq,          &odin3_cell_ne,
    &odin3_cell_lt,         &odin3_cell_le,          &odin3_cell_gt,
    &odin3_cell_ge,         &odin3_cell_reduce_and,  &odin3_cell_reduce_or,
    &odin3_cell_reduce_xor, &odin3_cell_mux,         &odin3_cell_pmux,
    &odin3_cell_dff,        &odin3_cell_dffe,        &odin3_cell_adff,
    &odin3_cell_sdff,       &odin3_cell_mem,         &odin3_cell_memrd,
    &odin3_cell_memwr,      &odin3_cell_tribuf,      &odin3_cell_g_buf,
    &odin3_cell_g_not,      &odin3_cell_g_and,       &odin3_cell_g_or,
    &odin3_cell_g_xor,      &odin3_cell_g_nand,      &odin3_cell_g_nor,
    &odin3_cell_g_xnor,     &odin3_cell_g_mux,       &odin3_cell_dff_p,
    &odin3_cell_dff_n,      &odin3_cell_dlatch_p,    &odin3_cell_dlatch_n,
    &odin3_cell_ff,         &odin3_cell_sop,         &odin3_cell_neg,
    &odin3_cell_xnor,       &odin3_cell_reduce_xnor, &odin3_cell_reduce_bool,
    &odin3_cell_logic_not,  &odin3_cell_logic_and,   &odin3_cell_logic_or,
    &odin3_cell_shiftx,     &odin3_cell_pow,         &odin3_cell_proc,
    &odin3_cell_adffe,      &odin3_cell_sdffe,       &odin3_cell_sdffce,
    &odin3_cell_aldff,      &odin3_cell_aldffe,      &odin3_cell_dffsr,
    &odin3_cell_dffsre,     &odin3_cell_dlatch,      &odin3_cell_adlatch,
    &odin3_cell_dlatchsr,   &odin3_cell_sr,          &odin3_cell_lut,
};

const uint32_t odin3_builtin_celltype_count =
    (uint32_t)(sizeof odin3_builtin_celltypes / sizeof odin3_builtin_celltypes[0]);
