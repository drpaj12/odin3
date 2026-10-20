/*
 * reader_sop.c — Yosys `$sop` (DEPTH, TABLE, WIDTH) as the cover cell, and a `$lut` of no inputs
 * as the constant cover it was (M-8, R-2 d).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

enum { TABLE_BITS_PER_LITERAL = 2 };

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
    odin3_value val = {0};
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

int jr_find_param(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The indices of WIDTH and COVER in the registry's `$sop`, found by name. */
typedef struct sop_slots {
    int width;
    int cover;
} sop_slots;

static odin3_status sop_slots_of(const jr *rd, const odin3_celltype_def *sop, sop_slots *slots) {
    slots->width = jr_find_param(sop, "WIDTH");
    slots->cover = jr_find_param(sop, "COVER");
    if (slots->width < 0 || slots->cover < 0) {
        return jr_error(rd, NULL, NULL, "internal error: $sop has no WIDTH or COVER parameter");
    }
    return ODIN3_OK;
}

odin3_status jr_sop_params(jr *rd, const odin3_json_node *params, const jr_where *where) {
    odin3_celltype_id sop_id = {0};
    sop_slots slots = {0, 0};
    odin3_status st = ODIN3_OK;
    if (!jr_lookup_type(rd, "$sop", &sop_id)) {
        return jr_error(rd, params, where, "internal error: no $sop cell type");
    }
    st = sop_slots_of(rd, odin3_celltype_get(rd->design, sop_id), &slots);
    int64_t width = 0;
    int64_t depth = 0;
    if (st == ODIN3_OK) {
        st = int_member(rd, params, where, "WIDTH", &width);
    }
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
        st = table_cover(rd, table, (uint64_t)width, (uint64_t)depth, &vals[slots.cover]);
    }
    if (st == ODIN3_OK) {
        vals[slots.width] = odin3_value_int(width);
    }
    return st;
}

odin3_status jr_lut0_params(jr *rd, const jr_lut_sop *defs, bool *converted) {
    odin3_value *vals = rd->cur.params.data;
    const int width_at = jr_find_param(defs->lut, "WIDTH");
    const int table_at = jr_find_param(defs->lut, "LUT");
    sop_slots slots = {0, 0};
    *converted = false;
    odin3_status st = sop_slots_of(rd, defs->sop, &slots);
    if (st != ODIN3_OK || width_at < 0 || table_at < 0 || vals[width_at].kind != ODIN3_VAL_INT ||
        vals[width_at].i != 0 || vals[table_at].kind != ODIN3_VAL_BITS || vals[table_at].len != 1) {
        return st;
    }
    bool one = vals[table_at].bits[0] == ODIN3_BIT_1;
    uint8_t *row = one ? jr_alloc(rd, 1) : NULL;
    if (one && row == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    if (one) {
        row[0] = '1';
    }
    vals[slots.width] = odin3_value_int(0);
    vals[slots.cover] = (odin3_value){
        .kind = ODIN3_VAL_COVER, .bits = row, .len = one ? 1U : 0U, .cover_inputs = 0};
    *converted = true;
    return ODIN3_OK;
}
