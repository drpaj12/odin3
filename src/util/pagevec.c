/* pagevec.c — fixed-size pages plus a growable page table; elements never move. */
#include "util/pagevec.h"

#include "util/alloc.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

enum { PAGEVEC_MIN_TABLE = 8, PAGEVEC_MASK = ODIN3_PAGEVEC_PAGE_ELEMS - 1 };

typedef struct pagevec_page {
    char *base;
} pagevec_page;

struct odin3_pagevec {
    pagevec_page *pages; /* page table */
    size_t npages;       /* pages allocated */
    size_t table_cap;    /* slots in the page table */
    size_t len;
    size_t elem_size;
    size_t page_bytes;
};

odin3_pagevec *odin3_pagevec_create(size_t elem_size) {
    assert(elem_size > 0);
    if (elem_size > SIZE_MAX / ODIN3_PAGEVEC_PAGE_ELEMS) {
        return NULL;
    }
    odin3_pagevec *pv = odin3_util_calloc(sizeof *pv);
    if (pv == NULL) {
        return NULL;
    }
    pv->elem_size = elem_size;
    pv->page_bytes = elem_size * ODIN3_PAGEVEC_PAGE_ELEMS;
    return pv;
}

void odin3_pagevec_destroy(odin3_pagevec *pv) {
    if (pv == NULL) {
        return;
    }
    for (size_t i = 0; i < pv->npages; i++) {
        odin3_util_free(pv->pages[i].base);
    }
    odin3_util_free(pv->pages);
    odin3_util_free(pv);
}

/* Makes sure the page table has a free slot; false on OOM or overflow. */
static bool table_has_room(odin3_pagevec *pv) {
    if (pv->npages < pv->table_cap) {
        return true;
    }
    size_t new_cap = pv->table_cap > 0 ? pv->table_cap * 2 : PAGEVEC_MIN_TABLE;
    if (new_cap > SIZE_MAX / sizeof *pv->pages) {
        return false;
    }
    pagevec_page *grown = odin3_util_realloc(pv->pages, new_cap * sizeof *pv->pages);
    if (grown == NULL) {
        return false;
    }
    pv->pages = grown;
    pv->table_cap = new_cap;
    return true;
}

void *odin3_pagevec_push(odin3_pagevec *pv, size_t *index) {
    if (pv->len >> ODIN3_PAGEVEC_PAGE_SHIFT == pv->npages) { /* current pages are full */
        if (!table_has_room(pv)) {
            return NULL;
        }
        char *page = odin3_util_calloc(pv->page_bytes);
        if (page == NULL) {
            return NULL;
        }
        pv->pages[pv->npages++].base = page;
    }
    size_t at = pv->len++;
    if (index != NULL) {
        *index = at;
    }
    return odin3_pagevec_at(pv, at);
}

void *odin3_pagevec_at(odin3_pagevec *pv, size_t idx) {
    assert(idx < pv->len);
    return pv->pages[idx >> ODIN3_PAGEVEC_PAGE_SHIFT].base + (idx & PAGEVEC_MASK) * pv->elem_size;
}

const void *odin3_pagevec_cat(const odin3_pagevec *pv, size_t idx) {
    assert(idx < pv->len);
    return pv->pages[idx >> ODIN3_PAGEVEC_PAGE_SHIFT].base + (idx & PAGEVEC_MASK) * pv->elem_size;
}

size_t odin3_pagevec_len(const odin3_pagevec *pv) {
    return pv->len;
}

size_t odin3_pagevec_bytes_reserved(const odin3_pagevec *pv) {
    return pv->npages * pv->page_bytes;
}
