/* vec.c — growable array; capacity doubles from a minimum of 8 elements. */
#include "util/vec.h"

#include "util/alloc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { VEC_MIN_CAP = 8 };

void odin3_vec_init(odin3_vec *vec, size_t elem_size) {
    assert(elem_size > 0);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
    vec->elem_size = elem_size;
}

void odin3_vec_free(odin3_vec *vec) {
    odin3_util_free(vec->data);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

odin3_status odin3_vec_reserve(odin3_vec *vec, size_t cap) {
    if (cap <= vec->cap) {
        return ODIN3_OK;
    }
    size_t new_cap = vec->cap > 0 ? vec->cap : VEC_MIN_CAP;
    while (new_cap < cap) {
        if (new_cap > SIZE_MAX / 2) {
            return ODIN3_ERR_NO_MEMORY;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / vec->elem_size) {
        return ODIN3_ERR_NO_MEMORY;
    }
    void *grown = odin3_util_realloc(vec->data, new_cap * vec->elem_size);
    if (grown == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    vec->data = grown;
    vec->cap = new_cap;
    return ODIN3_OK;
}

void *odin3_vec_push(odin3_vec *vec) {
    if (vec->len == vec->cap && odin3_vec_reserve(vec, vec->len + 1) != ODIN3_OK) {
        return NULL;
    }
    void *slot = (char *)vec->data + vec->len * vec->elem_size;
    memset(slot, 0, vec->elem_size);
    vec->len++;
    return slot;
}

void *odin3_vec_at(odin3_vec *vec, size_t idx) {
    assert(idx < vec->len);
    return (char *)vec->data + idx * vec->elem_size;
}

const void *odin3_vec_cat(const odin3_vec *vec, size_t idx) {
    assert(idx < vec->len);
    return (const char *)vec->data + idx * vec->elem_size;
}

void odin3_vec_pop(odin3_vec *vec) {
    assert(vec->len > 0);
    vec->len--;
}

void odin3_vec_clear(odin3_vec *vec) {
    vec->len = 0;
}
