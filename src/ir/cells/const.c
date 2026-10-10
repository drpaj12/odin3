/* const.c — constant driver cells $_CONST0_, $_CONST1_, $_CONSTX_, $_CONSTZ_ (IR-4). */
#include "cells.h"

static odin3_const const0_value(const odin3_value *params) {
    (void)params;
    return ODIN3_CONST_0;
}
static odin3_const const1_value(const odin3_value *params) {
    (void)params;
    return ODIN3_CONST_1;
}
static odin3_const constx_value(const odin3_value *params) {
    (void)params;
    return ODIN3_CONST_X;
}
static odin3_const constz_value(const odin3_value *params) {
    (void)params;
    return ODIN3_CONST_Z;
}

/* Simulate hooks (sim/cell.h). The simulator is 2-state: X and Z drive 0. */
static void const0_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, false);
}
static void const1_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, true);
}

/* One scalar output Y, no parameters; legal in every view (IR-9). */
static const odin3_port_def k_const_ports[] = {{"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};

const odin3_celltype_def odin3_cell_const0 = {
    "$_CONST0_", ODIN3_GRAN_BIT, ODIN3_CT_ANYVIEW, k_const_ports, 1,   NULL,
    0,           NULL,           const0_value,     const0_sim,    NULL};
const odin3_celltype_def odin3_cell_const1 = {
    "$_CONST1_", ODIN3_GRAN_BIT, ODIN3_CT_ANYVIEW, k_const_ports, 1,   NULL,
    0,           NULL,           const1_value,     const1_sim,    NULL};
const odin3_celltype_def odin3_cell_constx = {
    "$_CONSTX_", ODIN3_GRAN_BIT, ODIN3_CT_ANYVIEW, k_const_ports, 1,   NULL,
    0,           NULL,           constx_value,     const0_sim,    NULL};
const odin3_celltype_def odin3_cell_constz = {
    "$_CONSTZ_", ODIN3_GRAN_BIT, ODIN3_CT_ANYVIEW, k_const_ports, 1,   NULL,
    0,           NULL,           constz_value,     const0_sim,    NULL};
