/*
 * actions_expr.c — the expression actions of 2C Task 5 (spec §3.3, §3.6 "Expressions"; the 2A
 * slot table §4.3): operator nodes made from one pending span each (an `expr` leaves its node on
 * the pending stack), replication, and user, hierarchical and system calls. Concatenations,
 * names and selects are Task 4's (actions.c).
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "ast/print.h"
#include "backends/verilog/writer.h"
#include "frontends/verilog/actions_internal.h"
#include "frontends/verilog/parser.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most operands an operator node has (TERNARY). */
enum { VP_MAX_OPERANDS = 3 };

static odin3_status misuse(const char *what) {
    odin3_log(ODIN3_LOG_ERROR, "%s", what);
    return ODIN3_ERR_INVALID_ARG;
}

odin3_status vp_expr(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id *out) {
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    if (span.n != 1) { /* the grammar leaves exactly one node per expr */
        return misuse("vp_expr: not one expression on the pending stack");
    }
    *out = span.ids[0];
    odin3_ast_unwind(ctx->ast, mark);
    return ODIN3_OK;
}

/*
 * The operator node of spec over at from the pending span at mark: lead operands (0 for UNARY,
 * else 1), then the operator's ATTRs, then the remaining operands. The node gets the operands as
 * its children and the ATTRs in one attach, and replaces the span.
 */
static odin3_status operator_node(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                                  uint32_t spec, uint32_t *out) {
    *out = mark;
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    uint32_t lead = (spec & ODIN3_VP_SPEC_BYTE) == ODIN3_AST_UNARY ? 0U : 1U;
    uint32_t attrs = 0;
    while (lead + attrs < span.n &&
           odin3_ast_kind_of(ctx->ast, span.ids[lead + attrs]) == ODIN3_AST_ATTR) {
        attrs++;
    }
    if (span.n < lead + attrs || span.n - attrs > VP_MAX_OPERANDS) {
        return misuse("vp_operator: not an operator's span");
    }
    odin3_ast_id kids[VP_MAX_OPERANDS] = {{0}};
    uint32_t count = 0;
    for (uint32_t i = 0; i < span.n; i++) {
        if (i < lead || i >= lead + attrs) {
            kids[count++] = span.ids[i];
        }
    }
    odin3_ast_spec node = vp_spec_at(spec, at);
    odin3_ast_id made = {0};
    odin3_status st = odin3_ast_make(ctx->ast, &node, kids, count, &made);
    if (st == ODIN3_OK && attrs > 0) { /* the span view does not survive make: take it again */
        span = odin3_ast_pending_span(ctx->ast, mark);
        st = odin3_ast_attach(ctx->ast, made, span.ids + lead, attrs);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_unwind(ctx->ast, mark);
    return odin3_ast_push(ctx->ast, made);
}

odin3_status vp_unary(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t spec,
                      uint32_t *out) {
    return operator_node(ctx, mark, at, spec, out);
}

odin3_status vp_binary(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t spec,
                       uint32_t *out) {
    return operator_node(ctx, mark, at, spec, out);
}

odin3_status vp_binary_not(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t spec,
                           uint32_t *out) {
    odin3_status st = operator_node(ctx, mark, at, spec, out);
    return st == ODIN3_OK ? operator_node(ctx, mark, at, VP_UNARY(NOT), out) : st;
}

odin3_status vp_ternary(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t *out) {
    return operator_node(ctx, mark, at, VP_SPEC(ODIN3_AST_TERNARY, 0), out);
}

odin3_status vp_replicate(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark,
                          odin3_ast_id concat, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, concat);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_REPLICATE, 0), out) : st;
}

/* CALL{SYSTEM, HIER bits of spec} over at; its name and its arguments are set by the caller. */
static odin3_ast_spec call_spec(const odin3_vp_loc *at, uint32_t spec) {
    return vp_spec_at(VP_SPEC(ODIN3_AST_CALL, 0) | spec, at);
}

odin3_status vp_call(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                     odin3_ast_id *out) {
    odin3_ast_spec spec = call_spec(at, VP_NO_FLAGS);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_call_hier(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = call_spec(at, VP_FLAGS(ODIN3_AST_F_CALL_HIER));
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_call_system(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                            odin3_ast_id *out) {
    odin3_ast_spec spec = call_spec(at, VP_FLAGS(ODIN3_AST_F_CALL_SYSTEM));
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

/* The bytes of strtab ID name in ast's design. */
static odin3_bytes name_bytes(const odin3_ast *ast, uint32_t name) {
    const odin3_strtab *tab = odin3_design_strtab(odin3_ast_design(ast));
    const char *str = odin3_strtab_get(tab, name);
    return (odin3_bytes){str != NULL ? str : "", odin3_strtab_len(tab, name)};
}

/*
 * name as the 2A printer writes an identifier: plain or `\name `. A name the 1F writer calls
 * unwritable but the lexer accepts (an escaped identifier holding a backtick, `\f`x `) is written
 * `\name ` too, as the printer writes it, so the printed call re-parses to the same name.
 */
static odin3_status append_name(odin3_strbuf *text, odin3_bytes name) {
    if (odin3_verilog_ident_kind(name) != ODIN3_VERILOG_UNWRITABLE) {
        return odin3_verilog_append_ident(text, name);
    }
    return odin3_strbuf_appendf(text, "\\%.*s ", (int)name.len, (const char *)name.ptr);
}

/* A callee component as the printer writes it: `u`, `\a+b `, or `g[<index>]`. */
static odin3_status append_part(const odin3_ast *ast, odin3_ast_id part, odin3_strbuf *text) {
    if (odin3_ast_kind_of(ast, part) != ODIN3_AST_SELECT) {
        return append_name(text, name_bytes(ast, odin3_ast_name(ast, part)));
    }
    odin3_ast_id base = odin3_ast_child(ast, part, 0);
    odin3_status st = append_name(text, name_bytes(ast, odin3_ast_name(ast, base)));
    st = st == ODIN3_OK ? odin3_strbuf_append(text, odin3_bytes_cstr("[")) : st;
    st = st == ODIN3_OK ? odin3_ast_print(ast, odin3_ast_child(ast, part, 1), text) : st;
    return st == ODIN3_OK ? odin3_strbuf_append(text, odin3_bytes_cstr("]")) : st;
}

odin3_status vp_hier_call_name(odin3_vp_ctx *ctx, uint32_t mark, uint32_t last, uint32_t *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_ast_span parts = odin3_ast_pending_span(ctx->ast, mark);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < parts.n && st == ODIN3_OK; i++) {
        st = append_part(ctx->ast, parts.ids[i], &text);
        st = st == ODIN3_OK ? odin3_strbuf_append(&text, odin3_bytes_cstr(".")) : st;
    }
    st = st == ODIN3_OK ? append_name(&text, name_bytes(ctx->ast, last)) : st;
    st = st == ODIN3_OK ? odin3_ast_intern(ctx->ast, (odin3_bytes){text.data, text.len}, out) : st;
    odin3_strbuf_free(&text);
    odin3_ast_unwind(ctx->ast, mark);
    return st;
}
