/*
 * design.h — the design handle: owner of the design-global strtab, cell-type table, source
 * manager, per-run AST stores and project record.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

#include "ast/srcfmt.h"
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

/*
 * odin3_design_get_srcman (public, odin3.h): the design's source manager (AST-1), created on
 * first use; it lives as long as the design, which destroys it.
 */

/* The keep levels (odin3_keep_ast) and odin3_ast are public: odin3.h. The design holds at most
 * one store per (read run, form) (AST-15). */

/*
 * Hands ast (created on this design) to the design, which destroys it with itself or at
 * odin3_design_drop_ast. INVALID_ARG (logged; the caller still owns ast) for a NULL argument, a
 * store of another design or a held store with the same run and form; NO_MEMORY (likewise).
 */
odin3_status odin3_design_set_ast(odin3_design *design, odin3_ast *ast);

/*
 * The held store of pass run `run` and form in *out (borrowed; NULL when none is held). OK, or
 * INVALID_ARG (logged) for a NULL design or out.
 */
odin3_status odin3_design_find_ast(const odin3_design *design, odin3_passrun_id run,
                                   odin3_ast_form form, odin3_ast **out);

/* Number of held stores, and store idx in the order they were handed over (NULL out of range). */
uint32_t odin3_design_ast_count(const odin3_design *design);
odin3_ast *odin3_design_ast_at(const odin3_design *design, uint32_t idx);

/*
 * The end of a read pass: destroys the held store of (run, form) unless the keep level retains
 * that form; a store not held is a no-op.
 */
void odin3_design_drop_ast(odin3_design *design, odin3_passrun_id run, odin3_ast_form form);

/* odin3_design_set_keep_ast (public, odin3.h) sets the level later drops apply; this reads it. */
odin3_keep_ast odin3_design_keep_ast(const odin3_design *design);

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
