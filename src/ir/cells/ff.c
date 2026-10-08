/* ff.c — word-level flip-flops $dff, $dffe, $adff, $sdff. */
#include "cells.h"

enum { FF_WIDTH, FF_VALUE = 3 };

/* $dff: WIDTH CLK_POLARITY. */
static const odin3_param_def k_dff_params[] = {ODIN3_P_INT("WIDTH", 1),
                                               ODIN3_P_INT("CLK_POLARITY", 1)};
static const odin3_port_def k_dff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status dff_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[2] = {{"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}};
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_dff = {
    "$dff", ODIN3_GRAN_WORD, 0, k_dff_ports, 3, k_dff_params, 2, dff_verify, NULL};

/* $dffe adds EN / EN_POLARITY. */
static const odin3_param_def k_dffe_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1)};
static const odin3_port_def k_dffe_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status dffe_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[3] = {
        {"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}, {"EN_POLARITY", 0, 1}};
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_dffe = {
    "$dffe", ODIN3_GRAN_WORD, 0, k_dffe_ports, 4, k_dffe_params, 3, dffe_verify, NULL};

/* $adff / $sdff add a reset: ARST/SRST, its polarity and its value (WIDTH bits). */
typedef struct reset_names {
    const char *polarity;
    const char *value;
} reset_names;

static odin3_status reset_verify(const odin3_value *params, const reset_names *names) {
    const odin3_int_range ranges[3] = {
        {"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}, {names->polarity, 0, 1}};
    if (odin3_cells_check_ints(params, ranges, ODIN3_NELEM(ranges)) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_cells_check_bits(&params[FF_VALUE], names->value, (uint32_t)params[FF_WIDTH].i);
}

static odin3_status adff_verify(const odin3_value *params) {
    static const reset_names k_names = {"ARST_POLARITY", "ARST_VALUE"};
    return reset_verify(params, &k_names);
}
static odin3_status sdff_verify(const odin3_value *params) {
    static const reset_names k_names = {"SRST_POLARITY", "SRST_VALUE"};
    return reset_verify(params, &k_names);
}

static const odin3_param_def k_adff_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("ARST_POLARITY", 1),
    ODIN3_P_BITS("ARST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_param_def k_sdff_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("SRST_POLARITY", 1),
    ODIN3_P_BITS("SRST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_port_def k_adff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("ARST", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};
static const odin3_port_def k_sdff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("SRST", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

const odin3_celltype_def odin3_cell_adff = {
    "$adff", ODIN3_GRAN_WORD, 0, k_adff_ports, 4, k_adff_params, 4, adff_verify, NULL};
const odin3_celltype_def odin3_cell_sdff = {
    "$sdff", ODIN3_GRAN_WORD, 0, k_sdff_ports, 4, k_sdff_params, 4, sdff_verify, NULL};
