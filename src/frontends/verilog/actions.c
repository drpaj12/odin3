/*
 * actions.c — the generic grammar actions (2C spec §3.2): enum values, list marks and pushes, and
 * the stream's UNIT. The construct actions live in actions_*.c (Tasks 4–7).
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"

#include <stdint.h>

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
