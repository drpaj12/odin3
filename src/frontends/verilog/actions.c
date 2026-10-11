/*
 * actions.c — the shared grammar actions (2C spec §3.2): enum values, list marks and pushes, the
 * stream's UNIT (Task 1); spec values, items and their attributes, literals, leaves and the A.8.5
 * names and selects (Task 4). The construct actions live in actions_*.c (Tasks 4–7).
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/actions_internal.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Task 1 ---------------------------------------------------------------------------------- */

odin3_status vp_value(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out) {
    (void)ctx;
    *out = value;
    return ODIN3_OK;
}

odin3_status vp_mark(const odin3_vp_ctx *ctx, uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    return ODIN3_OK;
}

odin3_status vp_push(odin3_vp_ctx *ctx, odin3_ast_id node, const uint32_t *list) {
    (void)list; /* the list's value is its mark, which passes through */
    return odin3_ast_push(ctx->ast, node);
}

odin3_status vp_unit(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, odin3_ast_id *out) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_UNIT,
                           .loc = at->loc.v,
                           .end = at->end.v,
                           .name = odin3_vl_stream_name(ctx->lexer)};
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

/* --- shared helpers (actions_internal.h) ------------------------------------------------------ */

odin3_ast_spec vp_spec_at(uint32_t spec, const odin3_vp_loc *at) {
    return (odin3_ast_spec){
        .kind = (uint8_t)(spec & ODIN3_VP_SPEC_BYTE),
        .sub = (uint8_t)((spec >> ODIN3_VP_SPEC_SUB_SHIFT) & ODIN3_VP_SPEC_BYTE),
        .flags = (uint16_t)(spec >> ODIN3_VP_SPEC_FLAGS_SHIFT),
        .loc = at->loc.v,
        .end = at->end.v,
    };
}

odin3_status vp_marked(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                       odin3_ast_id *out) {
    odin3_ast_spec node = vp_spec_at(spec, at);
    return odin3_ast_make_marked(ctx->ast, &node, mark, out);
}

/* Attaches the pending stack from mark (ATTR nodes, if any) to node and pops it. */
static odin3_status attach_from(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id node) {
    odin3_ast_span attrs = odin3_ast_pending_span(ctx->ast, mark);
    odin3_status st = attrs.n > 0 ? odin3_ast_attach(ctx->ast, node, attrs.ids, attrs.n) : ODIN3_OK;
    odin3_ast_unwind(ctx->ast, mark);
    return st;
}

odin3_status vp_build(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                      odin3_ast_id *out) {
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    uint32_t attrs = 0;
    while (attrs < span.n && odin3_ast_kind_of(ctx->ast, span.ids[attrs]) == ODIN3_AST_ATTR) {
        attrs++;
    }
    odin3_status st = vp_marked(ctx, mark + attrs, at, spec, out);
    return st != ODIN3_OK ? st : attach_from(ctx, mark, *out);
}

odin3_status vp_name(odin3_vp_ctx *ctx, uint32_t mark, bool always_hier, odin3_ast_id *out) {
    odin3_ast_span parts = odin3_ast_pending_span(ctx->ast, mark);
    if (!always_hier && parts.n == 1 &&
        odin3_ast_kind_of(ctx->ast, parts.ids[0]) == ODIN3_AST_IDENT) {
        *out = parts.ids[0];
        odin3_ast_unwind(ctx->ast, mark);
        return ODIN3_OK;
    }
    odin3_vp_loc at = {{0}, {0}, 0};
    if (parts.n > 0) {
        at.loc = odin3_ast_loc(ctx->ast, parts.ids[0]);
        at.end = odin3_ast_end(ctx->ast, parts.ids[parts.n - 1]);
    }
    return vp_marked(ctx, mark, &at, VP_SPEC(ODIN3_AST_HIER_NAME, 0), out);
}

void vp_logged_error(odin3_vp_ctx *ctx, odin3_loc at) {
    odin3_vl_run *run = ctx->run;
    if (run->errors < run->limits.max_errors) {
        run->errors++;
    } else if (!run->stop) { /* the builder has printed it already: the stop line follows */
        run->stop = true;
        odin3_diag(run->design, ODIN3_LOG_ERROR, at, "too many errors; stopping");
    }
    odin3_vp_enter_error(ctx);
}

/* --- values, lists and slots ------------------------------------------------------------------ */

odin3_status vp_none(odin3_vp_ctx *ctx, odin3_ast_id *out) {
    (void)ctx;
    *out = (odin3_ast_id){0};
    return ODIN3_OK;
}

odin3_status vp_pass(odin3_vp_ctx *ctx, odin3_ast_id node, odin3_ast_id *out) {
    (void)ctx;
    *out = node;
    return ODIN3_OK;
}

odin3_status vp_or(odin3_vp_ctx *ctx, uint32_t left, uint32_t right, uint32_t *out) {
    (void)ctx;
    *out = left | right;
    return ODIN3_OK;
}

odin3_status vp_list_first(odin3_vp_ctx *ctx, odin3_ast_id node, uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    return odin3_ast_push(ctx->ast, node);
}

odin3_status vp_list_next(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id node, uint32_t *out) {
    *out = list;
    return odin3_ast_push(ctx->ast, node);
}

odin3_status vp_list_two(odin3_vp_ctx *ctx, odin3_ast_id first, odin3_ast_id second,
                         uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    odin3_status st = odin3_ast_push(ctx->ast, first);
    return st != ODIN3_OK ? st : odin3_ast_push(ctx->ast, second);
}

odin3_status vp_push_none(odin3_vp_ctx *ctx, uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    return odin3_ast_push(ctx->ast, (odin3_ast_id){0});
}

odin3_status vp_push_slot(odin3_vp_ctx *ctx, odin3_ast_id slot, uint32_t *out) {
    *out = odin3_ast_mark(ctx->ast);
    return odin3_ast_push(ctx->ast, slot);
}

odin3_status vp_spec_slot(odin3_vp_ctx *ctx, uint32_t spec, odin3_ast_id slot, uint32_t *out) {
    *out = spec;
    return odin3_ast_push(ctx->ast, slot);
}

odin3_status vp_leaf(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t spec, odin3_ast_id *out) {
    odin3_ast_spec node = vp_spec_at(spec, at);
    return odin3_ast_make(ctx->ast, &node, NULL, 0, out);
}

/* --- attributes and items --------------------------------------------------------------------- */

odin3_status vp_attrs(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id node, odin3_ast_id *out) {
    *out = node;
    return attach_from(ctx, mark, node);
}

odin3_status vp_item(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                     odin3_ast_id *out) {
    return vp_build(ctx, mark, at, spec, out);
}

odin3_status vp_item_push(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id node, uint32_t *out) {
    *out = mark;
    odin3_status st = attach_from(ctx, mark, node);
    return st != ODIN3_OK ? st : odin3_ast_push(ctx->ast, node);
}

odin3_status vp_item_next(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id node, uint32_t *out) {
    *out = list;
    return node.v != 0 ? odin3_ast_push(ctx->ast, node) : ODIN3_OK;
}

/* A named node of kind over at with the one child child (0: its optional slot absent). */
static odin3_status named1(odin3_vp_ctx *ctx, const odin3_ast_spec *spec, odin3_ast_id child,
                           odin3_ast_id *out) {
    return odin3_ast_make(ctx->ast, spec, &child, 1, out);
}

odin3_status vp_attr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, odin3_ast_id *out) {
    return vp_attr_value(ctx, at, name, (odin3_ast_id){0}, out);
}

odin3_status vp_attr_value(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                           odin3_ast_id value, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_ATTR, 0), at);
    spec.name = name;
    return named1(ctx, &spec, value, out);
}

/* --- literals and leaves ---------------------------------------------------------------------- */

odin3_status vp_piece(odin3_vp_ctx *ctx, odin3_bytes text, uint32_t *out) {
    *out = ctx->npieces;
    if (ctx->npieces >= ODIN3_VP_MAX_PIECES) { /* every literal reduces before the next starts */
        odin3_log(ODIN3_LOG_ERROR, "vp_piece: more than %d literal pieces", ODIN3_VP_MAX_PIECES);
        return ODIN3_ERR_INVALID_ARG;
    }
    ctx->pieces[ctx->npieces++] = text;
    return ODIN3_OK;
}

/* The pieces from mark as number_parse's three views (one: a plain decimal's digits; two: base
 * and digits; three: size, base and digits), popped. */
static odin3_number_text take_pieces(odin3_vp_ctx *ctx, uint32_t mark) {
    const odin3_bytes *at = &ctx->pieces[mark];
    odin3_bytes none = {NULL, 0};
    uint32_t count = ctx->npieces - mark;
    ctx->npieces = mark;
    switch (count) {
    case 1:
        return (odin3_number_text){none, none, at[0]};
    case 2:
        return (odin3_number_text){none, at[0], at[1]};
    default:
        return (odin3_number_text){at[0], at[1], at[2]};
    }
}

odin3_status vp_number(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark,
                       odin3_ast_id *out) {
    *out = (odin3_ast_id){0};
    odin3_number_text pieces = take_pieces(ctx, mark);
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_NUMBER, 0), at);
    odin3_status st = odin3_ast_number_parse(ctx->ast, at->loc, pieces, &spec.payload);
    if (st == ODIN3_ERR_PARSE) { /* located by number_parse: `'b12`, too many digits */
        vp_logged_error(ctx, at->loc);
        return ODIN3_OK;
    }
    st = st == ODIN3_OK ? odin3_ast_number_kind(pieces, &spec.sub, &spec.flags) : st;
    return st == ODIN3_OK ? odin3_ast_make(ctx->ast, &spec, NULL, 0, out) : st;
}

odin3_status vp_digitless(odin3_vp_ctx *ctx, const odin3_vp_loc *base) {
    odin3_bytes text = ctx->npieces > 0 ? ctx->pieces[ctx->npieces - 1] : (odin3_bytes){NULL, 0};
    ctx->npieces = 0; /* the literal is abandoned: no piece outlives it */
    odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, base->loc, "expected digits after %.*s",
                    (int)text.len, (const char *)text.ptr);
    odin3_vp_enter_error(ctx);
    return ODIN3_OK;
}

odin3_status vp_real(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t payload,
                     odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_REAL, 0), at);
    spec.payload = payload;
    return odin3_ast_make(ctx->ast, &spec, NULL, 0, out);
}

odin3_status vp_string(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                       odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_STRING, 0), at);
    spec.name = name;
    spec.flags = name == 0 ? ODIN3_AST_F_STRING_EMPTY : 0; /* "" is strtab ID 0 (2A §4.3) */
    return odin3_ast_make(ctx->ast, &spec, NULL, 0, out);
}

odin3_status vp_ident(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_IDENT, 0), at);
    spec.name = name;
    return odin3_ast_make(ctx->ast, &spec, NULL, 0, out);
}

/* --- A.8.5 names and selects ------------------------------------------------------------------ */

odin3_status vp_hier_first(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                           uint32_t *out) {
    return vp_hier_next(ctx, odin3_ast_mark(ctx->ast), at, name, out);
}

odin3_status vp_hier_next(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          uint32_t *out) {
    *out = mark;
    odin3_ast_id part = {0};
    odin3_status st = vp_ident(ctx, at, name, &part);
    return st != ODIN3_OK ? st : odin3_ast_push(ctx->ast, part);
}

odin3_status vp_hier_index(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id index, uint32_t *out) {
    *out = mark;
    uint32_t top = odin3_ast_mark(ctx->ast);
    if (top <= mark) { /* the name has no component: not reachable from the grammar */
        odin3_log(ODIN3_LOG_ERROR, "vp_hier_index: a name without components");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_id last = odin3_ast_pending_span(ctx->ast, top - 1).ids[0];
    odin3_vp_loc range = {odin3_ast_loc(ctx->ast, last), at->end, 0};
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_BIT), &range);
    const odin3_ast_id kids[] = {last, index, {0}};
    odin3_ast_id select = {0};
    odin3_status st = odin3_ast_make(ctx->ast, &spec, kids, sizeof kids / sizeof kids[0], &select);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_unwind(ctx->ast, top - 1);
    return odin3_ast_push(ctx->ast, select);
}

odin3_status vp_hier(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id *out) {
    return vp_name(ctx, mark, false, out);
}

odin3_status vp_select_hier(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id index, odin3_ast_id *out) {
    odin3_ast_id base = {0};
    odin3_status st = vp_name(ctx, mark, false, &base);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_BIT), at);
    const odin3_ast_id kids[] = {base, index, {0}};
    return odin3_ast_make(ctx->ast, &spec, kids, sizeof kids / sizeof kids[0], out);
}

odin3_status vp_select_open(odin3_vp_ctx *ctx, uint32_t mark, odin3_ast_id first, uint32_t *out) {
    *out = mark;
    odin3_ast_id base = {0};
    odin3_status st = vp_name(ctx, mark, false, &base);
    st = st == ODIN3_OK ? odin3_ast_push(ctx->ast, base) : st;
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, first) : st;
}

odin3_status vp_select_bit(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    st = st == ODIN3_OK ? odin3_ast_push(ctx->ast, (odin3_ast_id){0}) : st; /* no second bound */
    return st == ODIN3_OK
               ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_BIT), out)
               : st;
}

odin3_status vp_select_part(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    return st == ODIN3_OK
               ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART), out)
               : st;
}

odin3_status vp_select_plus(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                            odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at,
                                      VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART_PLUS), out)
                          : st;
}

odin3_status vp_select_minus(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                             odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at,
                                      VP_SPEC(ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART_MINUS), out)
                          : st;
}

odin3_status vp_concat(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                       odin3_ast_id *out) {
    return vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_CONCAT, 0), out);
}
