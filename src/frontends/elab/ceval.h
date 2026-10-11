/*
 * ceval.h — constant evaluation of elaboration (2D, spec §4), shared by both stages: the
 * expression typer (EL-13, with the EL-15/EL-16 types of literals and parameters). Task 9 adds
 * the evaluator and the constant-function interpreter here.
 *
 * The typer gives every node of an expression its self-determined type and its final type in
 * context, in the two sweeps of §4.2, each with an explicit stack (no recursion): a post-order
 * self sweep, then a pre-order context sweep in which the root's final type is
 * (max(self, context), self sign && context sign) and every context-determined operand inherits
 * its parent's final type. It reaches names only through its host (odin3_ctype_host), so one
 * typer serves both stages and both stores.
 */
#ifndef ODIN3_FRONTENDS_ELAB_CEVAL_H
#define ODIN3_FRONTENDS_ELAB_CEVAL_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * An expression type: width in bits, signedness, and whether the node is an unsized constant
 * (an unsized literal, or a parameter still unsized by EL-16), which extends to its final width
 * by repeating its top bit (EL-13, Yosys bitsAsUnsizedConst). Only leaves are unsized; an
 * operator's result is sized.
 */
typedef struct odin3_etype {
    uint32_t width;
    bool is_signed;
    bool is_unsized;
} odin3_etype;

/*
 * What the typer cannot know alone. All three callbacks are required.
 *  - leaf types an IDENT, a HIER_NAME, a CALL (any but the built-in $signed, $unsigned and
 *    $clog2 of one argument) and a memory word. Before typing a SELECT whose object is an IDENT
 *    or HIER_NAME (directly or through further SELECTs), the typer asks leaf about the SELECT:
 *    the host fills a width of at least 1 when the SELECT is a memory word (the typer then types
 *    only its index expressions, as self-determined), and leaves width 0 for an ordinary bit or
 *    part select. The host decides by the depth of the select chain; it is asked about the
 *    outermost SELECT first and then about each inner one it declined (O(depth²) walks, so a
 *    host should reject an over-indexed name at the first ask). For any other node a width of 0
 *    is a host bug (INVALID_ARG). A host error is reported by the host and its status returned;
 *    the typer stops.
 *  - constant gives the value of a replication count, a part-select bound or a +:/-: width, all
 *    self-determined. The node's subtree is typed (self and final) before the call, so the host
 *    may read it with odin3_ctype_self/final; odin3_ctype_constant_what names the role for its
 *    diagnostics. On failure the host reports and returns its status; the typer stops.
 *  - report routes the typer's own diagnostics (msg without location or level prefix) to the
 *    stage's counter, which also deduplicates repeated warnings (§9.2).
 * Arguments of a user function call are not typed here: whoever binds a call types each
 * argument as an assignment to its formal (context {L(formal), true, false}, §4.2).
 */
typedef struct odin3_ctype_host {
    odin3_status (*leaf)(void *user, odin3_ast_id node, odin3_etype *out);
    odin3_status (*constant)(void *user, odin3_ast_id node, int64_t *out);
    void (*report)(void *user, odin3_log_level level, odin3_loc loc, const char *msg);
    void *user;
} odin3_ctype_host;

/* A typer over one store: node → {self, final} in a u64map, reused across expressions. */
typedef struct odin3_ctype odin3_ctype;

/*
 * A typer for the expressions of ast, with a copy of *host. INVALID_ARG (logged) for a NULL
 * argument or callback; NO_MEMORY (*out unchanged).
 */
odin3_status odin3_ctype_create(odin3_ast *ast, const odin3_ctype_host *host, odin3_ctype **out);

/* Frees the typer; NULL is a no-op. */
void odin3_ctype_destroy(odin3_ctype *ty);

/*
 * Types the expression at root (spec §4.2) in context: width 0 = self-determined (the root's
 * final type is its self type, context's sign ignored); otherwise the root's final type is
 * (max(self, context.width), self sign && context.is_signed) — an assignment passes
 * {L(lhs), true, false}. Every node of the subtree is typed again (a later call overwrites).
 * Diagnostics, through host->report, located at the node named:
 *  - error `width N exceeds the limit of M bits` (M = the width cap in force), at the node
 *    whose width exceeds it (or the root, for a context above the cap);
 *  - error `replication count N is negative` (at the count), `part-select width N is not
 *    positive` (at the width), `real number in an expression is not supported` (L9, at the
 *    REAL);
 *  - warning `unsized constant in a concatenation is taken as N bits` (E12, at each unsized
 *    operand of a concatenation: a literal, or a parameter still unsized by EL-16);
 *  - warning `zero-width concatenation`, at the outermost zero-width node outside a
 *    concatenation (one whose parent is neither a concatenation nor zero-width, or the root).
 *    Only {0{…}} and what is built from it have width 0; as Yosys and Parmys, it is accepted
 *    anywhere with width 0, so its final width is that of its context (0 when self-determined).
 * Returns OK (warnings do not fail); PARSE after one of those errors; a host callback's failure
 * status; INVALID_ARG (logged) for a NULL typer, a root that is not an expression node or a host
 * leaf of width 0; NO_MEMORY. On failure the types of the subtree are unspecified. A host
 * callback may itself call odin3_ctype_expr on this typer (a constant function's body needs
 * typing): the nested call works on the stacks above the caller's and leaves them as it found
 * them.
 */
odin3_status odin3_ctype_expr(odin3_ctype *ty, odin3_ast_id root, odin3_etype context);

/*
 * The node's self-determined / final type from the last odin3_ctype_expr that typed it;
 * {0, false, false} for a node not typed (a user call's arguments, the identifier and inner
 * selects of a memory word); a zero-width node typed self-determined also reads {0, …}.
 */
odin3_etype odin3_ctype_self(const odin3_ctype *ty, odin3_ast_id node);
odin3_etype odin3_ctype_final(const odin3_ctype *ty, odin3_ast_id node);

/*
 * During a host->constant call: the §9.1 `<what>` of the constant asked for, "replication
 * count" or "range bound" (part-select bounds and +:/-: widths). NULL outside such a call.
 */
const char *odin3_ctype_constant_what(const odin3_ctype *ty);

#endif
