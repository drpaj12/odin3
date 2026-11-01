/*
 * sop.c — bit-level sum-of-products cell $sop (BLIF .names), zero inputs allowed.
 *
 * Deliberately not Yosys's $sop (DEPTH/TABLE): this one keeps the cover rows as written, with
 * parameters WIDTH (inputs) and COVER, so the BLIF writer reproduces them (IR-10).
 */
#include "cells.h"
#include "util/log.h"

#include <assert.h>
#include <string.h>

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

/*
 * The value of the cover for the input bits in assign (bit i is input i): the semantics of
 * sop_sim below, which is its only other user. Rows ending in 1 form the ON-set, rows ending in 0
 * the OFF-set; BLIF uses one kind per cover. ON-set: Y = 1 when a row matches. OFF-set: Y = 0 when
 * a row matches. No rows: Y = 0. A cover mixing both (not legal BLIF) follows its ON rows and
 * ignores the OFF rows, which agrees with sop_const_value for zero inputs.
 */
static bool cover_value(const odin3_value *cover, bool (*bit_of)(const void *, uint32_t),
                        const void *src) {
    uint32_t width = cover->cover_inputs;
    uint64_t row_len = (uint64_t)width + 1;
    bool has_on = false;
    bool off_hit = false;
    for (uint64_t off = 0; off + row_len <= cover->len; off += row_len) {
        const uint8_t *row = cover->bits + off;
        bool hit = true;
        for (uint32_t i = 0; hit && i < width; i++) {
            hit = row[i] == '-' || (row[i] == '1') == bit_of(src, i);
        }
        if (row[width] == '1') {
            if (hit) {
                return true;
            }
            has_on = true;
        } else {
            off_hit = off_hit || hit;
        }
    }
    return !has_on && cover->len > 0 && !off_hit;
}

static bool bit_of_cell(const void *src, uint32_t index) {
    const odin3_sim_cell *cell = src;
    return cell->values[cell->ports[0].idx[index]] != 0;
}

static bool bit_of_assign(const void *src, uint32_t index) {
    return (*(const uint32_t *)src >> index & 1U) != 0;
}

static void sop_sim(const odin3_sim_cell *cell);

enum {
    SOP_TABLE_MAX_WIDTH = 6,           /* the widest cover cached as a truth table (64 bits) */
    SOP_TABLE_BYTES = sizeof(uint64_t) /* the table; state byte 0 says it is built */
};

uint32_t odin3_cell_sop_sim_state_bytes(const odin3_celltype_def *def, const odin3_value *params) {
    bool cached = def->simulate == sop_sim && params[SOP_WIDTH].i >= 1 &&
                  params[SOP_WIDTH].i <= SOP_TABLE_MAX_WIDTH;
    return cached ? 1U + (uint32_t)SOP_TABLE_BYTES : 0U;
}

/* Builds the truth table of the cover into state[1..]: bit k is the value for the inputs k. */
static void build_table(const odin3_sim_cell *cell) {
    const odin3_value *cover = &cell->params[SOP_COVER];
    uint64_t table = 0;
    for (uint32_t assign = 0; assign < (1U << cover->cover_inputs); assign++) {
        table |= (uint64_t)cover_value(cover, bit_of_assign, &assign) << assign;
    }
    memcpy(cell->state + 1, &table, SOP_TABLE_BYTES);
    cell->state[0] = 1;
}

/*
 * Simulate hook (sim/cell.h). A cover of 1 to 6 inputs is cached in the cell's state (the
 * simulator gives a $sop that: odin3_cell_sop_sim_state_bytes) as a 64-bit truth table, built at
 * the first evaluation, so an evaluation reads the inputs and one word instead of walking the rows
 * (the covers of a lowered netlist are small and many: the walk is cache misses on the IR). A
 * wider cover walks its rows. The result is the same either way (cover_value).
 */
static void sop_sim(const odin3_sim_cell *cell) {
    const odin3_sim_span *in = &cell->ports[0];
    if (cell->n_state == 0) { /* a wide cover: the rows */
        const odin3_value *cover = &cell->params[SOP_COVER];
        assert(cover->cover_inputs == in->width);
        odin3_cells_sim_out(cell, cover_value(cover, bit_of_cell, cell));
        return;
    }
    if (cell->state[0] == 0) {
        build_table(cell);
    }
    uint32_t index = 0; /* the width is the span's: the IR parameters stay cold */
    for (uint32_t i = 0; i < in->width; i++) {
        index |= (uint32_t)(cell->values[in->idx[i]] != 0) << i;
    }
    uint64_t table = 0;
    memcpy(&table, cell->state + 1, SOP_TABLE_BYTES);
    odin3_cells_sim_out(cell, (table >> index & 1U) != 0);
}

const odin3_celltype_def odin3_cell_sop = {
    "$sop", ODIN3_GRAN_BIT, 0, k_ports, 2, k_params, 2, sop_verify, sop_const_value, sop_sim, NULL};
