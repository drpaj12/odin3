/*
 * dff_merge.c — opt_dff's rules 10 and 11 (2F spec OP-9 §9.3; plan Task 12): merging the muxes
 * in front of D into the FF as a sync reset (SRST) or a clock enable (EN), on the plans of
 * dff_muxes.c. Yosys 0.55 passes/opt/opt_dff.cc run() lines 582-712 (the "try to spice it up"
 * block) and make_patterns_logic / combine_resets.
 *
 * - Groups. The merged bits are grouped by their key (the controls, and for EN the patterns), in
 *   the order of their first bit. Each group becomes a new FF of the sliced bits, made on the old
 *   Q nets (a second driver until the old node is shrunk), with its helper gates; the FF's bits
 *   that were not merged stay in the old node, which is re-emitted at once (so the ties of the
 *   handed-on Q nets are "keep == drop": the init stays). The groups' FFs go on the work list.
 * - Build before destroy. The gates and the groups' FFs are made first; a failure then deletes
 *   the FFs made, and the mux inputs the patterns turned to x are turned back. Only the re-emit
 *   of the old node is destructive, and it fails whole or goes through odin3_kit_half_done.
 * - Provenance. A group's gates and FF carry one record whose parents are [the old FF, the muxes
 *   the group absorbed in ID order].
 * - Not done (spec §17 #32, #33): fine FFs (the registry has no $_SDFF_ or $_DFFE_), and SRST on
 *   an FF with ALOAD (Yosys 0.55 drops the reset there).
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
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NO_GROUP = UINT32_MAX };

/* --- grouping --------------------------------------------------------------------------------- */

typedef struct group {
    uint32_t first, next, count, start; /* first bit, hash chain, bits, offset in members */
} group;

typedef struct groups {
    odin3_vec list;    /* group */
    odin3_vec of_bit;  /* uint32_t: the group of each bit, NO_GROUP for an unmerged one */
    odin3_vec members; /* uint32_t: the bits, group after group */
    odin3_u64map *heads;
} groups;

static void groups_free(groups *gr) {
    odin3_vec_free(&gr->list);
    odin3_vec_free(&gr->of_bit);
    odin3_vec_free(&gr->members);
    odin3_u64map_destroy(gr->heads);
}

static const odin3_dff_bitplan *plan_bit(const odin3_dff_plan *plan, uint32_t idx) {
    return odin3_vec_cat(&plan->bits, idx);
}

static const uint64_t *key_words(const odin3_dff_plan *plan, const odin3_dff_bitplan *bp) {
    return (const uint64_t *)plan->keys.data + bp->key_off;
}

static uint64_t key_hash(const odin3_dff_plan *plan, const odin3_dff_bitplan *bp) {
    const uint64_t *words = key_words(plan, bp);
    uint64_t hash = 1469598103934665603ULL; /* FNV-1a over the words */
    for (uint32_t k = 0; k < bp->key_len; k++) {
        hash = (hash ^ words[k]) * 1099511628211ULL;
    }
    return hash;
}

static bool key_equal(const odin3_dff_plan *plan, const odin3_dff_bitplan *lhs,
                      const odin3_dff_bitplan *rhs) {
    return lhs->key_len == rhs->key_len &&
           (lhs->key_len == 0 ||
            memcmp(key_words(plan, lhs), key_words(plan, rhs), lhs->key_len * sizeof(uint64_t)) ==
                0);
}

/* The group of the chain at `head` whose key is bit's, NO_GROUP if none. */
static uint32_t chain_find(const groups *gr, const odin3_dff_plan *plan, uint32_t head,
                           const odin3_dff_bitplan *bp) {
    for (uint32_t at = head; at != NO_GROUP;) {
        const group *cand = odin3_vec_cat(&gr->list, at);
        if (key_equal(plan, plan_bit(plan, cand->first), bp)) {
            return at;
        }
        at = cand->next;
    }
    return NO_GROUP;
}

/* Bit idx joins its group (a new one when its key is new). */
static odin3_status group_add(groups *gr, const odin3_dff_plan *plan, uint32_t idx) {
    const odin3_dff_bitplan *bp = plan_bit(plan, idx);
    uint64_t hash = key_hash(plan, bp);
    uint64_t head = NO_GROUP;
    (void)odin3_u64map_get(gr->heads, hash, &head);
    uint32_t at = chain_find(gr, plan, (uint32_t)head, bp);
    if (at == NO_GROUP) {
        group *slot = odin3_vec_push(&gr->list);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (group){idx, (uint32_t)head, 0, 0};
        at = (uint32_t)gr->list.len - 1U;
        odin3_status st = odin3_u64map_put(gr->heads, (odin3_kv){hash, at});
        if (st != ODIN3_OK) {
            odin3_vec_pop(&gr->list);
            return st;
        }
    }
    ((group *)gr->list.data)[at].count++;
    ((uint32_t *)gr->of_bit.data)[idx] = at;
    return ODIN3_OK;
}

/* members: the merged bits ordered by group (counting sort), each group's bits in bit order. */
static odin3_status group_members(groups *gr, uint32_t n_merged) {
    odin3_vec fill;
    odin3_vec_init(&fill, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(&fill, gr->list.len);
    uint32_t next = 0;
    for (size_t g = 0; st == ODIN3_OK && g < gr->list.len; g++) {
        st = odin3_kit_push_u32(&fill, next);
        ((group *)gr->list.data)[g].start = next;
        next += ((const group *)gr->list.data)[g].count;
    }
    st = st == ODIN3_OK ? odin3_vec_reserve(&gr->members, n_merged) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < n_merged; k++) {
        st = odin3_kit_push_u32(&gr->members, 0);
    }
    for (size_t i = 0; st == ODIN3_OK && i < gr->of_bit.len; i++) {
        uint32_t g = ((const uint32_t *)gr->of_bit.data)[i];
        if (g != NO_GROUP) {
            ((uint32_t *)gr->members.data)[((uint32_t *)fill.data)[g]++] = (uint32_t)i;
        }
    }
    odin3_vec_free(&fill);
    return st;
}

/* Builds the groups of the merged bits of the plan; *n_merged = their number. */
static odin3_status groups_build(groups *gr, const odin3_dff_plan *plan, uint32_t *n_merged) {
    odin3_vec_init(&gr->list, sizeof(group));
    odin3_vec_init(&gr->of_bit, sizeof(uint32_t));
    odin3_vec_init(&gr->members, sizeof(uint32_t));
    gr->heads = odin3_u64map_create(0);
    odin3_status st = gr->heads != NULL ? odin3_vec_reserve(&gr->of_bit, plan->bits.len)
                                        : ODIN3_ERR_NO_MEMORY;
    *n_merged = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < plan->bits.len; i++) {
        bool merged = plan_bit(plan, i)->merged;
        st = odin3_kit_push_u32(&gr->of_bit, NO_GROUP);
        st = st == ODIN3_OK && merged ? group_add(gr, plan, i) : st;
        *n_merged += merged ? 1U : 0U;
    }
    return st == ODIN3_OK ? group_members(gr, *n_merged) : st;
}

/* --- the controls ----------------------------------------------------------------------------- */

typedef struct ctrl {
    odin3_net_id net;
    bool pol;
} ctrl;

static ctrl ctrl_of(uint64_t word) {
    return (ctrl){(odin3_net_id){(uint32_t)(word >> 1U)}, (word & 1U) != 0};
}

/* ~net when pol is false, net itself otherwise; appended to vec. */
static odin3_status push_active(odin3_dff_state *ds, odin3_vec *vec, ctrl item, bool want) {
    odin3_net_id net = item.net;
    odin3_status st = item.pol == want ? ODIN3_OK : odin3_dff_not_net(ds, item.net, &net);
    return st == ODIN3_OK ? odin3_kit_push_net(vec, net) : st;
}

/* Yosys combine_resets: the controls (words) as one SRST net and its polarity. */
static odin3_status combine_resets(odin3_dff_state *ds, const uint64_t *words, uint64_t n,
                                   ctrl *out) {
    if (n == 1) {
        *out = ctrl_of(words[0]);
        return ODIN3_OK;
    }
    bool final_pol = false;
    for (uint64_t k = 0; k < n; k++) {
        final_pol = final_pol || ctrl_of(words[k]).pol;
    }
    odin3_vec inputs;
    odin3_vec_init(&inputs, sizeof(odin3_net_id));
    odin3_status st = odin3_vec_reserve(&inputs, n);
    for (uint64_t k = 0; st == ODIN3_OK && k < n; k++) {
        st = push_active(ds, &inputs, ctrl_of(words[k]), final_pol);
    }
    out->pol = final_pol;
    st = st == ODIN3_OK ? odin3_dff_gate_reduce(ds, final_pol ? "$reduce_or" : "$reduce_and",
                                                &inputs, &out->net)
                        : st;
    odin3_vec_free(&inputs);
    return st;
}

/* The $ne of one pattern (words): the selects against the values they are not to have; an empty
 * pattern (D = Q on the bit) never enables, a constant 0. */
static odin3_status pattern_ne(odin3_dff_state *ds, const uint64_t *words, uint64_t len,
                               odin3_net_id *out) {
    if (len == 0) {
        return odin3_dff_const_net(ds, ODIN3_BIT_0, out);
    }
    odin3_vec sels;
    odin3_vec vals;
    odin3_vec_init(&sels, sizeof(odin3_net_id));
    odin3_vec_init(&vals, sizeof(odin3_net_id));
    odin3_status st = odin3_vec_reserve(&sels, len);
    st = st == ODIN3_OK ? odin3_vec_reserve(&vals, len) : st;
    for (uint64_t k = 0; st == ODIN3_OK && k < len; k++) {
        ctrl item = ctrl_of(words[k]);
        odin3_net_id value = {0};
        st = odin3_dff_const_net(ds, item.pol ? ODIN3_BIT_1 : ODIN3_BIT_0, &value);
        st = st == ODIN3_OK ? odin3_kit_push_net(&sels, item.net) : st;
        st = st == ODIN3_OK ? odin3_kit_push_net(&vals, value) : st;
    }
    st = st == ODIN3_OK ? odin3_dff_gate_ne(ds, &sels, &vals, out) : st;
    odin3_vec_free(&sels);
    odin3_vec_free(&vals);
    return st;
}

/* The ORed inputs of make_patterns_logic: the $ne of each pattern, then the controls. */
static odin3_status patterns_inputs(odin3_dff_state *ds, const uint64_t *key, odin3_vec *inputs) {
    uint64_t n_ctrls = key[0];
    uint64_t n_pats = key[1U + n_ctrls];
    const uint64_t *at = key + 2U + n_ctrls;
    odin3_status st = odin3_vec_reserve(inputs, n_ctrls + n_pats);
    for (uint64_t p = 0; st == ODIN3_OK && p < n_pats; p++) {
        odin3_net_id y = {0};
        st = pattern_ne(ds, at + 1, at[0], &y);
        st = st == ODIN3_OK ? odin3_kit_push_net(inputs, y) : st;
        at += 1U + at[0];
    }
    for (uint64_t k = 0; st == ODIN3_OK && k < n_ctrls; k++) {
        st = push_active(ds, inputs, ctrl_of(key[1U + k]), true);
    }
    return st;
}

/* Yosys make_patterns_logic: EN = every pattern not matched and every control (the key's). */
static odin3_status patterns_logic(odin3_dff_state *ds, const uint64_t *key, ctrl *out) {
    uint64_t n_ctrls = key[0];
    if (key[1U + n_ctrls] == 0 && n_ctrls == 1) {
        *out = ctrl_of(key[1]);
        return ODIN3_OK;
    }
    odin3_vec inputs;
    odin3_vec_init(&inputs, sizeof(odin3_net_id));
    odin3_status st = patterns_inputs(ds, key, &inputs);
    out->pol = true;
    if (st == ODIN3_OK && inputs.len == 0) {
        st = odin3_dff_const_net(ds, ODIN3_BIT_1, &out->net);
    } else if (st == ODIN3_OK && inputs.len == 1) {
        out->net = odin3_ff_net_at(&inputs, 0);
    } else if (st == ODIN3_OK) {
        st = odin3_dff_gate_reduce(ds, "$reduce_and", &inputs, &out->net);
    }
    odin3_vec_free(&inputs);
    return st;
}

/* --- the job ---------------------------------------------------------------------------------- */

typedef struct merge_job {
    odin3_dff_state *ds;
    const odin3_dff_plan *plan;
    groups gr;
    odin3_vec made; /* uint32_t: the FFs made */
    uint32_t applied;
    bool is_srst;
} merge_job;

static odin3_status push_node(odin3_vec *vec, uint32_t node) {
    odin3_node_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_node_id){node};
    return ODIN3_OK;
}

/* The record of a group: parents [the FF, the muxes its bits absorbed in ID order]. */
static odin3_status group_prov(merge_job *job, const group *grp) {
    odin3_dff_state *ds = job->ds;
    const uint32_t *bits = (const uint32_t *)job->gr.members.data + grp->start;
    odin3_vec nodes;
    odin3_vec parents;
    odin3_vec_init(&nodes, sizeof(uint64_t));
    odin3_vec_init(&parents, sizeof(odin3_node_id));
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < grp->count; k++) {
        const odin3_dff_bitplan *bp = plan_bit(job->plan, bits[k]);
        for (uint32_t w = 0; st == ODIN3_OK && w < bp->node_len; w++) {
            st = odin3_kit_push_u64(
                &nodes, ((const uint64_t *)job->plan->nodes.data)[bp->node_off + w]);
        }
    }
    odin3_kit_sort_unify(&nodes, 0);
    st = st == ODIN3_OK ? odin3_vec_reserve(&parents, nodes.len + 1U) : st;
    st = st == ODIN3_OK ? push_node(&parents, ds->cell.v) : st;
    for (size_t k = 0; st == ODIN3_OK && k < nodes.len; k++) {
        st = push_node(&parents, (uint32_t) * (const uint64_t *)odin3_vec_cat(&nodes, k));
    }
    st = st == ODIN3_OK ? odin3_kit_prov(ds->oc, parents.data, (uint32_t)parents.len, &ds->prov)
                        : st;
    ds->prov_ready = st == ODIN3_OK;
    odin3_vec_free(&nodes);
    odin3_vec_free(&parents);
    return st;
}

/* The new FF of a group: the old one's bits, D from the plan, the control set. */
static odin3_status group_ff(merge_job *job, const group *grp, ctrl control, odin3_ffdata *nf) {
    odin3_dff_state *ds = job->ds;
    const uint32_t *bits = (const uint32_t *)job->gr.members.data + grp->start;
    odin3_status st = odin3_ff_slice(&ds->ff, (odin3_ff_bits){bits, grp->count}, nf);
    if (st == ODIN3_OK && job->is_srst) {
        odin3_vec_clear(&nf->val_srst);
        st = odin3_vec_reserve(&nf->val_srst, grp->count);
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < grp->count; k++) {
        const odin3_dff_bitplan *bp = plan_bit(job->plan, bits[k]);
        ((odin3_net_id *)nf->sig_d.data)[k] = bp->d;
        st = job->is_srst ? odin3_dff_bit_push(&nf->val_srst, bp->reset) : st;
    }
    if (job->is_srst) {
        nf->has_srst = true;
        nf->sig_srst = control.net;
        nf->pol_srst = control.pol;
        nf->ce_over_srst = nf->has_ce;
    } else {
        nf->has_ce = true;
        nf->sig_ce = control.net;
        nf->pol_ce = control.pol;
        nf->ce_over_srst = false;
    }
    nf->prov = ds->prov;
    nf->prov_final = true;
    return st;
}

/* The control of group g: from its first bit's key. */
static odin3_status group_control(merge_job *job, const group *grp, ctrl *control) {
    const odin3_dff_bitplan *bp = plan_bit(job->plan, grp->first);
    const uint64_t *key = key_words(job->plan, bp);
    return job->is_srst ? combine_resets(job->ds, key + 1, key[0], control)
                        : patterns_logic(job->ds, key, control);
}

/* Makes group g: its gates, then its FF (a second driver on the Q nets). */
static odin3_status group_emit(merge_job *job, uint32_t g) {
    odin3_dff_state *ds = job->ds;
    const group *grp = odin3_vec_cat(&job->gr.list, g);
    ctrl control = {{0}, true};
    odin3_ffdata nf;
    odin3_ffdata_init(&nf);
    odin3_status st = group_prov(job, grp);
    st = st == ODIN3_OK ? group_control(job, grp, &control) : st;
    st = st == ODIN3_OK ? group_ff(job, grp, control, &nf) : st;
    odin3_node_id out = {0};
    st = st == ODIN3_OK ? odin3_ff_emit(ds->oc, &nf, &out) : st;
    if (out.v != 0) { /* kept to be deleted on a later failure; room was reserved */
        (void)odin3_kit_push_u32(&job->made, out.v);
    }
    if (st == ODIN3_OK) {
        odin3_dff_note(ds, job->is_srst ? ODIN3_OPT_DFF_ADD_SRST : ODIN3_OPT_DFF_ADD_EN, "");
    }
    odin3_ffdata_free(&nf);
    ds->prov_ready = false;
    return st;
}

/* --- the x marks and the old node ------------------------------------------------------------- */

/* The mux inputs that read Q take x (Yosys setPort(B, ... Sx)); job->applied counts the done. */
static odin3_status marks_apply(merge_job *job) {
    odin3_module *module = job->ds->oc->module;
    odin3_net_id xnet = {0};
    odin3_status st = job->plan->marks.len > 0 ? odin3_dff_const_net(job->ds, ODIN3_BIT_X, &xnet)
                                               : ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < job->plan->marks.len; k++) {
        const odin3_dff_xmark *mark = odin3_vec_cat(&job->plan->marks, k);
        st = odin3_pin_disconnect(module, mark->pin);
        st = st == ODIN3_OK ? odin3_pin_connect(module, mark->pin, xnet) : st;
        if (st != ODIN3_OK) {
            (void)odin3_pin_connect(module, mark->pin, mark->old);
            break;
        }
        job->applied++;
    }
    return st;
}

/* Rolls the job back: the marks turned back, the FFs made deleted (the old node is untouched). */
static void job_undo(merge_job *job) {
    odin3_module *module = job->ds->oc->module;
    for (uint32_t k = job->applied; k > 0; k--) {
        const odin3_dff_xmark *mark = odin3_vec_cat(&job->plan->marks, k - 1U);
        (void)odin3_pin_disconnect(module, mark->pin);
        (void)odin3_pin_connect(module, mark->pin, mark->old);
    }
    job->applied = 0;
    for (size_t k = 0; k < job->made.len; k++) {
        odin3_node_id node = {((const uint32_t *)job->made.data)[k]};
        if (odin3_node_live(module, node)) {
            (void)odin3_node_delete(module, node);
        }
    }
    odin3_vec_clear(&job->made);
}

/* The old node without the merged bits, re-emitted: their Q nets are handed on (keep == drop). */
static odin3_status job_retire(merge_job *job) {
    odin3_dff_state *ds = job->ds;
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ds->ff.width; i++) {
        if (plan_bit(job->plan, i)->merged) {
            st = odin3_dff_tie_bit(ds, i, odin3_ff_net_at(&ds->ff.sig_q, i));
        } else {
            st = odin3_kit_push_u32(&keep, i);
        }
    }
    if (st == ODIN3_OK && keep.len == 0) {
        st = odin3_dff_remove_all(ds, NULL);
    } else if (st == ODIN3_OK) {
        st = odin3_dff_state_keep(ds, keep.data, (uint32_t)keep.len);
        ds->changed = true;
        st = st == ODIN3_OK ? odin3_dff_commit(ds) : st;
    }
    odin3_vec_free(&keep);
    return st;
}

/* The groups made, the marks applied and the old node shrunk; the FFs made go on the work list. */
static odin3_status job_run(merge_job *job) {
    odin3_dff_state *ds = job->ds;
    odin3_status st = odin3_vec_reserve(&job->made, job->gr.list.len);
    st = st == ODIN3_OK && ds->queue != NULL
             ? odin3_vec_reserve(ds->queue, ds->queue->len + job->gr.list.len)
             : st;
    for (uint32_t g = 0; st == ODIN3_OK && g < job->gr.list.len; g++) {
        st = group_emit(job, g);
    }
    st = st == ODIN3_OK ? marks_apply(job) : st;
    st = st == ODIN3_OK ? job_retire(job) : st;
    if (st != ODIN3_OK && odin3_node_live(ds->oc->module, ds->cell)) {
        job_undo(job); /* nothing destructive happened: the IR is as it was but for dead gates */
        return st;
    }
    for (size_t k = 0; st == ODIN3_OK && ds->queue != NULL && k < job->made.len; k++) {
        st = odin3_kit_push_u32(ds->queue, ((const uint32_t *)job->made.data)[k]);
    }
    return st;
}

/* --- the rules -------------------------------------------------------------------------------- */

static odin3_status merge_rule(odin3_dff_state *ds, bool is_srst) {
    odin3_dff_plan plan;
    odin3_dff_plan_init(&plan);
    odin3_status st = odin3_vec_reserve(&plan.bits, ds->ff.width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ds->ff.width; i++) {
        st = is_srst ? odin3_dff_plan_srst_bit(ds, &plan, i) : odin3_dff_plan_en_bit(ds, &plan, i);
    }
    merge_job job;
    memset(&job, 0, sizeof job);
    job.ds = ds;
    job.plan = &plan;
    job.is_srst = is_srst;
    odin3_vec_init(&job.made, sizeof(uint32_t));
    uint32_t n_merged = 0;
    st = st == ODIN3_OK ? groups_build(&job.gr, &plan, &n_merged) : st;
    st = st == ODIN3_OK && n_merged > 0 ? job_run(&job) : st;
    groups_free(&job.gr);
    odin3_vec_free(&job.made);
    odin3_dff_plan_free(&plan);
    return st;
}

odin3_status odin3_dff_rule_srst_merge(odin3_dff_state *ds) {
    const odin3_ffdata *ff = &ds->ff;
    bool skip = ds->oc->opts->nosdff || !ff->has_clk || ff->is_fine || ff->has_arst || ff->has_sr ||
                ff->has_aload || (ff->has_srst && ff->has_ce && !ff->ce_over_srst) ||
                odin3_dff_vec_equal(&ff->sig_d, &ff->sig_q);
    return skip ? ODIN3_OK : merge_rule(ds, true);
}

odin3_status odin3_dff_rule_en_merge(odin3_dff_state *ds) {
    const odin3_ffdata *ff = &ds->ff;
    bool skip = ds->oc->opts->nodffe || !ff->has_clk || ff->is_fine ||
                (ff->has_srst && ff->has_ce && ff->ce_over_srst) ||
                odin3_dff_vec_equal(&ff->sig_d, &ff->sig_q);
    return skip ? ODIN3_OK : merge_rule(ds, false);
}
