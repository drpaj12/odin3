/* walk.c — iterative AST traversal (walk, cursor) and the on-demand parent index (§6). */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/u64map.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

static bool is_made(const odin3_ast *ast, odin3_ast_id node) {
    return odin3_ast_rec(ast, node) != NULL;
}

/* --- cursor -------------------------------------------------------------------------------- */

static odin3_status cursor_start(odin3_ast_cursor *cur, const odin3_ast *ast, odin3_ast_id root,
                                 const char *fn) {
    if (cur == NULL) {
        return misuse(fn, "NULL argument");
    }
    memset(cur, 0, sizeof *cur);
    if (ast == NULL) {
        return misuse(fn, "NULL argument");
    }
    if (!is_made(ast, root)) {
        return misuse(fn, "root is 0 or not made");
    }
    /* height(root) ≤ max_height bounds the depth; a made node has height ≥ 1 */
    uint32_t cap = ast->max_height > 0 ? ast->max_height : 1;
    odin3_ast_frame *frames = odin3_util_malloc((size_t)cap * sizeof *frames);
    if (frames == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    frames[0] = (odin3_ast_frame){root.v, 0};
    *cur = (odin3_ast_cursor){.ast = ast, .frames = frames, .depth = 1, .cap = cap, .fresh = true};
    return ODIN3_OK;
}

odin3_status odin3_ast_cursor_init(odin3_ast_cursor *cur, const odin3_ast *ast, odin3_ast_id root) {
    return cursor_start(cur, ast, root, "odin3_ast_cursor_init");
}

/* The next non-zero child of frame's node (advancing frame->next), 0 when none is left. */
static uint32_t next_child(const odin3_ast *ast, odin3_ast_frame *frame) {
    odin3_ast_span kids = odin3_ast_children(ast, (odin3_ast_id){frame->node});
    while (frame->next < kids.n) {
        uint32_t child = kids.ids[frame->next++].v;
        if (child != 0) {
            return child;
        }
    }
    return 0;
}

static bool emit_enter(odin3_ast_cursor *cur, uint32_t id, odin3_ast_event *ev,
                       odin3_ast_id *node) {
    cur->entered = true;
    *ev = ODIN3_AST_ENTER;
    *node = (odin3_ast_id){id};
    return true;
}

static bool emit_leave(odin3_ast_cursor *cur, uint32_t id, odin3_ast_event *ev,
                       odin3_ast_id *node) {
    cur->entered = false;
    *ev = ODIN3_AST_LEAVE;
    *node = (odin3_ast_id){id};
    return true;
}

bool odin3_ast_cursor_next(odin3_ast_cursor *cur, odin3_ast_event *ev, odin3_ast_id *node) {
    if (cur == NULL || ev == NULL || node == NULL || cur->depth == 0) {
        return false;
    }
    odin3_ast_frame *top = &cur->frames[cur->depth - 1];
    if (cur->fresh) {
        cur->fresh = false;
        return emit_enter(cur, top->node, ev, node);
    }
    uint32_t child = next_child(cur->ast, top);
    if (child == 0) {
        cur->depth--;
        return emit_leave(cur, top->node, ev, node);
    }
    if (cur->depth == cur->cap) { /* deeper than the recorded height: a corrupted store */
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_ast_cursor_next: node %" PRIu32
                  " is deeper than the store's recorded height %" PRIu32,
                  child, cur->cap);
        cur->corrupt = true;
        cur->depth = 0;
        return false;
    }
    cur->frames[cur->depth++] = (odin3_ast_frame){child, 0};
    return emit_enter(cur, child, ev, node);
}

void odin3_ast_cursor_skip(odin3_ast_cursor *cur) {
    if (cur == NULL || !cur->entered || cur->depth == 0) {
        return;
    }
    cur->frames[cur->depth - 1].next = UINT32_MAX;
    cur->entered = false;
}

void odin3_ast_cursor_free(odin3_ast_cursor *cur) {
    if (cur == NULL) {
        return;
    }
    odin3_util_free(cur->frames);
    memset(cur, 0, sizeof *cur);
}

/* --- walk ---------------------------------------------------------------------------------- */

odin3_status odin3_ast_walk(const odin3_ast *ast, odin3_ast_id root, odin3_ast_pre_fn pre,
                            odin3_ast_post_fn post, void *user) {
    odin3_ast_cursor cur;
    odin3_status st = cursor_start(&cur, ast, root, "odin3_ast_walk");
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_event ev = ODIN3_AST_ENTER;
    odin3_ast_id node = {0};
    while (odin3_ast_cursor_next(&cur, &ev, &node)) {
        if (ev == ODIN3_AST_LEAVE) {
            if (post != NULL) {
                post(ast, node, user);
            }
        } else if (pre != NULL && pre(ast, node, user) == ODIN3_AST_WALK_SKIP) {
            odin3_ast_cursor_skip(&cur);
        }
    }
    st = cur.corrupt ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    odin3_ast_cursor_free(&cur);
    return st;
}

/* --- parent index -------------------------------------------------------------------------- */

static void set_span_parents(const odin3_ast *ast, odin3_ast_id *parents, uint32_t end) {
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_span kids = odin3_ast_children(ast, (odin3_ast_id){id});
        for (uint32_t i = 0; i < kids.n; i++) {
            if (kids.ids[i].v != 0) {
                parents[kids.ids[i].v] = (odin3_ast_id){id};
            }
        }
    }
}

static void set_attr_parents(const odin3_ast *ast, odin3_ast_id *parents) {
    size_t cursor = 0;
    odin3_kv entry = {0, 0};
    while (ast->attrs != NULL && odin3_u64map_next(ast->attrs, &cursor, &entry)) {
        odin3_ast_span attrs = odin3_ast_attrs(ast, (odin3_ast_id){(uint32_t)entry.key});
        for (uint32_t i = 0; i < attrs.n; i++) {
            parents[attrs.ids[i].v] = (odin3_ast_id){(uint32_t)entry.key};
        }
    }
}

odin3_status odin3_ast_parent_index_build(const odin3_ast *ast, odin3_ast_parent_index *out) {
    if (ast == NULL || out == NULL) {
        return misuse("odin3_ast_parent_index_build", "NULL argument");
    }
    uint32_t end = odin3_ast_node_end(ast).v;
    odin3_ast_id *parents = odin3_util_calloc((size_t)end * sizeof *parents);
    if (parents == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    set_span_parents(ast, parents, end);
    set_attr_parents(ast, parents);
    *out = (odin3_ast_parent_index){parents, end};
    return ODIN3_OK;
}

odin3_ast_id odin3_ast_parent(const odin3_ast_parent_index *ix, odin3_ast_id node) {
    if (ix == NULL || ix->parents == NULL || node.v >= ix->n) {
        return (odin3_ast_id){0};
    }
    return ix->parents[node.v];
}

void odin3_ast_parent_index_destroy(odin3_ast_parent_index *ix) {
    if (ix == NULL) {
        return;
    }
    odin3_util_free(ix->parents);
    *ix = (odin3_ast_parent_index){NULL, 0};
}
