/*
 * pagevec.h — paged array: elements never move, lookup by index is O(1).
 */
#ifndef ODIN3_UTIL_PAGEVEC_H
#define ODIN3_UTIL_PAGEVEC_H

#include <stddef.h>

enum { ODIN3_PAGEVEC_PAGE_SHIFT = 12, ODIN3_PAGEVEC_PAGE_ELEMS = 1 << ODIN3_PAGEVEC_PAGE_SHIFT };

typedef struct odin3_pagevec odin3_pagevec;

/* Pages of ODIN3_PAGEVEC_PAGE_ELEMS elements of elem_size (> 0) bytes; NULL on OOM/overflow. */
odin3_pagevec *odin3_pagevec_create(size_t elem_size);

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
