/*
 * ast_test.h — hidden test hooks for the AST store (never part of the ABI).
 */
#ifndef ODIN3_AST_AST_TEST_H
#define ODIN3_AST_AST_TEST_H

#include "ast/ast.h"

#include <stdint.h>

/* The caps a test may lower so it can hit them cheaply. */
typedef struct odin3_ast_limits {
    uint32_t max_children; /* children per node (ODIN3_AST_MAX_CHILDREN) */
    uint32_t max_nodes;    /* nodes, slot 0 excluded (ODIN3_AST_MAX_NODES) */
    uint32_t max_depth;    /* node height, at most ODIN3_AST_MAX_DEPTH (ODIN3_AST_MAX_DEPTH) */
} odin3_ast_limits;

/* Replaces the store's caps; a max_depth above ODIN3_AST_MAX_DEPTH is clamped to it. */
void odin3_ast_test_set_limits(odin3_ast *ast, odin3_ast_limits limits);

#endif
