/* attr.c — the attribute map (§7): node ID → span of ATTR nodes in the child table. */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SPAN_COUNT_SHIFT = 32 };

static uint64_t pack_span(uint32_t start, uint32_t n) {
    return ((uint64_t)n << SPAN_COUNT_SHIFT) | start;
}

/* Why attach(node, attrs[0..n)) is misuse, NULL when it is not. */
static const char *check_attach(const odin3_ast *ast, odin3_ast_id node, const odin3_ast_id *attrs,
                                uint32_t n) {
    if (ast == NULL || (attrs == NULL && n > 0)) {
        return "NULL argument";
    }
    if (ast->finished) {
        return "the store is finished";
    }
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    if (kind == ODIN3_AST_NONE || kind == ODIN3_AST_ATTR) {
        return "node is not made or is an ATTR";
    }
    if (n == 0) {
        return "no attributes";
    }
    for (uint32_t i = 0; i < n; i++) {
        if (odin3_ast_kind_of(ast, attrs[i]) != ODIN3_AST_ATTR) {
            return "an entry is not a made ATTR";
        }
    }
    if (ast->attrs != NULL && odin3_u64map_get(ast->attrs, node.v, NULL)) {
        return "node already has attributes";
    }
    return NULL;
}

/* The offset of ids in the child table when the caller passed a view of it, else SIZE_MAX. */
static size_t child_view_offset(const odin3_ast *ast, const odin3_ast_id *ids, uint32_t n) {
    uintptr_t base = (uintptr_t)ast->children.data;
    uintptr_t at = (uintptr_t)ids;
    if (n == 0 || base == 0 || at < base || at >= base + ast->children.len * sizeof(odin3_ast_id)) {
        return SIZE_MAX;
    }
    return (at - base) / sizeof(odin3_ast_id);
}

odin3_status odin3_ast_attach(odin3_ast *ast, odin3_ast_id node, const odin3_ast_id *attrs,
                              uint32_t n) {
    const char *why = check_attach(ast, node, attrs, n);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_attach: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (ast->children.len + n > ODIN3_AST_MAX_CHILD_TABLE) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t alias = child_view_offset(ast, attrs, n);
    if (odin3_vec_reserve(&ast->children, ast->children.len + n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool created = ast->attrs == NULL;
    if (created) {
        ast->attrs = odin3_u64map_create(0);
        if (ast->attrs == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    uint32_t start = (uint32_t)ast->children.len;
    if (odin3_u64map_put(ast->attrs, (odin3_kv){node.v, pack_span(start, n)}) != ODIN3_OK) {
        if (created) { /* back to no map at all */
            odin3_u64map_destroy(ast->attrs);
            ast->attrs = NULL;
        }
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_ast_id *src = attrs;
    if (alias != SIZE_MAX) { /* a view of the child table: reserving may have moved it */
        src = (const odin3_ast_id *)ast->children.data + alias;
    }
    for (uint32_t i = 0; i < n; i++) {
        *(odin3_ast_id *)odin3_vec_push(&ast->children) = src[i];
    }
    return ODIN3_OK;
}

odin3_ast_span odin3_ast_attrs(const odin3_ast *ast, odin3_ast_id node) {
    uint64_t span = 0;
    if (ast == NULL || ast->attrs == NULL || !odin3_u64map_get(ast->attrs, node.v, &span)) {
        return (odin3_ast_span){NULL, 0};
    }
    const odin3_ast_id *ids = ast->children.data;
    return (odin3_ast_span){ids + (uint32_t)span, (uint32_t)(span >> SPAN_COUNT_SHIFT)};
}
