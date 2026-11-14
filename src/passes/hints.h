/*
 * hints.h — timing hints: what a routed pass-1 run measured, kept for a pass-2 run (spec
 * docs/specs/2026-11-14-timing-hints.md). A hint file is plain text; this module reads it into a
 * read-only table (a side table: the IR, its attributes and ABI 7 are not touched) that the passes
 * that consume hints look up. The first consumer is the abc pass (passes/abc/timing.c).
 *
 * The table of the design in hand is cached by the design's serial (as prov_depth.c caches the
 * depth), so a design read afresh never sees an older design's hints. Private to src/passes.
 */
#ifndef ODIN3_PASSES_HINTS_H
#define ODIN3_PASSES_HINTS_H

#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The first line of a hint file: this word, a TAB and the format version. */
#define ODIN3_HINTS_MAGIC "odin3-timing-hints"
enum { ODIN3_HINTS_VERSION = 1, ODIN3_HINTS_SOURCE_BYTES = 512 };
#define ODIN3_HINTS_TIGHTEN_DEFAULT 1.0

/* A hint for one provenance record: the gate-level record of a word cell (its `uid=` in a name map
 * written with ODIN3_PROV_UID=1, which is the record ID of the cell's gates in the design). */
typedef struct odin3_hint_cell {
    uint32_t uid;
    double slack_ps; /* routed slack, worst over the object's LUTs (and, in the file, over seeds) */
    double crit;     /* routed criticality, 0..1, worst over the object's LUTs */
    uint32_t depth;  /* pass-1 unit depth (LUT levels) of the object's shallowest LUT */
    uint32_t luts;   /* LUTs the object's hint took its worst slack from */
} odin3_hint_cell;

/* A hint for one net by name, "<module>/<net>": a routed arrival and/or required time in ps. A
 * time the file leaves as `-` is NAN. */
typedef struct odin3_hint_net {
    char *name;
    double arrival_ps;
    double required_ps;
} odin3_hint_net;

typedef struct odin3_hints {
    char *label;         /* the file name, for messages */
    double cpd_ps;       /* routed critical path delay of pass 1 */
    double slack_min_ps; /* the worst slack of pass 1 (the reference: slack - slack_min >= 0) */
    uint32_t depth;      /* mapped unit depth of pass 1 (whole design) */
    double ns_per_level; /* ns per LUT level: cpd / depth unless the file gives it */
    double tighten_levels; /* levels asked for on top of pass 1's depth (meta tighten_levels) */
    char source[ODIN3_HINTS_SOURCE_BYTES]; /* the `meta` lines as `key=value` words, for the log */
    odin3_hint_cell *cells;                /* sorted by uid, unique */
    size_t n_cells;
    odin3_hint_net *nets; /* sorted by name, unique */
    size_t n_nets;
    uint8_t *cell_used; /* per cell: a lookup found it in a region (the run's match count) */
    uint8_t *net_used;
} odin3_hints;

/*
 * Parses hint text (len bytes; label names it in messages). Format: line 1 `odin3-timing-hints`
 * TAB `1`; then lines of TAB-separated fields, blank lines and lines starting with `#` ignored:
 *   meta TAB key TAB value        keys cpd_ps, slack_min_ps, depth, ns_per_level, tighten_levels are read, any
 *                                 other key is kept in `source`
 *   cell TAB uid TAB slack_ps TAB crit TAB depth TAB luts
 *   net  TAB <module>/<net> TAB arrival_ps TAB required_ps      (a time may be `-`)
 * A duplicate cell or net keeps the worst (smallest slack, smallest required, largest arrival).
 * ODIN3_ERR_PARSE after logging one `read_hints: <label>:<line>: <what>` error for a wrong first
 * line, an unknown record kind, a wrong field count, a number that is not one, a cell with
 * depth 0 and luts 0 together, or a file whose cell or net records need a `cpd_ps`, `depth`
 * and `slack_min_ps` meta line that is missing. NO_MEMORY. *out is NULL on failure.
 */
odin3_status odin3_hints_parse(const char *text, size_t len, const char *label, odin3_hints **out);

void odin3_hints_free(odin3_hints *hints);

/* The cell hint of a record ID, or NULL; marks it used. */
const odin3_hint_cell *odin3_hints_find_cell(odin3_hints *hints, uint32_t uid);

/* The net hint of "<module>/<net>", or NULL; marks it used. */
const odin3_hint_net *odin3_hints_find_net(odin3_hints *hints, const char *name);

/* How many cell and net hints a find has returned so far. */
size_t odin3_hints_cells_used(const odin3_hints *hints);
size_t odin3_hints_nets_used(const odin3_hints *hints);

/*
 * The hints of design: those the `read_hints` pass loaded, else (loaded now, once) the file the
 * project's `timing_hints` key names; NULL when there are none. A project file that cannot be
 * read is an error logged once, and NULL. Valid until the next read_* pass or hints_set.
 */
odin3_hints *odin3_hints_of(const odin3_design *design);

/* Loads path and makes it the hints of design (replacing any). ODIN3_ERR_IO, _PARSE, NO_MEMORY. */
odin3_status odin3_hints_load(const odin3_design *design, const char *path);

/* Forgets the cached table (a design read afresh, as odin3_prov_depth_reset). */
void odin3_hints_reset(void);

#endif
