/* symtab.c — the symbol table's lifetime and builders: scopes, symbols, dims, values, assigns. */
#include "ast/symtab.h"
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

/* --- lifetime ------------------------------------------------------------------------------ */

void odin3_symtab_destroy(odin3_symtab *st) {
    if (st == NULL) {
        return;
    }
    odin3_pagevec_destroy(st->scopes);
    odin3_pagevec_destroy(st->symbols);
    odin3_vec_free(&st->assigns);
    odin3_vec_free(&st->dims);
    odin3_u64map_destroy(st->by_name);
    odin3_u64map_destroy(st->children);
    odin3_arena_destroy(st->arena);
    odin3_util_free(st);
}

static odin3_symtab *symtab_alloc(odin3_ast *ast) {
    odin3_symtab *st = odin3_util_calloc(sizeof *st);
    if (st == NULL) {
        return NULL;
    }
    st->ast = ast;
    odin3_vec_init(&st->assigns, sizeof(odin3_sym_assign));
    odin3_vec_init(&st->dims, sizeof(odin3_dim));
    st->scopes = odin3_pagevec_create(sizeof(odin3_scope));
    st->symbols = odin3_pagevec_create(sizeof(odin3_symbol));
    st->by_name = odin3_u64map_create(0);
    st->children = odin3_u64map_create(0);
    st->arena = odin3_arena_create(0);
    if (st->scopes == NULL || st->symbols == NULL || st->by_name == NULL || st->children == NULL ||
        st->arena == NULL || odin3_pagevec_push(st->scopes, NULL) == NULL ||
        odin3_pagevec_push(st->symbols, NULL) == NULL) {
        odin3_symtab_destroy(st);
        return NULL;
    }
    return st;
}

odin3_status odin3_symtab_create(odin3_ast *ast, odin3_symtab **out) {
    if (ast == NULL || out == NULL) {
        return misuse("odin3_symtab_create", "NULL argument");
    }
    if (ast->symtab != NULL) {
        return misuse("odin3_symtab_create", "the AST has a symbol table already");
    }
    odin3_symtab *st = symtab_alloc(ast);
    if (st == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ast->symtab = st;
    *out = st;
    return ODIN3_OK;
}

odin3_symtab *odin3_ast_symtab(const odin3_ast *ast) {
    return ast == NULL ? NULL : ast->symtab;
}

/* --- internal lookups ---------------------------------------------------------------------- */

static odin3_scope *scope_at(odin3_symtab *st, odin3_scope_id id) {
    if (odin3_scope_get(st, id) == NULL) {
        return NULL;
    }
    return odin3_pagevec_at(st->scopes, id.v);
}

static odin3_symbol *symbol_at(odin3_symtab *st, odin3_sym_id id) {
    if (odin3_symbol_get(st, id) == NULL) {
        return NULL;
    }
    return odin3_pagevec_at(st->symbols, id.v);
}

/* True for 0 (none) or a node of the owning AST. */
static bool node_ok(const odin3_symtab *st, odin3_ast_id node) {
    return node.v < odin3_ast_node_end(st->ast).v;
}

/* NO_MEMORY when another record would exhaust the 32-bit ID space or cannot be reserved. */
static odin3_status reserve_record(odin3_pagevec *pv) {
    if (odin3_pagevec_len(pv) >= UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return odin3_pagevec_reserve(pv, 1);
}

/* --- scopes -------------------------------------------------------------------------------- */

/* The parent-kind rule of the tree, or NULL when kind may sit under parent. */
static const char *parent_why(const odin3_symtab *st, uint8_t kind, odin3_scope_id parent) {
    if (kind == ODIN3_SCOPE_DESIGN) {
        if (parent.v != 0) {
            return "a DESIGN scope has no parent";
        }
        return st->design.v != 0 ? "a second DESIGN scope" : NULL;
    }
    const odin3_scope *up = odin3_scope_get(st, parent);
    if (up == NULL) {
        return "no such parent scope";
    }
    if (kind == ODIN3_SCOPE_LIBRARY) {
        return up->kind == ODIN3_SCOPE_DESIGN ? NULL : "a LIBRARY's parent must be the DESIGN";
    }
    if (kind == ODIN3_SCOPE_MODULE) {
        return up->kind == ODIN3_SCOPE_LIBRARY ? NULL : "a MODULE's parent must be a LIBRARY";
    }
    return up->kind >= ODIN3_SCOPE_MODULE ? NULL : "a scope under a DESIGN or LIBRARY";
}

static const char *scope_spec_why(const odin3_symtab *st, const odin3_scope_spec *spec) {
    if (spec->kind < ODIN3_SCOPE_DESIGN || spec->kind >= ODIN3_SCOPE_KIND_END) {
        return "unknown scope kind";
    }
    if (spec->has_index && spec->kind != ODIN3_SCOPE_GENERATE && spec->kind != ODIN3_SCOPE_BLOCK) {
        return "only a GENERATE or BLOCK scope has an index";
    }
    if (!spec->has_index && spec->index != 0) {
        return "an index without has_index";
    }
    if (!node_ok(st, spec->node)) {
        return "node is not a node of the AST";
    }
    return parent_why(st, spec->kind, spec->parent);
}

/*
 * Links scope id (named name, not yet linked) into its chain at slot; the map entry for a new
 * head was put by the caller.
 */
static void chain_link(odin3_symtab *st, odin3_scope_id id, odin3_chain_slot slot) {
    scope_at(st, id)->same_name_next = slot.cur;
    if (slot.prev.v != 0) {
        scope_at(st, slot.prev)->same_name_next = id;
    }
}

/* Puts the map entry (parent, name) -> id when slot makes id the chain's head; NO_MEMORY. */
static odin3_status head_put(odin3_symtab *st, odin3_scope_key key, odin3_scope_id id,
                             odin3_chain_slot slot) {
    if (slot.prev.v != 0) {
        return ODIN3_OK;
    }
    odin3_kv entry = {odin3_symtab_key(key.parent.v, key.name), id.v};
    return odin3_u64map_put(st->children, entry);
}

static const char *const CLASH = "a scope with this parent, name and index exists";

/* Appends scope child (made already) to its parent's child list (creation order). */
static void child_link(odin3_symtab *st, odin3_scope_id child) {
    odin3_scope *up = scope_at(st, scope_at(st, child)->parent);
    if (up == NULL) {
        st->design = child;
        return;
    }
    if (up->last_child.v == 0) {
        up->first_child = child;
    } else {
        scope_at(st, up->last_child)->next_sibling = child;
    }
    up->last_child = child;
}

odin3_status odin3_scope_new(odin3_symtab *st, const odin3_scope_spec *spec, odin3_scope_id *out) {
    if (st == NULL || spec == NULL || out == NULL) {
        return misuse("odin3_scope_new", "NULL argument");
    }
    const char *why = scope_spec_why(st, spec);
    if (why != NULL) {
        return misuse("odin3_scope_new", why);
    }
    odin3_scope_id id = {(uint32_t)odin3_pagevec_len(st->scopes)};
    odin3_scope_key key = {spec->parent, spec->name,
                           spec->has_index ? spec->index : ODIN3_SCOPE_NO_INDEX};
    odin3_chain_slot slot = {{0}, {0}, false};
    if (spec->name != 0) {
        slot = odin3_symtab_chain_find(st, key);
        if (slot.found) {
            return misuse("odin3_scope_new", CLASH);
        }
    }
    odin3_status status = reserve_record(st->scopes);
    if (status == ODIN3_OK && spec->name != 0) {
        status = head_put(st, key, id, slot);
    }
    if (status != ODIN3_OK) {
        return status;
    }
    odin3_scope *rec = odin3_pagevec_push(st->scopes, NULL); /* reserved: cannot fail */
    rec->kind = spec->kind;
    rec->has_index = spec->has_index;
    rec->index = spec->index;
    rec->name = spec->name;
    rec->parent = spec->parent;
    rec->node = spec->node;
    if (spec->name != 0) {
        chain_link(st, id, slot);
    }
    child_link(st, id);
    *out = id;
    return ODIN3_OK;
}

odin3_status odin3_scope_set_name(odin3_symtab *st, odin3_scope_id scope, uint32_t name) {
    odin3_scope *rec = st == NULL ? NULL : scope_at(st, scope);
    if (rec == NULL) {
        return misuse("odin3_scope_set_name", "no such scope");
    }
    if (rec->name != 0 || name == 0) {
        return misuse("odin3_scope_set_name", "names only an unnamed scope, with a name");
    }
    odin3_scope_key key = {rec->parent, name, odin3_scope_order(rec)};
    odin3_chain_slot slot = odin3_symtab_chain_find(st, key);
    if (slot.found) {
        return misuse("odin3_scope_set_name", CLASH);
    }
    odin3_status status = head_put(st, key, scope, slot);
    if (status != ODIN3_OK) {
        return status;
    }
    rec->name = name;
    chain_link(st, scope, slot);
    return ODIN3_OK;
}

/* --- symbols ------------------------------------------------------------------------------- */

static const char *sub_why(const odin3_symbol_spec *spec) {
    if (spec->kind == ODIN3_SYM_NET && spec->sub >= ODIN3_AST_NET_DECL_COUNT) {
        return "net kind out of range";
    }
    if (spec->kind == ODIN3_SYM_VAR && spec->sub >= ODIN3_AST_VAR_DECL_COUNT) {
        return "var kind out of range";
    }
    return NULL;
}

static const char *symbol_spec_why(const odin3_symtab *st, const odin3_symbol_spec *spec) {
    if (spec->name == 0) {
        return "a symbol needs a name";
    }
    if (odin3_scope_get(st, spec->scope) == NULL) {
        return "no such scope";
    }
    if (spec->kind < ODIN3_SYM_NET || spec->kind >= ODIN3_SYM_KIND_END) {
        return "unknown symbol kind";
    }
    if ((spec->flags & ~ODIN3_SYM_F_MASK) != 0 || (spec->flags & ODIN3_SYM_F_ASSIGNED_MASK) != 0) {
        return "flags outside the mask or ASSIGNED_* (set by add_assign)";
    }
    if (spec->dir > ODIN3_AST_PORT_DECL_COUNT) {
        return "direction out of range";
    }
    if (!spec->has_packed && (spec->msb != 0 || spec->lsb != 0)) {
        return "a packed range without has_packed";
    }
    if (!node_ok(st, spec->decl) || !node_ok(st, spec->declarator)) {
        return "decl or declarator is not a node of the AST";
    }
    return sub_why(spec);
}

static void symbol_fill(odin3_symbol *rec, const odin3_symbol_spec *spec) {
    rec->name = spec->name;
    rec->scope = spec->scope;
    rec->kind = spec->kind;
    rec->sub = spec->sub;
    rec->dir = spec->dir;
    rec->flags = spec->flags;
    rec->decl = spec->decl;
    rec->declarator = spec->declarator;
    rec->has_packed = spec->has_packed;
    rec->packed = (odin3_dim){spec->msb, spec->lsb};
}

odin3_status odin3_symbol_new(odin3_symtab *st, const odin3_symbol_spec *spec, odin3_sym_id *out) {
    if (st == NULL || spec == NULL || out == NULL) {
        return misuse("odin3_symbol_new", "NULL argument");
    }
    const char *why = symbol_spec_why(st, spec);
    if (why == NULL && odin3_symtab_lookup_local(st, spec->scope, spec->name).v != 0) {
        why = "a symbol with this scope and name exists";
    }
    if (why != NULL) {
        return misuse("odin3_symbol_new", why);
    }
    odin3_sym_id id = {(uint32_t)odin3_pagevec_len(st->symbols)};
    odin3_status status = reserve_record(st->symbols);
    if (status == ODIN3_OK) {
        odin3_kv entry = {odin3_symtab_key(spec->scope.v, spec->name), id.v};
        status = odin3_u64map_put(st->by_name, entry);
    }
    if (status != ODIN3_OK) {
        return status;
    }
    symbol_fill(odin3_pagevec_push(st->symbols, NULL), spec); /* reserved: cannot fail */
    odin3_scope *in = scope_at(st, spec->scope);
    if (in->last_symbol.v == 0) {
        in->first_symbol = id;
    } else {
        symbol_at(st, in->last_symbol)->next_in_scope = id;
    }
    in->last_symbol = id;
    *out = id;
    return ODIN3_OK;
}

static odin3_symbol *symbol_or_log(odin3_symtab *st, odin3_sym_id sym, const char *fn) {
    odin3_symbol *rec = st == NULL ? NULL : symbol_at(st, sym);
    if (rec == NULL) {
        (void)misuse(fn, "no such symbol");
    }
    return rec;
}

odin3_status odin3_symbol_add_flags(odin3_symtab *st, odin3_sym_id sym, uint16_t flags) {
    odin3_symbol *rec = symbol_or_log(st, sym, "odin3_symbol_add_flags");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if ((flags & ~ODIN3_SYM_F_MASK) != 0 || (flags & ODIN3_SYM_F_ASSIGNED_MASK) != 0) {
        return misuse("odin3_symbol_add_flags", "flags outside the mask or ASSIGNED_*");
    }
    rec->flags = (uint16_t)(rec->flags | flags);
    return ODIN3_OK;
}

odin3_status odin3_symbol_set_unpacked(odin3_symtab *st, odin3_sym_id sym, const odin3_dim *dims,
                                       uint32_t n) {
    odin3_symbol *rec = symbol_or_log(st, sym, "odin3_symbol_set_unpacked");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (dims == NULL && n != 0) {
        return misuse("odin3_symbol_set_unpacked", "NULL dims");
    }
    size_t first = st->dims.len;
    if (first + n > UINT32_MAX || (n != 0 && odin3_vec_reserve(&st->dims, first + n) != ODIN3_OK)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < n; i++) {
        *(odin3_dim *)odin3_vec_push(&st->dims) = dims[i]; /* reserved: cannot fail */
    }
    rec->unpacked_first = n == 0 ? 0 : (uint32_t)first;
    rec->n_unpacked = n;
    return ODIN3_OK;
}

odin3_status odin3_symbol_set_value(odin3_symtab *st, odin3_sym_id sym, const odin3_value *value,
                                    bool is_signed) {
    odin3_symbol *rec = symbol_or_log(st, sym, "odin3_symbol_set_value");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!odin3_value_valid(value)) {
        return misuse("odin3_symbol_set_value", "NULL or malformed value");
    }
    odin3_value copy;
    if (odin3_value_copy(st->arena, value, &copy) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rec->value = copy;
    rec->has_value = true;
    rec->value_signed = is_signed;
    return ODIN3_OK;
}

odin3_status odin3_symbol_set_ir(odin3_symtab *st, odin3_sym_id sym, odin3_module_id module,
                                 odin3_wire_id wire) {
    odin3_symbol *rec = symbol_or_log(st, sym, "odin3_symbol_set_ir");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    rec->ir_module = module;
    rec->ir_wire = wire;
    return ODIN3_OK;
}

static uint16_t assigned_flag(odin3_assign_kind kind) {
    if (kind == ODIN3_ASSIGN_BLOCKING) {
        return ODIN3_SYM_F_ASSIGNED_BLOCKING;
    }
    return kind == ODIN3_ASSIGN_NONBLOCKING ? ODIN3_SYM_F_ASSIGNED_NONBLOCKING
                                            : ODIN3_SYM_F_ASSIGNED_CONTINUOUS;
}

odin3_status odin3_symbol_add_assign(odin3_symtab *st, odin3_sym_id sym, odin3_ast_id node,
                                     odin3_assign_kind kind) {
    odin3_symbol *rec = symbol_or_log(st, sym, "odin3_symbol_add_assign");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (node.v == 0 || !node_ok(st, node)) {
        return misuse("odin3_symbol_add_assign", "node is not a node of the AST");
    }
    if (kind < ODIN3_ASSIGN_BLOCKING || kind >= ODIN3_ASSIGN_KIND_END) {
        return misuse("odin3_symbol_add_assign", "unknown assignment kind");
    }
    odin3_assign_id id = {(uint32_t)st->assigns.len + 1};
    odin3_sym_assign *slot = id.v == 0 ? NULL : odin3_vec_push(&st->assigns);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->node = node;
    slot->kind = (uint8_t)kind;
    if (rec->last_assign.v == 0) {
        rec->first_assign = id;
    } else {
        ((odin3_sym_assign *)odin3_vec_at(&st->assigns, rec->last_assign.v - 1))->next = id;
    }
    rec->last_assign = id;
    rec->flags = (uint16_t)(rec->flags | assigned_flag(kind));
    return ODIN3_OK;
}
