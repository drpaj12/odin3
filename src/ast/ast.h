/*
 * ast.h — the AST store (AST-4..11): 24-byte node records, the child table, payloads, the
 * shape-checking builder with its depth cap, and scalar read accessors.
 */
#ifndef ODIN3_AST_AST_H
#define ODIN3_AST_AST_H

#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A node ID, local to its store; 0 = none. IDs are dense from 1 in creation order. */
typedef struct odin3_ast_id {
    uint32_t v;
} odin3_ast_id;

/* The node record (AST-4): the field list is the contract; read it through the accessors. */
typedef struct odin3_ast_node {
    uint8_t kind;   /* odin3_ast_kind */
    uint8_t sub;    /* per-kind sub-kind */
    uint16_t flags; /* per-kind flag bits */
    uint32_t loc;   /* source range (§3.1); end 0 = unknown */
    uint32_t end;
    uint32_t name;   /* strtab ID, 0 = none */
    uint32_t child;  /* first child-table index of the span; payload kinds: the payload ID */
    uint32_t nchild; /* span length (0 for payload kinds) */
} odin3_ast_node;

/* odin3_ast (the store) is declared in ir/design.h; its fields are in ast_internal.h. */

/* The record's scalar fields for a builder call; payload is a payload ID (0 = none). */
typedef struct odin3_ast_spec {
    uint8_t kind;
    uint8_t sub;
    uint16_t flags;
    odin3_loc loc;
    odin3_loc end;
    uint32_t name;
    uint32_t payload;
    uint32_t reserved[2]; /* must be zero */
} odin3_ast_spec;

/* A read-only view of IDs; valid until the next builder call on the store. */
typedef struct odin3_ast_span {
    const odin3_ast_id *ids;
    uint32_t n;
} odin3_ast_span;

/* --- lifetime ------------------------------------------------------------------------------ */

/*
 * A new, empty store of the given form for pass run `run`, using the design's strtab and source
 * manager. INVALID_ARG (logged) for a NULL design or out or an unknown form; NO_MEMORY (with *out
 * unchanged) on out of memory.
 */
odin3_status odin3_ast_create(odin3_design *design, odin3_ast_form form, odin3_passrun_id run,
                              odin3_ast **out);

/* Frees the store and everything it owns (not the strtab's strings). NULL is a no-op. */
void odin3_ast_destroy(odin3_ast *ast);

/* --- building (§5) ------------------------------------------------------------------------- */

/*
 * Appends a node whose children are ids[0..n) (copied into the child table) and stores its ID in
 * *out. The shape is validated against the slot table; a violation, a NULL argument or a sealed
 * store is INVALID_ARG (logged). More than the child cap or a height above ODIN3_AST_MAX_DEPTH
 * is PARSE, located at spec->loc. NO_MEMORY on out of memory or ID exhaustion. Nothing changes
 * on failure.
 */
odin3_status odin3_ast_make(odin3_ast *ast, const odin3_ast_spec *spec, const odin3_ast_id *ids,
                            uint32_t n, odin3_ast_id *out);

/* The pending stack's height (a mark for make_marked/unwind); 0 for NULL or a sealed store. */
uint32_t odin3_ast_mark(const odin3_ast *ast);

/*
 * Pushes node (0, for an E*0 tail entry, or an ID made already) on the pending stack. INVALID_ARG
 * (logged) for a NULL or sealed store or an ID not made yet; NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_ast_push(odin3_ast *ast, odin3_ast_id node);

/*
 * odin3_ast_make with children = the pending stack from mark to its top; on success the stack is
 * popped back to mark. INVALID_ARG (logged) for a mark above the top; otherwise as make, with the
 * stack unchanged on failure.
 */
odin3_status odin3_ast_make_marked(odin3_ast *ast, const odin3_ast_spec *spec, uint32_t mark,
                                   odin3_ast_id *out);

/* Pops the pending stack back to mark (error recovery); a mark at or above the top is a no-op. */
void odin3_ast_unwind(odin3_ast *ast, uint32_t mark);

/* Stores a REAL payload and its payload ID (from 1) in *payload. INVALID_ARG; NO_MEMORY. */
odin3_status odin3_ast_real_new(odin3_ast *ast, double value, uint32_t *payload);

/*
 * Copies text (the raw bytes of an opaque construct) into the store as a TEXT payload and stores
 * its payload ID in *payload. INVALID_ARG (logged) for a NULL argument or a sealed store; PARSE
 * (logged; no location is known here) above ODIN3_AST_MAX_TEXT_BYTES; NO_MEMORY. Unchanged on
 * failure.
 */
odin3_status odin3_ast_text_new(odin3_ast *ast, odin3_bytes text, uint32_t *payload);

/* Interns str in the design's strtab (names live there, §14 Q2); NO_MEMORY; INVALID_ARG. */
odin3_status odin3_ast_intern(odin3_ast *ast, odin3_bytes str, uint32_t *name);

/*
 * Seals the store: frees the build scratch (pending stack, heights); a later builder call is
 * INVALID_ARG. INVALID_ARG (logged) for NULL or a store already sealed. The read API works
 * before and after.
 */
odin3_status odin3_ast_finish(odin3_ast *ast);

/* --- reading (§6): values, never faulting; NONE / 0 / empty for an out-of-range ID ---------- */

odin3_ast_kind odin3_ast_kind_of(const odin3_ast *ast, odin3_ast_id node);
uint32_t odin3_ast_sub(const odin3_ast *ast, odin3_ast_id node);
uint16_t odin3_ast_flags(const odin3_ast *ast, odin3_ast_id node);
odin3_loc odin3_ast_loc(const odin3_ast *ast, odin3_ast_id node);
odin3_loc odin3_ast_end(const odin3_ast *ast, odin3_ast_id node);
uint32_t odin3_ast_name(const odin3_ast *ast, odin3_ast_id node);

/* The node's name bytes from the design strtab ("" for none). */
const char *odin3_ast_name_str(const odin3_ast *ast, odin3_ast_id node);

/*
 * The spelling of the node's sub-kind ("+", "posedge", "casez", …); "" when its kind has none.
 * Never NULL. (By node rather than (kind, sub): two adjacent uint32_t fail the lint gate; a
 * kind-level lookup reads odin3_ast_kind_info_of(kind)->sub_names.)
 */
const char *odin3_ast_sub_name(const odin3_ast *ast, odin3_ast_id node);

uint32_t odin3_ast_nchild(const odin3_ast *ast, odin3_ast_id node);

/* Child idx (slots first, then the tail); 0 when idx ≥ nchild or the slot is absent. */
odin3_ast_id odin3_ast_child(const odin3_ast *ast, odin3_ast_id node, uint32_t idx);

/* The node's children as a view valid until the next make. */
odin3_ast_span odin3_ast_children(const odin3_ast *ast, odin3_ast_id node);

/* The payload ID of a NUMBER, REAL or TEXT node; 0 otherwise. */
uint32_t odin3_ast_payload(const odin3_ast *ast, odin3_ast_id node);

/* A REAL node's value; 0.0 for any other node. */
double odin3_ast_real(const odin3_ast *ast, odin3_ast_id node);

/* A TEXT-payload node's bytes (owned by the store); {NULL, 0} for any other node. */
odin3_bytes odin3_ast_text(const odin3_ast *ast, odin3_ast_id node);

/* One past the last node ID (1 for an empty store); 0 for NULL. */
odin3_ast_id odin3_ast_node_end(const odin3_ast *ast);

odin3_ast_form odin3_ast_form_of(const odin3_ast *ast);
odin3_passrun_id odin3_ast_run(const odin3_ast *ast);

/* The largest node height so far (valid during the build and after finish). */
uint32_t odin3_ast_max_height(const odin3_ast *ast);

/* The UNIT nodes, in creation order. */
uint32_t odin3_ast_root_count(const odin3_ast *ast);
odin3_ast_id odin3_ast_root(const odin3_ast *ast, uint32_t idx);

/* The design whose strtab and source manager the store uses. */
const odin3_design *odin3_ast_design(const odin3_ast *ast);

/* Bytes the store holds: nodes, child table, payload table and arena (strtab excluded). */
size_t odin3_ast_bytes_reserved(const odin3_ast *ast);

#endif
