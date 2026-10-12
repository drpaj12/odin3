/*
 * lut.c — the K-input look-up table $lut (3D spec §2, AB-2; docs/IR.md "LUT cells"), with Yosys
 * $lut semantics, and its table helpers (ir/lut.h).
 *
 * Parameters WIDTH (INT, 1 .. ODIN3_LUT_MAX_WIDTH, default 1) and LUT (BITS of exactly 2^WIDTH
 * bits, each 0 or 1; default LUT[0] = 0, LUT[1] = 1, the identity); ports A (in, WIDTH bits, LSB
 * first) and Y (out, 1 bit). Y = LUT[i] with i = sum of A[j] * 2^j, LUT[i] being bit i of the
 * parameter counted from its least significant bit (IR-10). Bit granularity, no flags.
 */
#include "cells.h"
#include "util/log.h"

#include <string.h>

enum {
    WORD_SHIFT = 6,   /* a table word holds 2^6 = 64 bits */
    WORD_MASK = 63,   /* bit position within a table word */
    ENGINE_SHIFT = 5, /* an engine word holds 2^5 = 32 bits */
    ENGINE_MASK = 31  /* bit position within an engine word */
};

static const uint8_t k_identity[2] = {ODIN3_BIT_0, ODIN3_BIT_1};

static const odin3_param_def k_params[] = {
    ODIN3_P_INT("WIDTH", 1),
    ODIN3_P_BITS("LUT", k_identity, 2),
};
static const odin3_port_def k_ports[] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_BIT("Y", ODIN3_DIR_OUT),
};

/* The index of the first LUT bit that is neither 0 nor 1; lut->len when there is none. */
static uint32_t first_non_binary(const odin3_value *lut) {
    for (uint32_t i = 0; i < lut->len; i++) {
        if (lut->bits[i] > ODIN3_BIT_1) {
            return i;
        }
    }
    return lut->len;
}

/* verify's conditions, without logging (odin3_lut_table). */
static bool lut_valid(const odin3_value *params) {
    const odin3_value *width = &params[ODIN3_LUT_P_WIDTH];
    if (width->kind != ODIN3_VAL_INT || width->i < 1 || width->i > ODIN3_LUT_MAX_WIDTH) {
        return false;
    }
    const odin3_value *lut = &params[ODIN3_LUT_P_LUT];
    return lut->kind == ODIN3_VAL_BITS && lut->len == (1U << (uint32_t)width->i) &&
           first_non_binary(lut) == lut->len;
}

static odin3_status lut_verify(const odin3_value *params) {
    static const odin3_int_range k_range = {"WIDTH", 1, ODIN3_LUT_MAX_WIDTH};
    if (odin3_cells_check_int(&params[ODIN3_LUT_P_WIDTH], &k_range) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_value *lut = &params[ODIN3_LUT_P_LUT];
    uint32_t bits = 1U << (uint32_t)params[ODIN3_LUT_P_WIDTH].i;
    if (odin3_cells_check_bits(lut, "LUT", bits) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t bad = first_non_binary(lut);
    if (bad < lut->len) {
        odin3_log(ODIN3_LOG_ERROR, "$lut: LUT bit %u is %c; only 0 and 1 are allowed", bad,
                  lut->bits[bad] == ODIN3_BIT_Z ? 'z' : 'x');
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* 0 or 1 when every LUT bit is (the output then ignores A), else none (IR-4). */
static odin3_const lut_const_value(const odin3_value *params) {
    const odin3_value *lut = &params[ODIN3_LUT_P_LUT];
    if (lut->kind != ODIN3_VAL_BITS || lut->len == 0 || lut->bits[0] > ODIN3_BIT_1) {
        return ODIN3_CONST_NONE;
    }
    for (uint32_t i = 1; i < lut->len; i++) {
        if (lut->bits[i] != lut->bits[0]) {
            return ODIN3_CONST_NONE;
        }
    }
    return lut->bits[0] == ODIN3_BIT_1 ? ODIN3_CONST_1 : ODIN3_CONST_0;
}

/* Simulate hook (sim/cell.h): i from the A bytes, A[0] least significant; Y = LUT[i]. */
static void lut_sim(const odin3_sim_cell *cell) {
    const odin3_sim_span *a_span = &cell->ports[ODIN3_LUT_PORT_A];
    uint32_t idx = 0;
    for (uint32_t j = 0; j < a_span->width; j++) {
        idx |= (uint32_t)cell->values[a_span->idx[j]] << j;
    }
    odin3_cells_sim_out(cell, cell->params[ODIN3_LUT_P_LUT].bits[idx] == ODIN3_BIT_1);
}

const odin3_celltype_def odin3_cell_lut = {
    "$lut", ODIN3_GRAN_BIT, 0, k_ports, 2, k_params, 2, lut_verify, lut_const_value, lut_sim, NULL};

/* --- table helpers (ir/lut.h) -------------------------------------------------------------- */

uint32_t odin3_lut_builtin_index(void) {
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        if (odin3_builtin_celltypes[i] == &odin3_cell_lut) {
            return i;
        }
    }
    return odin3_builtin_celltype_count; /* not reached: cells/builtin.c lists $lut */
}

uint32_t odin3_lut_words(uint32_t width) {
    uint32_t capped = width > ODIN3_LUT_MAX_WIDTH ? ODIN3_LUT_MAX_WIDTH : width;
    return capped <= WORD_SHIFT ? 1 : 1U << (capped - WORD_SHIFT);
}

/* Sets params to WIDTH width and LUT bits (2^width bytes). */
static void publish(uint32_t width, const uint8_t *bits, odin3_value params[2]) {
    params[ODIN3_LUT_P_WIDTH] = odin3_value_int(width);
    params[ODIN3_LUT_P_LUT] = (odin3_value){ODIN3_VAL_BITS, 0, bits, 1U << width, 0, 0};
}

odin3_status odin3_lut_params(odin3_arena *arena, uint32_t width, const uint64_t *words,
                              odin3_value params[2]) {
    if (arena == NULL || words == NULL || params == NULL || width == 0 ||
        width > ODIN3_LUT_MAX_WIDTH) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t len = 1U << width;
    uint8_t *bits = odin3_arena_alloc(arena, len);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < len; i++) {
        bits[i] = (uint8_t)((words[i >> WORD_SHIFT] >> (i & WORD_MASK)) & 1U);
    }
    publish(width, bits, params);
    return ODIN3_OK;
}

bool odin3_lut_table(const odin3_value params[2], uint64_t *words, uint32_t n_words) {
    if (params == NULL || words == NULL || !lut_valid(params)) {
        return false;
    }
    if (n_words < odin3_lut_words((uint32_t)params[ODIN3_LUT_P_WIDTH].i)) {
        return false;
    }
    const odin3_value *lut = &params[ODIN3_LUT_P_LUT];
    memset(words, 0, (size_t)n_words * sizeof *words);
    for (uint32_t i = 0; i < lut->len; i++) {
        if (lut->bits[i] == ODIN3_BIT_1) {
            words[i >> WORD_SHIFT] |= (uint64_t)1 << (i & WORD_MASK);
        }
    }
    return true;
}

odin3_status odin3_lut_from_engine(odin3_arena *arena, uint32_t n_inputs, uint32_t lut_size,
                                   const uint32_t *truth, odin3_value params[2]) {
    if (arena == NULL || truth == NULL || params == NULL || n_inputs == 0 || n_inputs > lut_size ||
        lut_size > ODIN3_LUT_MAX_WIDTH) {
        return ODIN3_ERR_INVALID_ARG;
    }
    /* i < 2^n_inputs <= 2^lut_size <= 32 W: every bit read lies inside the W words */
    uint32_t len = 1U << n_inputs;
    uint8_t *bits = odin3_arena_alloc(arena, len);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < len; i++) {
        bits[i] = (uint8_t)((truth[i >> ENGINE_SHIFT] >> (i & ENGINE_MASK)) & 1U);
    }
    publish(n_inputs, bits, params);
    return ODIN3_OK;
}
