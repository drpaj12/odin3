/*
 * prov_export.h — what a provenance record is, for export and for the ABI (V1.2.0 3.3): its class
 * labels, its construct, and the joins it makes. One definition, shared by the ABI calls
 * (odin3_prov_get_info and friends) and the map writers (Task 5), so no consumer recomputes it.
 */
#ifndef ODIN3_BACKENDS_COMMON_PROV_EXPORT_H
#define ODIN3_BACKENDS_COMMON_PROV_EXPORT_H

#include "backends/common/name_map.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most rows of J a record lists (3.3, "one cap rule"); n_joined is still the total. */
enum { ODIN3_PROV_JOIN_CAP = 16 };

/* The labels of one record. joined: every row of J (uint32_t), in the key order of 3.3. */
typedef struct odin3_prov_label {
    odin3_prov_info info;
    odin3_vec joined;
} odin3_prov_label;

/* Empties label (no allocation); odin3_prov_label_free releases what compute allocated. */
void odin3_prov_label_init(odin3_prov_label *label);
void odin3_prov_label_free(odin3_prov_label *label);

/*
 * Fills label for record: its fields, and for a coarsen record the join (3.3: J reduced to
 * anchors of level stmt or coarser, a join when two or more are distinct and no single data
 * parent has the same set; a join of instances when one anchor is reached through two context
 * chains) and the rows of J. Walks the record graph with an explicit worklist; the context
 * parents (the declared tail) are not entered. INVALID_ARG (logged) for record 0 or one past the
 * last; NO_MEMORY. label is unchanged on failure.
 */
odin3_status odin3_prov_label_compute(const odin3_design *design, uint32_t record,
                                      odin3_prov_label *label);

/* --- the version 2 name map (V1.2.0 3.2, 3.5) ------------------------------------------------- */

/* `--prov-map [refs|constructs|full]` as the writers take it. NONE: not requested (map version 1);
 * ANY: given without a mode (the default, refs, clipped to what the read kept). */
typedef enum odin3_prov_map_mode {
    ODIN3_PROV_MAP_NONE,
    ODIN3_PROV_MAP_ANY,
    ODIN3_PROV_MAP_REFS,
    ODIN3_PROV_MAP_CONSTRUCTS,
    ODIN3_PROV_MAP_FULL
} odin3_prov_map_mode;

/* True and *mode when word is exactly refs, constructs or full (the value of `--prov-map`). */
bool odin3_prov_map_word(odin3_bytes word, odin3_prov_map_mode *mode);

/*
 * Turns the requested mode into what the map holds (4.2): at most what the reads kept (the largest
 * mode of any run's snapshot; full allows all three, refs allows refs or constructs, constructs
 * allows constructs). *effective is ODIN3_CONSTRUCT_MODE_OFF for `ast=none` (no run kept a table;
 * only ANY is allowed then). INVALID_ARG (logged, naming `read_verilog --prov-ast`) for an explicit
 * mode above what was kept. `who` names the writer in the message.
 */
odin3_status odin3_prov_map_resolve(const odin3_design *design, odin3_prov_map_mode requested,
                                    const char *who, odin3_construct_mode *effective);

/* One object of the map: an O line (entry holds the seven version 1 fields) or, with kept, an N
 * line (entry.module, entry.kind and entry.short_name = the name it keeps). record is the
 * object's own provenance record, 0 for none. */
typedef struct odin3_prov_obj {
    odin3_name_entry entry;
    bool kept;
    uint32_t record;
} odin3_prov_obj;

typedef struct odin3_prov_map_req {
    const odin3_design *design;
    uint32_t width;           /* the hash width of the header (1..52) */
    odin3_construct_mode ast; /* the AST layer written (from odin3_prov_map_resolve) */
} odin3_prov_map_req;

/*
 * Appends the whole version 2 map text to out: the header, the O and N lines in sort order, then
 * R, C, F and T lines, numbered by the algorithm of 3.2 (post-order over the parents, constructs
 * after their parent chains, files and pass runs in order of first reference), so equal designs
 * give equal bytes. The join labels are those of odin3_prov_label_compute; a join lists at most
 * ODIN3_PROV_JOIN_CAP constructs. INVALID_ARG (logged) for two O lines, or two N lines, with one
 * (module, kind, name), a record that is not one of the design, or a bad width; NO_MEMORY. On
 * failure out is as it was.
 */
odin3_status odin3_prov_map_text(const odin3_prov_map_req *req, const odin3_prov_obj *objs,
                                 size_t count, odin3_strbuf *out);

#endif
