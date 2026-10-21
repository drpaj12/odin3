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

/* Frees storage and empties the vec (elem_size retained); safe to free again. */
void odin3_vec_free(odin3_vec *vec);

/* Ensures room for cap elements (capacity stays a power of two, minimum 8). Growth invalidates
 * pointers into data. */
odin3_status odin3_vec_reserve(odin3_vec *vec, size_t cap);

/* Appends a zeroed element and returns it; NULL on out of memory (vec unchanged). Growth
 * invalidates pointers into data. */
void *odin3_vec_push(odin3_vec *vec);

/* Element idx; requires idx < len. */
void *odin3_vec_at(odin3_vec *vec, size_t idx);
const void *odin3_vec_cat(const odin3_vec *vec, size_t idx);

/* Drops the last element; requires len > 0. */
void odin3_vec_pop(odin3_vec *vec);

/*
 * Makes dst (initialised, same elem_size, empty) a byte-for-byte copy of src's elements.
 * ODIN3_ERR_NO_MEMORY on out of memory (dst unchanged). Pointers inside the elements are copied
 * as they are: the caller re-points them.
 */
odin3_status odin3_vec_copy(odin3_vec *dst, const odin3_vec *src);

/* len = 0, keeps capacity. */
void odin3_vec_clear(odin3_vec *vec);

/*
 * Shrinks the allocation to len elements (for a vec that stops growing, such as a finished AST
 * table); cap becomes len, no longer a power of two, and a later reserve still grows by doubling.
 * An empty vec and a failed realloc keep the vec as it was.
 */
void odin3_vec_shrink_to_fit(odin3_vec *vec);

#endif
