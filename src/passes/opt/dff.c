/*
 * dff.c — opt_dff's driver (2F spec OP-9 §9.3; plan Task 11): Yosys 0.55 passes/opt/opt_dff.cc
 * OptDffWorker::run and run_constbits (no -sat) over Odin III's FF cells, on the FF model of ff.c.
 * The rules live in dff_rules.c (2-9) and dff_merge.c (10-11), the constant bits in
 * dff_constbits.c; this file loads each FF, runs them in order, re-emits a changed FF (rule 12)
 * and flushes the ties.
 *
 * - Work list. The module's FFs in ID order; each is loaded into an odin3_ffdata, the rules run on
 *   it in §9.3's order, and a changed FF is re-emitted (rule 12). Task 12 pushes the FFs its
 *   groups make; Yosys does not queue a re-emitted FF again, nor does this.
 * - Q ties (plan point (l)). Yosys's SigMap is built once per worker: module->connect does not
 *   change what a later rule or constbits sees. Here every Q tie (a removed FF, a given-up bit, a
 *   constbits constant) is queued in oc->ties (odin3_kit_connect and ff.c under oc->defer_ties)
 *   and made at the end of the module's pass, so a constant register pipeline collapses one stage
 *   per call. A removed bit stays in the FF model's `pend` until its FF is rebuilt without it.
 * - An FF with a pin left open, or two Q pins on one net, is no candidate (spec §17 #31).
 */
#include "passes/opt/dff.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
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

/* --- one FF ----------------------------------------------------------------------------------- */

/* The rules of §9.3 in order: 2-9, then 10 (SRST merging) and 11 (EN merging). */
static const odin3_dff_rule_fn k_rules[] = {
    odin3_dff_rule_sr,         odin3_dff_rule_aload,   odin3_dff_rule_arst, odin3_dff_rule_srst,
    odin3_dff_rule_en,         odin3_dff_rule_clk,     odin3_dff_rule_dq,   odin3_dff_rule_adq,
    odin3_dff_rule_srst_merge, odin3_dff_rule_en_merge};

/* Rule 12: a changed FF is re-emitted (Yosys `ff.emit()`), on the bits left of its node's Q. The
 * visit then goes on with the node made (rules 10 and 11 commit in the middle of a visit). */
odin3_status odin3_dff_commit(odin3_dff_state *ds) {
    odin3_node_id out = {0};
    if (ds->gone || !ds->changed) {
        return ODIN3_OK;
    }
    ds->ff.node = ds->cell;
    odin3_status st =
        ds->ff.width == ds->width0
            ? odin3_ff_emit(ds->oc, &ds->ff, &out)
            : odin3_ff_emit_shrunk(ds->oc, &ds->ff, ds->pend.data, (uint32_t)ds->pend.len, &out);
    if (st == ODIN3_OK) {
        ds->counted = true;
        ds->changed = false;
        ds->gone = out.v == 0;
        ds->cell = out.v != 0 ? out : ds->cell;
        ds->ff.node = out;
        ds->width0 = ds->ff.width;
        ds->prov_ready = false;
        odin3_vec_clear(&ds->pend);
    }
    return st;
}

static odin3_status visit_ff(odin3_dff_state *ds, odin3_node_id node) {
    bool usable = false;
    odin3_status st = odin3_dff_state_load(ds, node, &usable);
    for (size_t i = 0;
         st == ODIN3_OK && usable && !ds->gone && i < sizeof k_rules / sizeof k_rules[0]; i++) {
        st = k_rules[i](ds);
    }
    st = st == ODIN3_OK && usable ? odin3_dff_commit(ds) : st;
    ds->n_changed += st == ODIN3_OK && usable && (ds->gone || ds->changed || ds->counted) ? 1U : 0U;
    return st;
}

/* --- the module and the pass ------------------------------------------------------------------ */

/* The FFs of the module in ID order (elements uint32_t node IDs). */
static odin3_status collect_ffs(const odin3_opt_ctx *oc, odin3_vec *list) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(oc->module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(oc->module, node) &&
            odin3_ff_is_ff(oc, odin3_node_type(oc->module, node))) {
            st = odin3_kit_push_u32(list, id);
        }
    }
    return st;
}

static odin3_status module_rules(odin3_dff_state *ds) {
    odin3_vec list;
    odin3_vec_init(&list, sizeof(uint32_t));
    odin3_status st = collect_ffs(ds->oc, &list);
    st = st == ODIN3_OK ? odin3_dff_muxes_build(ds) : st;
    ds->queue = &list;
    /* the list grows when the groups of rules 10 and 11 push the FFs they make */
    for (size_t i = 0; st == ODIN3_OK && i < list.len; i++) {
        st = visit_ff(ds, (odin3_node_id){*(const uint32_t *)odin3_vec_cat(&list, i)});
    }
    ds->queue = NULL;
    odin3_vec_free(&list);
    odin3_dff_muxes_free(&ds->muxes);
    return st;
}

static odin3_status module_constbits(odin3_dff_state *ds) {
    odin3_status st = ODIN3_OK;
    const uint32_t end = odin3_module_node_end(ds->oc->module);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(ds->oc->module, node) &&
            odin3_ff_is_ff(ds->oc, odin3_node_type(ds->oc->module, node))) {
            st = odin3_dff_visit_constbits(ds, node);
        }
    }
    return st;
}

typedef struct dff_totals {
    uint32_t changed, bits;
} dff_totals;

static odin3_status dff_module(odin3_opt_ctx *oc, dff_totals *totals) {
    odin3_dff_state ds;
    odin3_dff_state_init(&ds, oc);
    odin3_status st = odin3_opt_canon_consts(oc);
    oc->defer_ties = true;
    st = st == ODIN3_OK ? module_rules(&ds) : st;
    st = st == ODIN3_OK ? module_constbits(&ds) : st;
    /* the ties are made even after a failure: the FFs that went are tied as far as they can be; a
     * tie that cannot be made leaves its Q net undriven (the IR valid, the design to be dropped) */
    odin3_status flushed = odin3_kit_half_done(oc, odin3_kit_flush_ties(oc));
    oc->defer_ties = false;
    totals->changed += ds.n_changed;
    totals->bits += ds.n_bits;
    odin3_dff_state_free(&ds);
    return st != ODIN3_OK ? st : flushed;
}

odin3_status odin3_opt_dff_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_dff: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    dff_totals totals = {0, 0};
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = dff_module(&oc, &totals);
        odin3_opt_ctx_free(&oc);
    }
    counts->dff_changed += totals.changed;
    counts->dff_const_bits += totals.bits;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "opt_dff: %" PRIu32 " flip-flops changed, %" PRIu32 " constant bits",
                  totals.changed, totals.bits);
    }
    return st;
}
