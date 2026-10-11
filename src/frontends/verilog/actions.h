/*
 * actions.h — the grammar's action layer (2C spec §3.2): the per-stream parser context, the two
 * guard macros every action of verilog.y is written with, and the vp_* functions of actions*.c
 * they wrap. Also the parser.c functions the generated parser calls (YYLLOC_DEFAULT, YYMAXDEPTH,
 * yyerror).
 *
 * Every vp_* function takes the context, at most a location and two union-typed values, and an
 * out-pointer (plan point (s)); it reports a recoverable error itself (located, then error mode)
 * and returns ODIN3_OK, and returns non-OK only for a fatal failure, which VP turns into
 * ctx->fatal and YYABORT.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ACTIONS_H
#define ODIN3_FRONTENDS_VERILOG_ACTIONS_H

#include "ast/ast.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* A function or task header's record in ctx->headers (Task 6). */
typedef struct odin3_vp_header {
    uint32_t name;
    uint32_t flags;
} odin3_vp_header;

/* A case header's metacomment window in ctx->windows: stream offsets [lo, hi) (§2.6, Task 6). */
typedef struct odin3_vp_window {
    uint32_t lo;
    uint32_t hi;
} odin3_vp_window;

/*
 * The parser context, one per stream (§3.2). span_scratch/part_scratch: the copy of a pending
 * span and its partitioned prefix during one reordering reduction; meta_scratch: the ATTR nodes
 * of one case's metacomments; headers: odin3_vp_header records of function and task headers;
 * windows: odin3_vp_window metacomment windows of case headers (headers and windows are indexed by
 * a `u` value, so vp_* arity stays at five); error_mode: actions are inert (§3.4); fatal: the
 * status that aborted the parse.
 */
typedef struct odin3_vp_ctx {
    odin3_vl_run *run;
    odin3_ast *ast;
    odin3_vl_lexer *lexer;
    odin3_vec span_scratch, part_scratch, meta_scratch, headers, windows;
    bool error_mode;
    odin3_status fatal;
} odin3_vp_ctx;

/* The generated parser's lex parameter (%lex-param): the context's scanner. */
#define ODIN3_VP_SCANNER odin3_vl_scanner(ctx->lexer)

/* VP(out, call): the ordinary action. Inert in error mode; a non-OK status is fatal. */
#define VP(out, call)                                                                              \
    do {                                                                                           \
        if (ctx->error_mode) {                                                                     \
            (out) = 0;                                                                             \
        } else {                                                                                   \
            odin3_status vp_st_ = (call); /* vp_xxx stores its node in &(out) */                   \
            if (vp_st_ != ODIN3_OK) {                                                              \
                ctx->fatal = vp_st_;                                                               \
                YYABORT;                                                                           \
            }                                                                                      \
        }                                                                                          \
    } while (0)
/* VP_ERR(call): an alternative that is itself the error (the digit-less number rule). The
 * vp_* function reports it, located, and enters error mode; YYERROR then lets Bison recover
 * quietly. */
#define VP_ERR(call)                                                                               \
    do {                                                                                           \
        if (!ctx->error_mode) {                                                                    \
            (void)(call);                                                                          \
        }                                                                                          \
        YYERROR;                                                                                   \
    } while (0)

/* --- Task 1: the generic actions (actions.c) ----------------------------------------------- */

/* *out = value (an enum-valued nonterminal: net kind, direction, strength; plan point (s)). */
odin3_status vp_value(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out);

/* *out = the pending stack's mark (a list's empty alternative, §3.2). */
odin3_status vp_mark(const odin3_vp_ctx *ctx, uint32_t *out);

/* Pushes node on the pending stack; *list (the list's mark) is left as it is. NO_MEMORY. */
odin3_status vp_push(odin3_vp_ctx *ctx, odin3_ast_id node, const uint32_t *list);

/*
 * The stream's UNIT over at, its children the pending stack from mark, named after the stream
 * (odin3_vl_stream_name). NO_MEMORY; PARSE (located) at the child cap.
 */
odin3_status vp_unit(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, odin3_ast_id *out);

/* --- Task 4: source text, modules, ports, parameters, declarations, items, opaque units,
 * attributes (actions_decl.c) --------------------------------------------------------------- */

/* --- Task 5: expressions and primaries (actions_expr.c) ------------------------------------ */

/* --- Task 6: statements, timing, functions and tasks, case and metacomments
 * (actions_stmt.c) ------------------------------------------------------------------------- */

/* --- Task 7: instantiation, gates, generate (actions_inst.c) ------------------------------- */

/* --- parser.c: called by the generated parser ---------------------------------------------- */

/*
 * YYLLOC_DEFAULT (§2.2): rhs[1..n] are the right-hand symbols' locations, rhs[0] the symbol's
 * before them. loc and off come from the first non-empty symbol (loc != end), end from the last
 * non-empty one; with none (an empty rule, or only empty symbols) *cur is {rhs[0].end,
 * rhs[0].end, rhs[0].off}.
 */
void odin3_vp_lloc(odin3_vp_loc *cur, const odin3_vp_loc *rhs, int n);

/* YYMAXDEPTH (plan point (d)): the stack cap the driver set from the run's limits. */
long odin3_vp_yymaxdepth(void);

/*
 * yyerror (§3.4, plan point (e)): reports Bison's message at *at with "syntax error, " replaced
 * by "syntax: ", and enters error mode; "memory exhausted" (the stack cap, §5) becomes the
 * located fatal "nesting deeper than the parser allows (<cap>)", which stops the run. Silent
 * when ctx->fatal is already set.
 */
void odin3_vp_error(const odin3_vp_loc *at, odin3_vp_ctx *ctx, const char *msg);

#endif
