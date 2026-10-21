/*
 * name_resolve.h — read-back of hashed names (spec
 * docs/specs/2026-10-20-5.4-hdl-locations-design.md section 13: H-6), shared by the BLIF, JSON and
 * Verilog readers.
 *
 * With `--name-map <file>` a reader replaces each hash name (`h` and 10 to 52 characters of a-z2-7,
 * H-3) that the map holds for the module it is in by the full name, before anything else, so the
 * rest of the read is the read of a `provenance` write. A hash-shaped name the map does not hold
 * stays and is counted (odin3_name_miss); the reader reports the count once per file. The map is
 * parsed by backends/common/name_map.h; there is no second parser.
 */
#ifndef ODIN3_FRONTENDS_NAME_RESOLVE_H
#define ODIN3_FRONTENDS_NAME_RESOLVE_H

#include "backends/common/name_map.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    ODIN3_NAME_MISS_FIRST = 64,               /* bytes of the first missed name, NUL incl. */
    ODIN3_NAME_RESOLVE_MAX_BYTES = 256 << 20, /* the largest map file a reader takes */
    ODIN3_NAME_RESOLVE_MAX_ENTRIES = 1 << 22  /* and the most entries it keeps */
};

typedef struct odin3_name_resolver {
    odin3_name_map map;
} odin3_name_resolver;

/*
 * Reads and parses the map file at path into res. who names the reader in messages ("read_blif").
 * Failure is logged (ERROR, with the path): ODIN3_ERR_IO for a file that cannot be read,
 * ODIN3_ERR_PARSE for a text that is not a map or an unknown version (name_map.h), NO_MEMORY.
 * On failure res holds nothing to free.
 */
odin3_status odin3_name_resolver_open(odin3_name_resolver *res, const char *who, const char *path);

/* Frees res; safe on a zeroed or failed one. */
void odin3_name_resolver_close(odin3_name_resolver *res);

/* True when text has the shape of a hash name: `h`, then 10 to 52 characters of a-z2-7. */
bool odin3_name_hash_shaped(odin3_bytes text);

/*
 * The full name for the hash name `name` of `kind` in `module`, or false when name is not
 * hash-shaped or the map has no such entry of that kind. *full views the map and lives until
 * odin3_name_resolver_close. Kinds are the writers' (name_map.h): BLIF nets are NET, JSON cells
 * NODE.
 */
bool odin3_name_resolver_find(const odin3_name_resolver *res, odin3_bytes module,
                              odin3_name_kind kind, odin3_bytes name, odin3_bytes *full);

/*
 * The same for a name declared as a wire: a JSON netname or a Verilog declarator, which the writers
 * name as a WIRE or, for an IR net they declare, as a NET (the wire entry first).
 */
bool odin3_name_resolver_find_decl(const odin3_name_resolver *res, odin3_bytes module,
                                   odin3_bytes name, odin3_bytes *full);

/*
 * The bounds on a map a reader takes: the file size and the entry count. open fails with a located
 * error over either. (0, 0) restores the defaults (ODIN3_NAME_RESOLVE_MAX_BYTES and
 * ODIN3_NAME_RESOLVE_MAX_ENTRIES); a nonzero value replaces one. For tests.
 */
typedef struct odin3_name_resolver_limits {
    uint64_t max_bytes;
    uint32_t max_entries;
} odin3_name_resolver_limits;
void odin3_name_resolver_set_limits(odin3_name_resolver_limits limits);

/* The hash-shaped names the map did not hold: how many and the first (with its line). */
typedef struct odin3_name_miss {
    uint32_t count;
    uint32_t line;                     /* of the first, 0 when the reader has no line */
    char first[ODIN3_NAME_MISS_FIRST]; /* the first name, cut */
} odin3_name_miss;

/* Counts a miss of `name` at `line`. */
void odin3_name_miss_note(odin3_name_miss *miss, odin3_bytes name, uint32_t line);

/* One WARNING for the file when miss counts anything: `<who>: <path>:<line>: N hash-shaped ...`. */
void odin3_name_miss_report(const odin3_name_miss *miss, const char *who, const char *path);

#endif
