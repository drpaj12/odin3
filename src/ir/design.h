/*
 * design.h — the design handle: owner of the design-global strtab and cell-type table.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

#include "ir/ids.h"
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

/*
 * Records module as the design's top module (the design record; DESIGN §4.0, PHASE1 #18): set by
 * the BLIF reader to its first model and by the `hierarchy` pass. Never allocates; module IDs are
 * stable (compact renumbers objects inside modules, never modules), so the top stays valid.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL design or an ID that is not a module of the design;
 * the top is unchanged then.
 */
odin3_status odin3_design_set_top(odin3_design *design, odin3_module_id module);

/* The top module, none (ID 0) until one is set. */
odin3_module_id odin3_design_top(const odin3_design *design);

#endif
