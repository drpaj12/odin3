/*
 * name_resolve.c — see name_resolve.h.
 */
#include "frontends/name_resolve.h"

#include "util/file.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>

enum {
    HASH_MIN_CHARS = 10,                  /* H-3: the shortest hash name is `h` and 10 characters */
    HASH_MAX_CHARS = 52,                  /* and the longest the 52 characters of 256 bits */
    FIRST_CUT = ODIN3_NAME_MISS_FIRST - 1 /* bytes of the first miss kept, room for the NUL */
};

static uint64_t max_bytes = ODIN3_NAME_RESOLVE_MAX_BYTES;
static uint32_t max_entries = ODIN3_NAME_RESOLVE_MAX_ENTRIES;

void odin3_name_resolver_set_limits(odin3_name_resolver_limits limits) {
    max_bytes = limits.max_bytes != 0 ? limits.max_bytes : ODIN3_NAME_RESOLVE_MAX_BYTES;
    max_entries = limits.max_entries != 0 ? limits.max_entries : ODIN3_NAME_RESOLVE_MAX_ENTRIES;
}

odin3_status odin3_name_resolver_open(odin3_name_resolver *res, const char *who, const char *path) {
    *res = (odin3_name_resolver){0};
    odin3_file_bytes file = {0};
    odin3_status st = odin3_file_read_all(path, (size_t)max_bytes, &file);
    if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %s: cannot read the name map: %s", who, path,
                  strerror(file.err));
    } else if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %s: the name map is over %llu bytes", who, path,
                  (unsigned long long)max_bytes);
    } else if (st == ODIN3_ERR_NO_MEMORY) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %s: out of memory reading the name map", who, path);
    }
    st = st == ODIN3_OK ? odin3_name_map_init(&res->map) : st;
    st = st == ODIN3_OK ? odin3_name_map_parse(&res->map, (odin3_bytes){file.data, file.len}, path)
                        : st;
    odin3_file_bytes_free(&file);
    if (st == ODIN3_OK && res->map.entries.len > max_entries) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %s: the name map has more than %u entries", who, path,
                  (unsigned)max_entries);
        st = ODIN3_ERR_PARSE;
    }
    if (st != ODIN3_OK) {
        odin3_name_map_free(&res->map);
    }
    return st;
}

void odin3_name_resolver_close(odin3_name_resolver *res) {
    odin3_restore_free(res->restore);
    res->restore = NULL;
    odin3_name_map_free(&res->map);
}

bool odin3_name_hash_shaped(odin3_bytes text) {
    const unsigned char *chars = text.ptr;
    if (chars == NULL || text.len < 1 + HASH_MIN_CHARS || text.len > 1 + HASH_MAX_CHARS ||
        chars[0] != 'h') {
        return false;
    }
    for (size_t i = 1; i < text.len; i++) {
        bool ok = (chars[i] >= 'a' && chars[i] <= 'z') || (chars[i] >= '2' && chars[i] <= '7');
        if (!ok) {
            return false;
        }
    }
    return true;
}

const odin3_name_entry *odin3_name_resolver_entry(const odin3_name_resolver *res,
                                                  odin3_bytes module, odin3_name_kind kind,
                                                  odin3_bytes name) {
    if (!odin3_name_hash_shaped(name)) {
        return NULL;
    }
    return odin3_name_map_find(&res->map, module, kind, name);
}

const odin3_name_entry *odin3_name_resolver_entry_decl(const odin3_name_resolver *res,
                                                       odin3_bytes module, odin3_bytes name) {
    const odin3_name_entry *entry = odin3_name_resolver_entry(res, module, ODIN3_NAME_WIRE, name);
    return entry != NULL ? entry : odin3_name_resolver_entry(res, module, ODIN3_NAME_NET, name);
}

bool odin3_name_resolver_find(const odin3_name_resolver *res, odin3_bytes module,
                              odin3_name_kind kind, odin3_bytes name, odin3_bytes *full) {
    const odin3_name_entry *entry = odin3_name_resolver_entry(res, module, kind, name);
    if (entry == NULL) {
        return false;
    }
    *full = entry->full;
    return true;
}

bool odin3_name_resolver_find_decl(const odin3_name_resolver *res, odin3_bytes module,
                                   odin3_bytes name, odin3_bytes *full) {
    const odin3_name_entry *entry = odin3_name_resolver_entry_decl(res, module, name);
    if (entry == NULL) {
        return false;
    }
    *full = entry->full;
    return true;
}

bool odin3_name_resolver_v2(const odin3_name_resolver *res) {
    return res != NULL && res->map.version == ODIN3_NAME_MAP_VERSION_2;
}

odin3_status odin3_name_resolver_restore(odin3_name_resolver *res, odin3_design *design,
                                         const char *path) {
    if (!odin3_name_resolver_v2(res) || res->restore != NULL) {
        return ODIN3_OK;
    }
    return odin3_design_restore(design, &res->map, path, &res->restore);
}

uint32_t odin3_name_resolver_record(const odin3_name_resolver *res, const odin3_name_entry *entry) {
    return res != NULL ? odin3_restore_entry_record(res->restore, entry) : 0U;
}

uint32_t odin3_name_resolver_named(const odin3_name_resolver *res, odin3_bytes module,
                                   odin3_name_kind kind, odin3_bytes name) {
    return res != NULL ? odin3_restore_named_record(res->restore, module, kind, name) : 0U;
}

void odin3_name_miss_note(odin3_name_miss *miss, odin3_bytes name, uint32_t line) {
    if (miss->count++ == 0) {
        size_t cut = name.len < FIRST_CUT ? name.len : (size_t)FIRST_CUT;
        memcpy(miss->first, name.ptr, cut);
        miss->first[cut] = '\0';
        miss->line = line;
    }
}

void odin3_name_miss_report(const odin3_name_miss *miss, const char *who, const char *path) {
    if (miss->count == 0) {
        return;
    }
    if (miss->line != 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s: %s:%u: %u use(s) of hash-shaped names are not in the name map and stay as "
                  "written; first: '%s'",
                  who, path, (unsigned)miss->line, (unsigned)miss->count, miss->first);
    } else {
        odin3_log(
            ODIN3_LOG_WARN,
            "%s: %s: %u use(s) of hash-shaped names are not in the name map and stay as written; "
            "first: '%s'",
            who, path, (unsigned)miss->count, miss->first);
    }
}
