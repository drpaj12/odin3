/*
 * design.h — the design handle: owner of the design-global strtab, cell-type table, source
 * manager and per-run AST stores.
 */
#ifndef ODIN3_IR_DESIGN_H
#define ODIN3_IR_DESIGN_H

#include "ast/kinds.h"
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

struct odin3_srcman; /* src/ast/srcman.h */

/*
 * The design's source manager (AST-1), created on first use; it lives as long as the design,
 * which destroys it. NO_MEMORY (with *out unchanged) when creating it fails.
 */
odin3_status odin3_design_get_srcman(odin3_design *design, struct odin3_srcman **out);

/* An AST store (src/ast/ast.h); the design holds at most one per (read run, form) (AST-15). */
typedef struct odin3_ast odin3_ast;

/* Which stores odin3_design_drop_ast keeps (AST-16; --keep-ast, --keep-ast=all). */
typedef enum odin3_keep_ast {
    ODIN3_KEEP_AST_NONE = 0,   /* drop destroys every store */
    ODIN3_KEEP_AST_ELABORATED, /* drop keeps elaborated stores */
    ODIN3_KEEP_AST_ALL         /* drop keeps parsed and elaborated stores */
} odin3_keep_ast;

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
odin3_status odin3_design_get_ast(const odin3_design *design, odin3_passrun_id run,
                                  odin3_ast_form form, odin3_ast **out);

/*
 * The end of a read pass: destroys the held store of (run, form) unless the keep level retains
 * that form; a store not held is a no-op.
 */
void odin3_design_drop_ast(odin3_design *design, odin3_passrun_id run, odin3_ast_form form);

/* Sets the keep level (default ODIN3_KEEP_AST_NONE); it applies to later drops. */
void odin3_design_set_keep_ast(odin3_design *design, odin3_keep_ast level);
odin3_keep_ast odin3_design_keep_ast(const odin3_design *design);

#endif
