/*
 * dff_rules.c — opt_dff rules 2-9 (2F spec OP-9 §9.3): Yosys 0.55 OptDffWorker::run's SET/CLR,
 * ALOAD, ARST, SRST, EN, constant CLK, D = Q and AD = Q rules, one function each over the loaded FF
 * model.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/dff.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- rule 2: SET and CLR ---------------------------------------------------------------------- */

/* One bit of rule 2's first part: tied (always-active CLR or SET: *removed) or kept. */
static odin3_status sr_bit(odin3_dff_state *ds, uint32_t bit, odin3_vec *keep, bool *removed) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_net_id clr = odin3_ff_net_at(&ff->sig_clr, bit);
    odin3_net_id set = odin3_ff_net_at(&ff->sig_set, bit);
    odin3_net_id src = clr;
    odin3_status st = ODIN3_OK;
    if (odin3_dff_always_active(ds, clr, ff->pol_clr)) {
        st = odin3_dff_const_net(ds, ODIN3_BIT_0, &src);
        odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_CLR, "");
    } else if (odin3_dff_always_active(ds, set, ff->pol_set)) {
        st = ff->pol_clr ? odin3_dff_not_net(ds, clr, &src) : ODIN3_OK;
        odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_SET, "");
    } else {
        return odin3_kit_push_u32(keep, bit);
    }
    *removed = true;
    return st == ODIN3_OK ? odin3_dff_tie_bit(ds, bit, src) : st;
}

/* The bits of the model that stay (keep) and those tied (pend), for always-active SET/CLR. */
static odin3_status sr_always_bits(odin3_dff_state *ds, odin3_vec *keep, bool *removed) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ds->ff.width; i++) {
        st = sr_bit(ds, i, keep, removed);
    }
    return st;
}

/* Never-active CLR (SET uniform) or SET (CLR uniform): the other side becomes an async reset. */
static odin3_status sr_to_arst(odin3_dff_state *ds, odin3_net_id sig, bool pol, odin3_bit val,
                               odin3_opt_dff_action action) {
    odin3_ffdata *ff = &ds->ff;
    ff->has_sr = false;
    ff->has_arst = true;
    ff->pol_arst = pol;
    ff->sig_arst = sig;
    odin3_vec_clear(&ff->sig_set);
    odin3_vec_clear(&ff->sig_clr);
    ds->changed = true;
    odin3_dff_note(ds, action, "");
    return odin3_dff_bits_fill(&ff->val_arst, val, ff->width);
}

static bool uniform(const odin3_vec *vec) {
    for (uint32_t i = 1; i < vec->len; i++) {
        if (odin3_ff_net_at(vec, i).v != odin3_ff_net_at(vec, 0).v) {
            return false;
        }
    }
    return true;
}

/* CLR and SET of one polarity on complementary bits of one signal: an async reset with the value
 * each bit selects (vals is filled; *ok = false when some bit is neither). */
static odin3_status sr_complement_vals(const odin3_dff_state *ds, odin3_net_id arst,
                                       odin3_vec *vals, bool *ok) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_const neutral = ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1;
    odin3_status st = odin3_vec_reserve(vals, ff->width);
    *ok = true;
    for (uint32_t i = 0; st == ODIN3_OK && *ok && i < ff->width; i++) {
        odin3_net_id clr = odin3_ff_net_at(&ff->sig_clr, i);
        odin3_net_id set = odin3_ff_net_at(&ff->sig_set, i);
        if (clr.v == arst.v && odin3_dff_net_is(ds, set, neutral)) {
            st = odin3_dff_bit_push(vals, ODIN3_BIT_0);
        } else if (set.v == arst.v && odin3_dff_net_is(ds, clr, neutral)) {
            st = odin3_dff_bit_push(vals, ODIN3_BIT_1);
        } else {
            *ok = false;
        }
    }
    return st;
}

static odin3_status sr_complement(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_const neutral = ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1;
    odin3_net_id arst = odin3_dff_net_is(ds, odin3_ff_net_at(&ff->sig_clr, 0), neutral)
                            ? odin3_ff_net_at(&ff->sig_set, 0)
                            : odin3_ff_net_at(&ff->sig_clr, 0);
    odin3_vec vals;
    odin3_vec_init(&vals, sizeof(odin3_bit));
    bool ok = false;
    odin3_status st = sr_complement_vals(ds, arst, &vals, &ok);
    if (st == ODIN3_OK && ok) {
        odin3_vec_free(&ff->val_arst);
        ff->val_arst = vals;
        odin3_vec_init(&vals, sizeof(odin3_bit));
        ff->has_sr = false;
        ff->has_arst = true;
        ff->sig_arst = arst;
        ff->pol_arst = ff->pol_clr;
        odin3_vec_clear(&ff->sig_set);
        odin3_vec_clear(&ff->sig_clr);
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_CLR_SET_TO_ARST, "");
    }
    odin3_vec_free(&vals);
    return st;
}

static odin3_status sr_conversions(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (odin3_dff_vec_is(ds, &ff->sig_clr, ff->pol_clr ? ODIN3_CONST_0 : ODIN3_CONST_1)) {
        return uniform(&ff->sig_set) ? sr_to_arst(ds, odin3_ff_net_at(&ff->sig_set, 0), ff->pol_set,
                                                  ODIN3_BIT_1, ODIN3_OPT_DFF_NEVER_CLR)
                                     : ODIN3_OK;
    }
    if (odin3_dff_vec_is(ds, &ff->sig_set, ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1)) {
        return uniform(&ff->sig_clr) ? sr_to_arst(ds, odin3_ff_net_at(&ff->sig_clr, 0), ff->pol_clr,
                                                  ODIN3_BIT_0, ODIN3_OPT_DFF_NEVER_SET)
                                     : ODIN3_OK;
    }
    return ff->pol_clr == ff->pol_set ? sr_complement(ds) : ODIN3_OK;
}

odin3_status odin3_dff_rule_sr(odin3_dff_state *ds) {
    if (!ds->ff.has_sr) {
        return ODIN3_OK;
    }
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(uint32_t));
    bool removed = false;
    odin3_status st = sr_always_bits(ds, &keep, &removed);
    if (st == ODIN3_OK && removed) {
        ds->changed = true;
        st = keep.len == 0 ? odin3_dff_remove_all(ds, NULL)
                           : odin3_dff_state_keep(ds, keep.data, (uint32_t)keep.len);
    }
    odin3_vec_free(&keep);
    return st == ODIN3_OK && !ds->gone ? sr_conversions(ds) : st;
}

/* --- rule 3: ALOAD ---------------------------------------------------------------------------- */

/* Always-active ALOAD on a FF with SET/CLR: q = (AD | SET) & ~CLR, each in its polarity. */
static odin3_status aload_sr_circuit(odin3_dff_state *ds, odin3_vec *qout) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_vec inv;
    odin3_vec both;
    odin3_vec_init(&inv, sizeof(odin3_net_id));
    odin3_vec_init(&both, sizeof(odin3_net_id));
    const odin3_vec *set = &ff->sig_set;
    odin3_status st = ff->pol_set ? ODIN3_OK : odin3_dff_gate_not(ds, set, &inv);
    set = ff->pol_set ? set : &inv;
    st = st == ODIN3_OK ? odin3_dff_gate_bin(ds, "$or", &ff->sig_ad, set, &both) : st;
    const odin3_vec *clr = &ff->sig_clr;
    odin3_vec nclr;
    odin3_vec_init(&nclr, sizeof(odin3_net_id));
    st = st == ODIN3_OK && ff->pol_clr ? odin3_dff_gate_not(ds, clr, &nclr) : st;
    clr = ff->pol_clr ? &nclr : clr;
    st = st == ODIN3_OK ? odin3_dff_gate_bin(ds, "$and", &both, clr, qout) : st;
    odin3_vec_free(&inv);
    odin3_vec_free(&both);
    odin3_vec_free(&nclr);
    return st;
}

/* Always-active ALOAD on a FF with ARST: q = ARST ? val_arst : AD (in the reset's polarity). */
static odin3_status aload_arst_circuit(odin3_dff_state *ds, odin3_vec *qout) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_vec rst;
    odin3_vec_init(&rst, sizeof(odin3_net_id));
    odin3_status st = odin3_dff_set_consts(ds, &rst, &ff->val_arst);
    if (st == ODIN3_OK && ff->pol_arst) {
        st = odin3_dff_gate_mux(ds, &ff->sig_ad, &rst, ff->sig_arst, qout);
    } else if (st == ODIN3_OK) {
        st = odin3_dff_gate_mux(ds, &rst, &ff->sig_ad, ff->sig_arst, qout);
    }
    odin3_vec_free(&rst);
    return st;
}

/* Always-active ALOAD: the combinational circuit of Q (Yosys builds it after ff.remove()); the
 * outputs are made first, the FF goes after, tied to them. */
static odin3_status aload_always(odin3_dff_state *ds) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_ALOAD, "");
    odin3_vec qout;
    odin3_vec_init(&qout, sizeof(odin3_net_id));
    odin3_status st = ODIN3_OK;
    if (ff->has_sr) {
        st = aload_sr_circuit(ds, &qout);
    } else if (ff->has_arst) {
        st = aload_arst_circuit(ds, &qout);
    }
    const odin3_vec *src = ff->has_sr || ff->has_arst ? &qout : &ff->sig_ad;
    st = st == ODIN3_OK ? odin3_dff_remove_all(ds, src->data) : st;
    odin3_vec_free(&qout);
    return st;
}

/* Async load of a constant on a FF with no ARST or SR: the load is an async reset. */
static odin3_status aload_to_arst(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_vec_clear(&ff->val_arst);
    odin3_status st = odin3_vec_reserve(&ff->val_arst, ff->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_dff_bit_push(&ff->val_arst, odin3_dff_const_bit(odin3_dff_net_const(
                                                   ds, odin3_ff_net_at(&ff->sig_ad, i))));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    ff->has_arst = true;
    ff->has_aload = false;
    ff->sig_arst = ff->sig_aload;
    ff->pol_arst = ff->pol_aload;
    ff->sig_aload = (odin3_net_id){0};
    odin3_vec_clear(&ff->sig_ad);
    ds->changed = true;
    odin3_dff_note(ds, ODIN3_OPT_DFF_ALOAD_TO_ARST, "");
    return ODIN3_OK;
}

odin3_status odin3_dff_rule_aload(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_aload) {
        return ODIN3_OK;
    }
    if (odin3_dff_never_active(ds, ff->sig_aload, ff->pol_aload) ||
        (!ds->oc->opts->keepdc && odin3_dff_net_is(ds, ff->sig_aload, ODIN3_CONST_X))) {
        ff->has_aload = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_NEVER_ALOAD, "");
        return ODIN3_OK;
    }
    if (odin3_dff_net_is(ds, ff->sig_aload, ff->pol_aload ? ODIN3_CONST_1 : ODIN3_CONST_0)) {
        return aload_always(ds);
    }
    if (odin3_dff_vec_all_const(ds, &ff->sig_ad) && !ff->has_arst && !ff->has_sr) {
        return aload_to_arst(ds);
    }
    return ODIN3_OK;
}

/* --- rule 4: ARST ----------------------------------------------------------------------------- */

odin3_status odin3_dff_rule_arst(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_arst) {
        return ODIN3_OK;
    }
    if (odin3_dff_never_active(ds, ff->sig_arst, ff->pol_arst)) {
        ff->has_arst = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_NEVER_ARST, "");
        return ODIN3_OK;
    }
    if (!odin3_dff_always_active(ds, ff->sig_arst, ff->pol_arst)) {
        return ODIN3_OK;
    }
    odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_ARST, "");
    odin3_vec src;
    odin3_vec_init(&src, sizeof(odin3_net_id));
    odin3_status st = odin3_dff_set_consts(ds, &src, &ff->val_arst);
    st = st == ODIN3_OK ? odin3_dff_remove_all(ds, src.data) : st;
    odin3_vec_free(&src);
    return st;
}

/* --- rule 5: SRST ----------------------------------------------------------------------------- */

odin3_status odin3_dff_rule_srst(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_srst) {
        return ODIN3_OK;
    }
    if (odin3_dff_never_active(ds, ff->sig_srst, ff->pol_srst)) {
        ff->has_srst = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_NEVER_SRST, "");
        return ODIN3_OK;
    }
    if (!odin3_dff_always_active(ds, ff->sig_srst, ff->pol_srst)) {
        return ODIN3_OK;
    }
    odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_SRST, "");
    ff->has_srst = false;
    ff->has_ce = ff->has_ce && ff->ce_over_srst;
    ds->changed = true;
    return odin3_dff_set_consts(ds, &ff->sig_d, &ff->val_srst);
}

/* --- rule 6: EN ------------------------------------------------------------------------------- */

static odin3_status en_never_active(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    ds->changed = true;
    if (ff->has_srst && !ff->ce_over_srst) { /* the sync reset becomes the enable */
        odin3_dff_note(ds, ODIN3_OPT_DFF_NEVER_EN, "");
        ff->pol_ce = ff->pol_srst;
        ff->sig_ce = ff->sig_srst;
        ff->has_srst = false;
        return odin3_dff_set_consts(ds, &ff->sig_d, &ff->val_srst);
    }
    if (!ds->oc->opts->keepdc || odin3_dff_init_defined(ds)) { /* the D path is useless */
        odin3_dff_note(ds, ODIN3_OPT_DFF_NEVER_EN, "");
        ff->has_ce = ff->has_clk = ff->has_srst = false;
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK; /* keep the undefined init: D = Q, no line */
    odin3_vec_clear(&ff->sig_d);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_net(&ff->sig_d, odin3_ff_net_at(&ff->sig_q, i));
    }
    ff->has_ce = ff->has_srst = false;
    return st;
}

odin3_status odin3_dff_rule_en(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_ce) {
        return ODIN3_OK;
    }
    if (odin3_dff_net_is(ds, ff->sig_ce, ff->pol_ce ? ODIN3_CONST_0 : ODIN3_CONST_1) ||
        (!ds->oc->opts->keepdc && odin3_dff_net_is(ds, ff->sig_ce, ODIN3_CONST_X))) {
        return en_never_active(ds);
    }
    if (odin3_dff_net_is(ds, ff->sig_ce, ff->pol_ce ? ODIN3_CONST_1 : ODIN3_CONST_0)) {
        ff->has_ce = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_ALWAYS_EN, "");
    }
    return ODIN3_OK;
}

/* --- rule 7: constant CLK --------------------------------------------------------------------- */

odin3_status odin3_dff_rule_clk(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_clk || odin3_dff_net_const(ds, ff->sig_clk) == ODIN3_CONST_NONE) {
        return ODIN3_OK;
    }
    if (!ds->oc->opts->keepdc || odin3_dff_init_defined(ds)) { /* the D path is useless */
        ff->has_ce = ff->has_clk = ff->has_srst = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_CONST_CLK, "");
        return ODIN3_OK;
    }
    if (!ff->has_ce && !ff->has_srst && odin3_dff_vec_equal(&ff->sig_d, &ff->sig_q)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK; /* keep the undefined init: D = Q */
    odin3_vec_clear(&ff->sig_d);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_net(&ff->sig_d, odin3_ff_net_at(&ff->sig_q, i));
    }
    ff->has_ce = ff->has_srst = false;
    ds->changed = true;
    return st;
}

/* --- rule 8: D = Q ---------------------------------------------------------------------------- */

/* D = Q on a FF with a sync reset: EN := SRST (anded with an EN that the reset is over), D :=
 * the reset value. */
static odin3_status dq_with_srst(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_status st = ODIN3_OK;
    if (ff->has_ce && ff->ce_over_srst) {
        odin3_net_id ce = ff->sig_ce;
        odin3_net_id srst = ff->sig_srst;
        st = !ff->pol_ce ? odin3_dff_not_net(ds, ce, &ce) : st;
        st = st == ODIN3_OK && !ff->pol_srst ? odin3_dff_not_net(ds, srst, &srst) : st;
        st = st == ODIN3_OK ? odin3_dff_and_nets(ds, ce, srst, &ff->sig_ce) : st;
        ff->pol_ce = true;
    } else {
        ff->pol_ce = ff->pol_srst;
        ff->sig_ce = ff->sig_srst;
    }
    ff->has_ce = true;
    ff->has_srst = false;
    ds->changed = true;
    return st == ODIN3_OK ? odin3_dff_set_consts(ds, &ff->sig_d, &ff->val_srst) : st;
}

odin3_status odin3_dff_rule_dq(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!(ff->has_clk || ff->has_gclk) || !odin3_dff_vec_equal(&ff->sig_d, &ff->sig_q)) {
        return ODIN3_OK;
    }
    if (ff->has_clk && ff->has_srst) {
        odin3_dff_note(ds, ODIN3_OPT_DFF_D_EQ_Q, "");
        return dq_with_srst(ds);
    }
    if (!ds->oc->opts->keepdc || odin3_dff_init_defined(ds)) {
        ff->has_gclk = ff->has_clk = ff->has_ce = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_D_EQ_Q, "");
    }
    return ODIN3_OK;
}

/* --- rule 9: AD = Q --------------------------------------------------------------------------- */

odin3_status odin3_dff_rule_adq(odin3_dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (ff->has_aload && !ff->has_clk && odin3_dff_vec_equal(&ff->sig_ad, &ff->sig_q)) {
        ff->has_aload = false;
        ds->changed = true;
        odin3_dff_note(ds, ODIN3_OPT_DFF_AD_EQ_Q, "");
    }
    return ODIN3_OK;
}
