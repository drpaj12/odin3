/*
 * check.h — the AST structural check (spec §10, rules 1–8; AST-9's elaborated subset as rule 7).
 */
#ifndef ODIN3_AST_CHECK_H
#define ODIN3_AST_CHECK_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/check.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Checks the store against spec §10. FAST runs rule 1 (without decoding locations), 2, 4 and 5;
 * FULL runs all eight (rule 6's comment order only after finish, rule 7 only on an ELABORATED
 * store, rule 8 only when odin3_ast_symtab(ast) is non-NULL). Each violation is logged at
 * ODIN3_LOG_ERROR as "ast check: rule <n>: <kind> <id>: <what>" (<kind> a node kind name, or
 * node / payload / comment / attrs / store / scope / symbol / symtab); per rule the first few
 * are shown and the rest summed in one line. Rule 3's count of unreachable non-root nodes (an
 * abandoned subtree's root, an unattached ATTR) is logged at ODIN3_LOG_INFO, never an error.
 * ODIN3_ERR_CHECK when any rule is violated; ODIN3_ERR_NO_MEMORY when the scratch arrays (4 bytes
 * per node, plus 1 under FULL) or the design's source manager cannot be allocated (nothing is
 * logged as a violation then); ODIN3_ERR_INVALID_ARG (logged) for a NULL store or a level outside
 * its enum. Never changes the store; iterative (one forward sweep over node IDs, AST-5).
 */
odin3_status odin3_ast_check(const odin3_ast *ast, odin3_check_level level);

/* --- internals (src/ast/check*.c only) ----------------------------------------------------- */

enum { ODIN3_AST_CHECK_RULES = 8 };

/* One check run's state. */
typedef struct odin3_ast_checker {
    const odin3_ast *ast;
    const odin3_srcman *sm; /* FULL only; NULL under FAST */
    bool full;
    uint32_t end;                               /* one past the last node ID */
    uint32_t *heights;                          /* rule 5: per node */
    uint8_t *refs;                              /* rule 3: per node (FULL only) */
    uint32_t errors[ODIN3_AST_CHECK_RULES + 1]; /* per rule, [0] unused */
    odin3_ast_id id;                            /* the node being checked */
    const odin3_ast_node *rec;                  /* its record */
} odin3_ast_checker;

/* Where a violation is: the rule, a label (a kind name, "comment", …) and an ID. */
typedef struct odin3_ast_check_site {
    uint32_t rule;
    const char *label;
    uint32_t id;
} odin3_ast_check_site;

/* Counts a violation and logs it while the rule is under its display cap. */
void odin3_ast_check_fail(odin3_ast_checker *cx, odin3_ast_check_site site, const char *what);

/* odin3_ast_check_fail at the node being checked (cx->id, labelled by its kind). */
void odin3_ast_check_node_fail(odin3_ast_checker *cx, uint32_t rule, const char *what);

/*
 * The children of node when it exists, takes no payload and its span lies inside the child
 * table; empty otherwise (so a corrupt record is never read past the table).
 */
odin3_ast_span odin3_ast_check_span(const odin3_ast_checker *cx, odin3_ast_id node);

/* Child idx of node through odin3_ast_check_span; 0 when absent. */
odin3_ast_id odin3_ast_check_kid(const odin3_ast_checker *cx, odin3_ast_id node, uint32_t idx);

/* Rule 7 for the node being checked (check_elab.c): a known kind whose span is readable. */
void odin3_ast_check_elab_node(odin3_ast_checker *cx);

/* Rule 7's store-level part: exactly one UNIT. */
void odin3_ast_check_elab_store(odin3_ast_checker *cx);

#endif
