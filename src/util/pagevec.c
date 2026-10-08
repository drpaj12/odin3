/* pagevec.c — fixed-size pages plus a growable page table; elements never move. */
#include "util/pagevec.h"

#include "util/alloc.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { PAGEVEC_MIN_TABLE = 8 };

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
    unsigned shift; /* log2 of elements per page */
    size_t mask;    /* elements per page - 1 */
};

odin3_pagevec *odin3_pagevec_create_paged(odin3_pagevec_spec spec) {
    assert(spec.elem_size > 0);
    if (spec.page_shift < ODIN3_PAGEVEC_MIN_SHIFT || spec.page_shift > ODIN3_PAGEVEC_MAX_SHIFT) {
        return NULL;
    }
    size_t page_elems = (size_t)1 << spec.page_shift;
    if (spec.elem_size > SIZE_MAX / page_elems) {
        return NULL;
    }
    odin3_pagevec *pv = odin3_util_calloc(sizeof *pv);
    if (pv == NULL) {
        return NULL;
    }
    pv->elem_size = spec.elem_size;
    pv->page_bytes = spec.elem_size * page_elems;
    pv->shift = spec.page_shift;
    pv->mask = page_elems - 1;
    return pv;
}

odin3_pagevec *odin3_pagevec_create(size_t elem_size) {
    odin3_pagevec_spec spec = {elem_size, ODIN3_PAGEVEC_PAGE_SHIFT};
    return odin3_pagevec_create_paged(spec);
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

/* Makes sure the page table has at least `want` slots; false on OOM or overflow. */
static bool table_reserve(odin3_pagevec *pv, size_t want) {
    if (want <= pv->table_cap) {
        return true;
    }
    size_t new_cap = pv->table_cap > 0 ? pv->table_cap : PAGEVEC_MIN_TABLE;
    while (new_cap < want) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }
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

/* Appends one zeroed page; false on OOM or overflow (state unchanged). */
static bool add_page(odin3_pagevec *pv) {
    if (!table_reserve(pv, pv->npages + 1)) {
        return false;
    }
    char *page = odin3_util_calloc(pv->page_bytes);
    if (page == NULL) {
        return false;
    }
    pv->pages[pv->npages++].base = page;
    return true;
}

odin3_status odin3_pagevec_reserve(odin3_pagevec *pv, size_t extra) {
    if (extra > SIZE_MAX - pv->len) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t need = ((pv->len + extra) >> pv->shift) + (((pv->len + extra) & pv->mask) != 0);
    if (need > pv->npages && !table_reserve(pv, need)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (pv->npages < need) {
        if (!add_page(pv)) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

void odin3_pagevec_truncate(odin3_pagevec *pv, size_t new_len) {
    assert(new_len <= pv->len);
    for (size_t i = new_len; i < pv->len; i++) {
        memset(odin3_pagevec_at(pv, i), 0, pv->elem_size);
    }
    pv->len = new_len;
}

void *odin3_pagevec_push(odin3_pagevec *pv, size_t *index) {
    if (pv->len >> pv->shift == pv->npages && !add_page(pv)) { /* current pages are full */
        return NULL;
    }
    size_t at = pv->len++;
    if (index != NULL) {
        *index = at;
    }
    return odin3_pagevec_at(pv, at);
}

/* Address of element idx: shift, mask, two loads. */
static char *elem_ptr(pagevec_page *pages, const odin3_pagevec *pv, size_t idx) {
    return pages[idx >> pv->shift].base + (idx & pv->mask) * pv->elem_size;
}

void *odin3_pagevec_at(odin3_pagevec *pv, size_t idx) {
    assert(idx < pv->len);
    return elem_ptr(pv->pages, pv, idx);
}

const void *odin3_pagevec_cat(const odin3_pagevec *pv, size_t idx) {
    assert(idx < pv->len);
    return elem_ptr(pv->pages, pv, idx);
}

size_t odin3_pagevec_len(const odin3_pagevec *pv) {
    return pv->len;
}

size_t odin3_pagevec_bytes_reserved(const odin3_pagevec *pv) {
    return pv->npages * pv->page_bytes;
}
