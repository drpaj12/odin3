/*
 * ceval_interp.c — the constant-function interpreter (ceval_internal.h; spec §4.6, EL-17): a
 * call pushes a frame whose declarations, argument bindings and body run as tasks on the
 * evaluator's stack (no C recursion, so recursive functions (K8) work up to the call-depth cap),
 * and odin3_ceval_exec's module-level statements (EL-40) run the same way against a store.
 *
 * A call's frame is filled in this order: the result variable (its range in module scope, as the
 * typer saw it), the localparams, each input with its argument (evaluated in the caller's frame
 * with the input's width as context: the Task 8 handoff), the other variables, then the body.
 * Every variable starts all x. Statements: blocking assignments to locals (bit and part selects
 * included; in exec also to the store), if, case/casez/casex (in_case_match), for, while,
 * repeat, blocks (a named block's declarations are made at each entry, x again, as Yosys does)
 * and $display-family tasks (ignored). Anything else stops the call as §4.6 and §9.1 say.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/ceval_internal.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    IN_RANGE = 0,      /* a declaration's range slot */
    IN_FN_PORTS = 1,   /* FUNCTION_DECL: the ports LIST */
    IN_FN_LOCALS = 2,  /* FUNCTION_DECL: the locals LIST */
    IN_FN_BODY = 3,    /* FUNCTION_DECL: the body */
    IN_ASSIGN_RHS = 2, /* BLOCKING_ASSIGN: lhs, timing, rhs */
    IN_LOOP_STEP = 2,  /* FOR: init, cond, step, body */
    IN_FOR_BODY = 3,
    IN_MAX_ATOM_INDICES = 8 /* index values of one lvalue atom we read at a time */
};

/* ST_LOOP phases. */
enum { LP_START, LP_TEST, LP_DECIDE, LP_COUNT, LP_REPEAT };

static ev_task in_task(uint8_t op, odin3_ast_id node, uint32_t frame) {
    return (ev_task){.op = op, .node = node.v, .frame = frame};
}

/* An EV_ROOT task: node evaluated in context, names in frame. */
static ev_task in_root(odin3_ast_id node, uint32_t frame, odin3_etype context) {
    return (ev_task){.op = EV_ROOT,
                     .node = node.v,
                     .frame = frame,
                     .a = context.width,
                     .sign = context.width != 0 && context.is_signed ? 1 : 0};
}

static odin3_etype in_self(void) {
    return (odin3_etype){0, false, false};
}

static bool in_is_function(const odin3_ceval *ev, uint32_t frame) {
    return ev_frame(ev, frame)->decl.v != 0;
}

static const char *in_fn_name(const odin3_ceval *ev, uint32_t frame) {
    return odin3_ast_name_str(ev->ast, ev_frame(ev, frame)->decl);
}

/* --- locals -------------------------------------------------------------------------------- */

/* A local's new value: val's low bits at the local's width and sign (val is at least as wide,
 * or empty), copied into the local's own planes. */
static odin3_status in_local_assign(odin3_ceval *ev, odin3_arena *tmp, uint32_t idx,
                                    const odin3_cval *val) {
    ev_local *loc = odin3_vec_at(&ev->locals, idx);
    odin3_cval fitted = *val;
    odin3_status st = ev_fit(tmp, &fitted, loc->type);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t bytes = (size_t)odin3_cval_words(loc->type.width) * sizeof *loc->value.aval;
    memcpy(loc->value.aval, fitted.aval, bytes);
    memcpy(loc->value.bval, fitted.bval, bytes);
    return ODIN3_OK;
}

/* Declares (or, in a block entered again, re-declares) the local of DECLARATOR decl. */
typedef struct in_new_local {
    odin3_ast_id decl;
    odin3_etype type;
    int64_t msb, lsb;
    bool is_const;
    const odin3_cval *value; /* a localparam's value; NULL: all x */
} in_new_local;

/* A re-entered block's declaration of the same width reuses its planes in place: a variable's
 * become all x again, a localparam's take its new value (fix round 1, finding 2: nothing grows
 * the frame's arena per iteration). */
static bool in_redeclare(odin3_ceval *ev, uint32_t idx, const in_new_local *req) {
    ev_local *loc = odin3_vec_at(&ev->locals, idx);
    if (loc->value.width != req->type.width || loc->value.width == 0) {
        return false;
    }
    size_t words = odin3_cval_words(loc->value.width);
    loc->visible = true;
    if (req->value != NULL) {
        memcpy(loc->value.aval, req->value->aval, words * sizeof *loc->value.aval);
        memcpy(loc->value.bval, req->value->bval, words * sizeof *loc->value.bval);
        loc->value.flags = req->value->flags;
        loc->type = req->type;
        return true;
    }
    memset(loc->value.aval, UINT8_MAX, words * sizeof *loc->value.aval);
    memset(loc->value.bval, UINT8_MAX, words * sizeof *loc->value.bval);
    uint32_t used = loc->value.width % ODIN3_CVAL_WORD_BITS;
    uint64_t top = used == 0 ? UINT64_MAX : ((uint64_t)1 << used) - 1;
    loc->value.aval[words - 1] &= top;
    loc->value.bval[words - 1] &= top;
    return true;
}

static odin3_status in_declare(odin3_ceval *ev, uint32_t frame, const in_new_local *req,
                               uint32_t *out) {
    uint32_t idx = ev_find_local(ev, frame, req->decl);
    if (out != NULL) {
        *out = idx;
    }
    if (idx != UINT32_MAX && in_redeclare(ev, idx, req)) {
        return ODIN3_OK;
    }
    odin3_cval val = {0};
    odin3_arena *mem = ev_frame(ev, frame)->mem;
    odin3_status st = ev_bytes_check(ev, req->decl);
    if (st != ODIN3_OK) {
        return st;
    }
    st = req->value != NULL ? odin3_cval_copy(mem, req->value, &val)
                            : ev_filled(mem, req->type, ODIN3_BIT_X, &val);
    if (st == ODIN3_OK && idx == UINT32_MAX) {
        st = odin3_vec_push(&ev->locals) != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        idx = (uint32_t)ev->locals.len - 1;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *ev_local_at(ev, idx) = (ev_local){odin3_ast_name(ev->ast, req->decl),
                                       req->decl.v,
                                       req->is_const,
                                       true,
                                       req->type,
                                       req->msb,
                                       req->lsb,
                                       val};
    if (out != NULL) {
        *out = idx;
    }
    return ODIN3_OK;
}

/* --- calls --------------------------------------------------------------------------------- */

/* The DECLARATORs of a PORT_DECL (its tail after the range slot). */
static uint32_t in_declarators(const odin3_ast *ast, odin3_ast_id decl) {
    uint32_t count = odin3_ast_nchild(ast, decl);
    return count > 1 ? count - 1 : 0;
}

static uint32_t in_formals(const odin3_ast *ast, odin3_ast_id fn) {
    odin3_ast_id ports = odin3_ast_child(ast, fn, IN_FN_PORTS);
    uint32_t count = 0;
    for (uint32_t k = 0; ports.v != 0 && k < odin3_ast_nchild(ast, ports); k++) {
        count += in_declarators(ast, odin3_ast_child(ast, ports, k));
    }
    return count;
}

/* The callee of call (a FUNCTION_DECL), or the located error. */
static odin3_status in_callee(odin3_ceval *ev, odin3_ast_id call, odin3_ast_id *decl) {
    const char *name = odin3_ast_name_str(ev->ast, call);
    if ((odin3_ast_flags(ev->ast, call) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        return ev_error(ev, call, "system function '%s' is not supported", name);
    }
    odin3_csym sym = {0};
    odin3_status st = ev->host.callee(ev->host.user, call, &sym);
    if (st != ODIN3_OK) {
        return st;
    }
    if (sym.kind == ODIN3_CSYM_TASK) {
        return ev_error(ev, call, "'%s' is not a function", name);
    }
    if (sym.kind != ODIN3_CSYM_FUNCTION) {
        return ev_error(ev, call, "'%s' is not declared", name);
    }
    uint32_t formals = in_formals(ev->ast, sym.decl);
    uint32_t args = odin3_ast_nchild(ev->ast, call);
    if (formals != args) {
        return ev_error(ev, call,
                        "function '%s' has %" PRIu32 " inputs; %" PRIu32 " arguments given", name,
                        formals, args);
    }
    *decl = sym.decl;
    return ODIN3_OK;
}

static odin3_status in_depth_check(const odin3_ceval *ev, odin3_ast_id call) {
    uint32_t cap = odin3_elab_limits_get()->max_call_depth;
    if (ev_function_frames(ev) + ev->nest + 1 <= cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, call, "constant function recursion deeper than %" PRIu32 " in '%s'", cap,
                    odin3_ast_name_str(ev->ast, call));
}

/* Pushes FN_DECL for the items of list whose kind is (or, with `other`, is not) PARAM_DECL. */
static odin3_status in_push_decls(odin3_ceval *ev, odin3_ast_id list, uint32_t frame, bool other) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = list.v != 0 ? odin3_ast_nchild(ev->ast, list) : 0; k > 0 && st == ODIN3_OK;
         k--) {
        odin3_ast_id item = odin3_ast_child(ev->ast, list, k - 1);
        bool param = odin3_ast_kind_of(ev->ast, item) == ODIN3_AST_PARAM_DECL;
        if (param != other) {
            st = ev_push(ev, in_task(FN_DECL, item, frame));
        }
    }
    return st;
}

/* The inputs: per PORT_DECL, its declaration then its arguments (b = its first formal). */
static odin3_status in_push_ports(odin3_ceval *ev, odin3_ast_id ports, uint32_t frame,
                                  uint32_t caller) {
    uint32_t count = ports.v != 0 ? odin3_ast_nchild(ev->ast, ports) : 0;
    uint32_t first = in_formals(ev->ast, ev_frame(ev, frame)->decl);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = count; k > 0 && st == ODIN3_OK; k--) {
        odin3_ast_id port = odin3_ast_child(ev->ast, ports, k - 1);
        first -= in_declarators(ev->ast, port);
        st = ev_push(
            ev, (ev_task){.op = FN_BIND, .node = port.v, .frame = frame, .a = caller, .b = first});
        st = st == ODIN3_OK ? ev_push(ev, in_task(FN_DECL, port, frame)) : st;
    }
    return st;
}

odin3_status ev_do_call(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id call = {task->node};
    odin3_ast_id decl = {0};
    odin3_status st = in_callee(ev, call, &decl);
    st = st == ODIN3_OK ? in_depth_check(ev, call) : st;
    uint32_t frame = 0;
    st = st == ODIN3_OK ? ev_frame_push(ev, decl, call, &frame) : st;
    st = st == ODIN3_OK ? ev_bytes_check(ev, call) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_id locals = odin3_ast_child(ev->ast, decl, IN_FN_LOCALS);
    st = ev_push(ev, (ev_task){.op = FN_RETURN, .node = call.v, .frame = frame, .a = task->frame});
    st = st == ODIN3_OK
             ? ev_push(ev, in_task(ST_STMT, odin3_ast_child(ev->ast, decl, IN_FN_BODY), frame))
             : st;
    st = st == ODIN3_OK ? in_push_decls(ev, locals, frame, true) : st;
    st = st == ODIN3_OK
             ? in_push_ports(ev, odin3_ast_child(ev->ast, decl, IN_FN_PORTS), frame, task->frame)
             : st;
    st = st == ODIN3_OK ? in_push_decls(ev, locals, frame, false) : st;
    return st == ODIN3_OK ? ev_push(ev, in_task(FN_RESULT, decl, frame)) : st;
}

/* Pushes `then`, preceded by the evaluation of decl's range bounds (msb first) if it has one. */
static odin3_status in_push_range(odin3_ceval *ev, const ev_task *then, odin3_ast_id range) {
    odin3_status st = ev_push(ev, *then);
    for (uint32_t k = 2; k > 0 && st == ODIN3_OK; k--) {
        st = ev_push(ev, in_root(odin3_ast_child(ev->ast, range, k - 1), then->frame, in_self()));
    }
    return st;
}

/* The range bounds on the stack (lsb on top), popped; an x bound is an error. */
static odin3_status in_pop_range(odin3_ceval *ev, odin3_ast_id range, int64_t *msb, int64_t *lsb) {
    bool known_l = true;
    bool known_m = true;
    ev_pop_int(ev, lsb, &known_l);
    ev_pop_int(ev, msb, &known_m);
    if (!known_l || !known_m) {
        return ev_error(ev, odin3_ast_child(ev->ast, range, known_m ? 1 : 0),
                        "range bound has x or z bits");
    }
    return ODIN3_OK;
}

odin3_status ev_do_result(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id decl = {task->node};
    odin3_ast_id range = odin3_ast_child(ev->ast, decl, IN_RANGE);
    if (task->phase == 0 && range.v != 0) {
        ev_task then = *task;
        then.phase = 1;
        return in_push_range(ev, &then, range);
    }
    ev_decl_type req = {decl, range.v != 0, 0, 0};
    odin3_status st = range.v != 0 ? in_pop_range(ev, range, &req.msb, &req.lsb) : ODIN3_OK;
    in_new_local res = {decl, {0, false, false}, 0, 0, false, NULL};
    st = st == ODIN3_OK ? ev_decl_type_of(ev, &req, &res.type, &res.msb, &res.lsb) : st;
    return st == ODIN3_OK ? in_declare(ev, task->frame, &res, NULL) : st;
}

/* --- declarations -------------------------------------------------------------------------- */

/* A localparam's EL-16 mode (odin3_cparam_mode) and target type (FN_PARAM's spare, a, sign). */
static void in_param_mode(const odin3_ceval *ev, odin3_ast_id decl, const odin3_etype *type,
                          ev_task *param) {
    bool ranged = odin3_ast_child(ev->ast, decl, IN_RANGE).v != 0;
    uint32_t dt = ((uint32_t)odin3_ast_flags(ev->ast, decl) & ODIN3_AST_F_PARAM_DECL_DT_MASK) >>
                  ODIN3_AST_F_PARAM_DECL_DT_SHIFT;
    param->spare = ODIN3_CPARAM_UNTYPED;
    if (ranged || dt == ODIN3_AST_DT_INTEGER) {
        param->spare = ODIN3_CPARAM_SIZED;
    } else if (type->is_signed) {
        param->spare = ODIN3_CPARAM_SIGNED;
    }
    param->a = type->width;
    param->sign = type->is_signed ? 1 : 0;
}

/* A memory (unpacked dimensions) cannot be a constant function's variable (as Yosys 0.55). */
static odin3_status in_check_memory(odin3_ceval *ev, uint32_t frame, odin3_ast_id dcl) {
    if (odin3_ast_nchild(ev->ast, dcl) <= 1) {
        return ODIN3_OK;
    }
    return ev_nonconst_fn(ev, frame, dcl, "it declares memory '%s'",
                          odin3_ast_name_str(ev->ast, dcl));
}

/* One DECLARATOR of a VAR_DECL/PORT_DECL: a variable, all x, with its initialiser if any. */
static odin3_status in_declare_var(odin3_ceval *ev, const ev_task *task, odin3_ast_id dcl,
                                   const in_new_local *proto) {
    odin3_status st = in_check_memory(ev, task->frame, dcl);
    in_new_local req = *proto;
    req.decl = dcl;
    uint32_t idx = 0;
    st = st == ODIN3_OK ? in_declare(ev, task->frame, &req, &idx) : st;
    odin3_ast_id init = odin3_ast_child(ev->ast, dcl, 0);
    bool is_port = odin3_ast_kind_of(ev->ast, (odin3_ast_id){task->node}) == ODIN3_AST_PORT_DECL;
    if (st != ODIN3_OK || init.v == 0 || is_port) {
        return st;
    }
    st = ev_push(ev, (ev_task){.op = FN_SET, .node = dcl.v, .frame = task->frame, .a = idx});
    odin3_etype ctx = {req.type.width, true, false};
    return st == ODIN3_OK ? ev_push(ev, in_root(init, task->frame, ctx)) : st;
}

odin3_status ev_do_decl(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id decl = {task->node};
    odin3_ast_kind kind = odin3_ast_kind_of(ev->ast, decl);
    if (kind != ODIN3_AST_VAR_DECL && kind != ODIN3_AST_PORT_DECL && kind != ODIN3_AST_PARAM_DECL) {
        return ODIN3_OK; /* other block items (2C keeps only declarations here) */
    }
    odin3_ast_id range = odin3_ast_child(ev->ast, decl, IN_RANGE);
    if (task->phase == 0 && range.v != 0) {
        ev_task then = *task;
        then.phase = 1;
        return in_push_range(ev, &then, range);
    }
    ev_decl_type req = {decl, range.v != 0, 0, 0};
    odin3_status st = range.v != 0 ? in_pop_range(ev, range, &req.msb, &req.lsb) : ODIN3_OK;
    in_new_local proto = {{0}, {0, false, false}, 0, 0, false, NULL};
    st = st == ODIN3_OK ? ev_decl_type_of(ev, &req, &proto.type, &proto.msb, &proto.lsb) : st;
    for (uint32_t k = odin3_ast_nchild(ev->ast, decl); k > 1 && st == ODIN3_OK; k--) {
        odin3_ast_id dcl = odin3_ast_child(ev->ast, decl, k - 1);
        if (kind != ODIN3_AST_PARAM_DECL) {
            st = in_declare_var(ev, task, dcl, &proto);
            continue;
        }
        ev_task param = {
            .op = FN_PARAM, .node = dcl.v, .frame = task->frame, .n = proto.msb, .m = proto.lsb};
        in_param_mode(ev, decl, &proto.type, &param);
        st = ev_push(ev, param);
        st = st == ODIN3_OK
                 ? ev_push(ev, in_root(odin3_ast_child(ev->ast, dcl, 0), task->frame, in_self()))
                 : st;
    }
    return st;
}

odin3_status ev_do_param(odin3_ceval *ev, const ev_task *task) {
    odin3_cval val = ev_pop_value(ev);
    const odin3_cparam_type decl = {task->spare, task->a, task->sign != 0}; /* EL-16 (cval.h) */
    odin3_status st = odin3_cval_to_param(ev_tmp(ev, task->frame), &val, &decl, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    bool sized = task->spare == ODIN3_CPARAM_SIZED;
    in_new_local req = {
        (odin3_ast_id){task->node},
        {val.width, (val.flags & ODIN3_CVAL_SIGNED) != 0, (val.flags & ODIN3_CVAL_UNSIZED) != 0},
        sized ? task->n : (int64_t)val.width - 1,
        sized ? task->m : 0,
        true,
        &val};
    if (val.width == 0) { /* a zero-width localparam reads as one 0 bit */
        st = ev_filled(ev_tmp(ev, task->frame), (odin3_etype){1, false, false}, ODIN3_BIT_0, &val);
        req.type.width = 1;
        req.msb = 0;
    }
    return st == ODIN3_OK ? in_declare(ev, task->frame, &req, NULL) : st;
}

/* Binds the arguments of a PORT_DECL's inputs in order; n counts the ones bound so far. */
odin3_status ev_do_bind(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id port = {task->node};
    uint32_t done = (uint32_t)task->n;
    if (done > 0) {
        odin3_ast_id dcl = odin3_ast_child(ev->ast, port, done);
        odin3_cval val = ev_pop_value(ev);
        odin3_status st =
            in_local_assign(ev, ev_tmp(ev, task->a), ev_find_local(ev, task->frame, dcl), &val);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (done >= in_declarators(ev->ast, port)) {
        return ODIN3_OK;
    }
    odin3_ast_id dcl = odin3_ast_child(ev->ast, port, done + 1);
    uint32_t idx = ev_find_local(ev, task->frame, dcl);
    odin3_ast_id arg = odin3_ast_child(ev->ast, ev_frame(ev, task->frame)->call, task->b + done);
    ev_task next = *task;
    next.n = (int64_t)done + 1;
    odin3_status st = ev_push(ev, next);
    odin3_etype ctx = {ev_local_at(ev, idx)->type.width, true, false};
    return st == ODIN3_OK ? ev_push(ev, in_root(arg, task->a, ctx))
                          : st; /* in the caller's frame */
}

odin3_status ev_do_set(odin3_ceval *ev, const ev_task *task) {
    odin3_cval val = ev_pop_value(ev);
    return in_local_assign(ev, ev_tmp(ev, task->frame), task->a, &val);
}

odin3_status ev_do_return(odin3_ceval *ev, const ev_task *task) {
    const odin3_ceval_frame *fr = ev_frame(ev, task->frame);
    uint32_t idx = ev_find_local(ev, task->frame, fr->decl);
    odin3_cval val = {0};
    odin3_status st = odin3_cval_copy(ev_tmp(ev, task->a), &ev_local_at(ev, idx)->value, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    ev_frame_pop(ev);
    st = ev_retype(ev, task->a); /* a recursive call typed the caller's nodes in its own frame */
    st = st == ODIN3_OK
             ? ev_fit(ev_tmp(ev, task->a), &val,
                      odin3_ctype_final(ev_types(ev, task->a), (odin3_ast_id){task->node}))
             : st;
    return st == ODIN3_OK ? ev_push_value(ev, &val) : st;
}

/* --- assignments --------------------------------------------------------------------------- */

static odin3_status in_push_id(odin3_vec *vec, uint32_t id) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    return ODIN3_OK;
}

/* One node off the walk: a CONCAT's parts go back on the stack (first on top), anything else
 * is the next atom. */
static odin3_status in_atom_step(const odin3_ceval *ev, odin3_vec *stack, odin3_vec *out) {
    odin3_ast_id node = {*(const uint32_t *)odin3_vec_cat(stack, stack->len - 1)};
    odin3_vec_pop(stack);
    if (odin3_ast_kind_of(ev->ast, node) != ODIN3_AST_CONCAT) {
        return in_push_id(out, node.v);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = odin3_ast_nchild(ev->ast, node); k > 0 && st == ODIN3_OK; k--) {
        st = in_push_id(stack, odin3_ast_child(ev->ast, node, k - 1).v);
    }
    return st;
}

/* The atoms of an lvalue, MSB first (a CONCAT's parts, nested ones flattened), into out. */
static odin3_status in_atoms(const odin3_ceval *ev, odin3_ast_id lhs, odin3_vec *out) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(uint32_t));
    odin3_vec_clear(out);
    odin3_status st = in_push_id(&stack, lhs.v);
    while (st == ODIN3_OK && stack.len > 0) {
        st = in_atom_step(ev, &stack, out);
    }
    odin3_vec_free(&stack);
    return st;
}

/* The index expressions of an atom, in evaluation order (innermost select first): the first
 * IN_MAX_ATOM_INDICES into out; returns how many it has (ev_do_assign refuses more). */
static uint32_t in_atom_indices(const odin3_ast *ast, odin3_ast_id atom, odin3_ast_id *out) {
    uint32_t count = 0;
    uint32_t depth = 0;
    (void)ev_select_base(ast, atom, &depth);
    for (uint32_t level = depth; level > 0; level--) {
        odin3_ast_id sel = atom;
        for (uint32_t k = 1; k < level; k++) {
            sel = odin3_ast_child(ast, sel, 0);
        }
        for (uint32_t k = 1; k < odin3_ast_nchild(ast, sel); k++) {
            odin3_ast_id idx = odin3_ast_child(ast, sel, k);
            if (idx.v != 0 && count < IN_MAX_ATOM_INDICES) {
                out[count] = idx;
            }
            count += idx.v != 0 ? 1U : 0U;
        }
    }
    return count;
}

/* Pushes the index expressions of an lvalue atom (the last runs first); more than
 * IN_MAX_ATOM_INDICES is a located error (a 7-dimensional word with a part select). */
static odin3_status in_push_indices(odin3_ceval *ev, uint32_t frame, odin3_ast_id atom) {
    odin3_ast_id idx[IN_MAX_ATOM_INDICES];
    uint32_t count = in_atom_indices(ev->ast, atom, idx);
    if (count > IN_MAX_ATOM_INDICES) {
        return ev_error(ev, atom,
                        "assignment to a select with more than %d indices is not supported",
                        IN_MAX_ATOM_INDICES);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t j = count; j > 0 && st == ODIN3_OK; j--) {
        st = ev_push(ev, in_root(idx[j - 1], frame, in_self()));
    }
    return st;
}

static odin3_status in_assign_begin(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    odin3_ast_id lhs = odin3_ast_child(ev->ast, stmt, 0);
    odin3_status st = ev_root_open(ev, task->frame, lhs, in_self()); /* closed by ST_STORE */
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t width = odin3_ctype_final(ev_types(ev, task->frame), lhs).width;
    st = ev_push(
        ev,
        (ev_task){.op = ST_ASSIGN, .node = stmt.v, .frame = task->frame, .phase = 1, .a = width});
    odin3_etype ctx = {width, true, false};
    return st == ODIN3_OK ? ev_push(ev, in_root(odin3_ast_child(ev->ast, stmt, IN_ASSIGN_RHS),
                                                task->frame, ctx))
                          : st;
}

/* Phase 1: the right side is on the stack; the lvalue's indices are evaluated next. */
odin3_status ev_do_assign(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id lhs = odin3_ast_child(ev->ast, (odin3_ast_id){task->node}, 0);
    odin3_vec atoms;
    odin3_vec_init(&atoms, sizeof(uint32_t));
    odin3_status st = in_atoms(ev, lhs, &atoms);
    st = st == ODIN3_OK
             ? ev_push(ev,
                       (ev_task){
                           .op = ST_STORE, .node = task->node, .frame = task->frame, .a = task->a})
             : st;
    for (size_t k = atoms.len; k > 0 && st == ODIN3_OK; k--) {
        st = in_push_indices(ev, task->frame,
                             (odin3_ast_id){*(const uint32_t *)odin3_vec_cat(&atoms, k - 1)});
    }
    odin3_vec_free(&atoms);
    return st;
}

/* One atom's write: its node, its bits of the right side, its index values. */
typedef struct in_write {
    odin3_ast_id atom;
    odin3_cval part;
    size_t depth; /* the value-stack depth of its first index */
    uint32_t n_idx;
} in_write;

/* The index value idx of wr (from the stack), as an integer (false: x or z). */
static bool in_index(const odin3_ceval *ev, const in_write *wr, uint32_t idx, int64_t *out) {
    const odin3_cval *val = ev_peek_value(ev, wr->depth - idx);
    *out = 0;
    return val->width == 0 || odin3_cval_to_i64(val, out);
}

/* A variable's current value through the store (base frame) for a read-merge-write. */
static odin3_status in_store_get(odin3_ceval *ev, uint32_t frame, odin3_ast_id ref,
                                 odin3_cval *out) {
    const odin3_ceval_store *store = ev_frame(ev, frame)->store;
    odin3_cval val = {0};
    bool known = false;
    odin3_status st = store->read(store->user, ref, &val, &known);
    if (st == ODIN3_OK && !known) {
        uint32_t depth = 0;
        st = ev_nonconst_read(ev, frame, ref,
                              odin3_ast_name(ev->ast, ev_select_base(ev->ast, ref, &depth)));
    }
    return st == ODIN3_OK ? odin3_cval_copy(ev_tmp(ev, frame), &val, out) : st;
}

/* The word's indices for the store call (false: an x index, and the write is dropped). */
static odin3_status in_set_word(odin3_ceval *ev, const in_write *wr, uint32_t rank, bool *known) {
    odin3_vec_clear(&ev->word);
    *known = true;
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; k < rank && st == ODIN3_OK; k++) {
        int64_t *slot = odin3_vec_push(&ev->word);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        *known = *known && (slot == NULL || in_index(ev, wr, k, slot));
    }
    return st;
}

/* What an assignment writes: a local, or (through the store) a variable or a memory word. */
typedef struct in_target {
    odin3_ast_id ref; /* the IDENT, HIER_NAME or memory-word SELECT written */
    uint32_t local;   /* UINT32_MAX: not a local */
    uint32_t rank;    /* a memory word's index count */
    odin3_csym sym;   /* the host's symbol (not a local) */
    int64_t msb, lsb; /* its declared range */
} in_target;

static odin3_status in_misuse(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "odin3_ceval: %s", why);
    return ODIN3_ERR_INVALID_ARG;
}

/* The target of ref: a name, or a SELECT that must be a memory word. */
static odin3_status in_target_of(odin3_ceval *ev, uint32_t frame, odin3_ast_id ref,
                                 in_target *out) {
    *out = (in_target){ref, UINT32_MAX, 0, {0}, 0, 0};
    if (odin3_ast_kind_of(ev->ast, ref) == ODIN3_AST_SELECT) {
        odin3_status st = ev_word_sym(ev, frame, ref, &out->sym);
        if (st == ODIN3_OK && out->sym.kind == ODIN3_CSYM_NONE) {
            return in_misuse("an lvalue select of a select that is not a memory word");
        }
        (void)ev_select_base(ev->ast, ref, &out->rank);
        out->msb = out->sym.msb;
        out->lsb = out->sym.lsb;
        return st;
    }
    ev_name nm;
    odin3_status st = ev_resolve(ev, frame, ref, &nm);
    if (st != ODIN3_OK) {
        return st;
    }
    if (nm.is_local) {
        const ev_local *loc = ev_local_at(ev, nm.local);
        out->local = loc->is_const ? UINT32_MAX : nm.local;
        out->sym.name = loc->name;
        out->msb = loc->msb;
        out->lsb = loc->lsb;
        return ODIN3_OK;
    }
    out->sym = nm.sym;
    out->msb = nm.sym.msb;
    out->lsb = nm.sym.lsb;
    return ODIN3_OK;
}

/* A non-local target: not constant in a function frame, and without a store. */
static odin3_status in_check_store(odin3_ceval *ev, uint32_t frame, const in_target *tg) {
    if (tg->local != UINT32_MAX) {
        return ODIN3_OK;
    }
    if (in_is_function(ev, frame)) {
        return ev_nonconst_fn(ev, frame, tg->ref, "it assigns '%s', which is not local",
                              ev_str(ev, tg->sym.name));
    }
    if (ev_frame(ev, frame)->store == NULL) {
        return ev_nonconst_fn(ev, frame, tg->ref, "it assigns '%s'", ev_str(ev, tg->sym.name));
    }
    return ODIN3_OK;
}

/* Writes the whole value of a target. */
static odin3_status in_put(odin3_ceval *ev, uint32_t frame, const in_target *tg,
                           const odin3_cval *val) {
    if (tg->local != UINT32_MAX) {
        return in_local_assign(ev, ev_tmp(ev, frame), tg->local, val);
    }
    odin3_cval out = *val; /* the variable's (or word's) declared width and sign */
    odin3_status st = ev_fit(ev_tmp(ev, frame), &out, tg->sym.type);
    const odin3_ceval_store *store = ev_frame(ev, frame)->store;
    return st == ODIN3_OK ? store->write(store->user, tg->ref, &out) : st;
}

/* Bits [lo, lo + part.width) of cur (positions outside it are dropped) become part. */
static void in_merge(odin3_cval *cur, const odin3_cval *part, int64_t lo) {
    for (uint32_t k = 0; k < part->width; k++) {
        int64_t pos = lo + (int64_t)k;
        if (pos < 0 || pos >= (int64_t)cur->width) {
            continue;
        }
        uint64_t mask = (uint64_t)1 << ((uint64_t)pos % ODIN3_CVAL_WORD_BITS);
        size_t word = (size_t)((uint64_t)pos / ODIN3_CVAL_WORD_BITS);
        uint64_t abit = part->aval[k / ODIN3_CVAL_WORD_BITS] >> (k % ODIN3_CVAL_WORD_BITS) & 1U;
        uint64_t bbit = part->bval[k / ODIN3_CVAL_WORD_BITS] >> (k % ODIN3_CVAL_WORD_BITS) & 1U;
        cur->aval[word] = (cur->aval[word] & ~mask) | (abit != 0 ? mask : 0);
        cur->bval[word] = (cur->bval[word] & ~mask) | (bbit != 0 ? mask : 0);
    }
}

/* The lowest position an ordinary select writes, from its index values at `first`; false for
 * an x index (the write is dropped). */
static bool in_select_low(const odin3_ceval *ev, const in_write *wr, uint32_t first,
                          const in_target *tg, int64_t *out) {
    ev_sel_req req = {odin3_ast_sub(ev->ast, wr->atom), 0, 0, tg->msb, tg->lsb};
    bool known = in_index(ev, wr, first, &req.first);
    if (req.sub != ODIN3_AST_SELECT_BIT) {
        known = in_index(ev, wr, first + 1, &req.second) && known;
    }
    *out = ev_select_span(&req).lo;
    return known;
}

/* A select of a variable or of a memory word: read it, merge the part, write it whole. */
static odin3_status in_write_part(odin3_ceval *ev, uint32_t frame, const in_write *wr) {
    odin3_ast_id obj = odin3_ast_child(ev->ast, wr->atom, 0);
    in_target tg;
    odin3_status st = in_target_of(ev, frame, obj, &tg);
    st = st == ODIN3_OK ? in_check_store(ev, frame, &tg) : st;
    bool known = true;
    if (st == ODIN3_OK && tg.rank > 0) {
        st = in_set_word(ev, wr, tg.rank, &known);
    }
    int64_t lo = 0;
    if (st != ODIN3_OK || !known || !in_select_low(ev, wr, tg.rank, &tg, &lo)) {
        return st; /* an x index: the write is dropped */
    }
    odin3_cval cur = {0};
    if (tg.local != UINT32_MAX) {
        st = odin3_cval_copy(ev_tmp(ev, frame), &ev_local_at(ev, tg.local)->value, &cur);
    } else {
        st = in_store_get(ev, frame, obj, &cur);
    }
    if (st == ODIN3_OK) {
        in_merge(&cur, &wr->part, lo);
        st = in_put(ev, frame, &tg, &cur);
    }
    return st;
}

/* One atom: a whole name, a whole memory word, or a select of either. */
static odin3_status in_write_atom(odin3_ceval *ev, uint32_t frame, const in_write *wr) {
    odin3_ast_id atom = wr->atom;
    if (odin3_ast_kind_of(ev->ast, atom) == ODIN3_AST_SELECT) {
        odin3_csym sym;
        odin3_status st = ev_word_sym(ev, frame, atom, &sym);
        if (st != ODIN3_OK || sym.kind == ODIN3_CSYM_NONE) {
            st = st == ODIN3_OK ? in_write_part(ev, frame, wr) : st;
            odin3_vec_clear(&ev->word);
            return st;
        }
    }
    in_target tg;
    odin3_status st = in_target_of(ev, frame, atom, &tg);
    st = st == ODIN3_OK ? in_check_store(ev, frame, &tg) : st;
    bool known = true;
    if (st == ODIN3_OK && tg.rank > 0) {
        st = in_set_word(ev, wr, tg.rank, &known);
    }
    st = st == ODIN3_OK && known ? in_put(ev, frame, &tg, &wr->part) : st; /* x index: dropped */
    odin3_vec_clear(&ev->word);
    return st;
}

/* The right side, then every atom's index values, are on the stack. */
odin3_status ev_do_store(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id lhs = odin3_ast_child(ev->ast, (odin3_ast_id){task->node}, 0);
    odin3_vec atoms;
    odin3_vec_init(&atoms, sizeof(uint32_t));
    odin3_status st = in_atoms(ev, lhs, &atoms);
    size_t total = 0;
    odin3_ast_id idx[IN_MAX_ATOM_INDICES];
    for (size_t k = 0; k < atoms.len && st == ODIN3_OK; k++) {
        total += in_atom_indices(ev->ast,
                                 (odin3_ast_id){*(const uint32_t *)odin3_vec_cat(&atoms, k)}, idx);
    }
    odin3_cval rhs = st == ODIN3_OK ? *ev_peek_value(ev, total) : (odin3_cval){0};
    int64_t hi = task->a; /* the bits above an atom's: atoms run MSB first */
    size_t depth = total;
    for (size_t k = 0; k < atoms.len && st == ODIN3_OK; k++) {
        in_write wr = {{*(const uint32_t *)odin3_vec_cat(&atoms, k)}, {0}, depth - 1, 0};
        wr.n_idx = in_atom_indices(ev->ast, wr.atom, idx);
        uint32_t width = odin3_ctype_final(ev_types(ev, task->frame), wr.atom).width;
        hi -= width;
        const odin3_cval_part part = {&rhs, hi, width};
        st =
            rhs.width != 0 ? odin3_cval_select(ev_tmp(ev, task->frame), &part, &wr.part) : ODIN3_OK;
        st = st == ODIN3_OK ? in_write_atom(ev, task->frame, &wr) : st;
        depth -= wr.n_idx;
    }
    odin3_vec_free(&atoms);
    for (size_t k = 0; k <= total && st == ODIN3_OK; k++) {
        (void)ev_pop_value(ev);
    }
    ev_root_close(ev, lhs);
    return st;
}

/* --- control ------------------------------------------------------------------------------- */

odin3_status ev_do_if(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    odin3_cval cond = ev_pop_value(ev);
    bool taken = cond.width != 0 && odin3_cval_truth(&cond) == ODIN3_BIT_1; /* x: else */
    odin3_ast_id next = odin3_ast_child(ev->ast, stmt, taken ? 1 : 2);
    return next.v != 0 ? ev_push(ev, in_task(ST_STMT, next, task->frame)) : ODIN3_OK;
}

/* A hybrid (spec §4.3's declared divergences): the wildcards are IEEE 1364-2005 §9.5.1's, so
 * casez ignores the bits that are z in the item or in the case value and casex those that are
 * x or z in either (fix round 1, finding 6); every other bit must be a known 0 or 1 on both
 * sides and equal, as Yosys's == truth: a plain case item with an x or z bit, and a non-wildcard
 * x bit of a casez item or value, never match (IEEE's === would). */
static bool in_case_match(const odin3_cval *val, const odin3_cval *item, uint32_t sub) {
    if (val->width == 0 || item->width == 0) {
        return val->width == item->width;
    }
    for (uint32_t word = 0; word < odin3_cval_words(val->width); word++) {
        uint64_t ignore = 0;
        if (sub == ODIN3_AST_CASE_CASEZ) {
            ignore = (item->bval[word] & ~item->aval[word]) | (val->bval[word] & ~val->aval[word]);
        } else if (sub == ODIN3_AST_CASE_CASEX) {
            ignore = item->bval[word] | val->bval[word];
        }
        uint64_t unknown = (val->bval[word] | item->bval[word]) & ~ignore;
        uint64_t differ = (val->aval[word] ^ item->aval[word]) & ~ignore;
        if (unknown != 0 || differ != 0) {
            return false;
        }
    }
    return true;
}

/* Folds expr's self-determined type into the case type *acc. */
static odin3_status in_case_fold(odin3_ceval *ev, uint32_t frame, odin3_ast_id expr,
                                 odin3_etype *acc) {
    uint64_t size = 0; /* typing every item again each time: the step budget sees it */
    odin3_status st = ev_subtree_size(ev, expr, &size);
    st = st == ODIN3_OK ? ev_steps(ev, expr, size) : st;
    st = st == ODIN3_OK ? ev_root_open(ev, frame, expr, in_self()) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_etype self = odin3_ctype_final(ev_types(ev, frame), expr);
    ev_root_close(ev, expr);
    acc->width = self.width > acc->width ? self.width : acc->width;
    acc->is_signed = acc->is_signed && self.is_signed;
    return ODIN3_OK;
}

/* The case width and sign (EL-36): the widest of e and every item expression, signed only if
 * every one is. */
static odin3_status in_case_type(odin3_ceval *ev, const ev_task *task, odin3_etype *out) {
    odin3_ast_id stmt = {task->node};
    *out = (odin3_etype){0, true, false};
    odin3_status st = in_case_fold(ev, task->frame, odin3_ast_child(ev->ast, stmt, 0), out);
    for (uint32_t k = 1; k < odin3_ast_nchild(ev->ast, stmt) && st == ODIN3_OK; k++) {
        odin3_ast_id item = odin3_ast_child(ev->ast, stmt, k);
        for (uint32_t j = 1; j < odin3_ast_nchild(ev->ast, item) && st == ODIN3_OK; j++) {
            st = in_case_fold(ev, task->frame, odin3_ast_child(ev->ast, item, j), out);
        }
    }
    return st;
}

/* The next item expression after (item b, expression n) to compare, or the end. */
static odin3_status in_case_next(odin3_ceval *ev, ev_task scan) {
    odin3_ast_id stmt = {scan.node};
    uint32_t items = odin3_ast_nchild(ev->ast, stmt);
    while (scan.b < items) {
        odin3_ast_id item = odin3_ast_child(ev->ast, stmt, scan.b);
        if ((odin3_ast_flags(ev->ast, item) & ODIN3_AST_F_CASE_ITEM_DEFAULT) != 0) {
            scan.m = scan.m < 0 ? scan.b : scan.m;
        } else if ((uint32_t)scan.n + 1 < odin3_ast_nchild(ev->ast, item)) {
            odin3_ast_id expr = odin3_ast_child(ev->ast, item, (uint32_t)scan.n + 1);
            scan.phase = 2;
            odin3_status st = ev_push(ev, scan);
            odin3_etype ctx = {scan.a, scan.sign != 0, false};
            return st == ODIN3_OK ? ev_push(ev, in_root(expr, scan.frame, ctx)) : st;
        }
        scan.b++;
        scan.n = 0;
    }
    (void)ev_pop_value(ev); /* no item matched: the default, if any */
    odin3_ast_id def = odin3_ast_child(ev->ast, stmt, scan.m >= 0 ? (uint32_t)scan.m : 0);
    return scan.m >= 0 ? ev_push(ev, in_task(ST_STMT, odin3_ast_child(ev->ast, def, 0), scan.frame))
                       : ODIN3_OK;
}

static odin3_status in_case_begin(odin3_ceval *ev, const ev_task *task) {
    odin3_etype type;
    odin3_status st = in_case_type(ev, task, &type);
    ev_task next = *task;
    next.op = ST_CASE;
    next.phase = 1;
    next.a = type.width;
    next.sign = type.is_signed ? 1 : 0;
    st = st == ODIN3_OK ? ev_push(ev, next) : st;
    odin3_ast_id expr = odin3_ast_child(ev->ast, (odin3_ast_id){task->node}, 0);
    return st == ODIN3_OK ? ev_push(ev, in_root(expr, task->frame, type)) : st;
}

odin3_status ev_do_case(odin3_ceval *ev, const ev_task *task) {
    ev_task scan = *task;
    if (task->phase == 1) { /* the case value is on the stack: scan from the first item */
        scan.b = 1;
        scan.n = 0;
        scan.m = -1;
        return in_case_next(ev, scan);
    }
    odin3_cval item = ev_pop_value(ev);
    const odin3_cval *val = ev_peek_value(ev, 0);
    odin3_ast_id stmt = {task->node};
    if (in_case_match(val, &item, odin3_ast_sub(ev->ast, stmt))) {
        (void)ev_pop_value(ev);
        odin3_ast_id body = odin3_ast_child(ev->ast, odin3_ast_child(ev->ast, stmt, task->b), 0);
        return ev_push(ev, in_task(ST_STMT, body, task->frame));
    }
    scan.n++;
    return in_case_next(ev, scan);
}

static odin3_status in_loop_cap(const odin3_ceval *ev, odin3_ast_id loop, int64_t count) {
    uint32_t cap = odin3_elab_limits_get()->max_loop_iterations;
    if (count <= (int64_t)cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, loop, "loop exceeds %" PRIu32 " iterations", cap);
}

static bool in_is_for(const odin3_ceval *ev, const ev_task *task) {
    return odin3_ast_kind_of(ev->ast, (odin3_ast_id){task->node}) == ODIN3_AST_FOR;
}

/* Pushes the loop task again (in phase) and then a statement or an expression to run first. */
static odin3_status in_loop_then(odin3_ceval *ev, ev_task next, ev_task first) {
    odin3_status st = ev_push(ev, next);
    return st == ODIN3_OK ? ev_push(ev, first) : st;
}

/* LP_DECIDE: the condition is on the stack; true runs the body (and a FOR's step). */
static odin3_status in_loop_decide(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id loop = {task->node};
    odin3_cval cond = ev_pop_value(ev);
    if (cond.width == 0 || odin3_cval_truth(&cond) != ODIN3_BIT_1) {
        return ODIN3_OK; /* false or x: the loop ends */
    }
    ev_task next = *task;
    next.phase = LP_TEST;
    next.n = task->n + 1;
    odin3_status st = in_loop_cap(ev, loop, next.n);
    st = st == ODIN3_OK ? ev_push(ev, next) : st;
    bool is_for = in_is_for(ev, task);
    if (st == ODIN3_OK && is_for) {
        st = ev_push(ev,
                     in_task(ST_STMT, odin3_ast_child(ev->ast, loop, IN_LOOP_STEP), task->frame));
    }
    odin3_ast_id body = odin3_ast_child(ev->ast, loop, is_for ? IN_FOR_BODY : 1);
    return st == ODIN3_OK ? ev_push(ev, in_task(ST_STMT, body, task->frame)) : st;
}

/* LP_COUNT: a REPEAT's count is on the stack (x, z or negative: no iteration). */
static odin3_status in_loop_count(odin3_ceval *ev, const ev_task *task) {
    ev_task next = *task;
    bool known = true;
    ev_pop_int(ev, &next.n, &known);
    next.n = known && next.n > 0 ? next.n : 0;
    odin3_status st = in_loop_cap(ev, (odin3_ast_id){task->node}, next.n);
    next.phase = LP_REPEAT;
    return st == ODIN3_OK ? ev_push(ev, next) : st;
}

/* FOR (init, cond, step, body), WHILE (cond, body), REPEAT (count, body). */
odin3_status ev_do_loop(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id loop = {task->node};
    ev_task next = *task;
    switch (task->phase) {
    case LP_START: /* FOR: its initialisation first */
        next.phase = LP_TEST;
        return in_loop_then(ev, next,
                            in_task(ST_STMT, odin3_ast_child(ev->ast, loop, 0), task->frame));
    case LP_TEST: {
        next.phase = LP_DECIDE;
        odin3_ast_id cond = odin3_ast_child(ev->ast, loop, in_is_for(ev, task) ? 1 : 0);
        return in_loop_then(ev, next, in_root(cond, task->frame, in_self()));
    }
    case LP_DECIDE:
        return in_loop_decide(ev, task);
    case LP_COUNT:
        return in_loop_count(ev, task);
    default: /* LP_REPEAT */
        if (task->n <= 0) {
            return ODIN3_OK;
        }
        next.n = task->n - 1;
        return in_loop_then(ev, next,
                            in_task(ST_STMT, odin3_ast_child(ev->ast, loop, 1), task->frame));
    }
}

/* --- blocks and statements ----------------------------------------------------------------- */

odin3_status ev_do_block_end(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id list = odin3_ast_child(ev->ast, (odin3_ast_id){task->node}, 0);
    for (uint32_t k = 0; k < odin3_ast_nchild(ev->ast, list); k++) {
        odin3_ast_id decl = odin3_ast_child(ev->ast, list, k);
        for (uint32_t j = 1; j < odin3_ast_nchild(ev->ast, decl); j++) {
            uint32_t idx = ev_find_local(ev, task->frame, odin3_ast_child(ev->ast, decl, j));
            if (idx != UINT32_MAX) {
                ((ev_local *)odin3_vec_at(&ev->locals, idx))->visible = false;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status in_block(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id block = {task->node};
    odin3_ast_id list = odin3_ast_child(ev->ast, block, 0);
    odin3_status st =
        list.v != 0 ? ev_push(ev, in_task(ST_BLOCK_END, block, task->frame)) : ODIN3_OK;
    for (uint32_t k = odin3_ast_nchild(ev->ast, block); k > 1 && st == ODIN3_OK; k--) {
        st = ev_push(ev, in_task(ST_STMT, odin3_ast_child(ev->ast, block, k - 1), task->frame));
    }
    st = st == ODIN3_OK ? in_push_decls(ev, list, task->frame, true) : st;
    return st == ODIN3_OK ? in_push_decls(ev, list, task->frame, false) : st;
}

/* The $display family (§4.6: ignored inside a function, as Yosys), by exact name: the eight
 * bases with no suffix or b, h or o (so $writememh is not one; fix round 1, finding 8). */
static bool in_display_task(const char *name) {
    static const char *const FAMILY[] = {"$display",  "$write",  "$strobe",  "$monitor",
                                         "$fdisplay", "$fwrite", "$fstrobe", "$fmonitor"};
    for (size_t k = 0; k < sizeof FAMILY / sizeof FAMILY[0]; k++) {
        size_t len = strlen(FAMILY[k]);
        if (strncmp(name, FAMILY[k], len) != 0) {
            continue;
        }
        const char *rest = name + len;
        if (rest[0] == '\0' || (rest[1] == '\0' && strchr("bho", rest[0]) != NULL)) {
            return true;
        }
    }
    return false;
}

static odin3_status in_task_call(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    const char *name = odin3_ast_name_str(ev->ast, stmt);
    if ((odin3_ast_flags(ev->ast, stmt) & ODIN3_AST_F_TASK_CALL_SYSTEM) == 0) {
        return ev_nonconst_fn(ev, task->frame, stmt, "it calls task '%s'", name);
    }
    if (in_display_task(name)) {
        return ODIN3_OK;
    }
    return ev_nonconst_fn(ev, task->frame, stmt, "it calls system task '%s'", name);
}

/* The statements §4.6 does not run: their §9.1 texts (K12 for a function's own). */
static odin3_status in_unsupported(odin3_ceval *ev, const ev_task *task, odin3_ast_kind kind) {
    odin3_ast_id stmt = {task->node};
    switch (kind) {
    case ODIN3_AST_PAR_BLOCK:
        return ev_error(ev, stmt, "'fork ... join' is not supported");
    case ODIN3_AST_WAIT:
        return ev_error(ev, stmt, "'wait' is not supported");
    case ODIN3_AST_DISABLE:
        return ev_error(ev, stmt, "'disable' is not supported");
    case ODIN3_AST_EVENT_TRIGGER:
        return ev_error(ev, stmt, "event trigger '->' is not supported");
    case ODIN3_AST_FOREVER:
        return ev_error(ev, stmt, "'forever' is not supported");
    case ODIN3_AST_PROC_CONT_ASSIGN:
        return ev_error(ev, stmt, "procedural '%s' is not supported",
                        odin3_ast_sub_name(ev->ast, stmt));
    case ODIN3_AST_TIMING_STMT:
        if (in_is_function(ev, task->frame)) {
            return ev_error(ev, stmt, "timing control in function '%s' is not supported",
                            in_fn_name(ev, task->frame));
        }
        return ev_nonconst_fn(ev, task->frame, stmt, "it waits%s", ""); /* exec: an event */
    default:
        return in_misuse("a statement kind the interpreter does not know");
    }
}

static odin3_status in_timing(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    odin3_ast_id control = odin3_ast_child(ev->ast, stmt, 0);
    if (in_is_function(ev, task->frame) || odin3_ast_kind_of(ev->ast, control) != ODIN3_AST_DELAY) {
        return in_unsupported(ev, task, ODIN3_AST_TIMING_STMT);
    }
    odin3_ast_id inner = odin3_ast_child(ev->ast, stmt, 1); /* exec: a delay is ignored */
    return inner.v != 0 ? ev_push(ev, in_task(ST_STMT, inner, task->frame)) : ODIN3_OK;
}

/* FOR and WHILE start at their tests (a FOR at its initialisation); REPEAT at its count. */
static odin3_status in_loop_begin(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    odin3_ast_kind kind = odin3_ast_kind_of(ev->ast, stmt);
    ev_task loop = {.op = ST_LOOP, .node = stmt.v, .frame = task->frame};
    if (kind != ODIN3_AST_REPEAT) {
        loop.phase = kind == ODIN3_AST_FOR ? LP_START : LP_TEST;
        return ev_push(ev, loop);
    }
    loop.phase = LP_COUNT;
    return in_loop_then(ev, loop,
                        in_root(odin3_ast_child(ev->ast, stmt, 0), task->frame, in_self()));
}

static odin3_status in_if_begin(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    return in_loop_then(ev, in_task(ST_IF, stmt, task->frame),
                        in_root(odin3_ast_child(ev->ast, stmt, 0), task->frame, in_self()));
}

static odin3_status in_nonblocking(odin3_ceval *ev, const ev_task *task) {
    if (in_is_function(ev, task->frame)) {
        return ev_error(ev, (odin3_ast_id){task->node},
                        "nonblocking assignment in function '%s' is not supported",
                        in_fn_name(ev, task->frame));
    }
    return in_assign_begin(ev, task); /* exec: both kinds just set the value (EL-40) */
}

odin3_status ev_do_stmt(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id stmt = {task->node};
    odin3_status st = ev_step(ev, stmt);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_kind kind = odin3_ast_kind_of(ev->ast, stmt);
    switch (kind) {
    case ODIN3_AST_SEQ_BLOCK:
        return in_block(ev, task);
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        return in_nonblocking(ev, task);
    case ODIN3_AST_BLOCKING_ASSIGN:
        return in_assign_begin(ev, task);
    case ODIN3_AST_IF:
        return in_if_begin(ev, task);
    case ODIN3_AST_CASE:
        return in_case_begin(ev, task);
    case ODIN3_AST_FOR:
    case ODIN3_AST_WHILE:
    case ODIN3_AST_REPEAT:
        return in_loop_begin(ev, task);
    case ODIN3_AST_NULL_STMT:
        return ODIN3_OK;
    case ODIN3_AST_TASK_CALL:
        return in_task_call(ev, task);
    case ODIN3_AST_TIMING_STMT:
        return in_timing(ev, task);
    default:
        return in_unsupported(ev, task, kind);
    }
}
