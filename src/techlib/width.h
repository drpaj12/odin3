/*
 * width.h — compiled width expressions: the IR's fourth width rule (odin3_width_expr) backed by the
 * .o3lib expression evaluator, and an identifier walk over expression trees.
 */
#ifndef ODIN3_TECHLIB_WIDTH_H
#define ODIN3_TECHLIB_WIDTH_H

#include "ir/celltype.h"
#include "odin3/odin3.h"
#include "techlib/expr.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/vec.h"

/*
 * Appends to nodes (a vec of const odin3_expr *) every IDENT node of the tree at root, in
 * depth-first order, left operand first. Iterative. ODIN3_ERR_NO_MEMORY on out of memory (nodes
 * may then hold a prefix of the result).
 */
odin3_status odin3_expr_collect_idents(const odin3_expr *root, odin3_vec *nodes);

/*
 * What a width expression is compiled from: the tree, the strtab naming its identifiers, and the
 * parameter names (strtab IDs) of the definition it will belong to, in definition order.
 */
typedef struct odin3_width_source {
    odin3_arena *arena; /* receives the compiled expression; must outlive its users */
    const odin3_strtab *strtab;
    const odin3_expr *expr;
    const uint32_t *param_names;
    uint32_t n_params;
} odin3_width_source;

/*
 * Compiles src->expr into a width expression allocated in src->arena. Identifiers are resolved to
 * parameter indices here, so evaluation never compares names. Its check hook accepts a definition
 * whose parameter at each resolved index has the identifier's name and kind INT (an identifier
 * that is not a parameter fails the check, naming it); its eval hook evaluates with those
 * parameters' values (64-bit, expr.h semantics; allocation-free unless the expression is very
 * deep) and requires a result in [0, UINT32_MAX]. ODIN3_ERR_NO_MEMORY on out of memory; *out is
 * untouched on failure.
 */
odin3_status odin3_width_expr_compile(const odin3_width_source *src, const odin3_width_expr **out);

/* The expression a width expression made by odin3_width_expr_compile evaluates. */
const odin3_expr *odin3_width_expr_tree(const odin3_width_expr *wexpr);

#endif
