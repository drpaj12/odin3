/*
 * pg_select.c — the case method for a target that is a select with a non-constant index (2D
 * Task 22; EL-32 writes): `y[s] = d`, `y[b +: w] <= d`. Yosys 0.55 offers it as `nowrshmsk` beside
 * its default mask-and-shift method, which needs a `$shift` cell the registry has none of (EL-31);
 * the two are equivalent on every position the select can reach (spec 17 #4).
 *
 * The index is generated once through rv and assigned to a position temporary p in the root case;
 * a switch on p has one case per position (elab2_varsel.c), each assigning that constant slice of
 * the value, and no default for `<=`. For `=` the switch is the switch of EL-36: the bits any
 * position writes get case temporaries, so it also has the added default case that holds them.
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum { SEL_SIGN_BITS = 63 };

/* The index, generated through rv at its self-determined width. */
static odin3_status select_index(odin3_pg *pg, odin3_sig *out) {
    const odin3_elab2_varsel *vs = &pg->lvs.var;
    odin3_elab2_reads reads = {pg_read_bit, pg};
    odin3_elab2_expr_req req = {vs->index, {0, false, false}, NULL, &reads};
    odin3_status st = odin3_elab2_expr(pg->e2, &req, out);
    return st != ODIN3_OK ? st
                          : odin3_elab2_extend(pg->e2, out, vs->index_width, out->is_signed, out);
}

/* The position temporary p, assigned the index in the root case; *position is its signal. */
static odin3_status select_position(odin3_pg *pg, odin3_ast_id stmt, odin3_sig *position) {
    odin3_sig index;
    odin3_net_id first = {0};
    odin3_status st = select_index(pg, &index);
    st = st != ODIN3_OK ? st : odin3_pg_temp(pg, index.width, stmt, &first);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(pg->e2, index.width, position);
    for (uint32_t i = 0; st == ODIN3_OK && i < index.width; i++) {
        position->bits[i] = first.v + i;
    }
    pg_act_req act = {PG_ROOT, 0, position->bits, index.bits, index.width, pg->prov};
    return st != ODIN3_OK ? st : pg_act_add(pg, &act);
}

/* The compare of one position: its base value as the index's width of two's complement bits. */
static odin3_status select_compare(odin3_pg *pg, int64_t value, odin3_sig *out) {
    uint32_t width = pg->lvs.var.index_width;
    odin3_status st = odin3_elab2_sig_new(pg->e2, width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        bool one = i < SEL_SIGN_BITS ? ((value >> i) & 1) != 0 : value < 0;
        out->bits[i] = odin3_sb_of_bit(one ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return st;
}

/* The bits some position writes, sorted and unique (the targets of a `=` switch), in pg->blk. */
static odin3_status select_targets(odin3_pg *pg) {
    odin3_vec_clear(&pg->blk);
    for (size_t i = 0; i < pg->lvs.nets.len; i++) {
        uint32_t *slot = odin3_vec_push(&pg->blk);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = ((const odin3_net_id *)odin3_vec_cat(&pg->lvs.nets, i))->v;
    }
    if (pg->blk.len > 0) {
        qsort(pg->blk.data, pg->blk.len, sizeof(uint32_t), pg_u32_compare);
    }
    return ODIN3_OK;
}

/* The slice of the value at one position, assigned to the wire's bits there. */
static odin3_status select_arm(odin3_pg *pg, const odin3_pg_assign_req *req,
                               const odin3_elab2_varcand *cand) {
    const odin3_elab2_varsel *vs = &pg->lvs.var;
    odin3_vec_clear(&pg->slv.nets);
    odin3_vec_clear(&pg->slv.bits);
    for (uint32_t j = 0; j < cand->count; j++) {
        odin3_net_id *slot = odin3_vec_push(&pg->slv.nets);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_wire_net(pg->e2->mod, vs->wire, cand->pos + j);
    }
    /* a slice that straddles an end of the object writes the bits of the value that fall inside */
    odin3_sig part = {req->rhs->bits + cand->first, cand->count, req->rhs->is_signed};
    pg_asg asg = {&pg->slv, &part, req->stmt, req->blocking, true};
    return pg_apply(pg, &asg);
}

/* One case per position, then (for `=`) the default that holds the case temporaries. */
static odin3_status select_cases(odin3_pg *pg, const odin3_pg_assign_req *req) {
    odin3_status st = ODIN3_OK;
    size_t count = pg->lvs.var.cands.len;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        odin3_elab2_varcand cand =
            *(const odin3_elab2_varcand *)odin3_vec_cat(&pg->lvs.var.cands, i);
        odin3_sig cmp;
        st = select_compare(pg, cand.value, &cmp);
        odin3_pg_case_req item = {req->stmt, &cmp, 1};
        st = st != ODIN3_OK ? st : pg_sw_case(pg, &item, false);
        st = st != ODIN3_OK ? st : select_arm(pg, req, &cand);
        if (st == ODIN3_OK) {
            pg_sw_case_end(pg);
        }
    }
    if (st == ODIN3_OK && req->blocking) {
        odin3_pg_case_req held = {req->stmt, NULL, 0};
        st = pg_sw_case(pg, &held, false);
        if (st == ODIN3_OK) {
            pg_sw_case_end(pg);
        }
    }
    return st;
}

odin3_status pg_select_apply(odin3_pg *pg, const odin3_pg_assign_req *req) {
    if (req->rhs->width != pg->lvs.var.width) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_select_write: the value is %u bits; the select %u",
                  req->rhs->width, pg->lvs.var.width);
        return ODIN3_ERR_INVALID_ARG;
    }
    pg_asg mixed = {&pg->lvs, req->rhs, req->stmt, req->blocking, false};
    odin3_status st = pg_check_mixed(pg, &mixed);
    if (st != ODIN3_OK || pg->lvs.var.cands.len == 0) { /* no position: nothing is written */
        return st;
    }
    odin3_sig position;
    st = select_position(pg, req->stmt, &position);
    st = st != ODIN3_OK ? st : select_targets(pg);
    pg_sw_req sw = {req->stmt, &position, pg->blk.data, 0, 0, 0, false, PG_WILD_NONE};
    sw.n_nets = req->blocking ? (uint32_t)pg->blk.len : 0U;
    st = st != ODIN3_OK ? st : pg_sw_begin(pg, &sw);
    st = st != ODIN3_OK ? st : select_cases(pg, req);
    return st != ODIN3_OK ? st : pg_sw_end(pg);
}
