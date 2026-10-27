/*
 * names_v2.h — the version 2 views of the `names` pass (V1.2.0 3.5): the records and the chain of
 * an object, read from a parsed map. Reads the lines name_map.c kept; parses nothing itself.
 */
#ifndef ODIN3_PASSES_NAMES_V2_H
#define ODIN3_PASSES_NAMES_V2_H

#include "backends/common/name_map.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdint.h>

/* One line of a table, so that the table is a vector of structs. */
typedef struct names_v2_ref {
    const odin3_nm_line *line;
} names_v2_ref;

typedef enum names_v2_tab { NV2_R, NV2_C, NV2_F, NV2_T, NV2_TABS } names_v2_tab;

/* Marks of one query: one byte per R, C, F and T line. They are allocated once per command and
 * only the entries a query set (the touched ones) are cleared before the next query. */
typedef struct names_v2_marks {
    uint8_t *seen[NV2_TABS]; /* indexed by line number, so size + 1 */
    odin3_vec touched;       /* names_v2_touch: the marks set since the last reset */
} names_v2_marks;

/* The R, C, F and T lines of a map by number (index = number - 1). */
typedef struct names_v2 {
    const odin3_name_map *map;
    odin3_vec tab[NV2_TABS]; /* names_v2_ref */
    names_v2_marks walk;     /* the records and constructs reached from an object */
    names_v2_marks reach;    /* the files and runs a printout names */
} names_v2;

typedef enum names_v2_mode {
    NV2_LOOKUP, /* the object line, its records (indented by depth), then its chain, files, runs */
    NV2_CHAIN   /* the chain: its construct lines, then the file lines they name */
} names_v2_mode;

/* Indexes the R, C, F and T lines of map (a version 2 map). ODIN3_ERR_NO_MEMORY on failure. */
odin3_status names_v2_init(names_v2 *v2, const odin3_name_map *map);
void names_v2_free(names_v2 *v2);

/*
 * Appends, for each O line whose short name or N line whose name is `value`, the text of mode to
 * out, and sets *matches to how many lines matched. NO_MEMORY on failure. Never fails on a map
 * that does not pass the check: a reference that does not resolve is skipped.
 */
odin3_status names_v2_print(names_v2 *v2, odin3_bytes value, names_v2_mode mode,
                            odin3_strbuf *out, uint32_t *matches);

#endif
