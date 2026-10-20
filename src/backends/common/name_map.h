/*
 * name_map.h — the name map of hashed names (spec docs/specs/2026-10-20-5.4-hdl-locations-design.md
 * section 13: H-4, H-8).
 *
 * A sidecar text file, UTF-8, LF lines:
 *
 *   # odin3-names 1 hash=sha256 width=10
 *   module<TAB>kind<TAB>short<TAB>full<TAB>hier<TAB>file<TAB>line
 *
 * one line per renamed object, sorted by module, kind, short (byte order). In each field TAB, LF,
 * `\` and `%` are written %09, %0A, %5C, %25. This module writes, parses and checks that text; it
 * does no file I/O and touches no IR.
 */
#ifndef ODIN3_BACKENDS_COMMON_NAME_MAP_H
#define ODIN3_BACKENDS_COMMON_NAME_MAP_H

#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    ODIN3_NAME_MAP_VERSION = 1,
    ODIN3_NAME_MAP_FIELDS = 7,
    ODIN3_NAME_MAP_MAX_LINE = 1 << 20,   /* longest accepted line, bytes (longer: ignored) */
    ODIN3_NAME_MAP_MAX_ENTRIES = 1 << 24 /* most entries a parse keeps (over: an error) */
};

typedef enum odin3_name_kind {
    ODIN3_NAME_NET,
    ODIN3_NAME_NODE,
    ODIN3_NAME_WIRE,
    ODIN3_NAME_KIND_COUNT
} odin3_name_kind;

/* "net", "node" or "wire"; "" for an out-of-range kind. */
const char *odin3_name_kind_str(odin3_name_kind kind);

/* True and *kind when text is exactly one of the three kind words. */
bool odin3_name_kind_parse(odin3_bytes text, odin3_name_kind *kind);

/* One map line; the views point into the caller's text (write) or the map's arena (parse). */
typedef struct odin3_name_entry {
    odin3_bytes module;
    odin3_name_kind kind;
    odin3_bytes short_name; /* the hash name */
    odin3_bytes full;       /* the H-2 text the hash name is the hash of */
    odin3_bytes hier, file;
    uint32_t line;
} odin3_name_entry;

/* A parsed map: entries sorted by (module, kind, short); every view lives until map_free. */
typedef struct odin3_name_map {
    odin3_vec entries; /* odin3_name_entry */
    struct odin3_arena *arena;
    uint32_t width;   /* the header's width (H-4) */
    uint32_t ignored; /* entry lines the parse warned about and dropped */
} odin3_name_map;

/* ODIN3_ERR_NO_MEMORY when the arena cannot be created. */
odin3_status odin3_name_map_init(odin3_name_map *map);

/* Frees the entries and the arena; safe on a map that failed to init or was freed. */
void odin3_name_map_free(odin3_name_map *map);

/*
 * Appends the map text for entries[0..n) to out: the header with `width` (1 to 52), then one line
 * per entry sorted by module, kind, short in byte order, so equal sets give equal bytes whatever
 * the input order. ODIN3_ERR_INVALID_ARG (logged) for a width out of range, a kind out of range or
 * two entries with one (module, kind, short): a map must name each object once. NO_MEMORY on out of
 * memory. On any failure out is as it was.
 */
odin3_status odin3_name_map_write(const odin3_name_entry *entries, uint32_t n, uint32_t width,
                                  odin3_strbuf *out);

/*
 * Parses text into map (which must be freshly initialised). path names the file in messages
 * ("path:LINE: ..."). The first line must be the header: an unknown version, hash function, or a
 * width outside 1 to 52 is a located ERROR and ODIN3_ERR_PARSE (a text that is not a map at all
 * too). After it, empty lines and lines starting `#` are skipped. An entry line that is malformed
 * (not 7 fields, a bad escape, a kind word or line number that does not parse, longer than
 * ODIN3_NAME_MAP_MAX_LINE) or whose short is not the hash of its full at the header's width or
 * longer (H-3) is a located WARNING and is ignored (map->ignored counts them). More than
 * ODIN3_NAME_MAP_MAX_ENTRIES entries is ODIN3_ERR_PARSE. NO_MEMORY on out of memory. On success
 * the entries are sorted by (module, kind, short).
 */
odin3_status odin3_name_map_parse(odin3_name_map *map, odin3_bytes text, const char *path);

/* The entry with exactly this module, kind and short name (first of equals), or NULL. */
const odin3_name_entry *odin3_name_map_find(const odin3_name_map *map, odin3_bytes module,
                                            odin3_name_kind kind, odin3_bytes short_name);

/* The entry with this module, kind and full name (linear scan), or NULL. */
const odin3_name_entry *odin3_name_map_find_full(const odin3_name_map *map, odin3_bytes module,
                                                 odin3_name_kind kind, odin3_bytes full);

/* What `names --check` reports (H-5). */
typedef struct odin3_name_check {
    uint32_t entries;    /* entries in the map */
    uint32_t bad_hash;   /* entries whose short is not the hash of full at the width or longer */
    uint32_t duplicates; /* entries repeating an earlier (module, kind, short) */
} odin3_name_check;

/* Verifies every entry of a map (H-4). Never fails; a parsed map has bad_hash 0. */
odin3_name_check odin3_name_map_check(const odin3_name_map *map);

#endif
