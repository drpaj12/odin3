/*
 * actions_decl.c — the declaration actions of 2C Task 4 (spec §3.6 "Units and directives",
 * "Modules and ports", "Parameters", "Nets and variables", "Continuous assignment"): opaque
 * units and blocks, directives, modules and their header lists, ports, declarators, ranges,
 * strengths, delays, defparam and net assignments. Each maps keywords to 2A enums; no token code
 * is ever stored.
 */
#include "frontends/verilog/actions.h"

#include "ast/ast.h"
#include "frontends/verilog/actions_internal.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One signed_opt serves every declaration kind (VP_SIGNED): their SIGNED bits agree. */
_Static_assert(ODIN3_AST_F_PORT_DECL_SIGNED == ODIN3_AST_F_NET_DECL_SIGNED &&
                   ODIN3_AST_F_PORT_DECL_SIGNED == ODIN3_AST_F_VAR_DECL_SIGNED &&
                   ODIN3_AST_F_PORT_DECL_SIGNED == ODIN3_AST_F_PARAM_DECL_SIGNED,
               "2A §4.4: SIGNED is bit 0 of the declaration kinds");
/* One IN_HEADER bit for the two kinds a header group makes (2A §4.4: bit 1 of both). */
_Static_assert(ODIN3_AST_F_PORT_DECL_IN_HEADER == ODIN3_AST_F_PARAM_DECL_IN_HEADER,
               "2A §4.4: IN_HEADER is bit 1 of PORT_DECL and PARAM_DECL");

/* A node of spec over at with the children ids[0..n). */
static odin3_status make_n(odin3_vp_ctx *ctx, const odin3_ast_spec *spec, const odin3_ast_id *ids,
                           uint32_t n, odin3_ast_id *out) {
    return odin3_ast_make(ctx->ast, spec, ids, n, out);
}

/* --- units, directives, modules ---------------------------------------------------------- */

odin3_status vp_opaque(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_bytes body, uint32_t kind,
                       odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(kind, 0), at);
    if (kind != ODIN3_AST_SPECIFY_BLOCK) {
        spec.name = odin3_vl_opaque_name(ctx->lexer, at->off);
        if (spec.name == 0) { /* the lexer reported the missing name and is in error mode */
            odin3_vp_enter_error(ctx);
            *out = (odin3_ast_id){0};
            return ODIN3_OK;
        }
    }
    odin3_status st = odin3_ast_text_new(ctx->ast, body, &spec.payload);
    return st == ODIN3_OK ? make_n(ctx, &spec, NULL, 0, out) : st;
}

odin3_status vp_directive(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_bytes arg,
                          odin3_ast_id *out) {
    uint32_t kind = odin3_vl_directive_kind(ctx->lexer, at->off);
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_DIRECTIVE, kind), at);
    odin3_status st = arg.len > 0 ? odin3_ast_intern(ctx->ast, arg, &spec.name) : ODIN3_OK;
    return st == ODIN3_OK ? make_n(ctx, &spec, NULL, 0, out) : st;
}

/* The MODULE of spec from mark; endmodule empties the pending metacomments (§2.6). */
static odin3_status module_end(odin3_vp_ctx *ctx, uint32_t mark, const odin3_ast_spec *spec,
                               odin3_ast_id *out) {
    odin3_vl_meta_clear(ctx->lexer);
    return odin3_ast_make_marked(ctx->ast, spec, mark, out);
}

odin3_status vp_module(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                       odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_MODULE, 0), at);
    spec.name = name;
    return module_end(ctx, mark, &spec, out);
}

odin3_status vp_macromodule(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                            odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_MODULE, 0), at);
    spec.name = name;
    spec.flags = ODIN3_AST_F_MODULE_MACROMODULE;
    return module_end(ctx, mark, &spec, out);
}

odin3_status vp_list_slot(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark, uint32_t *out) {
    *out = mark;
    odin3_ast_id list = {0};
    odin3_status st = vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_LIST, 0), &list);
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, list) : st;
}

odin3_status vp_group_list_slot(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t mark,
                                uint32_t *out) {
    odin3_status st = vp_group_close(ctx, out);
    return st == ODIN3_OK ? vp_list_slot(ctx, at, mark, out) : st;
}

/* --- header groups ------------------------------------------------------------------------- */

odin3_status vp_group_close(odin3_vp_ctx *ctx, uint32_t *out) {
    *out = 0;
    if (!ctx->group.open) {
        return ODIN3_OK;
    }
    ctx->group.open = false;
    odin3_ast_id node = {0};
    odin3_status st = vp_build(ctx, ctx->group.mark, &ctx->group.at, ctx->group.spec, &node);
    return st == ODIN3_OK ? odin3_ast_push(ctx->ast, node) : st;
}

odin3_status vp_group_open(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                           uint32_t *out) {
    ctx->group =
        (odin3_vp_group){true, mark, spec | VP_FLAGS(ODIN3_AST_F_PORT_DECL_IN_HEADER), *at};
    *out = mark;
    return ODIN3_OK;
}

odin3_status vp_group_first(odin3_vp_ctx *ctx, odin3_ast_id range, odin3_ast_id decl,
                            uint32_t *out) {
    *out = ctx->group.mark;
    odin3_status st = odin3_ast_push(ctx->ast, range);
    st = st == ODIN3_OK ? odin3_ast_push(ctx->ast, decl) : st;
    ctx->group.at.end = odin3_ast_end(ctx->ast, decl);
    return st;
}

odin3_status vp_group_join(odin3_vp_ctx *ctx, uint32_t list, odin3_ast_id decl, uint32_t *out) {
    *out = list;
    ctx->group.at.end = odin3_ast_end(ctx->ast, decl);
    return odin3_ast_push(ctx->ast, decl);
}

/* --- ports --------------------------------------------------------------------------------- */

/* PORT_REF of spec with its E? slot. */
static odin3_status port_ref(odin3_vp_ctx *ctx, const odin3_ast_spec *spec, odin3_ast_id expr,
                             odin3_ast_id *out) {
    return make_n(ctx, spec, &expr, 1, out);
}

odin3_status vp_port_empty(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_PORT_REF, 0), at);
    return port_ref(ctx, &spec, (odin3_ast_id){0}, out);
}

odin3_status vp_port_name(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_PORT_REF, 0), at);
    spec.name = name;
    return port_ref(ctx, &spec, (odin3_ast_id){0}, out);
}

odin3_status vp_port_expr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id expr,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_PORT_REF, 0), at);
    return port_ref(ctx, &spec, expr, out);
}

odin3_status vp_port_dotted(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                            odin3_ast_id *out) {
    return vp_port_dotted_expr(ctx, at, name, (odin3_ast_id){0}, out);
}

odin3_status vp_port_dotted_expr(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                                 odin3_ast_id expr, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_PORT_REF, 0), at);
    spec.name = name;
    spec.flags = ODIN3_AST_F_PORT_REF_DOTTED;
    return port_ref(ctx, &spec, expr, out);
}

odin3_status vp_port_net(odin3_vp_ctx *ctx, uint32_t net_spec, uint32_t *out) {
    (void)ctx;
    uint32_t kind = (net_spec >> ODIN3_VP_SPEC_SUB_SHIFT) & ODIN3_VP_SPEC_BYTE;
    *out = VP_FLAGS((1U + kind) << ODIN3_AST_F_PORT_DECL_NET_SHIFT); /* 0 = none written */
    return ODIN3_OK;
}

/* --- declarators, ranges, defparam ------------------------------------------------------- */

odin3_status vp_decl_name(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    return vp_decl_init(ctx, at, name, (odin3_ast_id){0}, out);
}

odin3_status vp_decl_init(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id init, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_DECLARATOR, 0), at);
    spec.name = name;
    return make_n(ctx, &spec, &init, 1, out);
}

odin3_status vp_decl_dims(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t name,
                          odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_DECLARATOR, 0), at);
    spec.name = name;
    return odin3_ast_make_marked(ctx->ast, &spec, mark, out);
}

odin3_status vp_dims_first(odin3_vp_ctx *ctx, odin3_ast_id range, uint32_t *out) {
    return vp_list_two(ctx, (odin3_ast_id){0}, range, out);
}

odin3_status vp_range(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id msb, odin3_ast_id lsb,
                      odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_RANGE, 0), at);
    const odin3_ast_id kids[] = {msb, lsb};
    return make_n(ctx, &spec, kids, sizeof kids / sizeof kids[0], out);
}

odin3_status vp_defparam(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                         odin3_ast_id value, odin3_ast_id *out) {
    odin3_ast_id target = {0};
    odin3_status st = vp_name(ctx, mark, true, &target);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_DEFPARAM, 0), at);
    const odin3_ast_id kids[] = {target, value};
    return make_n(ctx, &spec, kids, sizeof kids / sizeof kids[0], out);
}

/* --- strengths and delays ------------------------------------------------------------------ */

odin3_status vp_drive0(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out) {
    (void)ctx;
    *out = VP_SPEC(ODIN3_AST_STRENGTH, ODIN3_AST_STRENGTH_DRIVE) |
           VP_FLAGS(value << ODIN3_AST_F_STRENGTH_S0_SHIFT);
    return ODIN3_OK;
}

odin3_status vp_drive1(odin3_vp_ctx *ctx, uint32_t value, uint32_t *out) {
    (void)ctx;
    *out = VP_SPEC(ODIN3_AST_STRENGTH, ODIN3_AST_STRENGTH_DRIVE) |
           VP_FLAGS(value << ODIN3_AST_F_STRENGTH_S1_SHIFT);
    return ODIN3_OK;
}

odin3_status vp_charge(odin3_vp_ctx *ctx, const odin3_vp_loc *at, uint32_t value,
                       odin3_ast_id *out) {
    return vp_leaf(ctx, at,
                   VP_SPEC(ODIN3_AST_STRENGTH, ODIN3_AST_STRENGTH_CHARGE) |
                       VP_FLAGS(value << ODIN3_AST_F_STRENGTH_S0_SHIFT),
                   out);
}

odin3_status vp_delay1(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id value,
                       odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_DELAY, 0), at);
    return make_n(ctx, &spec, &value, 1, out);
}

odin3_status vp_delay_marked(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                             odin3_ast_id *out) {
    return vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_DELAY, 0), out);
}

odin3_status vp_delay_last(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                           odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_DELAY, 0), out) : st;
}

odin3_status vp_mintypmax(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at,
                          odin3_ast_id last, odin3_ast_id *out) {
    odin3_status st = odin3_ast_push(ctx->ast, last);
    return st == ODIN3_OK ? vp_marked(ctx, mark, at, VP_SPEC(ODIN3_AST_MINTYPMAX, 0), out) : st;
}

odin3_status vp_net_assign(odin3_vp_ctx *ctx, const odin3_vp_loc *at, odin3_ast_id lhs,
                           odin3_ast_id rhs, odin3_ast_id *out) {
    odin3_ast_spec spec = vp_spec_at(VP_SPEC(ODIN3_AST_NET_ASSIGN, 0), at);
    const odin3_ast_id kids[] = {lhs, rhs};
    return make_n(ctx, &spec, kids, sizeof kids / sizeof kids[0], out);
}
