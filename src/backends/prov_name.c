/* prov_name.c — provenance names for writers; see prov_name.h. */
#include "backends/prov_name.h"

#include "util/str.h"

#include <stddef.h>

enum { FIRST_PRINTABLE = 0x20, LAST_PRINTABLE = 0x7e };

/* The first leaf of the backward walk (odin3_prov_sources' first call): the walk follows parents
 * in order, so it is the end of the chain of first parents. 0 when a record on the way is
 * unknown or has no parent; the caller then asks odin3_prov_sources. */
static odin3_prov_id first_leaf_of(const odin3_design *design, odin3_prov_id prov) {
    odin3_prov_id at = prov;
    for (;;) {
        const odin3_prov_record *rec = odin3_prov_get(design, at);
        if (rec == NULL) {
            return (odin3_prov_id){0};
        }
        if (rec->kind != ODIN3_PROV_DERIVED) {
            return at;
        }
        odin3_prov_list parents = odin3_prov_parents(design, at);
        if (parents.count == 0) {
            return (odin3_prov_id){0};
        }
        at = parents.ids[0];
    }
}

typedef struct first_leaf {
    odin3_prov_id first;
} first_leaf;

static void note_first(void *user, odin3_prov_id leaf) {
    first_leaf *found = user;
    if (!odin3_prov_valid(found->first)) {
        found->first = leaf;
    }
}

/* The record ID that names prov: prov, or for DERIVED its first leaf; 0 for none. */
static odin3_status find_leaf(const odin3_design *design, odin3_prov_id prov, uint64_t *leaf) {
    const odin3_prov_record *self = odin3_prov_get(design, prov);
    first_leaf found = {self != NULL ? prov : (odin3_prov_id){0}};
    if (self != NULL && self->kind == ODIN3_PROV_DERIVED) {
        found.first = first_leaf_of(design, prov);
        if (!odin3_prov_valid(found.first)) {
            odin3_status st = odin3_prov_sources(design, prov, note_first, &found);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    *leaf = found.first.v;
    return ODIN3_OK;
}

odin3_status odin3_prov_leaf(const odin3_design *design, odin3_prov_id prov, odin3_u64map *cache,
                             const odin3_prov_record **rec) {
    *rec = NULL;
    if (prov.v == 0) {
        return ODIN3_OK;
    }
    uint64_t leaf = 0;
    if (cache == NULL || !odin3_u64map_get(cache, prov.v, &leaf)) {
        odin3_status st = find_leaf(design, prov, &leaf);
        if (st != ODIN3_OK) {
            return st;
        }
        if (cache != NULL && odin3_u64map_put(cache, (odin3_kv){prov.v, leaf}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *rec = odin3_prov_get(design, (odin3_prov_id){(uint32_t)leaf});
    return ODIN3_OK;
}

static bool printable(const odin3_strtab *tab, uint32_t str) {
    const uint8_t *bytes = (const uint8_t *)odin3_strtab_get(tab, str);
    size_t len = odin3_strtab_len(tab, str);
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] < FIRST_PRINTABLE || bytes[i] > LAST_PRINTABLE) {
            return false;
        }
    }
    return true;
}

bool odin3_prov_location(const odin3_design *design, const odin3_prov_record *rec,
                         odin3_srcloc *loc) {
    if (rec == NULL || rec->n_locs == 0 || rec->locs[0].file == 0 ||
        !printable(odin3_design_strtab(design), rec->locs[0].file)) {
        return false;
    }
    *loc = rec->locs[0];
    return true;
}

odin3_status odin3_prov_name(const odin3_design *design, odin3_prov_id prov, odin3_bytes short_name,
                             odin3_u64map *cache, odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    const odin3_prov_record *rec = NULL;
    odin3_status st = odin3_prov_leaf(design, prov, cache, &rec);
    odin3_srcloc loc = {0};
    bool located = st == ODIN3_OK && odin3_prov_location(design, rec, &loc);
    if (st != ODIN3_OK || rec == NULL || (rec->hier == 0 && !located)) {
        return st;
    }
    const odin3_strtab *tab = odin3_design_strtab(design);
    if (rec->hier != 0) {
        st = odin3_strbuf_appendf(out, "%s/", odin3_strtab_get(tab, rec->hier));
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(out, short_name) : st;
    if (st == ODIN3_OK && located) {
        st = odin3_strbuf_appendf(out, "@%s:%u", odin3_strtab_get(tab, loc.file),
                                  (unsigned)loc.line);
    }
    return st;
}

/* True when the len bytes of tail, with `/` read as `.`, start name and a `.` follows.
 */
static bool suffix_starts(const char *tail, size_t len, const char *name) {
    for (size_t i = 0; i < len; i++) {
        char want = tail[i];
        if (want == '/') {
            want = '.';
        }
        if (want != name[i]) {
            return false;
        }
    }
    return name[len] == '.';
}

size_t odin3_prov_hier_overlap(odin3_bytes hier, odin3_bytes short_name) {
    for (size_t start = 0; start < hier.len; start++) {
        size_t len = hier.len - start;
        bool at_component = start == 0 || ((const char *)hier.ptr)[start - 1] == '/';
        if (at_component && short_name.len > len + 1 &&
            suffix_starts((const char *)hier.ptr + start, len, short_name.ptr)) {
            return len + 1;
        }
    }
    return 0;
}
