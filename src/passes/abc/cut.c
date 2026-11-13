/*
 * cut.c — the exact cut of each LUT (v1.6.2, `abc --prov-cut on`): which old gates a LUT's function
 * covers. cutsig.c finds, for the LUT's inputs and for the LUT, old nets with the same function
 * (equal signatures on random patterns) and proves the cut: the AIG function of the root net over
 * the leaf nets is the LUT's truth table. The gates between the root net and the leaf nets are the
 * cut; the LUT then carries one record whose parents are the records of those gates, labelled
 * cut=exact. A LUT with no such nets, or whose proof fails, keeps its region record under a
 * one-parent record labelled cut=approx.
 */
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/cut_int.h"
#include "passes/abc/prov.h"
#include "passes/abc/region.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const odin3_opclass cut_class = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, false,
                                        0};
static const odin3_opclass approx_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_LUT, ODIN3_LEVEL_LUT,
                                           false, 0};

static uint32_t at32(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

static odin3_status push32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static odin3_status zero32(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(vec, count + 1U);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, sizeof(uint32_t) * (count + 1U));
        vec->len = count;
    }
    return st;
}

static bool is_soft(const cut *ct, odin3_node_id node) {
    return ((const uint8_t *)ct->run->region->role.data)[node.v] == ODIN3_ABC_ROLE_SOFT;
}

/* The soft cell that alone drives net, or 0. */
static uint32_t soft_driver(const cut *ct, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(ct->mod, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(ct->mod, odin3_net_driver(ct->mod, net));
    return is_soft(ct, node) ? node.v : 0;
}

/* --- the leaves of one LUT ------------------------------------------------------------------ */

/* Marks the candidate nets of every input of LUT lut as its leaves. False when an input has none
 * (or is a constant): the cut cannot be found. */
static bool mark_leaves(cut *ct, uint32_t lut) {
    uint32_t inputs = at32(&ct->res->lut_k, lut);
    const uint32_t *fanins =
        (const uint32_t *)ct->res->fanins.data + at32(&ct->res->fanin_start, lut);
    odin3_vec cands;
    odin3_vec_init(&cands, sizeof(cut_net));
    odin3_vec_clear(&ct->leafs);
    bool all = true;
    for (uint32_t var = 0; all && var < inputs; var++) {
        all = fanins[var] >= CUT_FIRST_OBJ && cut_candidates(ct, fanins[var], &cands) == ODIN3_OK &&
              cands.len > 0;
        for (size_t at = 0; all && at < cands.len; at++) {
            const cut_net *cn = odin3_vec_cat(&cands, at);
            cut_leaf *slot = odin3_vec_push(&ct->leafs);
            all = slot != NULL;
            if (all) {
                *slot = (cut_leaf){cn->net, var, cn->inv};
                ((uint32_t *)ct->leaf.data)[cn->net] = lut + 1U;
            }
        }
    }
    odin3_vec_free(&cands);
    return all;
}

/* --- the walk over the old gates ------------------------------------------------------------ */

/* Visits one gate of the cut: adds it and queues the drivers of its inputs that are not leaves.
 * Sets *escape when an input is neither a leaf, a constant nor driven by one soft cell. */
static odin3_status visit_gate(cut *ct, uint32_t node, bool *escape) {
    const uint32_t mark = ct->cur + 1U;
    ((uint32_t *)ct->seen.data)[node] = mark;
    odin3_status st = push32(&ct->gates, node);
    odin3_pinslice pins = odin3_node_pins(ct->mod, (odin3_node_id){node});
    for (uint32_t i = 0; st == ODIN3_OK && i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(ct->mod, pin);
        if (!odin3_pin_reads(ct->mod, pin) || !odin3_net_valid(net) ||
            at32(&ct->leaf, net.v) == mark || odin3_abc_pin_literal(ct->run, net) >= 0) {
            continue;
        }
        uint32_t drv = soft_driver(ct, net);
        if (drv == 0) {
            *escape = true;
        } else if (at32(&ct->seen, drv) != mark) {
            st = push32(&ct->stack, drv);
        }
    }
    return st;
}

/* Walks from the root cell. *escape is set when the walk leaves the leaves or runs too long. */
static odin3_status walk_cut(cut *ct, uint32_t root, bool *escape) {
    odin3_vec_clear(&ct->stack);
    odin3_vec_clear(&ct->gates);
    odin3_status st = push32(&ct->stack, root);
    while (st == ODIN3_OK && ct->stack.len > 0 && !*escape) {
        uint32_t node = at32(&ct->stack, ct->stack.len - 1U);
        odin3_vec_pop(&ct->stack);
        if (at32(&ct->seen, node) == ct->cur + 1U) {
            continue;
        }
        st = visit_gate(ct, node, escape);
        *escape = *escape || ct->gates.len > CUT_WALK_CAP;
    }
    return st;
}

/* --- the records ---------------------------------------------------------------------------- */

/* The distinct nonzero records of the gates found, in node order, as parents of one record. */
static odin3_status derive_cut(cut *ct, odin3_prov_id *out) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    odin3_status st = ODIN3_OK;
    for (size_t at = 0; st == ODIN3_OK && at < ct->gates.len; at++) {
        odin3_prov_id prov = odin3_node_prov(ct->mod, (odin3_node_id){at32(&ct->gates, at)});
        bool dup = prov.v == 0;
        for (size_t i = 0; !dup && i < parents.len; i++) {
            dup = ((const odin3_prov_id *)parents.data)[i].v == prov.v;
        }
        odin3_prov_id *slot = dup ? NULL : odin3_vec_push(&parents);
        if (slot != NULL) {
            *slot = prov;
        } else if (!dup) {
            st = ODIN3_ERR_NO_MEMORY;
        }
    }
    if (st == ODIN3_OK && parents.len > 0) {
        odin3_prov_begin_op_class(ct->run->ctx, &cut_class);
        st = odin3_prov_derive_class(ct->run->ctx,
                                     (odin3_prov_list){parents.data, (uint32_t)parents.len}, 0,
                                     &cut_class, out);
    }
    odin3_vec_free(&parents);
    return st;
}

static odin3_status annotate_cut(const cut *ct, odin3_prov_id rec, uint32_t kind) {
    odin3_prov_annot annot = {0, 0, 0, kind};
    return odin3_prov_annotate(ct->run->design, rec, &annot);
}

/* Tries one root candidate: the walk, then the proof. *rec stays 0 when it fails. */
static odin3_status try_root(cut *ct, uint32_t lut, const cut_net *root, odin3_prov_id *rec) {
    uint32_t drv = soft_driver(ct, (odin3_net_id){root->net});
    bool escape = drv == 0 || at32(&ct->leaf, root->net) == lut + 1U;
    odin3_status st = ODIN3_OK;
    ct->why = CUT_WHY_WALK;
    if (!escape) {
        st = walk_cut(ct, drv, &escape);
        ct->why = CUT_WHY_PROOF;
    }
    if (st == ODIN3_OK && !escape && cut_prove(ct, lut, root)) {
        st = derive_cut(ct, rec);
        st = st == ODIN3_OK && rec->v != 0 ? annotate_cut(ct, *rec, ODIN3_PROV_CUT_EXACT) : st;
    }
    return st;
}

/* Tries the exact cut of one LUT. *rec stays 0 when it cannot be exact. */
static odin3_status exact_record(cut *ct, uint32_t lut, odin3_prov_id *rec) {
    uint32_t obj = odin3_abc_result_first_lut(ct->res) + lut;
    ct->cur = lut;
    ct->why = CUT_WHY_INPUT;
    odin3_vec roots;
    odin3_vec_init(&roots, sizeof(cut_net));
    odin3_status st = ODIN3_OK;
    if (mark_leaves(ct, lut)) {
        ct->why = CUT_WHY_ROOT;
        st = cut_candidates(ct, obj, &roots);
    }
    for (size_t at = 0; st == ODIN3_OK && rec->v == 0 && at < roots.len; at++) {
        st = try_root(ct, lut, odin3_vec_cat(&roots, at), rec);
    }
    odin3_vec_free(&roots);
    return st;
}

/* The one-parent record of an approximate LUT: the region record it already carries. */
static odin3_status approx_record(cut *ct, uint32_t current, odin3_prov_id *rec) {
    if (current == 0) {
        return ODIN3_OK;
    }
    odin3_prov_begin_op_class(ct->run->ctx, &approx_class);
    odin3_prov_id parent = {current};
    odin3_status st =
        odin3_prov_derive_class(ct->run->ctx, (odin3_prov_list){&parent, 1}, 0, &approx_class, rec);
    return st == ODIN3_OK && rec->v != 0 ? annotate_cut(ct, *rec, ODIN3_PROV_CUT_APPROX) : st;
}

static odin3_status all_luts(cut *ct, odin3_vec *obj_prov) {
    uint32_t first_lut = odin3_abc_result_first_lut(ct->res);
    odin3_status st = ODIN3_OK;
    for (uint32_t lut = 0; st == ODIN3_OK && lut < ct->res->n_luts; lut++) {
        odin3_prov_id rec = {0};
        uint32_t current = at32(obj_prov, first_lut + lut);
        st = exact_record(ct, lut, &rec);
        if (st == ODIN3_OK && rec.v != 0) {
            ct->exact++;
        } else if (st == ODIN3_OK) {
            st = approx_record(ct, current, &rec);
            ct->approx += current != 0 ? 1U : 0U;
            ct->miss[ct->why] += current != 0 ? 1U : 0U;
        }
        if (st == ODIN3_OK && rec.v != 0) {
            ((uint32_t *)obj_prov->data)[first_lut + lut] = rec.v;
        }
    }
    return st;
}

static odin3_status cut_setup(cut *ct) {
    odin3_vec_init(&ct->table, sizeof(cut_cand));
    odin3_vec_init(&ct->stack, sizeof(uint32_t));
    odin3_vec_init(&ct->gates, sizeof(uint32_t));
    odin3_vec_init(&ct->vpool, sizeof(uint64_t));
    odin3_vec_init(&ct->leafs, sizeof(cut_leaf));
    odin3_status st = zero32(&ct->leaf, odin3_module_net_end(ct->mod));
    st = st == ODIN3_OK ? zero32(&ct->seen, odin3_module_node_end(ct->mod)) : st;
    st = st == ODIN3_OK ? cut_sigs_build(ct) : st;
    size_t n_aig = ct->aig.objs.len / 2U;
    st = st == ODIN3_OK ? zero32(&ct->vstamp, n_aig) : st;
    return st == ODIN3_OK ? zero32(&ct->voff, n_aig) : st;
}

odin3_status odin3_abc_cut_records(odin3_abc_run *run, const odin3_abc_result *res,
                                   const odin3_vec *anchors, odin3_vec *obj_prov) {
    (void)anchors;
    cut ct = {0};
    ct.run = run;
    ct.res = res;
    ct.mod = run->module;
    odin3_vec_init(&ct.asig, sizeof(uint64_t));
    odin3_vec_init(&ct.osig, sizeof(uint64_t));
    odin3_status st = cut_setup(&ct);
    if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_WARN, "abc: --prov-cut: the region is too large for the cut proof; "
                                  "the LUTs keep their region records");
        st = ODIN3_OK;
    } else {
        st = st == ODIN3_OK ? all_luts(&ct, obj_prov) : st;
    }
    if (st == ODIN3_OK) {
        const char *name =
            odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module));
        odin3_log(ODIN3_LOG_INFO,
                  "abc: module '%s': LUT cuts: %u exact, %u approximate (--prov-cut on); "
                  "approximate: %u without an old net for an input, %u without one for the output, "
                  "%u whose walk left the leaves, %u not proved",
                  name, (unsigned)ct.exact, (unsigned)ct.approx, (unsigned)ct.miss[CUT_WHY_INPUT],
                  (unsigned)ct.miss[CUT_WHY_ROOT], (unsigned)ct.miss[CUT_WHY_WALK],
                  (unsigned)ct.miss[CUT_WHY_PROOF]);
        run->stats.cut_exact = ct.exact;
        run->stats.cut_approx = ct.approx;
    }
    cut_free(&ct);
    return st;
}
