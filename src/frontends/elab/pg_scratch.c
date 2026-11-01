/*
 * pg_scratch.c — block scratch variables (front-end triage 2026-10-31, docs/notes). Two `always`
 * blocks that blocking-assign one variable are T10 (EL-41), an error, because the net would have
 * two drivers. Yosys accepts the pattern when the blocks only use the variable as scratch space:
 * each block gets a flip-flop for it that nothing reads (a read after a blocking assignment takes
 * the block's own value), and `opt_clean` removes them. Machine-converted designs write `TEMPORARY
 * = a / 4; Address = TEMPORARY[29:0];` in every block this way (ITC-99 b15 to b19).
 *
 * A symbol is a scratch variable when
 *   - it is a plain variable (not a port, not an array),
 *   - at least two `always` blocks assign it,
 *   - every assignment to it anywhere is a whole-variable blocking assignment inside an `always`,
 *   - every mention of it is inside an `always`, and a read follows, in an enclosing begin/end of
 *     the same block, a whole-variable blocking assignment of it (so no value reaches a read from
 *     outside the block, another block or an earlier cycle).
 * Each block that writes one (the first included) then gets a private IR wire for it, swapped in
 * for the symbol's ir_wire while the block is built, so the blocks drive different nets and there
 * is no conflict. The module-level wire of the symbol stays undriven and unread (it is kept, not
 * removed; `opt` drops it as dead).
 * The private flip-flops have no reader and `opt` removes them, as in Yosys. Every other case keeps
 * the T10 error. The module walk runs once, and only when the module has a process.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/pg_internal.h"
#include "odin3/odin3.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SC_BAD = 1U, SC_MULTI = 2U, SC_SHIFT = 8U };

/* The walk of one module. state: symbol -> (first writing always << SC_SHIFT) | flags. */
typedef struct sc_walk {
    odin3_pg *pg;
    odin3_vec stack; /* odin3_ast_id: the open nodes */
    odin3_vec defs;  /* uint32_t: symbols whose whole value a statement of an open begin/end set */
    odin3_vec marks; /* size_t: defs.len when each open begin/end was entered */
    odin3_u64map *state;
    odin3_ast_id lhs; /* the target of the assignment being walked, 0 outside one */
    uint32_t always;  /* the open always block, 0 outside one */
} sc_walk;

static bool is_assign(odin3_ast_kind kind) {
    return kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN ||
           kind == ODIN3_AST_NET_ASSIGN || kind == ODIN3_AST_CONT_ASSIGN;
}

static bool is_name(odin3_ast_kind kind) {
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME;
}

/* The symbol a name node is, when it can be a scratch variable; 0 otherwise. */
static uint32_t sc_symbol(const sc_walk *walk, odin3_ast_id node) {
    const odin3_elab2 *e2 = walk->pg->e2;
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, node);
    const odin3_symbol *rec = sym.v != 0 ? odin3_symbol_get(e2->st, sym) : NULL;
    bool plain = rec != NULL && rec->kind == ODIN3_SYM_VAR && rec->ir_wire.v != 0 &&
                 rec->n_unpacked == 0 && (rec->flags & ODIN3_SYM_F_PORT) == 0;
    return plain ? sym.v : 0U;
}

static odin3_status sc_flag(sc_walk *walk, uint32_t sym, uint64_t flags) {
    uint64_t was = 0;
    (void)odin3_u64map_get(walk->state, sym, &was);
    return odin3_u64map_put(walk->state, (odin3_kv){sym, was | flags});
}

/* Whether this block already has (always, sym) in the writer list. */
static bool sc_listed(const sc_walk *walk, uint32_t sym) {
    const odin3_vec *list = &walk->pg->scratch_w;
    for (size_t i = list->len; i > 0; i--) {
        const pg_scratch_w *rec = odin3_vec_cat(list, i - 1U);
        if (rec->always != walk->always) {
            return false;
        }
        if (rec->sym == sym) {
            return true;
        }
    }
    return false;
}

/* A whole-variable blocking assignment of sym: a writer, in a second block a multi-writer. */
static odin3_status sc_write(sc_walk *walk, uint32_t sym) {
    if (walk->always == 0) {
        return sc_flag(walk, sym, SC_BAD);
    }
    uint64_t was = 0;
    (void)odin3_u64map_get(walk->state, sym, &was);
    uint64_t first = was >> SC_SHIFT;
    odin3_status st = ODIN3_OK;
    if (first == 0) { /* the first writer is listed too: every block gets a private wire */
        was |= (uint64_t)walk->always << SC_SHIFT;
        st = odin3_u64map_put(walk->state, (odin3_kv){sym, was});
    } else if (first != walk->always) {
        st = sc_flag(walk, sym, SC_MULTI);
    }
    if (st != ODIN3_OK || sc_listed(walk, sym)) {
        return st;
    }
    pg_scratch_w *rec = odin3_vec_push(&walk->pg->scratch_w);
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (pg_scratch_w){walk->always, sym};
    return ODIN3_OK;
}

static bool sc_defined(const sc_walk *walk, uint32_t sym) {
    for (size_t i = 0; i < walk->defs.len; i++) {
        if (*(const uint32_t *)odin3_vec_cat(&walk->defs, i) == sym) {
            return true;
        }
    }
    return false;
}

/* A mention of a candidate variable. */
static odin3_status sc_mention(sc_walk *walk, odin3_ast_id node, uint32_t sym) {
    const odin3_ast *ast = walk->pg->e2->elab;
    odin3_ast_id parent =
        walk->stack.len > 0
            ? *(const odin3_ast_id *)odin3_vec_cat(&walk->stack, walk->stack.len - 1)
            : (odin3_ast_id){0};
    if (walk->lhs.v != 0) { /* a target: only the bare name of a blocking assignment is allowed */
        bool bare =
            walk->lhs.v == node.v && odin3_ast_kind_of(ast, parent) == ODIN3_AST_BLOCKING_ASSIGN;
        return bare ? sc_write(walk, sym) : sc_flag(walk, sym, SC_BAD);
    }
    if (walk->always == 0 || !sc_defined(walk, sym)) {
        return sc_flag(walk, sym, SC_BAD);
    }
    return ODIN3_OK;
}

/* Notes when node is the target of the assignment that is its parent. */
static void sc_note_target(sc_walk *walk, odin3_ast_id node) {
    const odin3_ast *ast = walk->pg->e2->elab;
    if (walk->stack.len == 0 || walk->lhs.v != 0) {
        return;
    }
    odin3_ast_id parent = *(const odin3_ast_id *)odin3_vec_cat(&walk->stack, walk->stack.len - 1);
    if (is_assign(odin3_ast_kind_of(ast, parent)) && odin3_ast_child(ast, parent, 0).v == node.v) {
        walk->lhs = node;
    }
}

static odin3_status sc_enter(sc_walk *walk, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(walk->pg->e2->elab, node);
    odin3_status st = ODIN3_OK;
    sc_note_target(walk, node);
    if (kind == ODIN3_AST_ALWAYS) {
        walk->always = node.v;
    } else if (kind == ODIN3_AST_SEQ_BLOCK) {
        size_t *mark = odin3_vec_push(&walk->marks);
        st = mark != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (mark != NULL) {
            *mark = walk->defs.len;
        }
    } else if (is_name(kind)) {
        uint32_t sym = sc_symbol(walk, node);
        st = sym != 0 ? sc_mention(walk, node, sym) : ODIN3_OK;
    }
    odin3_ast_id *slot = st == ODIN3_OK ? odin3_vec_push(&walk->stack) : NULL;
    if (slot != NULL) {
        *slot = node;
    }
    return st == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : st;
}

/* The end of a whole-variable blocking assignment directly in a begin/end: the value is set from
 * here to the end of that begin/end. */
static odin3_status sc_define(sc_walk *walk, odin3_ast_id assign) {
    const odin3_ast *ast = walk->pg->e2->elab;
    odin3_ast_id lhs = odin3_ast_child(ast, assign, 0);
    uint32_t sym = is_name(odin3_ast_kind_of(ast, lhs)) ? sc_symbol(walk, lhs) : 0U;
    odin3_ast_id parent =
        walk->stack.len > 0
            ? *(const odin3_ast_id *)odin3_vec_cat(&walk->stack, walk->stack.len - 1)
            : (odin3_ast_id){0};
    if (sym == 0 || walk->always == 0 || odin3_ast_kind_of(ast, parent) != ODIN3_AST_SEQ_BLOCK) {
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_push(&walk->defs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = sym;
    return ODIN3_OK;
}

static odin3_status sc_leave(sc_walk *walk, odin3_ast_id node) {
    const odin3_ast *ast = walk->pg->e2->elab;
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    odin3_vec_pop(&walk->stack);
    if (walk->lhs.v == node.v) {
        walk->lhs.v = 0;
    }
    if (kind == ODIN3_AST_ALWAYS) {
        walk->always = 0;
    } else if (kind == ODIN3_AST_SEQ_BLOCK && walk->marks.len > 0) {
        size_t mark = *(const size_t *)odin3_vec_cat(&walk->marks, walk->marks.len - 1);
        odin3_vec_pop(&walk->marks);
        while (walk->defs.len > mark) {
            odin3_vec_pop(&walk->defs);
        }
    } else if (kind == ODIN3_AST_BLOCKING_ASSIGN) {
        return sc_define(walk, node);
    }
    return ODIN3_OK;
}

/* The writers of variables that stayed good in two or more blocks are the scratch set. */
static odin3_status sc_finish(sc_walk *walk) {
    odin3_vec *list = &walk->pg->scratch_w;
    size_t kept = 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < list->len; i++) {
        pg_scratch_w rec = *(const pg_scratch_w *)odin3_vec_cat(list, i);
        uint64_t flags = 0;
        (void)odin3_u64map_get(walk->state, rec.sym, &flags);
        if ((flags & SC_BAD) != 0 || (flags & SC_MULTI) == 0) {
            continue;
        }
        *(pg_scratch_w *)odin3_vec_at(list, kept++) = rec;
        st = odin3_u64map_put(walk->pg->scratch, (odin3_kv){rec.sym, 1});
    }
    while (list->len > kept) {
        odin3_vec_pop(list);
    }
    return st;
}

/* The module walk, once. */
static odin3_status sc_build(odin3_pg *pg) {
    sc_walk walk = {pg, {0}, {0}, {0}, NULL, {0}, 0};
    odin3_vec_init(&walk.stack, sizeof(odin3_ast_id));
    odin3_vec_init(&walk.defs, sizeof(uint32_t));
    odin3_vec_init(&walk.marks, sizeof(size_t));
    walk.state = odin3_u64map_create(0);
    pg->scratch = pg->scratch != NULL ? pg->scratch : odin3_u64map_create(0);
    odin3_ast_cursor cur;
    odin3_status st = walk.state != NULL && pg->scratch != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    st = st != ODIN3_OK ? st : odin3_ast_cursor_init(&cur, pg->e2->elab, pg->e2->mod_node);
    if (st == ODIN3_OK) {
        odin3_ast_event ev;
        odin3_ast_id node;
        while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
            st = ev == ODIN3_AST_ENTER ? sc_enter(&walk, node) : sc_leave(&walk, node);
        }
        odin3_ast_cursor_free(&cur);
    }
    st = st != ODIN3_OK ? st : sc_finish(&walk);
    odin3_u64map_destroy(walk.state);
    odin3_vec_free(&walk.stack);
    odin3_vec_free(&walk.defs);
    odin3_vec_free(&walk.marks);
    pg->scratch_ready = st == ODIN3_OK;
    return st;
}

odin3_status pg_scratch_enter(odin3_pg *pg, odin3_ast_id always_node) {
    odin3_status st = pg->scratch_ready ? ODIN3_OK : sc_build(pg);
    odin3_vec_clear(&pg->scratch_old);
    for (size_t i = 0; st == ODIN3_OK && i < pg->scratch_w.len; i++) {
        const pg_scratch_w *rec = odin3_vec_cat(&pg->scratch_w, i);
        if (rec->always != always_node.v) {
            continue;
        }
        odin3_sym_id sym = {rec->sym};
        pg_scratch_old *old = odin3_vec_push(&pg->scratch_old);
        if (old == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *old = (pg_scratch_old){rec->sym, odin3_symbol_get(pg->e2->st, sym)->ir_wire};
        uint32_t width = 0;
        st = pg_unnamed_wire(pg, sym, &width);
    }
    return st;
}

void pg_scratch_leave(odin3_pg *pg) {
    for (size_t i = 0; i < pg->scratch_old.len; i++) {
        const pg_scratch_old *old = odin3_vec_cat(&pg->scratch_old, i);
        (void)odin3_symbol_set_ir(pg->e2->st, (odin3_sym_id){old->sym}, pg->e2->mod_id, old->wire);
    }
    odin3_vec_clear(&pg->scratch_old);
}
