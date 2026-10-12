/*
 * ceval_internal.h — the evaluator's machine (ceval.h), shared by ceval.c (state, API, typer
 * callbacks, names, diagnostics), ceval_expr.c (expressions) and ceval_interp.c (calls and
 * statements); not for other users.
 *
 * Everything runs on explicit stacks, never C recursion (spec §4.6):
 *  - tasks: ev_task work items, popped and dispatched by ev_run through a handler table; a
 *    handler pushes the tasks that follow it (an expression's operands before its operator, a
 *    loop's body before its next test);
 *  - values: the odin3_cval results of expressions (width 0 = the empty value of {0{…}});
 *  - frames: odin3_ceval_frame, a base frame per API call (and per nested range evaluation) and
 *    one per interpreted function call, each with its locals and two arenas;
 *  - locals: every frame's variables and localparams, a frame's from its `locals` index;
 *  - roots: the expressions typed and still being evaluated (re-typed when a call returns into
 *    their frame: a recursive call at the same level types the same nodes in another frame).
 * The only re-entry is through the typer's callbacks (function pointers): typing a replication
 * count, a part-select bound or a function's return range evaluates it with a nested ev_run on
 * the stacks above the caller's. A host's own re-entry (a lazy parameter through lookup, Task 11;
 * a store callback) nests the same way. Both count against the call-depth cap with the function
 * frames (the 2D Task 9 ruling; final review, finding 1), which bounds the C stack: about
 * 2.5 KiB a level under ASan for a nested run (1024 levels, a return range calling itself, need
 * about 3 MiB of the 8 MiB default) and about 4 KiB for a re-entry through the test host (a
 * 1024-link parameter chain, about 4 MiB); less without ASan.
 *
 * Levels: every nested run (and every API call made from a host callback) raises the level, and
 * each level has its own typer. A frame types and reads types through the typer of the level it
 * was pushed at, so a run nested inside a typing still in progress (a count calling the same
 * function) never overwrites the entries that typing is building (fix round 1, finding 1).
 */
#ifndef ODIN3_FRONTENDS_ELAB_CEVAL_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_CEVAL_INTERNAL_H

#include "ast/ast.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Task ops; ev_run dispatches through ev_handlers (ceval.c). */
enum {
    /* expressions: ceval_expr.c */
    EV_EXPR,     /* node (typed): push its value, or push the tasks that compute it */
    EV_OP,       /* UNARY, BINARY, CONCAT, REPLICATE: the operands are on the stack */
    EV_TERN,     /* the condition is on the stack: evaluate a branch, or both */
    EV_MERGE,    /* both branches are on the stack (x condition) */
    EV_SEL,      /* an ordinary select: object, then index values, on the stack */
    EV_WORD,     /* a memory word: its index values on the stack */
    EV_SYS,      /* $signed, $unsigned, $clog2: the argument on the stack */
    EV_ROOT,     /* type node in context (a = width, sign), then evaluate it */
    EV_ROOT_END, /* node's value is on the stack: close its root */
    /* calls and statements: ceval_interp.c */
    EV_CALL,      /* a user function call: push its frame and declarations */
    FN_RESULT,    /* the result variable (phase 1: its range values on the stack) */
    FN_DECL,      /* a PORT_DECL/VAR_DECL/PARAM_DECL of frame (phase 1: range values) */
    FN_PARAM,     /* a localparam's value (phase 1: on the stack) */
    FN_BIND,      /* the arguments of a PORT_DECL's formals (b = first formal) */
    FN_SET,       /* the value on the stack goes to local a of frame */
    FN_RETURN,    /* the body is done: pop the frame, push the result */
    ST_STMT,      /* a statement */
    ST_ASSIGN,    /* phase 1: the right side is on the stack */
    ST_STORE,     /* the right side, then the lvalue's index values, are on the stack */
    ST_IF,        /* phase 1: the condition is on the stack */
    ST_CASE,      /* phase 1: the case value; phase 2: an item value above it */
    ST_LOOP,      /* FOR / WHILE / REPEAT */
    ST_BLOCK_END, /* a named block's locals go out of scope */
    EV_OPS
};

/* One work item (copied out of the stack before its handler runs). */
typedef struct ev_task {
    uint8_t op;    /* EV_* / FN_* / ST_* */
    uint8_t phase; /* per op */
    uint8_t sign;  /* EV_ROOT: the context's sign; ST_CASE: the case sign */
    uint8_t spare;
    uint32_t node;  /* the AST node */
    uint32_t frame; /* the frame whose names the node reads (its scope) */
    uint32_t a;     /* per op: a width, a local, an item */
    uint32_t b;     /* per op: a formal, an expression index */
    int64_t n;      /* per op: an iteration count, a range bound */
    int64_t m;      /* per op: a range bound */
} ev_task;

/*
 * A frame (spec §4.6's odin3_ceval_frame): a base frame (decl 0) for each API call and each
 * nested range evaluation, or one interpreted function call. Its cursor stack is the task stack
 * above `tasks`; its locals start at `locals`.
 */
typedef struct odin3_ceval_frame {
    odin3_ast_id decl;              /* the FUNCTION_DECL; 0 for a base frame */
    odin3_ast_id call;              /* the CALL being interpreted; 0 for a base frame */
    uint32_t locals;                /* index of its first local */
    uint32_t values;                /* the value stack's length when it was pushed */
    uint32_t tasks;                 /* the task stack's length when it was pushed */
    odin3_arena *mem;               /* its locals' values */
    odin3_arena *tmp;               /* its expression temporaries, renewed between statements */
    const odin3_ceval_store *store; /* exec's base frame: the module's variables; else NULL */
    uint32_t level;                 /* the nesting level it was pushed at: its typer */
    bool dirty; /* a frame of the same function and level typed its nodes since: re-type them */
} odin3_ceval_frame;

/* A function's (or block's) variable, port, result or localparam. */
typedef struct ev_local {
    uint32_t name;    /* strtab ID */
    uint32_t decl;    /* its DECLARATOR (the FUNCTION_DECL for the result) */
    bool is_const;    /* a localparam: read-only */
    bool visible;     /* false once its block has ended */
    odin3_etype type; /* declared width and sign */
    int64_t msb, lsb; /* declared range */
    odin3_cval value; /* planes in its frame's mem arena (localparam: as evaluated) */
} ev_local;

/* An expression typed and still being evaluated. */
typedef struct ev_root {
    uint32_t node;
    uint32_t frame;
    odin3_etype context;
    bool typing; /* odin3_ctype_expr is running on it: never re-typed then */
} ev_root;

struct odin3_ceval {
    odin3_ast *ast;
    odin3_ceval_host host;
    odin3_vec typers; /* odin3_ctype *, one per level; typers[0] is odin3_ceval_types' */
    uint32_t cur;     /* the level of the typer running (its constant callback's <what>) */
    odin3_vec tasks;  /* ev_task */
    odin3_vec values; /* odin3_cval */
    odin3_vec frames; /* odin3_ceval_frame */
    odin3_vec locals; /* ev_local */
    odin3_vec roots;  /* ev_root */
    odin3_vec word;   /* int64_t: the indices of the memory word of the store call running */
    uint32_t depth;   /* API calls and callback runs active */
    uint32_t nest;    /* callback runs inside an active evaluation (C nesting, counted) */
    uint32_t level;   /* nested runs and re-entrant API calls: the typer level */
    uint32_t scope;   /* the frame whose names the typer's leaf callback resolves */
    uint32_t clog2;   /* $clog2 arguments being evaluated (the <what> becomes theirs) */
    bool quiet;       /* try / exec: no diagnostic for non-constant input */
    bool nonconst;    /* a non-constant input stopped the evaluation */
    const char *what; /* §9.1's <what> of the evaluation running */
    uint64_t steps;   /* statements and expression nodes of the outermost evaluation */
    uint64_t work;    /* word products of the outermost evaluation's * / % ** */
    uint64_t fresh;   /* bytes of values made (estimated) since the last byte-budget check */
};

/* A name resolved in a scope: a local of the scope frame, or the host's symbol. */
typedef struct ev_name {
    bool is_local;
    uint32_t local; /* index into ev->locals */
    odin3_csym sym; /* !is_local: the host's answer (CONST or SIGNAL) */
} ev_name;

typedef odin3_status (*ev_handler)(odin3_ceval *ev, const ev_task *task);

/* The handlers, by op (each file defines its own). */
odin3_status ev_do_expr(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_op(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_tern(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_merge(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_sel(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_word(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_sys(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_root(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_root_end(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_call(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_result(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_decl(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_param(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_bind(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_set(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_return(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_stmt(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_assign(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_store(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_if(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_case(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_loop(odin3_ceval *ev, const ev_task *task);
odin3_status ev_do_block_end(odin3_ceval *ev, const ev_task *task);

/* --- stacks (ceval.c) ---------------------------------------------------------------------- */

/* Pushes a task (NO_MEMORY). */
odin3_status ev_push(odin3_ceval *ev, ev_task task);

/* Pushes a copy of the value struct (its planes are not copied; NO_MEMORY). */
odin3_status ev_push_value(odin3_ceval *ev, const odin3_cval *val);

/* Pops the top value (the stack must hold one). */
odin3_cval ev_pop_value(odin3_ceval *ev);

/* The value `depth` below the top (0 = the top). */
const odin3_cval *ev_peek_value(const odin3_ceval *ev, size_t depth);

odin3_ceval_frame *ev_frame(const odin3_ceval *ev, uint32_t idx);
ev_local *ev_local_at(const odin3_ceval *ev, uint32_t idx);

/* The arena for temporaries of an expression read in frame idx. */
odin3_arena *ev_tmp(const odin3_ceval *ev, uint32_t idx);

/* Pushes a frame (arenas made here) for decl/call; its index in *idx. NO_MEMORY. */
odin3_status ev_frame_push(odin3_ceval *ev, odin3_ast_id decl, odin3_ast_id call, uint32_t *idx);

/* Pops the top frame: its arenas freed, its locals dropped. */
void ev_frame_pop(odin3_ceval *ev);

/* The function frames on the stack (base frames do not count). */
uint32_t ev_function_frames(const odin3_ceval *ev);

/* --- names, types, values (ceval.c) -------------------------------------------------------- */

/* The typer of frame idx's level (typers[0] for an index past the stack). */
odin3_ctype *ev_types(const odin3_ceval *ev, uint32_t idx);

/*
 * Resolves the IDENT or HIER_NAME ref in frame scope: a visible local of that frame, else the
 * host's CONST or SIGNAL. A name the host does not know is reported (`'x' is not declared`);
 * a host failure returns its status.
 */
odin3_status ev_resolve(odin3_ceval *ev, uint32_t scope, odin3_ast_id ref, ev_name *out);

/* The local of frame `frame` declared by DECLARATOR decl (visible or not); UINT32_MAX if none. */
uint32_t ev_find_local(const odin3_ceval *ev, uint32_t frame, odin3_ast_id decl);

/* The innermost SELECT chain's name (an IDENT or HIER_NAME), and the chain's depth. */
odin3_ast_id ev_select_base(const odin3_ast *ast, odin3_ast_id node, uint32_t *depth);

/*
 * Types root (odin3_ctype_expr in context, names in frame scope) and opens it as a root (so a
 * call returning into frame re-types it). The root stays open until ev_root_close.
 */
odin3_status ev_root_open(odin3_ceval *ev, uint32_t scope, odin3_ast_id root, odin3_etype context);

/* Closes the top root (node must be it). */
void ev_root_close(odin3_ceval *ev, odin3_ast_id node);

/* Re-types the open roots of frame scope (after a call returned into it). */
odin3_status ev_retype(odin3_ceval *ev, uint32_t scope);

/*
 * Brings *val to type t in arena: a wider t extends it (UNSIZED repeats its top bit, otherwise
 * sign or zero extension by t.is_signed; STRING kept when zero-extended to whole bytes), a
 * narrower t truncates it, the same width only sets SIGNED; the empty value becomes t.width
 * zeros (t.width 0 keeps it empty). *val may alias a host's or a local's value: a new value is
 * made whenever anything changes.
 */
odin3_status ev_fit(odin3_arena *arena, odin3_cval *val, odin3_etype type);

/* A new t.width-bit value of t's sign with every bit `fill` (t.width 0: the empty value). */
odin3_status ev_filled(odin3_arena *arena, odin3_etype type, odin3_bit fill, odin3_cval *out);

/* Counts one step (a statement or an expression node) against the budget; the cap's error. */
odin3_status ev_step(odin3_ceval *ev, odin3_ast_id node);

/* Counts `count` steps at once (typing work the budget must see); the cap's error at node. */
odin3_status ev_steps(odin3_ceval *ev, odin3_ast_id node, uint64_t count);

/* Counts `products` word products of a * / % ** against the work budget (finding 4). */
odin3_status ev_work(odin3_ceval *ev, odin3_ast_id node, uint64_t products);

/* The byte budget (§11's ODIN3_ELAB_MAX_BYTES, the hook's max_bytes) against the bytes the
 * evaluator holds (its arenas, stacks and every level's typer): `elaboration needs more than N
 * bytes (…)` at node (finding 5). Checked at frame push, local declaration, every 4096 steps and
 * after every 1 MiB of values made (ev_bytes_note). */
odin3_status ev_bytes_check(odin3_ceval *ev, odin3_ast_id node);

/* Notes the values an expression node of `width` bits may make (its operands' extensions, its
 * result and its fit) and checks the byte budget once 1 MiB has been made since the last check:
 * a statement-free expression of wide operators cannot overshoot it by more than that (final
 * review, finding 5). */
odin3_status ev_bytes_note(odin3_ceval *ev, odin3_ast_id node, uint32_t width);

/* The nodes of node's subtree (an explicit stack; NO_MEMORY), for ev_steps. */
odin3_status ev_subtree_size(const odin3_ceval *ev, odin3_ast_id node, uint64_t *out);

/* --- diagnostics (ceval.c) ----------------------------------------------------------------- */

/* Reports a located error (always) and returns PARSE. */
odin3_status ev_error(const odin3_ceval *ev, odin3_ast_id node, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/*
 * A non-constant input at node (a read of `name`, a strtab ID): in a function frame, the
 * function's `it reads module variable 'x'`; otherwise `<what> is not constant (it reads 'x')`.
 * Reported unless quiet; sets nonconst; returns PARSE.
 */
odin3_status ev_nonconst_read(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, uint32_t name);

/*
 * `function 'f' is not a constant function: <reason>` for the function of frame scope, the
 * reason a printf format with one %s (the name given). Reported unless quiet; sets nonconst;
 * returns PARSE.
 */
odin3_status ev_nonconst_fn(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, const char *reason,
                            const char *name);

/* The strtab string of id. */
const char *ev_str(const odin3_ceval *ev, uint32_t id);

/* --- shared with ceval_interp.c ------------------------------------------------------------ */

/*
 * The type and range of a declaration with flags/DT (a PORT_DECL, VAR_DECL, PARAM_DECL or
 * FUNCTION_DECL) whose range, if any, evaluated to msb and lsb: integer 32 signed, time 64,
 * a range |msb - lsb| + 1 bits, else 1 bit; signed by the SIGNED flag. The width cap's error at
 * node; `variable type 'real' is not supported` for real/realtime.
 */
typedef struct ev_decl_type {
    odin3_ast_id node; /* the declaration */
    bool has_range;
    int64_t msb, lsb; /* when has_range */
} ev_decl_type;

odin3_status ev_decl_type_of(const odin3_ceval *ev, const ev_decl_type *req, odin3_etype *type,
                             int64_t *msb, int64_t *lsb);

/* The value on top of the stack as an integer (x/z: *known false; a known value outside
 * int64_t saturates, far outside any width); pops it. */
void ev_pop_int(odin3_ceval *ev, int64_t *out, bool *known);

/*
 * Whether the SELECT node (in frame scope) is a memory word: its base name is not a local, and
 * the host's lookup of the SELECT gives SIGNAL or CONST (*out; kind NONE otherwise). The same
 * question the typer's leaf callback asks.
 */
odin3_status ev_word_sym(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, odin3_csym *out);

/* A select's index values and its object's declared range. */
typedef struct ev_sel_req {
    uint32_t sub;   /* ODIN3_AST_SELECT_* */
    int64_t first;  /* the index, msb or base */
    int64_t second; /* lsb or width (not for a bit select) */
    int64_t msb, lsb;
} ev_sel_req;

/* The indices it covers as written (left, right) and its bit positions (lowest, width). */
typedef struct ev_span {
    int64_t left, right;
    int64_t lo;
    uint64_t width;
} ev_span;

/* A select's span (+: and -: follow the declaration's direction); far indices are clamped. */
ev_span ev_select_span(const ev_sel_req *req);

#endif
