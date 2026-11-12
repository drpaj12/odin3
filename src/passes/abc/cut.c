/*
 * cut.c — the exact cut of each LUT (v1.6.2, `abc --prov-cut on`): which old gates a LUT's function
 * covers. A LUT has leaves (its fan-in objects) and a root (itself); when each of those has an old
 * net that computes the same function (an anchor), the old gates between the root net and the leaf
 * nets are the cut. The LUT then carries one record whose parents are the records of those gates,
 * labelled cut=exact. Where a leaf or the root has no old net, or the walk from the root reaches
 * something that is not a leaf, the LUT keeps its region record under a one-parent record labelled
 * cut=approx.
 */
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/prov.h"
#include "passes/abc/region.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Gates one walk may visit before the LUT is called approximate. */
enum { CUT_WALK_CAP = 4096, FIRST_OBJECT = 2, STACK_PARENTS = 16 };

static const odin3_opclass cut_class = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, false,
                                        0};
static const odin3_opclass approx_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_LUT, ODIN3_LEVEL_LUT,
                                           false, 0};

typedef struct cut {
    odin3_abc_run *run;
    const odin3_abc_result *res;
    const odin3_module *mod;
    const odin3_abc_anchor *anchors;
    odin3_vec first; /* uint32_t per object + 1: where its anchors start in order */
    odin3_vec order; /* uint32_t: anchor indexes grouped by object */
    odin3_vec leaf;  /* uint32_t per net: 1 + the LUT whose leaf the net is */
    odin3_vec seen;  /* uint32_t per node: 1 + the LUT that visited it */
    odin3_vec stack; /* uint32_t */
    odin3_vec gates; /* uint32_t: nodes of the cut found so far */
    uint32_t exact, approx;
    uint32_t cur; /* the LUT being walked */
} cut;

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

static void cut_free(cut *ct) {
    odin3_vec_free(&ct->first);
    odin3_vec_free(&ct->order);
    odin3_vec_free(&ct->leaf);
    odin3_vec_free(&ct->seen);
    odin3_vec_free(&ct->stack);
    odin3_vec_free(&ct->gates);
}

/* Groups the anchor indexes by object (counting sort). */
static odin3_status group_anchors(cut *ct, size_t n_anchors, uint32_t n_objs) {
    odin3_status st = zero32(&ct->first, (size_t)n_objs + 1U);
    for (size_t at = 0; at < n_anchors; at++) {
        ((uint32_t *)ct->first.data)[ct->anchors[at].obj + 1U]++;
    }
    for (uint32_t obj = 0; obj < n_objs; obj++) {
        ((uint32_t *)ct->first.data)[obj + 1U] += at32(&ct->first, obj);
    }
    odin3_vec fill;
    odin3_vec_init(&fill, sizeof(uint32_t));
    st = st == ODIN3_OK ? zero32(&ct->order, n_anchors) : st;
    st = st == ODIN3_OK ? zero32(&fill, n_objs) : st;
    for (size_t at = 0; st == ODIN3_OK && at < n_anchors; at++) {
        uint32_t obj = ct->anchors[at].obj;
        ((uint32_t *)ct->order.data)[at32(&ct->first, obj) + at32(&fill, obj)] = (uint32_t)at;
        ((uint32_t *)fill.data)[obj]++;
    }
    odin3_vec_free(&fill);
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

/* Marks the old nets of the fan-in objects of LUT lut as its leaves. False when a fan-in object
 * (not a constant) has no old net. */
static bool mark_leaves(cut *ct, uint32_t lut) {
    uint32_t inputs = at32(&ct->res->lut_k, lut);
    const uint32_t *fanins =
        (const uint32_t *)ct->res->fanins.data + at32(&ct->res->fanin_start, lut);
    bool all = true;
    for (uint32_t var = 0; var < inputs; var++) {
        uint32_t obj = fanins[var];
        if (obj < FIRST_OBJECT) {
            continue;
        }
        bool any = false;
        for (uint32_t k = at32(&ct->first, obj); k < at32(&ct->first, obj + 1U); k++) {
            odin3_net_id net = ct->anchors[at32(&ct->order, k)].old_net;
            if (odin3_net_valid(net)) {
                ((uint32_t *)ct->leaf.data)[net.v] = lut + 1U;
                any = true;
            }
        }
        all = all && any;
    }
    return all;
}

/* The first old net of the LUT's own object, or none. */
static odin3_net_id root_net(const cut *ct, uint32_t obj) {
    for (uint32_t k = at32(&ct->first, obj); k < at32(&ct->first, obj + 1U); k++) {
        odin3_net_id net = ct->anchors[at32(&ct->order, k)].old_net;
        if (odin3_net_valid(net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

/* Visits one gate of the cut: adds it and queues the drivers of its inputs that are not leaves.
 * Sets *escape when an input is neither a leaf, a constant nor driven by one soft cell. */
static odin3_status visit_gate(cut *ct, uint32_t node, bool *escape) {
    const uint32_t lut = ct->cur;
    ((uint32_t *)ct->seen.data)[node] = lut + 1U;
    odin3_status st = push32(&ct->gates, node);
    odin3_pinslice pins = odin3_node_pins(ct->mod, (odin3_node_id){node});
    for (uint32_t i = 0; st == ODIN3_OK && i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(ct->mod, pin);
        if (!odin3_pin_reads(ct->mod, pin) || !odin3_net_valid(net) ||
            at32(&ct->leaf, net.v) == lut + 1U || odin3_abc_pin_literal(ct->run, net) >= 0) {
            continue;
        }
        uint32_t drv = soft_driver(ct, net);
        if (drv == 0) {
            *escape = true;
        } else if (at32(&ct->seen, drv) != lut + 1U) {
            st = push32(&ct->stack, drv);
        }
    }
    return st;
}

/* Walks from the root cell. *escape is set when the walk leaves the leaves or runs too long. */
static odin3_status walk_cut(cut *ct, uint32_t root, bool *escape) {
    const uint32_t lut = ct->cur;
    odin3_vec_clear(&ct->stack);
    odin3_vec_clear(&ct->gates);
    odin3_status st = push32(&ct->stack, root);
    while (st == ODIN3_OK && ct->stack.len > 0 && !*escape) {
        uint32_t node = at32(&ct->stack, ct->stack.len - 1U);
        odin3_vec_pop(&ct->stack);
        if (at32(&ct->seen, node) == lut + 1U) {
            continue;
        }
        st = visit_gate(ct, node, escape);
        *escape = *escape || ct->gates.len > CUT_WALK_CAP;
    }
    return st;
}

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

/* Tries the exact cut of one LUT. *rec stays 0 when it cannot be exact. */
static odin3_status exact_record(cut *ct, uint32_t lut, odin3_prov_id *rec) {
    uint32_t obj = odin3_abc_result_first_lut(ct->res) + lut;
    ct->cur = lut;
    odin3_net_id root = root_net(ct, obj);
    bool leaves = mark_leaves(ct, lut);
    uint32_t drv = leaves ? soft_driver(ct, root) : 0;
    bool escape = drv == 0;
    odin3_status st = ODIN3_OK;
    if (!escape) {
        st = walk_cut(ct, drv, &escape);
    }
    if (st == ODIN3_OK && !escape) {
        odin3_prov_begin_op_class(ct->run->ctx, &cut_class);
        st = derive_cut(ct, rec);
        st = st == ODIN3_OK && rec->v != 0 ? annotate_cut(ct, *rec, ODIN3_PROV_CUT_EXACT) : st;
    }
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
        }
        if (st == ODIN3_OK && rec.v != 0) {
            ((uint32_t *)obj_prov->data)[first_lut + lut] = rec.v;
        }
    }
    return st;
}

odin3_status odin3_abc_cut_records(odin3_abc_run *run, const odin3_abc_result *res,
                                   const odin3_vec *anchors, odin3_vec *obj_prov) {
    cut ct = {0};
    ct.run = run;
    ct.res = res;
    ct.mod = run->module;
    ct.anchors = anchors->data;
    odin3_vec_init(&ct.stack, sizeof(uint32_t));
    odin3_vec_init(&ct.gates, sizeof(uint32_t));
    uint32_t n_objs = odin3_abc_result_first_lut(res) + res->n_luts;
    odin3_status st = group_anchors(&ct, anchors->len, n_objs);
    st = st == ODIN3_OK ? zero32(&ct.leaf, odin3_module_net_end(run->module)) : st;
    st = st == ODIN3_OK ? zero32(&ct.seen, odin3_module_node_end(run->module)) : st;
    st = st == ODIN3_OK ? all_luts(&ct, obj_prov) : st;
    if (st == ODIN3_OK) {
        odin3_log(
            ODIN3_LOG_INFO, "abc: module '%s': LUT cuts: %u exact, %u approximate (--prov-cut on)",
            odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module)),
            (unsigned)ct.exact, (unsigned)ct.approx);
        run->stats.cut_exact = ct.exact;
        run->stats.cut_approx = ct.approx;
    }
    cut_free(&ct);
    return st;
}
