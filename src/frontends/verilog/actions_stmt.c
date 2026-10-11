/*
 * actions_stmt.c — the statement actions of 2C Task 6 (spec §3.1 (e), (f), (h), §3.2, §3.5,
 * §3.6 "Procedural blocks and timing", "if, case", "Loops", "Functions and tasks"; the 2A slot
 * table §4.3): procedural items, statements, timing controls, the three reordering reductions
 * (CASE_ITEM, SEQ_BLOCK/PAR_BLOCK, FUNCTION_DECL/TASK_DECL) through the context's two scratch
 * vectors, and the case metacomments (§2.6).
 *
 * The scratch vectors serve one reduction at a time: an inner block, case item or function has
 * reduced before the outer one starts, so nesting is safe (§3.2).
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/verilog/actions_internal.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One automatic_opt serves functions and tasks (VP_FLAGS of FUNCTION_DECL's bit). */
_Static_assert(ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC == ODIN3_AST_F_TASK_DECL_AUTOMATIC,
               "2A §4.4: AUTOMATIC is bit 0 of FUNCTION_DECL and TASK_DECL");

/* The most children a node made here has (FUNCTION_DECL: return, ports, locals, body). */
enum { VP_MAX_STMT_KIDS = 4 };

static const char DECL_AFTER_STMT[] = "declaration after a statement in block";

/* A node of spec over at with the children kids[0..n). */
static odin3_status make_n(odin3_vp_ctx *ctx, const odin3_ast_spec *spec, const odin3_ast_id *kids,
                           uint32_t n, odin3_ast_id *out) {
    return odin3_ast_make(ctx->ast, spec, kids, n, out);
}

/* A node of spec (VP_SPEC) over at with the two children first and second. */
static odin3_status make_two(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec,
                             const odin3_ast_id kids[2], odin3_ast_id *out) {
    odin3_ast_spec node = vp_spec_at(spec, at);
    return make_n(ctx, &node, kids, 2, out);
}

/* A node of spec over at with the one child kid. */
static odin3_status make_one(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec,
                             odin3_ast_id kid, odin3_ast_id *out) {
    odin3_ast_spec node = vp_spec_at(spec, at);
    return make_n(ctx, &node, &kid, 1, out);
}

/* A recoverable error of a reordering reduction: reported at loc, counted, error mode. */
static odin3_status located_error(odin3_vp_ctx *ctx, odin3_loc loc, const char *what,
                                  odin3_ast_id *out) {
    odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, loc, "%s", what);
    odin3_vp_enter_error(ctx);
    *out = (odin3_ast_id){0};
    return ODIN3_OK;
}

/* --- scratch vectors (odin3_ast_id entries) --------------------------------------------- */

static odin3_status scratch_push(odin3_vec *vec, odin3_ast_id id) {
    odin3_ast_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    return ODIN3_OK;
}

/* vec = the pending span from mark (after lead, if any, already in vec), then the stack unwound
 * to mark. */
static odin3_status scratch_take(odin3_vp_ctx *ctx, uint32_t mark, odin3_vec *vec) {
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    if (odin3_vec_reserve(vec, vec->len + span.n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < span.n; i++) {
        odin3_status st = scratch_push(vec, span.ids[i]); /* reserved: cannot fail */
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_ast_unwind(ctx->ast, mark);
    return ODIN3_OK;
}

static const odin3_ast_id *scratch_ids(const odin3_vec *vec) {
    return (const odin3_ast_id *)vec->data;
}

/* Pushes ids[0..n) on the pending stack. */
static odin3_status push_all(odin3_vp_ctx *ctx, const odin3_ast_id *ids, uint32_t n) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < n && st == ODIN3_OK; i++) {
        st = odin3_ast_push(ctx->ast, ids[i]);
    }
    return st;
}

/* A LIST of ids[0..n) from its first element's start to its last one's end; empty, the zero-width
 * range at empty (§3.6, Task 2 default 3: an empty list sits one past the preceding token). */
static odin3_status list_of(odin3_vp_ctx *ctx, const odin3_ast_id *ids, uint32_t n, odin3_loc empty,
                            odin3_ast_id *out) {
    odin3_vp_loc at = {empty, empty, 0};
    if (n > 0) {
        at.loc = odin3_ast_loc(ctx->ast, ids[0]);
        at.end = odin3_ast_end(ctx->ast, ids[n - 1]);
    }
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_LIST, 0), &at);
    return make_n(ctx, &spec, ids, n, out);
}

static bool in_class(const odin3_ast *ast, odin3_ast_id node, odin3_ast_class cls) {
    return odin3_ast_class_has(cls, odin3_ast_kind_info_of(odin3_ast_kind_of(ast, node)));
}

/* --- procedural items and blocks ------------------------------------------------------------ */

odin3_status vp_always(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id stmt,
                       odin3_ast_id *out) {
    return make_one(ctx, at, VP_SPEC(ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS), stmt, out);
}

odin3_status vp_initial(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id stmt,
                        odin3_ast_id *out) {
    return make_one(ctx, at, VP_SPEC(ODIN3_AST_INITIAL, 0), stmt, out);
}

odin3_status vp_null(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_NULL_STMT, 0), at);
    odin3_status st = make_n(ctx, &spec, NULL, 0, out);
    return st == ODIN3_OK ? vp_attrs(ctx, mark, *out, out) : st;
}

odin3_status vp_null_opt(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                         odin3_ast_id *out) {
    if (odin3_ast_mark(ctx->ast) == mark) { /* a bare `;`: the S? slot stays absent */
        *out = (odin3_ast_id){0};
        return ODIN3_OK;
    }
    return vp_null(ctx, mark, at, out);
}

/* SEQ_BLOCK / PAR_BLOCK (§3.2): the span copied, the leading D-class items made the locals LIST
 * (or slot 0), the statements pushed back, the block made from mark. */
static odin3_status make_block(odin3_vp_ctx *ctx, uint32_t mark, const odin3_ast_spec *spec,
                               odin3_ast_id *out) {
    odin3_vec_clear(&ctx->span_scratch);
    odin3_status st = scratch_take(ctx, mark, &ctx->span_scratch);
    const odin3_ast_id *items = scratch_ids(&ctx->span_scratch);
    uint32_t count = (uint32_t)ctx->span_scratch.len;
    uint32_t decls = 0;
    while (st == ODIN3_OK && decls < count && in_class(ctx->ast, items[decls], ODIN3_AST_CLS_D)) {
        decls++;
    }
    for (uint32_t i = decls; st == ODIN3_OK && i < count; i++) {
        if (in_class(ctx->ast, items[i], ODIN3_AST_CLS_D)) {
            return located_error(ctx, odin3_ast_loc(ctx->ast, items[i]), DECL_AFTER_STMT, out);
        }
    }
    odin3_ast_id locals = {0};
    if (st == ODIN3_OK && decls > 0) {
        st = list_of(ctx, items, decls, (odin3_loc){0}, &locals);
    }
    st = st == ODIN3_OK ? odin3_ast_push(ctx->ast, locals) : st;
    st = st == ODIN3_OK ? push_all(ctx, items + decls, count - decls) : st;
    return st == ODIN3_OK ? odin3_ast_make_marked(ctx->ast, spec, mark, out) : st;
}

odin3_status vp_make_block(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                           odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_SEQ_BLOCK, 0), at);
    spec.name = name;
    return make_block(ctx, mark, &spec, out);
}

odin3_status vp_make_fork(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_PAR_BLOCK, 0), at);
    spec.name = name;
    return make_block(ctx, mark, &spec, out);
}

/* --- statements ----------------------------------------------------------------------------- */

odin3_status vp_blocking(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id rhs,
                         odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, rhs);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_BLOCKING_ASSIGN, 0), out)
                          : st;
}

odin3_status vp_nonblocking(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id rhs, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, rhs);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_NONBLOCKING_ASSIGN, 0), out)
                          : st;
}

odin3_status vp_assign_plain(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                             odin3_ast_id rhs, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_BLOCKING_ASSIGN, 0), at);
    const odin3_ast_id kids[] = {lhs, {0}, rhs};
    return make_n(ctx, &spec, kids, sizeof kids / sizeof kids[0], out);
}

odin3_status vp_proc_assign(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                            odin3_ast_id rhs, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {lhs, rhs};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_PROC_CONT_ASSIGN, ODIN3_AST_PROC_CONT_ASSIGN_ASSIGN),
                    kids, out);
}

odin3_status vp_proc_force(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                           odin3_ast_id rhs, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {lhs, rhs};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_PROC_CONT_ASSIGN, ODIN3_AST_PROC_CONT_ASSIGN_FORCE),
                    kids, out);
}

odin3_status vp_proc_deassign(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                              odin3_ast_id *out) {
    const odin3_ast_id kids[] = {lhs, {0}};
    return make_two(ctx, at,
                    VP_SPEC(ODIN3_AST_PROC_CONT_ASSIGN, ODIN3_AST_PROC_CONT_ASSIGN_DEASSIGN), kids,
                    out);
}

odin3_status vp_proc_release(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                             odin3_ast_id *out) {
    const odin3_ast_id kids[] = {lhs, {0}};
    return make_two(ctx, at,
                    VP_SPEC(ODIN3_AST_PROC_CONT_ASSIGN, ODIN3_AST_PROC_CONT_ASSIGN_RELEASE), kids,
                    out);
}

odin3_status vp_if(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id other,
                   odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, other);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_IF, 0), out) : st;
}

odin3_status vp_for(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id body,
                    odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, body);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_FOR, 0), out) : st;
}

odin3_status vp_while(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id cond,
                      odin3_ast_id body, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {cond, body};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_WHILE, 0), kids, out);
}

odin3_status vp_repeat(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id cond,
                       odin3_ast_id body, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {cond, body};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_REPEAT, 0), kids, out);
}

odin3_status vp_forever(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id body,
                        odin3_ast_id *out) {
    return make_one(ctx, at, VP_SPEC(ODIN3_AST_FOREVER, 0), body, out);
}

odin3_status vp_wait(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id cond,
                     odin3_ast_id stmt, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {cond, stmt};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_WAIT, 0), kids, out);
}

odin3_status vp_timing(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id control,
                       odin3_ast_id stmt, odin3_ast_id *out) {
    const odin3_ast_id kids[] = {control, stmt};
    return make_two(ctx, at, VP_SPEC(ODIN3_AST_TIMING_STMT, 0), kids, out);
}

/* A statement of spec over at whose one child is the HIER_NAME of the name from mark. */
static odin3_status named_target(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                                 uint32_t spec, odin3_ast_id *out) {
    odin3_ast_id target = {0};
    odin3_status st = vp_name(ctx, mark, true, &target);
    return st == ODIN3_OK ? make_one(ctx, at, spec, target, out) : st;
}

odin3_status vp_disable(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                        odin3_ast_id *out) {
    return named_target(ctx, mark, at, VP_SPEC(ODIN3_AST_DISABLE, 0), out);
}

odin3_status vp_trigger(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                        odin3_ast_id *out) {
    return named_target(ctx, mark, at, VP_SPEC(ODIN3_AST_EVENT_TRIGGER, 0), out);
}

odin3_status vp_task_call(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_TASK_CALL, 0), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_task_call_hier(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                               uint32_t name, odin3_ast_id *out) {
    odin3_ast_spec spec =
        vp_spec_at(VP_SPEC(ODIN3_AST_TASK_CALL, 0) | VP_FLAGS(ODIN3_AST_F_TASK_CALL_HIER), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_task_call_system(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                                 uint32_t name, odin3_ast_id *out) {
    odin3_ast_spec spec =
        vp_spec_at(VP_SPEC(ODIN3_AST_TASK_CALL, 0) | VP_FLAGS(ODIN3_AST_F_TASK_CALL_SYSTEM), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

/* --- case and metacomments ------------------------------------------------------------------ */

odin3_status vp_case_head(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec,
                          odin3_ast_id expr, uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    odin3_vp_window *window = odin3_vec_push(&ctx->windows);
    if (window == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *window = (odin3_vp_window){at->off + 1U, 0, spec}; /* one past the `)` */
    return odin3_ast_push(ctx->ast, expr);
}

odin3_status vp_case_first(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id item,
                           uint32_t *out) {
    if (ctx->windows.len == 0) { /* every case_items follows its case_head */
        odin3_log(ODIN3_LOG_ERROR, "vp_case_first: no open case window");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (uint32_t)ctx->windows.len - 1U;
    odin3_vp_window *window = odin3_vec_at(&ctx->windows, *out);
    window->hi = at->off;
    return odin3_ast_push(ctx->ast, item);
}

/* The metacomment ATTR named key over the entry's range, pushed on the pending stack. */
static odin3_status push_meta_attr(odin3_vp_ctx *ctx, const char *key, const odin3_vl_meta *meta) {
    odin3_vp_loc at = {meta->loc, meta->end, meta->off};
    odin3_ast_spec spec =
        vp_spec_at(VP_SPEC(ODIN3_AST_ATTR, 0) | VP_FLAGS(ODIN3_AST_F_ATTR_METACOMMENT), &at);
    odin3_status st = odin3_ast_intern(ctx->ast, odin3_bytes_cstr(key), &spec.name);
    odin3_ast_id attr = {0};
    const odin3_ast_id no_value = {0};
    st = st == ODIN3_OK ? make_n(ctx, &spec, &no_value, 1, &attr) : st;
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, attr) : st;
}

/* The first taken entry carrying key, NULL when none does. */
static const odin3_vl_meta *meta_with(const odin3_vec *taken, uint8_t key) {
    for (size_t i = 0; i < taken->len; i++) {
        const odin3_vl_meta *meta = odin3_vec_cat(taken, i);
        if ((meta->keys & key) != 0) {
            return meta;
        }
    }
    return NULL;
}

/* Takes the metacomments of window and pushes their ATTRs, full_case then parallel_case, each
 * once (a canonical order, so the printed `// synopsys …` re-parses to the same attach). */
static odin3_status take_metacomments(odin3_vp_ctx *ctx, const odin3_vp_window *window) {
    odin3_vec_clear(&ctx->meta_scratch);
    odin3_status st = odin3_vl_meta_take(ctx->lexer, window->lo, window->hi, &ctx->meta_scratch);
    const odin3_vl_meta *full = meta_with(&ctx->meta_scratch, ODIN3_VL_META_FULL_CASE);
    const odin3_vl_meta *parallel = meta_with(&ctx->meta_scratch, ODIN3_VL_META_PARALLEL_CASE);
    if (st == ODIN3_OK && full != NULL) {
        st = push_meta_attr(ctx, "full_case", full);
    }
    if (st == ODIN3_OK && parallel != NULL) {
        st = push_meta_attr(ctx, "parallel_case", parallel);
    }
    return st;
}

odin3_status vp_make_case(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t window,
                          odin3_ast_id *out) {
    if (window >= ctx->windows.len) { /* case_items yields its case's window */
        odin3_log(ODIN3_LOG_ERROR, "vp_make_case: no case window %u", (unsigned)window);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vp_window taken = *(const odin3_vp_window *)odin3_vec_cat(&ctx->windows, window);
    while (ctx->windows.len > window) {
        odin3_vec_pop(&ctx->windows);
    }
    odin3_status st = vp_marked(ctx, mark, at, taken.spec, out);
    return st == ODIN3_OK ? take_metacomments(ctx, &taken) : st;
}

odin3_status vp_make_case_item(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark,
                               odin3_ast_id body, odin3_ast_id *out) {
    odin3_vec_clear(&ctx->span_scratch);
    odin3_status st = scratch_push(&ctx->span_scratch, body);
    st = st == ODIN3_OK ? scratch_take(ctx, mark, &ctx->span_scratch) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t count = (uint32_t)ctx->span_scratch.len;
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_CASE_ITEM, 0), at);
    spec.flags = count == 1 ? ODIN3_AST_F_CASE_ITEM_DEFAULT : 0; /* no match expression */
    return make_n(ctx, &spec, scratch_ids(&ctx->span_scratch), count, out);
}

/* --- timing controls ------------------------------------------------------------------------ */

odin3_status vp_event_control(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                              uint32_t flags, odin3_ast_id *out) {
    return vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_EVENT_CONTROL, 0) | VP_FLAGS(flags), out);
}

odin3_status vp_event_name(odin3_vp_ctx *ctx, uint32_t mark, uint32_t *out) {
    *out = ODIN3_AST_F_EVENT_CONTROL_NO_PARENS;
    odin3_ast_id name = {0};
    odin3_status st = vp_name(ctx, mark, false, &name);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vp_loc at = {odin3_ast_loc(ctx->ast, name), odin3_ast_end(ctx->ast, name), 0};
    odin3_ast_id expr = {0};
    st = make_one(ctx, &at, VP_SPEC(ODIN3_AST_EVENT_EXPR, ODIN3_AST_EVENT_EXPR_NONE), name, &expr);
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, expr) : st;
}

/* EVENT_EXPR[spec] over at of the expression on top of the pending stack, in its place. */
static odin3_status event_expr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec) {
    uint32_t top = odin3_ast_mark(ctx->ast);
    if (top == 0) { /* ev_item always pushes its expression */
        odin3_log(ODIN3_LOG_ERROR, "vp_event_expr: no expression on the pending stack");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_id expr = odin3_ast_pending_span(ctx->ast, top - 1U).ids[0];
    odin3_ast_unwind(ctx->ast, top - 1U);
    odin3_ast_id made = {0};
    odin3_status st = make_one(ctx, at, spec, expr, &made);
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, made) : st;
}

odin3_status vp_event_first(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec,
                            uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast) - 1U; /* the expression's place */
    return event_expr(ctx, at, spec);
}

odin3_status vp_event_next(odin3_vp_ctx *ctx, uint32_t list, const odin3_vp_loc *at, uint32_t spec,
                           uint32_t *out) {
    *out = list;
    return event_expr(ctx, at, spec);
}

odin3_status vp_event_or(odin3_vp_ctx *ctx, uint32_t list, const odin3_vp_loc *at, uint32_t spec,
                         uint32_t *out) {
    *out = list;
    return event_expr(ctx, at, spec | VP_FLAGS(ODIN3_AST_F_EVENT_EXPR_AFTER_OR));
}

/* --- functions and tasks -------------------------------------------------------------------- */

odin3_status vp_function_range(odin3_vp_ctx *ctx, uint32_t flags, odin3_ast_id range,
                               uint32_t *out) {
    bool is_signed = (flags & VP_SIGNED) != 0;
    *out = VP_SPEC(ODIN3_AST_FUNCTION_DECL, 0) |
           (is_signed ? VP_FLAGS(ODIN3_AST_F_FUNCTION_DECL_SIGNED) : VP_NO_FLAGS);
    return odin3_ast_push(ctx->ast, range);
}

odin3_status vp_function_dt(odin3_vp_ctx *ctx, uint32_t dt, uint32_t *out) {
    *out = VP_SPEC(ODIN3_AST_FUNCTION_DECL, 0) | VP_FLAGS(dt << ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT);
    return odin3_ast_push(ctx->ast, (odin3_ast_id){0}); /* no return range */
}

odin3_status vp_tf_header(odin3_vp_ctx *ctx, uint32_t spec, const odin3_vp_loc *at, uint32_t name,
                          uint32_t *out) {
    *out = (uint32_t)ctx->headers.len;
    odin3_vp_header *header = odin3_vec_push(&ctx->headers);
    if (header == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *header = (odin3_vp_header){name, spec, at->end};
    return ODIN3_OK;
}

odin3_status vp_tf_header_ansi(odin3_vp_ctx *ctx, uint32_t spec, const odin3_vp_loc *at,
                               uint32_t name, uint32_t *out) {
    odin3_status st = vp_group_close(ctx, out); /* the last header port */
    return st == ODIN3_OK ? vp_tf_header(ctx, spec, at, name, out) : st;
}

/* The body items of a function or task (§3.2): the statements counted, the first and its
 * position errors found in source order. */
typedef struct tf_parts {
    uint32_t stmts;
    odin3_ast_id body;  /* the first statement (0: none) */
    odin3_ast_id wrong; /* the first misplaced item: a second statement or a late declaration */
    bool late;          /* wrong is a declaration after the statement */
} tf_parts;

static void count_statements(const odin3_ast *ast, const odin3_ast_id *items, uint32_t n,
                             tf_parts *parts) {
    for (uint32_t i = 0; i < n; i++) {
        bool stmt = in_class(ast, items[i], ODIN3_AST_CLS_S);
        if (parts->wrong.v == 0 && parts->stmts > 0) {
            parts->wrong = items[i];
            parts->late = !stmt;
        }
        parts->body = stmt && parts->stmts == 0 ? items[i] : parts->body;
        parts->stmts += stmt ? 1U : 0U;
    }
}

static bool is_function(const odin3_vp_header *header) {
    return (header->flags & ODIN3_VP_SPEC_BYTE) == ODIN3_AST_FUNCTION_DECL;
}

/* The body's located error, if any (*reported): a declaration after the statement, a second
 * statement, or (a function) none; counted, error mode. */
static void report_body(odin3_vp_ctx *ctx, const odin3_vp_header *header, const odin3_vp_loc *at,
                        const tf_parts *parts, bool *reported) {
    const char *what = is_function(header) ? "function" : "task";
    const char *name =
        odin3_strtab_get(odin3_design_strtab(odin3_ast_design(ctx->ast)), header->name);
    name = name != NULL ? name : "";
    *reported = true;
    if (parts->wrong.v != 0 && parts->late) {
        odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, odin3_ast_loc(ctx->ast, parts->wrong), "%s",
                        DECL_AFTER_STMT);
    } else if (parts->wrong.v != 0) {
        odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, odin3_ast_loc(ctx->ast, parts->wrong),
                        "%s '%s' has more than one statement", what, name);
    } else if (parts->stmts == 0 && is_function(header)) {
        odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, at->loc, "%s '%s' has no statement", what, name);
    } else {
        *reported = false;
        return;
    }
    odin3_vp_enter_error(ctx);
}

/* Appends the items[0..n) of class cls (cls NONE: the PORT_DECLs) to part_scratch. */
static odin3_status collect(odin3_vp_ctx *ctx, odin3_ast_class cls, const odin3_ast_id *items,
                            uint32_t n, uint32_t *count) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < n && st == ODIN3_OK; i++) {
        bool port = odin3_ast_kind_of(ctx->ast, items[i]) == ODIN3_AST_PORT_DECL;
        if (cls == ODIN3_AST_CLS_NONE ? port : in_class(ctx->ast, items[i], cls)) {
            st = scratch_push(&ctx->part_scratch, items[i]);
            (*count)++;
        }
    }
    return st;
}

/* Where a task's implicit NULL_STMT sits: one past its last body item (a header port is not
 * one), else one past the header's `;`. */
static odin3_loc after_body(const odin3_vp_ctx *ctx, const odin3_vp_header *header) {
    uint32_t count = (uint32_t)ctx->span_scratch.len;
    uint32_t lead = is_function(header) ? 1U : 0U;
    if (count <= lead) {
        return header->body;
    }
    odin3_ast_id last = scratch_ids(&ctx->span_scratch)[count - 1U];
    bool header_port = odin3_ast_kind_of(ctx->ast, last) == ODIN3_AST_PORT_DECL &&
                       (odin3_ast_flags(ctx->ast, last) & ODIN3_AST_F_PORT_DECL_IN_HEADER) != 0;
    return header_port ? header->body : odin3_ast_end(ctx->ast, last);
}

/* The FUNCTION_DECL / TASK_DECL from span_scratch: [return,] ports LIST, locals LIST, body. */
static odin3_status build_function(odin3_vp_ctx *ctx, const odin3_vp_header *header,
                                   const odin3_vp_loc *at, odin3_ast_id body, odin3_ast_id *out) {
    uint32_t lead = is_function(header) ? 1U : 0U;
    const odin3_ast_id *span = scratch_ids(&ctx->span_scratch);
    uint32_t items = (uint32_t)ctx->span_scratch.len - lead;
    uint32_t ports = 0;
    uint32_t locals = 0;
    odin3_vec_clear(&ctx->part_scratch);
    odin3_status st = collect(ctx, ODIN3_AST_CLS_NONE, span + lead, items, &ports);
    st = st == ODIN3_OK ? collect(ctx, ODIN3_AST_CLS_D, span + lead, items, &locals) : st;
    odin3_ast_id kids[VP_MAX_STMT_KIDS] = {lead > 0 ? span[0] : (odin3_ast_id){0}, {0}, {0}, body};
    const odin3_ast_id *parts = scratch_ids(&ctx->part_scratch);
    st = st == ODIN3_OK ? list_of(ctx, parts, ports, header->body, &kids[1]) : st;
    st = st == ODIN3_OK ? list_of(ctx, parts + ports, locals, header->body, &kids[2]) : st;
    if (st == ODIN3_OK && body.v == 0) { /* a task without a statement (§3.6) */
        odin3_loc where = after_body(ctx, header);
        odin3_vp_loc null_at = {where, where, 0};
        st = vp_null(ctx, odin3_ast_mark(ctx->ast), &null_at, &kids[3]);
    }
    odin3_ast_spec spec = vp_spec_at(header->flags, at);
    spec.name = header->name;
    return st == ODIN3_OK ? make_n(ctx, &spec, kids + 1U - lead, VP_MAX_STMT_KIDS - 1U + lead, out)
                          : st;
}

odin3_status vp_make_function(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                              uint32_t header, odin3_ast_id *out) {
    *out = (odin3_ast_id){0};
    if (header >= ctx->headers.len) { /* the header rule recorded it */
        odin3_log(ODIN3_LOG_ERROR, "vp_make_function: no header %u", (unsigned)header);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vp_header head = *(const odin3_vp_header *)odin3_vec_cat(&ctx->headers, header);
    while (ctx->headers.len > header) {
        odin3_vec_pop(&ctx->headers);
    }
    uint32_t lead = is_function(&head) ? 1U : 0U;
    odin3_vec_clear(&ctx->span_scratch);
    odin3_status st = scratch_take(ctx, mark, &ctx->span_scratch);
    if (st != ODIN3_OK || ctx->span_scratch.len < lead) {
        return st != ODIN3_OK ? st : ODIN3_ERR_INVALID_ARG; /* a function pushed its return */
    }
    tf_parts parts = {0};
    count_statements(ctx->ast, scratch_ids(&ctx->span_scratch) + lead,
                     (uint32_t)ctx->span_scratch.len - lead, &parts);
    bool reported = false;
    report_body(ctx, &head, at, &parts, &reported);
    return reported ? ODIN3_OK : build_function(ctx, &head, at, parts.body, out);
}
