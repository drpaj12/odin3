/*
 * str.c — string interning (strtab) and a growable string builder (strbuf).
 *
 * strtab copies each string (plus a NUL) into an arena, so pointers never move. A vec of
 * {ptr, len} indexed by ID gives O(1) get/len; an idindex keyed by odin3_hash_bytes finds
 * existing strings, comparing against the vec entry. ID 0 is the empty string.
 */
#include "util/str.h"

#include "util/alloc.h"
#include "util/arena.h"
#include "util/idindex.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { STRBUF_MIN_CAP = 32 };

typedef struct str_entry {
    const char *ptr;
    uint32_t len;
} str_entry;

struct odin3_strtab {
    odin3_arena *arena;
    odin3_vec entries;
    odin3_idindex *index;
};

static bool str_equals(const void *ctx, uint32_t id, odin3_bytes probe) {
    const odin3_strtab *tab = ctx;
    const str_entry *ent = odin3_vec_cat(&tab->entries, id);
    return ent->len == probe.len && (probe.len == 0 || memcmp(ent->ptr, probe.ptr, probe.len) == 0);
}

static odin3_idcmp str_cmp(const odin3_strtab *tab, odin3_bytes str) {
    const odin3_idcmp cmp = {odin3_hash_bytes(str, ODIN3_HASH_SEED), str, str_equals, tab};
    return cmp;
}

odin3_strtab *odin3_strtab_create(void) {
    odin3_strtab *tab = odin3_util_calloc(sizeof *tab);
    if (tab == NULL) {
        return NULL;
    }
    odin3_vec_init(&tab->entries, sizeof(str_entry));
    tab->arena = odin3_arena_create(0);
    tab->index = odin3_idindex_create(0);
    uint32_t empty = 0;
    if (tab->arena == NULL || tab->index == NULL ||
        odin3_strtab_intern(tab, (odin3_bytes){NULL, 0}, &empty) != ODIN3_OK) {
        odin3_strtab_destroy(tab);
        return NULL;
    }
    return tab;
}

void odin3_strtab_destroy(odin3_strtab *tab) {
    if (tab == NULL) {
        return;
    }
    odin3_idindex_destroy(tab->index);
    odin3_vec_free(&tab->entries);
    odin3_arena_destroy(tab->arena);
    odin3_util_free(tab);
}

bool odin3_strtab_find(const odin3_strtab *tab, odin3_bytes str, uint32_t *id) {
    if (str.len != 0 && memchr(str.ptr, '\0', str.len) != NULL) {
        return false;
    }
    const odin3_idcmp cmp = str_cmp(tab, str);
    return odin3_idindex_find(tab->index, &cmp, id);
}

odin3_status odin3_strtab_intern(odin3_strtab *tab, odin3_bytes str, uint32_t *id) {
    if (str.len != 0 && memchr(str.ptr, '\0', str.len) != NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_idcmp cmp = str_cmp(tab, str);
    if (odin3_idindex_find(tab->index, &cmp, id)) {
        return ODIN3_OK;
    }
    if (tab->entries.len >= UINT32_MAX || str.len > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    char *copy = odin3_arena_strndup(tab->arena, str.ptr, str.len);
    str_entry *ent = copy != NULL ? odin3_vec_push(&tab->entries) : NULL;
    if (ent == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ent->ptr = copy;
    ent->len = (uint32_t)str.len;
    const odin3_identry entry = {cmp.hash, (uint32_t)(tab->entries.len - 1)};
    if (odin3_idindex_insert(tab->index, entry) != ODIN3_OK) {
        odin3_vec_pop(&tab->entries);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (id != NULL) {
        *id = entry.id;
    }
    return ODIN3_OK;
}

const char *odin3_strtab_get(const odin3_strtab *tab, uint32_t id) {
    return id < tab->entries.len ? ((const str_entry *)odin3_vec_cat(&tab->entries, id))->ptr
                                 : NULL;
}

size_t odin3_strtab_len(const odin3_strtab *tab, uint32_t id) {
    return id < tab->entries.len ? ((const str_entry *)odin3_vec_cat(&tab->entries, id))->len : 0;
}

size_t odin3_strtab_count(const odin3_strtab *tab) {
    return tab->entries.len;
}

void odin3_strbuf_init(odin3_strbuf *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void odin3_strbuf_free(odin3_strbuf *buf) {
    odin3_util_free(buf->data);
    odin3_strbuf_init(buf);
}

/* Ensures room for `extra` more bytes plus the NUL; capacity doubles from 32. */
static odin3_status strbuf_reserve(odin3_strbuf *buf, size_t extra) {
    if (extra > SIZE_MAX - buf->len - 1) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return ODIN3_OK;
    }
    size_t cap = buf->cap != 0 ? buf->cap : STRBUF_MIN_CAP;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return ODIN3_ERR_NO_MEMORY;
        }
        cap *= 2;
    }
    char *data = odin3_util_realloc(buf->data, cap);
    if (data == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    buf->data = data;
    buf->cap = cap;
    return ODIN3_OK;
}

odin3_status odin3_strbuf_append(odin3_strbuf *buf, odin3_bytes str) {
    if (str.len == 0) {
        return ODIN3_OK;
    }
    const odin3_status st = strbuf_reserve(buf, str.len);
    if (st != ODIN3_OK) {
        return st;
    }
    memcpy(buf->data + buf->len, str.ptr, str.len);
    buf->len += str.len;
    buf->data[buf->len] = '\0';
    return ODIN3_OK;
}

odin3_status odin3_strbuf_appendf(odin3_strbuf *buf, const char *fmt, ...) {
    va_list args;
    va_list measure;
    va_start(args, fmt);
    va_copy(measure, args);
    const int need = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    odin3_status st = need < 0 ? ODIN3_ERR_INVALID_ARG : strbuf_reserve(buf, (size_t)need);
    if (st == ODIN3_OK) {
        const int wrote = vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, args);
        if (wrote < 0) {
            st = ODIN3_ERR_INVALID_ARG;
            buf->data[buf->len] = '\0';
        } else {
            buf->len += (size_t)wrote;
        }
    }
    va_end(args);
    return st;
}

void odin3_strbuf_clear(odin3_strbuf *buf) {
    buf->len = 0;
    if (buf->data != NULL) {
        buf->data[0] = '\0';
    }
}
