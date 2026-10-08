/* common.c — parameter checks and table pieces shared by the built-in cell types. */
#include "cells.h"
#include "util/log.h"

const uint8_t odin3_cells_zero_bit[1] = {0};

odin3_status odin3_cells_check_int(const odin3_value *val, const odin3_int_range *range) {
    if (val->kind != ODIN3_VAL_INT || val->i < range->lo || val->i > range->hi) {
        odin3_log(ODIN3_LOG_ERROR, "cell parameter %s must be an int in %lld..%lld", range->name,
                  (long long)range->lo, (long long)range->hi);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_cells_check_ints(const odin3_value *params, const odin3_int_range *ranges,
                                    uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (odin3_cells_check_int(&params[i], &ranges[i]) != ODIN3_OK) {
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_cells_check_bits(const odin3_value *val, const char *name, uint32_t width) {
    if (val->kind != ODIN3_VAL_BITS || val->len != width) {
        odin3_log(ODIN3_LOG_ERROR, "cell parameter %s must be %u bits", name, width);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

const odin3_port_def odin3_cells_binary_ports[3] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "A_WIDTH"),
    ODIN3_PORT_VEC("B", ODIN3_DIR_IN, "B_WIDTH"),
    ODIN3_PORT_VEC("Y", ODIN3_DIR_OUT, "Y_WIDTH"),
};

const odin3_param_def odin3_cells_binary_params[5] = {
    ODIN3_P_INT("A_SIGNED", 0), ODIN3_P_INT("B_SIGNED", 0), ODIN3_P_INT("A_WIDTH", 1),
    ODIN3_P_INT("B_WIDTH", 1),  ODIN3_P_INT("Y_WIDTH", 1),
};

odin3_status odin3_cells_binary_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[5] = {
        {"A_SIGNED", 0, 1},         {"B_SIGNED", 0, 1},         {"A_WIDTH", 1, UINT32_MAX},
        {"B_WIDTH", 1, UINT32_MAX}, {"Y_WIDTH", 1, UINT32_MAX},
    };
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_port_def odin3_cells_unary_ports[2] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "A_WIDTH"),
    ODIN3_PORT_VEC("Y", ODIN3_DIR_OUT, "Y_WIDTH"),
};

const odin3_param_def odin3_cells_unary_params[3] = {
    ODIN3_P_INT("A_SIGNED", 0),
    ODIN3_P_INT("A_WIDTH", 1),
    ODIN3_P_INT("Y_WIDTH", 1),
};

odin3_status odin3_cells_unary_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[3] = {
        {"A_SIGNED", 0, 1},
        {"A_WIDTH", 1, UINT32_MAX},
        {"Y_WIDTH", 1, UINT32_MAX},
    };
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

/* INIT: BLIF 0, 1, 2 = don't care, 3 = unknown (IR-10). */
const odin3_param_def odin3_cells_init_params[1] = {ODIN3_P_INT("INIT", 3)};

odin3_status odin3_cells_init_verify(const odin3_value *params) {
    static const odin3_int_range k_range = {"INIT", 0, 3};
    return odin3_cells_check_int(&params[0], &k_range);
}
