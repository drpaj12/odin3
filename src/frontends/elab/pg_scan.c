/*
 * pg_scan.c — the scan of an `always` block's statements before anything is built (2D Task 21,
 * EL-35 step 1; Task 22, EL-36): every bit an assignment can write is an output; the nets each
 * `if` and `case` can assign by a blocking assignment in its branches are the targets of its case
 * temporaries (Yosys's collect_lvalues, once per statement instead of once per nesting level of
 * it); and an `if`/`case` whose condition or selector and items are constant by the AST folds to
 * one branch, whose dead branches then write nothing (their outputs are not outputs, as in Yosys,
 * which simplifies them away first). The scan walks with a worklist (no recursion); a switch
 * pushes an exit marker below its children, and the marker pops when the last child is done.
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
#include <stdlib.h>

enum { SCAN_SHIFT = 32 };
#define SCAN_EXIT (1U << 31) /* the high bit of a worklist entry: leave this switch */

/* The symbol a lhs piece names: through its selects to the identifier. */
static const odin3_symbol *piece_symbol(const odin3_elab2 *e2, odin3_ast_id piece) {
    while (odin3_ast_kind_of(e2->elab, piece) == ODIN3_AST_SELECT) {
        piece = odin3_ast_child(e2->elab, piece, 0);
    }
    return odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, piece));
}

/* `'w' is a net assigned in an always block; treated as a variable` (Yosys: "wire '\\w' is
 * assigned in a block"), once per piece of a target. */
static void warn_nets(odin3_pg *pg, const odin3_elab2_lvalue *lv) {
    odin3_elab2 *e2 = pg->e2;
    odin3_ast_id last = {0};
    for (size_t i = 0; i < lv->bits.len; i++) {
        odin3_ast_id piece = ((const odin3_elab2_lvbit *)odin3_vec_cat(&lv->bits, i))->node;
        if (piece.v == last.v) {
            continue;
        }
        last = piece;
        const odin3_symbol *rec = piece_symbol(e2, piece);
        if (rec != NULL && rec->kind == ODIN3_SYM_NET) {
            odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, piece),
                             "'%s' is a net assigned in an always block; treated as a variable",
                             odin3_strtab_get(odin3_design_strtab(e2->design), rec->name));
        }
    }
}

/* The nets of the assignment just taken apart join the targets of the open switches. */
static odin3_status scan_blocking(odin3_pg *pg, const odin3_elab2_lvalue *lv) {
    for (size_t i = 0; pg->blk_marks.len > 0 && i < lv->nets.len; i++) {
        uint32_t net = ((const odin3_net_id *)odin3_vec_cat(&lv->nets, i))->v;
        uint32_t *slot = net != 0 ? odin3_vec_push(&pg->blk) : NULL;
        if (net != 0 && slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (slot != NULL) {
            *slot = net;
        }
    }
    return ODIN3_OK;
}

/* Whether a lvalue names a variable of a function or task that has no frame now: its writes belong
 * to the callee's frame (made where the call is), not to the block's outputs. */
static bool scan_local(const odin3_elab2 *e2, odin3_ast_id lhs) {
    while (odin3_ast_kind_of(e2->elab, lhs) == ODIN3_AST_SELECT) {
        lhs = odin3_ast_child(e2->elab, lhs, 0);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, lhs);
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        return false;
    }
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, lhs));
    return rec != NULL && (rec->kind == ODIN3_SYM_VAR || rec->kind == ODIN3_SYM_NET) &&
           rec->ir_wire.v == 0 && rec->n_unpacked == 0;
}

/* Whether a lvalue reads a variable of a function or task that has no frame now (in an index). */
static bool names_local(const odin3_elab2 *e2, odin3_ast_id lhs) {
    odin3_ast_cursor cur;
    bool found = false;
    if (odin3_ast_cursor_init(&cur, e2->elab, lhs) != ODIN3_OK) {
        return false;
    }
    odin3_ast_event ev;
    odin3_ast_id node;
    while (!found && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev == ODIN3_AST_ENTER && (kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME)) {
            found = scan_local(e2, node);
        }
    }
    odin3_ast_cursor_free(&cur);
    return found;
}

/* The bits a target can write join the outputs (first writer kept), unless the scan is of a call's
 * body, whose variables are temporaries and whose other targets are the block's already. */
static odin3_status scan_target(odin3_pg *pg, odin3_ast_id lhs, bool blocking) {
    if (scan_local(pg->e2, lhs)) {
        return ODIN3_OK;
    }
    pg->scan_local = !pg->scan_frame && names_local(pg->e2, lhs);
    odin3_status st = pg_take_target_of(pg, lhs);
    pg->scan_local = false;
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_elab2_lvalue *lv = &pg->lvs;
    warn_nets(pg, lv);
    for (size_t i = 0; !pg->scan_frame && i < lv->nets.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&lv->nets, i);
        if (net.v == 0 || odin3_u64map_get(pg->out_of, net.v, NULL)) {
            continue;
        }
        const odin3_elab2_lvbit *bit = odin3_vec_cat(&lv->bits, i);
        pg_out *out = odin3_vec_push(&pg->outs);
        if (out == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *out = (pg_out){net, {0}, bit->wb, bit->node, bit->whole, 0};
        st = odin3_u64map_put(pg->out_of, (odin3_kv){net.v, pg->outs.len});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return blocking ? scan_blocking(pg, lv) : ODIN3_OK;
}

static odin3_status scan_assign(odin3_pg *pg, odin3_ast_id assign) {
    bool blocking = odin3_ast_kind_of(pg->e2->elab, assign) == ODIN3_AST_BLOCKING_ASSIGN;
    return scan_target(pg, odin3_ast_child(pg->e2->elab, assign, 0), blocking);
}

static odin3_status scan_push_id(odin3_pg *pg, uint32_t entry) {
    odin3_ast_id *slot = odin3_vec_push(&pg->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->v = entry;
    return ODIN3_OK;
}

/* Pushes the children of node from index `from` on, last to first, so the first is scanned
 * first. */
static odin3_status scan_push(odin3_pg *pg, odin3_ast_id node, uint32_t from) {
    odin3_ast_span kids = odin3_ast_children(pg->e2->elab, node);
    for (uint32_t i = kids.n; i > from; i--) {
        odin3_status st = kids.ids[i - 1U].v != 0 ? scan_push_id(pg, kids.ids[i - 1U].v) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* An if/case that folded to one branch: only that branch is scanned. */
static odin3_status scan_folded(odin3_pg *pg, odin3_ast_id node, uint32_t pick) {
    odin3_ast_id body = pg_fold_body(pg, node, pick);
    return body.v != 0 ? scan_push_id(pg, body.v) : ODIN3_OK;
}

/* The switch opens: its targets begin here; the exit marker is scanned after the last branch. */
static odin3_status scan_switch(odin3_pg *pg, odin3_ast_id node) {
    uint32_t *mark = odin3_vec_push(&pg->blk_marks);
    if (mark == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *mark = (uint32_t)pg->blk.len;
    odin3_status st = scan_push_id(pg, node.v | SCAN_EXIT);
    if (st != ODIN3_OK) {
        return st;
    }
    return scan_push(pg, node,
                     1); /* the branches (an if) or the items (a case), not the condition */
}

int pg_u32_compare(const void *lhs, const void *rhs) {
    uint32_t first = *(const uint32_t *)lhs;
    uint32_t second = *(const uint32_t *)rhs;
    return (first > second) - (first < second);
}

/* The sorted unique run of blk from start on: appended to the pool, and left in blk (the parent
 * switch's share of it). *first is where it sits in the pool, *count its length. */
static odin3_status scan_unique(odin3_pg *pg, size_t start, uint32_t *first, uint32_t *count) {
    size_t total = pg->blk.len - start;
    uint32_t kept = 0;
    if (total > 0) {
        uint32_t *run = (uint32_t *)pg->blk.data + start;
        qsort(run, total, sizeof *run, pg_u32_compare);
        for (size_t i = 0; i < total; i++) {
            if (kept == 0 || run[i] != run[kept - 1U]) {
                run[kept++] = run[i];
            }
        }
    }
    while (pg->blk.len > start + kept) {
        odin3_vec_pop(&pg->blk);
    }
    *first = (uint32_t)pg->sw_pool.len;
    *count = kept;
    odin3_status st = odin3_vec_reserve(&pg->sw_pool, pg->sw_pool.len + kept);
    for (uint32_t i = 0; st == ODIN3_OK && i < kept; i++) {
        uint32_t *slot = odin3_vec_push(&pg->sw_pool);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = *(const uint32_t *)odin3_vec_cat(&pg->blk, start + i);
        }
    }
    return st;
}

/* The exit marker of a switch: its targets are the unique blocking nets of its branches. */
static odin3_status scan_exit(odin3_pg *pg, odin3_ast_id node) {
    size_t start = *(const uint32_t *)odin3_vec_cat(&pg->blk_marks, pg->blk_marks.len - 1);
    odin3_vec_pop(&pg->blk_marks);
    uint32_t first = 0;
    uint32_t count = 0;
    odin3_status st = scan_unique(pg, start, &first, &count);
    return st != ODIN3_OK
               ? st
               : odin3_u64map_put(pg->swl,
                                  (odin3_kv){node.v, ((uint64_t)first << SCAN_SHIFT) | count});
}

/* An if or case: folded, or a switch whose items are scanned. */
static odin3_status scan_branching(odin3_pg *pg, odin3_ast_id node) {
    uint32_t pick = 0;
    odin3_status st = pg_fold(pg, node, &pick);
    if (st != ODIN3_OK) {
        return st;
    }
    return pick != 0 ? scan_folded(pg, node, pick) : scan_switch(pg, node);
}

/* A for statement: its init and step assign a variable that is an output only when the module reads
 * it outside its loops (EL-35, divergence 5); the condition is not a statement. */
static odin3_status scan_for(odin3_pg *pg, odin3_ast_id node) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id init = odin3_ast_child(ast, node, 0);
    odin3_ast_id step = odin3_ast_child(ast, node, 2);
    odin3_sym_id sym = pg_assign_symbol(pg, init);
    bool output = true;
    odin3_status st = ODIN3_OK;
    if (sym.v != 0) {
        const odin3_symbol *rec = odin3_symbol_get(pg->e2->st, sym);
        output = false;
        if (rec != NULL && rec->ir_wire.v != 0 && !pg->scan_frame) {
            st = pg_loopvar_is_output(pg, sym, &output);
        }
    }
    st = st != ODIN3_OK || !output ? st : scan_assign(pg, init);
    st = st != ODIN3_OK || !output ? st : scan_assign(pg, step);
    odin3_ast_id body = odin3_ast_child(ast, node, 3);
    return st != ODIN3_OK || body.v == 0 ? st : scan_push_id(pg, body.v);
}

/* Whether node is a name, select or concatenation: something an output argument can be. */
static bool is_variable_kind(const odin3_ast *ast, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME || kind == ODIN3_AST_SELECT ||
           kind == ODIN3_AST_CONCAT;
}

/* The actual of an output or inout formal is a target (blocking, K9); anything else is the error.
 */
static odin3_status scan_output_actual(odin3_pg *pg, odin3_ast_id call, uint32_t at) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id actual = odin3_ast_child(ast, call, at);
    if (actual.v == 0) {
        return ODIN3_OK;
    }
    if (!is_variable_kind(ast, actual)) {
        odin3_elab2_error(pg->e2, odin3_ast_loc(ast, actual),
                          "output argument %u of task '%s' must be a variable", at + 1U,
                          odin3_ast_name_str(ast, call));
        return ODIN3_ERR_PARSE;
    }
    return scan_target(pg, actual, true);
}

/* A task enable: the actuals of its output and inout formals are targets; its body is scanned once
 * for the targets that are the block's own. */
static odin3_status scan_task_call(odin3_pg *pg, odin3_ast_id node) {
    odin3_elab2 *e2 = pg->e2;
    const odin3_ast *ast = e2->elab;
    if ((odin3_ast_flags(ast, node) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        return ODIN3_OK;
    }
    odin3_ast_id decl = {0};
    odin3_status st = odin3_elab2_callee(e2, node, &decl);
    odin3_ast_id ports = st == ODIN3_OK ? odin3_ast_child(ast, decl, 0) : (odin3_ast_id){0};
    uint32_t at = 0;
    for (uint32_t k = 0; st == ODIN3_OK && ports.v != 0 && k < odin3_ast_nchild(ast, ports); k++) {
        odin3_ast_id port = odin3_ast_child(ast, ports, k);
        bool out = odin3_ast_sub(ast, port) != ODIN3_AST_PORT_DECL_INPUT;
        for (uint32_t j = 1; st == ODIN3_OK && j < odin3_ast_nchild(ast, port); j++, at++) {
            st = out ? scan_output_actual(pg, node, at) : ODIN3_OK;
        }
    }
    if (st != ODIN3_OK || odin3_u64map_get(pg->tvis, decl.v, NULL)) {
        return st;
    }
    st = odin3_u64map_put(pg->tvis, (odin3_kv){decl.v, 1});
    odin3_ast_id body = st == ODIN3_OK ? odin3_ast_child(ast, decl, 2) : (odin3_ast_id){0};
    return st != ODIN3_OK || body.v == 0 ? st : scan_push_id(pg, body.v);
}

/* One node of the scan: an assignment adds its outputs, a block its statements, an if or case its
 * branches, a pending construct is reported; any other kind is looked into (the statements of
 * Task 23). */
static odin3_status scan_node(odin3_pg *pg, odin3_ast_id node) {
    if ((node.v & SCAN_EXIT) != 0) {
        return scan_exit(pg, (odin3_ast_id){node.v & ~(uint32_t)SCAN_EXIT});
    }
    switch (odin3_ast_kind_of(pg->e2->elab, node)) {
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        return scan_assign(pg, node);
    case ODIN3_AST_SEQ_BLOCK:
        return scan_push(pg, node, odin3_ast_slot_count(ODIN3_AST_SEQ_BLOCK));
    case ODIN3_AST_IF:
    case ODIN3_AST_CASE:
        return scan_branching(pg, node);
    case ODIN3_AST_FOR:
        return scan_for(pg, node);
    case ODIN3_AST_TASK_CALL:
        return scan_task_call(pg, node);
    case ODIN3_AST_CASE_ITEM: /* its body; the match expressions are not statements */
        return scan_push_id(pg, odin3_ast_child(pg->e2->elab, node, 0).v);
    case ODIN3_AST_NULL_STMT:
        return ODIN3_OK;
    default:
        if (odin3_elab2_pending(pg->e2, node)) {
            return ODIN3_ERR_PARSE;
        }
        return scan_push(pg, node, 0);
    }
}

odin3_status pg_scan_call(odin3_pg *pg, odin3_ast_id body) {
    pg->scan_frame = true;
    odin3_status st = pg_scan(pg, body);
    pg->scan_frame = false;
    return st;
}

odin3_status pg_scan(odin3_pg *pg, odin3_ast_id body) {
    odin3_u64map_destroy(pg->tvis);
    pg->tvis = odin3_u64map_create(0);
    if (pg->tvis == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_clear(&pg->work);
    odin3_status st = body.v != 0 ? scan_push_id(pg, body.v) : ODIN3_OK;
    while (st == ODIN3_OK && pg->work.len > 0 && !pg->e2->stopped) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&pg->work, pg->work.len - 1);
        odin3_vec_pop(&pg->work);
        st = node.v != 0 ? scan_node(pg, node) : ODIN3_OK;
    }
    return st;
}
