/*
 * pg_init.c — initial values (2D Task 24; EL-40, D2): every `initial` statement and variable
 * initialiser of a module runs through the interpreter (EL-17) over a store of the bits it has
 * written, in source order, after the instances and continuous assignments (so the drivers of a
 * net are known) and before the processes. A statement that is not constant (a read of an input or
 * of a net driven by logic) is ignored with a warning and leaves its targets as they were. The
 * results then go, per bit or word with a known value: onto the `init` attribute of a wire (or
 * the `INIT` of a `$mem`) when an `always` block assigns it; onto the shared constant net when
 * nothing else drives it; nowhere, with a warning, when a continuous assignment, an instance or an
 * input port does. `$readmemh/b` as a statement of an `initial` is pg_readmem.c's.
 */
#include "frontends/elab/pg_init.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { INIT_MODULE_PORTS = 1, INIT_MAX_DIMS = 8 };

/* --- the pool ----------------------------------------------------------------------------------
 */

static void run_free(odin3_init *run) {
    odin3_vec_free(&run->bits);
    odin3_vec_free(&run->vlist);
    odin3_vec_free(&run->moff);
    odin3_vec_free(&run->mloc);
    odin3_vec_free(&run->journal);
    odin3_vec_free(&run->saved);
    odin3_vec_free(&run->blocks);
    odin3_u64map_destroy(run->vars);
}

/* Bytes of the pool at off. */
static uint8_t *pool_at(odin3_init *run, uint64_t off) {
    return (uint8_t *)run->bits.data + off;
}

static const uint8_t *pool_cat(const odin3_init *run, uint64_t off) {
    return (const uint8_t *)run->bits.data + off;
}

/* n more bytes in the pool, all x, against the cap `initial values need more than N bits` (a
 * located error at the statement, ODIN3_ERR_PARSE). *off is where they begin. */
static odin3_status pool_alloc(odin3_init *run, uint64_t n, uint64_t *off) {
    odin3_elab2 *e2 = run->e2;
    if (run->total + n > e2->limits->max_init_bits) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, run->stmt),
                          "initial values need more than %" PRIu32 " bits",
                          e2->limits->max_init_bits);
        return ODIN3_ERR_PARSE;
    }
    odin3_status st = odin3_vec_reserve(&run->bits, run->bits.len + (size_t)n);
    if (st != ODIN3_OK) {
        return st;
    }
    *off = run->bits.len;
    for (uint64_t k = 0; k < n; k++) { /* room is reserved: the pushes cannot fail */
        *(uint8_t *)odin3_vec_push(&run->bits) = (uint8_t)ODIN3_BIT_X;
    }
    run->total += n;
    return ODIN3_OK;
}

odin3_status odin3_init_journal(odin3_init *run, const uint8_t *bytes, uint32_t count) {
    odin3_init_undo *undo = odin3_vec_push(&run->journal);
    if (undo == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *undo = (odin3_init_undo){(uint64_t)(bytes - (const uint8_t *)run->bits.data), run->saved.len,
                              count};
    odin3_status st = odin3_vec_reserve(&run->saved, run->saved.len + count);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        *(uint8_t *)odin3_vec_push(&run->saved) = bytes[k];
    }
    if (st != ODIN3_OK) {
        odin3_vec_pop(&run->journal);
    }
    return st;
}

/* The statement was not constant: its writes are taken back (last first). */
static void journal_undo(odin3_init *run) {
    while (run->journal.len > 0) {
        const odin3_init_undo *undo = odin3_vec_cat(&run->journal, run->journal.len - 1);
        memcpy(pool_at(run, undo->off), (const uint8_t *)run->saved.data + undo->saved, undo->n);
        odin3_vec_pop(&run->journal);
    }
    odin3_vec_clear(&run->saved);
}

static void journal_commit(odin3_init *run) {
    odin3_vec_clear(&run->journal);
    odin3_vec_clear(&run->saved);
}

/* --- variables and memory words -----------------------------------------------------------------
 */

static uint32_t sym_width(const odin3_symbol *rec) {
    return rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
}

/* The record of variable sym (written first time here: its bits allocated). */
static odin3_init_var *var_of(odin3_init *run, odin3_sym_id sym, odin3_status *st) {
    uint64_t at = 0;
    if (odin3_u64map_get(run->vars, sym.v, &at)) {
        return odin3_vec_at(&run->vlist, (size_t)at - 1U);
    }
    odin3_init_var var = {sym, sym_width(odin3_symbol_get(run->e2->st, sym)), 0,
                          odin3_ast_loc(run->e2->elab, run->stmt)};
    *st = pool_alloc(run, var.width, &var.off);
    odin3_init_var *slot = *st == ODIN3_OK ? odin3_vec_push(&run->vlist) : NULL;
    if (*st == ODIN3_OK && slot == NULL) {
        *st = ODIN3_ERR_NO_MEMORY;
    }
    if (slot == NULL) {
        return NULL;
    }
    *slot = var;
    *st = odin3_u64map_put(run->vars, (odin3_kv){sym.v, run->vlist.len});
    return *st == ODIN3_OK ? slot : NULL;
}

uint8_t *odin3_init_word(odin3_init *run, const odin3_init_wordref *ref, odin3_status *st) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(run->e2, ref->mem);
    uint32_t mem_index = ref->mem;
    *st = ODIN3_OK;
    while (run->moff.len <= mem_index) {
        uint64_t *slot = odin3_vec_push(&run->moff);
        odin3_loc *where = slot != NULL ? odin3_vec_push(&run->mloc) : NULL;
        if (where == NULL) {
            *st = ODIN3_ERR_NO_MEMORY;
            return NULL;
        }
        *slot = 0;
        *where = (odin3_loc){0};
    }
    uint64_t *off = odin3_vec_at(&run->moff, mem_index);
    if (*off == 0) {
        uint64_t base = 0;
        *st = pool_alloc(run, (uint64_t)mem->size * mem->width, &base);
        if (*st != ODIN3_OK) {
            return NULL;
        }
        *off = base + 1U;
        *(odin3_loc *)odin3_vec_at(&run->mloc, mem_index) = odin3_ast_loc(run->e2->elab, run->stmt);
    }
    return pool_at(run, *off - 1U) + (size_t)ref->pos * mem->width;
}

/* The bytes as a value of width bits (NULL bits: all x) in the scratch arena. */
static odin3_status bytes_to_cval(odin3_init *run, const uint8_t *bits, uint32_t width,
                                  odin3_cval *out) {
    uint8_t *copy = odin3_arena_alloc(run->e2->scratch, width);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < width; k++) {
        copy[k] = bits != NULL ? bits[k] : (uint8_t)ODIN3_BIT_X;
    }
    const odin3_ast_literal lit = {
        {.kind = ODIN3_VAL_BITS, .bits = copy, .len = width}, false, false};
    return odin3_cval_from_literal(run->e2->scratch, &lit, out);
}

/* The word a SELECT names: the table index and the position, -1 outside the array. */
typedef struct word_at {
    uint32_t mem;
    int64_t pos;
    bool known;
} word_at;

static word_at word_of(const odin3_init *run, odin3_ast_id ref) {
    odin3_elab2_wordref wr;
    word_at at = {0, -1, false};
    if (!odin3_elab2_mem_word_of(run->e2, ref, &wr)) {
        return at;
    }
    int64_t values[INIT_MAX_DIMS] = {0};
    at.mem = wr.mem;
    at.known = wr.n <= INIT_MAX_DIMS;
    for (uint32_t k = 0; at.known && k < wr.n; k++) {
        at.known = odin3_ceval_word_index(run->ev, k, &values[k]);
    }
    at.pos = at.known ? odin3_elab2_mem_pos(run->e2, wr.mem, values) : -1;
    return at;
}

static const odin3_symbol *ref_symbol(const odin3_init *run, odin3_ast_id ref) {
    return odin3_symbol_get(run->e2->st, odin3_elab_sym_of(&run->e2->bind, ref));
}

/* A variable the store can supply: a `reg` or `integer` that no continuous assignment drives. */
static bool readable(const odin3_symbol *rec) {
    return rec->kind == ODIN3_SYM_VAR && rec->n_unpacked == 0 &&
           (rec->flags & ODIN3_SYM_F_ASSIGNED_CONTINUOUS) == 0 &&
           rec->dir != 1U + (uint8_t)ODIN3_AST_PORT_DECL_INPUT;
}

/* The bytes of a word the initials have written, NULL for a word of a memory not touched yet. */
static const uint8_t *word_bytes(const odin3_init *run, const word_at *at) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(run->e2, at->mem);
    if (mem == NULL || !at->known || at->pos < 0 || at->mem >= run->moff.len) {
        return NULL;
    }
    uint64_t off = *(const uint64_t *)odin3_vec_cat(&run->moff, at->mem);
    return off == 0 ? NULL : pool_cat(run, off - 1U) + (size_t)at->pos * mem->width;
}

static odin3_status store_read(void *user, odin3_ast_id ref, odin3_cval *out, bool *known) {
    odin3_init *run = user;
    *known = true;
    if (odin3_ast_kind_of(run->e2->elab, ref) == ODIN3_AST_SELECT) {
        word_at at = word_of(run, ref);
        const odin3_elab_mem *mem = odin3_elab2_mem_get(run->e2, at.mem);
        *known = mem != NULL;
        return mem == NULL ? ODIN3_OK : bytes_to_cval(run, word_bytes(run, &at), mem->width, out);
    }
    const odin3_symbol *rec = ref_symbol(run, ref);
    uint64_t at = 0;
    if (rec == NULL || !readable(rec)) {
        *known = false;
        return ODIN3_OK;
    }
    bool written = odin3_u64map_get(run->vars, odin3_elab_sym_of(&run->e2->bind, ref).v, &at);
    const odin3_init_var *var = written ? odin3_vec_cat(&run->vlist, (size_t)at - 1U) : NULL;
    return bytes_to_cval(run, var != NULL ? pool_cat(run, var->off) : NULL, sym_width(rec), out);
}

/* The cval written into `count` bytes at `bytes` (journalled first). */
static odin3_status put_bits(odin3_init *run, uint8_t *bytes, uint32_t count,
                             const odin3_cval *value) {
    odin3_status st = odin3_init_journal(run, bytes, count);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        bytes[k] = (uint8_t)odin3_cval_bit(value, k);
    }
    return st;
}

static odin3_status store_write_word(odin3_init *run, odin3_ast_id lvalue,
                                     const odin3_cval *value) {
    word_at at = word_of(run, lvalue);
    const odin3_elab_mem *mem = odin3_elab2_mem_get(run->e2, at.mem);
    if (!at.known) {
        return ODIN3_OK;
    }
    if (at.pos < 0) {
        odin3_elab2_warn(run->e2, odin3_ast_loc(run->e2->elab, lvalue),
                         "assignment to a word outside array '%s' is ignored",
                         odin3_strtab_get(odin3_design_strtab(run->e2->design), mem->name));
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    uint8_t *word = odin3_init_word(run, &(odin3_init_wordref){at.mem, (uint32_t)at.pos}, &st);
    return st != ODIN3_OK ? st : put_bits(run, word, mem->width, value);
}

static odin3_status store_write(void *user, odin3_ast_id lvalue, const odin3_cval *value) {
    odin3_init *run = user;
    if (odin3_ast_kind_of(run->e2->elab, lvalue) == ODIN3_AST_SELECT) {
        return store_write_word(run, lvalue, value);
    }
    odin3_sym_id sym = odin3_elab_sym_of(&run->e2->bind, lvalue);
    const odin3_symbol *rec = odin3_symbol_get(run->e2->st, sym);
    if (rec == NULL || rec->ir_wire.v == 0 || rec->n_unpacked != 0) {
        return ODIN3_OK; /* nothing of the module's to hold */
    }
    odin3_status st = ODIN3_OK;
    const odin3_init_var *var = var_of(run, sym, &st);
    return var == NULL ? st : put_bits(run, pool_at(run, var->off), var->width, value);
}

/* --- statements --------------------------------------------------------------------------------
 */

/* Whether the statement holds a `$readmemh`/`$readmemb` call, and the first one. */
static odin3_ast_id find_readmem(const odin3_elab2 *e2, odin3_ast_id stmt) {
    odin3_ast_cursor cur;
    odin3_ast_id found = {0};
    if (odin3_ast_cursor_init(&cur, e2->elab, stmt) != ODIN3_OK) {
        return found;
    }
    odin3_ast_event ev;
    odin3_ast_id node;
    while (found.v == 0 && odin3_ast_cursor_next(&cur, &ev, &node)) {
        found = ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_TASK_CALL &&
                        (odin3_ast_flags(e2->elab, node) & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0
                    ? node
                    : found;
    }
    odin3_ast_cursor_free(&cur);
    return found;
}

/* Runs one statement through the interpreter: not constant, it is ignored with D2's warning. */
static odin3_status run_exec(odin3_init *run, odin3_ast_id stmt) {
    odin3_elab2 *e2 = run->e2;
    bool is_const = false;
    run->stmt = stmt;
    odin3_status st = odin3_ceval_exec(run->ev, stmt, &run->store, &is_const);
    if (st != ODIN3_OK) {
        journal_undo(run);
        return st;
    }
    if (!is_const) {
        journal_undo(run);
        if (odin3_ast_kind_of(e2->elab, stmt) == ODIN3_AST_TASK_CALL) {
            odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, stmt),
                             "task call in an initial block is not applied and is ignored");
            return ODIN3_OK;
        }
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, stmt),
                         "initial statement is not constant and is ignored");
        return ODIN3_OK;
    }
    journal_commit(run);
    return ODIN3_OK;
}

/* The `$readmemh` met somewhere a statement list cannot reach it (inside an `if` or a loop). */
static odin3_status readmem_nested(odin3_init *run, odin3_ast_id call) {
    odin3_elab2_error(run->e2, odin3_ast_loc(run->e2->elab, call),
                      "%s is supported only as a statement of an initial block",
                      odin3_ast_name_str(run->e2->elab, call));
    return ODIN3_ERR_PARSE;
}

/* A `begin ... end` being walked: its node and the next statement. */
typedef struct init_frame {
    odin3_ast_id node;
    uint32_t next;
} init_frame;

static odin3_status frame_open(odin3_init *run, odin3_ast_id block) {
    init_frame *frame = odin3_vec_push(&run->blocks);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (init_frame){block, odin3_ast_slot_count(ODIN3_AST_SEQ_BLOCK)};
    return ODIN3_OK;
}

/* One statement: a block without locals opens a frame (D2 is per statement; a `$readmem` among
 * them is a statement of its own), a `$readmem` is read, anything else is run as one unit. */
static odin3_status run_leaf(odin3_init *run, odin3_ast_id stmt) {
    const odin3_ast *ast = run->e2->elab;
    odin3_ast_kind kind = odin3_ast_kind_of(ast, stmt);
    if (kind == ODIN3_AST_NULL_STMT) {
        return ODIN3_OK;
    }
    run->stmt = stmt;
    if (kind == ODIN3_AST_TASK_CALL &&
        (odin3_ast_flags(ast, stmt) & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0) {
        return odin3_elab2_readmem(run->e2, stmt, &run->store);
    }
    if (kind == ODIN3_AST_SEQ_BLOCK && odin3_ast_child(ast, stmt, 0).v == 0) {
        return frame_open(run, stmt);
    }
    odin3_ast_id inner = find_readmem(run->e2, stmt);
    return inner.v != 0 ? readmem_nested(run, inner) : run_exec(run, stmt);
}

/* The statements of a top statement of an initial, nested blocks walked off a stack. */
static odin3_status run_stmt(odin3_init *run, odin3_ast_id top) {
    const odin3_ast *ast = run->e2->elab;
    size_t base = run->blocks.len;
    odin3_status st = run_leaf(run, top);
    while (st == ODIN3_OK && run->blocks.len > base && !run->e2->stopped) {
        init_frame *frame = odin3_vec_at(&run->blocks, run->blocks.len - 1);
        odin3_ast_span kids = odin3_ast_children(ast, frame->node);
        if (frame->next >= kids.n) {
            odin3_vec_pop(&run->blocks);
            continue;
        }
        odin3_ast_id kid = kids.ids[frame->next++];
        st = kid.v != 0 ? run_leaf(run, kid) : ODIN3_OK;
    }
    while (run->blocks.len > base) {
        odin3_vec_pop(&run->blocks);
    }
    return st;
}

/* A variable initialiser `reg r = v;`: the value at the variable's width, like `initial r = v;`. */
static odin3_status run_declarator(odin3_init *run, odin3_ast_id declarator) {
    odin3_elab2 *e2 = run->e2;
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, declarator);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    odin3_ast_id expr = odin3_ast_child(e2->elab, declarator, 0);
    if (rec == NULL || rec->kind != ODIN3_SYM_VAR || rec->ir_wire.v == 0 || expr.v == 0) {
        return ODIN3_OK; /* a net's is a continuous assignment's */
    }
    uint32_t width = sym_width(rec);
    const odin3_ceval_req req = {expr, {width, true, false}, "initial value"};
    odin3_cval value;
    odin3_cval fit;
    bool is_const = false;
    run->stmt = declarator;
    odin3_status st = odin3_ceval_try(run->ev, &req, e2->scratch, &value, &is_const);
    st = st != ODIN3_OK || !is_const
             ? st
             : odin3_cval_resize(e2->scratch, &value, width, (value.flags & ODIN3_CVAL_SIGNED) != 0,
                                 &fit);
    if (st != ODIN3_OK || !is_const) {
        if (st == ODIN3_OK) {
            odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, declarator),
                             "initial statement is not constant and is ignored");
        }
        return st;
    }
    st = store_write(run, declarator, &fit);
    journal_commit(run);
    return st;
}

/* --- the walk ----------------------------------------------------------------------------------
 */

/* The declarators with an initialiser of a declaration. */
static odin3_status run_declaration(odin3_init *run, odin3_ast_id decl) {
    odin3_ast_span kids = odin3_ast_children(run->e2->elab, decl);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < kids.n && !run->e2->stopped; k++) {
        if (kids.ids[k].v != 0 &&
            odin3_ast_kind_of(run->e2->elab, kids.ids[k]) == ODIN3_AST_DECLARATOR) {
            st = run_declarator(run, kids.ids[k]);
        }
    }
    return st;
}

/* One module item: an initial block, a declaration, or a generate block to descend. */
static odin3_status run_item(odin3_init *run, odin3_ast_id item) {
    const odin3_elab2 *e2 = run->e2;
    switch (odin3_ast_kind_of(e2->elab, item)) {
    case ODIN3_AST_INITIAL:
        if (odin3_elab_pending(ODIN3_ELAB_STAGE2, (uint32_t)ODIN3_AST_INITIAL)) {
            return ODIN3_OK; /* forced by a test: the walk of the assignments reported it */
        }
        return odin3_ast_child(e2->elab, item, 0).v != 0
                   ? run_stmt(run, odin3_ast_child(e2->elab, item, 0))
                   : ODIN3_OK;
    case ODIN3_AST_VAR_DECL:
        return run_declaration(run, item);
    default:
        return ODIN3_OK;
    }
}

/* The items of the module in source order, generate blocks descended (a worklist, no recursion). */
static odin3_status walk_items(odin3_init *run, odin3_ast_id module_node) {
    odin3_elab2 *e2 = run->e2;
    odin3_vec_clear(&e2->stack);
    odin3_ast_span ports =
        odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, INIT_MODULE_PORTS));
    odin3_status st = ODIN3_OK;
    e2->scope = (odin3_scope_id){0};
    for (uint32_t k = 0; st == ODIN3_OK && k < ports.n; k++) {
        st = run_declaration(run, ports.ids[k]);
    }
    st = st != ODIN3_OK
             ? st
             : odin3_elab2_push_items(e2, module_node, odin3_ast_slot_count(ODIN3_AST_MODULE),
                                      (odin3_scope_id){0});
    while (st == ODIN3_OK && e2->stack.len > 0 && !e2->stopped) {
        odin3_elab2_item item = *(odin3_elab2_item *)odin3_vec_at(&e2->stack, e2->stack.len - 1);
        odin3_vec_pop(&e2->stack);
        e2->scope = item.scope;
        if (odin3_ast_kind_of(e2->elab, item.node) == ODIN3_AST_GEN_BLOCK) {
            st =
                odin3_elab2_push_items(e2, item.node, 1, odin3_elab_scope_of(&e2->bind, item.node));
        } else {
            st = run_item(run, item.node);
        }
    }
    e2->scope = (odin3_scope_id){0};
    return st;
}

/* --- the results -------------------------------------------------------------------------------
 */

/* Where one variable's or word's bits go. */
typedef struct result {
    odin3_wire_id wire;
    const uint8_t *bits; /* the pool bytes of the whole wire */
    uint32_t width;
    odin3_sym_id sym; /* a variable; 0 for a word of registers */
    odin3_loc loc;
} result;

static const char *driver_name(uint8_t kind) {
    switch (kind) {
    case ODIN3_DRV_PORT_IN:
        return "an input port";
    case ODIN3_DRV_INSTANCE:
    case ODIN3_DRV_INOUT_PIN:
        return "an instance";
    case ODIN3_DRV_GATE:
        return "a gate";
    default:
        return "a continuous assignment";
    }
}

static bool bit_known(uint8_t bit) {
    return bit == (uint8_t)ODIN3_BIT_0 || bit == (uint8_t)ODIN3_BIT_1 ||
           bit == (uint8_t)ODIN3_BIT_Z;
}

/* The `init` attribute of the wire: the bits an always block assigns, x elsewhere. */
static odin3_status set_init_attr(odin3_elab2 *e2, odin3_wire_id wire, const uint8_t *init,
                                  uint32_t width) {
    uint32_t key = 0;
    odin3_status st = odin3_design_intern(e2->design, odin3_bytes_cstr("init"), &key);
    odin3_value value = {.kind = ODIN3_VAL_BITS, .bits = init, .len = width};
    return st != ODIN3_OK
               ? st
               : odin3_attr_set(e2->mod, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, key, &value);
}

/* One known bit that nothing else drives: its net becomes the shared constant net. */
static odin3_status bind_constant(odin3_elab2 *e2, const result *res, uint32_t bit) {
    odin3_net_id konst = {0};
    odin3_status st = odin3_elab2_const_net(e2, (odin3_bit)res->bits[bit], &konst);
    odin3_net_id net = odin3_elab2_resolve(e2, odin3_wire_net(e2->mod, res->wire, bit));
    if (st != ODIN3_OK || net.v == konst.v) {
        return st;
    }
    return odin3_elab2_merge(e2, (odin3_net_pair){konst, net});
}

static void warn_driven(odin3_elab2 *e2, const result *res, uint8_t kind) {
    const char *name =
        odin3_strtab_get(odin3_design_strtab(e2->design), odin3_wire_name(e2->mod, res->wire));
    odin3_elab2_warn(e2, res->loc, "initial value of '%s' is ignored: '%s' is driven by %s", name,
                     name, driver_name(kind));
}

/* Whether an always block assigns bit i of the object. */
static bool is_always(const odin3_elab2 *e2, const result *res, uint32_t bit) {
    return res->sym.v != 0 ? odin3_elab2_always_assigned(e2, res->sym, bit)
                           : odin3_elab2_always_assigned_word(e2, res->wire, bit);
}

/* What the bits of one wire came to: the init attribute's bits, whether any is set, and the first
 * kind of driver that made a bit's value be ignored. */
typedef struct apply_acc {
    uint8_t *init;
    uint8_t ignored;
    bool any;
} apply_acc;

/* One bit: a known value goes to the init attribute (an always block assigns it), is ignored (a
 * driver) or is the constant net. */
static odin3_status apply_bit(odin3_elab2 *e2, const result *res, uint32_t bit, apply_acc *acc) {
    acc->init[bit] = (uint8_t)ODIN3_BIT_X;
    if (!bit_known(res->bits[bit])) {
        return ODIN3_OK;
    }
    if (is_always(e2, res, bit)) {
        acc->init[bit] = res->bits[bit];
        acc->any = true;
        return ODIN3_OK;
    }
    odin3_drv drv = odin3_elab2_bit_driver(e2, (odin3_wirebit){res->wire, bit});
    if (drv.kind == ODIN3_DRV_NONE) {
        return bind_constant(e2, res, bit);
    }
    acc->ignored = acc->ignored == ODIN3_DRV_NONE ? drv.kind : acc->ignored;
    return ODIN3_OK;
}

/* One variable or word: the init attribute, the constant nets and the warning. */
static odin3_status apply_wire(odin3_elab2 *e2, const result *res) {
    apply_acc acc = {odin3_arena_alloc(e2->scratch, res->width), ODIN3_DRV_NONE, false};
    odin3_status st = acc.init != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < res->width; bit++) {
        st = apply_bit(e2, res, bit, &acc);
    }
    if (st == ODIN3_OK && acc.ignored != ODIN3_DRV_NONE) {
        warn_driven(e2, res, acc.ignored);
    }
    return st == ODIN3_OK && acc.any ? set_init_attr(e2, res->wire, acc.init, res->width) : st;
}

static odin3_status apply_var(odin3_init *run, const odin3_init_var *var) {
    const odin3_symbol *rec = odin3_symbol_get(run->e2->st, var->sym);
    result res = {rec->ir_wire, pool_cat(run, var->off), var->width, var->sym, var->loc};
    return apply_wire(run->e2, &res);
}

/* The pool offset of the memory at table index (it was touched). */
static uint64_t memory_base(const odin3_init *run, uint32_t index) {
    return *(const uint64_t *)odin3_vec_cat(&run->moff, index) - 1U;
}

/* The memory's pool as the INIT parameter of its `$mem` (x where unknown). */
static odin3_status apply_memory(odin3_init *run, uint32_t index) {
    odin3_elab2 *e2 = run->e2;
    odin3_elab_mem *mem = odin3_elab2_mem_at(e2, index);
    uint64_t total = (uint64_t)mem->size * mem->width;
    const uint8_t *bits = pool_cat(run, memory_base(run, index));
    bool any = false;
    for (uint64_t k = 0; k < total && !any; k++) {
        any = bit_known(bits[k]);
    }
    uint8_t *copy = any ? odin3_arena_alloc(e2->scratch, (size_t)total) : NULL;
    if (any && copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (copy != NULL) {
        memcpy(copy, bits, (size_t)total);
        mem->init = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = copy, .len = (uint32_t)total};
    }
    return ODIN3_OK;
}

/* The words of an array that became registers: each is a wire. */
static odin3_status apply_words(odin3_init *run, uint32_t index) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(run->e2, index);
    odin3_loc loc = *(const odin3_loc *)odin3_vec_cat(&run->mloc, index);
    odin3_status st = ODIN3_OK;
    for (uint32_t pos = 0; st == ODIN3_OK && pos < mem->words.len; pos++) {
        odin3_wire_id wire = *(const odin3_wire_id *)odin3_vec_cat(&mem->words, pos);
        result res = {wire, pool_cat(run, memory_base(run, index)) + (size_t)pos * mem->width,
                      mem->width, (odin3_sym_id){0}, loc};
        st = wire.v != 0 ? apply_wire(run->e2, &res) : ODIN3_OK;
    }
    return st;
}

/* Every variable and memory an initial wrote, in the order they were first written. */
static odin3_status apply_all(odin3_init *run) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < run->vlist.len; i++) {
        st = apply_var(run, odin3_vec_cat(&run->vlist, i));
    }
    for (uint32_t index = 0; st == ODIN3_OK && index < run->moff.len; index++) {
        if (*(const uint64_t *)odin3_vec_cat(&run->moff, index) == 0) {
            continue;
        }
        st = odin3_elab2_mem_get(run->e2, index)->to_regs ? apply_words(run, index)
                                                          : apply_memory(run, index);
    }
    return st;
}

/* --- the entry point ---------------------------------------------------------------------------
 */

static odin3_status run_open(odin3_init *run, odin3_elab2 *e2) {
    *run = (odin3_init){.e2 = e2};
    odin3_vec_init(&run->bits, sizeof(uint8_t));
    odin3_vec_init(&run->vlist, sizeof(odin3_init_var));
    odin3_vec_init(&run->moff, sizeof(uint64_t));
    odin3_vec_init(&run->mloc, sizeof(odin3_loc));
    odin3_vec_init(&run->journal, sizeof(odin3_init_undo));
    odin3_vec_init(&run->saved, sizeof(uint8_t));
    odin3_vec_init(&run->blocks, sizeof(init_frame));
    run->vars = odin3_u64map_create(0);
    run->store = (odin3_ceval_store){store_read, store_write, run};
    odin3_status st = run->vars != NULL ? odin3_elab2_ceval_get(e2, &run->ev) : ODIN3_ERR_NO_MEMORY;
    if (st != ODIN3_OK) {
        run_free(run);
    }
    return st;
}

odin3_status odin3_elab2_initials(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_initials: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_init run;
    odin3_status st = run_open(&run, e2);
    if (st != ODIN3_OK) {
        return st;
    }
    st = walk_items(&run, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : apply_all(&run);
    run_free(&run);
    return st;
}
