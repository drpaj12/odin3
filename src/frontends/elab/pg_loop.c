/*
 * pg_loop.c — `for` and `repeat` in a process (2D Task 23; EL-37): both are unrolled. A loop is a
 * PG_F_LOOP frame on the statement stack; each step of it runs the init, tests the condition (it
 * must be constant given what the generator knows: `rv` bits that are constants), pushes the body
 * or runs the step. The loop variable is bound for the AST constant test (e2->bound), so a branch
 * whose condition reads it folds in every iteration (Task 22's carry). Its assignments are the
 * ordinary blocking assignments: the variable is an output of the block only when the module reads
 * it outside the loops it is the variable of (EL-35, divergence 5; a variable that is not an
 * output has no `lv`, so only `rv` records its values), and its last assignment is then its final
 * value. The caps are the loop's own (ODIN3_ELAB_MAX_LOOP_ITERATIONS) and the run's total, which
 * counts on from the generate loops stage 1 unrolled (point (k)).
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LOOP_INIT = 0, LOOP_TEST = 1, LOOP_STEP = 2 };
enum { FOR_INIT = 0, FOR_COND = 1, FOR_STEP = 2, FOR_BODY = 3 };
enum { REPEAT_COUNT = 0, REPEAT_BODY = 1 };

odin3_sym_id pg_assign_symbol(const odin3_pg *pg, odin3_ast_id assign) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id lhs = odin3_ast_child(ast, assign, 0);
    odin3_ast_kind kind = odin3_ast_kind_of(ast, lhs);
    if (odin3_ast_kind_of(ast, assign) != ODIN3_AST_BLOCKING_ASSIGN ||
        (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME)) {
        return (odin3_sym_id){0};
    }
    return odin3_elab_sym_of(&pg->e2->bind, lhs);
}

/* --- the variable's value is constant -------------------------------------------------------- */

/* Whether every bit of the variable (or net) that node names is a constant in `rv`. */
static bool name_is_constant(const odin3_pg *pg, const odin3_symbol *rec) {
    if (rec->n_unpacked > 0 || rec->ir_wire.v == 0) {
        return false;
    }
    uint32_t width = odin3_wire_width(pg->e2->mod, rec->ir_wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_sigbit value = 0;
        if (!pg_rv_get(pg, odin3_wire_net(pg->e2->mod, rec->ir_wire, bit), &value) ||
            !odin3_sb_is_const(value)) {
            return false;
        }
    }
    return true;
}

/* The name of node when it is not a constant now: a variable with a bit `rv` does not hold as a
 * constant, a memory, a user function; NULL for a constant or a name that is none of these. */
static const char *node_nonconst(odin3_pg *pg, odin3_ast_id node) {
    odin3_elab2 *e2 = pg->e2;
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
    if (kind == ODIN3_AST_CALL) {
        bool user = (odin3_ast_flags(e2->elab, node) & ODIN3_AST_F_CALL_SYSTEM) == 0;
        return user ? odin3_ast_name_str(e2->elab, node) : NULL;
    }
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        return NULL;
    }
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, node));
    bool variable = rec != NULL && (rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR);
    return variable && !name_is_constant(pg, rec) ? odin3_ast_name_str(e2->elab, node) : NULL;
}

/* The first name in expr that is not a constant now (*found), NULL when it is constant. */
static odin3_status nonconst_name(odin3_pg *pg, odin3_ast_id expr, const char **found) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, pg->e2->elab, expr);
    *found = NULL;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_event ev;
    odin3_ast_id node;
    while (*found == NULL && odin3_ast_cursor_next(&cur, &ev, &node)) {
        *found = ev == ODIN3_AST_ENTER ? node_nonconst(pg, node) : NULL;
    }
    odin3_ast_cursor_free(&cur);
    return ODIN3_OK;
}

/* A constant expression's value: true when some bit is a 1. */
static odin3_status const_truth(odin3_pg *pg, odin3_ast_id expr, bool *truth) {
    odin3_elab2_reads reads = pg_reads(pg);
    odin3_elab2_expr_req req = {expr, {0, false, false}, NULL, &reads};
    odin3_sig sig;
    odin3_status st = odin3_elab2_expr(pg->e2, &req, &sig);
    *truth = false;
    for (uint32_t i = 0; st == ODIN3_OK && i < sig.width; i++) {
        *truth = *truth || sig.bits[i] == odin3_sb_of_bit(ODIN3_BIT_1);
    }
    return st;
}

/* `for-loop <what> is not constant (it reads 'x')` at the loop. */
typedef struct need {
    odin3_ast_id loop,
        expr; /* the statement the error is at, and the expression that must be constant */
    const char *what;
} need;

static odin3_status need_constant(odin3_pg *pg, const need *nd) {
    const char *name = NULL;
    odin3_status st = nonconst_name(pg, nd->expr, &name);
    if (st != ODIN3_OK || name == NULL) {
        return st;
    }
    odin3_elab2_error(pg->e2, odin3_ast_loc(pg->e2->elab, nd->loop),
                      "for-loop %s is not constant (it reads '%s')", nd->what, name);
    return ODIN3_ERR_PARSE;
}

/* --- opening ---------------------------------------------------------------------------------- */

static odin3_status loop_record(odin3_pg *pg, odin3_ast_id node, odin3_sym_id sym, int64_t times) {
    pg_loop *loop = odin3_vec_push(&pg->loops);
    if (loop == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *loop = (pg_loop){node, sym, 0, times};
    odin3_elab2 *e2 = pg->e2;
    if (sym.v != 0) {
        if (e2->bound == NULL) {
            e2->bound = odin3_u64map_create(0);
        }
        odin3_status st = e2->bound != NULL ? odin3_u64map_put(e2->bound, (odin3_kv){sym.v, 1})
                                            : ODIN3_ERR_NO_MEMORY;
        if (st != ODIN3_OK) {
            odin3_vec_pop(&pg->loops);
            return st;
        }
    }
    pg_frame frame = {PG_F_LOOP, node, LOOP_INIT, 0, 0, (uint32_t)pg->loops.len - 1U};
    odin3_status st = pg_frame_push(pg, &frame);
    if (st != ODIN3_OK) {
        odin3_vec_pop(&pg->loops);
        if (sym.v != 0) {
            (void)odin3_u64map_remove(e2->bound, sym.v);
        }
    }
    return st;
}

/* for (i = a; c; i = s) body: both assignments must write the same plain variable. */
static odin3_status open_for(odin3_pg *pg, odin3_ast_id node) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_sym_id init = pg_assign_symbol(pg, odin3_ast_child(ast, node, FOR_INIT));
    odin3_sym_id step = pg_assign_symbol(pg, odin3_ast_child(ast, node, FOR_STEP));
    if (init.v == 0 || init.v != step.v) {
        odin3_elab2_error(pg->e2, odin3_ast_loc(ast, node),
                          "for-loop initialisation and step must assign the same variable");
        return ODIN3_ERR_PARSE;
    }
    return loop_record(pg, node, init, 0);
}

/* repeat (n) body: n is a NUMBER after stage 1; an x, z or negative count runs nothing. */
static odin3_status open_repeat(odin3_pg *pg, odin3_ast_id node) {
    int64_t times = 0;
    bool known = false;
    odin3_status st = odin3_elab2_number_i64(
        pg->e2, odin3_ast_child(pg->e2->elab, node, REPEAT_COUNT), &times, &known);
    if (st != ODIN3_OK) {
        return st;
    }
    return loop_record(pg, node, (odin3_sym_id){0}, known && times > 0 ? times : 0);
}

odin3_status pg_loop_open(odin3_pg *pg, odin3_ast_id node) {
    return odin3_ast_kind_of(pg->e2->elab, node) == ODIN3_AST_FOR ? open_for(pg, node)
                                                                  : open_repeat(pg, node);
}

/* --- stepping --------------------------------------------------------------------------------- */

/* The loop ends: its frame and record go, its variable is no longer a constant to the AST test. */
static void loop_close(odin3_pg *pg) {
    const pg_loop *loop = odin3_vec_cat(&pg->loops, pg->loops.len - 1);
    if (loop->sym.v != 0 && pg->e2->bound != NULL) {
        (void)odin3_u64map_remove(pg->e2->bound, loop->sym.v);
    }
    odin3_vec_pop(&pg->loops);
    odin3_vec_pop(&pg->frames);
}

/* One more iteration is admitted: the loop's cap, then the run's total (fatal). */
static odin3_status admit(odin3_pg *pg, pg_loop *loop) {
    odin3_elab2 *e2 = pg->e2;
    odin3_loc loc = odin3_ast_loc(e2->elab, loop->node);
    if (loop->count >= e2->limits->max_loop_iterations) {
        odin3_elab2_error(e2, loc, "loop exceeds %u iterations", e2->limits->max_loop_iterations);
        return ODIN3_ERR_PARSE;
    }
    if (e2->iterations >= e2->limits->max_total_iterations) {
        odin3_elab2_fatal(e2, loc, "unrolled loops exceed %u iterations in this design",
                          e2->limits->max_total_iterations);
        return ODIN3_ERR_PARSE;
    }
    loop->count++;
    e2->iterations++;
    return ODIN3_OK;
}

static odin3_status push_body(odin3_pg *pg, odin3_ast_id body) {
    if (body.v == 0) {
        return ODIN3_OK;
    }
    pg_frame frame = {PG_F_STMT, body, 0, 0, 0, 0};
    return pg_frame_push(pg, &frame);
}

/* A for statement's assignment (init or step): its value must be constant, then it is built. */
static odin3_status for_assign(odin3_pg *pg, odin3_ast_id loop, uint32_t slot, const char *what) {
    odin3_ast_id assign = odin3_ast_child(pg->e2->elab, loop, slot);
    need nd = {loop, odin3_ast_child(pg->e2->elab, assign, 2), what};
    odin3_status st = need_constant(pg, &nd);
    return st != ODIN3_OK ? st : pg_exec_assign(pg, assign);
}

/* The test of a for loop: false ends it; a true condition admits an iteration and runs the body. */
static odin3_status for_test(odin3_pg *pg, pg_frame *frame, pg_loop *loop) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id cond = odin3_ast_child(ast, loop->node, FOR_COND);
    need nd = {loop->node, cond, "condition"};
    odin3_status st = need_constant(pg, &nd);
    bool truth = false;
    st = st != ODIN3_OK ? st : const_truth(pg, cond, &truth);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!truth) {
        loop_close(pg);
        return ODIN3_OK;
    }
    st = admit(pg, loop);
    frame->next = LOOP_STEP;
    return st != ODIN3_OK ? st : push_body(pg, odin3_ast_child(ast, loop->node, FOR_BODY));
}

static odin3_status for_step(odin3_pg *pg, pg_frame *frame, pg_loop *loop) {
    switch (frame->next) {
    case LOOP_INIT:
        frame->next = LOOP_TEST;
        return for_assign(pg, loop->node, FOR_INIT, "initialisation");
    case LOOP_TEST:
        return for_test(pg, frame, loop);
    default:
        frame->next = LOOP_TEST;
        return for_assign(pg, loop->node, FOR_STEP, "step");
    }
}

static odin3_status repeat_step(odin3_pg *pg, pg_loop *loop) {
    if (loop->remaining <= 0) {
        loop_close(pg);
        return ODIN3_OK;
    }
    odin3_status st = admit(pg, loop);
    loop->remaining--;
    return st != ODIN3_OK ? st
                          : push_body(pg, odin3_ast_child(pg->e2->elab, loop->node, REPEAT_BODY));
}

odin3_status pg_loop_step(odin3_pg *pg) {
    pg_frame *frame = odin3_vec_at(&pg->frames, pg->frames.len - 1);
    pg_loop *loop = odin3_vec_at(&pg->loops, frame->aux);
    if (odin3_ast_kind_of(pg->e2->elab, loop->node) == ODIN3_AST_FOR) {
        return for_step(pg, frame, loop);
    }
    return repeat_step(pg, loop);
}

/* --- is the variable an output of the block? ------------------------------------------------- */

/* The parts of a select or concatenation lvalue onto the worklist. */
static odin3_status lhs_push_parts(odin3_pg *pg, odin3_ast_id node, odin3_ast_kind kind) {
    uint32_t count = kind == ODIN3_AST_SELECT ? 1U : odin3_ast_nchild(pg->e2->elab, node);
    for (uint32_t part = 0; part < count; part++) {
        odin3_ast_id *slot = odin3_vec_push(&pg->work);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_ast_child(pg->e2->elab, node, part);
    }
    return ODIN3_OK;
}

/* The names an lvalue assigns (through selects and concatenations) onto set. */
static odin3_status lhs_bases(odin3_pg *pg, odin3_ast_id lhs, odin3_u64map *set) {
    odin3_vec_clear(&pg->work);
    odin3_ast_id *slot = odin3_vec_push(&pg->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = lhs;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && pg->work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&pg->work, pg->work.len - 1);
        odin3_vec_pop(&pg->work);
        odin3_ast_kind kind = odin3_ast_kind_of(pg->e2->elab, node);
        if (kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME) {
            st = odin3_u64map_put(set, (odin3_kv){node.v, 1});
        } else if (kind == ODIN3_AST_SELECT || kind == ODIN3_AST_CONCAT) {
            st = lhs_push_parts(pg, node, kind);
        }
    }
    return st;
}

/* One entered node of the module walk. */
typedef struct lv_walk {
    odin3_vec stack; /* uint32_t: the loop variables of the for statements being walked */
    odin3_u64map *bases;
} lv_walk;

static bool in_stack(const lv_walk *walk, uint32_t sym) {
    for (size_t i = 0; i < walk->stack.len; i++) {
        if (*(const uint32_t *)odin3_vec_cat(&walk->stack, i) == sym) {
            return true;
        }
    }
    return false;
}

static odin3_status walk_enter(odin3_pg *pg, lv_walk *walk, odin3_ast_id node) {
    const odin3_ast *ast = pg->e2->elab;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_FOR: {
        uint32_t *slot = odin3_vec_push(&walk->stack);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = pg_assign_symbol(pg, odin3_ast_child(ast, node, FOR_INIT)).v;
        return ODIN3_OK;
    }
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
    case ODIN3_AST_NET_ASSIGN:
        return lhs_bases(pg, odin3_ast_child(ast, node, 0), walk->bases);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME: {
        odin3_sym_id sym = odin3_elab_sym_of(&pg->e2->bind, node);
        if (sym.v == 0 || odin3_u64map_get(walk->bases, node.v, NULL) || in_stack(walk, sym.v)) {
            return ODIN3_OK;
        }
        return odin3_u64map_put(pg->loopout, (odin3_kv){sym.v, 1});
    }
    default:
        return ODIN3_OK;
    }
}

/* Every name of the module the loops of its variable do not read, once per module. */
static odin3_status build_loopout(odin3_pg *pg) {
    lv_walk walk = {{0}, NULL};
    odin3_vec_init(&walk.stack, sizeof(uint32_t));
    walk.bases = odin3_u64map_create(0);
    pg->loopout = pg->loopout != NULL ? pg->loopout : odin3_u64map_create(0);
    odin3_ast_cursor cur;
    odin3_status st = walk.bases != NULL && pg->loopout != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    st = st != ODIN3_OK ? st : odin3_ast_cursor_init(&cur, pg->e2->elab, pg->e2->mod_node);
    if (st == ODIN3_OK) {
        odin3_ast_event ev;
        odin3_ast_id node;
        while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
            if (ev == ODIN3_AST_ENTER) {
                st = walk_enter(pg, &walk, node);
            } else if (odin3_ast_kind_of(pg->e2->elab, node) == ODIN3_AST_FOR &&
                       walk.stack.len > 0) {
                odin3_vec_pop(&walk.stack);
            }
        }
        odin3_ast_cursor_free(&cur);
    }
    odin3_u64map_destroy(walk.bases);
    odin3_vec_free(&walk.stack);
    pg->loopout_ready = st == ODIN3_OK;
    return st;
}

odin3_status pg_loopvar_is_output(odin3_pg *pg, odin3_sym_id sym, bool *out) {
    const odin3_symbol *rec = odin3_symbol_get(pg->e2->st, sym);
    *out = false;
    if (rec == NULL) {
        return ODIN3_OK;
    }
    if ((rec->flags & ODIN3_SYM_F_PORT) != 0 && rec->dir == 1U + ODIN3_AST_PORT_DECL_OUTPUT) {
        *out = true;
        return ODIN3_OK;
    }
    odin3_status st = pg->loopout_ready ? ODIN3_OK : build_loopout(pg);
    *out = st == ODIN3_OK && odin3_u64map_get(pg->loopout, sym.v, NULL);
    return st;
}

/* odin3_pg_stmt for one loop. */
static odin3_status loop_stmt(odin3_pg *pg, odin3_ast_id stmt, odin3_ast_kind kind) {
    if (odin3_ast_kind_of(pg->e2->elab, stmt) != kind) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_for/repeat: not a %s statement",
                  odin3_ast_kind_name(kind));
        return ODIN3_ERR_INVALID_ARG;
    }
    return pg_run(pg, stmt);
}

odin3_status odin3_pg_for(odin3_pg *pg, odin3_ast_id stmt) {
    return loop_stmt(pg, stmt, ODIN3_AST_FOR);
}

odin3_status odin3_pg_repeat(odin3_pg *pg, odin3_ast_id stmt) {
    return loop_stmt(pg, stmt, ODIN3_AST_REPEAT);
}
