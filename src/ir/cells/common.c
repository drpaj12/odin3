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

bool odin3_cells_product(const odin3_value *lhs, const odin3_value *rhs, uint32_t *out) {
    if (lhs->kind != ODIN3_VAL_INT || lhs->i < 0 || lhs->i > (int64_t)UINT32_MAX) {
        return false;
    }
    uint64_t prod = (uint64_t)lhs->i;
    if (rhs != NULL) {
        if (rhs->kind != ODIN3_VAL_INT || rhs->i < 0 || rhs->i > (int64_t)UINT32_MAX) {
            return false;
        }
        prod *= (uint64_t)rhs->i;
    }
    if (prod > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)prod;
    return true;
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

/* --- word-level simulation (cells.h, sim/word.h) ---------------------------------------------- */

enum { WORD_A, WORD_B, WORD_DST, WORD_COUNT };

static odin3_word_port word_port(const odin3_sim_cell *cell, uint32_t port) {
    const odin3_word_port out = {cell->values, cell->ports[port],
                                 cell->params[port].kind == ODIN3_VAL_INT &&
                                     cell->params[port].i != 0};
    return out;
}

odin3_word_bin odin3_cells_word_load(const odin3_sim_cell *cell, uint32_t width) {
    uint64_t *words = (uint64_t *)cell->scratch;
    uint32_t stride = odin3_word_limbs(width);
    odin3_word_bin op = {words + (size_t)WORD_DST * stride, words, words + stride, width};
    const odin3_word_port a_port = word_port(cell, WORD_A); /* A_SIGNED is parameter 0 */
    odin3_word_load(words, width, &a_port);
    if (cell->n_ports > 2) {
        const odin3_word_port b_port = word_port(cell, WORD_B); /* B_SIGNED is parameter 1 */
        odin3_word_load(words + stride, width, &b_port);
    } else {
        odin3_word_zero(words + stride, width);
    }
    return op;
}

void odin3_cells_word_store(const odin3_sim_cell *cell, const uint64_t *word, uint32_t width) {
    const odin3_word_port y_port = {cell->values, cell->ports[cell->n_ports - 1], false};
    odin3_word_store(word, width, &y_port);
}

void odin3_cells_flag_store(const odin3_sim_cell *cell, bool flag) {
    const odin3_sim_span *y_span = &cell->ports[cell->n_ports - 1];
    for (uint32_t k = 0; k < y_span->width; k++) {
        cell->values[y_span->idx[k]] = (uint8_t)(k == 0 && flag ? 1 : 0);
    }
}

void odin3_cells_word_sim(const odin3_sim_cell *cell, odin3_word_fn fn) {
    uint32_t width = cell->ports[cell->n_ports - 1].width;
    odin3_word_bin op = odin3_cells_word_load(cell, width);
    fn(&op);
    odin3_cells_word_store(cell, op.dst, width);
}

/* max(A_WIDTH, B_WIDTH) + 1: room for both operands as signed integers; 0 when it overflows. */
static uint32_t cmp_width(const odin3_sim_cell *cell) {
    uint32_t a_width = cell->ports[WORD_A].width;
    uint32_t b_width = cell->ports[WORD_B].width;
    uint32_t wider = a_width > b_width ? a_width : b_width;
    return wider < UINT32_MAX ? wider + 1 : 0;
}

int odin3_cells_word_compare(const odin3_sim_cell *cell) {
    odin3_word_bin op = odin3_cells_word_load(cell, cmp_width(cell));
    return odin3_word_scmp(&op);
}

static odin3_status words_scratch(uint32_t width, uint32_t *bytes) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < WORD_COUNT; i++) {
        if (!odin3_word_reserve(width, &total)) {
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    *bytes = total;
    return ODIN3_OK;
}

odin3_status odin3_cells_y_scratch(const odin3_sim_cell *cell, uint32_t *bytes) {
    return words_scratch(cell->ports[cell->n_ports - 1].width, bytes);
}

odin3_status odin3_cells_cmp_scratch(const odin3_sim_cell *cell, uint32_t *bytes) {
    uint32_t width = cmp_width(cell);
    return width == 0 ? ODIN3_ERR_INVALID_ARG : words_scratch(width, bytes);
}
