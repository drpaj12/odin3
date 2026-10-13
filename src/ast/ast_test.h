/*
 * ast_test.h — hidden test hooks for the AST store (never part of the ABI, never called outside
 * tests/unit): cap overrides, and corruption hooks that each break one check rule (§10) on purpose.
 */
#ifndef ODIN3_AST_AST_TEST_H
#define ODIN3_AST_AST_TEST_H

#include "ast/ast.h"
#include "odin3/odin3.h"

#include <stdint.h>

/* The caps a test may lower so it can hit them cheaply. */
typedef struct odin3_ast_limits {
    uint32_t max_children; /* children per node (ODIN3_AST_MAX_CHILDREN) */
    uint32_t max_nodes;    /* nodes, slot 0 excluded (ODIN3_AST_MAX_NODES) */
    uint32_t max_depth;    /* node height, at most ODIN3_AST_MAX_DEPTH (ODIN3_AST_MAX_DEPTH) */
} odin3_ast_limits;

/* Replaces the store's caps; a max_depth above ODIN3_AST_MAX_DEPTH is clamped to it. */
void odin3_ast_test_set_limits(odin3_ast *ast, odin3_ast_limits limits);

/*
 * One fault per check line the builder cannot produce (states it can build, such as a child of
 * the wrong class, are built directly by the tests). `id` names a node unless the fault says
 * otherwise; `arg` is a second node or name where the fault uses one.
 */
typedef enum odin3_ast_test_fault {
    ODIN3_AST_TEST_KIND = 1,      /* rule 1: node id's kind = ODIN3_AST_KIND_COUNT */
    ODIN3_AST_TEST_SUB,           /* rule 1: sub = the kind's sub count (1 when it has none) */
    ODIN3_AST_TEST_FLAG,          /* rule 1: gains the lowest flag bit its kind does not define */
    ODIN3_AST_TEST_PAYLOAD_PLAIN, /* rule 1: a childless node of a kind without a payload gets
                                     payload 1 */
    ODIN3_AST_TEST_PAYLOAD_INDEX, /* rule 1: a payload node's payload = one past the last */
    ODIN3_AST_TEST_LOC,           /* rule 1: loc = UINT32_MAX, beyond every buffer */
    ODIN3_AST_TEST_LOC_ORDER,     /* rule 1: loc and end swap (needs 0 < loc < end) */
    ODIN3_AST_TEST_COUNT,         /* rule 2: nchild drops by one (needs no tail entries, or the
                                     tail at its minimum, so a count rule breaks) */
    ODIN3_AST_TEST_MANDATORY,     /* rule 2: the first mandatory slot becomes 0 */
    ODIN3_AST_TEST_TAIL_ZERO,     /* rule 2: the first tail entry becomes 0 */
    ODIN3_AST_TEST_CHILD_ABOVE,   /* rule 2: the first non-zero child becomes arg (≥ id) */
    ODIN3_AST_TEST_CHILD_SHARED,  /* rule 3: the first non-zero child becomes arg (< id, not that
                                     child): rule 3 when arg is in another span, else rule 5
                                     when it changes the height */
    ODIN3_AST_TEST_NAME_MISSING,  /* rule 4: a named node loses its name */
    ODIN3_AST_TEST_NAME_BAD,      /* rule 4: name = the strtab's count (no such string) */
    ODIN3_AST_TEST_NAME_EXTRA,    /* rule 4: a kind without names gets name arg */
    ODIN3_AST_TEST_HEIGHT,        /* rule 5: the recorded maximum height grows by one (no id) */
    ODIN3_AST_TEST_COMMENT_ORDER, /* rule 6: comment indices id and id + 1 swap */
    ODIN3_AST_TEST_ATTR_SPAN,     /* rule 6: node id's attribute span runs past the table */
    ODIN3_AST_TEST_NUMBER_XZ,     /* rule 6: NUMBER id's payload has_xz flips */
    ODIN3_AST_TEST_FORM,          /* rule 7: the store's form becomes ELABORATED (no id) */
    ODIN3_AST_TEST_SYMBOL_SCOPE,  /* rule 8: symbol id's scope = one past the last scope */
    ODIN3_AST_TEST_SYMBOL_NAME,   /* rule 8: symbol id's name becomes arg (the map disagrees) */
    ODIN3_AST_TEST_SPAN_OUT,      /* rule 2: a node with children: its span starts at the end
                                     of the child table */
    ODIN3_AST_TEST_PAYLOAD_CHILDREN, /* rule 1: a payload node gets nchild 1 */
    ODIN3_AST_TEST_END,              /* rule 1: end = UINT32_MAX, beyond every buffer */
    ODIN3_AST_TEST_NOT_SYSTEM,       /* rule 2: a CALL/TASK_CALL loses SYSTEM (needs a 0 tail
                                        entry, which then lies outside E*0) */
    ODIN3_AST_TEST_NUMBER_BIT,       /* rule 6: NUMBER id's bit 0 becomes 4 (not 0 1 x z) */
    ODIN3_AST_TEST_NUMBER_EMPTY,     /* rule 6: NUMBER id's nbits = 0 */
    ODIN3_AST_TEST_NUMBER_WIDE,      /* rule 6: NUMBER id's width = the cap + 1 */
    ODIN3_AST_TEST_NUMBER_QUESTION,  /* rule 6: NUMBER id (no z bit) gains has_question */
    ODIN3_AST_TEST_ATTR_ENTRY,       /* rule 6: node id's first attribute becomes node arg (an
                                        existing node that is not an ATTR) */
    ODIN3_AST_TEST_ATTR_EMPTY,       /* rule 6: node id's attribute count = 0 */
    ODIN3_AST_TEST_ATTR_KEY,         /* rule 6: node id's attributes move to key arg (an ATTR or
                                        no node; it has no attributes yet) */
    ODIN3_AST_TEST_FAULT_END
} odin3_ast_test_fault;

/* A fault and what it is applied to. */
typedef struct odin3_ast_test_target {
    odin3_ast_test_fault fault;
    uint32_t id;
    uint32_t arg;
} odin3_ast_test_target;

/*
 * Applies the fault, writing the store's fields directly (bypassing the builder's guarantees).
 * ODIN3_ERR_INVALID_ARG (logged; nothing changes) when the store is NULL, the fault is unknown
 * or its precondition does not hold (an existing node of the right shape, a symbol table for
 * the rule 8 faults, two comments at different locs for COMMENT_ORDER, an annotated node for
 * the ATTR_* faults); ODIN3_ERR_NO_MEMORY (nothing changes) from an ATTR_* map update.
 */
odin3_status odin3_ast_test_corrupt(odin3_ast *ast, odin3_ast_test_target target);

/*
 * Writes node's loc and end directly (the location oracle's seeded faults, odin3-parse-check:
 * a range the check accepts but the oracle must not). ODIN3_ERR_INVALID_ARG (logged; nothing
 * changes) for a NULL store or a node not made.
 */
odin3_status odin3_ast_test_set_range(odin3_ast *ast, odin3_ast_id node, odin3_range range);

/*
 * Masks the keys of every symbol table's indexed scope map (symtab.h), process-wide; UINT64_MAX
 * (the default) restores them. A narrow mask forces collisions, so a test can check that chain
 * lookups stay exact through the fallback walk. Set it before a table is built and keep it until
 * the table is destroyed: entries keep the keys they were put with.
 */
void odin3_symtab_test_set_index_mask(uint64_t mask);

#endif
