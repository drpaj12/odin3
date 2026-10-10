/* ast_test.c — hidden test hooks for the AST store. */
#include "ast/ast_test.h"
#include "ast/ast_internal.h"

#include <stdint.h>

void odin3_ast_test_set_limits(odin3_ast *ast, odin3_ast_limits limits) {
    ast->max_children = limits.max_children;
    ast->max_nodes = limits.max_nodes;
    ast->max_depth =
        limits.max_depth < ODIN3_AST_MAX_DEPTH ? limits.max_depth : ODIN3_AST_MAX_DEPTH;
}
