/*
 * prov.c — anchors and the records of a run's new objects (3D spec §8, AB-25; plan Task 18).
 */
#include "passes/abc/prov.h"

#include "ir/module.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/region.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    FIRST_OBJ = 2,
    TEXT_BYTES = 192,
    PARENT_STACK = 16,  /* parents of one LUT kept on the stack */
    COUNT_CAP = 1 << 28 /* the edge sum stops here: a message, not an answer, needs it exact */
};

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static odin3_status zeros(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(vec, count == 0 ? 1 : count);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, sizeof(uint32_t) * (count == 0 ? 1 : count));
        vec->len = count;
    }
    return st;
}

static uint32_t obj_total(const odin3_abc_result *res) {
    return odin3_abc_result_first_lut(res) + res->n_luts;
}

/* --- anchors ------------------------------------------------------------------------------- */

static odin3_net_id drive_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

static odin3_status add_anchor(odin3_vec *out, uint32_t obj, odin3_net_id net, uint8_t kind) {
    odin3_abc_anchor *slot = odin3_vec_push(out);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_abc_anchor){obj, net, kind, false};
    return ODIN3_OK;
}

odin3_status odin3_abc_anchors(odin3_abc_run *run, const odin3_abc_result *res, odin3_vec *out) {
    const odin3_abc_region *region = run->region;
    odin3_vec_clear(out);
    uint32_t base = FIRST_OBJ + res->n_cis;
    odin3_status st = ODIN3_OK;
    for (uint32_t co = 0; st == ODIN3_OK && co < res->n_cos; co++) {
        st = add_anchor(out, u32_at(&res->co_driver, co), (odin3_net_id){u32_at(&region->cos, co)},
                        ODIN3_ABC_ANCHOR_CO);
    }
    for (uint32_t reg = 0; st == ODIN3_OK && reg < res->n_regs_out; reg++) {
        odin3_net_id dnet = {u32_at(&region->reg_d, u32_at(&res->reg_from, reg))};
        if (odin3_net_valid(dnet)) {
            st = add_anchor(out, u32_at(&res->co_driver, res->n_cos + res->n_obs + reg), dnet,
                            ODIN3_ABC_ANCHOR_REG_IN);
        }
    }
    for (uint32_t reg = 0; st == ODIN3_OK && reg < res->n_regs_out; reg++) {
        odin3_node_id rep = {u32_at(&region->regs, u32_at(&res->reg_from, reg))};
        st = add_anchor(out, base + reg, drive_net(run->module, rep), ODIN3_ABC_ANCHOR_REG);
    }
    for (uint32_t ci = 0; st == ODIN3_OK && ci < res->n_cis; ci++) {
        st = add_anchor(out, FIRST_OBJ + ci, (odin3_net_id){u32_at(&region->cis, ci)},
                        ODIN3_ABC_ANCHOR_CI);
    }
    return st;
}

/* --- the walks ----------------------------------------------------------------------------- */

typedef struct pv {
    odin3_abc_run *run;
    const odin3_abc_result *res;
    const odin3_vec *anchors;
    const odin3_module *mod;
    const odin3_abc_region *region;
    odin3_vec net_anch;    /* uint32_t per net: anchors on the net */
    odin3_vec obj_anch;    /* uint32_t per object: anchors on the object */
    odin3_vec node_stamp;  /* uint32_t per node: last anchor (1 + index) that visited it */
    odin3_vec lut_stamp;   /* uint32_t per LUT */
    odin3_vec memb;        /* uint32_t per LUT: new regions it is in */
    odin3_vec stack;       /* uint32_t */
    odin3_vec old_start;   /* uint32_t per anchor + 1 */
    odin3_vec old_cells;   /* uint32_t node IDs, anchor after anchor, each ascending */
    odin3_vec pairs;       /* uint32_t (lut, anchor) pairs */
} pv;

static const odin3_abc_anchor *anchor_at(const pv *p, size_t idx) {
    return odin3_vec_cat(p->anchors, idx);
}

static void pv_free(pv *p) {
    odin3_vec_free(&p->net_anch);
    odin3_vec_free(&p->obj_anch);
    odin3_vec_free(&p->node_stamp);
    odin3_vec_free(&p->lut_stamp);
    odin3_vec_free(&p->memb);
    odin3_vec_free(&p->stack);
    odin3_vec_free(&p->old_start);
    odin3_vec_free(&p->old_cells);
    odin3_vec_free(&p->pairs);
}

static odin3_status pv_init(pv *p, odin3_abc_run *run, const odin3_abc_result *res,
                            const odin3_vec *anchors) {
    *p = (pv){0};
    p->run = run;
    p->res = res;
    p->anchors = anchors;
    p->mod = run->module;
    p->region = run->region;
    odin3_vec_init(&p->stack, sizeof(uint32_t));
    odin3_vec_init(&p->old_start, sizeof(uint32_t));
    odin3_vec_init(&p->old_cells, sizeof(uint32_t));
    odin3_vec_init(&p->pairs, sizeof(uint32_t));
    odin3_status st = zeros(&p->net_anch, odin3_module_net_end(p->mod));
    st = st == ODIN3_OK ? zeros(&p->obj_anch, obj_total(res)) : st;
    st = st == ODIN3_OK ? zeros(&p->node_stamp, odin3_module_node_end(p->mod)) : st;
    st = st == ODIN3_OK ? zeros(&p->lut_stamp, res->n_luts) : st;
    st = st == ODIN3_OK ? zeros(&p->memb, res->n_luts) : st;
    for (size_t at = 0; st == ODIN3_OK && at < anchors->len; at++) {
        const odin3_abc_anchor *an = anchor_at(p, at);
        ((uint32_t *)p->obj_anch.data)[an->obj]++;
        if (odin3_net_valid(an->old_net)) {
            ((uint32_t *)p->net_anch.data)[an->old_net.v]++;
        }
    }
    return st;
}

static int cmp_u32(const void *lhs, const void *rhs) {
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;
    return left < right ? -1 : (left > right ? 1 : 0);
}

/* True when node is a soft cell of the region. */
static bool is_soft(const pv *p, odin3_node_id node) {
    return ((const uint8_t *)p->region->role.data)[node.v] == ODIN3_ABC_ROLE_SOFT;
}

/* The soft cell driving net, or 0. */
static uint32_t soft_driver(const pv *p, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(p->mod, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(p->mod, odin3_net_driver(p->mod, net));
    return is_soft(p, node) ? node.v : 0;
}

/* Visits the soft cell `node` for anchor a: queues the soft drivers of its inputs that are not
 * another anchor's net. */
static odin3_status old_visit(pv *p, uint32_t node, uint32_t stamp) {
    ((uint32_t *)p->node_stamp.data)[node] = stamp;
    odin3_pinslice pins = odin3_node_pins(p->mod, (odin3_node_id){node});
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(p->mod, pin);
        if (!odin3_pin_reads(p->mod, pin) || !odin3_net_valid(net) ||
            u32_at(&p->net_anch, net.v) != 0) {
            continue;
        }
        uint32_t drv = soft_driver(p, net);
        if (drv != 0 && u32_at(&p->node_stamp, drv) != stamp) {
            odin3_status st = push_u32(&p->stack, drv);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* The old region of anchor a. cells (may be NULL: count only) gets its node IDs ascending; *edges
 * grows by the cells that have a record. */
static odin3_status old_region(pv *p, uint32_t a, odin3_vec *cells, uint64_t *edges) {
    const odin3_abc_anchor *an = anchor_at(p, a);
    size_t start = cells != NULL ? cells->len : 0;
    odin3_vec_clear(&p->stack);
    odin3_status st = ODIN3_OK;
    if (an->kind == ODIN3_ABC_ANCHOR_REG) {
        uint32_t reg = an->obj - FIRST_OBJ - p->res->n_cis;
        uint32_t rep = u32_at(&p->region->regs, u32_at(&p->res->reg_from, reg));
        st = push_u32(&p->stack, rep);
    } else if (an->kind == ODIN3_ABC_ANCHOR_CO || an->kind == ODIN3_ABC_ANCHOR_REG_IN) {
        uint32_t root = soft_driver(p, an->old_net);
        st = root != 0 ? push_u32(&p->stack, root) : ODIN3_OK;
    }
    uint32_t stamp = a + 1U;
    while (st == ODIN3_OK && p->stack.len > 0) {
        uint32_t node = u32_at(&p->stack, p->stack.len - 1U);
        odin3_vec_pop(&p->stack);
        if (u32_at(&p->node_stamp, node) == stamp) {
            continue;
        }
        st = an->kind == ODIN3_ABC_ANCHOR_REG ? ODIN3_OK : old_visit(p, node, stamp);
        ((uint32_t *)p->node_stamp.data)[node] = stamp;
        *edges += odin3_node_prov(p->mod, (odin3_node_id){node}).v != 0 ? 1U : 0U;
        st = st == ODIN3_OK && cells != NULL ? push_u32(cells, node) : st;
        if (*edges > COUNT_CAP) {
            break;
        }
    }
    if (st == ODIN3_OK && cells != NULL) {
        qsort((uint32_t *)cells->data + start, cells->len - start, sizeof(uint32_t), cmp_u32);
    }
    return st;
}

/* The new region of anchor a: the LUTs in the fan-in cone of its object up to other anchors'
 * objects. Counts the memberships in memb; with store, records (LUT, anchor) pairs too. */
static odin3_status new_region(pv *p, uint32_t a, bool store) {
    const odin3_abc_anchor *an = anchor_at(p, a);
    uint32_t first = odin3_abc_result_first_lut(p->res);
    odin3_vec_clear(&p->stack);
    odin3_status st = an->obj >= first ? push_u32(&p->stack, an->obj - first) : ODIN3_OK;
    while (st == ODIN3_OK && p->stack.len > 0) {
        uint32_t lut = u32_at(&p->stack, p->stack.len - 1U);
        odin3_vec_pop(&p->stack);
        if (u32_at(&p->lut_stamp, lut) == a + 1U) {
            continue;
        }
        ((uint32_t *)p->lut_stamp.data)[lut] = a + 1U;
        ((uint32_t *)p->memb.data)[lut]++;
        st = store ? push_u32(&p->pairs, lut) : ODIN3_OK;
        st = st == ODIN3_OK && store ? push_u32(&p->pairs, a) : st;
        uint32_t inputs = u32_at(&p->res->lut_k, lut);
        const uint32_t *fanins =
            (const uint32_t *)p->res->fanins.data + u32_at(&p->res->fanin_start, lut);
        for (uint32_t var = 0; st == ODIN3_OK && var < inputs; var++) {
            if (fanins[var] >= first && u32_at(&p->obj_anch, fanins[var]) == 0) {
                st = push_u32(&p->stack, fanins[var] - first);
            }
        }
    }
    return st;
}

/* All the walks: the old regions (stored when store), the new regions' memberships, and the
 * parent edges they need. */
static odin3_status walk_all(pv *p, bool store, uint64_t *edges) {
    odin3_status st = ODIN3_OK;
    *edges = 0;
    memset(p->node_stamp.data, 0, p->node_stamp.len * sizeof(uint32_t));
    memset(p->lut_stamp.data, 0, p->lut_stamp.len * sizeof(uint32_t));
    memset(p->memb.data, 0, p->memb.len * sizeof(uint32_t));
    odin3_vec_clear(&p->old_cells);
    odin3_vec_clear(&p->old_start);
    odin3_vec_clear(&p->pairs);
    for (uint32_t a = 0; st == ODIN3_OK && a < p->anchors->len; a++) {
        st = store ? push_u32(&p->old_start, (uint32_t)p->old_cells.len) : ODIN3_OK;
        st = st == ODIN3_OK ? old_region(p, a, store ? &p->old_cells : NULL, edges) : st;
        st = st == ODIN3_OK ? new_region(p, a, store) : st;
    }
    st = st == ODIN3_OK && store ? push_u32(&p->old_start, (uint32_t)p->old_cells.len) : st;
    for (size_t lut = 0; lut < p->memb.len; lut++) {
        uint32_t regions = u32_at(&p->memb, lut);
        *edges += regions >= 2 ? regions : 0;
    }
    return st;
}

/* --- records ------------------------------------------------------------------------------- */

static const char *module_text(const odin3_abc_run *run) {
    return odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module));
}

/* One DERIVED record in the context's current operation from the provenance IDs of nodes (those
 * that have one); *out stays 0 when none has. */
static odin3_status derive_from_nodes(odin3_abc_run *run, const uint32_t *nodes, size_t count,
                                      uint32_t *out) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    odin3_status st = ODIN3_OK;
    for (size_t at = 0; st == ODIN3_OK && at < count; at++) {
        odin3_prov_id prov = odin3_node_prov(run->module, (odin3_node_id){nodes[at]});
        odin3_prov_id *slot = prov.v != 0 ? odin3_vec_push(&parents) : NULL;
        if (prov.v != 0 && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else if (slot != NULL) {
            *slot = prov;
        }
    }
    if (st == ODIN3_OK && parents.len > 0) {
        odin3_prov_id rec = {0};
        st = odin3_prov_derive(run->ctx, (odin3_prov_list){parents.data, (uint32_t)parents.len},
                               &rec);
        *out = rec.v;
    }
    odin3_vec_free(&parents);
    return st;
}

/* The ascending node IDs of every old soft cell and register of the region. */
static odin3_status all_old_cells(const odin3_abc_run *run, odin3_vec *cells) {
    odin3_vec_init(cells, sizeof(uint32_t));
    const uint8_t *role = run->region->role.data;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < run->region->role.len; id++) {
        bool old = (role[id] == ODIN3_ABC_ROLE_SOFT || role[id] == ODIN3_ABC_ROLE_REG) &&
                   odin3_node_live(run->module, (odin3_node_id){id});
        st = old ? push_u32(cells, id) : st;
    }
    return st;
}

static uint64_t edge_budget(const odin3_abc_run *run, const odin3_abc_result *res) {
    uint64_t fixed = odin3_abc_run_limits_get()->prov_edges;
    if (fixed != 0) {
        return fixed;
    }
    uint64_t old_cells = run->region->soft.len + run->region->regs.len;
    uint64_t new_objs = (uint64_t)res->n_luts + res->n_regs_out;
    return ODIN3_ABC_PROV_EDGES_PER_OBJ * (old_cells + new_objs) + ODIN3_ABC_PROV_EDGES_BASE;
}

/* The over-budget form: one record for every new object. */
static odin3_status coarse_records(odin3_abc_run *run, const odin3_abc_result *res,
                                   uint64_t edges, odin3_vec *obj_prov) {
    char domain[TEXT_BYTES];
    odin3_abc_domain_text(run->module, run->domain, domain, sizeof domain);
    odin3_log(ODIN3_LOG_WARN,
              "abc: module '%s', %s: provenance needs %" PRIu64 " parent edges (budget %" PRIu64
              "); the run's LUTs share one record",
              module_text(run), domain, edges, edge_budget(run, res));
    odin3_vec cells;
    odin3_status st = all_old_cells(run, &cells);
    uint32_t rec = 0;
    if (st == ODIN3_OK) {
        odin3_prov_begin_op(run->ctx);
        st = derive_from_nodes(run, cells.data, cells.len, &rec);
    }
    odin3_vec_free(&cells);
    uint32_t first_new = FIRST_OBJ + res->n_cis;
    for (uint32_t obj = 0; st == ODIN3_OK && obj < obj_total(res); obj++) {
        st = push_u32(obj_prov, obj >= first_new ? rec : 0);
    }
    return st;
}

/* One record per anchor that has an old region with records. */
static odin3_status anchor_records(pv *p, odin3_vec *rec) {
    for (uint32_t a = 0; a < p->anchors->len; a++) {
        uint32_t from = u32_at(&p->old_start, a);
        uint32_t to = u32_at(&p->old_start, a + 1U);
        uint32_t made = 0;
        if (to > from) {
            odin3_prov_begin_op(p->run->ctx);
            odin3_status st =
                derive_from_nodes(p->run, (const uint32_t *)p->old_cells.data + from, to - from,
                                  &made);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        odin3_status st = push_u32(rec, made);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The records of the anchors the pairs list for one LUT, distinct and nonzero, in anchor order. */
static uint32_t lut_parents(const odin3_vec *rec, const uint32_t *anchors, uint32_t count,
                            odin3_prov_id *parents) {
    uint32_t found = 0;
    for (uint32_t at = 0; at < count; at++) {
        uint32_t made = u32_at(rec, anchors[at]);
        bool dup = made == 0;
        for (uint32_t i = 0; i < found; i++) {
            dup = dup || parents[i].v == made;
        }
        if (!dup) {
            parents[found++] = (odin3_prov_id){made};
        }
    }
    return found;
}

/* Groups the (LUT, anchor) pairs by LUT: members holds anchors, start the offsets. */
static odin3_status group_pairs(const pv *p, odin3_vec *members, odin3_vec *start) {
    odin3_status st = zeros(start, p->memb.len + 1U);
    st = st == ODIN3_OK ? zeros(members, p->pairs.len / 2U) : st;
    for (size_t lut = 0; st == ODIN3_OK && lut < p->memb.len; lut++) {
        ((uint32_t *)start->data)[lut + 1U] = u32_at(start, lut) + u32_at(&p->memb, lut);
    }
    odin3_vec fill;
    odin3_vec_init(&fill, sizeof(uint32_t));
    st = st == ODIN3_OK ? zeros(&fill, p->memb.len) : st;
    for (size_t at = 0; st == ODIN3_OK && at < p->pairs.len; at += 2U) {
        uint32_t lut = u32_at(&p->pairs, at);
        ((uint32_t *)members->data)[u32_at(start, lut) + u32_at(&fill, lut)] =
            u32_at(&p->pairs, at + 1U);
        ((uint32_t *)fill.data)[lut]++;
    }
    odin3_vec_free(&fill);
    return st;
}

/* The record each LUT carries: its single region's, or one more operation's for several. */
static odin3_status lut_records(pv *p, const odin3_vec *rec, odin3_vec *obj_prov) {
    odin3_vec members;
    odin3_vec start;
    odin3_vec_init(&members, sizeof(uint32_t));
    odin3_vec_init(&start, sizeof(uint32_t));
    odin3_status st = group_pairs(p, &members, &start);
    odin3_prov_begin_op(p->run->ctx);
    for (uint32_t lut = 0; st == ODIN3_OK && lut < p->res->n_luts; lut++) {
        uint32_t count = u32_at(&p->memb, lut);
        odin3_prov_id parents[PARENT_STACK];
        odin3_prov_id *list = parents;
        odin3_vec big;
        odin3_vec_init(&big, sizeof(odin3_prov_id));
        if (count > PARENT_STACK) {
            st = odin3_vec_reserve(&big, count);
            list = big.data;
        }
        uint32_t found = 0;
        if (st == ODIN3_OK && count > 0) {
            found = lut_parents(rec, (const uint32_t *)members.data + u32_at(&start, lut), count,
                                list);
        }
        uint32_t made = found == 1 ? list[0].v : 0;
        if (st == ODIN3_OK && found >= 2) {
            odin3_prov_id extra = {0};
            st = odin3_prov_derive(p->run->ctx, (odin3_prov_list){list, found}, &extra);
            made = extra.v;
        }
        odin3_vec_free(&big);
        ((uint32_t *)obj_prov->data)[odin3_abc_result_first_lut(p->res) + lut] = made;
    }
    odin3_vec_free(&members);
    odin3_vec_free(&start);
    return st;
}

odin3_status odin3_abc_records(odin3_abc_run *run, const odin3_abc_result *res,
                               const odin3_vec *anchors, odin3_vec *obj_prov) {
    pv p;
    odin3_vec rec;
    odin3_vec_init(&rec, sizeof(uint32_t));
    odin3_vec_free(obj_prov);
    odin3_status st = pv_init(&p, run, res, anchors);
    uint64_t edges = 0;
    st = st == ODIN3_OK ? walk_all(&p, false, &edges) : st;
    if (st == ODIN3_OK && edges > edge_budget(run, res)) {
        st = coarse_records(run, res, edges, obj_prov);
    } else if (st == ODIN3_OK) {
        st = walk_all(&p, true, &edges);
        st = st == ODIN3_OK ? zeros(obj_prov, obj_total(res)) : st;
        st = st == ODIN3_OK ? anchor_records(&p, &rec) : st;
        st = st == ODIN3_OK ? lut_records(&p, &rec, obj_prov) : st;
        for (uint32_t a = 0; st == ODIN3_OK && a < anchors->len; a++) {
            const odin3_abc_anchor *an = anchor_at(&p, a);
            if (an->kind == ODIN3_ABC_ANCHOR_REG) {
                ((uint32_t *)obj_prov->data)[an->obj] = u32_at(&rec, a);
            }
        }
    }
    odin3_vec_free(&rec);
    pv_free(&p);
    return st;
}
