/*
 * actions_inst.c — the instantiation, gate and generate actions of 2C Task 7 (spec §3.1 (g), (i),
 * (k), §3.6 "Generate", "Instantiation", "Gates"; the 2A slot table §4.3).
 *
 * Two reductions place leading slots the grammar could not push in order (§3.1 g: nothing
 * nullable may precede a gate's `(`): an unnamed instance's absent array slot and a gate's
 * STRENGTH/DELAY slots. Each copies its pending span into the context's span_scratch behind the
 * leading slots, within the one reduction (§3.2).
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "frontends/verilog/actions_internal.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdint.h>

/* The leading slots of a reordered node (GATE_DECL: strength, delay; INSTANCE: its array). */
enum { VP_MAX_LEAD = 2 };

typedef struct vp_lead {
    odin3_ast_id ids[VP_MAX_LEAD];
    uint32_t n;
} vp_lead;

/* The node of spec whose children are lead's slots then the pending span from mark (left on the
 * stack: the caller unwinds), built through span_scratch. NO_MEMORY; PARSE (make's caps). */
static odin3_status make_led(odin3_vp_ctx *ctx, const odin3_ast_spec *spec, const vp_lead *lead,
                             uint32_t mark, odin3_ast_id *out) {
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    odin3_vec *kids = &ctx->span_scratch;
    odin3_vec_clear(kids);
    if (odin3_vec_reserve(kids, (size_t)lead->n + span.n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < lead->n + span.n; i++) {
        odin3_ast_id *slot = odin3_vec_push(kids); /* reserved: cannot fail */
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = i < lead->n ? lead->ids[i] : span.ids[i - lead->n];
    }
    return odin3_ast_make(ctx->ast, spec, (const odin3_ast_id *)kids->data, (uint32_t)kids->len,
                          out);
}

static odin3_ast_kind kind_at(const odin3_vp_ctx *ctx, odin3_ast_span span, uint32_t at) {
    return at < span.n ? odin3_ast_kind_of(ctx->ast, span.ids[at]) : ODIN3_AST_NONE;
}

/* --- instantiation and gates ------------------------------------------------------------------ */

odin3_status vp_connection(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                           odin3_ast_id expr, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_CONNECTION, 0), at);
    spec.name = name;
    return odin3_ast_make(ctx->ast, &spec, &expr, 1, out);
}

odin3_status vp_instance(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                         odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_INSTANCE, 0), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_instance_unnamed(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                                 odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_INSTANCE, 0), at);
    const vp_lead no_array = {{{0}}, 1};
    odin3_status st = make_led(ctx, &spec, &no_array, mark, out);
    if (st == ODIN3_OK) {
        odin3_ast_unwind(ctx->ast, mark);
    }
    return st;
}

odin3_status vp_instantiation(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                              uint32_t name, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_INSTANTIATION, 0), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_gate(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                     odin3_ast_id *out) {
    odin3_ast_span span = odin3_ast_pending_span(ctx->ast, mark);
    uint32_t attrs = 0;
    while (kind_at(ctx, span, attrs) == ODIN3_AST_ATTR) {
        attrs++;
    }
    uint32_t next = attrs;
    vp_lead slots = {{{0}, {0}}, VP_MAX_LEAD}; /* STRENGTH?, DELAY? */
    if (kind_at(ctx, span, next) == ODIN3_AST_STRENGTH) {
        slots.ids[0] = span.ids[next++];
    }
    if (kind_at(ctx, span, next) == ODIN3_AST_DELAY) {
        slots.ids[1] = span.ids[next++];
    }
    odin3_ast_spec node = vp_spec_at(spec, at);
    odin3_status st = make_led(ctx, &node, &slots, mark + next, out);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_unwind(ctx->ast, mark + attrs);
    return vp_attrs(ctx, mark, *out, out);
}

/* --- generate ------------------------------------------------------------------------------- */

odin3_status vp_generate(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                         odin3_ast_id *out) {
    return vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_GENERATE, 0), out);
}

odin3_status vp_gen_block(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_GEN_BLOCK, 0), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_gen_for(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id body,
                        odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, body);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_GEN_FOR, 0), out) : st;
}

odin3_status vp_gen_if(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, odin3_ast_id other,
                       odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, other);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_GEN_IF, 0), out) : st;
}

odin3_status vp_gen_case(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id expr,
                         uint32_t *out) {
    return vp_case_head(ctx, at, VP_SPEC(ODIN3_AST_GEN_CASE, 0), expr, out);
}
