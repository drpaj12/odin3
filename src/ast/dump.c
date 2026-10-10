/*
 * dump.c — the AST as an s-expression for golden tests (§6): one node per line, located through
 * the source manager, iterative over an explicit frame stack.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CHILD_INDENT = 2, MAX_COL = CHILD_INDENT * ODIN3_PR_MAX_INDENT };

typedef enum dump_stage { STAGE_CHILDREN = 0, STAGE_ATTRS } dump_stage;

/* One node being dumped: its next child or attribute and its column. */
typedef struct dump_frame {
    uint32_t node;
    uint32_t next;
    uint32_t col;
    uint32_t stage; /* dump_stage */
} dump_frame;

typedef struct dumper {
    const odin3_ast *ast;
    const odin3_srcman *sm;
    odin3_strbuf *out;
    odin3_vec frames; /* dump_frame */
} dumper;

static odin3_bytes name_bytes(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_strtab *tab = odin3_design_strtab(ast->design);
    uint32_t name = odin3_ast_name(ast, node);
    const char *str = odin3_strtab_get(tab, name);
    return (odin3_bytes){(const uint8_t *)(str != NULL ? str : ""), odin3_strtab_len(tab, name)};
}

static odin3_status append_payload(const dumper *dp, odin3_ast_id node, uint32_t payload) {
    odin3_status st = odin3_strbuf_append(dp->out, odin3_bytes_cstr(" "));
    if (st != ODIN3_OK) {
        return st;
    }
    switch (payload) {
    case ODIN3_AST_PAYLOAD_NUMBER:
        return odin3_pr_append_number(dp->out, dp->ast, node);
    case ODIN3_AST_PAYLOAD_REAL:
        return odin3_pr_append_real(dp->out, odin3_ast_real(dp->ast, node));
    default:
        return odin3_pr_append_cstring(dp->out, odin3_ast_text(dp->ast, node));
    }
}

/* `(KIND[.sub][:name] @file:line:col[ payload]`. */
static odin3_status append_head(const dumper *dp, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(dp->ast, node);
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    odin3_status st = odin3_strbuf_appendf(dp->out, "(%s", odin3_ast_kind_name(kind));
    if (st == ODIN3_OK && info->sub_count > 0) {
        st = odin3_strbuf_appendf(dp->out, ".%s", odin3_ast_sub_name(dp->ast, node));
    }
    if (st == ODIN3_OK && odin3_ast_name(dp->ast, node) != 0) {
        st = odin3_strbuf_append(dp->out, odin3_bytes_cstr(":"));
        odin3_bytes name = name_bytes(dp->ast, node);
        if (st == ODIN3_OK) {
            st = kind == ODIN3_AST_STRING ? odin3_pr_append_cstring(dp->out, name)
                                          : odin3_strbuf_append(dp->out, name);
        }
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(dp->out, odin3_bytes_cstr(" @"));
    }
    if (st == ODIN3_OK) {
        st = odin3_srcman_format(dp->sm, odin3_ast_loc(dp->ast, node), dp->out, ODIN3_SRCFMT_GIVEN);
    }
    if (st == ODIN3_OK && info->payload != ODIN3_AST_PAYLOAD_NONE) {
        st = append_payload(dp, node, info->payload);
    }
    return st;
}

/* A new line at col, the `{` or ` ` lead of an attribute, then node's head (or `_` for 0). */
static odin3_status open_line(dumper *dp, uint32_t col, const char *lead, odin3_ast_id node) {
    uint32_t blanks = col < MAX_COL ? col : MAX_COL;
    odin3_status st = odin3_strbuf_appendf(dp->out, "\n%*s%s", (int)blanks, "", lead);
    if (st == ODIN3_OK && node.v == 0) {
        return odin3_strbuf_append(dp->out, odin3_bytes_cstr("_"));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    st = append_head(dp, node);
    dump_frame *frame = st == ODIN3_OK ? odin3_vec_push(&dp->frames) : NULL;
    if (frame == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *frame = (dump_frame){node.v, 0, col + (uint32_t)(lead[0] != '\0'), STAGE_CHILDREN};
    return ODIN3_OK;
}

/* One step of the top frame: the next child or attribute line, or the node's close. */
static odin3_status step(dumper *dp) {
    dump_frame top = *(const dump_frame *)odin3_vec_cat(&dp->frames, dp->frames.len - 1);
    dump_frame *slot = odin3_vec_at(&dp->frames, dp->frames.len - 1);
    odin3_ast_id node = {top.node};
    if (top.stage == STAGE_CHILDREN) {
        odin3_ast_span kids = odin3_ast_children(dp->ast, node);
        if (top.next < kids.n) {
            slot->next++;
            return open_line(dp, top.col + CHILD_INDENT, "", kids.ids[top.next]);
        }
        *slot = (dump_frame){top.node, 0, top.col, STAGE_ATTRS};
        return ODIN3_OK;
    }
    odin3_ast_span attrs = odin3_ast_attrs(dp->ast, node);
    if (top.next < attrs.n) {
        slot->next++;
        return open_line(dp, top.col + CHILD_INDENT, top.next == 0 ? "{" : " ",
                         attrs.ids[top.next]);
    }
    odin3_vec_pop(&dp->frames);
    return odin3_strbuf_append(dp->out, odin3_bytes_cstr(attrs.n > 0 ? "})" : ")"));
}

odin3_status odin3_ast_dump(const odin3_ast *ast, odin3_ast_id root, odin3_strbuf *out) {
    if (ast == NULL || out == NULL || odin3_ast_kind_of(ast, root) == ODIN3_AST_NONE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_dump: %s",
                  ast == NULL || out == NULL ? "NULL argument" : "root is 0 or not made");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(ast->design, &sm);
    if (st != ODIN3_OK) {
        return st;
    }
    dumper dp = {.ast = ast, .sm = sm, .out = out};
    odin3_vec_init(&dp.frames, sizeof(dump_frame));
    st = append_head(&dp, root);
    dump_frame *first = st == ODIN3_OK ? odin3_vec_push(&dp.frames) : NULL;
    if (first != NULL) {
        *first = (dump_frame){root.v, 0, 0, STAGE_CHILDREN};
    } else if (st == ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    while (st == ODIN3_OK && dp.frames.len > 0) {
        st = step(&dp);
    }
    odin3_vec_free(&dp.frames);
    return st;
}
