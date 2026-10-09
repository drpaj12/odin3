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

/* Simulate hooks (sim/cell.h): combinational, 2-state. */
enum { GATE_A, GATE_B, GATE_S };

static bool in_a(const odin3_sim_cell *cell) {
    return odin3_cells_sim_in(cell, GATE_A);
}
static bool in_b(const odin3_sim_cell *cell) {
    return odin3_cells_sim_in(cell, GATE_B);
}

static void buf_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, in_a(cell));
}
static void not_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, !in_a(cell));
}
static void and_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, in_a(cell) && in_b(cell));
}
static void or_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, in_a(cell) || in_b(cell));
}
static void xor_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, in_a(cell) != in_b(cell));
}
static void nand_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, !(in_a(cell) && in_b(cell)));
}
static void nor_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, !(in_a(cell) || in_b(cell)));
}
static void xnor_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, in_a(cell) == in_b(cell));
}
/* Y = S ? B : A (Yosys). */
static void mux_sim(const odin3_sim_cell *cell) {
    odin3_cells_sim_out(cell, odin3_cells_sim_in(cell, GATE_S) ? in_b(cell) : in_a(cell));
}

#define ODIN3_GATE(var, label, ports, count, sim)                                                  \
    const odin3_celltype_def var = {label, ODIN3_GRAN_BIT, 0,    ports, count, NULL,               \
                                    0,     NULL,           NULL, sim,   NULL}

ODIN3_GATE(odin3_cell_g_buf, "$_BUF_", k_unary_ports, 2, buf_sim);
ODIN3_GATE(odin3_cell_g_not, "$_NOT_", k_unary_ports, 2, not_sim);
ODIN3_GATE(odin3_cell_g_and, "$_AND_", k_binary_ports, 3, and_sim);
ODIN3_GATE(odin3_cell_g_or, "$_OR_", k_binary_ports, 3, or_sim);
ODIN3_GATE(odin3_cell_g_xor, "$_XOR_", k_binary_ports, 3, xor_sim);
ODIN3_GATE(odin3_cell_g_nand, "$_NAND_", k_binary_ports, 3, nand_sim);
ODIN3_GATE(odin3_cell_g_nor, "$_NOR_", k_binary_ports, 3, nor_sim);
ODIN3_GATE(odin3_cell_g_xnor, "$_XNOR_", k_binary_ports, 3, xnor_sim);
ODIN3_GATE(odin3_cell_g_mux, "$_MUX_", k_mux_ports, 4, mux_sim);
