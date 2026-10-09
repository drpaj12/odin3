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

/* What a width expression is compiled from; strtab holds the names of expr's identifiers. */
typedef struct odin3_width_source {
    odin3_arena *arena; /* receives the compiled expression; must outlive its users */
    const odin3_strtab *strtab;
    const odin3_expr *expr;
} odin3_width_source;

/*
 * Compiles src->expr into a width expression allocated in src->arena: its check hook accepts a
 * definition when every identifier names an INT parameter of it; its eval hook evaluates the
 * expression with those parameters' values (64-bit, expr.h semantics) and requires a result in
 * [0, UINT32_MAX]. ODIN3_ERR_NO_MEMORY on out of memory; *out is untouched on failure.
 */
odin3_status odin3_width_expr_compile(const odin3_width_source *src, const odin3_width_expr **out);

/* The expression a width expression made by odin3_width_expr_compile evaluates. */
const odin3_expr *odin3_width_expr_tree(const odin3_width_expr *wexpr);

#endif
