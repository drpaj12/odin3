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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * An open header group (Task 4): the ANSI port or header parameter declaration whose node is
 * made once its last declarator is known (at the next direction or keyword, or at the `)`).
 * mark: where its attributes start on the pending stack (its slots and declarators follow);
 * spec: its VP_SPEC with IN_HEADER; at: from its first token to its last declarator's end.
 */
typedef struct odin3_vp_group {
    bool open;
    uint32_t mark;
    uint32_t spec;
    odin3_vp_loc at;
} odin3_vp_group;

/* The pieces of one literal at most (§2.3: size, base, digits). */
enum { ODIN3_VP_MAX_PIECES = 3 };

/*
 * The parser context, one per stream (§3.2). span_scratch/part_scratch: the copy of a pending
 * span and its partitioned prefix during one reordering reduction; meta_scratch: the ATTR nodes
 * of one case's metacomments; headers: odin3_vp_header records of function and task headers;
 * windows: odin3_vp_window metacomment windows of case headers (headers and windows are indexed by
 * a `u` value, so vp_* arity stays at five); error_mode: actions are inert (§3.4); fatal: the
 * status that aborted the parse. Task 4: base, the pending stack's height when the stream began
 * (the first error unwinds to it); group, the open header group; pieces/npieces, the byte views
 * of the literal being reduced (vp_piece, vp_number).
 */
typedef struct odin3_vp_ctx {
    odin3_vl_run *run;
    odin3_ast *ast;
    odin3_vl_lexer *lexer;
    odin3_vec span_scratch, part_scratch, meta_scratch, headers, windows;
    bool error_mode;
    odin3_status fatal;
    uint32_t base;
    odin3_vp_group group;
    odin3_bytes pieces[ODIN3_VP_MAX_PIECES];
    uint32_t npieces;
} odin3_vp_ctx;

/* The generated parser's lex parameter (%lex-param): the context's scanner. */
#define ODIN3_VP_SCANNER odin3_vl_scanner(ctx->lexer)

/*
 * Bison's stacks and its message buffer (YYSTACK_ALLOC with YYSTACK_USE_ALLOCA 0) go through the
 * allocation hook, and a failure is recorded, so "memory exhausted" tells a real NO_MEMORY from
 * the YYMAXDEPTH cap (Task 1 review M6; parser.c).
 */
#define YYMALLOC odin3_vp_stack_alloc
#define YYFREE odin3_vp_stack_free

/* VP(out, call): the ordinary action. Inert in error mode (out zeroed: memset, since a node
 * value is the struct odin3_ast_id, which `(out) = 0` cannot assign); a non-OK status is fatal. */
#define VP(out, call)                                                                              \
    do {                                                                                           \
        if (ctx->error_mode) {                                                                     \
            memset(&(out), 0, sizeof(out));                                                        \
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
 * attributes (actions.c: shared; actions_decl.c: declarations) ---------------------------------
 *
 * A spec is one `u` value holding a node's kind (bits 0-7), sub-kind (8-15) and flags (16-31), so
 * an item rule can accumulate it while it pushes the node's slots; flag values OR into it. Every
 * mapping from a keyword to a 2A enum is written here or in the grammar with the enum's name;
 * no token code is ever stored (§3.7).
 */
enum { ODIN3_VP_SPEC_SUB_SHIFT = 8, ODIN3_VP_SPEC_FLAGS_SHIFT = 16, ODIN3_VP_SPEC_BYTE = 0xFF };
#define VP_SPEC(kind, sub) ((uint32_t)(kind) | (uint32_t)(sub) << ODIN3_VP_SPEC_SUB_SHIFT)
#define VP_FLAGS(flags) ((uint32_t)(flags) << ODIN3_VP_SPEC_FLAGS_SHIFT)
#define VP_NO_FLAGS 0U
/* SIGNED is bit 0 of PORT_DECL, NET_DECL, VAR_DECL and PARAM_DECL alike (actions_decl.c asserts
 * it), so one signed_opt serves them all. */
#define VP_SIGNED VP_FLAGS(ODIN3_AST_F_PORT_DECL_SIGNED)
#define VP_PORT_DT(dt) VP_FLAGS((uint32_t)(dt) << ODIN3_AST_F_PORT_DECL_DT_SHIFT)
#define VP_PARAM_DT(dt) VP_FLAGS((uint32_t)(dt) << ODIN3_AST_F_PARAM_DECL_DT_SHIFT)

/* actions.c — values, lists and slots on the pending stack. */

/* *out = node 0 (an absent optional slot, an inert alternative). */
odin3_status vp_none(odin3_vp_ctx *ctx, odin3_ast_id *out);
/* *out = node (a parenthesised expression is the expression itself). */
odin3_status vp_pass(odin3_vp_ctx *ctx, odin3_ast_id node, odin3_ast_id *out);
/* *out = left | right (two specs or flag sets). */
odin3_status vp_or(odin3_vp_ctx *ctx, uint32_t left, uint32_t right, uint32_t *out);
/* A list's first entry: *out = the mark below it; node is pushed. NO_MEMORY. */
odin3_status vp_list_first(odin3_vp_ctx *ctx, odin3_ast_id node, uint32_t *out);
/* A further entry: node is pushed; *out = list (the list's mark). NO_MEMORY. */
odin3_status vp_list_next(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id node, uint32_t *out);
/* Two entries at once (either may be 0): *out = the mark below them. NO_MEMORY. */
odin3_status vp_list_two(odin3_vp_ctx *ctx, odin3_ast_id first, odin3_ast_id second, uint32_t *out);
/* An absent slot: pushes 0; *out = the mark below it. NO_MEMORY. */
odin3_status vp_push_none(odin3_vp_ctx *ctx, uint32_t *out);
/* A slot: pushes slot (0 when absent); *out = the mark below it. NO_MEMORY. */
odin3_status vp_push_slot(odin3_vp_ctx *ctx, odin3_ast_id slot, uint32_t *out);
/* A spec's first slot: pushes slot; *out = spec. NO_MEMORY. */
odin3_status vp_spec_slot(odin3_vp_ctx *ctx, uint32_t spec, odin3_ast_id slot, uint32_t *out);
/* A childless node of spec over at (STRENGTH). NO_MEMORY. */
odin3_status vp_leaf(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec, odin3_ast_id *out);

/* actions.c — attributes and items (§3.5, point (g)). */

/* Attaches every ATTR pushed since mark to node in one attach (instances merge), pops them, and
 * *out = node. NO_MEMORY. */
odin3_status vp_attrs(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id node, odin3_ast_id *out);
/* An item: the node of spec over at from what was pushed above the ATTRs since mark, the ATTRs
 * attached, the stack back at mark. NO_MEMORY; PARSE (make's caps, located). */
odin3_status vp_item(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                     odin3_ast_id *out);
/* vp_attrs, then node pushed; *out = mark. NO_MEMORY. */
odin3_status vp_item_push(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id node, uint32_t *out);
/* An item list's entry: node is pushed unless 0 (the item pushed itself); *out = list. */
odin3_status vp_item_next(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id node, uint32_t *out);
/* ATTR(key name; no value) over at; ATTR(name; value). NO_MEMORY. */
odin3_status vp_attr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, odin3_ast_id *out);
odin3_status vp_attr_value(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                           odin3_ast_id value, odin3_ast_id *out);

/* actions.c — literals and leaves (§2.3, §3.6 expressions). */

/* A literal's next piece (UNSIGNED_NUMBER, BASE or BASED_DIGITS, a view into the stream): *out =
 * the pieces mark below it. */
odin3_status vp_piece(odin3_vp_ctx *ctx, odin3_bytes text, uint32_t *out);
/* The literal of the pieces from mark (a plain decimal; base and digits; size, base and digits)
 * over at: odin3_ast_number_parse, then odin3_ast_number_kind for the NUMBER's sub and SIGNED.
 * number_parse's located errors are counted recoverable errors (error mode, *out 0). NO_MEMORY;
 * PARSE (make's caps). */
odin3_status vp_number(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, odin3_ast_id *out);
/* A base without digits (VP_ERR): "expected digits after '<base as written>" at base, then error
 * mode. */
odin3_status vp_digitless(odin3_vp_ctx *ctx, const odin3_vp_loc *base);
/* REAL (the lexer's payload; 0 after its range error), STRING and IDENT leaves over at. */
odin3_status vp_real(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t payload,
                     odin3_ast_id *out);
odin3_status vp_string(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, odin3_ast_id *out);
odin3_status vp_ident(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, odin3_ast_id *out);

/* actions.c — A.8.5 names and selects (§3.1 h, §3.6). A hier_ref's value is the mark of its
 * components (IDENT, or a bit SELECT over one) on the pending stack. */

/* The first component: an IDENT over at pushed; *out = the mark below it. */
odin3_status vp_hier_first(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, uint32_t *out);
/* A further component: an IDENT over at pushed; *out = mark. */
odin3_status vp_hier_next(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          uint32_t *out);
/* `[ index ]` on the last component (G7): it becomes SELECT[bit](IDENT, index), ending at at's
 * end; *out = mark. */
odin3_status vp_hier_index(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id index, uint32_t *out);
/* The name from mark: its one IDENT, or a HIER_NAME of its components (first to last). */
odin3_status vp_hier(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id *out);
/* SELECT[bit](the name from mark, index) over at. */
odin3_status vp_select_hier(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id index, odin3_ast_id *out);
/* A part select's opening: the name from mark and its first bound pushed; *out = mark. */
odin3_status vp_select_open(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id first, uint32_t *out);
/* SELECT of the given kind over at: the base (and first bound) pushed from mark, then last. */
odin3_status vp_select_bit(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id last, odin3_ast_id *out);
odin3_status vp_select_part(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id last, odin3_ast_id *out);
odin3_status vp_select_plus(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id last, odin3_ast_id *out);
odin3_status vp_select_minus(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                             odin3_ast_id last, odin3_ast_id *out);
/* CONCAT(E+) of the entries pushed from mark, over at. */
odin3_status vp_concat(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id *out);

/* actions_decl.c — units, modules and header lists (§3.6 "Units and directives", "Modules and
 * ports"). */

/* UDP_DECL / CONFIG_DECL (named by the lexer's first identifier after the keyword) or
 * SPECIFY_BLOCK, kind, over at with body as its TEXT (§2.8). NO_MEMORY. */
odin3_status vp_opaque(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_bytes body, uint32_t kind,
                       odin3_ast_id *out);
/* DIRECTIVE[kind of the token at at->off] named by its trimmed argument (none when empty). */
odin3_status vp_directive(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_bytes arg,
                          odin3_ast_id *out);
/* MODULE / MODULE{MACROMODULE} named name over at, its children pushed from mark; endmodule
 * empties the lexer's pending metacomments (§2.6). */
odin3_status vp_module(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                       odin3_ast_id *out);
odin3_status vp_macromodule(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                            odin3_ast_id *out);
/* A header LIST over at of the entries from mark, pushed as the MODULE's slot; *out = mark.
 * vp_group_list_slot closes the open group first. */
odin3_status vp_list_slot(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t *out);
odin3_status vp_group_list_slot(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark,
                                uint32_t *out);
/* Header groups (odin3_vp_group): close the open one (its node made, attached and pushed);
 * open one of spec at at with its attributes from mark; push its range and first declarator;
 * join a further declarator. *out = 0 / mark / mark / list. */
odin3_status vp_group_close(odin3_vp_ctx *ctx, uint32_t *out);
odin3_status vp_group_open(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                           uint32_t *out);
odin3_status vp_group_first(odin3_vp_ctx *ctx, odin3_ast_id range, odin3_ast_id decl,
                            uint32_t *out);
odin3_status vp_group_join(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id decl, uint32_t *out);
/* PORT_REF forms (§3.6): the empty port; a; {…} or d[3:0] (name 0 with the expression); .x();
 * .x(e). */
odin3_status vp_port_empty(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id *out);
odin3_status vp_port_name(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out);
odin3_status vp_port_expr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id expr,
                          odin3_ast_id *out);
odin3_status vp_port_dotted(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                            odin3_ast_id *out);
odin3_status vp_port_dotted_expr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                                 odin3_ast_id expr, odin3_ast_id *out);
/* A PORT_DECL's NET field (1 + the net kind of net_spec, a NET_DECL spec) as spec flags. */
odin3_status vp_port_net(odin3_vp_ctx *ctx, uint32_t net_spec, uint32_t *out);

/* actions_decl.c — declarators, ranges, strengths, delays, assignments (§3.6). */

/* DECLARATOR(name; no initialiser; no dimension) / (name; init) / (name; 0; the dimensions
 * pushed from mark) over at. */
odin3_status vp_decl_name(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out);
odin3_status vp_decl_init(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id init, odin3_ast_id *out);
odin3_status vp_decl_dims(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out);
/* The first dimension: the absent initialiser slot and range pushed; *out = the mark below. */
odin3_status vp_dims_first(odin3_vp_ctx *ctx, odin3_ast_id range, uint32_t *out);
/* RANGE(msb, lsb) over at. */
odin3_status vp_range(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id msb, odin3_ast_id lsb,
                      odin3_ast_id *out);
/* DEFPARAM(HIER_NAME of the name from mark, value) over at. */
odin3_status vp_defparam(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                         odin3_ast_id value, odin3_ast_id *out);
/* A drive strength for 0 (S0) or for 1 (S1), value an odin3_ast_drive_value: a STRENGTH[drive]
 * spec. */
odin3_status vp_drive0(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out);
odin3_status vp_drive1(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out);
/* STRENGTH[charge]{S0 = value} over at. */
odin3_status vp_charge(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t value,
                       odin3_ast_id *out);
/* DELAY(value) over at; DELAY of the two values pushed from mark; of those and last. */
odin3_status vp_delay1(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id value,
                       odin3_ast_id *out);
odin3_status vp_delay_marked(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                             odin3_ast_id *out);
odin3_status vp_delay_last(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id last, odin3_ast_id *out);
/* MINTYPMAX(min, typ pushed from mark; max = last) over at. */
odin3_status vp_mintypmax(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                          odin3_ast_id last, odin3_ast_id *out);
/* NET_ASSIGN(lhs, rhs) over at. */
odin3_status vp_net_assign(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                           odin3_ast_id rhs, odin3_ast_id *out);

/* --- Task 5: expressions and primaries (actions_expr.c) ------------------------------------ */

/* --- Task 6: statements, timing, functions and tasks, case and metacomments
 * (actions_stmt.c) ------------------------------------------------------------------------- */

/* --- Task 7: instantiation, gates, generate (actions_inst.c) ------------------------------- */

/* --- parser.c: called by the generated parser ---------------------------------------------- */

/*
 * YYLLOC_DEFAULT (§2.2): rhs[1..n] are the right-hand symbols' locations, rhs[0] the symbol's
 * before them. loc and off come from the first non-empty symbol (loc != end), end from the last
 * non-empty one. An empty rule (n 0) gives {rhs[0].end, rhs[0].end, rhs[0].off}; a rule of only
 * empty symbols gives {rhs[1].loc, rhs[1].loc, rhs[1].off} and never reads rhs[0] (Bison's
 * error recovery passes yyerror_range, whose [0] is unset).
 */
void odin3_vp_lloc(odin3_vp_loc *cur, const odin3_vp_loc *rhs, int n);

/* YYMAXDEPTH (plan point (d)): the stack cap the driver set from the run's limits. */
long odin3_vp_yymaxdepth(void);

/* YYMALLOC / YYFREE: odin3_util_malloc / odin3_util_free; a failed allocation is recorded for
 * odin3_vp_error, so the following "memory exhausted" is a fatal NO_MEMORY (Task 1 review M6). */
void *odin3_vp_stack_alloc(size_t bytes);
void odin3_vp_stack_free(void *ptr);

/* The first error of a stream (syntax, literal or digit-less base): error mode, and the pending
 * stack unwound to the stream's base (§3.4). */
void odin3_vp_enter_error(odin3_vp_ctx *ctx);

/*
 * yyerror (§3.4, plan point (e)): enters error mode (odin3_vp_enter_error) and reports Bison's
 * message at *at with "syntax error, " replaced by "syntax: "; "memory exhausted" is a fatal
 * NO_MEMORY (silent, the run stopped) after a failed YYMALLOC, else the stack cap (§5): the
 * located fatal "nesting deeper than the parser allows (<cap>)", which stops the run. Silent
 * when ctx->fatal is already set.
 */
void odin3_vp_error(const odin3_vp_loc *at, odin3_vp_ctx *ctx, const char *msg);

#endif
