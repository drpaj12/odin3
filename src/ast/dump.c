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

/* One flag or field of a kind (§4.4), in bit order; a field (mask wider than a bit) is NAME=v. */
typedef struct flag_name {
    uint8_t kind;
    uint8_t shift;
    uint16_t mask;
    const char *name;
} flag_name;

#define FLAG_(KIND, NAME)                                                                          \
    { ODIN3_AST_##KIND, 0, ODIN3_AST_F_##KIND##_##NAME, #NAME }
#define FIELD_(KIND, NAME)                                                                         \
    {                                                                                              \
        ODIN3_AST_##KIND, ODIN3_AST_F_##KIND##_##NAME##_SHIFT, ODIN3_AST_F_##KIND##_##NAME##_MASK, \
            #NAME                                                                                  \
    }

static const flag_name FLAG_NAMES[] = {
    FLAG_(MODULE, MACROMODULE),
    FLAG_(PORT_REF, DOTTED),
    FLAG_(PORT_DECL, SIGNED),
    FLAG_(PORT_DECL, IN_HEADER),
    FIELD_(PORT_DECL, DT),
    FIELD_(PORT_DECL, NET),
    FLAG_(NET_DECL, SIGNED),
    FLAG_(NET_DECL, VECTORED),
    FLAG_(NET_DECL, SCALARED),
    FLAG_(VAR_DECL, SIGNED),
    FLAG_(PARAM_DECL, SIGNED),
    FLAG_(PARAM_DECL, IN_HEADER),
    FIELD_(PARAM_DECL, DT),
    FIELD_(STRENGTH, S0),
    FIELD_(STRENGTH, S1),
    FLAG_(FUNCTION_DECL, AUTOMATIC),
    FLAG_(FUNCTION_DECL, SIGNED),
    FIELD_(FUNCTION_DECL, DT),
    FLAG_(TASK_DECL, AUTOMATIC),
    FLAG_(ATTR, METACOMMENT),
    FLAG_(TASK_CALL, SYSTEM),
    FLAG_(TASK_CALL, HIER),
    FLAG_(CASE_ITEM, DEFAULT),
    FLAG_(EVENT_CONTROL, STAR),
    FLAG_(EVENT_CONTROL, NO_PARENS),
    FLAG_(EVENT_EXPR, AFTER_OR),
    FLAG_(NUMBER, SIGNED),
    FLAG_(STRING, EMPTY),
    FLAG_(CALL, SYSTEM),
    FLAG_(CALL, HIER),
};

#undef FLAG_
#undef FIELD_

/* ` [NAME NAME FIELD=v …]` for non-zero flags; bits the kind does not define as `0x…`. */
static odin3_status append_flags(const dumper *dp, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(dp->ast, node);
    uint32_t flags = odin3_ast_flags(dp->ast, node);
    odin3_status st = odin3_strbuf_append(dp->out, odin3_bytes_cstr(" ["));
    const char *sep = "";
    for (size_t i = 0; i < sizeof FLAG_NAMES / sizeof FLAG_NAMES[0] && st == ODIN3_OK; i++) {
        const flag_name *fl = &FLAG_NAMES[i];
        uint32_t value = (flags & fl->mask) >> fl->shift;
        if (fl->kind != kind || value == 0) {
            continue;
        }
        flags &= ~(uint32_t)fl->mask;
        bool field = (fl->mask & (fl->mask - 1U)) != 0; /* more than one bit */
        st = field ? odin3_strbuf_appendf(dp->out, "%s%s=%u", sep, fl->name, (unsigned)value)
                   : odin3_strbuf_appendf(dp->out, "%s%s", sep, fl->name);
        sep = " ";
    }
    if (st == ODIN3_OK && flags != 0) {
        st = odin3_strbuf_appendf(dp->out, "%s0x%x", sep, (unsigned)flags);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(dp->out, odin3_bytes_cstr("]")) : st;
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

/* `:name` when the node has one; a STRING always shows its value (`:""` for the empty string,
 * whose name is 0). */
static odin3_status append_name(const dumper *dp, odin3_ast_id node, odin3_ast_kind kind) {
    if (odin3_ast_name(dp->ast, node) == 0 && kind != ODIN3_AST_STRING) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_strbuf_append(dp->out, odin3_bytes_cstr(":"));
    odin3_bytes name = odin3_pr_name_bytes(dp->ast, node);
    if (st != ODIN3_OK) {
        return st;
    }
    return kind == ODIN3_AST_STRING ? odin3_pr_append_cstring(dp->out, name)
                                    : odin3_strbuf_append(dp->out, name);
}

/* `(KIND[.sub][:name] @file:line:col[ payload]`. */
static odin3_status append_head(const dumper *dp, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(dp->ast, node);
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    odin3_status st = odin3_strbuf_appendf(dp->out, "(%s", odin3_ast_kind_name(kind));
    if (st == ODIN3_OK && info->sub_count > 0) {
        st = odin3_strbuf_appendf(dp->out, ".%s", odin3_ast_sub_name(dp->ast, node));
    }
    if (st == ODIN3_OK) {
        st = append_name(dp, node, kind);
    }
    if (st == ODIN3_OK && odin3_ast_flags(dp->ast, node) != 0) {
        st = append_flags(dp, node);
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
