/*
 * ceval.h — constant evaluation of elaboration (2D, spec §4), shared by both stages: the
 * expression typer (EL-13, with the EL-15/EL-16 types of literals and parameters), the
 * evaluator (EL-12, EL-14, EL-18) and the constant-function interpreter (EL-17).
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
#include "frontends/elab/cval.h"
#include "odin3/odin3.h"
#include "util/arena.h"

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

/* --- the evaluator and the interpreter (spec §4.1, §4.6, §4.7; EL-12, EL-17, EL-18) ---------- */

/* What a name denotes, for the evaluator (odin3_ceval_host). */
typedef enum odin3_csym_kind {
    ODIN3_CSYM_NONE = 0, /* not declared (lookup), or not a memory word (lookup of a SELECT) */
    ODIN3_CSYM_CONST,    /* a parameter, localparam or genvar with a known value */
    ODIN3_CSYM_SIGNAL,   /* a net or variable (or a memory word): not a constant */
    ODIN3_CSYM_FUNCTION,
    ODIN3_CSYM_TASK
} odin3_csym_kind;

/*
 * The host's answer about a name. kind is an odin3_csym_kind. For CONST and SIGNAL: type is the
 * name's expression type (EL-13; a still-unsized parameter has is_unsized), msb and lsb its
 * declared range as written (an unranged value: width - 1 and 0; integer: 31 and 0), so bit and
 * part selects find their bits; for CONST, value is the value, of type.width bits with SIGNED and
 * UNSIZED as type says (STRING kept), owned by the host and valid until the evaluation ends.
 * decl is the declaration: the DECLARATOR (CONST, SIGNAL), the FUNCTION_DECL or the TASK_DECL;
 * name the strtab ID of the name, for diagnostics (needed for a HIER_NAME, which has none).
 */
typedef struct odin3_csym {
    uint8_t kind;
    odin3_cval value;
    odin3_etype type;
    odin3_ast_id decl;
    uint32_t name;
    int64_t msb; /* added to the brief's fields: the declared range (agent default) */
    int64_t lsb;
} odin3_csym;

/*
 * What the evaluator cannot know alone: the scope of the store being evaluated. All three
 * callbacks are required; *out starts zeroed (kind NONE).
 *  - lookup resolves an IDENT or HIER_NAME at ref in its own scope (a function's locals are the
 *    evaluator's). It also answers the typer's memory-word question (ceval.h, odin3_ctype_host):
 *    for a SELECT whose object is a name, it gives SIGNAL with the word's type and range when the
 *    SELECT is a memory word, else NONE. A name that is a whole memory is the host's to report
 *    ('m' is a memory; select a word). NONE for an IDENT is reported by the evaluator as
 *    `'x' is not declared`.
 *  - callee resolves the CALL or TASK_CALL at call to its FUNCTION_DECL or TASK_DECL (a function
 *    may be declared after its use, K7). NONE is reported as `'f' is not declared`.
 *  - report routes a diagnostic (msg without location or level prefix) to the stage's counter,
 *    which also deduplicates warnings (§9.2): the typer may report a warning again when an
 *    expression is typed again.
 * A callback that fails reports its own diagnostic and returns its status; the evaluation stops
 * with it. A callback may call the evaluator again (lazy parameters, EL-16): the nested call
 * works on the stacks above the caller's.
 */
typedef struct odin3_ceval_host {
    odin3_status (*lookup)(void *user, odin3_ast_id ref, odin3_csym *out);
    odin3_status (*callee)(void *user, odin3_ast_id call, odin3_csym *out);
    void (*report)(void *user, odin3_log_level level, odin3_loc loc, const char *msg);
    void *user;
} odin3_ceval_host;

/*
 * The values of variables outside function frames (EL-40's initial state), for
 * odin3_ceval_exec. Without a store, reading or assigning such a variable is not constant.
 *  - read gives the value of the variable named by ref (an IDENT or HIER_NAME) or of the memory
 *    word ref (a SELECT the host's lookup called a memory word; its indices are
 *    odin3_ceval_word_index), of its declared width: *known false when it is not a constant
 *    (an input, a net driven by logic), which makes the statement non-constant; bits never
 *    written read x. *out must stay valid until the next store call.
 *  - write sets the whole variable named by lvalue (an IDENT or HIER_NAME; a part select is
 *    read, merged and written whole) or the whole memory word lvalue (a SELECT, as for read) to
 *    *value (valid during the call only). A word index with x or z bits is never passed: such a
 *    read is all x and such a write is dropped.
 * Writes happen as the statement runs: when odin3_ceval_exec reports the statement
 * non-constant, the store drops the writes of that call (EL-40: targets left as they were).
 */
typedef struct odin3_ceval_store {
    odin3_status (*read)(void *user, odin3_ast_id ref, odin3_cval *out, bool *known);
    odin3_status (*write)(void *user, odin3_ast_id lvalue, const odin3_cval *value);
    void *user;
} odin3_ceval_store;

/*
 * An evaluator over one store: its typer (whose host it is), an explicit stack of tasks, values
 * and frames (odin3_ceval_frame: a function's decl, its cursor stack and its locals), so neither
 * expressions nor recursive constant functions (K8) recurse in C.
 */
typedef struct odin3_ceval odin3_ceval;

/*
 * An evaluator for ast with a copy of *host, and its typer. INVALID_ARG (logged) for a NULL
 * argument or callback; NO_MEMORY (*out unchanged).
 */
odin3_status odin3_ceval_create(odin3_ast *ast, const odin3_ceval_host *host, odin3_ceval **out);

/* Frees the evaluator and its typer; NULL is a no-op. */
void odin3_ceval_destroy(odin3_ceval *ev);

/*
 * The evaluator's typer, for a caller that types without evaluating (stage 2's cells). Owned by
 * the evaluator; its host is the evaluator (names through odin3_ceval_host.lookup, constants
 * evaluated). NULL for NULL.
 */
odin3_ctype *odin3_ceval_types(odin3_ceval *ev);

/*
 * One evaluation: the expression, its context (as odin3_ctype_expr: width 0 = self-determined;
 * an assignment passes {L(lhs), true, false}) and §9.1's <what> for its diagnostics
 * ("parameter 'P'", "range bound", …; NULL reads "expression").
 */
typedef struct odin3_ceval_req {
    odin3_ast_id expr;
    odin3_etype context;
    const char *what;
} odin3_ceval_req;

/*
 * The value of req->expr (§4): typed (odin3_ctype_expr in req->context), then evaluated with
 * Yosys 0.55's constant semantics (odin3_cval_op), user function calls interpreted (§4.6) and
 * $clog2, $signed and $unsigned folded (§4.7). The result has the root's final width and sign
 * (UNSIZED or STRING kept when the root is such a leaf at its own width) and is copied into
 * arena; the evaluator's own memory is released when the outermost call returns. A
 * zero-width result ({0{…}} self-determined; only with context width 0) is width 0 with NULL
 * planes. Returns OK; PARSE after a located diagnostic: `<what> is not constant (it reads 'x')`
 * (a read of a net or variable), `function 'f' is not a constant function: <reason>`, an E20
 * `system function '$name' is not supported`, a typer error, or a cap (call depth, steps,
 * loop iterations, width, the '**' work bound); a host callback's failure status;
 * INVALID_ARG (logged) for a NULL argument or a root that is not an expression; NO_MEMORY.
 * *out is unchanged on failure.
 */
odin3_status odin3_ceval_expr(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                              odin3_cval *out);

/*
 * odin3_ceval_expr without diagnostics for non-constant input (stage 2 tries before it builds
 * cells): a read of a net or variable, a non-constant function (any reason) or a non-constant
 * replication count or bound gives OK with *is_const false and nothing logged. Every other
 * diagnostic (caps, errors) is still reported and returns PARSE. *out is set only when
 * *is_const is true.
 */
odin3_status odin3_ceval_try(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                             odin3_cval *out, bool *is_const);

/*
 * odin3_ceval_expr for a CALL of a user function (§4.6): the interpreter runs its body in a
 * fresh frame. INVALID_ARG (logged) when req->expr is not a CALL without the SYSTEM flag.
 */
odin3_status odin3_ceval_call(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                              odin3_cval *out);

/*
 * Runs the statement stmt at module level against store (EL-40; Task 24): variables outside
 * function frames are read and written through store (NULL: any such access is not constant);
 * blocking and nonblocking assignments both just set the value; $display-family tasks are
 * ignored. A statement that is not constant (a read store->read does not know, a non-constant
 * function, another system or user task) stops with OK and *is_const false and nothing logged;
 * the store then drops the writes it was given (odin3_ceval_store). Caps and errors report and
 * return PARSE; INVALID_ARG (logged) for a NULL argument or a node that is not a statement;
 * NO_MEMORY.
 */
odin3_status odin3_ceval_exec(odin3_ceval *ev, odin3_ast_id stmt, const odin3_ceval_store *store,
                              bool *is_const);

/*
 * During a store read or write of a memory word: its index idx (0 = the first written: m[i0][i1])
 * into *out, true; false when idx is not an index of the current word (or outside a store call).
 */
bool odin3_ceval_word_index(const odin3_ceval *ev, uint32_t idx, int64_t *out);

#endif
