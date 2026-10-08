/*
 * pagevec.h — paged array: elements never move, lookup by index is O(1).
 */
#ifndef ODIN3_UTIL_PAGEVEC_H
#define ODIN3_UTIL_PAGEVEC_H

#include "odin3/odin3.h"

#include <stddef.h>

enum { ODIN3_PAGEVEC_PAGE_SHIFT = 12, ODIN3_PAGEVEC_PAGE_ELEMS = 1 << ODIN3_PAGEVEC_PAGE_SHIFT };

enum { ODIN3_PAGEVEC_MIN_SHIFT = 4, ODIN3_PAGEVEC_MAX_SHIFT = 16 };

/* Layout of a pagevec: element size (> 0) and log2 of elements per page. */
typedef struct odin3_pagevec_spec {
    size_t elem_size;
    unsigned page_shift;
} odin3_pagevec_spec;

typedef struct odin3_pagevec odin3_pagevec;

/* Pages of ODIN3_PAGEVEC_PAGE_ELEMS elements of elem_size (> 0) bytes; NULL on OOM/overflow. */
odin3_pagevec *odin3_pagevec_create(size_t elem_size);

/* Pages of (1 << spec.page_shift) elements; NULL on OOM/overflow or shift outside [MIN, MAX]. */
odin3_pagevec *odin3_pagevec_create_paged(odin3_pagevec_spec spec);

/* Ensures the next `extra` pushes cannot fail (allocates pages/page table now); len unchanged.
 * ODIN3_ERR_NO_MEMORY on OOM (len unchanged; pages already allocated are kept). */
odin3_status odin3_pagevec_reserve(odin3_pagevec *pv, size_t extra);

/* Shrinks len to new_len (<= len). Elements past new_len are zeroed so a later push returns a
 * zeroed slot at the same address; pages are kept. */
void odin3_pagevec_truncate(odin3_pagevec *pv, size_t new_len);

/* Frees every page and the pagevec. NULL is a no-op. */
void odin3_pagevec_destroy(odin3_pagevec *pv);

/* Appends a zeroed element; *index (if non-NULL) gets its index. NULL on OOM (len unchanged). */
void *odin3_pagevec_push(odin3_pagevec *pv, size_t *index);

/* Element idx (address stable for the pagevec's lifetime); requires idx < len. */
void *odin3_pagevec_at(odin3_pagevec *pv, size_t idx);
const void *odin3_pagevec_cat(const odin3_pagevec *pv, size_t idx);

size_t odin3_pagevec_len(const odin3_pagevec *pv);

/* Bytes of element storage in allocated pages (excludes the page table). */
size_t odin3_pagevec_bytes_reserved(const odin3_pagevec *pv);

#endif
