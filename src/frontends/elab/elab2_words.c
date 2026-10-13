/*
 * elab2_words.c — stage 2's second half of the pre-scan (2D Task 21; EL-28 step (2), EL-27): the
 * word wires of an array that became registers are made by the mem2reg decision, after the first
 * pre-scan of elab2_drive.c, so a second walk over the module's `always` blocks marks the wires of
 * the words they assign (odin3_elab2_always_assigned_word). A word is exact for NUMBER indices
 * (`m[2] <= x`, also under a bit or part select) and every word of the array otherwise (a variable
 * index, or an index written as an expression): a superset, never a miss.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every word wire of the array. */
static odin3_status words_all(odin3_elab2 *e2, uint32_t mem_index) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, mem_index);
    odin3_status st = ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < mem->words.len; k++) {
        odin3_wire_id wire = *(const odin3_wire_id *)odin3_vec_cat(&mem->words, k);
        st = wire.v != 0 ? odin3_elab2_word_assigned(e2, wire) : ODIN3_OK;
    }
    return st;
}

/* True when every index of the word node is a NUMBER. */
static bool words_numbers(const odin3_elab2 *e2, odin3_ast_id word,
                          const odin3_elab2_wordref *ref) {
    for (uint32_t k = 0; k < ref->n; k++) {
        odin3_ast_id idx = odin3_elab2_mem_word_index(e2, word, k);
        if (odin3_ast_kind_of(e2->elab, idx) != ODIN3_AST_NUMBER) {
            return false;
        }
    }
    return true;
}

/* The word wire the NUMBER indices select, marked; every word when they are not exact. */
static odin3_status words_mark_word(odin3_elab2 *e2, odin3_ast_id word,
                                    const odin3_elab2_wordref *ref) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref->mem);
    if (!mem->to_regs) {
        return ODIN3_OK; /* a memory write: the write port of Task 24 */
    }
    if (!words_numbers(e2, word, ref)) {
        return words_all(e2, ref->mem);
    }
    odin3_sig *idx = odin3_arena_alloc(e2->scratch, (size_t)ref->n * sizeof *idx);
    if (idx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < ref->n; k++) {
        odin3_elab2_expr_req req = {
            odin3_elab2_mem_word_index(e2, word, k), {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &req, &idx[k]);
    }
    odin3_elab2_word got = {ODIN3_WORD_X, {0}};
    odin3_elab2_mem_rd rd = {ref->mem, word, idx, ref->n};
    st = st != ODIN3_OK ? st : odin3_elab2_mem_word(e2, &rd, true, &got);
    if (st != ODIN3_OK) {
        return st == ODIN3_ERR_PARSE ? ODIN3_OK : st; /* reported; the generator reports it again */
    }
    if (got.kind == ODIN3_WORD_WIRE) {
        return odin3_elab2_word_assigned(e2, got.wire);
    }
    return got.kind == ODIN3_WORD_VARIABLE ? words_all(e2, ref->mem) : ODIN3_OK;
}

/* One lvalue node: a concatenation is its parts, a word select (or a select of one) its word. */
static odin3_status words_lvalue_node(odin3_elab2 *e2, odin3_vec *work, odin3_ast_id node) {
    if (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_CONCAT) {
        odin3_ast_span parts = odin3_ast_children(e2->elab, node);
        for (uint32_t i = 0; i < parts.n; i++) {
            odin3_ast_id *slot = odin3_vec_push(work);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = parts.ids[i];
        }
        return ODIN3_OK;
    }
    if (odin3_ast_kind_of(e2->elab, node) != ODIN3_AST_SELECT) {
        return ODIN3_OK;
    }
    odin3_elab2_wordref ref;
    if (odin3_elab2_mem_word_of(e2, node, &ref)) {
        return words_mark_word(e2, node, &ref);
    }
    odin3_ast_id object = odin3_ast_child(e2->elab, node, 0);
    if (odin3_ast_kind_of(e2->elab, object) == ODIN3_AST_SELECT &&
        odin3_elab2_mem_word_of(e2, object, &ref)) {
        return words_mark_word(e2, object, &ref);
    }
    return ODIN3_OK;
}

/* The lvalue of one assignment inside an always block. */
static odin3_status words_lvalue(odin3_elab2 *e2, odin3_ast_id lhs) {
    odin3_vec work;
    odin3_vec_init(&work, sizeof(odin3_ast_id));
    odin3_ast_id *slot = odin3_vec_push(&work);
    odin3_status st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (slot != NULL) {
        *slot = lhs;
    }
    while (st == ODIN3_OK && work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&work, work.len - 1);
        odin3_vec_pop(&work);
        st = words_lvalue_node(e2, &work, node);
    }
    odin3_vec_free(&work);
    return st;
}

/* Every blocking and nonblocking assignment below one ALWAYS. */
static odin3_status words_one_always(odin3_elab2 *e2, odin3_ast_id always) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, always);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev == ODIN3_AST_ENTER &&
            (kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN)) {
            st = words_lvalue(e2, odin3_ast_child(e2->elab, node, 0));
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

odin3_status odin3_elab2_prescan_words(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_prescan_words: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, module_node);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && !e2->stopped && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev != ODIN3_AST_ENTER || kind == ODIN3_AST_MODULE || kind == ODIN3_AST_GEN_BLOCK) {
            continue;
        }
        st = kind == ODIN3_AST_ALWAYS ? words_one_always(e2, node) : ODIN3_OK;
        odin3_ast_cursor_skip(&cur);
    }
    odin3_ast_cursor_free(&cur);
    return st != ODIN3_OK ? st : e2->fatal;
}
