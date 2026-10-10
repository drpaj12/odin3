/* comment.c — the comment side table (§7): appended in stream order, sorted by loc at finish. */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The FILE buffer holding loc, or 0. */
static uint32_t file_buffer_of(const odin3_srcman *sm, uint32_t loc) {
    odin3_srcbuf_id buf = odin3_srcman_buffer_of(sm, (odin3_loc){loc});
    odin3_srcbuf_info info;
    if (!odin3_srcman_buffer_info(sm, buf, &info) || info.kind != ODIN3_SRCBUF_FILE) {
        return 0;
    }
    return buf.v;
}

static const char *check_fields(const odin3_ast_comment_spec *spec) {
    if (spec->reserved[0] != 0 || spec->reserved[1] != 0) {
        return "reserved fields must be zero";
    }
    if (spec->kind != ODIN3_AST_COMMENT_LINE && spec->kind != ODIN3_AST_COMMENT_BLOCK) {
        return "unknown comment kind";
    }
    if ((spec->flags & ~(uint32_t)ODIN3_AST_COMMENT_METACOMMENT) != 0) {
        return "unknown comment flag";
    }
    return spec->text_ptr == NULL && spec->text_len > 0 ? "NULL argument" : NULL;
}

/* Why spec cannot be recorded (NULL when it can); *status says INVALID_ARG or NO_MEMORY. */
static const char *check_comment(odin3_ast *ast, const odin3_ast_comment_spec *spec,
                                 odin3_status *status) {
    *status = ODIN3_ERR_INVALID_ARG;
    if (ast == NULL || spec == NULL) {
        return "NULL argument";
    }
    if (ast->finished) {
        return "the store is finished";
    }
    const char *why = check_fields(spec);
    if (why != NULL) {
        return why;
    }
    odin3_srcman *sm = NULL;
    if (odin3_design_get_srcman(ast->design, &sm) != ODIN3_OK) {
        *status = ODIN3_ERR_NO_MEMORY;
        return "out of memory";
    }
    uint32_t buf = file_buffer_of(sm, spec->loc);
    if (buf == 0) {
        return "loc is not in a FILE buffer";
    }
    if (spec->end != 0 &&
        (spec->end < spec->loc || odin3_srcman_buffer_of(sm, (odin3_loc){spec->end}).v != buf)) {
        return "end is not 0 or a later loc of loc's buffer";
    }
    return NULL;
}

odin3_status odin3_ast_comment_add(odin3_ast *ast, const odin3_ast_comment_spec *spec) {
    odin3_status st = ODIN3_OK;
    const char *why = check_comment(ast, spec, &st);
    if (why != NULL) {
        if (st == ODIN3_ERR_INVALID_ARG) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_ast_comment_add: %s", why);
        }
        return st;
    }
    if (ast->comments.len >= UINT32_MAX - 1 ||
        odin3_vec_reserve(&ast->comments, ast->comments.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const char *src = spec->text_ptr != NULL ? spec->text_ptr : "";
    char *text = odin3_arena_strndup(ast->arena, src, spec->text_len);
    if (text == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_ast_comment *slot = odin3_vec_push(&ast->comments);
    *slot = (odin3_ast_comment){.text = text,
                                .loc = {spec->loc},
                                .end = {spec->end},
                                .len = spec->text_len,
                                .kind = (uint8_t)spec->kind,
                                .flags = (uint8_t)spec->flags};
    return ODIN3_OK;
}

/* --- sorting and queries ------------------------------------------------------------------- */

static int cmp_u32(uint32_t lhs, uint32_t rhs) {
    return (lhs > rhs) - (lhs < rhs);
}

/*
 * A total order on comment contents (loc first), so qsort's result is the same as a stable
 * sort's: records equal under it are identical.
 */
static int comment_cmp(const void *lhs, const void *rhs) {
    const odin3_ast_comment *left = lhs;
    const odin3_ast_comment *right = rhs;
    int order = cmp_u32(left->loc.v, right->loc.v);
    order = order != 0 ? order : cmp_u32(left->end.v, right->end.v);
    order = order != 0 ? order : cmp_u32(left->kind, right->kind);
    order = order != 0 ? order : cmp_u32(left->flags, right->flags);
    order = order != 0 ? order : cmp_u32(left->len, right->len);
    return order != 0 ? order : memcmp(left->text, right->text, left->len);
}

void odin3_ast_comments_sort(odin3_ast *ast) {
    if (ast->comments.len > 1) {
        qsort(ast->comments.data, ast->comments.len, sizeof(odin3_ast_comment), comment_cmp);
    }
}

/* The first comment index whose loc is ≥ loc. */
static uint32_t lower_bound(const odin3_ast *ast, uint64_t loc) {
    const odin3_ast_comment *items = ast->comments.data;
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)ast->comments.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (items[mid].loc.v < loc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The comments with lo ≤ loc < hi; empty unless the store is finished and holds comments. */
static odin3_ast_comment_span comments_between(const odin3_ast *ast, uint64_t lo, uint64_t hi) {
    odin3_ast_comment_span none = {NULL, 0};
    if (ast == NULL || !ast->finished || ast->comments.len == 0 || lo >= hi) {
        return none;
    }
    uint32_t first = lower_bound(ast, lo);
    uint32_t last = lower_bound(ast, hi);
    const odin3_ast_comment *items = ast->comments.data;
    return first < last ? (odin3_ast_comment_span){items + first, last - first} : none;
}

/* The design's source manager when comments exist (it was made for them), else NULL. */
static const odin3_srcman *comment_srcman(const odin3_ast *ast) {
    odin3_srcman *sm = NULL;
    if (ast == NULL || ast->comments.len == 0 ||
        odin3_design_get_srcman(ast->design, &sm) != ODIN3_OK) {
        return NULL;
    }
    return sm;
}

odin3_ast_comment_span odin3_ast_comments_in(const odin3_ast *ast, odin3_range range) {
    const odin3_srcman *sm = comment_srcman(ast);
    if (sm == NULL) {
        return (odin3_ast_comment_span){NULL, 0};
    }
    odin3_range mapped = odin3_srcman_expansion_range(sm, range);
    if (mapped.end.v == 0) { /* not in one file */
        return (odin3_ast_comment_span){NULL, 0};
    }
    return comments_between(ast, mapped.loc.v, mapped.end.v);
}

odin3_ast_comment_span odin3_ast_comments_of(const odin3_ast *ast, odin3_srcbuf_id buf) {
    const odin3_srcman *sm = comment_srcman(ast);
    odin3_srcbuf_info info;
    if (sm == NULL || !odin3_srcman_buffer_info(sm, buf, &info) || info.kind != ODIN3_SRCBUF_FILE) {
        return (odin3_ast_comment_span){NULL, 0};
    }
    return comments_between(ast, info.start, (uint64_t)info.start + info.len + 1);
}
