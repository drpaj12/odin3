/*
 * elab2_memscan.c — pass 1 of Yosys 0.55's mem2reg_as_needed (simplify.cc 5240-5367) over one
 * module of the elaborated store (2D Task 20; EL-27): for every array the flags that decide
 * whether it stays a memory (written in a block without exactly one edge event, read after a
 * blocking write of the same block, inside a complex lvalue, written only at constant indices)
 * and the places Yosys names in its warning. The traversal order, the first-time guards on the
 * places and the proc-local blocking-write flag are Yosys's, so the places come out the same.
 *
 * Differences, deliberate: a function's or task's body is not scanned (its statements run where it
 * is called, Task 23); a concatenation lvalue is split into its parts first, as Yosys does; a
 * constant index is a NUMBER, a localparam, an operator over them or the variable of a `for` that
 * Yosys unrolls before this point (Yosys has folded and unrolled by then).
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A for statement being scanned: its node, and its loop variable when Yosys would unroll it (the
 * variable starts and steps by constants and the condition is constant on it): the variable's
 * indices are constants in the body. */
typedef struct ms_loop {
    uint32_t node, sym;
} ms_loop;

typedef struct ms_run {
    odin3_elab2 *e2;
    odin3_vec *facts;
    odin3_vec work;      /* odin3_ast_id: the nodes of a region still to visit */
    odin3_vec parts;     /* odin3_ast_id: the parts of a concatenation lvalue still to judge */
    odin3_vec constwork; /* odin3_ast_id: the nodes of a constant test still to visit */
    odin3_vec chain;     /* odin3_ast_id: the selects of the lvalue being looked at */
    odin3_vec loops;     /* ms_loop: the for statements being scanned, innermost last */
    uint32_t serial;     /* the block being scanned (0: outside any) */
    uint32_t last_serial;
    bool async;
    bool init;
    odin3_ast_id lhs; /* the lvalue the walk is to skip */
} ms_run;

typedef enum ms_mode { MS_MARK, MS_READ } ms_mode;

static odin3_mem_fact *ms_fact(const ms_run *run, uint32_t mem) {
    return odin3_vec_at(run->facts, mem);
}

/* The table index of the array an IDENT or HIER_NAME names; MEM_NONE for anything else. */
static uint32_t ms_array_of(const ms_run *run, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(run->e2->elab, node);
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        return MEM_NONE;
    }
    odin3_sym_id sym = odin3_elab_sym_of(&run->e2->bind, node);
    return sym.v == 0 ? MEM_NONE : odin3_elab2_mem_find(run->e2, sym);
}

/* Remembers node's place for the warning (Yosys's mem2reg_places insert). */
static odin3_status ms_place(const ms_run *run, odin3_mem_fact *fact, odin3_ast_id node) {
    odin3_loc *slot = odin3_vec_push(&fact->places);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = odin3_ast_loc(run->e2->elab, node);
    return ODIN3_OK;
}

/* Sets flag on the fact, remembering node's place the first time (Yosys's `if (!(flags & F))`). */
static odin3_status ms_note(const ms_run *run, odin3_mem_fact *fact, uint32_t flag,
                            odin3_ast_id node) {
    odin3_status st = (fact->flags & flag) == 0 ? ms_place(run, fact, node) : ODIN3_OK;
    fact->flags |= flag;
    return st;
}

/* A read of an array: after a blocking write of this block it is rule 3. */
static odin3_status ms_read(const ms_run *run, odin3_ast_id ident) {
    uint32_t mem = ms_array_of(run, ident);
    if (mem == MEM_NONE || run->serial == 0) {
        return ODIN3_OK;
    }
    odin3_mem_fact *fact = ms_fact(run, mem);
    if (fact->eq1_proc != run->serial || (fact->flags & MEM_FL_EQ2) != 0) {
        return ODIN3_OK;
    }
    return ms_note(run, fact, MEM_FL_EQ2, ident);
}

static odin3_status ms_push(odin3_vec *vec, odin3_ast_id node) {
    odin3_ast_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

/* The children of node onto vec, the last first, so the first pops first. */
static odin3_status ms_push_kids(const ms_run *run, odin3_vec *vec, odin3_ast_id node) {
    odin3_ast_span kids = odin3_ast_children(run->e2->elab, node);
    for (uint32_t i = kids.n; i > 0; i--) {
        if (kids.ids[i - 1].v != 0) {
            odin3_status st = ms_push(vec, kids.ids[i - 1]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_ast_id ms_pop(odin3_vec *vec) {
    odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(vec, vec->len - 1);
    odin3_vec_pop(vec);
    return node;
}

/* Every array identifier below root (root included), in source order: marked as inside a complex
 * lvalue, or read. */
static odin3_status ms_idents(ms_run *run, odin3_ast_id root, ms_mode mode) {
    odin3_vec_clear(&run->work);
    odin3_status st = ms_push(&run->work, root);
    while (st == ODIN3_OK && run->work.len > 0) {
        odin3_ast_id node = ms_pop(&run->work);
        odin3_ast_kind kind = odin3_ast_kind_of(run->e2->elab, node);
        if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
            st = ms_push_kids(run, &run->work, node);
            continue;
        }
        uint32_t mem = ms_array_of(run, node);
        if (mode == MS_READ) {
            st = ms_read(run, node);
        } else if (mem != MEM_NONE) {
            st = ms_note(run, ms_fact(run, mem), MEM_FL_CMPLX_LHS, node);
        }
    }
    return st;
}

/* The selects of lhs from the outermost down to the name, in run->chain; the name is returned. */
static odin3_ast_id ms_chain(ms_run *run, odin3_ast_id lhs, odin3_status *st) {
    odin3_vec_clear(&run->chain);
    odin3_ast_id node = lhs;
    *st = ODIN3_OK;
    while (*st == ODIN3_OK && odin3_ast_kind_of(run->e2->elab, node) == ODIN3_AST_SELECT) {
        *st = ms_push(&run->chain, node);
        node = odin3_ast_child(run->e2->elab, node, 0);
    }
    return node;
}

/* The expressions of an lvalue part that are not its name: the index expressions of its selects. */
static odin3_status ms_lhs_walk(ms_run *run, odin3_ast_id lhs, ms_mode mode) {
    odin3_status st;
    (void)ms_chain(run, lhs, &st);
    /* run->chain is walked outside in; the index expressions are visited from the name out */
    for (size_t i = run->chain.len; st == ODIN3_OK && i > 0; i--) {
        odin3_ast_id sel = *(const odin3_ast_id *)odin3_vec_cat(&run->chain, i - 1);
        for (uint32_t k = 1; st == ODIN3_OK && k < odin3_ast_nchild(run->e2->elab, sel); k++) {
            st = ms_idents(run, odin3_ast_child(run->e2->elab, sel, k), mode);
        }
    }
    return st;
}

/* --- constant indices ------------------------------------------------------------------------ */

/* Whether sym is the variable of a for statement being scanned that Yosys would unroll. */
static bool ms_loop_var(const ms_run *run, uint32_t sym) {
    for (size_t i = 0; sym != 0 && i < run->loops.len; i++) {
        if (((const ms_loop *)odin3_vec_cat(&run->loops, i))->sym == sym) {
            return true;
        }
    }
    return false;
}

/* Whether a name is constant: a parameter, or the variable of a loop Yosys unrolls. */
static bool ms_const_name(const ms_run *run, odin3_ast_id node) {
    odin3_sym_id sym = odin3_elab_sym_of(&run->e2->bind, node);
    const odin3_symbol *rec = odin3_symbol_get(run->e2->st, sym);
    return rec != NULL && (rec->kind == ODIN3_SYM_PARAM || rec->kind == ODIN3_SYM_LOCALPARAM ||
                           ms_loop_var(run, sym.v));
}

/* Whether a leaf or an operator is constant: a NUMBER, a localparam, $signed/$unsigned/$clog2 and
 * the operators over constants (kids pushed to the worklist); false for anything else. */
static bool ms_const_node(ms_run *run, odin3_ast_id node, odin3_status *st) {
    const odin3_ast *ast = run->e2->elab;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_NUMBER:
        return true;
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return ms_const_name(run, node);
    case ODIN3_AST_UNARY:
    case ODIN3_AST_BINARY:
    case ODIN3_AST_TERNARY:
    case ODIN3_AST_CONCAT:
    case ODIN3_AST_REPLICATE:
        *st = ms_push_kids(run, &run->constwork, node);
        return true;
    case ODIN3_AST_CALL: {
        const char *name = odin3_ast_name_str(ast, node);
        bool known = strcmp(name, "$signed") == 0 || strcmp(name, "$unsigned") == 0 ||
                     strcmp(name, "$clog2") == 0;
        *st = known ? ms_push_kids(run, &run->constwork, node) : ODIN3_OK;
        return known;
    }
    default:
        return false;
    }
}

static bool ms_is_const(ms_run *run, odin3_ast_id expr, odin3_status *st) {
    odin3_vec_clear(&run->constwork);
    *st = ms_push(&run->constwork, expr);
    bool all = true;
    while (*st == ODIN3_OK && all && run->constwork.len > 0) {
        all = ms_const_node(run, ms_pop(&run->constwork), st);
    }
    return all && *st == ODIN3_OK;
}

/* Whether the array indices of the lvalue (its n innermost selects, run->chain) are all constant:
 * 0 for no index at all, 1 constant, 2 not constant. */
static int ms_index_kind(ms_run *run, uint32_t n_dims, odin3_status *st) {
    size_t have = run->chain.len < n_dims ? run->chain.len : n_dims;
    *st = ODIN3_OK;
    for (size_t k = 0; k < have; k++) {
        odin3_ast_id sel =
            *(const odin3_ast_id *)odin3_vec_cat(&run->chain, run->chain.len - 1 - k);
        bool bit = odin3_ast_sub(run->e2->elab, sel) == ODIN3_AST_SELECT_BIT;
        if (!bit || !ms_is_const(run, odin3_ast_child(run->e2->elab, sel, 1), st)) {
            return *st == ODIN3_OK ? 2 : 0;
        }
    }
    return have == 0 ? 0 : 1;
}

/* --- assignments ------------------------------------------------------------------------------ */

/* What one assignment is: the node, and whether it is a blocking one in a block. */
typedef struct ms_asg {
    odin3_ast_id node;
    uint32_t mem;
    bool blocking, procedural;
} ms_asg;

/* The flag of an index kind: 1 constant, 2 not constant. */
static uint32_t ms_index_flag(int kind) {
    switch (kind) {
    case 1:
        return MEM_FL_CONST_LHS;
    case 2:
        return MEM_FL_VAR_LHS;
    default:
        return 0;
    }
}

/* The block-level flags of a write: asynchronous, and the first blocking write of the block. */
static odin3_status ms_write_block(const ms_run *run, odin3_mem_fact *fact, const ms_asg *as) {
    odin3_status st = ODIN3_OK;
    if (as->procedural && run->async) {
        fact->async_node = (fact->flags & MEM_FL_SET_ASYNC) != 0 ? fact->async_node : as->node;
        st = ms_note(run, fact, MEM_FL_SET_ASYNC, as->node);
    }
    if (st == ODIN3_OK && as->blocking && fact->eq1_proc != run->serial) {
        st = ms_place(run, fact, as->node); /* the first blocking write of the block */
        fact->eq1_proc = run->serial;
    }
    return st;
}

/* The flags of a write to an array (Yosys's AST_ASSIGN* branch). */
static odin3_status ms_write(ms_run *run, const ms_asg *as) {
    odin3_mem_fact *fact = ms_fact(run, as->mem);
    odin3_status st = ms_write_block(run, fact, as);
    if (st == ODIN3_OK && !run->init) {
        int kind = ms_index_kind(run, odin3_elab2_mem_get(run->e2, as->mem)->n_dims, &st);
        fact->flags |= ms_index_flag(kind);
    }
    st = st != ODIN3_OK
             ? st
             : ms_note(run, fact, run->init ? MEM_FL_SET_INIT : MEM_FL_SET_ELSE, as->node);
    if (st == ODIN3_OK && !as->procedural) {
        fact->cont_node = (fact->flags & MEM_FL_CONT) != 0 ? fact->cont_node : as->node;
        fact->flags |= MEM_FL_CONT;
    }
    return st;
}

/* One part of an lvalue (not a concatenation): the complex-lvalue marks, the write flags of an
 * array target, then the reads of the target's index expressions. */
static odin3_status ms_assign_part(ms_run *run, odin3_ast_id node, odin3_ast_id lhs, bool blocking,
                                   bool procedural) {
    odin3_status st = ms_lhs_walk(run, lhs, MS_MARK);
    odin3_ast_id name = ms_chain(run, lhs, &st);
    uint32_t mem = st == ODIN3_OK ? ms_array_of(run, name) : MEM_NONE;
    if (mem != MEM_NONE) {
        ms_asg as = {node, mem, blocking, procedural};
        st = ms_write(run, &as);
    }
    return st != ODIN3_OK ? st : ms_lhs_walk(run, lhs, MS_READ);
}

/* One assignment. A concatenation lvalue is split into its parts first, as Yosys does, and each
 * part is judged on its own (an array in a concatenation is no complex lvalue). */
static odin3_status ms_assign(ms_run *run, odin3_ast_id node, bool blocking, bool procedural) {
    const odin3_ast *ast = run->e2->elab;
    odin3_ast_id lhs = odin3_ast_child(ast, node, 0);
    odin3_vec_clear(&run->parts);
    odin3_status st = ms_push(&run->parts, lhs);
    while (st == ODIN3_OK && run->parts.len > 0) {
        odin3_ast_id part = ms_pop(&run->parts);
        if (odin3_ast_kind_of(ast, part) != ODIN3_AST_CONCAT) {
            st = ms_assign_part(run, node, part, blocking, procedural);
            continue;
        }
        /* pushed last to first, so the parts are judged in source order */
        for (uint32_t k = odin3_ast_nchild(ast, part); st == ODIN3_OK && k > 0; k--) {
            st = ms_push(&run->parts, odin3_ast_child(ast, part, k - 1));
        }
    }
    run->lhs = lhs;
    return st;
}

/* --- blocks and the walk ---------------------------------------------------------------------- */

/* The edge events of an always block's own event list (Yosys counts only those). */
static uint32_t ms_edges(const odin3_ast *ast, odin3_ast_id always) {
    odin3_ast_id stmt = odin3_ast_child(ast, always, 0);
    if (odin3_ast_kind_of(ast, stmt) != ODIN3_AST_TIMING_STMT) {
        return 0;
    }
    odin3_ast_id control = odin3_ast_child(ast, stmt, 0);
    if (odin3_ast_kind_of(ast, control) != ODIN3_AST_EVENT_CONTROL) {
        return 0;
    }
    odin3_ast_span events = odin3_ast_children(ast, control);
    uint32_t edges = 0;
    for (uint32_t i = 0; i < events.n; i++) {
        if (events.ids[i].v != 0 && odin3_ast_kind_of(ast, events.ids[i]) == ODIN3_AST_EVENT_EXPR &&
            odin3_ast_sub(ast, events.ids[i]) != ODIN3_AST_EVENT_EXPR_NONE) {
            edges++;
        }
    }
    return edges;
}

static void ms_begin(ms_run *run, odin3_ast_id block, bool init) {
    run->serial = ++run->last_serial;
    run->init = init;
    run->async = !init && ms_edges(run->e2->elab, block) != 1;
}

/* The variable a for statement's init assigns: the symbol of its left side (0: none). */
static uint32_t ms_loop_symbol(const ms_run *run, odin3_ast_id assign) {
    odin3_ast_id lhs = odin3_ast_child(run->e2->elab, assign, 0);
    odin3_ast_kind kind = odin3_ast_kind_of(run->e2->elab, lhs);
    if (odin3_ast_kind_of(run->e2->elab, assign) != ODIN3_AST_BLOCKING_ASSIGN ||
        (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME)) {
        return 0;
    }
    odin3_sym_id sym = odin3_elab_sym_of(&run->e2->bind, lhs);
    const odin3_symbol *rec = odin3_symbol_get(run->e2->st, sym);
    return rec != NULL && rec->kind == ODIN3_SYM_VAR ? sym.v : 0;
}

/* Starts scanning a for statement: Yosys unrolls it before mem2reg when the variable starts at a
 * constant, steps by a constant and the condition is constant on it. */
static odin3_status ms_loop_open(ms_run *run, odin3_ast_id node) {
    const odin3_ast *ast = run->e2->elab;
    odin3_ast_id init = odin3_ast_child(ast, node, 0);
    odin3_ast_id step = odin3_ast_child(ast, node, 2);
    uint32_t sym = ms_loop_symbol(run, init);
    odin3_status st = ODIN3_OK;
    bool unrolls = sym != 0 && ms_is_const(run, odin3_ast_child(ast, init, 2), &st);
    ms_loop *slot = st == ODIN3_OK ? odin3_vec_push(&run->loops) : NULL;
    if (slot == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = (ms_loop){node.v, unrolls ? sym : 0};
    if (unrolls) {
        unrolls = ms_is_const(run, odin3_ast_child(ast, node, 1), &st) && st == ODIN3_OK &&
                  ms_loop_symbol(run, step) == sym &&
                  ms_is_const(run, odin3_ast_child(ast, step, 2), &st);
        slot->sym = unrolls && st == ODIN3_OK ? sym : 0;
    }
    return st;
}

/* One ENTER of the walk. */
static odin3_status ms_enter(ms_run *run, odin3_ast_cursor *cur, odin3_ast_id node) {
    if (run->lhs.v == node.v) {
        run->lhs = (odin3_ast_id){0};
        odin3_ast_cursor_skip(cur);
        return ODIN3_OK;
    }
    switch (odin3_ast_kind_of(run->e2->elab, node)) {
    case ODIN3_AST_ALWAYS:
        ms_begin(run, node, false);
        return ODIN3_OK;
    case ODIN3_AST_INITIAL:
        ms_begin(run, node, true);
        return ODIN3_OK;
    case ODIN3_AST_FUNCTION_DECL:
    case ODIN3_AST_TASK_DECL:
        odin3_ast_cursor_skip(cur);
        return ODIN3_OK;
    case ODIN3_AST_FOR:
        return run->serial == 0 ? ODIN3_OK : ms_loop_open(run, node);
    case ODIN3_AST_NET_ASSIGN: {
        odin3_ast_cursor_skip(cur); /* outside a block nothing is read after a blocking write */
        odin3_status st = ms_assign(run, node, false, false);
        run->lhs = (odin3_ast_id){0};
        return st;
    }
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        return run->serial == 0
                   ? ODIN3_OK
                   : ms_assign(run, node,
                               odin3_ast_kind_of(run->e2->elab, node) == ODIN3_AST_BLOCKING_ASSIGN,
                               true);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return ms_read(run, node);
    default:
        return ODIN3_OK;
    }
}

odin3_status odin3_elab2_mem_scan(odin3_elab2 *e2, odin3_ast_id module_node, odin3_vec *facts) {
    ms_run run = {.e2 = e2, .facts = facts};
    odin3_vec_init(&run.work, sizeof(odin3_ast_id));
    odin3_vec_init(&run.parts, sizeof(odin3_ast_id));
    odin3_vec_init(&run.constwork, sizeof(odin3_ast_id));
    odin3_vec_init(&run.chain, sizeof(odin3_ast_id));
    odin3_vec_init(&run.loops, sizeof(ms_loop));
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, module_node);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev == ODIN3_AST_ENTER) {
            st = ms_enter(&run, &cur, node);
        } else if (kind == ODIN3_AST_FOR && run.loops.len > 0 &&
                   ((const ms_loop *)odin3_vec_cat(&run.loops, run.loops.len - 1))->node ==
                       node.v) {
            odin3_vec_pop(&run.loops);
        } else if (kind == ODIN3_AST_ALWAYS || kind == ODIN3_AST_INITIAL) {
            run.serial = 0;
            run.init = false;
            run.async = false;
        }
    }
    odin3_ast_cursor_free(&cur);
    odin3_vec_free(&run.work);
    odin3_vec_free(&run.constwork);
    odin3_vec_free(&run.parts);
    odin3_vec_free(&run.chain);
    odin3_vec_free(&run.loops);
    return st;
}
