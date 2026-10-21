/*
 * hash_rows.h — the objects a writer renamed under `--name-style hash`, their hash names and the
 * name map file (spec docs/specs/2026-10-20-5.4-hdl-locations-design.md section 13: H-1, H-2,
 * H-3, H-4).
 *
 * A writer adds one row per object it renamed (the full provenance name of H-2, the module and
 * kind), resolves each module's rows to hash names (hash_name.h), looks the names up by the key it
 * chose for the object, and at the end writes the map text through name_map.h. The helpers know
 * nothing of any format; the three writers share them.
 */
#ifndef ODIN3_BACKENDS_COMMON_HASH_ROWS_H
#define ODIN3_BACKENDS_COMMON_HASH_ROWS_H

#include "backends/common/hash_name.h"
#include "backends/common/name_map.h"
#include "odin3/odin3.h"
#include "util/file.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One renamed object; the texts live in the collection (use the accessors). */
typedef struct odin3_hash_row {
    uint32_t module; /* the writer's module ID: rows are resolved one module at a time */
    odin3_name_kind kind;
    uint64_t key; /* the writer's own key for the object, unique among the rows */
    size_t module_name_off, module_name_len;
    size_t full_off, full_len;
    odin3_hash_name hash; /* set by odin3_hash_rows_resolve */
} odin3_hash_row;

/* The collection. Fields are private to hash_rows.c. */
typedef struct odin3_hash_rows {
    odin3_vec rows; /* odin3_hash_row */
    odin3_strbuf text;
    uint32_t start; /* the starting width of the hash names; 0 = ODIN3_HASH_NAME_WIDTH */
} odin3_hash_rows;

/* What odin3_hash_rows_add records. */
typedef struct odin3_hash_row_req {
    uint32_t module;
    odin3_bytes module_name;
    odin3_name_kind kind;
    uint64_t key;
    odin3_bytes full; /* H-2: the provenance name, with the writer's uniqueness suffix */
} odin3_hash_row_req;

void odin3_hash_rows_init(odin3_hash_rows *rows);
void odin3_hash_rows_free(odin3_hash_rows *rows);

/*
 * Test hook (T-hash-3): the starting width of the hash names of every collection initialised
 * after this call (1 to 52; 0 = the default, 10). A narrow start forces collisions through a
 * writer. The map header records it. Not for production use: a process-wide setting, so a test
 * sets it and resets it to 0.
 */
void odin3_hash_rows_test_start(uint32_t start);
void odin3_hash_rows_set_start(odin3_hash_rows *rows, uint32_t start);

/* Appends a row (the texts are copied). ODIN3_ERR_NO_MEMORY; the collection is unchanged then. */
odin3_status odin3_hash_rows_add(odin3_hash_rows *rows, const odin3_hash_row_req *req);

size_t odin3_hash_rows_count(const odin3_hash_rows *rows);
const odin3_hash_row *odin3_hash_rows_at(const odin3_hash_rows *rows, size_t index);

/* The full name of a row; valid until the next add. */
odin3_bytes odin3_hash_rows_full(const odin3_hash_rows *rows, const odin3_hash_row *row);

/* Which rows odin3_hash_rows_resolve names, and the names that stay as they are. */
typedef struct odin3_hash_resolve_req {
    size_t first; /* rows[first..count) are one module's */
    const odin3_bytes *reserved;
    uint32_t nreserved;
    bool merge_kinds; /* one namespace for every kind (Verilog), else collisions are per kind */
} odin3_hash_resolve_req;

/*
 * Gives rows[first..count) their hash names (H-3), resolved per kind or, with merge_kinds, over
 * all kinds at once (a format with one namespace per module needs no two kinds to share a name).
 * The result depends only on the set of full names. The status of odin3_hash_names_resolve.
 */
odin3_status odin3_hash_rows_resolve(odin3_hash_rows *rows, const odin3_hash_resolve_req *req);

/*
 * An index key -> row number over all rows, for the writer's second look-up. *index is created
 * here; the caller destroys it. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_hash_rows_index(const odin3_hash_rows *rows, odin3_u64map **index);

/* Appends the whole map text (header and sorted lines, H-4) of all rows to out. */
odin3_status odin3_hash_rows_map_text(const odin3_hash_rows *rows, odin3_strbuf *out);

/* --- the map file ------------------------------------------------------------------------------
 */

/* The name map file of a write: a private temporary until odin3_hash_map_close. */
typedef struct odin3_hash_map_file {
    odin3_atomic_file file;
    odin3_strbuf path; /* the destination; the file borrows it */
    bool open;
} odin3_hash_map_file;

/* What odin3_hash_map_path resolves. */
typedef struct odin3_hash_map_req {
    const char *who;      /* the writer's name for its messages */
    const char *out_path; /* the netlist's path */
    const char *name_map; /* --name-map, or NULL */
} odin3_hash_map_req;

/*
 * Resolves the path of the name map: name_map if it is non-NULL, else `<out_path>.names`, into
 * *path (cleared first). ODIN3_ERR_INVALID_ARG, logged `<who>: --name-style hash needs --name-map
 * when the output is stdout`, for out_path "-" with no name_map.
 */
odin3_status odin3_hash_map_path(const odin3_hash_map_req *req, odin3_strbuf *path);

/*
 * Opens the map file beside the netlist before anything is written, so a map that cannot be
 * written refuses the write (H-1). The path is copied. The file is a private temporary until
 * commit. ODIN3_ERR_IO (logged by util/file.h). A map that was never opened (open false) is
 * closed by odin3_hash_map_close as a no-op.
 */
odin3_status odin3_hash_map_open(odin3_hash_map_file *map, const char *path);

/*
 * Commits the netlist and the map together. netlist is the netlist's atomic file (written, not
 * yet closed; NULL when the caller has none to hold) and status is the write's. Both temporaries
 * are finished first; then the map is renamed into place, then the netlist, so a failed map never
 * leaves a new netlist beside a stale map. Any failure removes both temporaries. Returns the final
 * status.
 */
odin3_status odin3_hash_map_close(odin3_hash_map_file *map, odin3_bytes text,
                                  odin3_atomic_file *netlist, odin3_status status);

#endif
