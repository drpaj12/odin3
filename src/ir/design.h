/*
 * design.h — the design handle: owner of the design-global strtab, cell-type table and source
 * manager.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdint.h>

/*
 * odin3_design (opaque), odin3_design_create and odin3_design_destroy are part of the public ABI
 * and declared in odin3.h: a new design's cell-type table holds every process-global definition
 * registered so far (built-ins first, then plugin additions in registration order; IR-11).
 */

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
struct odin3_srcman; /* src/ast/srcman.h */

/*
 * The design's source manager (AST-1), created on first use; it lives as long as the design,
 * which destroys it. NO_MEMORY (with *out unchanged) when creating it fails.
 */
odin3_status odin3_design_get_srcman(odin3_design *design, struct odin3_srcman **out);

#endif
