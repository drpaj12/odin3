/*
 * restore.h — restoring a version 2 name map (docs/V1.2.0.md 4.5): the design-held `extra` table
 * and the restore entry point the three netlist readers call.
 *
 * The IR addition is exactly this and the per-run `restored` flag of the pass-run row
 * (ir_internal.h): no field of any IR object, no ID, no attribute and no ABI function changes.
 *   - `extra`: the A, Q and K lines of the map (post-Odin data), held as text with their record ids
 *     rewritten to the design's, in file order, and one slot, the mutating run: the first pass run
 *     after the restore that is not a reader or a writer (0 = none). A design with no restore has
 *     no table and no slot.
 *   - `restored`: set on the pass-run rows a restore appends, so code that expects `ast` to be a
 *     node ID of an AST store (the elaborator, dump_locs) can tell a restored run's construct rows
 *     (whose `ast` is the number of the C line) from a live run's.
 */
#ifndef ODIN3_IR_RESTORE_H
#define ODIN3_IR_RESTORE_H

#include "backends/common/name_map.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The extra table: opaque; the design owns it (design.c copies and frees it; restore creates it).
 */
typedef struct odin3_extra_table odin3_extra_table;

/* A new empty table, NULL on out of memory. */
odin3_extra_table *odin3_extra_create(void);

/* Frees the table; NULL is a no-op. */
void odin3_extra_destroy(odin3_extra_table *table);

/* Gives dst (the design's, NULL) a copy of src's table; nothing when src has none. NO_MEMORY. */
odin3_status odin3_extra_clone(odin3_design *dst, const odin3_design *src);

/* Appends one line (type letter, TAB, the escaped fields joined by TAB; no LF). NO_MEMORY. */
odin3_status odin3_extra_add(odin3_extra_table *table, odin3_bytes line);

/* The lines in stored order: the count and line `index` (0-based; a view valid until the next
 * odin3_extra_add). */
uint32_t odin3_extra_count(const odin3_extra_table *table);
odin3_bytes odin3_extra_line(const odin3_extra_table *table, uint32_t index);

/* The mutating run, 0 for none (V1.2.0 4.5). */
uint32_t odin3_extra_mutating(const odin3_extra_table *table);

/* Bytes the table holds (0 for NULL). */
size_t odin3_extra_bytes_reserved(const odin3_extra_table *table);

/* The design's table, NULL when the design was not restored from a map. */
const odin3_extra_table *odin3_design_extra(const odin3_design *design);

/*
 * odin3_pass_run_begin calls this for every run it opens: when the design has an extra table,
 * its slot is 0 and the pass name does not begin with `read_` or `write_`, the slot becomes the
 * run. A restored run never sets it. Nothing happens for a design with no table.
 */
void odin3_extra_note_run(odin3_design *design, odin3_passrun_id run, bool restored);

/* True when the run is a pass-run row a restore appended. */
bool odin3_passrun_restored(const odin3_design *design, odin3_passrun_id run);

/* The strtab IDs of a restored run's pass name, arguments and information (0 = none). */
typedef struct odin3_passrun_text {
    uint32_t name, args, info;
} odin3_passrun_text;

/*
 * Appends a pass-run row flagged restored (odin3_pass_run_begin_args for a restore: same errors,
 * and the row never becomes the mutating run) and fills *ctx with op 0. Used by the restore only.
 */
odin3_status odin3_passrun_append_restored(odin3_design *design, const odin3_passrun_text *text,
                                           odin3_pass_ctx *ctx);

/* What a restore leaves for the reader: the record each object of the map is bound to. */
typedef struct odin3_restore odin3_restore;

/*
 * Restores the version 2 map `map` (parsed by odin3_name_map_parse; path names the file in
 * messages) into the design, after the reader's own pass run:
 *   - validates the whole map first (odin3_name_map_check_v2 and the rules of the restore) and
 *     changes nothing when it fails (ODIN3_ERR_PARSE, every problem logged with path:line);
 *   - appends a pass-run row per T line (flagged restored), interns F lines, creates the records of
 *     the R lines in file order (SOURCE and IMPORTED with their origins, DERIVED with parents and
 *     class: the operation's default when it has none yet, an override otherwise), the construct
 *     rows of the C lines that SOURCE records reach (`ast` = the number of the C line, per run),
 *     marks the runs that hold rows with the map's AST mode, and keeps A, Q, K lines in the
 *     design's extra table with the mutating slot at 0;
 *   - a recs list of an O or N line with more than one id is a located WARNING (the first is
 * bound). *out is the binding table (free it with odin3_restore_free), NULL on failure. A map of
 * version 1 is INVALID_ARG. NO_MEMORY. The map must stay alive until odin3_restore_free.
 */
odin3_status odin3_design_restore(odin3_design *design, odin3_name_map *map, const char *path,
                                  odin3_restore **out);

/* The record bound to an O entry of the restored map (a pointer into map->entries), 0 for none. */
uint32_t odin3_restore_entry_record(const odin3_restore *restore, const odin3_name_entry *entry);

/* The record bound to the N line of (module, kind, name), 0 for none. */
uint32_t odin3_restore_named_record(const odin3_restore *restore, odin3_bytes module,
                                    odin3_name_kind kind, odin3_bytes name);

/* Frees the table; NULL is a no-op. */
void odin3_restore_free(odin3_restore *restore);

#endif
