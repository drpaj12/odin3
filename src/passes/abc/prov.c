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

odin3_celltype_id odin3_abc_lut_type(const odin3_design *design) {
    uint32_t name = 0;
    odin3_celltype_id type = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("$lut"), &name) ||
        !odin3_celltype_find(design, name, &type)) {
        return (odin3_celltype_id){0};
    }
    return type;
}

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

/* Marks the objects and nets the anchors so far use. */
static odin3_status mark_taken(const odin3_abc_run *run, const odin3_vec *anchors, odin3_vec *objs,
                               odin3_vec *nets) {
    odin3_status st = zeros(nets, odin3_module_net_end(run->module));
    for (size_t at = 0; st == ODIN3_OK && at < anchors->len; at++) {
        const odin3_abc_anchor *an = odin3_vec_cat(anchors, at);
        ((uint32_t *)objs->data)[an->obj] = 1;
        if (odin3_net_valid(an->old_net)) {
            ((uint32_t *)nets->data)[an->old_net.v] = 1;
        }
    }
    return st;
}

/* §8.2's name-map anchors: each LUT whose object ABC proved equal to literal l (old_lit) is paired
 * with the lowest-ID old internal net on l's variable, unless the LUT or the net already has an
 * anchor. The pair is complemented when the net's literal is the complement of l's. */
static odin3_status add_namemap_anchors(const odin3_abc_run *run, const odin3_abc_result *res,
                                        odin3_vec *out) {
    if (res->old_lit.len == 0 || run->aig.net_lit == NULL) {
        return ODIN3_OK;
    }
    odin3_vec objs;
    odin3_vec nets;
    odin3_vec_init(&nets, sizeof(uint32_t));
    odin3_status st = zeros(&objs, obj_total(res));
    st = st == ODIN3_OK ? mark_taken(run, out, &objs, &nets) : st;
    uint32_t total =
        obj_total(res) < res->old_lit.len ? obj_total(res) : (uint32_t)res->old_lit.len;
    for (uint32_t obj = odin3_abc_result_first_lut(res); st == ODIN3_OK && obj < total; obj++) {
        uint32_t lit = u32_at(&res->old_lit, obj);
        odin3_net_id net = {0};
        if (lit != UINT32_MAX && lit >> 1 != 0 && u32_at(&objs, obj) == 0) {
            net = odin3_abc_aig_net_of_var(&run->aig, lit >> 1);
        }
        if (!odin3_net_valid(net) || u32_at(&nets, net.v) != 0 ||
            u32_at(&run->region->ci_of, net.v) != 0 || u32_at(&run->region->co_of, net.v) != 0) {
            continue;
        }
        bool inverted = ((uint32_t)odin3_abc_aig_lit(&run->aig, net) & 1U) != (lit & 1U);
        st = add_anchor(out, obj, net, ODIN3_ABC_ANCHOR_NAMEMAP);
        if (st == ODIN3_OK) {
            ((odin3_abc_anchor *)out->data)[out->len - 1U].complemented = inverted;
            ((uint32_t *)nets.data)[net.v] = 1;
        }
    }
    odin3_vec_free(&objs);
    odin3_vec_free(&nets);
    return st;
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
    return st == ODIN3_OK ? add_namemap_anchors(run, res, out) : st;
}

/* --- the walks ----------------------------------------------------------------------------- */

typedef struct pv {
    odin3_abc_run *run;
    const odin3_abc_result *res;
    const odin3_vec *anchors;
    const odin3_module *mod;
    const odin3_abc_region *region;
    odin3_vec net_anch;   /* uint32_t per net: anchors on the net */
    odin3_vec obj_anch;   /* uint32_t per object: anchors on the object */
    odin3_vec node_stamp; /* uint32_t per node: last anchor (1 + index) that visited it */
    odin3_vec lut_stamp;  /* uint32_t per LUT */
    odin3_vec memb;       /* uint32_t per LUT: new regions it is in */
    odin3_vec stack;      /* uint32_t */
    odin3_vec old_start;  /* uint32_t per anchor + 1 */
    odin3_vec old_cells;  /* uint32_t node IDs, anchor after anchor, each ascending */
    odin3_vec pairs;      /* uint32_t (lut, anchor) pairs */
} pv;

static const odin3_abc_anchor *anchor_at(const pv *pw, size_t idx) {
    return odin3_vec_cat(pw->anchors, idx);
}

static void pv_free(pv *pw) {
    odin3_vec_free(&pw->net_anch);
    odin3_vec_free(&pw->obj_anch);
    odin3_vec_free(&pw->node_stamp);
    odin3_vec_free(&pw->lut_stamp);
    odin3_vec_free(&pw->memb);
    odin3_vec_free(&pw->stack);
    odin3_vec_free(&pw->old_start);
    odin3_vec_free(&pw->old_cells);
    odin3_vec_free(&pw->pairs);
}

static odin3_status pv_init(pv *pw, odin3_abc_run *run, const odin3_abc_result *res,
                            const odin3_vec *anchors) {
    *pw = (pv){0};
    pw->run = run;
    pw->res = res;
    pw->anchors = anchors;
    pw->mod = run->module;
    pw->region = run->region;
    odin3_vec_init(&pw->stack, sizeof(uint32_t));
    odin3_vec_init(&pw->old_start, sizeof(uint32_t));
    odin3_vec_init(&pw->old_cells, sizeof(uint32_t));
    odin3_vec_init(&pw->pairs, sizeof(uint32_t));
    odin3_status st = zeros(&pw->net_anch, odin3_module_net_end(pw->mod));
    st = st == ODIN3_OK ? zeros(&pw->obj_anch, obj_total(res)) : st;
    st = st == ODIN3_OK ? zeros(&pw->node_stamp, odin3_module_node_end(pw->mod)) : st;
    st = st == ODIN3_OK ? zeros(&pw->lut_stamp, res->n_luts) : st;
    st = st == ODIN3_OK ? zeros(&pw->memb, res->n_luts) : st;
    for (size_t at = 0; st == ODIN3_OK && at < anchors->len; at++) {
        const odin3_abc_anchor *an = anchor_at(pw, at);
        ((uint32_t *)pw->obj_anch.data)[an->obj]++;
        if (odin3_net_valid(an->old_net)) {
            ((uint32_t *)pw->net_anch.data)[an->old_net.v]++;
        }
    }
    return st;
}

static int cmp_u32(const void *lhs, const void *rhs) {
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;
    if (left != right) {
        return left < right ? -1 : 1;
    }
    return 0;
}

/* True when node is a soft cell of the region. */
static bool is_soft(const pv *pw, odin3_node_id node) {
    return ((const uint8_t *)pw->region->role.data)[node.v] == ODIN3_ABC_ROLE_SOFT;
}

/* The soft cell driving net, or 0. */
static uint32_t soft_driver(const pv *pw, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(pw->mod, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(pw->mod, odin3_net_driver(pw->mod, net));
    return is_soft(pw, node) ? node.v : 0;
}

/* Visits the soft cell `node` for anchor a: queues the soft drivers of its inputs that are not
 * another anchor's net. */
static odin3_status old_visit(pv *pw, uint32_t node, uint32_t stamp) {
    ((uint32_t *)pw->node_stamp.data)[node] = stamp;
    odin3_pinslice pins = odin3_node_pins(pw->mod, (odin3_node_id){node});
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(pw->mod, pin);
        if (!odin3_pin_reads(pw->mod, pin) || !odin3_net_valid(net) ||
            u32_at(&pw->net_anch, net.v) != 0) {
            continue;
        }
        uint32_t drv = soft_driver(pw, net);
        if (drv != 0 && u32_at(&pw->node_stamp, drv) != stamp) {
            odin3_status st = push_u32(&pw->stack, drv);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* Queues the first cell of anchor a's old region: a register anchor's flip-flop, or the soft cell
 * that drives a CO's or a register input's old net. */
static odin3_status old_seed(pv *pw, const odin3_abc_anchor *an) {
    uint32_t root = 0;
    if (an->kind == ODIN3_ABC_ANCHOR_REG) {
        uint32_t reg = an->obj - FIRST_OBJ - pw->res->n_cis;
        root = u32_at(&pw->region->regs, u32_at(&pw->res->reg_from, reg));
    } else if (an->kind == ODIN3_ABC_ANCHOR_CO || an->kind == ODIN3_ABC_ANCHOR_REG_IN ||
               an->kind == ODIN3_ABC_ANCHOR_NAMEMAP) {
        root = soft_driver(pw, an->old_net);
    }
    return root != 0 ? push_u32(&pw->stack, root) : ODIN3_OK;
}

/* Walks the queued cells for anchor a (stamp a + 1), counting the ones with a record. */
static odin3_status old_walk(pv *pw, uint32_t anc, odin3_vec *cells, uint64_t *edges) {
    const bool single = anchor_at(pw, anc)->kind == ODIN3_ABC_ANCHOR_REG;
    uint32_t stamp = anc + 1U;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && pw->stack.len > 0 && *edges <= COUNT_CAP) {
        uint32_t node = u32_at(&pw->stack, pw->stack.len - 1U);
        odin3_vec_pop(&pw->stack);
        if (u32_at(&pw->node_stamp, node) == stamp) {
            continue;
        }
        st = single ? ODIN3_OK : old_visit(pw, node, stamp);
        ((uint32_t *)pw->node_stamp.data)[node] = stamp;
        *edges += odin3_node_prov(pw->mod, (odin3_node_id){node}).v != 0 ? 1U : 0U;
        st = st == ODIN3_OK && cells != NULL ? push_u32(cells, node) : st;
    }
    return st;
}

/* The old region of anchor a. cells (may be NULL: count only) gets its node IDs ascending; *edges
 * grows by the cells that have a record. */
static odin3_status old_region(pv *pw, uint32_t anc, odin3_vec *cells, uint64_t *edges) {
    size_t start = cells != NULL ? cells->len : 0;
    odin3_vec_clear(&pw->stack);
    odin3_status st = old_seed(pw, anchor_at(pw, anc));
    st = st == ODIN3_OK ? old_walk(pw, anc, cells, edges) : st;
    if (st == ODIN3_OK && cells != NULL && cells->len - start > 1) {
        qsort((uint32_t *)cells->data + start, cells->len - start, sizeof(uint32_t), cmp_u32);
    }
    return st;
}

/* One LUT of anchor anc's new region: counted (and paired with anc when store), then its fanins
 * that are no other anchor's object are queued. */
static odin3_status new_visit(pv *pw, uint32_t lut, uint32_t anc, bool store) {
    uint32_t first = odin3_abc_result_first_lut(pw->res);
    ((uint32_t *)pw->lut_stamp.data)[lut] = anc + 1U;
    ((uint32_t *)pw->memb.data)[lut]++;
    odin3_status st = store ? push_u32(&pw->pairs, lut) : ODIN3_OK;
    st = st == ODIN3_OK && store ? push_u32(&pw->pairs, anc) : st;
    uint32_t inputs = u32_at(&pw->res->lut_k, lut);
    const uint32_t *fanins =
        (const uint32_t *)pw->res->fanins.data + u32_at(&pw->res->fanin_start, lut);
    for (uint32_t var = 0; st == ODIN3_OK && var < inputs; var++) {
        if (fanins[var] >= first && u32_at(&pw->obj_anch, fanins[var]) == 0) {
            st = push_u32(&pw->stack, fanins[var] - first);
        }
    }
    return st;
}

/* The new region of anchor anc: the LUTs in the fan-in cone of its object up to other anchors'
 * objects. Counts the memberships in memb; with store, records (LUT, anchor) pairs too. */
static odin3_status new_region(pv *pw, uint32_t anc, bool store) {
    const odin3_abc_anchor *an = anchor_at(pw, anc);
    uint32_t first = odin3_abc_result_first_lut(pw->res);
    odin3_vec_clear(&pw->stack);
    odin3_status st = an->obj >= first ? push_u32(&pw->stack, an->obj - first) : ODIN3_OK;
    while (st == ODIN3_OK && pw->stack.len > 0) {
        uint32_t lut = u32_at(&pw->stack, pw->stack.len - 1U);
        odin3_vec_pop(&pw->stack);
        if (u32_at(&pw->lut_stamp, lut) != anc + 1U) {
            st = new_visit(pw, lut, anc, store);
        }
    }
    return st;
}

/* Clears the stamps, counts and stored lists of a walk. */
static void walk_reset(pv *pw) {
    memset(pw->node_stamp.data, 0, pw->node_stamp.len * sizeof(uint32_t));
    memset(pw->lut_stamp.data, 0, pw->lut_stamp.len * sizeof(uint32_t));
    memset(pw->memb.data, 0, pw->memb.len * sizeof(uint32_t));
    odin3_vec_clear(&pw->old_cells);
    odin3_vec_clear(&pw->old_start);
    odin3_vec_clear(&pw->pairs);
}

/* The parent edges of the extra records: one per region of each LUT that is in several. */
static uint64_t shared_edges(const pv *pw) {
    uint64_t edges = 0;
    for (size_t lut = 0; lut < pw->memb.len; lut++) {
        uint32_t regions = u32_at(&pw->memb, lut);
        edges += regions >= 2 ? regions : 0;
    }
    return edges;
}

/* All the walks: the old regions (stored when store), the new regions' memberships, and the
 * parent edges they need. */
static odin3_status walk_all(pv *pw, bool store, uint64_t *edges) {
    odin3_status st = ODIN3_OK;
    *edges = 0;
    walk_reset(pw);
    for (uint32_t anc = 0; st == ODIN3_OK && anc < pw->anchors->len; anc++) {
        st = store ? push_u32(&pw->old_start, (uint32_t)pw->old_cells.len) : ODIN3_OK;
        st = st == ODIN3_OK ? old_region(pw, anc, store ? &pw->old_cells : NULL, edges) : st;
        st = st == ODIN3_OK ? new_region(pw, anc, store) : st;
    }
    st = st == ODIN3_OK && store ? push_u32(&pw->old_start, (uint32_t)pw->old_cells.len) : st;
    *edges += shared_edges(pw);
    return st;
}

/* --- records ------------------------------------------------------------------------------- */

static const char *module_text(const odin3_abc_run *run) {
    return odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module));
}

/* One DERIVED record in the context's current operation from the provenance IDs of nodes (those
 * that have one); *out stays 0 when none has. */
static odin3_status derive_from_nodes(const odin3_abc_run *run, const uint32_t *nodes, size_t count,
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

odin3_status odin3_abc_coarse_record(odin3_abc_run *run, odin3_prov_id *out) {
    odin3_vec cells;
    uint32_t rec = 0;
    odin3_status st = all_old_cells(run, &cells);
    if (st == ODIN3_OK) {
        odin3_prov_begin_op(run->ctx);
        st = derive_from_nodes(run, cells.data, cells.len, &rec);
    }
    odin3_vec_free(&cells);
    *out = (odin3_prov_id){rec};
    return st;
}

/* The over-budget form: one record for every new object. */
static odin3_status coarse_records(odin3_abc_run *run, const odin3_abc_result *res, uint64_t edges,
                                   odin3_vec *obj_prov) {
    char domain[TEXT_BYTES];
    odin3_abc_domain_text(run->module, run->domain, domain, sizeof domain);
    odin3_log(ODIN3_LOG_WARN,
              "abc: module '%s', %s: provenance needs %" PRIu64 " parent edges (budget %" PRIu64
              "); the run's LUTs share one record",
              module_text(run), domain, edges, edge_budget(run, res));
    odin3_prov_id rec = {0};
    odin3_status st = odin3_abc_coarse_record(run, &rec);
    uint32_t first_new = FIRST_OBJ + res->n_cis;
    for (uint32_t obj = 0; st == ODIN3_OK && obj < obj_total(res); obj++) {
        st = push_u32(obj_prov, obj >= first_new ? rec.v : 0);
    }
    return st;
}

/* One record per anchor that has an old region with records. */
static odin3_status anchor_records(pv *pw, odin3_vec *rec) {
    for (uint32_t anc = 0; anc < pw->anchors->len; anc++) {
        uint32_t from = u32_at(&pw->old_start, anc);
        uint32_t to = u32_at(&pw->old_start, anc + 1U);
        uint32_t made = 0;
        if (to > from) {
            odin3_prov_begin_op(pw->run->ctx);
            odin3_status st = derive_from_nodes(
                pw->run, (const uint32_t *)pw->old_cells.data + from, to - from, &made);
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
static odin3_status group_pairs(const pv *pw, odin3_vec *members, odin3_vec *start) {
    odin3_status st = zeros(start, pw->memb.len + 1U);
    st = st == ODIN3_OK ? zeros(members, pw->pairs.len / 2U) : st;
    for (size_t lut = 0; st == ODIN3_OK && lut < pw->memb.len; lut++) {
        ((uint32_t *)start->data)[lut + 1U] = u32_at(start, lut) + u32_at(&pw->memb, lut);
    }
    odin3_vec fill;
    odin3_vec_init(&fill, sizeof(uint32_t));
    st = st == ODIN3_OK ? zeros(&fill, pw->memb.len) : st;
    for (size_t at = 0; st == ODIN3_OK && at < pw->pairs.len; at += 2U) {
        uint32_t lut = u32_at(&pw->pairs, at);
        ((uint32_t *)members->data)[u32_at(start, lut) + u32_at(&fill, lut)] =
            u32_at(&pw->pairs, at + 1U);
        ((uint32_t *)fill.data)[lut]++;
    }
    odin3_vec_free(&fill);
    return st;
}

/* The record each LUT carries: its single region's, or one more operation's for several. */
static odin3_status lut_records(pv *pw, const odin3_vec *rec, odin3_vec *obj_prov) {
    odin3_vec members;
    odin3_vec start;
    odin3_vec_init(&members, sizeof(uint32_t));
    odin3_vec_init(&start, sizeof(uint32_t));
    odin3_status st = group_pairs(pw, &members, &start);
    odin3_prov_begin_op(pw->run->ctx);
    for (uint32_t lut = 0; st == ODIN3_OK && lut < pw->res->n_luts; lut++) {
        uint32_t count = u32_at(&pw->memb, lut);
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
            found =
                lut_parents(rec, (const uint32_t *)members.data + u32_at(&start, lut), count, list);
        }
        uint32_t made = found == 1 ? list[0].v : 0;
        if (st == ODIN3_OK && found >= 2) {
            odin3_prov_id extra = {0};
            st = odin3_prov_derive(pw->run->ctx, (odin3_prov_list){list, found}, &extra);
            made = extra.v;
        }
        odin3_vec_free(&big);
        ((uint32_t *)obj_prov->data)[odin3_abc_result_first_lut(pw->res) + lut] = made;
    }
    odin3_vec_free(&members);
    odin3_vec_free(&start);
    return st;
}

/* The precise form: one record per anchor, one per shared LUT; a register object carries its
 * anchor's record. */
static odin3_status precise_records(pv *pw, odin3_vec *obj_prov) {
    odin3_vec rec;
    odin3_vec_init(&rec, sizeof(uint32_t));
    uint64_t edges = 0;
    odin3_status st = walk_all(pw, true, &edges);
    st = st == ODIN3_OK ? zeros(obj_prov, obj_total(pw->res)) : st;
    st = st == ODIN3_OK ? anchor_records(pw, &rec) : st;
    st = st == ODIN3_OK ? lut_records(pw, &rec, obj_prov) : st;
    for (uint32_t anc = 0; st == ODIN3_OK && anc < pw->anchors->len; anc++) {
        const odin3_abc_anchor *an = anchor_at(pw, anc);
        if (an->kind == ODIN3_ABC_ANCHOR_REG) {
            ((uint32_t *)obj_prov->data)[an->obj] = u32_at(&rec, anc);
        }
    }
    odin3_vec_free(&rec);
    return st;
}

odin3_status odin3_abc_records(odin3_abc_run *run, const odin3_abc_result *res,
                               const odin3_vec *anchors, odin3_vec *obj_prov) {
    pv pw;
    odin3_vec_free(obj_prov);
    odin3_status st = pv_init(&pw, run, res, anchors);
    uint64_t edges = 0;
    st = st == ODIN3_OK ? walk_all(&pw, false, &edges) : st;
    run->stats.edges = edges;
    if (st == ODIN3_OK) {
        st = edges > edge_budget(run, res) ? coarse_records(run, res, edges, obj_prov)
                                           : precise_records(&pw, obj_prov);
    }
    pv_free(&pw);
    return st;
}
