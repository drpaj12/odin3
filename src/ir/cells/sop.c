/*
 * sop.c — bit-level sum-of-products cell $sop (BLIF .names), zero inputs allowed.
 *
 * Deliberately not Yosys's $sop (DEPTH/TABLE): this one keeps the cover rows as written, with
 * parameters WIDTH (inputs) and COVER, so the BLIF writer reproduces them (IR-10).
 */
#include "cells.h"
#include "util/log.h"

#include <assert.h>

enum { SOP_WIDTH, SOP_COVER };

static const odin3_param_def k_params[] = {
    ODIN3_P_INT("WIDTH", 0),
    {"COVER", ODIN3_VAL_COVER, {ODIN3_VAL_COVER, 0, NULL, 0, 0, 0}},
};
static const odin3_port_def k_ports[] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_BIT("Y", ODIN3_DIR_OUT),
};

static bool is_input_char(uint8_t chr) {
    return chr == '0' || chr == '1' || chr == '-';
}

/* Row layout: width input chars then one output char. */
static bool row_ok(const uint8_t *row, uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
        if (!is_input_char(row[i])) {
            return false;
        }
    }
    return row[width] == '0' || row[width] == '1';
}

static odin3_status sop_verify(const odin3_value *params) {
    static const odin3_int_range k_range = {"WIDTH", 0, UINT32_MAX};
    if (odin3_cells_check_int(&params[SOP_WIDTH], &k_range) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_value *cover = &params[SOP_COVER];
    uint64_t row_len = (uint64_t)params[SOP_WIDTH].i + 1;
    if (cover->kind != ODIN3_VAL_COVER || (int64_t)cover->cover_inputs != params[SOP_WIDTH].i ||
        cover->len % row_len != 0) {
        odin3_log(ODIN3_LOG_ERROR, "$sop: COVER rows must have WIDTH+1 bytes");
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint64_t off = 0; off < cover->len; off += row_len) {
        if (!row_ok(cover->bits + off, cover->cover_inputs)) {
            odin3_log(ODIN3_LOG_ERROR, "$sop: COVER row has an invalid character");
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

/* With no inputs the output is the OR of the rows' output bits: 1 if any row says 1. */
static odin3_const sop_const_value(const odin3_value *params) {
    const odin3_value *cover = &params[SOP_COVER];
    if (params[SOP_WIDTH].i != 0 || cover->kind != ODIN3_VAL_COVER || cover->cover_inputs != 0) {
        return ODIN3_CONST_NONE;
    }
    for (uint32_t i = 0; i < cover->len; i++) {
        if (cover->bits[i] == '1') {
            return ODIN3_CONST_1;
        }
    }
    return ODIN3_CONST_0;
}

/* True when every literal of row (width input chars) matches the input bits ('-' always does). */
static bool row_matches(const odin3_sim_cell *cell, const uint8_t *row, uint32_t width) {
    const odin3_sim_span *inputs = &cell->ports[0];
    for (uint32_t i = 0; i < width; i++) {
        if (row[i] != '-' && (row[i] == '1') != (cell->values[inputs->idx[i]] != 0)) {
            return false;
        }
    }
    return true;
}

/*
 * Simulate hook (sim/cell.h). Rows ending in 1 form the ON-set, rows ending in 0 the OFF-set; BLIF
 * uses one kind per cover. ON-set: Y = 1 when a row matches. OFF-set: Y = 0 when a row matches.
 * No rows: Y = 0. A cover mixing both (not legal BLIF) follows its ON rows and ignores the OFF
 * rows, which agrees with sop_const_value for zero inputs.
 */
static void sop_sim(const odin3_sim_cell *cell) {
    const odin3_value *cover = &cell->params[SOP_COVER];
    uint32_t width = cover->cover_inputs;
    assert(width == cell->ports[0].width);
    uint64_t row_len = (uint64_t)width + 1;
    bool has_on = false;
    bool off_hit = false;
    for (uint64_t off = 0; off + row_len <= cover->len; off += row_len) {
        const uint8_t *row = cover->bits + off;
        bool hit = row_matches(cell, row, width);
        if (row[width] == '1') {
            if (hit) {
                odin3_cells_sim_out(cell, true);
                return;
            }
            has_on = true;
        } else {
            off_hit = off_hit || hit;
        }
    }
    odin3_cells_sim_out(cell, !has_on && cover->len > 0 && !off_hit);
}

const odin3_celltype_def odin3_cell_sop = {
    "$sop", ODIN3_GRAN_BIT, 0, k_ports, 2, k_params, 2, sop_verify, sop_const_value, sop_sim, NULL};
