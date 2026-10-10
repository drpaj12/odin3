/*
 * design.h — the design handle: owner of the design-global strtab, cell-type table, source
 * manager and project record.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

#include "ast/srcfmt.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdint.h>

typedef struct odin3_design odin3_design; /* opaque */

/*
 * New design whose cell-type table holds every process-global definition registered so far
 * (built-ins first, then plugin additions in registration order; IR-11). NULL on out of memory.
 */
odin3_design *odin3_design_create(void);

/* Frees the design and everything it owns. NULL is a no-op. */
void odin3_design_destroy(odin3_design *design);

/* The design-global string table (names, string parameter values). */
odin3_strtab *odin3_design_strtab(const odin3_design *design);

/* Interns bytes in the design's strtab; same contract as odin3_strtab_intern. */
odin3_status odin3_design_intern(odin3_design *design, odin3_bytes bytes, uint32_t *str);

struct odin3_srcman; /* src/ast/srcman.h */

/*
 * The design's source manager (AST-1), created on first use; it lives as long as the design,
 * which destroys it. NO_MEMORY (with *out unchanged) when creating it fails.
 */
odin3_status odin3_design_get_srcman(odin3_design *design, struct odin3_srcman **out);

struct odin3_project; /* src/frontends/project/project.h */

/*
 * The design's project record (PRJ-1), created empty on first use; the design destroys it.
 * NO_MEMORY (with *out unchanged) when creating it fails.
 */
odin3_status odin3_design_get_project(odin3_design *design, struct odin3_project **out);

/* The design's project record, NULL when there is none. */
struct odin3_project *odin3_design_project(const odin3_design *design);

/* Destroys the design's project record (a failed read_project), so the next use starts clean. */
void odin3_design_drop_project(odin3_design *design);

/*
 * How diagnostics name files (PRJ-18 --diag-paths): ODIN3_SRCFMT_GIVEN (the default) or
 * ODIN3_SRCFMT_RESOLVED. Any other value is stored as GIVEN.
 */
void odin3_design_set_diag_paths(odin3_design *design, odin3_srcfmt style);
odin3_srcfmt odin3_design_diag_paths(const odin3_design *design);

#endif
