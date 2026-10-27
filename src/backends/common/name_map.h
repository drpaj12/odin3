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
 * `\` and `%` are written %09, %0A, %5C, %25, and a first byte `#` of the module (which would make
 * the line a comment) is written %23. This module writes, parses and checks that text; it
 * does no file I/O and touches no IR.
 *
 * Version 2 (V1.2.0 3.2) begins `# odin3-names 2 hash=sha256 width=N prov=1 ast=<mode>` and every
 * later line starts with a one letter type and a TAB: O (an O line is the seven fields above, then
 * recs and level), N, R, C, F, T, A, Q, K. The escapes are the four above plus %2C and %7C (list fields
 * are `,` and `|` separated, each element escaped on its own). This module writes the lines it is
 * given and parses and checks them; the numbering and the join rules are prov_export.c's.
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
    ODIN3_NAME_MAP_VERSION_2 = 2,
    ODIN3_NAME_MAP_FIELDS = 7,
    ODIN3_NM2_MAX_FIELDS = 12,         /* fields kept of a version 2 line (the rest are ignored) */
    ODIN3_NAME_MAP_MAX_LINE = 1 << 20, /* longest accepted line, bytes (longer: ignored) */
    ODIN3_NAME_MAP_MAX_ENTRIES = 1 << 24 /* most entries a parse keeps (over: an error) */
};

/* Field positions (after the type letter) of the version 2 lines that name each other. */
enum {
    NM_R_KIND = 1,
    NM_R_LEVEL = 2,
    NM_R_DIR = 3,
    NM_R_STEP = 4,
    NM_R_PARENTS = 5,
    NM_R_ORIGINS = 6,
    NM_R_CONS = 7,
    NM_R_FLAGS = 8,
    NM_C_PARENT = 3,
    NM_C_FILE = 4,
    NM_O_SHORT = 2,
    NM_O_RECS = 7,
    NM_N_NAME = 2,
    NM_N_RECS = 3,
    NM_A_RECS = 1, /* A: atom recs stale */
    NM_A_STALE = 2,
    NM_Q_ID = 0, /* Q: id why objective gain pcrit rho slack near npaths members basis stale */
    NM_Q_STALE = 11,
    NM_K_ID = 0, /* K: id member pcrit rho slack basis stale */
    NM_K_STALE = 6
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

/* An index slot: a pointer to an entry (a struct so the index is an array of structs). */
typedef struct odin3_name_ref {
    const odin3_name_entry *entry;
} odin3_name_ref;

/*
 * A version 2 line as parsed, the fields still escaped (a list field is split at `,` or `|`
 * first, then each element unescaped with odin3_name_map_unescape). type is the letter; f holds
 * the fields after it. O lines are kept here too, beside their odin3_name_entry.
 */
typedef struct odin3_nm_line {
    char type;
    uint32_t lineno;
    uint32_t nfields;
    odin3_bytes f[ODIN3_NM2_MAX_FIELDS];
} odin3_nm_line;

/* A parsed map: entries sorted by (module, kind, short); every view lives until map_free. */
typedef struct odin3_name_map {
    odin3_vec entries; /* odin3_name_entry */
    struct odin3_arena *arena;
    uint32_t width;          /* the header's width (H-4) */
    uint32_t ignored;        /* entry lines the parse warned about and dropped */
    odin3_name_ref *by_full; /* sorted by (module, kind, full): find_full */
    uint32_t version;        /* 1 or 2 */
    odin3_bytes ast;         /* version 2: the header's ast= word (refs, constructs, full, none) */
    odin3_vec lines; /* version 2: odin3_nm_line, in file order, every type but the header */
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

/* --- version 2 -------------------------------------------------------------------------------- */

/* One field of a version 2 line: raw text is written as is (a list whose elements the caller
 * escaped with odin3_name_map_escape and joined with `,` or `|`), else it is escaped. */
typedef struct odin3_nm_field {
    odin3_bytes text;
    bool raw;
} odin3_nm_field;

/* Appends text with TAB, LF, `\`, `%`, `,` and `|` escaped (the version 2 set). NO_MEMORY. */
odin3_status odin3_name_map_escape(odin3_strbuf *out, odin3_bytes text);

/*
 * Appends the version 2 header (`# odin3-names 2 hash=sha256 width=<width> prov=1 ast=<ast>`).
 * INVALID_ARG for a width outside 1..52 or an ast word that is not refs, constructs, full or none.
 */
odin3_status odin3_name_map_write_header_v2(odin3_strbuf *out, uint32_t width, const char *ast);

/*
 * Appends one version 2 line: the type letter, a TAB, the fields separated by TABs, LF.
 * INVALID_ARG (logged) for a type letter that is not a letter, n of 0 or over
 * ODIN3_NM2_MAX_FIELDS, or a line over ODIN3_NAME_MAP_MAX_LINE bytes; NO_MEMORY. On failure out is
 * as it was.
 */
odin3_status odin3_name_map_write_line_v2(odin3_strbuf *out, char type,
                                          const odin3_nm_field *fields, size_t n);

/* The order of O and N objects: module, kind, short (byte order). Negative, zero, positive. */
int odin3_name_entry_cmp(const odin3_name_entry *lhs, const odin3_name_entry *rhs);

/* Unescapes a version 2 element (all six escapes) into the map's arena. INVALID_ARG for a bad
 * escape; NO_MEMORY. */
odin3_status odin3_name_map_unescape(odin3_name_map *map, odin3_bytes field, odin3_bytes *out);

/* True and *num when tok is `<prefix><n>` with n a decimal number above 0 (`r12`, `c3`). */
bool odin3_name_map_ref_number(odin3_bytes tok, char prefix, uint32_t *num);

/* What the version 2 check counts; a map that parsed and is well formed has every problem at 0. */
typedef struct odin3_name_check_v2 {
    uint32_t records, constructs, files, runs; /* R, C, F, T lines */
    uint32_t objects;                          /* O and N lines */
    uint32_t bad_id;                           /* an R, C, F or T id that is not its line number */
    uint32_t bad_ref;                          /* an r, c, f or t reference that does not resolve */
    uint32_t bad_order;                        /* a parent or construct parent not numbered first */
    uint32_t bad_class;                        /* an R line with no valid kind, dir or step */
    uint32_t no_record;                        /* an O or N line whose record is missing */
    uint32_t ignored; /* lines the parse dropped (warned about); counted, not a problem here */
} odin3_name_check_v2;

/* Verifies the version 2 invariants of a parsed map (V1.2.0 3.5). A version 1 map gives zeros. */
odin3_name_check_v2 odin3_name_map_check_v2(const odin3_name_map *map);

/* True when the problem counters of c are all zero (`ignored` is not a problem counter). */
bool odin3_name_check_v2_ok(const odin3_name_check_v2 *check);

/* Logs an error naming each non-zero problem counter (and a warning for ignored lines) with `who`
 * as the prefix; returns odin3_name_check_v2_ok(check). Silent for a map that is well formed. */
bool odin3_name_check_v2_report(const odin3_name_check_v2 *check, const char *who);

/* The entry with exactly this module, kind and short name (first of equals), or NULL. */
const odin3_name_entry *odin3_name_map_find(const odin3_name_map *map, odin3_bytes module,
                                            odin3_name_kind kind, odin3_bytes short_name);

/* The entry with this module, kind and full name (binary search over an index built by parse;
 * the first of equals in entry order), or NULL. */
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
