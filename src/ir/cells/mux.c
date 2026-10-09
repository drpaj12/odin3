/* mux.c — word-level multiplexers $mux (2:1) and $pmux (one-hot parallel, B = WIDTH*S_WIDTH). */
#include "cells.h"
#include "util/log.h"

enum { MUX_WIDTH, MUX_S_WIDTH };

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

const odin3_celltype_def odin3_cell_mux = {
    "$mux", ODIN3_GRAN_WORD, 0, k_mux_ports, 4, k_mux_params, 1, mux_verify, NULL, NULL};

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

const odin3_celltype_def odin3_cell_pmux = {
    "$pmux", ODIN3_GRAN_WORD, 0, k_pmux_ports, 4, k_pmux_params, 2, pmux_verify, NULL, NULL};
