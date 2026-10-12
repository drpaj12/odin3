/*
 * elab_internal.h — stage 2's internal interfaces (2D, spec §12, plan point (j)). This part is the
 * binding (EL-5): the symbol table of an elaborated store and the resolution array stage 2 reads,
 * filled by the binder (bind.c), which also checks the rows of the EL-4 form that 2A's check rule 7
 * does not. Later tasks add the stage-2 context, emission, drivers and memories here.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H

#include "ast/ast.h"
#include "ast/symtab.h"
#include "odin3/odin3.h"
#include "util/u64map.h"

#include <stdint.h>

/*
 * The binding of an elaborated store (EL-5), stage 2's (freed with it); the symbol table it fills
 * is the store's (odin3_ast_symtab, freed with the store).
 *  - sym_of[node.v] for node.v < n: for every IDENT, HIER_NAME, CALL, TASK_CALL and INSTANTIATION,
 *    the symbol it resolves to (an INSTANTIATION: the MODULE symbol of the store's MODULE it names,
 *    0 for a registered cell type, which stage 2 finds in the registry; a system CALL or TASK_CALL:
 *    0); for every declaring node (DECLARATOR, INSTANCE, MODULE, GEN_BLOCK, named SEQ_BLOCK,
 *    FUNCTION_DECL, TASK_DECL), the symbol it declares (the first GEN_BLOCK of a loop's name and
 *    each further iteration share one BLOCK symbol); 0 elsewhere.
 *  - scope_of: node ID → scope ID for every MODULE, GEN_BLOCK, named SEQ_BLOCK, FUNCTION_DECL and
 *    TASK_DECL (odin3_elab_scope_of).
 */
typedef struct odin3_elab_binding {
    odin3_sym_id *sym_of;
    uint32_t n;
    odin3_u64map *scope_of;
} odin3_elab_binding;

/*
 * Binds elab (EL-5) in one pre-order sweep: creates its symbol table (odin3_symtab_create) and
 * fills *out. Scopes: DESIGN → one LIBRARY per library (the FILE buffer's of each MODULE's loc,
 * "work" when it has none) → MODULE → GENERATE per GEN_BLOCK (index from its slot), BLOCK per
 * named SEQ_BLOCK, FUNCTION, TASK. Symbols: one per declarator (NET, VAR, LOCALPARAM with its
 * value), INSTANCE, BLOCK, FUNCTION (and, in its scope, the VAR of its result, named like it),
 * TASK, and a MODULE per MODULE in its LIBRARY; a black-box MODULE (its ATTR blackbox) has a scope
 * holding its ports only. Names resolve from their scope outward to the MODULE scope, so a name
 * declared later in a scope resolves (divergence §17 #1); an INSTANTIATION names a MODULE of the
 * store. Assignments (odin3_symbol_add_assign, source order, the assigning node): every NET_ASSIGN
 * lvalue and net declaration assignment (CONTINUOUS), every BLOCKING_ASSIGN / NONBLOCKING_ASSIGN
 * lvalue outside function and task bodies.
 *
 * Errors, located (§9.1), counted against the error cap in force (ODIN3_ELAB_MAX_ERRORS: past it,
 * "too many errors; elaboration stopped after N" and the sweep stops):
 *  - user errors (§2.5), then ODIN3_ERR_PARSE: `'x' is not declared`; `'x' is already declared
 *    (first at f.v:L:C)`; `hierarchical reference 'u.w' is not supported`; `'g[5].t' does not
 *    exist (generate block 'g[5]' was not elaborated)`; `'f' is not a function`; `'t' is not a
 *    task`; `width N exceeds the limit of M bits` (a packed range, or a bound outside int32);
 *    `range bound has x or z bits`; `'u' is not a net, variable or parameter` (a name of an
 *    instance, function or task; a block's name binds to its BLOCK symbol, Task 18's carry);
 *  - EL-4 violations, the rows rule 7 does not check (a misuse of the seam), then
 *    ODIN3_ERR_INVALID_ARG: `elaborated form: <rule>` (spec §2.4, v4.9).
 * ODIN3_ERR_NO_MEMORY stops at once. On any failure *out is emptied (nothing to free) and the
 * partly built symbol table stays with the store (it passes check rule 8; odin3_ast_destroy frees
 * it). INVALID_ARG (logged, nothing bound) for a NULL argument, a store that is not ELABORATED or
 * does not hold exactly one UNIT, or one that has a symbol table already. Never changes the nodes;
 * iterative (explicit stacks, no recursion).
 */
odin3_status odin3_elab_bind(odin3_ast *elab, odin3_elab_binding *out);

/* The scope a MODULE, GEN_BLOCK, named SEQ_BLOCK, FUNCTION_DECL or TASK_DECL opened; 0 otherwise
 * (or for a NULL binding). */
odin3_scope_id odin3_elab_scope_of(const odin3_elab_binding *binding, odin3_ast_id node);

/* The symbol node resolves to or declares (sym_of); 0 outside the array or for a NULL binding. */
odin3_sym_id odin3_elab_sym_of(const odin3_elab_binding *binding, odin3_ast_id node);

/* Frees the binding's arrays and empties it; NULL and an empty binding are no-ops. */
void odin3_elab_binding_free(odin3_elab_binding *binding);

#endif
