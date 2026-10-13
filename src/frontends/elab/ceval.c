/*
 * ceval.c — the evaluator's core (ceval.h, ceval_internal.h): its state and API, the stacks and
 * frames, name resolution, the typer's callbacks (names, constants, a function's return type),
 * value fitting, the step budget and the diagnostics. Expressions are ceval_expr.c's, calls and
 * statements ceval_interp.c's.
 *
 * Every API call runs the machine on the stacks above what it found and leaves them as it found
 * them (ev_mark / ev_unwind), so a host callback may call the evaluator again. The typer's
 * callbacks run nested evaluations the same way; inside an active evaluation each, and each
 * host re-entry, counts as one level of the call-depth cap, as a function frame does (2D Task 9
 * ruling), and the C nestings alone are bounded by the lower C re-entry cap
 * (ODIN3_ELAB_MAX_REENTRY_DEPTH, 2D Task 11), which bounds the C stack they use.
 */
#include "frontends/elab/ceval.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval_internal.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    EV_MSG = 512,             /* a diagnostic's text */
    EV_MEM_CHUNK = 1024,      /* a frame's locals arena chunk */
    EV_TMP_CHUNK = 4096,      /* a frame's temporaries arena chunk */
    EV_TMP_RENEW = 1 << 20,   /* renew a frame's temporaries above this many bytes */
    EV_INT_BITS = 32,         /* integer */
    EV_TIME_BITS = 64,        /* time */
    EV_BYTE_BITS = 8,         /* a string character */
    EV_BYTES_EVERY = 4096,    /* steps between two checks of the byte budget */
    EV_BYTES_SLACK = 1 << 20, /* bytes of values made between two checks of the byte budget */
    EV_NODE_VALUES = 4        /* values a node may make: two operand extensions, result, fit */
};

static const uint32_t EV_NO_SCOPE = UINT32_MAX; /* no frame: names go to the host */

/* An index this far out is outside any value (the width cap is 2^20), and safe to add to. */
static const int64_t EV_FAR = (int64_t)1 << 40;

/* --- diagnostics --------------------------------------------------------------------------- */

static odin3_status misuse(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "odin3_ceval: %s", why);
    return ODIN3_ERR_INVALID_ARG;
}

const char *ev_str(const odin3_ceval *ev, uint32_t id) {
    const char *str = odin3_strtab_get(odin3_design_strtab(odin3_ast_design(ev->ast)), id);
    return str != NULL ? str : "";
}

static void ev_vreport(const odin3_ceval *ev, odin3_log_level level, odin3_ast_id node,
                       const char *fmt, va_list args) ODIN3_PRINTF(4, 0);

static void ev_vreport(const odin3_ceval *ev, odin3_log_level level, odin3_ast_id node,
                       const char *fmt, va_list args) {
    char msg[EV_MSG];
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    ev->host.report(ev->host.user, level, odin3_ast_loc(ev->ast, node), msg);
}

odin3_status ev_error(const odin3_ceval *ev, odin3_ast_id node, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ev_vreport(ev, ODIN3_LOG_ERROR, node, fmt, args);
    va_end(args);
    return ODIN3_ERR_PARSE;
}

/* The name of frame idx's function ("" for a base frame). */
static const char *ev_frame_name(const odin3_ceval *ev, uint32_t idx) {
    return odin3_ast_name_str(ev->ast, ev_frame(ev, idx)->decl);
}

static bool ev_in_function(const odin3_ceval *ev, uint32_t scope) {
    return scope < ev->frames.len && ev_frame(ev, scope)->decl.v != 0;
}

odin3_status ev_nonconst_fn(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, const char *reason,
                            const char *name) {
    ev->nonconst = true;
    if (ev->quiet) {
        return ODIN3_ERR_PARSE;
    }
    char why[EV_MSG];
    (void)snprintf(why, sizeof why, reason, name);
    if (!ev_in_function(ev, scope)) {
        const char *what = ev->clog2 > 0 ? "$clog2 argument" : ev->what;
        return ev_error(ev, node, "%s is not constant (%s)", what, why);
    }
    return ev_error(ev, node, "function '%s' is not a constant function: %s",
                    ev_frame_name(ev, scope), why);
}

odin3_status ev_nonconst_read(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, uint32_t name) {
    const char *reason =
        ev_in_function(ev, scope) ? "it reads module variable '%s'" : "it reads '%s'";
    return ev_nonconst_fn(ev, scope, node, reason, ev_str(ev, name));
}

/* The typer's callbacks (defined below; every level's typer gets them). */
static odin3_status ev_leaf(void *user, odin3_ast_id node, odin3_etype *out);
static odin3_status ev_constant(void *user, odin3_ast_id node, int64_t *out);
static void ev_report_cb(void *user, odin3_log_level level, odin3_loc loc, const char *msg);

/* --- stacks and frames --------------------------------------------------------------------- */

odin3_status ev_push(odin3_ceval *ev, ev_task task) {
    ev_task *slot = odin3_vec_push(&ev->tasks);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = task;
    return ODIN3_OK;
}

odin3_status ev_push_value(odin3_ceval *ev, const odin3_cval *val) {
    odin3_cval *slot = odin3_vec_push(&ev->values);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *val;
    return ODIN3_OK;
}

odin3_cval ev_pop_value(odin3_ceval *ev) {
    odin3_cval val = *(const odin3_cval *)odin3_vec_cat(&ev->values, ev->values.len - 1);
    odin3_vec_pop(&ev->values);
    return val;
}

const odin3_cval *ev_peek_value(const odin3_ceval *ev, size_t depth) {
    return odin3_vec_cat(&ev->values, ev->values.len - 1 - depth);
}

odin3_ceval_frame *ev_frame(const odin3_ceval *ev, uint32_t idx) {
    return (odin3_ceval_frame *)ev->frames.data + idx;
}

ev_local *ev_local_at(const odin3_ceval *ev, uint32_t idx) {
    return (ev_local *)ev->locals.data + idx;
}

odin3_arena *ev_tmp(const odin3_ceval *ev, uint32_t idx) {
    return ev_frame(ev, idx)->tmp;
}

/* The typer of level `level`, made (with every level below) when first needed. */
static odin3_status ev_typer_at(odin3_ceval *ev, uint32_t level) {
    while (ev->typers.len <= level) {
        const odin3_ctype_host host = {ev_leaf, ev_constant, ev_report_cb, ev};
        odin3_ctype *ty = NULL;
        odin3_status st = odin3_ctype_create(ev->ast, &host, &ty);
        odin3_ctype **slot = st == ODIN3_OK ? (odin3_ctype **)odin3_vec_push(&ev->typers) : NULL;
        if (slot == NULL) {
            odin3_ctype_destroy(ty);
            return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
        }
        *slot = ty;
    }
    return ODIN3_OK;
}

odin3_ctype *ev_types(const odin3_ceval *ev, uint32_t idx) {
    uint32_t level = idx < ev->frames.len ? ev_frame(ev, idx)->level : 0;
    return ((odin3_ctype *const *)ev->typers.data)[level];
}

/* A function's frame types its body's nodes in its level's typer: the frames below of the same
 * function and level must re-type their open roots when control returns to them. */
static void ev_mark_dirty(odin3_ceval *ev, odin3_ast_id decl) {
    for (uint32_t k = 0; decl.v != 0 && k < ev->frames.len; k++) {
        odin3_ceval_frame *fr = odin3_vec_at(&ev->frames, k);
        fr->dirty = fr->dirty || (fr->decl.v == decl.v && fr->level == ev->level);
    }
}

odin3_status ev_frame_push(odin3_ceval *ev, odin3_ast_id decl, odin3_ast_id call, uint32_t *idx) {
    odin3_status st = ev_typer_at(ev, ev->level);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_arena *mem = odin3_arena_create(EV_MEM_CHUNK);
    odin3_arena *tmp = mem != NULL ? odin3_arena_create(EV_TMP_CHUNK) : NULL;
    odin3_ceval_frame *fr = tmp != NULL ? odin3_vec_push(&ev->frames) : NULL;
    if (fr == NULL) {
        odin3_arena_destroy(tmp);
        odin3_arena_destroy(mem);
        return ODIN3_ERR_NO_MEMORY;
    }
    *fr = (odin3_ceval_frame){decl,
                              call,
                              (uint32_t)ev->locals.len,
                              (uint32_t)ev->values.len,
                              (uint32_t)ev->tasks.len,
                              mem,
                              tmp,
                              NULL,
                              ev->level,
                              false};
    *idx = (uint32_t)ev->frames.len - 1;
    ev_mark_dirty(ev, decl);
    ((odin3_ceval_frame *)odin3_vec_at(&ev->frames, *idx))->dirty = false;
    return ODIN3_OK;
}

void ev_frame_pop(odin3_ceval *ev) {
    const odin3_ceval_frame *fr = ev_frame(ev, (uint32_t)ev->frames.len - 1);
    while (ev->locals.len > fr->locals) {
        odin3_vec_pop(&ev->locals);
    }
    odin3_arena_destroy(fr->tmp);
    odin3_arena_destroy(fr->mem);
    odin3_vec_pop(&ev->frames);
}

uint32_t ev_function_frames(const odin3_ceval *ev) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < ev->frames.len; k++) {
        count += ev_frame(ev, k)->decl.v != 0 ? 1U : 0U;
    }
    return count;
}

/* Renews frame idx's temporaries between statements once they hold more than EV_TMP_RENEW. */
static odin3_status ev_tmp_renew(odin3_ceval *ev, uint32_t idx) {
    odin3_ceval_frame *fr = odin3_vec_at(&ev->frames, idx);
    if (idx + 1 != ev->frames.len || ev->values.len != fr->values ||
        odin3_arena_bytes_used(fr->tmp) <= EV_TMP_RENEW) {
        return ODIN3_OK;
    }
    odin3_arena *fresh = odin3_arena_create(EV_TMP_CHUNK);
    if (fresh == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_arena_destroy(fr->tmp);
    fr->tmp = fresh;
    return ODIN3_OK;
}

/* --- marks: what an API call or a nested run restores ------------------------------------- */

typedef struct ev_mark {
    size_t tasks, values, frames, locals, roots;
    uint32_t scope, clog2, nest, level, cur;
    bool quiet, nonconst;
    const char *what;
} ev_mark;

static ev_mark ev_mark_take(const odin3_ceval *ev) {
    return (ev_mark){ev->tasks.len, ev->values.len, ev->frames.len, ev->locals.len, ev->roots.len,
                     ev->scope,     ev->clog2,      ev->nest,       ev->level,      ev->cur,
                     ev->quiet,     ev->nonconst,   ev->what};
}

static void ev_unwind(odin3_ceval *ev, const ev_mark *mark) {
    while (ev->frames.len > mark->frames) {
        ev_frame_pop(ev);
    }
    while (ev->tasks.len > mark->tasks) {
        odin3_vec_pop(&ev->tasks);
    }
    while (ev->values.len > mark->values) {
        odin3_vec_pop(&ev->values);
    }
    while (ev->locals.len > mark->locals) {
        odin3_vec_pop(&ev->locals);
    }
    while (ev->roots.len > mark->roots) {
        odin3_vec_pop(&ev->roots);
    }
    ev->scope = mark->scope;
    ev->clog2 = mark->clog2;
    ev->nest = mark->nest;
    ev->level = mark->level;
    ev->cur = mark->cur;
    ev->quiet = mark->quiet;
    ev->nonconst = mark->nonconst;
    ev->what = mark->what;
}

/* --- the machine --------------------------------------------------------------------------- */

static const ev_handler HANDLERS[EV_OPS] = {
    [EV_EXPR] = ev_do_expr,
    [EV_OP] = ev_do_op,
    [EV_TERN] = ev_do_tern,
    [EV_MERGE] = ev_do_merge,
    [EV_SEL] = ev_do_sel,
    [EV_WORD] = ev_do_word,
    [EV_SYS] = ev_do_sys,
    [EV_ROOT] = ev_do_root,
    [EV_ROOT_END] = ev_do_root_end,
    [EV_CALL] = ev_do_call,
    [FN_RESULT] = ev_do_result,
    [FN_DECL] = ev_do_decl,
    [FN_PARAM] = ev_do_param,
    [FN_BIND] = ev_do_bind,
    [FN_SET] = ev_do_set,
    [FN_RETURN] = ev_do_return,
    [ST_STMT] = ev_do_stmt,
    [ST_ASSIGN] = ev_do_assign,
    [ST_STORE] = ev_do_store,
    [ST_IF] = ev_do_if,
    [ST_CASE] = ev_do_case,
    [ST_LOOP] = ev_do_loop,
    [ST_BLOCK_END] = ev_do_block_end,
};

/* Runs tasks until the stack is back at base (or one fails). */
static odin3_status ev_run(odin3_ceval *ev, size_t base) {
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && ev->tasks.len > base) {
        ev_task task = *(const ev_task *)odin3_vec_cat(&ev->tasks, ev->tasks.len - 1);
        odin3_vec_pop(&ev->tasks);
        if (task.op == ST_STMT) {
            st = ev_tmp_renew(ev, task.frame);
        }
        st = st == ODIN3_OK ? HANDLERS[task.op](ev, &task) : st;
    }
    return st;
}

static void ev_enter(odin3_ceval *ev) {
    if (ev->depth == 0) {
        ev->steps = 0;
        ev->work = 0;
        ev->fresh = 0;
    }
    ev->depth++;
}

/* --- names --------------------------------------------------------------------------------- */

uint32_t ev_find_local(const odin3_ceval *ev, uint32_t frame, odin3_ast_id decl) {
    uint32_t end =
        frame + 1 < ev->frames.len ? ev_frame(ev, frame + 1)->locals : (uint32_t)ev->locals.len;
    for (uint32_t k = ev_frame(ev, frame)->locals; k < end; k++) {
        if (ev_local_at(ev, k)->decl == decl.v) {
            return k;
        }
    }
    return UINT32_MAX;
}

/* The visible local of frame scope named name (the innermost declaration), or UINT32_MAX. */
static uint32_t ev_local_named(const odin3_ceval *ev, uint32_t scope, uint32_t name) {
    if (scope >= ev->frames.len || name == 0) {
        return UINT32_MAX;
    }
    uint32_t end =
        scope + 1 < ev->frames.len ? ev_frame(ev, scope + 1)->locals : (uint32_t)ev->locals.len;
    for (uint32_t k = end; k > ev_frame(ev, scope)->locals; k--) {
        const ev_local *loc = ev_local_at(ev, k - 1);
        if (loc->visible && loc->name == name) {
            return k - 1;
        }
    }
    return UINT32_MAX;
}

/* A CONST's value agrees with its type: the width, SIGNED and UNSIZED (ceval.h, odin3_csym). */
static bool ev_value_fits_type(const odin3_csym *sym) {
    bool is_signed = (sym->value.flags & ODIN3_CVAL_SIGNED) != 0;
    bool is_unsized = (sym->value.flags & ODIN3_CVAL_UNSIZED) != 0;
    return sym->value.width == sym->type.width && is_signed == sym->type.is_signed &&
           is_unsized == sym->type.is_unsized;
}

/* The host's symbol must be a usable CONST or SIGNAL, its type, value and range in agreement
 * (the typer reads the type, the evaluator the value and the range): a host bug otherwise. */
static odin3_status ev_check_sym(const odin3_csym *sym) {
    if (sym->type.width == 0) {
        return misuse("host lookup gave a width of 0");
    }
    if (sym->kind == ODIN3_CSYM_CONST && !ev_value_fits_type(sym)) {
        return misuse("host lookup gave a value whose width or flags differ from its type");
    }
    uint64_t span = sym->msb >= sym->lsb ? (uint64_t)sym->msb - (uint64_t)sym->lsb
                                         : (uint64_t)sym->lsb - (uint64_t)sym->msb;
    if (span != (uint64_t)sym->type.width - 1) {
        return misuse("host lookup gave a range of another width than its type");
    }
    return ODIN3_OK;
}

odin3_status ev_resolve(odin3_ceval *ev, uint32_t scope, odin3_ast_id ref, ev_name *out) {
    uint32_t name =
        odin3_ast_kind_of(ev->ast, ref) == ODIN3_AST_IDENT ? odin3_ast_name(ev->ast, ref) : 0;
    uint32_t local = ev_local_named(ev, scope, name);
    if (local != UINT32_MAX) {
        *out = (ev_name){true, local, {0}};
        return ODIN3_OK;
    }
    odin3_csym sym = {0};
    odin3_status st = ev->host.lookup(ev->host.user, ref, &sym);
    if (st != ODIN3_OK) {
        return st;
    }
    if (sym.kind != ODIN3_CSYM_CONST && sym.kind != ODIN3_CSYM_SIGNAL) {
        uint32_t shown = sym.name != 0 ? sym.name : name;
        return ev_error(ev, ref, "'%s' is not declared", ev_str(ev, shown));
    }
    *out = (ev_name){false, 0, sym};
    return ev_check_sym(&sym);
}

odin3_ast_id ev_select_base(const odin3_ast *ast, odin3_ast_id node, uint32_t *depth) {
    uint32_t count = 0;
    while (odin3_ast_kind_of(ast, node) == ODIN3_AST_SELECT) {
        node = odin3_ast_child(ast, node, 0);
        count++;
    }
    *depth = count;
    return node;
}

odin3_status ev_word_sym(odin3_ceval *ev, uint32_t scope, odin3_ast_id node, odin3_csym *out) {
    uint32_t depth = 0;
    odin3_ast_id base = ev_select_base(ev->ast, node, &depth);
    *out = (odin3_csym){0};
    uint32_t name =
        odin3_ast_kind_of(ev->ast, base) == ODIN3_AST_IDENT ? odin3_ast_name(ev->ast, base) : 0;
    if (ev_local_named(ev, scope, name) != UINT32_MAX) {
        return ODIN3_OK; /* a select of a local vector */
    }
    odin3_status st = ev->host.lookup(ev->host.user, node, out);
    if (st == ODIN3_OK && out->kind != ODIN3_CSYM_SIGNAL && out->kind != ODIN3_CSYM_CONST) {
        out->kind = ODIN3_CSYM_NONE;
    }
    return st == ODIN3_OK && out->kind != ODIN3_CSYM_NONE ? ev_check_sym(out) : st;
}

static int64_t ev_clamp(int64_t idx) {
    if (idx > EV_FAR) {
        return EV_FAR;
    }
    return idx < -EV_FAR ? -EV_FAR : idx;
}

ev_span ev_select_span(const ev_sel_req *req) {
    int64_t first = ev_clamp(req->first);
    int64_t second = ev_clamp(req->second);
    bool down = req->msb >= req->lsb;
    ev_span span = {first, first, 0, 1};
    if (req->sub == ODIN3_AST_SELECT_PART) {
        span.right = second;
    } else if (req->sub == ODIN3_AST_SELECT_PART_PLUS) { /* [b+w-1:b] or [b:b+w-1] */
        span.left = down ? first + second - 1 : first;
        span.right = down ? first : first + second - 1;
    } else if (req->sub == ODIN3_AST_SELECT_PART_MINUS) { /* [b:b-w+1] or [b-w+1:b] */
        span.left = down ? first : first - second + 1;
        span.right = down ? first - second + 1 : first;
    }
    int64_t pos_l = down ? span.left - req->lsb : req->lsb - span.left;
    int64_t pos_r = down ? span.right - req->lsb : req->lsb - span.right;
    span.lo = pos_l < pos_r ? pos_l : pos_r;
    span.width = (uint64_t)(pos_l < pos_r ? pos_r - pos_l : pos_l - pos_r) + 1;
    return span;
}

/* --- types --------------------------------------------------------------------------------- */

/* odin3_ctype_expr with names resolved in frame scope. */
static odin3_status ev_type_in(odin3_ceval *ev, uint32_t scope, odin3_ast_id root,
                               odin3_etype context) {
    uint32_t saved = ev->scope;
    uint32_t saved_cur = ev->cur;
    ev->scope = scope;
    ev->cur = scope < ev->frames.len ? ev_frame(ev, scope)->level : 0;
    odin3_status st = odin3_ctype_expr(ev_types(ev, scope), root, context);
    ev->scope = saved;
    ev->cur = saved_cur;
    return st;
}

odin3_status ev_root_open(odin3_ceval *ev, uint32_t scope, odin3_ast_id root, odin3_etype context) {
    ev_root *slot = odin3_vec_push(&ev->roots);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (ev_root){root.v, scope, context, true};
    size_t idx = ev->roots.len - 1;
    odin3_status st = ev_type_in(ev, scope, root, context);
    ((ev_root *)ev->roots.data)[idx].typing = false;
    if (st != ODIN3_OK) {
        odin3_vec_pop(&ev->roots);
    }
    return st;
}

void ev_root_close(odin3_ceval *ev, odin3_ast_id node) {
    (void)node; /* the top root is node's: roots open and close in stack order */
    odin3_vec_pop(&ev->roots);
}

odin3_status ev_retype(odin3_ceval *ev, uint32_t scope) {
    odin3_ceval_frame *fr = odin3_vec_at(&ev->frames, scope);
    if (!fr->dirty) {
        return ODIN3_OK; /* nothing of its level and function typed its nodes since */
    }
    fr->dirty = false;
    size_t first = ev->roots.len;
    while (first > 0 && ((const ev_root *)ev->roots.data)[first - 1].frame == scope) {
        first--;
    }
    odin3_status st = ODIN3_OK;
    for (size_t k = first; k < ev->roots.len && st == ODIN3_OK; k++) {
        ev_root root = ((const ev_root *)ev->roots.data)[k];
        if (root.typing) {
            continue; /* its typing is running further down the C stack */
        }
        uint64_t size = 0;
        st = ev_subtree_size(ev, (odin3_ast_id){root.node}, &size);
        st = st == ODIN3_OK ? ev_steps(ev, (odin3_ast_id){root.node}, size) : st;
        ((ev_root *)ev->roots.data)[k].typing = true;
        st = st == ODIN3_OK ? ev_type_in(ev, scope, (odin3_ast_id){root.node}, root.context) : st;
        ((ev_root *)ev->roots.data)[k].typing = false;
    }
    return st;
}

/* --- values -------------------------------------------------------------------------------- */

odin3_status ev_filled(odin3_arena *arena, odin3_etype type, odin3_bit fill, odin3_cval *out) {
    if (type.width == 0) {
        *out = (odin3_cval){0};
        return ODIN3_OK;
    }
    odin3_cval_spec spec = {type.width, type.is_signed ? (uint8_t)ODIN3_CVAL_SIGNED : 0, fill};
    return odin3_cval_new(arena, &spec, out);
}

odin3_status ev_fit(odin3_arena *arena, odin3_cval *val, odin3_etype type) {
    if (type.width == 0) {
        return ODIN3_OK;
    }
    if (val->width == 0) {
        return ev_filled(arena, type, ODIN3_BIT_0, val);
    }
    uint8_t sign = type.is_signed ? (uint8_t)ODIN3_CVAL_SIGNED : 0;
    if (val->width == type.width) {
        uint8_t flags = (uint8_t)((val->flags & ~ODIN3_CVAL_SIGNED) | sign);
        if (flags == val->flags) {
            return ODIN3_OK;
        }
        odin3_status st = odin3_cval_copy(arena, val, val);
        val->flags = flags;
        return st;
    }
    bool keep_string = (val->flags & ODIN3_CVAL_STRING) != 0 && !type.is_signed &&
                       type.width > val->width && type.width % EV_BYTE_BITS == 0;
    odin3_cval out;
    odin3_status st = odin3_cval_resize(arena, val, type.width, type.is_signed, &out);
    if (st == ODIN3_OK) {
        out.flags = (uint8_t)(out.flags | (keep_string ? ODIN3_CVAL_STRING : 0));
        *val = out;
    }
    return st;
}

void ev_pop_int(odin3_ceval *ev, int64_t *out, bool *known) {
    odin3_cval val = ev_pop_value(ev);
    *known = true;
    *out = 0;
    if (val.width == 0 || odin3_cval_to_i64(&val, out)) {
        return;
    }
    if (!odin3_cval_is_known(&val)) {
        *known = false;
        return;
    }
    bool negative =
        (val.flags & ODIN3_CVAL_SIGNED) != 0 && odin3_cval_bit(&val, val.width - 1) == ODIN3_BIT_1;
    *out = negative ? INT64_MIN : INT64_MAX; /* far outside any width */
}

/* --- caps ---------------------------------------------------------------------------------- */

/* The outermost function frame (the one whose evaluation the caps name), or UINT32_MAX. */
static uint32_t ev_outermost_function(const odin3_ceval *ev) {
    for (uint32_t k = 0; k < ev->frames.len; k++) {
        if (ev_frame(ev, k)->decl.v != 0) {
            return k;
        }
    }
    return UINT32_MAX;
}

/* `constant evaluation of 'f' exceeds <limit>`, at f's call (no function: at node). */
static odin3_status ev_evaluation_cap(const odin3_ceval *ev, odin3_ast_id node, uint32_t cap,
                                      const char *unit) {
    uint32_t outer = ev_outermost_function(ev);
    if (outer == UINT32_MAX) {
        return ev_error(ev, node, "constant evaluation exceeds %" PRIu32 " %s", cap, unit);
    }
    return ev_error(ev, ev_frame(ev, outer)->call,
                    "constant evaluation of '%s' exceeds %" PRIu32 " %s", ev_frame_name(ev, outer),
                    cap, unit);
}

/* The bytes the evaluator holds: every frame's arenas, its stacks and every level's typer. */
static uint64_t ev_bytes_held(const odin3_ceval *ev) {
    uint64_t bytes = ev->tasks.cap * ev->tasks.elem_size + ev->values.cap * ev->values.elem_size +
                     ev->frames.cap * ev->frames.elem_size + ev->locals.cap * ev->locals.elem_size +
                     ev->roots.cap * ev->roots.elem_size + ev->typers.cap * ev->typers.elem_size;
    for (uint32_t k = 0; k < ev->frames.len; k++) {
        const odin3_ceval_frame *fr = ev_frame(ev, k);
        bytes += odin3_arena_bytes_reserved(fr->mem) + odin3_arena_bytes_reserved(fr->tmp);
    }
    for (size_t k = 0; k < ev->typers.len; k++) {
        bytes += odin3_ctype_bytes(((odin3_ctype *const *)ev->typers.data)[k]);
    }
    return bytes;
}

odin3_status ev_bytes_check(odin3_ceval *ev, odin3_ast_id node) {
    ev->fresh = 0;
    uint64_t cap = odin3_elab_limits_get()->max_bytes;
    if (ev_bytes_held(ev) <= cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, node,
                    "elaboration needs more than %" PRIu64
                    " bytes (the budget is 60%% of physical memory)",
                    cap);
}

odin3_status ev_steps(odin3_ceval *ev, odin3_ast_id node, uint64_t count) {
    uint32_t cap = odin3_elab_limits_get()->max_eval_steps;
    uint64_t before = ev->steps;
    ev->steps += count;
    if (ev->steps > cap) {
        return ev_evaluation_cap(ev, node, cap, "steps");
    }
    bool crossed = before / EV_BYTES_EVERY != ev->steps / EV_BYTES_EVERY;
    return crossed ? ev_bytes_check(ev, node) : ODIN3_OK; /* the byte budget, now and then */
}

odin3_status ev_step(odin3_ceval *ev, odin3_ast_id node) {
    return ev_steps(ev, node, 1);
}

odin3_status ev_bytes_note(odin3_ceval *ev, odin3_ast_id node, uint32_t width) {
    ev->fresh += (uint64_t)odin3_cval_words(width) * 2 * sizeof(uint64_t) * EV_NODE_VALUES;
    return ev->fresh >= EV_BYTES_SLACK ? ev_bytes_check(ev, node) : ODIN3_OK;
}

odin3_status ev_work(odin3_ceval *ev, odin3_ast_id node, uint64_t products) {
    uint32_t cap = odin3_elab_limits_get()->max_eval_work;
    ev->work += products;
    return ev->work <= cap ? ODIN3_OK : ev_evaluation_cap(ev, node, cap, "word operations");
}

odin3_status ev_subtree_size(const odin3_ceval *ev, odin3_ast_id node, uint64_t *out) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, ev->ast, node);
    odin3_ast_event event = ODIN3_AST_ENTER;
    odin3_ast_id at = {0};
    *out = 0;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &event, &at)) {
        *out += event == ODIN3_AST_ENTER ? 1U : 0U;
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* The C re-entry cap (ODIN3_ELAB_MAX_REENTRY_DEPTH, 2D Task 11): one more evaluation nested in C
 * (a typer callback's run or a host's re-entry) must stay within it, apart from the call-depth cap
 * that counts the nestings with the function frames. */
static odin3_status ev_reentry_cap(const odin3_ceval *ev, odin3_ast_id node, const char *what) {
    uint32_t cap = odin3_elab_limits_get()->max_reentry_depth;
    if (ev->nest + 1 <= cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, node, "constant evaluation of %s nested deeper than %" PRIu32, what, cap);
}

/* The <what> of a nested run the typer asks for: its constant's role ("replication count",
 * "range bound"), or "range bound" for a function's return range (typed outside a callback). */
static const char *ev_nested_what(const odin3_ceval *ev) {
    const char *what = NULL;
    if (ev->cur < ev->typers.len) {
        what = odin3_ctype_constant_what(((odin3_ctype *const *)ev->typers.data)[ev->cur]);
    }
    return what != NULL ? what : "range bound";
}

/* The caps before a nested run inside an evaluation (name: for the message): the C re-entry cap,
 * then the call-depth cap. */
static odin3_status ev_nest_check(const odin3_ceval *ev, odin3_ast_id node, const char *name) {
    odin3_status st = ev_reentry_cap(ev, node, ev_nested_what(ev));
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t cap = odin3_elab_limits_get()->max_call_depth;
    if (ev_function_frames(ev) + ev->nest + 1 <= cap) {
        return ODIN3_OK;
    }
    if (name[0] == '\0') {
        return ev_error(ev, node, "constant function recursion deeper than %" PRIu32, cap);
    }
    return ev_error(ev, node, "constant function recursion deeper than %" PRIu32 " in '%s'", cap,
                    name);
}

/* A host's re-entry (a lazy parameter read through lookup, a store callback) nests one more
 * evaluation in C: within the C re-entry cap, and counted with the function frames and callback
 * runs against the call-depth cap (final review, finding 1). No check for the outermost call. */
static odin3_status ev_reentry_check(const odin3_ceval *ev, odin3_ast_id node, const char *what) {
    if (ev->depth == 0) {
        return ODIN3_OK;
    }
    odin3_status st = ev_reentry_cap(ev, node, what);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t cap = odin3_elab_limits_get()->max_call_depth;
    if (ev_function_frames(ev) + ev->nest + 1 <= cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, node, "constant evaluation of %s nested deeper than %" PRIu32, what, cap);
}

/* The innermost function frame's name, "" when none. */
static const char *ev_innermost_function(const odin3_ceval *ev) {
    for (uint32_t k = (uint32_t)ev->frames.len; k > 0; k--) {
        if (ev_frame(ev, k - 1)->decl.v != 0) {
            return ev_frame_name(ev, k - 1);
        }
    }
    return "";
}

/* --- declaration types --------------------------------------------------------------------- */

/* The DT field of a declaration with one (0 = none); VAR_DECL's comes from its sub-kind. */
static uint32_t ev_dt_of(const odin3_ast *ast, odin3_ast_id node) {
    uint32_t flags = odin3_ast_flags(ast, node);
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_PORT_DECL:
        return (flags & ODIN3_AST_F_PORT_DECL_DT_MASK) >> ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    case ODIN3_AST_PARAM_DECL:
        return (flags & ODIN3_AST_F_PARAM_DECL_DT_MASK) >> ODIN3_AST_F_PARAM_DECL_DT_SHIFT;
    case ODIN3_AST_FUNCTION_DECL:
        return (flags & ODIN3_AST_F_FUNCTION_DECL_DT_MASK) >> ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT;
    case ODIN3_AST_VAR_DECL: {
        static const uint8_t VAR_DT[ODIN3_AST_VAR_DECL_COUNT] = {
            [ODIN3_AST_VAR_DECL_REG] = ODIN3_AST_DT_REG,
            [ODIN3_AST_VAR_DECL_INTEGER] = ODIN3_AST_DT_INTEGER,
            [ODIN3_AST_VAR_DECL_TIME] = ODIN3_AST_DT_TIME,
            [ODIN3_AST_VAR_DECL_REAL] = ODIN3_AST_DT_REAL,
            [ODIN3_AST_VAR_DECL_REALTIME] = ODIN3_AST_DT_REALTIME};
        uint32_t sub = odin3_ast_sub(ast, node);
        return sub < ODIN3_AST_VAR_DECL_COUNT ? VAR_DT[sub] : 0;
    }
    default:
        return 0;
    }
}

static bool ev_signed_flag(const odin3_ast *ast, odin3_ast_id node) {
    uint32_t flags = odin3_ast_flags(ast, node);
    if (odin3_ast_kind_of(ast, node) == ODIN3_AST_FUNCTION_DECL) {
        return (flags & ODIN3_AST_F_FUNCTION_DECL_SIGNED) != 0;
    }
    return (flags & ODIN3_AST_F_VAR_DECL_SIGNED) != 0; /* bit 0 for every other declaration */
}

odin3_status ev_decl_type_of(const odin3_ceval *ev, const ev_decl_type *req, odin3_etype *type,
                             int64_t *msb, int64_t *lsb) {
    uint32_t dt = ev_dt_of(ev->ast, req->node);
    if (dt == ODIN3_AST_DT_REAL || dt == ODIN3_AST_DT_REALTIME) {
        return ev_error(ev, req->node, "variable type '%s' is not supported",
                        dt == ODIN3_AST_DT_REAL ? "real" : "realtime");
    }
    if (dt == ODIN3_AST_DT_INTEGER || dt == ODIN3_AST_DT_TIME) {
        uint32_t bits = dt == ODIN3_AST_DT_INTEGER ? EV_INT_BITS : EV_TIME_BITS;
        *type = (odin3_etype){bits, dt == ODIN3_AST_DT_INTEGER, false};
        *msb = (int64_t)bits - 1;
        *lsb = 0;
        return ODIN3_OK;
    }
    *msb = req->has_range ? req->msb : 0;
    *lsb = req->has_range ? req->lsb : 0;
    uint64_t span =
        *msb >= *lsb ? (uint64_t)*msb - (uint64_t)*lsb : (uint64_t)*lsb - (uint64_t)*msb;
    uint32_t cap = odin3_elab_limits_get()->max_width;
    if (span >= cap) {
        return ev_error(ev, req->node, "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits",
                        span == UINT64_MAX ? span : span + 1, cap);
    }
    *type = (odin3_etype){(uint32_t)span + 1, ev_signed_flag(ev->ast, req->node), false};
    return ODIN3_OK;
}

/* --- the typer's callbacks ----------------------------------------------------------------- */

/* A nested evaluation's setup: the mark to restore, and whether it is nested in another. */
typedef struct ev_nested {
    ev_mark mark;
    bool nested;
} ev_nested;

static odin3_status ev_nested_begin(odin3_ceval *ev, odin3_ast_id node, const char *name,
                                    ev_nested *out) {
    out->mark = ev_mark_take(ev);
    out->nested = ev->depth > 0;
    odin3_status st = out->nested ? ev_nest_check(ev, node, name) : ODIN3_OK;
    if (st == ODIN3_OK) {
        ev_enter(ev);
        ev->nest += out->nested ? 1U : 0U; /* counted against the call-depth cap */
        ev->level++;                       /* its frames type with a typer of their own */
        ev->clog2 = 0;
        ev->nonconst = false;
    }
    return st;
}

static void ev_nested_end(odin3_ceval *ev, const ev_nested *nst) {
    bool nonconst = ev->nonconst;
    ev_unwind(ev, &nst->mark);               /* restores nest and level */
    ev->nonconst = ev->nonconst || nonconst; /* the outer evaluation stops for the same reason */
    ev->depth--;
}

/* Evaluates the range [msb:lsb] in a base frame (no locals): a function's return range. */
static odin3_status ev_nested_range(odin3_ceval *ev, odin3_ast_id range, const char *name,
                                    int64_t bounds[2]) {
    ev_nested nst;
    odin3_status st = ev_nested_begin(ev, range, name, &nst);
    if (st != ODIN3_OK) {
        return st;
    }
    ev->what = "range bound";
    uint32_t base = 0;
    st = ev_frame_push(ev, (odin3_ast_id){0}, (odin3_ast_id){0}, &base);
    for (uint32_t k = 2; k > 0 && st == ODIN3_OK; k--) { /* msb runs first */
        st = ev_push(ev, (ev_task){.op = EV_ROOT,
                                   .node = odin3_ast_child(ev->ast, range, k - 1).v,
                                   .frame = base});
    }
    st = st == ODIN3_OK ? ev_run(ev, nst.mark.tasks) : st;
    for (uint32_t k = 2; k > 0 && st == ODIN3_OK; k--) {
        bool known = true;
        ev_pop_int(ev, &bounds[k - 1], &known);
        if (!known) {
            st =
                ev_error(ev, odin3_ast_child(ev->ast, range, k - 1), "range bound has x or z bits");
        }
    }
    ev_nested_end(ev, &nst);
    return st;
}

/* The type of a user function's result (§4.6): its return range or DT, its SIGNED flag. */
static odin3_status ev_call_type(odin3_ceval *ev, odin3_ast_id call, odin3_etype *out) {
    odin3_csym sym = {0};
    odin3_status st = ev->host.callee(ev->host.user, call, &sym);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *name = odin3_ast_name_str(ev->ast, call);
    if (sym.kind == ODIN3_CSYM_TASK) {
        return ev_error(ev, call, "'%s' is not a function", name);
    }
    if (sym.kind != ODIN3_CSYM_FUNCTION) {
        return ev_error(ev, call, "'%s' is not declared", name);
    }
    odin3_ast_id range = odin3_ast_child(ev->ast, sym.decl, 0);
    int64_t bounds[2] = {0, 0};
    st = range.v != 0 ? ev_nested_range(ev, range, name, bounds) : ODIN3_OK;
    ev_decl_type req = {sym.decl, range.v != 0, bounds[0], bounds[1]};
    int64_t msb = 0;
    int64_t lsb = 0;
    return st == ODIN3_OK ? ev_decl_type_of(ev, &req, out, &msb, &lsb) : st;
}

/* A system call the typer could not type: a built-in with another argument count than 1, or
 * E20 for any other $name. */
static odin3_status ev_system_call(const odin3_ceval *ev, odin3_ast_id node) {
    static const char *const BUILT_IN[] = {"$signed", "$unsigned", "$clog2"};
    const char *name = odin3_ast_name_str(ev->ast, node);
    uint32_t args = 0;
    for (uint32_t k = 0; k < odin3_ast_nchild(ev->ast, node); k++) {
        args += odin3_ast_child(ev->ast, node, k).v != 0 ? 1U : 0U;
    }
    for (size_t k = 0; k < sizeof BUILT_IN / sizeof BUILT_IN[0]; k++) {
        if (strcmp(name, BUILT_IN[k]) == 0) {
            return ev_error(ev, node,
                            "system function '%s' has 1 input; %" PRIu32 " arguments given", name,
                            args);
        }
    }
    return ev_error(ev, node, "system function '%s' is not supported", name);
}

/* A memory word's type (width 0: an ordinary select), for the typer. */
static odin3_status ev_word_type(odin3_ceval *ev, odin3_ast_id node, odin3_etype *out) {
    odin3_csym sym;
    odin3_status st = ev_word_sym(ev, ev->scope, node, &sym);
    if (st == ODIN3_OK && sym.kind != ODIN3_CSYM_NONE) {
        *out = sym.type;
    }
    return st;
}

static odin3_status ev_leaf(void *user, odin3_ast_id node, odin3_etype *out) {
    odin3_ceval *ev = user;
    switch (odin3_ast_kind_of(ev->ast, node)) {
    case ODIN3_AST_SELECT:
        return ev_word_type(ev, node, out);
    case ODIN3_AST_CALL:
        if ((odin3_ast_flags(ev->ast, node) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
            return ev_system_call(ev, node);
        }
        return ev_call_type(ev, node, out);
    default: {
        ev_name nm;
        odin3_status st = ev_resolve(ev, ev->scope, node, &nm);
        if (st == ODIN3_OK) {
            *out = nm.is_local ? ev_local_at(ev, nm.local)->type : nm.sym.type;
        }
        return st;
    }
    }
}

/* A replication count or part-select bound (typed already): evaluated in the typer's scope. */
static odin3_status ev_constant(void *user, odin3_ast_id node, int64_t *out) {
    odin3_ceval *ev = user;
    ev_nested nst;
    odin3_status st = ev_nested_begin(ev, node, ev_innermost_function(ev), &nst);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *what =
        odin3_ctype_constant_what(((odin3_ctype *const *)ev->typers.data)[nst.mark.cur]);
    ev->what = what;
    uint32_t scope = nst.mark.scope;
    if (scope >= ev->frames.len) {
        st = ev_frame_push(ev, (odin3_ast_id){0}, (odin3_ast_id){0}, &scope);
        if (st == ODIN3_OK) { /* node was typed by the calling typer: read its types there */
            ((odin3_ceval_frame *)odin3_vec_at(&ev->frames, scope))->level = nst.mark.cur;
        }
    }
    st =
        st == ODIN3_OK ? ev_push(ev, (ev_task){.op = EV_EXPR, .node = node.v, .frame = scope}) : st;
    st = st == ODIN3_OK ? ev_run(ev, nst.mark.tasks) : st;
    bool known = true;
    if (st == ODIN3_OK) {
        ev_pop_int(ev, out, &known);
    }
    if (st == ODIN3_OK && !known) {
        st = ev_error(ev, node, "%s has x or z bits", what);
    }
    ev_nested_end(ev, &nst);
    return st;
}

static void ev_report_cb(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    const odin3_ceval *ev = user;
    ev->host.report(ev->host.user, level, loc, msg);
}

/* --- the API ------------------------------------------------------------------------------- */

odin3_status odin3_ceval_create(odin3_ast *ast, const odin3_ceval_host *host, odin3_ceval **out) {
    if (ast == NULL || host == NULL || out == NULL || host->lookup == NULL ||
        host->callee == NULL || host->report == NULL) {
        return misuse("odin3_ceval_create: NULL argument or callback");
    }
    odin3_ceval *ev = odin3_util_calloc(sizeof *ev);
    if (ev == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ev->ast = ast;
    ev->host = *host;
    ev->scope = EV_NO_SCOPE;
    odin3_vec_init(&ev->typers, sizeof(odin3_ctype *));
    odin3_status st = ev_typer_at(ev, 0);
    if (st != ODIN3_OK) {
        odin3_vec_free(&ev->typers);
        odin3_util_free(ev);
        return st;
    }
    odin3_vec_init(&ev->tasks, sizeof(ev_task));
    odin3_vec_init(&ev->values, sizeof(odin3_cval));
    odin3_vec_init(&ev->frames, sizeof(odin3_ceval_frame));
    odin3_vec_init(&ev->locals, sizeof(ev_local));
    odin3_vec_init(&ev->roots, sizeof(ev_root));
    odin3_vec_init(&ev->word, sizeof(int64_t));
    *out = ev;
    return ODIN3_OK;
}

void odin3_ceval_destroy(odin3_ceval *ev) {
    if (ev == NULL) {
        return;
    }
    while (ev->frames.len > 0) {
        ev_frame_pop(ev);
    }
    odin3_vec_free(&ev->tasks);
    odin3_vec_free(&ev->values);
    odin3_vec_free(&ev->frames);
    odin3_vec_free(&ev->locals);
    odin3_vec_free(&ev->roots);
    odin3_vec_free(&ev->word);
    for (size_t k = 0; k < ev->typers.len; k++) {
        odin3_ctype_destroy(((odin3_ctype **)ev->typers.data)[k]);
    }
    odin3_vec_free(&ev->typers);
    odin3_util_free(ev);
}

odin3_ctype *odin3_ceval_types(odin3_ceval *ev) {
    return ev != NULL ? *(odin3_ctype **)odin3_vec_at(&ev->typers, 0) : NULL; /* level 0 */
}

bool odin3_ceval_word_index(const odin3_ceval *ev, uint32_t idx, int64_t *out) {
    if (ev == NULL || out == NULL || idx >= ev->word.len) {
        return false;
    }
    *out = ((const int64_t *)ev->word.data)[idx];
    return true;
}

static bool ev_is_kind_class(const odin3_ast *ast, odin3_ast_id node, odin3_ast_class cls) {
    if (node.v == 0 || node.v >= odin3_ast_node_end(ast).v) {
        return false;
    }
    return odin3_ast_class_has(cls, odin3_ast_kind_info_of((uint32_t)odin3_ast_kind_of(ast, node)));
}

/* One API evaluation: the request, how it reports, and where the result goes. */
typedef struct ev_api {
    const odin3_ceval_req *req;
    odin3_arena *arena;
    odin3_cval *out;
    bool quiet;
} ev_api;

static odin3_status ev_copy_out(odin3_arena *arena, const odin3_cval *val, odin3_cval *out) {
    if (val->width == 0) {
        *out = (odin3_cval){.flags = val->flags};
        return ODIN3_OK;
    }
    return odin3_cval_copy(arena, val, out);
}

/* Evaluates api->req; *nonconst tells a non-constant stop (PARSE) from an error. */
static odin3_status ev_evaluate(odin3_ceval *ev, const ev_api *api, bool *nonconst) {
    const char *what = api->req->what != NULL ? api->req->what : "expression";
    odin3_status st = ev_reentry_check(ev, api->req->expr, what);
    if (st != ODIN3_OK) {
        return st;
    }
    ev_mark mark = ev_mark_take(ev);
    ev->nest += ev->depth > 0 ? 1U : 0U;  /* counted against the call-depth cap */
    ev->level += ev->depth > 0 ? 1U : 0U; /* a host's re-entry types with a typer of its own */
    ev_enter(ev);
    ev->quiet = api->quiet;
    ev->nonconst = false;
    ev->clog2 = 0;
    ev->what = what;
    uint32_t base = 0;
    st = ev_frame_push(ev, (odin3_ast_id){0}, (odin3_ast_id){0}, &base);
    odin3_etype ctx = api->req->context;
    ev_task root = {.op = EV_ROOT,
                    .node = api->req->expr.v,
                    .frame = base,
                    .a = ctx.width,
                    .sign = ctx.width != 0 && ctx.is_signed ? 1 : 0};
    st = st == ODIN3_OK ? ev_push(ev, root) : st;
    st = st == ODIN3_OK ? ev_run(ev, mark.tasks) : st;
    if (st == ODIN3_OK) {
        odin3_cval val = ev_pop_value(ev);
        st = ev_copy_out(api->arena, &val, api->out);
    }
    *nonconst = ev->nonconst;
    ev_unwind(ev, &mark);
    ev->depth--;
    return st;
}

odin3_status odin3_ceval_expr(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                              odin3_cval *out) {
    if (ev == NULL || req == NULL || arena == NULL || out == NULL ||
        !ev_is_kind_class(ev->ast, req->expr, ODIN3_AST_CLS_E)) {
        return misuse("odin3_ceval_expr: NULL argument or a root that is not an expression");
    }
    bool nonconst = false;
    const ev_api api = {req, arena, out, false};
    return ev_evaluate(ev, &api, &nonconst);
}

odin3_status odin3_ceval_try(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                             odin3_cval *out, bool *is_const) {
    if (ev == NULL || req == NULL || arena == NULL || out == NULL || is_const == NULL ||
        !ev_is_kind_class(ev->ast, req->expr, ODIN3_AST_CLS_E)) {
        return misuse("odin3_ceval_try: NULL argument or a root that is not an expression");
    }
    bool nonconst = false;
    const ev_api api = {req, arena, out, true};
    odin3_status st = ev_evaluate(ev, &api, &nonconst);
    *is_const = st == ODIN3_OK;
    return st == ODIN3_ERR_PARSE && nonconst ? ODIN3_OK : st;
}

odin3_status odin3_ceval_call(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena,
                              odin3_cval *out) {
    if (ev == NULL || req == NULL || odin3_ast_kind_of(ev->ast, req->expr) != ODIN3_AST_CALL ||
        (odin3_ast_flags(ev->ast, req->expr) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        return misuse("odin3_ceval_call: not a CALL of a user function");
    }
    return odin3_ceval_expr(ev, req, arena, out);
}

odin3_status odin3_ceval_exec(odin3_ceval *ev, odin3_ast_id stmt, const odin3_ceval_store *store,
                              bool *is_const) {
    if (ev == NULL || is_const == NULL || !ev_is_kind_class(ev->ast, stmt, ODIN3_AST_CLS_S)) {
        return misuse("odin3_ceval_exec: NULL argument or a node that is not a statement");
    }
    odin3_status st = ev_reentry_check(ev, stmt, "statement");
    if (st != ODIN3_OK) {
        *is_const = false;
        return st;
    }
    ev_mark mark = ev_mark_take(ev);
    ev->nest += ev->depth > 0 ? 1U : 0U;
    ev->level += ev->depth > 0 ? 1U : 0U;
    ev_enter(ev);
    ev->quiet = true;
    ev->nonconst = false;
    ev->clog2 = 0;
    ev->what = "statement";
    uint32_t base = 0;
    st = ev_frame_push(ev, (odin3_ast_id){0}, (odin3_ast_id){0}, &base);
    if (st == ODIN3_OK) {
        ev_frame(ev, base)->store = store;
        st = ev_push(ev, (ev_task){.op = ST_STMT, .node = stmt.v, .frame = base});
    }
    st = st == ODIN3_OK ? ev_run(ev, mark.tasks) : st;
    bool nonconst = ev->nonconst;
    ev_unwind(ev, &mark);
    ev->depth--;
    *is_const = st == ODIN3_OK;
    return st == ODIN3_ERR_PARSE && nonconst ? ODIN3_OK : st;
}
