/*
 * ast_internal.h — the AST store's struct and payload records, shared by the src/ast store
 * sources (other code uses ast.h).
 */
#ifndef ODIN3_AST_AST_INTERNAL_H
#define ODIN3_AST_AST_INTERNAL_H

#include "ast/ast.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One payload; its ID is its index + 1. Bytes live in the store's arena. */
typedef struct odin3_ast_payload_rec {
    uint32_t kind; /* odin3_ast_payload_kind */
    union {
        double real;
        const void *number; /* odin3_ast_number in the arena (number.c) */
        struct {
            const uint8_t *ptr;
            size_t len;
        } text;
    } u;
} odin3_ast_payload_rec;

struct odin3_symtab; /* src/ast/symtab.h */

struct odin3_ast {
    odin3_design *design;        /* borrowed: strtab, source manager, diagnostics */
    odin3_ast_form form;         /* fixed at create */
    odin3_passrun_id run;        /* the read run that built it */
    odin3_pagevec *nodes;        /* odin3_ast_node; slot 0 reserved */
    odin3_vec children;          /* odin3_ast_id: every node's span */
    odin3_vec payloads;          /* odin3_ast_payload_rec */
    odin3_arena *arena;          /* payload bytes, comment text */
    odin3_vec units;             /* odin3_ast_id of every UNIT, in order */
    odin3_vec pending;           /* odin3_ast_id: the builder's pending stack (freed at finish) */
    odin3_vec heights;           /* uint16_t per node, slot 0 = 0 (freed at finish) */
    uint32_t max_height;         /* running maximum, kept after finish */
    bool finished;               /* sealed */
    odin3_vec comments;          /* comment.c's records (elem size set there; zeroed until then) */
    odin3_u64map *attrs;         /* attr.c: node ID -> span; NULL until the first attach */
    struct odin3_symtab *symtab; /* symtab.c; NULL until built (owned) */
    uint32_t max_children;       /* ODIN3_AST_MAX_CHILDREN unless a test lowered it */
    uint32_t max_nodes;          /* ODIN3_AST_MAX_NODES unless a test lowered it */
    uint32_t max_depth;          /* ODIN3_AST_MAX_DEPTH unless a test lowered it */
};

/* The record of node, NULL for 0 or an ID at or past the end. */
const odin3_ast_node *odin3_ast_rec(const odin3_ast *ast, odin3_ast_id node);

/* The payload record of payload ID id, NULL when out of range. */
const odin3_ast_payload_rec *odin3_ast_payload_rec_of(const odin3_ast *ast, uint32_t id);

/*
 * Appends a payload record (the caller allocated its bytes in the arena first) and stores its ID.
 * NO_MEMORY with the table unchanged; requires an unsealed store.
 */
odin3_status odin3_ast_payload_push(odin3_ast *ast, odin3_ast_payload_rec rec, uint32_t *payload);

#endif
