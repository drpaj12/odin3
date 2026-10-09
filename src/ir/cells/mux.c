/* mux.c — word-level multiplexers $mux (2:1) and $pmux (one-hot parallel, B = WIDTH*S_WIDTH). */
#include "cells.h"
#include "util/log.h"

enum { MUX_WIDTH, MUX_S_WIDTH };
enum { PORT_A, PORT_B, PORT_S, PORT_Y };

static const odin3_param_def k_mux_params[] = {ODIN3_P_INT("WIDTH", 1)};
static const odin3_port_def k_mux_ports[] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("B", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_BIT("S", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("Y", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status mux_verify(const odin3_value *params) {
    static const odin3_int_range k_range = {"WIDTH", 1, UINT32_MAX};
    return odin3_cells_check_int(&params[MUX_WIDTH], &k_range);
}

/* Copies the bits of span `from` onto Y (as wide). */
static void copy_to_y(const odin3_sim_cell *cell, const odin3_sim_span *from) {
    const odin3_sim_span *y_span = &cell->ports[PORT_Y];
    for (uint32_t k = 0; k < y_span->width; k++) {
        cell->values[y_span->idx[k]] = cell->values[from->idx[k]];
    }
}

/* Simulate hook (sim/cell.h): Y = S ? B : A. */
static void mux_sim(const odin3_sim_cell *cell) {
    bool sel = odin3_cells_sim_in(cell, PORT_S);
    copy_to_y(cell, &cell->ports[sel ? PORT_B : PORT_A]);
}

const odin3_celltype_def odin3_cell_mux = {
    "$mux", ODIN3_GRAN_WORD, 0, k_mux_ports, 4, k_mux_params, 1, mux_verify, NULL, mux_sim, NULL};

static const odin3_param_def k_pmux_params[] = {ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("S_WIDTH", 1)};

/* B carries one WIDTH-bit input per select bit; 0 when the product does not fit. */
static uint32_t pmux_b_width(const odin3_value *params, uint32_t port) {
    (void)port;
    uint32_t width = 0;
    if (!odin3_cells_product(&params[MUX_WIDTH], &params[MUX_S_WIDTH], &width)) {
        odin3_log(ODIN3_LOG_ERROR, "$pmux: B width WIDTH*S_WIDTH is not in 0..%u", UINT32_MAX);
        return 0;
    }
    return width;
}

static const odin3_port_def k_pmux_ports[] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_FN("B", ODIN3_DIR_IN, pmux_b_width),
    ODIN3_PORT_VEC("S", ODIN3_DIR_IN, "S_WIDTH"),
    ODIN3_PORT_VEC("Y", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status pmux_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[2] = {
        {"WIDTH", 1, UINT32_MAX},
        {"S_WIDTH", 1, UINT32_MAX},
    };
    if (odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges)) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!odin3_cells_product(&params[MUX_WIDTH], &params[MUX_S_WIDTH], &(uint32_t){0})) {
        odin3_log(ODIN3_LOG_ERROR, "$pmux: WIDTH*S_WIDTH exceeds %u", UINT32_MAX);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* Bit `bit` of the OR of the B slices whose select bit is set. */
static uint8_t selected_or(const odin3_sim_cell *cell, uint32_t bit) {
    const odin3_sim_span *b_span = &cell->ports[PORT_B];
    const odin3_sim_span *s_span = &cell->ports[PORT_S];
    uint32_t width = cell->ports[PORT_Y].width;
    for (uint32_t j = 0; j < s_span->width; j++) {
        if (cell->values[s_span->idx[j]] != 0 && cell->values[b_span->idx[j * width + bit]] != 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Simulate hook (sim/cell.h): Y = A when no select bit is set, else the OR of the selected B
 * slices. With exactly one bit set that is the slice; with several, Yosys's $pmux gives x and its
 * gate-level lowering (simplemap) gives this OR, which the 2-state simulator follows.
 */
static void pmux_sim(const odin3_sim_cell *cell) {
    const odin3_sim_span *s_span = &cell->ports[PORT_S];
    bool any = false;
    for (uint32_t j = 0; j < s_span->width && !any; j++) {
        any = cell->values[s_span->idx[j]] != 0;
    }
    if (!any) {
        copy_to_y(cell, &cell->ports[PORT_A]);
        return;
    }
    const odin3_sim_span *y_span = &cell->ports[PORT_Y];
    for (uint32_t k = 0; k < y_span->width; k++) {
        cell->values[y_span->idx[k]] = selected_or(cell, k);
    }
}

const odin3_celltype_def odin3_cell_pmux = {"$pmux", ODIN3_GRAN_WORD, 0,   k_pmux_ports,
                                            4,       k_pmux_params,   2,   pmux_verify,
                                            NULL,    pmux_sim,        NULL};
