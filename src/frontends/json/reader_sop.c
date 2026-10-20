/*
 * reader_sop.c — Yosys `$sop` (DEPTH, TABLE, WIDTH) as the cover cell, and a `$lut` of no inputs
 * as the constant cover it was (M-8, R-2 d).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

enum { SOP_PARAM_WIDTH = 0, SOP_PARAM_COVER = 1, TABLE_BITS_PER_LITERAL = 2 };

/* The parameter of the cell named key, as an INT; absent is an error. */
static odin3_status int_member(jr *rd, const odin3_json_node *params, const jr_where *where,
                               const char *key, int64_t *num) {
    const odin3_json_node *node = jr_member(params, key);
    char path[JR_NAME_BUF];
    (void)snprintf(path, sizeof path, "parameters.%s", key);
    jr_where here = {where->section, where->key, path};
    if (node == NULL) {
        return jr_error(rd, params, &here, "a $sop has no %s", key);
    }
    odin3_value val;
    odin3_status st = jr_param_value(rd, ODIN3_VAL_INT, node, &here, &val);
    *num = val.i;
    return st;
}

/* The literal of input j in row r from the TABLE text (most significant bit first). */
static char literal(const char *table, size_t length, uint64_t bit0) {
    bool one = table[length - 1 - (bit0 + 1)] == '1';
    bool zero = table[length - 1 - bit0] == '1';
    if (one && !zero) {
        return '1';
    }
    return zero && !one ? '0' : '-';
}

static odin3_status table_cover(jr *rd, const odin3_json_node *table, uint64_t width,
                                uint64_t depth, odin3_value *cover) {
    size_t len = 0;
    const char *text = jr_text(table, &len);
    if (jr_kind(table) != ODIN3_JSON_STRING ||
        (uint64_t)len != TABLE_BITS_PER_LITERAL * width * depth || width > ODIN3_READER_MAX_WIDTH) {
        return jr_error(rd, table, NULL, "TABLE is not 2 * WIDTH * DEPTH binary digits");
    }
    uint8_t *bytes = jr_alloc(rd, depth * (width + 1));
    if (bytes == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    for (uint64_t row = 0; row < depth; row++) {
        for (uint64_t in = 0; in < width; in++) {
            bytes[row * (width + 1) + in] =
                (uint8_t)literal(text, len, TABLE_BITS_PER_LITERAL * (row * width + in));
        }
        bytes[row * (width + 1) + width] = '1';
    }
    *cover = (odin3_value){.kind = ODIN3_VAL_COVER,
                           .bits = bytes,
                           .len = (uint32_t)(depth * (width + 1)),
                           .cover_inputs = (uint32_t)width};
    return ODIN3_OK;
}

odin3_status jr_sop_params(jr *rd, const odin3_json_node *params, const jr_where *where) {
    int64_t width = 0;
    int64_t depth = 0;
    odin3_status st = int_member(rd, params, where, "WIDTH", &width);
    if (st == ODIN3_OK) {
        st = int_member(rd, params, where, "DEPTH", &depth);
    }
    if (st == ODIN3_OK && (width < 0 || depth < 0 || depth > ODIN3_READER_MAX_TOTAL_WIDTH)) {
        return jr_error(rd, params, where, "WIDTH or DEPTH of a $sop is out of range");
    }
    const odin3_json_node *table = jr_member(params, "TABLE");
    odin3_value *vals = rd->cur.params.data;
    if (st == ODIN3_OK && table == NULL) {
        return jr_error(rd, params, where, "a $sop has no TABLE");
    }
    if (st == ODIN3_OK) {
        st = table_cover(rd, table, (uint64_t)width, (uint64_t)depth, &vals[SOP_PARAM_COVER]);
    }
    if (st == ODIN3_OK) {
        vals[SOP_PARAM_WIDTH] = odin3_value_int(width);
    }
    return st;
}

bool jr_lut0_params(jr *rd, const odin3_celltype_def *lut) {
    odin3_value *vals = rd->cur.params.data;
    int width_at = -1;
    int table_at = -1;
    for (uint32_t i = 0; i < lut->n_params; i++) {
        width_at = strcmp(lut->params[i].name, "WIDTH") == 0 ? (int)i : width_at;
        table_at = strcmp(lut->params[i].name, "LUT") == 0 ? (int)i : table_at;
    }
    if (width_at < 0 || table_at < 0 || vals[width_at].kind != ODIN3_VAL_INT ||
        vals[width_at].i != 0 || vals[table_at].kind != ODIN3_VAL_BITS || vals[table_at].len != 1) {
        return false;
    }
    bool one = vals[table_at].bits[0] == ODIN3_BIT_1;
    uint8_t *row = one ? jr_alloc(rd, 1) : NULL;
    if (one && row == NULL) {
        return false;
    }
    if (one) {
        row[0] = '1';
    }
    vals[SOP_PARAM_WIDTH] = odin3_value_int(0);
    vals[SOP_PARAM_COVER] = (odin3_value){
        .kind = ODIN3_VAL_COVER, .bits = row, .len = one ? 1U : 0U, .cover_inputs = 0};
    return true;
}
