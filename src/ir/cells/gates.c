/* gates.c — bit-level gates $_BUF_, $_NOT_, $_AND_, $_OR_, $_XOR_, $_NAND_, $_NOR_, $_XNOR_,
 * $_MUX_. */
#include "cells.h"

static const odin3_port_def k_unary_ports[] = {
    ODIN3_PORT_BIT("A", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Y", ODIN3_DIR_OUT),
};
static const odin3_port_def k_binary_ports[] = {
    ODIN3_PORT_BIT("A", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("B", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Y", ODIN3_DIR_OUT),
};
static const odin3_port_def k_mux_ports[] = {
    ODIN3_PORT_BIT("A", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("B", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("S", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Y", ODIN3_DIR_OUT),
};

#define ODIN3_GATE(var, label, ports, count)                                                       \
    const odin3_celltype_def var = {label, ODIN3_GRAN_BIT, 0, ports, count, NULL, 0, NULL, NULL}

ODIN3_GATE(odin3_cell_g_buf, "$_BUF_", k_unary_ports, 2);
ODIN3_GATE(odin3_cell_g_not, "$_NOT_", k_unary_ports, 2);
ODIN3_GATE(odin3_cell_g_and, "$_AND_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_or, "$_OR_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_xor, "$_XOR_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_nand, "$_NAND_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_nor, "$_NOR_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_xnor, "$_XNOR_", k_binary_ports, 3);
ODIN3_GATE(odin3_cell_g_mux, "$_MUX_", k_mux_ports, 4);
