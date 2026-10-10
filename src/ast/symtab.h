/*
 * symtab.h — the symbol table (AST-14): scopes, symbols, lookup, assignment lists, IR remap.
 */
#ifndef ODIN3_AST_SYMTAB_H
#define ODIN3_AST_SYMTAB_H

#include "ast/ast.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* IDs local to one symbol table; 0 = none; dense from 1 in creation order. */
typedef struct odin3_scope_id {
    uint32_t v;
} odin3_scope_id;

typedef struct odin3_sym_id {
    uint32_t v;
} odin3_sym_id;

typedef struct odin3_assign_id {
    uint32_t v;
} odin3_assign_id;

typedef struct odin3_symtab odin3_symtab;

/* The tree: DESIGN -> LIBRARY (one per project library) -> MODULE -> GENERATE/BLOCK/FUNCTION/TASK.
 */
typedef enum odin3_scope_kind {
    ODIN3_SCOPE_DESIGN = 1,
    ODIN3_SCOPE_LIBRARY,
    ODIN3_SCOPE_MODULE,
    ODIN3_SCOPE_GENERATE,
    ODIN3_SCOPE_BLOCK,
    ODIN3_SCOPE_FUNCTION,
    ODIN3_SCOPE_TASK,
    ODIN3_SCOPE_KIND_END
} odin3_scope_kind;

/* One namespace per scope holds every kind. */
typedef enum odin3_sym_kind {
    ODIN3_SYM_NET = 1,
    ODIN3_SYM_VAR,
    ODIN3_SYM_PARAM,
    ODIN3_SYM_LOCALPARAM,
    ODIN3_SYM_GENVAR,
    ODIN3_SYM_INSTANCE,
    ODIN3_SYM_BLOCK,
    ODIN3_SYM_FUNCTION,
    ODIN3_SYM_TASK,
    ODIN3_SYM_MODULE,
    ODIN3_SYM_EVENT,
    ODIN3_SYM_KIND_END
} odin3_sym_kind;

/* Symbol flags. The ASSIGNED_* bits are set only by odin3_symbol_add_assign. */
enum {
    ODIN3_SYM_F_SIGNED = 1U << 0,
    ODIN3_SYM_F_IMPLICIT = 1U << 1, /* an implicit net (M10) */
    ODIN3_SYM_F_PORT = 1U << 2,
    ODIN3_SYM_F_OVERRIDDEN = 1U << 3, /* set by #() or defparam */
    ODIN3_SYM_F_ASSIGNED_BLOCKING = 1U << 4,
    ODIN3_SYM_F_ASSIGNED_NONBLOCKING = 1U << 5,
    ODIN3_SYM_F_ASSIGNED_CONTINUOUS = 1U << 6,
    ODIN3_SYM_F_ASSIGNED_MASK = 0x7U << 4,
    ODIN3_SYM_F_MASK = 0x7FU,
};

/* How a node assigns a symbol; selects the matching ASSIGNED_* flag. */
typedef enum odin3_assign_kind {
    ODIN3_ASSIGN_BLOCKING = 1,
    ODIN3_ASSIGN_NONBLOCKING,
    ODIN3_ASSIGN_CONTINUOUS,
    ODIN3_ASSIGN_KIND_END
} odin3_assign_kind;

/* A range bound pair; direction preserved ([0:3] stays msb 0, lsb 3). */
typedef struct odin3_dim {
    int32_t msb;
    int32_t lsb;
} odin3_dim;

/* A read-only view of dims, valid until the next set_unpacked on the table. */
typedef struct odin3_dim_span {
    const odin3_dim *dims;
    uint32_t n;
} odin3_dim_span;

/* odin3_scope_child's index for a scope without one (a plain named block). */
#define ODIN3_SCOPE_NO_INDEX INT64_MIN

/* A scope record. Read it through odin3_scope_get; only symtab.c writes it. */
typedef struct odin3_scope {
    uint8_t kind;   /* odin3_scope_kind */
    bool has_index; /* a generate-loop iteration */
    int32_t index;  /* 0 unless has_index */
    uint32_t name;  /* strtab ID; 0 = unnamed */
    odin3_scope_id parent;
    odin3_ast_id node; /* the AST node that opened it (0 = none) */
    odin3_scope_id first_child, last_child, next_sibling; /* creation order */
    odin3_scope_id same_name_next;          /* next same-named sibling, in index order */
    odin3_sym_id first_symbol, last_symbol; /* ID order */
} odin3_scope;

/* A symbol record. Read it through odin3_symbol_get; only symtab.c writes it. */
typedef struct odin3_symbol {
    uint32_t name; /* strtab ID, never 0 */
    odin3_scope_id scope;
    uint8_t kind; /* odin3_sym_kind */
    uint8_t sub;  /* odin3_ast_net_kind or odin3_ast_var_kind */
    uint8_t dir;  /* 0 = none, else 1 + odin3_ast_direction */
    bool has_packed;
    uint16_t flags; /* ODIN3_SYM_F_* */
    bool has_value;
    bool value_signed;
    odin3_ast_id decl; /* an implicit net: the first use */
    odin3_ast_id declarator;
    odin3_dim packed;        /* valid when has_packed */
    uint32_t unpacked_first; /* index into the dims table */
    uint32_t n_unpacked;     /* outermost first */
    odin3_value value;       /* payload in the symtab arena; valid when has_value */
    odin3_assign_id first_assign, last_assign; /* source order */
    odin3_module_id ir_module;                 /* 0 until 2D creates the IR object */
    odin3_wire_id ir_wire;                     /* module-local; remapped by odin3_symtab_remap */
    odin3_sym_id next_in_scope;
} odin3_symbol;

/* One assignment of a symbol. */
typedef struct odin3_sym_assign {
    odin3_ast_id node;
    uint8_t kind; /* odin3_assign_kind */
    odin3_assign_id next;
} odin3_sym_assign;

typedef struct odin3_scope_spec {
    uint8_t kind; /* odin3_scope_kind */
    uint32_t name;
    int32_t index;
    bool has_index;
    odin3_scope_id parent;
    odin3_ast_id node;
} odin3_scope_spec;

typedef struct odin3_symbol_spec {
    uint32_t name;
    odin3_scope_id scope;
    uint8_t kind;
    uint8_t sub;
    uint16_t flags; /* no ASSIGNED_* bits */
    uint8_t dir;
    odin3_ast_id decl;
    odin3_ast_id declarator;
    int32_t msb;
    int32_t lsb;
    bool has_packed;
} odin3_symbol_spec;

/* --- lifetime ------------------------------------------------------------------------------ */

/*
 * Creates the AST's symbol table (one per AST, owned by it: odin3_ast_destroy frees it) and
 * stores it in *out. INVALID_ARG (logged) for a NULL argument or an AST that has one already;
 * NO_MEMORY with the AST unchanged.
 */
odin3_status odin3_symtab_create(odin3_ast *ast, odin3_symtab **out);

/* The AST's symbol table; NULL when it has none (or ast is NULL). */
odin3_symtab *odin3_ast_symtab(const odin3_ast *ast);

/* --- building ------------------------------------------------------------------------------ */

/*
 * Adds a scope. Shape: DESIGN has no parent (at most one); LIBRARY's parent is DESIGN; MODULE's
 * is LIBRARY; GENERATE/BLOCK/FUNCTION/TASK's is MODULE or one of those four. has_index only on
 * GENERATE/BLOCK, index 0 without it; node an existing AST node or 0. A named scope whose
 * (parent, name, index) exists is INVALID_ARG. All misuse is INVALID_ARG (logged); NO_MEMORY on
 * out of memory or ID exhaustion. Unchanged on failure.
 */
odin3_status odin3_scope_new(odin3_symtab *st, const odin3_scope_spec *spec, odin3_scope_id *out);

/* Names an unnamed scope (2D's genblkN). INVALID_ARG for a named scope, name 0 or a clash. */
odin3_status odin3_scope_set_name(odin3_symtab *st, odin3_scope_id scope, uint32_t name);

/*
 * Adds a symbol. INVALID_ARG (logged) on a duplicate (scope, name) (2D reports M11 with both
 * decls), name 0, an unknown kind, flags outside the mask or with an ASSIGNED_* bit, dir past
 * 1 + the last direction, msb/lsb non-zero without has_packed, or decl/declarator not an AST node.
 * NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_symbol_new(odin3_symtab *st, const odin3_symbol_spec *spec, odin3_sym_id *out);

/* ORs in SIGNED/IMPLICIT/PORT/OVERRIDDEN bits (not ASSIGNED_*); INVALID_ARG otherwise. */
odin3_status odin3_symbol_add_flags(odin3_symtab *st, odin3_sym_id sym, uint16_t flags);

/* Copies n unpacked dims (outermost first) into the table; replaces any earlier span. */
odin3_status odin3_symbol_set_unpacked(odin3_symtab *st, odin3_sym_id sym, const odin3_dim *dims,
                                       uint32_t n);

/* Copies a valid value (payload into the symtab arena) and its signedness; replaces any earlier. */
odin3_status odin3_symbol_set_value(odin3_symtab *st, odin3_sym_id sym, const odin3_value *value,
                                    bool is_signed);

/* Records the IR objects the symbol became. INVALID_ARG for an unknown symbol. */
odin3_status odin3_symbol_set_ir(odin3_symtab *st, odin3_sym_id sym, odin3_module_id module,
                                 odin3_wire_id wire);

/*
 * Appends an assigning node to the symbol's list (source order is the call order) and sets the
 * matching ASSIGNED_* flag. INVALID_ARG for an unknown symbol, node 0 or not an AST node, or an
 * unknown kind; NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_symbol_add_assign(odin3_symtab *st, odin3_sym_id sym, odin3_ast_id node,
                                     odin3_assign_kind kind);

/* --- lookup and iteration (0 / NULL for anything out of range) ----------------------------- */

/* The symbol named name in scope or the nearest enclosing scope (walks parent outward). */
odin3_sym_id odin3_symtab_lookup(const odin3_symtab *st, odin3_scope_id scope, uint32_t name);

/* The symbol named name in scope itself. */
odin3_sym_id odin3_symtab_lookup_local(const odin3_symtab *st, odin3_scope_id scope, uint32_t name);

/*
 * The child scope of parent named name with the given loop index (g[2]), or the one without an
 * index for ODIN3_SCOPE_NO_INDEX (a plain named block).
 */
odin3_scope_id odin3_scope_child(const odin3_symtab *st, odin3_scope_id parent, uint32_t name,
                                 int64_t index);

odin3_scope_id odin3_scope_first_child(const odin3_symtab *st, odin3_scope_id scope);
odin3_scope_id odin3_scope_next_sibling(const odin3_symtab *st, odin3_scope_id scope);
odin3_sym_id odin3_scope_first_symbol(const odin3_symtab *st, odin3_scope_id scope);
odin3_sym_id odin3_scope_next_symbol(const odin3_symtab *st, odin3_sym_id sym);
odin3_assign_id odin3_symbol_first_assign(const odin3_symtab *st, odin3_sym_id sym);
odin3_assign_id odin3_symbol_next_assign(const odin3_symtab *st, odin3_assign_id assign);

const odin3_scope *odin3_scope_get(const odin3_symtab *st, odin3_scope_id scope);
const odin3_symbol *odin3_symbol_get(const odin3_symtab *st, odin3_sym_id sym);
const odin3_sym_assign *odin3_assign_get(const odin3_symtab *st, odin3_assign_id assign);

/* The symbol's unpacked dims; {NULL, 0} when it has none. */
odin3_dim_span odin3_symbol_unpacked(const odin3_symtab *st, odin3_sym_id sym);

/* Records in the table (slot 0 excluded); 0 for NULL. */
uint32_t odin3_symtab_scope_count(const odin3_symtab *st);
uint32_t odin3_symtab_symbol_count(const odin3_symtab *st);

/* --- IR remap and check -------------------------------------------------------------------- */

/*
 * After odin3_module_compact(module, map): every symbol whose ir_module is module gets
 * ir_wire = map->wire[ir_wire] (0 for a dead or out-of-map wire). Other modules are untouched.
 */
void odin3_symtab_remap(odin3_symtab *st, odin3_module_id module, const odin3_compact_map *map);

/*
 * Check rule 8 (§10): every scope's parent and every symbol's scope exist, AST node IDs are below
 * ast's end, the maps and lists agree with the records, no duplicate (scope, name). Each error is
 * logged as "ast check: rule 8: <scope|symbol|symtab> <id>: <what>" (the first 10; the rest are
 * summed in one line); ODIN3_ERR_CHECK if any; INVALID_ARG (logged) for NULL. odin3_ast_check
 * (FULL) calls it when the AST has a table.
 */
odin3_status odin3_symtab_check(const odin3_symtab *st, const odin3_ast *ast);

/* --- internals (src/ast/symtab*.c only) ---------------------------------------------------- */

struct odin3_symtab {
    odin3_ast *ast;         /* the owner */
    odin3_pagevec *scopes;  /* odin3_scope; slot 0 reserved */
    odin3_pagevec *symbols; /* odin3_symbol; slot 0 reserved */
    odin3_vec assigns;      /* odin3_sym_assign; ID = index + 1 */
    odin3_vec dims;         /* odin3_dim */
    odin3_u64map *by_name;  /* (scope << 32) | name -> symbol */
    odin3_u64map *children; /* (parent << 32) | name -> first same-named child */
    odin3_arena *arena;     /* value payloads */
    odin3_scope_id design;  /* the DESIGN scope, 0 until made */
};

/* Frees the table; called only by odin3_ast_destroy. NULL is a no-op. */
void odin3_symtab_destroy(odin3_symtab *st);

enum { ODIN3_SYMTAB_KEY_SHIFT = 32 };

/* The map key (hi << 32) | lo. */
static inline uint64_t odin3_symtab_key(uint32_t hi, uint32_t lo) {
    return ((uint64_t)hi << ODIN3_SYMTAB_KEY_SHIFT) | lo;
}

/* The chain order of a scope: its index, or NO_INDEX (first) without one. */
static inline int64_t odin3_scope_order(const odin3_scope *scope) {
    return scope->has_index ? (int64_t)scope->index : ODIN3_SCOPE_NO_INDEX;
}

/* A position in the same-name chain of (parent, name): the scope of chain order `order`. */
typedef struct odin3_scope_key {
    odin3_scope_id parent;
    uint32_t name;
    int64_t order;
} odin3_scope_key;

/* prev: the last member ordered before key (0 = key would be the head); cur: the member after
 * prev (0 = none); found: cur has key's order. */
typedef struct odin3_chain_slot {
    odin3_scope_id prev, cur;
    bool found;
} odin3_chain_slot;

odin3_chain_slot odin3_symtab_chain_find(const odin3_symtab *st, odin3_scope_key key);

#endif
