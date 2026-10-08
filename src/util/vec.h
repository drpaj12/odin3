/*
 * vec.h — growable contiguous array of fixed-size elements.
 */
#ifndef ODIN3_UTIL_VEC_H
#define ODIN3_UTIL_VEC_H

#include "odin3/odin3.h"

#include <stddef.h>

/* Outside vec.c, code may read data and len but never writes a field. */
typedef struct odin3_vec {
    void *data;
    size_t len, cap, elem_size;
} odin3_vec;

/* Empty vec of elements of elem_size bytes (> 0); allocates nothing. */
void odin3_vec_init(odin3_vec *vec, size_t elem_size);

/* Frees storage and leaves a zeroed vec that is safe to free again. */
void odin3_vec_free(odin3_vec *vec);

/* Ensures room for cap elements (capacity stays a power of two, minimum 8). */
odin3_status odin3_vec_reserve(odin3_vec *vec, size_t cap);

/* Appends a zeroed element and returns it; NULL on out of memory (vec unchanged). */
void *odin3_vec_push(odin3_vec *vec);

/* Element idx; requires idx < len. */
void *odin3_vec_at(odin3_vec *vec, size_t idx);
const void *odin3_vec_cat(const odin3_vec *vec, size_t idx);

/* Drops the last element; requires len > 0. */
void odin3_vec_pop(odin3_vec *vec);

/* len = 0, keeps capacity. */
void odin3_vec_clear(odin3_vec *vec);

#endif
