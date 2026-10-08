/* dff.c — bit-level flip-flops and latches $_DFF_P_/N_, $_DLATCH_P_/N_, $_FF_ (BLIF .latch). */
#include "cells.h"

static const odin3_port_def k_dff_ports[] = {
    ODIN3_PORT_BIT("C", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};
static const odin3_port_def k_latch_ports[] = {
    ODIN3_PORT_BIT("E", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};
static const odin3_port_def k_ff_ports[] = {
    ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};

#define ODIN3_STORAGE(var, label, ports, count)                                                    \
    const odin3_celltype_def var = {label, ODIN3_GRAN_BIT,          0, ports,                      \
                                    count, odin3_cells_init_params, 1, odin3_cells_init_verify,    \
                                    NULL}

ODIN3_STORAGE(odin3_cell_dff_p, "$_DFF_P_", k_dff_ports, 3);
ODIN3_STORAGE(odin3_cell_dff_n, "$_DFF_N_", k_dff_ports, 3);
ODIN3_STORAGE(odin3_cell_dlatch_p, "$_DLATCH_P_", k_latch_ports, 3);
ODIN3_STORAGE(odin3_cell_dlatch_n, "$_DLATCH_N_", k_latch_ports, 3);
ODIN3_STORAGE(odin3_cell_ff, "$_FF_", k_ff_ports, 2);
