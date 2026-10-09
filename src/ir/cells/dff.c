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

/*
 * Simulate hooks (sim/cell.h). State: one byte, the stored Q. D is the second-to-last port and Q
 * the last for every type here; INIT is parameter 0.
 */
enum { SEQ_INIT, LATCH_E = 0, D_FROM_END = 2 };

static bool in_d(const odin3_sim_cell *cell) {
    return odin3_cells_sim_in(cell, cell->n_ports - D_FROM_END);
}

/* Edge-triggered: the state samples D on edge; COMB drives Q from the state alone. */
static void flop_sim(const odin3_sim_cell *cell, odin3_sim_event edge) {
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = odin3_sim_init_bit(&cell->params[SEQ_INIT]);
    } else if (cell->event == edge) {
        cell->state[0] = (uint8_t)(in_d(cell) ? 1 : 0);
    } else if (cell->event == ODIN3_SIM_COMB) {
        odin3_cells_sim_out(cell, cell->state[0] != 0);
    }
}

/* Level-sensitive: transparent (state follows D) while E equals active; edges are ignored. */
static void latch_sim(const odin3_sim_cell *cell, bool active) {
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = odin3_sim_init_bit(&cell->params[SEQ_INIT]);
    } else if (cell->event == ODIN3_SIM_COMB) {
        if (odin3_cells_sim_in(cell, LATCH_E) == active) {
            cell->state[0] = (uint8_t)(in_d(cell) ? 1 : 0);
        }
        odin3_cells_sim_out(cell, cell->state[0] != 0);
    }
}

static void dff_p_sim(const odin3_sim_cell *cell) {
    flop_sim(cell, ODIN3_SIM_POSEDGE);
}
static void dff_n_sim(const odin3_sim_cell *cell) {
    flop_sim(cell, ODIN3_SIM_NEGEDGE);
}
/* $_FF_ (BLIF .latch without a clock): the global clock, sampled at the rising edge. */
static void ff_sim(const odin3_sim_cell *cell) {
    flop_sim(cell, ODIN3_SIM_POSEDGE);
}
static void dlatch_p_sim(const odin3_sim_cell *cell) {
    latch_sim(cell, true);
}
static void dlatch_n_sim(const odin3_sim_cell *cell) {
    latch_sim(cell, false);
}

#define ODIN3_STORAGE(var, label, flags, ports, sim)                                               \
    const odin3_celltype_def var = {                                                               \
        label, ODIN3_GRAN_BIT,          flags, ports, ODIN3_NELEM(ports), odin3_cells_init_params, \
        1,     odin3_cells_init_verify, NULL,  sim}

enum {
    EDGE_CLOCKED = ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0,
    LEVEL_ENABLED = ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0
};

ODIN3_STORAGE(odin3_cell_dff_p, "$_DFF_P_", EDGE_CLOCKED, k_dff_ports, dff_p_sim);
ODIN3_STORAGE(odin3_cell_dff_n, "$_DFF_N_", EDGE_CLOCKED, k_dff_ports, dff_n_sim);
ODIN3_STORAGE(odin3_cell_dlatch_p, "$_DLATCH_P_", LEVEL_ENABLED, k_latch_ports, dlatch_p_sim);
ODIN3_STORAGE(odin3_cell_dlatch_n, "$_DLATCH_N_", LEVEL_ENABLED, k_latch_ports, dlatch_n_sim);
ODIN3_STORAGE(odin3_cell_ff, "$_FF_", ODIN3_CT_SEQ_EDGE, k_ff_ports, ff_sim);
