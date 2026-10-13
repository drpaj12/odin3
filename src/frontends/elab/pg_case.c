/*
 * pg_case.c — `if`, `case`, `casez` and `casex` in the process generator (2D Task 22; EL-36),
 * following Yosys 0.55's ProcessGenerator::processCase:
 *
 *  - an `if` is a switch on `$reduce_bool(c)` (c itself when 1 bit) with the case `1'1` and, with
 *    an `else`, a default; a `case` is a switch on the selector with the item expressions as the
 *    compares, all generated at the widest width of the selector and the items (signed only if all
 *    are) through rv (C8: an item may be an expression);
 *  - the bits the branches assign by `=` get case temporaries t1 (one group per chunk, v4.2): each
 *    case starts with `t1 = rv(bits)` and runs with lv(bits) = t1, lv and rv saved and restored
 *    around it; after the switch rv(bits) = t1 and the current case gets `lv(bits) = t1`. The
 *    added default case (no default item) holds `t1 = rv(bits)`, or x under `full_case`;
 *  - the default item is the last case wherever it was written (C6), `casez`/`casex` turn the
 *    z/? (x too under casex) bits of an item into ANY, and the `full_case`/`parallel_case`
 *    attributes and metacomments are the switch flags, each with D1's warning;
 *  - an `if` or `case` whose condition or selector and items are constant by the AST folds to the
 *    branch it takes (pg_scan.c asks pg_fold first, so the dead branches write nothing).
 *
 * Statements nest through SWITCH frames of pg_always.c's frame stack, never recursion: a step
 * closes the open case, then opens the next item (its compares generated, its body pushed as a
 * frame) or, after the items, the default and the exit. The switch primitives below
 * (pg_sw_begin ... pg_sw_end) are shared with the select write (pg_select.c).
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { SW_ARM_OPEN = 1, SW_DEFAULT_DONE = 2, SW_SHIFT = 32 };

/* --- generating the selector and the items --------------------------------------------------- */

/* expr at the context (through rv), extended to the context width (not when it is self-determined,
 * width 0). */
static odin3_status gen_at(odin3_pg *pg, odin3_ast_id expr, odin3_etype ctx, odin3_sig *out) {
    odin3_elab2_reads reads = {pg_read_bit, pg};
    odin3_elab2_expr_req req = {expr, ctx, NULL, &reads};
    odin3_status st = odin3_elab2_expr(pg->e2, &req, out);
    if (st == ODIN3_OK && ctx.width > 0) {
        st = odin3_elab2_extend(pg->e2, out, ctx.width, out->is_signed, out);
    }
    return st;
}

/* Folds the self-determined type of expr into the case type *acc: the widest, signed only if all
 * are. */
static odin3_status type_fold(odin3_pg *pg, odin3_ast_id expr, odin3_etype *acc) {
    odin3_etype self;
    odin3_status st = odin3_elab2_self_type(pg->e2, expr, &self);
    if (st == ODIN3_OK) {
        acc->width = self.width > acc->width ? self.width : acc->width;
        acc->is_signed = acc->is_signed && self.is_signed;
    }
    return st;
}

/* The case width and sign (EL-13, EL-36): the widest of the selector and every item expression,
 * signed only if every one is. At least 1 bit wide. */
static odin3_status case_type(odin3_pg *pg, odin3_ast_id node, odin3_etype *out) {
    const odin3_ast *ast = pg->e2->elab;
    *out = (odin3_etype){0, true, false};
    odin3_status st = type_fold(pg, odin3_ast_child(ast, node, 0), out);
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_ast_nchild(ast, node); i++) {
        odin3_ast_id item = odin3_ast_child(ast, node, i);
        for (uint32_t j = 1; st == ODIN3_OK && j < odin3_ast_nchild(ast, item); j++) {
            st = type_fold(pg, odin3_ast_child(ast, item, j), out);
        }
    }
    out->width = out->width > 0 ? out->width : 1U;
    return st;
}

static bool is_default_item(const odin3_pg *pg, odin3_ast_id item) {
    return (odin3_ast_flags(pg->e2->elab, item) & ODIN3_AST_F_CASE_ITEM_DEFAULT) != 0;
}

/* The wildcard rule on one bit of an item (EL-36): casez makes z (and ?) ANY, casex x and z too. */
static odin3_sigbit wild_bit(uint8_t wild, odin3_sigbit bit) {
    bool is_z = bit == odin3_sb_of_bit(ODIN3_BIT_Z);
    bool is_x = bit == odin3_sb_of_bit(ODIN3_BIT_X);
    return (wild != PG_WILD_NONE && is_z) || (wild == PG_WILD_X && is_x) ? ODIN3_SB_ANY : bit;
}

/* The compare of one item expression: generated at the case type, the wildcards applied (on a
 * copy: the generated bits may be shared). */
static odin3_status gen_compare(odin3_pg *pg, odin3_ast_id expr, const pg_swctx *ctx,
                                odin3_sig *out) {
    odin3_sig sig;
    odin3_status st = gen_at(pg, expr, (odin3_etype){ctx->width, ctx->is_signed, false}, &sig);
    if (st != ODIN3_OK || ctx->wild == PG_WILD_NONE) {
        *out = sig;
        return st;
    }
    st = odin3_elab2_sig_new(pg->e2, sig.width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < sig.width; i++) {
        out->bits[i] = wild_bit(ctx->wild, sig.bits[i]);
    }
    out->is_signed = sig.is_signed;
    return st;
}

/* --- folding --------------------------------------------------------------------------------- */

/* Whether every bit is a known constant 0 or 1. */
static bool sig_known(const odin3_sig *sig) {
    for (uint32_t i = 0; i < sig->width; i++) {
        if (!odin3_sb_is_const(sig->bits[i]) || odin3_sb_bit(sig->bits[i]) > ODIN3_BIT_1) {
            return false;
        }
    }
    return true;
}

static bool sig_has_one(const odin3_sig *sig) {
    for (uint32_t i = 0; i < sig->width; i++) {
        if (sig->bits[i] == odin3_sb_of_bit(ODIN3_BIT_1)) {
            return true;
        }
    }
    return false;
}

static odin3_status fold_set(odin3_pg *pg, odin3_ast_id node, uint32_t pick) {
    return odin3_u64map_put(pg->folds, (odin3_kv){node.v, pick});
}

/* `if (c)` with a constant c of known bits: the then branch for a 1 bit, else the else branch. */
static odin3_status fold_if(odin3_pg *pg, odin3_ast_id node, uint32_t *pick) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id expr = odin3_ast_child(ast, node, 0);
    bool constant = false;
    odin3_sig cond;
    odin3_status st = odin3_elab2_expr_is_const(pg->e2, expr, &constant);
    if (st != ODIN3_OK || !constant) {
        return st;
    }
    st = gen_at(pg, expr, (odin3_etype){0, false, false}, &cond);
    if (st != ODIN3_OK || !sig_known(&cond)) {
        return st;
    }
    bool has_else = odin3_ast_child(ast, node, 2).v != 0;
    *pick = has_else ? 3U : PG_FOLD_NONE;
    *pick = sig_has_one(&cond) ? 2U : *pick;
    return fold_set(pg, node, *pick);
}

/* Whether the selector and every item expression of the case are constant by the AST. */
static odin3_status case_all_const(odin3_pg *pg, odin3_ast_id node, bool *out) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_status st = odin3_elab2_expr_is_const(pg->e2, odin3_ast_child(ast, node, 0), out);
    for (uint32_t i = 1; st == ODIN3_OK && *out && i < odin3_ast_nchild(ast, node); i++) {
        odin3_ast_id item = odin3_ast_child(ast, node, i);
        for (uint32_t j = 1; st == ODIN3_OK && *out && j < odin3_ast_nchild(ast, item); j++) {
            st = odin3_elab2_expr_is_const(pg->e2, odin3_ast_child(ast, item, j), out);
        }
    }
    return st;
}

/* Whether the compare matches the selector (equal on the bits that are not ANY); *unknown is set
 * for an x or z bit that is not a wildcard: such a case is left to the switch. */
static bool compare_matches(const odin3_sig *sel, const odin3_sig *cmp, bool *unknown) {
    bool match = true;
    for (uint32_t i = 0; i < cmp->width; i++) {
        if (cmp->bits[i] == ODIN3_SB_ANY) {
            continue;
        }
        *unknown |= !odin3_sb_is_const(cmp->bits[i]) || odin3_sb_bit(cmp->bits[i]) > ODIN3_BIT_1;
        match = match && cmp->bits[i] == sel->bits[i];
    }
    return match;
}

/* The case being folded: the type, the selector and where the scan has got to. */
typedef struct fold_run {
    pg_swctx ctx;
    odin3_sig sel;
    uint32_t match, dflt; /* item child indices (0: none) */
    bool unknown;
} fold_run;

/* Scans one item: a default is remembered, the first item with a matching compare is the match. */
static odin3_status fold_item(odin3_pg *pg, fold_run *run, uint32_t index, odin3_ast_id item) {
    const odin3_ast *ast = pg->e2->elab;
    if (is_default_item(pg, item)) {
        run->dflt = index;
        return ODIN3_OK;
    }
    for (uint32_t j = 1; j < odin3_ast_nchild(ast, item); j++) {
        odin3_sig cmp;
        odin3_status st = gen_compare(pg, odin3_ast_child(ast, item, j), &run->ctx, &cmp);
        if (st != ODIN3_OK) {
            return st;
        }
        if (compare_matches(&run->sel, &cmp, &run->unknown) && run->match == 0) {
            run->match = index;
        }
    }
    return ODIN3_OK;
}

static uint8_t case_wild(const odin3_pg *pg, odin3_ast_id node) {
    uint32_t sub = odin3_ast_sub(pg->e2->elab, node);
    if (sub == ODIN3_AST_CASE_CASEZ) {
        return PG_WILD_Z;
    }
    return sub == ODIN3_AST_CASE_CASEX ? PG_WILD_X : PG_WILD_NONE;
}

/* The selector and the items of a case whose expressions are all constant. */
static odin3_status fold_case_scan(odin3_pg *pg, odin3_ast_id node, fold_run *run) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_etype type;
    odin3_status st = case_type(pg, node, &type);
    run->ctx.width = type.width;
    run->ctx.is_signed = type.is_signed;
    st = st != ODIN3_OK ? st
                        : gen_at(pg, odin3_ast_child(ast, node, 0),
                                 (odin3_etype){type.width, type.is_signed, false}, &run->sel);
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_ast_nchild(ast, node); i++) {
        st = fold_item(pg, run, i, odin3_ast_child(ast, node, i));
    }
    return st;
}

/* A `case` with a constant selector and constant items, all known: the first matching item, else
 * the default. An x or z bit that is no wildcard anywhere leaves the switch to `proc` (C9). */
static odin3_status fold_case(odin3_pg *pg, odin3_ast_id node, uint32_t *pick) {
    bool constant = false;
    odin3_status st = case_all_const(pg, node, &constant);
    if (st != ODIN3_OK || !constant) {
        return st;
    }
    fold_run run = {
        {node, 0, 0, 0, 0, false, false, case_wild(pg, node)}, {NULL, 0, false}, 0, 0, false};
    st = fold_case_scan(pg, node, &run);
    if (st != ODIN3_OK || run.unknown || !sig_known(&run.sel)) {
        return st;
    }
    uint32_t chosen = run.match != 0 ? run.match : run.dflt;
    *pick = chosen != 0 ? chosen + 1U : PG_FOLD_NONE;
    return fold_set(pg, node, *pick);
}

odin3_status pg_fold(odin3_pg *pg, odin3_ast_id node, uint32_t *pick) {
    *pick = 0;
    return odin3_ast_kind_of(pg->e2->elab, node) == ODIN3_AST_IF ? fold_if(pg, node, pick)
                                                                 : fold_case(pg, node, pick);
}

odin3_ast_id pg_fold_body(const odin3_pg *pg, odin3_ast_id node, uint32_t pick) {
    const odin3_ast *ast = pg->e2->elab;
    if (pick == 0 || pick == PG_FOLD_NONE) {
        return (odin3_ast_id){0};
    }
    odin3_ast_id child = odin3_ast_child(ast, node, pick - 1U);
    return odin3_ast_kind_of(ast, node) == ODIN3_AST_IF ? child : odin3_ast_child(ast, child, 0);
}

/* --- the switch primitives (shared with the select write) ------------------------------------- */

/* The process output a target net is (its wire bit says which chunk it belongs to), or NULL. */
static const pg_out *target_out(const odin3_pg *pg, odin3_sigbit net) {
    uint64_t idx = 0;
    return odin3_u64map_get(pg->out_of, net, &idx) ? odin3_vec_cat(&pg->outs, idx - 1U) : NULL;
}

/* The end of the chunk that starts at target `first`: the targets after it that are the next bits
 * of the same variable. */
static uint32_t chunk_end(const odin3_pg *pg, const pg_sw_req *req, uint32_t first) {
    const pg_out *prev = target_out(pg, req->nets[first]);
    uint32_t next = first + 1U;
    while (prev != NULL && next < req->n_nets) {
        const pg_out *out = target_out(pg, req->nets[next]);
        if (out == NULL || !pg_out_continues(prev, out)) {
            break;
        }
        prev = out;
        next++;
    }
    return next;
}

/* The case temporaries of the targets: one group per chunk of consecutive bits of one variable. */
static odin3_status sw_make_t1(odin3_pg *pg, const pg_sw_req *req) {
    uint32_t from = 0;
    while (from < req->n_nets) {
        uint32_t to = chunk_end(pg, req, from);
        odin3_net_id first = {0};
        odin3_status st = odin3_pg_temp(pg, to - from, req->stmt, &first);
        for (uint32_t k = 0; st == ODIN3_OK && k < to - from; k++) {
            odin3_sigbit *slot = odin3_vec_push(&pg->sw_t1);
            st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
            if (slot != NULL) {
                *slot = first.v + k;
            }
        }
        if (st != ODIN3_OK) {
            return st;
        }
        from = to;
    }
    return ODIN3_OK;
}

/* The target nets and their values at the switch's start (rv, else the net itself). */
static odin3_status sw_push_targets(odin3_pg *pg, const pg_sw_req *req) {
    for (uint32_t i = 0; i < req->n_nets; i++) {
        odin3_sigbit *net = odin3_vec_push(&pg->sw_nets);
        odin3_sigbit *value = net != NULL ? odin3_vec_push(&pg->sw_rv) : NULL;
        if (value == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *net = req->nets[i];
        if (!pg_rv_get(pg, (odin3_net_id){req->nets[i]}, value)) {
            *value = req->nets[i];
        }
    }
    return ODIN3_OK;
}

odin3_status pg_sw_begin(odin3_pg *pg, const pg_sw_req *req) {
    pg_swctx *ctx = odin3_vec_push(&pg->sw_ctx);
    if (ctx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *ctx = (pg_swctx){req->stmt,
                      (uint32_t)pg->sw_nets.len,
                      req->n_nets,
                      req->width,
                      0,
                      req->is_signed,
                      (req->flags & ODIN3_SWITCH_FULL_CASE) != 0,
                      req->wild};
    odin3_status st = sw_push_targets(pg, req);
    st = st != ODIN3_OK ? st : sw_make_t1(pg, req);
    odin3_pg_switch_req sw = {req->stmt, req->signal, req->flags};
    return st != ODIN3_OK ? st : odin3_pg_switch(pg, &sw);
}

static pg_swctx *sw_top(odin3_pg *pg) {
    return odin3_vec_at(&pg->sw_ctx, pg->sw_ctx.len - 1);
}

/* One entry action over the temporaries from .. to (one group): t1 = rv(bits), or x. */
static odin3_status entry_run(odin3_pg *pg, const pg_swctx *ctx, uint32_t from, uint32_t to,
                              bool x_entry) {
    const odin3_sigbit *t1 = (const odin3_sigbit *)pg->sw_t1.data + ctx->first;
    const odin3_sigbit *rv = (const odin3_sigbit *)pg->sw_rv.data + ctx->first;
    odin3_vec_clear(&pg->run_r);
    odin3_status st = odin3_vec_reserve(&pg->run_r, to - from);
    for (uint32_t k = from; st == ODIN3_OK && k < to; k++) {
        odin3_sigbit *slot = odin3_vec_push(&pg->run_r);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = x_entry ? odin3_sb_of_bit(ODIN3_BIT_X) : rv[k];
        }
    }
    pg_act_req req = {pg->cur, 0, t1 + from, pg->run_r.data, to - from, pg->prov};
    return st != ODIN3_OK ? st : pg_act_add(pg, &req);
}

/* The entry actions of the open case: t1 = rv(bits) (x when x_entry), one action per run of
 * consecutive bits of one temporary group. */
static odin3_status sw_entry(odin3_pg *pg, bool x_entry) {
    pg_swctx ctx = *sw_top(pg);
    uint32_t from = 0;
    while (from < ctx.n) {
        const odin3_sigbit *t1 = (const odin3_sigbit *)pg->sw_t1.data + ctx.first;
        uint32_t to = from + 1U;
        while (to < ctx.n &&
               pg_run_continues(pg, (odin3_net_id){t1[to - 1U]}, (odin3_net_id){t1[to]})) {
            to++;
        }
        odin3_status st = entry_run(pg, &ctx, from, to, x_entry);
        if (st != ODIN3_OK) {
            return st;
        }
        from = to;
    }
    return ODIN3_OK;
}

odin3_status pg_sw_case(odin3_pg *pg, const odin3_pg_case_req *req, bool x_entry) {
    pg_swctx *ctx = sw_top(pg);
    ctx->mark = odin3_pg_save(pg);
    odin3_status st = odin3_pg_case(pg, req);
    for (uint32_t i = 0; st == ODIN3_OK && i < sw_top(pg)->n; i++) {
        uint32_t at = sw_top(pg)->first + i;
        st = pg_lv_set(pg, (odin3_net_id){*(const odin3_sigbit *)odin3_vec_cat(&pg->sw_nets, at)},
                       (odin3_net_id){*(const odin3_sigbit *)odin3_vec_cat(&pg->sw_t1, at)});
    }
    return st != ODIN3_OK ? st : sw_entry(pg, x_entry);
}

void pg_sw_case_end(odin3_pg *pg) {
    odin3_pg_case_end(pg);
    odin3_pg_restore(pg, sw_top(pg)->mark);
}

/* After the switch: rv(bits) = t1 and lv(bits) = t1 in the current case, earlier assignments to
 * lv(bits) removed: the assignment of t1 to the targets, T9 already checked. */
odin3_status pg_sw_end(odin3_pg *pg) {
    pg_swctx ctx = *sw_top(pg);
    odin3_status st = ODIN3_OK;
    odin3_vec_clear(&pg->slv.nets);
    odin3_vec_clear(&pg->slv.bits);
    for (uint32_t i = 0; st == ODIN3_OK && i < ctx.n; i++) {
        odin3_net_id *slot = odin3_vec_push(&pg->slv.nets);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            slot->v = *(const odin3_sigbit *)odin3_vec_cat(&pg->sw_nets, ctx.first + i);
        }
    }
    if (st == ODIN3_OK && ctx.n > 0) {
        odin3_sig rhs = {(odin3_sigbit *)pg->sw_t1.data + ctx.first, ctx.n, false};
        pg_asg asg = {&pg->slv, &rhs, ctx.node, true, true};
        st = pg_apply(pg, &asg);
    }
    odin3_vec_pop(&pg->sw_ctx);
    while (pg->sw_nets.len > ctx.first) {
        odin3_vec_pop(&pg->sw_nets);
        odin3_vec_pop(&pg->sw_t1);
        odin3_vec_pop(&pg->sw_rv);
    }
    return st;
}

/* --- statements ------------------------------------------------------------------------------ */

/* The targets of a switch statement the scan recorded. */
static odin3_status stmt_targets(const odin3_pg *pg, odin3_ast_id node, pg_sw_req *req) {
    uint64_t packed = 0;
    if (!odin3_u64map_get(pg->swl, node.v, &packed)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg: an if/case that the scan of the block did not see");
        return ODIN3_ERR_INVALID_ARG;
    }
    req->n_nets = (uint32_t)packed;
    req->nets =
        req->n_nets > 0 ? (const odin3_sigbit *)pg->sw_pool.data + (packed >> SW_SHIFT) : NULL;
    return ODIN3_OK;
}

/* A folded statement runs the branch it takes, if any. */
static odin3_status folded_run(odin3_pg *pg, odin3_ast_id node, uint32_t pick) {
    odin3_ast_id body = pg_fold_body(pg, node, pick);
    pg_frame frame = {PG_F_STMT, body, 0, 0, 0, 0};
    return body.v != 0 ? pg_frame_push(pg, &frame) : ODIN3_OK;
}

/* D1: the attribute is true when it has no value or a nonzero NUMBER. */
static odin3_status attr_true(odin3_pg *pg, odin3_ast_id attr, bool *out) {
    odin3_ast_id value = odin3_ast_child(pg->e2->elab, attr, 0);
    int64_t number = 1;
    bool known = true;
    *out = true;
    if (value.v == 0 || odin3_ast_kind_of(pg->e2->elab, value) != ODIN3_AST_NUMBER) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_elab2_number_i64(pg->e2, value, &number, &known);
    *out = !known || number != 0;
    return st;
}

/* The switch flags of a case from its attributes (`(* full_case *)` and the metacomments), each
 * with D1's warning at the attribute. */
static odin3_status case_flags(odin3_pg *pg, odin3_ast_id node, uint16_t *flags) {
    odin3_ast_span attrs = odin3_ast_attrs(pg->e2->elab, node);
    *flags = 0;
    for (uint32_t i = 0; i < attrs.n; i++) {
        const char *name = odin3_ast_name_str(pg->e2->elab, attrs.ids[i]);
        bool full = strcmp(name, "full_case") == 0;
        bool parallel = strcmp(name, "parallel_case") == 0;
        bool on = false;
        odin3_status st = full || parallel ? attr_true(pg, attrs.ids[i], &on) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_loc loc = odin3_ast_loc(pg->e2->elab, attrs.ids[i]);
        if (on && full) {
            *flags |= ODIN3_SWITCH_FULL_CASE;
            odin3_elab2_warn(pg->e2, loc,
                             "'full_case' is honoured: case values no item lists assign x");
        } else if (on) {
            *flags |= ODIN3_SWITCH_PARALLEL_CASE;
            odin3_elab2_warn(pg->e2, loc,
                             "'parallel_case' is honoured: case items are not prioritised");
        }
    }
    return ODIN3_OK;
}

/* Opens the switch of a statement and pushes its frame: items next..end, the default item dflt. */
static odin3_status switch_frame(odin3_pg *pg, const pg_sw_req *req, const pg_frame *frame) {
    odin3_status st = pg_sw_begin(pg, req);
    return st != ODIN3_OK ? st : pg_frame_push(pg, frame);
}

odin3_status pg_if_open(odin3_pg *pg, odin3_ast_id node) {
    uint64_t fold = 0;
    if (odin3_u64map_get(pg->folds, node.v, &fold)) {
        return folded_run(pg, node, (uint32_t)fold);
    }
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id cond = odin3_ast_child(ast, node, 0);
    odin3_sig sig;
    pg_sw_req req = {node, &sig, NULL, 0, 0, 1, false, PG_WILD_NONE};
    odin3_status st = gen_at(pg, cond, (odin3_etype){0, false, false}, &sig);
    if (st == ODIN3_OK && sig.width != 1) { /* $reduce_bool(c) */
        odin3_elab2_cell_req cell = {ODIN3_E2T_REDUCE_BOOL, cond, &sig, NULL, 1, NULL, NULL};
        st = odin3_elab2_cell(pg->e2, &cell, &sig);
    }
    st = st != ODIN3_OK ? st : stmt_targets(pg, node, &req);
    bool has_else = odin3_ast_child(ast, node, 2).v != 0;
    pg_frame frame = {PG_F_SWITCH, node, 1, 2, has_else ? 2U : 0U, 0};
    return st != ODIN3_OK ? st : switch_frame(pg, &req, &frame);
}

odin3_status pg_case_open(odin3_pg *pg, odin3_ast_id node) {
    uint64_t fold = 0;
    if (odin3_u64map_get(pg->folds, node.v, &fold)) {
        return folded_run(pg, node, (uint32_t)fold);
    }
    const odin3_ast *ast = pg->e2->elab;
    odin3_etype type = {1, false, false};
    odin3_sig sel;
    pg_sw_req req = {node, &sel, NULL, 0, 0, 0, false, case_wild(pg, node)};
    odin3_status st = case_type(pg, node, &type);
    st = st != ODIN3_OK ? st : gen_at(pg, odin3_ast_child(ast, node, 0), type, &sel);
    st = st != ODIN3_OK ? st : case_flags(pg, node, &req.flags);
    st = st != ODIN3_OK ? st : stmt_targets(pg, node, &req);
    req.width = type.width;
    req.is_signed = type.is_signed;
    uint32_t items = odin3_ast_nchild(ast, node);
    uint32_t dflt = 0;
    for (uint32_t i = 1; i < items; i++) {
        dflt = is_default_item(pg, odin3_ast_child(ast, node, i)) ? i : dflt;
    }
    pg_frame frame = {PG_F_SWITCH, node, 1, items, dflt, 0};
    return st != ODIN3_OK ? st : switch_frame(pg, &req, &frame);
}

/* The statement of item idx of the switch frame: an if's branch, a case item's body. */
static odin3_ast_id item_body(const odin3_pg *pg, const pg_frame *frame, uint32_t idx) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id child = odin3_ast_child(ast, frame->node, idx);
    return odin3_ast_kind_of(ast, frame->node) == ODIN3_AST_IF ? child
                                                               : odin3_ast_child(ast, child, 0);
}

/* Adds a compare to the case being opened. */
static odin3_status compare_push(odin3_pg *pg, const odin3_sig *sig) {
    odin3_sig *slot = odin3_vec_push(&pg->cmp_sigs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *sig;
    return ODIN3_OK;
}

/* The compares of item idx: an if's `1'1`, a case item's expressions. */
static odin3_status item_compares(odin3_pg *pg, const pg_frame *frame, uint32_t idx) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_vec_clear(&pg->cmp_sigs);
    if (odin3_ast_kind_of(ast, frame->node) == ODIN3_AST_IF) {
        odin3_sig one;
        odin3_status st = odin3_elab2_sig_new(pg->e2, 1, &one);
        if (st == ODIN3_OK) {
            one.bits[0] = odin3_sb_of_bit(ODIN3_BIT_1);
            st = compare_push(pg, &one);
        }
        return st;
    }
    odin3_ast_id item = odin3_ast_child(ast, frame->node, idx);
    for (uint32_t j = 1; j < odin3_ast_nchild(ast, item); j++) {
        odin3_sig cmp;
        odin3_status st = gen_compare(pg, odin3_ast_child(ast, item, j), sw_top(pg), &cmp);
        st = st != ODIN3_OK ? st : compare_push(pg, &cmp);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Which item of which frame. */
typedef struct item_ref {
    uint32_t frame, idx;
} item_ref;

/* Opens the case of item idx (compares, entry actions) and pushes its body. */
static odin3_status open_item(odin3_pg *pg, const item_ref *ref) {
    uint32_t frame_at = ref->frame;
    uint32_t idx = ref->idx;
    pg_frame frame = *(const pg_frame *)odin3_vec_cat(&pg->frames, frame_at);
    odin3_status st = item_compares(pg, &frame, idx);
    odin3_ast_id stmt = odin3_ast_kind_of(pg->e2->elab, frame.node) == ODIN3_AST_IF
                            ? frame.node
                            : odin3_ast_child(pg->e2->elab, frame.node, idx);
    odin3_pg_case_req req = {stmt, pg->cmp_sigs.data, (uint32_t)pg->cmp_sigs.len};
    st = st != ODIN3_OK ? st : pg_sw_case(pg, &req, false);
    if (st != ODIN3_OK) {
        return st;
    }
    ((pg_frame *)odin3_vec_at(&pg->frames, frame_at))->aux |= SW_ARM_OPEN;
    odin3_ast_id body = item_body(pg, &frame, idx);
    pg_frame body_frame = {PG_F_STMT, body, 0, 0, 0, 0};
    return body.v != 0 ? pg_frame_push(pg, &body_frame) : ODIN3_OK;
}

/* The default case: the default item's, or the added one holding the targets (x under full_case)
 * when the branches assign by `=`. */
static odin3_status open_default(odin3_pg *pg, uint32_t frame_at) {
    pg_frame frame = *(const pg_frame *)odin3_vec_cat(&pg->frames, frame_at);
    const pg_swctx *ctx = sw_top(pg);
    if (frame.dflt == 0 && ctx->n == 0) {
        return ODIN3_OK;
    }
    odin3_ast_id stmt =
        frame.dflt != 0 && odin3_ast_kind_of(pg->e2->elab, frame.node) == ODIN3_AST_CASE
            ? odin3_ast_child(pg->e2->elab, frame.node, frame.dflt)
            : frame.node;
    odin3_pg_case_req req = {stmt, NULL, 0};
    odin3_status st = pg_sw_case(pg, &req, frame.dflt == 0 && ctx->full_case);
    if (st != ODIN3_OK) {
        return st;
    }
    if (frame.dflt == 0) { /* the added default has no body */
        pg_sw_case_end(pg);
        return ODIN3_OK;
    }
    ((pg_frame *)odin3_vec_at(&pg->frames, frame_at))->aux |= SW_ARM_OPEN;
    odin3_ast_id body = item_body(pg, &frame, frame.dflt);
    pg_frame body_frame = {PG_F_STMT, body, 0, 0, 0, 0};
    return body.v != 0 ? pg_frame_push(pg, &body_frame) : ODIN3_OK;
}

odin3_status pg_switch_step(odin3_pg *pg) {
    uint32_t at = (uint32_t)pg->frames.len - 1U;
    pg_frame *frame = odin3_vec_at(&pg->frames, at);
    if ((frame->aux & SW_ARM_OPEN) != 0) {
        pg_sw_case_end(pg);
        frame->aux &= ~(uint32_t)SW_ARM_OPEN;
    }
    while (frame->next < frame->end) {
        uint32_t idx = frame->next++;
        bool is_if = odin3_ast_kind_of(pg->e2->elab, frame->node) == ODIN3_AST_IF;
        if (is_if || !is_default_item(pg, odin3_ast_child(pg->e2->elab, frame->node, idx))) {
            return open_item(pg, &(item_ref){at, idx});
        }
    }
    if ((frame->aux & SW_DEFAULT_DONE) == 0) {
        frame->aux |= SW_DEFAULT_DONE;
        return open_default(pg, at);
    }
    odin3_vec_pop(&pg->frames);
    return pg_sw_end(pg);
}

/* --- the public statements ----------------------------------------------------------------- */

static odin3_status run_kind(odin3_pg *pg, odin3_ast_id stmt, odin3_ast_kind kind) {
    if (stmt.v == 0 || odin3_ast_kind_of(pg->e2->elab, stmt) != kind) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg: the statement is not an %s",
                  odin3_ast_kind_name(kind));
        return ODIN3_ERR_INVALID_ARG;
    }
    return pg_run(pg, stmt);
}

odin3_status odin3_pg_if(odin3_pg *pg, odin3_ast_id stmt) {
    return run_kind(pg, stmt, ODIN3_AST_IF);
}

odin3_status odin3_pg_case_stmt(odin3_pg *pg, odin3_ast_id stmt) {
    return run_kind(pg, stmt, ODIN3_AST_CASE);
}
