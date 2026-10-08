/*
 * design.h — the design handle: owner of the design-global strtab and cell-type table.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

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

#endif
