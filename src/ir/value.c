/* value.c — copy and compare typed IR parameter values. */
#include "ir/value.h"

#include <string.h>

odin3_value odin3_value_int(int64_t num) {
    odin3_value val = {0};
    val.kind = ODIN3_VAL_INT;
    val.i = num;
    return val;
}

static bool has_payload(const odin3_value *val) {
    return val->kind == ODIN3_VAL_BITS || val->kind == ODIN3_VAL_COVER;
}

odin3_status odin3_value_copy(odin3_arena *arena, const odin3_value *src, odin3_value *dst) {
    odin3_value out = *src;
    if (has_payload(src)) {
        out.bits = NULL;
        if (src->len > 0) {
            uint8_t *mem = odin3_arena_alloc(arena, src->len);
            if (mem == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            memcpy(mem, src->bits, src->len);
            out.bits = mem;
        }
    }
    *dst = out;
    return ODIN3_OK;
}

bool odin3_value_equal(const odin3_value *lhs, const odin3_value *rhs) {
    if (lhs->kind != rhs->kind) {
        return false;
    }
    switch (lhs->kind) {
    case ODIN3_VAL_INT:
        return lhs->i == rhs->i;
    case ODIN3_VAL_STRING:
        return lhs->str == rhs->str;
    case ODIN3_VAL_COVER:
        if (lhs->cover_inputs != rhs->cover_inputs) {
            return false;
        }
        break;
    case ODIN3_VAL_BITS:
        break;
    }
    return lhs->len == rhs->len && (lhs->len == 0 || memcmp(lhs->bits, rhs->bits, lhs->len) == 0);
}
