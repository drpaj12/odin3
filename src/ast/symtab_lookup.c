/* symtab_lookup.c — symbol table lookup, iteration, accessors, IR remap and check rule 8. */
#include "ast/ast.h"
#include "ast/symtab.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- accessors ----------------------------------------------------------------------------- */

const odin3_scope *odin3_scope_get(const odin3_symtab *st, odin3_scope_id scope) {
    if (st == NULL || scope.v == 0 || scope.v >= odin3_pagevec_len(st->scopes)) {
        return NULL;
    }
    return odin3_pagevec_cat(st->scopes, scope.v);
}

const odin3_symbol *odin3_symbol_get(const odin3_symtab *st, odin3_sym_id sym) {
    if (st == NULL || sym.v == 0 || sym.v >= odin3_pagevec_len(st->symbols)) {
        return NULL;
    }
    return odin3_pagevec_cat(st->symbols, sym.v);
}

const odin3_sym_assign *odin3_assign_get(const odin3_symtab *st, odin3_assign_id assign) {
    if (st == NULL || assign.v == 0 || assign.v > st->assigns.len) {
        return NULL;
    }
    return odin3_vec_cat(&st->assigns, assign.v - 1);
}

odin3_dim_span odin3_symbol_unpacked(const odin3_symtab *st, odin3_sym_id sym) {
    const odin3_symbol *rec = odin3_symbol_get(st, sym);
    if (rec == NULL || rec->n_unpacked == 0) {
        return (odin3_dim_span){NULL, 0};
    }
    return (odin3_dim_span){odin3_vec_cat(&st->dims, rec->unpacked_first), rec->n_unpacked};
}

uint32_t odin3_symtab_scope_count(const odin3_symtab *st) {
    return st == NULL ? 0 : (uint32_t)(odin3_pagevec_len(st->scopes) - 1);
}

uint32_t odin3_symtab_symbol_count(const odin3_symtab *st) {
    return st == NULL ? 0 : (uint32_t)(odin3_pagevec_len(st->symbols) - 1);
}

/* --- lookup -------------------------------------------------------------------------------- */

odin3_sym_id odin3_symtab_lookup_local(const odin3_symtab *st, odin3_scope_id scope,
                                       uint32_t name) {
    uint64_t found = 0;
    if (st == NULL || scope.v == 0 ||
        !odin3_u64map_get(st->by_name, odin3_symtab_key(scope.v, name), &found)) {
        return (odin3_sym_id){0};
    }
    return (odin3_sym_id){(uint32_t)found};
}

odin3_sym_id odin3_symtab_lookup(const odin3_symtab *st, odin3_scope_id scope, uint32_t name) {
    const odin3_scope *rec = odin3_scope_get(st, scope);
    while (rec != NULL) {
        odin3_sym_id found = odin3_symtab_lookup_local(st, scope, name);
        if (found.v != 0) {
            return found;
        }
        scope = rec->parent;
        rec = odin3_scope_get(st, scope);
    }
    return (odin3_sym_id){0};
}

odin3_chain_slot odin3_symtab_chain_find(const odin3_symtab *st, odin3_scope_key key) {
    odin3_chain_slot slot = {{0}, {0}, false};
    uint64_t head = 0;
    if (key.name == 0 ||
        !odin3_u64map_get(st->children, odin3_symtab_key(key.parent.v, key.name), &head)) {
        return slot;
    }
    slot.cur = (odin3_scope_id){(uint32_t)head};
    const odin3_scope *rec = odin3_scope_get(st, slot.cur);
    while (rec != NULL && odin3_scope_order(rec) < key.order) {
        slot.prev = slot.cur;
        slot.cur = rec->same_name_next;
        rec = odin3_scope_get(st, slot.cur);
    }
    slot.found = rec != NULL && odin3_scope_order(rec) == key.order;
    return slot;
}

odin3_scope_id odin3_scope_child(const odin3_symtab *st, odin3_scope_id parent, uint32_t name,
                                 int64_t index) {
    bool in_range = index == ODIN3_SCOPE_NO_INDEX || (index >= INT32_MIN && index <= INT32_MAX);
    if (st == NULL || !in_range) {
        return (odin3_scope_id){0};
    }
    odin3_chain_slot slot = odin3_symtab_chain_find(st, (odin3_scope_key){parent, name, index});
    if (!slot.found) {
        return (odin3_scope_id){0};
    }
    return slot.cur;
}

/* --- iteration ----------------------------------------------------------------------------- */

odin3_scope_id odin3_scope_first_child(const odin3_symtab *st, odin3_scope_id scope) {
    const odin3_scope *rec = odin3_scope_get(st, scope);
    if (rec == NULL) {
        return (odin3_scope_id){0};
    }
    return rec->first_child;
}

odin3_scope_id odin3_scope_next_sibling(const odin3_symtab *st, odin3_scope_id scope) {
    const odin3_scope *rec = odin3_scope_get(st, scope);
    if (rec == NULL) {
        return (odin3_scope_id){0};
    }
    return rec->next_sibling;
}

odin3_sym_id odin3_scope_first_symbol(const odin3_symtab *st, odin3_scope_id scope) {
    const odin3_scope *rec = odin3_scope_get(st, scope);
    if (rec == NULL) {
        return (odin3_sym_id){0};
    }
    return rec->first_symbol;
}

odin3_sym_id odin3_scope_next_symbol(const odin3_symtab *st, odin3_sym_id sym) {
    const odin3_symbol *rec = odin3_symbol_get(st, sym);
    if (rec == NULL) {
        return (odin3_sym_id){0};
    }
    return rec->next_in_scope;
}

odin3_assign_id odin3_symbol_first_assign(const odin3_symtab *st, odin3_sym_id sym) {
    const odin3_symbol *rec = odin3_symbol_get(st, sym);
    if (rec == NULL) {
        return (odin3_assign_id){0};
    }
    return rec->first_assign;
}

odin3_assign_id odin3_symbol_next_assign(const odin3_symtab *st, odin3_assign_id assign) {
    const odin3_sym_assign *rec = odin3_assign_get(st, assign);
    if (rec == NULL) {
        return (odin3_assign_id){0};
    }
    return rec->next;
}

/* --- remap --------------------------------------------------------------------------------- */

void odin3_symtab_remap(odin3_symtab *st, odin3_module_id module, const odin3_compact_map *map) {
    if (st == NULL || map == NULL || module.v == 0) {
        return;
    }
    size_t end = odin3_pagevec_len(st->symbols);
    for (size_t i = 1; i < end; i++) {
        odin3_symbol *rec = odin3_pagevec_at(st->symbols, i);
        if (rec->ir_module.v != module.v || rec->ir_wire.v == 0) {
            continue;
        }
        uint32_t old = rec->ir_wire.v;
        rec->ir_wire.v = map->wire != NULL && old < map->n_wire ? map->wire[old] : 0;
    }
}

/* --- check rule 8 -------------------------------------------------------------------------- */

typedef struct check_ctx {
    const odin3_symtab *st;
    uint32_t node_end;  /* the checked AST's end */
    uint32_t n_scopes;  /* scope ID end */
    uint32_t n_symbols; /* symbol ID end */
    uint32_t errors;
} check_ctx;

static void bad(check_ctx *cx, const char *what, uint32_t id) {
    odin3_log(ODIN3_LOG_ERROR, "symtab check: %s (%" PRIu32 ")", what, id);
    cx->errors++;
}

static bool node_in_range(const check_ctx *cx, odin3_ast_id node) {
    return node.v < cx->node_end;
}

/* The record's own fields: kind, parent, node, index. */
static void check_scope_rec(check_ctx *cx, uint32_t id, const odin3_scope *rec) {
    if (rec->kind < ODIN3_SCOPE_DESIGN || rec->kind >= ODIN3_SCOPE_KIND_END) {
        bad(cx, "scope kind out of range", id);
    }
    bool root = rec->kind == ODIN3_SCOPE_DESIGN;
    if (root ? rec->parent.v != 0 : rec->parent.v == 0 || rec->parent.v >= id) {
        bad(cx, "scope parent missing or not created before it", id);
    }
    if (!node_in_range(cx, rec->node)) {
        bad(cx, "scope node out of the AST", id);
    }
    if (!rec->has_index && rec->index != 0) {
        bad(cx, "scope index without has_index", id);
    }
}

/* List membership totals over all scopes. */
typedef struct list_counts {
    uint32_t children, syms;
} list_counts;

/* Walks scope id's child and symbol lists (IDs strictly increase, so a walk ends); counts. */
static void check_scope_lists(check_ctx *cx, uint32_t id, list_counts *counts) {
    const odin3_scope *rec = odin3_scope_get(cx->st, (odin3_scope_id){id});
    uint32_t prev = id;
    for (odin3_scope_id at = rec->first_child; at.v != 0; at = rec->next_sibling) {
        rec = odin3_scope_get(cx->st, at);
        if (rec == NULL || at.v <= prev || rec->parent.v != id) {
            bad(cx, "scope child list broken", id);
            break;
        }
        prev = at.v;
        counts->children++;
    }
    const odin3_symbol *sym = NULL;
    prev = 0;
    rec = odin3_scope_get(cx->st, (odin3_scope_id){id});
    for (odin3_sym_id at = rec->first_symbol; at.v != 0; at = sym->next_in_scope) {
        sym = odin3_symbol_get(cx->st, at);
        if (sym == NULL || at.v <= prev || sym->scope.v != id) {
            bad(cx, "scope symbol list broken", id);
            break;
        }
        prev = at.v;
        counts->syms++;
    }
}

static void check_scopes(check_ctx *cx) {
    list_counts counts = {0, 0};
    uint32_t roots = 0;
    for (uint32_t id = 1; id < cx->n_scopes; id++) {
        const odin3_scope *rec = odin3_scope_get(cx->st, (odin3_scope_id){id});
        check_scope_rec(cx, id, rec);
        roots += rec->kind == ODIN3_SCOPE_DESIGN;
        check_scope_lists(cx, id, &counts);
    }
    if (roots > 1 || counts.children + roots != cx->n_scopes - 1) {
        bad(cx, "scopes missing from their parents' child lists", counts.children);
    }
    if (counts.syms != cx->n_symbols - 1) {
        bad(cx, "symbols missing from their scopes' lists", counts.syms);
    }
}

/* The assignment list: nodes in the AST, known kinds, IDs increasing, tail = last. */
static void check_assigns(check_ctx *cx, uint32_t id, const odin3_symbol *rec) {
    uint32_t prev = 0;
    const odin3_sym_assign *as = NULL;
    for (odin3_assign_id at = rec->first_assign; at.v != 0; at = as->next) {
        as = odin3_assign_get(cx->st, at);
        if (as == NULL || at.v <= prev || as->node.v == 0 || !node_in_range(cx, as->node) ||
            as->kind < ODIN3_ASSIGN_BLOCKING || as->kind >= ODIN3_ASSIGN_KIND_END) {
            bad(cx, "symbol assignment list broken", id);
            return;
        }
        prev = at.v;
    }
    if (prev != rec->last_assign.v) {
        bad(cx, "symbol assignment tail wrong", id);
    }
}

static void check_symbol(check_ctx *cx, uint32_t id, const odin3_symbol *rec) {
    if (rec->name == 0 || odin3_scope_get(cx->st, rec->scope) == NULL) {
        bad(cx, "symbol without a name or an existing scope", id);
    }
    if (rec->kind < ODIN3_SYM_NET || rec->kind >= ODIN3_SYM_KIND_END) {
        bad(cx, "symbol kind out of range", id);
    }
    if (!node_in_range(cx, rec->decl) || !node_in_range(cx, rec->declarator)) {
        bad(cx, "symbol decl or declarator out of the AST", id);
    }
    if ((uint64_t)rec->unpacked_first + rec->n_unpacked > cx->st->dims.len) {
        bad(cx, "symbol unpacked dims out of the table", id);
    }
    if (odin3_symtab_lookup_local(cx->st, rec->scope, rec->name).v != id) {
        bad(cx, "symbol not found by its (scope, name): map disagrees or duplicate", id);
    }
    check_assigns(cx, id, rec);
}

/* Every chain: members match the key, orders strictly increase; returns the member count. */
static uint32_t check_chains(check_ctx *cx) {
    uint32_t members = 0;
    size_t cursor = 0;
    odin3_kv entry;
    while (odin3_u64map_next(cx->st->children, &cursor, &entry)) {
        uint32_t parent = (uint32_t)(entry.key >> ODIN3_SYMTAB_KEY_SHIFT);
        uint32_t name = (uint32_t)entry.key;
        const odin3_scope *prev = NULL;
        odin3_scope_id at = {(uint32_t)entry.value};
        for (const odin3_scope *rec = odin3_scope_get(cx->st, at); at.v != 0;
             rec = odin3_scope_get(cx->st, at)) {
            if (rec == NULL || rec->parent.v != parent || rec->name != name ||
                (prev != NULL && odin3_scope_order(prev) >= odin3_scope_order(rec))) {
                bad(cx, "scope name chain broken", at.v);
                break;
            }
            members++;
            prev = rec;
            at = rec->same_name_next;
        }
    }
    return members;
}

static void check_maps(check_ctx *cx) {
    if (odin3_u64map_count(cx->st->by_name) != (size_t)cx->n_symbols - 1) {
        bad(cx, "symbol map count differs from the symbols", cx->n_symbols - 1);
    }
    uint32_t named = 0;
    for (uint32_t id = 1; id < cx->n_scopes; id++) {
        named += odin3_scope_get(cx->st, (odin3_scope_id){id})->name != 0;
    }
    if (check_chains(cx) != named) {
        bad(cx, "named scopes missing from or repeated in the child map", named);
    }
}

odin3_status odin3_symtab_check(const odin3_symtab *st, const odin3_ast *ast) {
    if (st == NULL || ast == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_symtab_check: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    check_ctx cx = {st, odin3_ast_node_end(ast).v, (uint32_t)odin3_pagevec_len(st->scopes),
                    (uint32_t)odin3_pagevec_len(st->symbols), 0};
    check_scopes(&cx);
    for (uint32_t id = 1; id < cx.n_symbols; id++) {
        check_symbol(&cx, id, odin3_symbol_get(st, (odin3_sym_id){id}));
    }
    check_maps(&cx);
    return cx.errors == 0 ? ODIN3_OK : ODIN3_ERR_CHECK;
}
