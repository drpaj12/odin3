/*
 * dff_state.c — opt_dff's per-FF state (2F spec OP-9 §9.3): the log texts of the actions, the
 * loading and editing of the FF model, the constant tests and the tie bookkeeping that every rule
 * shares.
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

/* --- the log texts of the actions (spec §16) -------------------------------------------------- */

static const char *const k_action_text[ODIN3_OPT_DFF_N_ACTIONS] = {
    [ODIN3_OPT_DFF_NEVER_CLR] = "removing never-active CLR",
    [ODIN3_OPT_DFF_NEVER_SET] = "removing never-active SET",
    [ODIN3_OPT_DFF_ALWAYS_CLR] = "handling always-active CLR",
    [ODIN3_OPT_DFF_ALWAYS_SET] = "handling always-active SET",
    [ODIN3_OPT_DFF_CLR_SET_TO_ARST] = "converting CLR/SET to ARST",
    [ODIN3_OPT_DFF_NEVER_ALOAD] = "removing never-active ALOAD",
    [ODIN3_OPT_DFF_ALWAYS_ALOAD] = "handling always-active ALOAD",
    [ODIN3_OPT_DFF_ALOAD_TO_ARST] = "changing const-value ALOAD to ARST",
    [ODIN3_OPT_DFF_NEVER_ARST] = "removing never-active ARST",
    [ODIN3_OPT_DFF_ALWAYS_ARST] = "handling always-active ARST",
    [ODIN3_OPT_DFF_NEVER_SRST] = "removing never-active SRST",
    [ODIN3_OPT_DFF_ALWAYS_SRST] = "handling always-active SRST",
    [ODIN3_OPT_DFF_NEVER_EN] = "removing never-active EN",
    [ODIN3_OPT_DFF_ALWAYS_EN] = "handling always-active EN",
    [ODIN3_OPT_DFF_CONST_CLK] = "handling const clock",
    [ODIN3_OPT_DFF_D_EQ_Q] = "handling D = Q",
    [ODIN3_OPT_DFF_AD_EQ_Q] = "handling AD = Q",
    [ODIN3_OPT_DFF_ADD_SRST] = "adding SRST",
    [ODIN3_OPT_DFF_ADD_EN] = "adding EN",
    [ODIN3_OPT_DFF_CONST_BIT_0] = "setting constant 0",
    [ODIN3_OPT_DFF_CONST_BIT_1] = "setting constant 1",
    [ODIN3_OPT_DFF_CONST_BIT_X] = "setting constant x",
    [ODIN3_OPT_DFF_CONST_BIT_Z] = "setting constant z",
};

void odin3_dff_state_init(odin3_dff_state *ds, odin3_opt_ctx *oc) {
    memset(ds, 0, sizeof *ds);
    ds->oc = oc;
    odin3_ffdata_init(&ds->ff);
    odin3_vec_init(&ds->pend, sizeof(odin3_net_pair));
}

void odin3_dff_state_free(odin3_dff_state *ds) {
    odin3_ffdata_free(&ds->ff);
    odin3_vec_free(&ds->pend);
}

/* What net carries: odin3_net_const_value (CONST_NONE for a wire or an open pin). */
odin3_const odin3_dff_net_const(const odin3_dff_state *ds, odin3_net_id net) {
    return odin3_net_valid(net) ? odin3_net_const_value(ds->oc->module, net) : ODIN3_CONST_NONE;
}

bool odin3_dff_net_is(const odin3_dff_state *ds, odin3_net_id net, odin3_const want) {
    return odin3_dff_net_const(ds, net) == want;
}

/* True when every net of vec carries want. */
bool odin3_dff_vec_is(const odin3_dff_state *ds, const odin3_vec *vec, odin3_const want) {
    for (uint32_t i = 0; i < vec->len; i++) {
        if (!odin3_dff_net_is(ds, odin3_ff_net_at(vec, i), want)) {
            return false;
        }
    }
    return true;
}

bool odin3_dff_vec_all_const(const odin3_dff_state *ds, const odin3_vec *vec) {
    for (uint32_t i = 0; i < vec->len; i++) {
        if (odin3_dff_net_const(ds, odin3_ff_net_at(vec, i)) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

bool odin3_dff_vec_equal(const odin3_vec *lhs, const odin3_vec *rhs) {
    if (lhs->len != rhs->len) {
        return false;
    }
    for (uint32_t i = 0; i < lhs->len; i++) {
        if (odin3_ff_net_at(lhs, i).v != odin3_ff_net_at(rhs, i).v) {
            return false;
        }
    }
    return true;
}

bool odin3_dff_init_defined(const odin3_dff_state *ds) {
    for (uint32_t i = 0; i < ds->ff.width; i++) {
        odin3_bit bit = odin3_ff_bit_at(&ds->ff.val_init, i);
        if (bit != ODIN3_BIT_0 && bit != ODIN3_BIT_1) {
            return false;
        }
    }
    return true;
}

/* A control that is constant `active` (the polarity's active value) or, without -keepdc, x. */
bool odin3_dff_always_active(const odin3_dff_state *ds, odin3_net_id sig, bool pol) {
    return odin3_dff_net_is(ds, sig, pol ? ODIN3_CONST_1 : ODIN3_CONST_0) ||
           (!ds->oc->opts->keepdc && odin3_dff_net_is(ds, sig, ODIN3_CONST_X));
}

bool odin3_dff_never_active(const odin3_dff_state *ds, odin3_net_id sig, bool pol) {
    return odin3_dff_net_is(ds, sig, pol ? ODIN3_CONST_0 : ODIN3_CONST_1);
}

/* --- logging and counting --------------------------------------------------------------------- */

void odin3_dff_note(odin3_dff_state *ds, odin3_opt_dff_action action, const char *detail) {
    ds->oc->counts->dff_action[action]++;
    const char *module =
        odin3_strtab_get(odin3_design_strtab(ds->oc->design), odin3_module_name(ds->oc->module));
    odin3_log(ODIN3_LOG_DEBUG, "opt_dff: module '%s': %s%s on %s '%s'", module,
              k_action_text[action], detail, ds->type, ds->label);
}

/* --- loading ---------------------------------------------------------------------------------- */

/* True when the nets of one bit (Q, and D, AD, SET, CLR where the FF has them) are connected. */
static bool bit_connected(const odin3_ffdata *ff, uint32_t bit) {
    bool ok = odin3_net_valid(odin3_ff_net_at(&ff->sig_q, bit));
    ok =
        ok && (!(ff->has_clk || ff->has_gclk) || odin3_net_valid(odin3_ff_net_at(&ff->sig_d, bit)));
    ok = ok && (!ff->has_aload || odin3_net_valid(odin3_ff_net_at(&ff->sig_ad, bit)));
    return ok && (!ff->has_sr || (odin3_net_valid(odin3_ff_net_at(&ff->sig_set, bit)) &&
                                  odin3_net_valid(odin3_ff_net_at(&ff->sig_clr, bit))));
}

/* True when every net the model reads is connected. */
static bool all_connected(const odin3_ffdata *ff) {
    bool ok = ff->sig_q.len == ff->width && ff->width > 0;
    for (uint32_t i = 0; ok && i < ff->width; i++) {
        ok = bit_connected(ff, i);
    }
    return ok && (!ff->has_clk || odin3_net_valid(ff->sig_clk)) &&
           (!ff->has_ce || odin3_net_valid(ff->sig_ce)) &&
           (!ff->has_srst || odin3_net_valid(ff->sig_srst)) &&
           (!ff->has_arst || odin3_net_valid(ff->sig_arst)) &&
           (!ff->has_aload || odin3_net_valid(ff->sig_aload));
}

/* True when no two Q bits are on one net. */
static odin3_status q_distinct(const odin3_ffdata *ff, bool *distinct) {
    odin3_vec keys;
    odin3_vec_init(&keys, sizeof(uint64_t));
    odin3_status st = odin3_vec_reserve(&keys, ff->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_u64(&keys, odin3_ff_net_at(&ff->sig_q, i).v);
    }
    odin3_kit_sort_unify(&keys, 0);
    *distinct = st == ODIN3_OK && keys.len == ff->width;
    odin3_vec_free(&keys);
    return st;
}

/* Loads node into ds (Yosys `FfData ff(&initvals, cell)`); *usable = false for an FF this pass
 * leaves alone. */
odin3_status odin3_dff_state_load(odin3_dff_state *ds, odin3_node_id node, bool *usable) {
    odin3_ffdata_free(&ds->ff);
    odin3_vec_clear(&ds->pend);
    ds->cell = node;
    ds->changed = ds->gone = ds->prov_ready = false;
    *usable = false;
    odin3_status st = odin3_ff_load(ds->oc, node, &ds->ff);
    if (st != ODIN3_OK) {
        return st;
    }
    ds->width0 = ds->ff.width;
    (void)snprintf(ds->type, sizeof ds->type, "%s",
                   odin3_celltype_get(ds->oc->design, odin3_node_type(ds->oc->module, node))->name);
    odin3_kit_node_label(ds->oc, node, ds->label, sizeof ds->label);
    odin3_kit_begin_rewrite(ds->oc);
    *usable = all_connected(&ds->ff);
    return *usable ? q_distinct(&ds->ff, usable) : ODIN3_OK;
}

/* --- editing the model ------------------------------------------------------------------------ */

/* Yosys `ff = ff.slice(keep); ff.cell = cell`. */
odin3_status odin3_dff_state_keep(odin3_dff_state *ds, const uint32_t *keep, uint32_t n) {
    odin3_ffdata sliced;
    odin3_status st = odin3_ff_slice(&ds->ff, (odin3_ff_bits){keep, n}, &sliced);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ffdata_free(&ds->ff);
    ds->ff = sliced;
    ds->ff.node = ds->cell;
    return ODIN3_OK;
}

/* Yosys `module->connect(sig_q[i], src)`, made when the FF is rebuilt without bit i. */
odin3_status odin3_dff_tie_bit(odin3_dff_state *ds, uint32_t idx, odin3_net_id src) {
    odin3_net_pair *slot = odin3_vec_push(&ds->pend);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_net_pair){src, odin3_ff_net_at(&ds->ff.sig_q, idx)};
    return ODIN3_OK;
}

odin3_status odin3_dff_const_net(odin3_dff_state *ds, odin3_bit bit, odin3_net_id *out) {
    return odin3_opt_const_net(ds->oc, bit, out);
}

/* Yosys `ff.remove()` with every remaining bit tied to src[i] (NULL: all are in pend already): the
 * node goes; every pending bit is tied. */
odin3_status odin3_dff_remove_all(odin3_dff_state *ds, const odin3_net_id *src) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && src != NULL && i < ds->ff.width; i++) {
        st = odin3_dff_tie_bit(ds, i, src[i]);
    }
    odin3_ffdata empty;
    st = st == ODIN3_OK ? odin3_ff_slice(&ds->ff, (odin3_ff_bits){NULL, 0}, &empty) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    empty.node = ds->cell;
    odin3_node_id none = {0};
    st = odin3_ff_emit_shrunk(ds->oc, &empty, ds->pend.data, (uint32_t)ds->pend.len, &none);
    odin3_ffdata_free(&empty);
    ds->gone = ds->gone || st == ODIN3_OK;
    return st;
}

/* The constant nets of vals[0 ..) in nets (replaced). */
odin3_status odin3_dff_set_consts(odin3_dff_state *ds, odin3_vec *nets, const odin3_vec *vals) {
    odin3_vec_clear(nets);
    odin3_status st = odin3_vec_reserve(nets, vals->len);
    for (uint32_t i = 0; st == ODIN3_OK && i < vals->len; i++) {
        odin3_net_id net = {0};
        st = odin3_dff_const_net(ds, odin3_ff_bit_at(vals, i), &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(nets, net) : st;
    }
    return st;
}

odin3_status odin3_dff_bit_push(odin3_vec *vec, odin3_bit bit) {
    odin3_bit *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = bit;
    return ODIN3_OK;
}

/* vec = n copies of bit. */
odin3_status odin3_dff_bits_fill(odin3_vec *vec, odin3_bit bit, uint32_t n) {
    odin3_vec_clear(vec);
    odin3_status st = odin3_vec_reserve(vec, n);
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = odin3_dff_bit_push(vec, bit);
    }
    return st;
}

/* The odin3_bit a constant net carries (0, 1, x or z). */
odin3_bit odin3_dff_const_bit(odin3_const val) {
    return (odin3_bit)(val - ODIN3_CONST_0);
}
