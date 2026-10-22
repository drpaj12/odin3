/*
 * ram_group.c — which hard RAM cells are one memory (R-5 to R-7). Cells with the same origin (the
 * same non-zero provenance record: partial_map gives every piece of one memory one record) and the
 * same control nets are width slices of it; slices of a memory with identical write nets are
 * replicas built for more read ports. Equal control nets with no common origin are only a
 * candidate: nothing is merged and the cells carry `unmap_cand`. The order is by node ID and
 * index everywhere: no pointers, no hash order.
 */
#include "passes/unmap/ram.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- a stable merge sort of indices with a context comparator ------------------------------ */

enum { UM_CAND_CAP = 64, UM_DECIMAL = 10 };

/* A half-open range of positions. */
typedef struct um_range {
    size_t first;
    size_t last;
} um_range;

typedef int (*um_cmp)(const void *ctx, uint32_t left, uint32_t right);

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* One merge pass of the sort: `src` runs of `width` merged pairwise into `dst`. */
typedef struct um_merge {
    uint32_t *src;
    uint32_t *dst;
    size_t count;
    um_cmp cmp;
    const void *ctx;
} um_merge;

static void merge_pair(const um_merge *mg, um_range left, um_range right) {
    size_t li = left.first;
    size_t ri = right.first;
    size_t out = left.first;
    while (li < left.last && ri < right.last) {
        bool take_right = mg->cmp(mg->ctx, mg->src[ri], mg->src[li]) < 0;
        mg->dst[out++] = take_right ? mg->src[ri++] : mg->src[li++];
    }
    while (li < left.last) {
        mg->dst[out++] = mg->src[li++];
    }
    while (ri < right.last) {
        mg->dst[out++] = mg->src[ri++];
    }
}

static void merge_pass(const um_merge *mg, size_t width) {
    for (size_t lo = 0; lo < mg->count; lo += 2 * width) {
        size_t mid = lo + width < mg->count ? lo + width : mg->count;
        size_t hi = lo + 2 * width < mg->count ? lo + 2 * width : mg->count;
        merge_pair(mg, (um_range){lo, mid}, (um_range){mid, hi});
    }
}

/* Sorts the uint32 values of `idx` by cmp (stable: equal values keep their order). */
static odin3_status sort_idx(odin3_vec *idx, um_cmp cmp, const void *ctx) {
    size_t count = idx->len;
    odin3_vec tmp;
    odin3_vec_init(&tmp, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(&tmp, count);
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = push_u32(&tmp, 0);
    }
    um_merge mg = {idx->data, tmp.data, count, cmp, ctx};
    for (size_t width = 1; st == ODIN3_OK && width < count; width *= 2) {
        merge_pass(&mg, width);
        uint32_t *swap = mg.src;
        mg.src = mg.dst;
        mg.dst = swap;
    }
    if (st == ODIN3_OK && mg.src != idx->data) {
        memcpy(idx->data, mg.src, count * sizeof(uint32_t));
    }
    odin3_vec_free(&tmp);
    return st;
}

/* --- comparing cells ----------------------------------------------------------------------- */

typedef struct um_gctx {
    const um_ctx *uc;
    const um_ram *cells;
} um_gctx;

static int sign_of(int64_t left, int64_t right) {
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* Two cells whose pins are compared. */
typedef struct um_nodes {
    odin3_node_id left;
    odin3_node_id right;
} um_nodes;

/* The nets of one port of two cells: length first, then net by net. */
static int cmp_nets(const um_ctx *uc, um_nodes nodes, uint32_t port) {
    odin3_pinslice lp = odin3_node_port(uc->module, nodes.left, port);
    odin3_pinslice rp = odin3_node_port(uc->module, nodes.right, port);
    int order = sign_of(lp.count, rp.count);
    for (uint32_t i = 0; order == 0 && i < lp.count; i++) {
        order = sign_of(um_port_net(uc->module, nodes.left, (um_pos){port, i}).v,
                        um_port_net(uc->module, nodes.right, (um_pos){port, i}).v);
    }
    return order;
}

/* The pin numbers of the hard cells, per port (as ram.c). */
typedef struct um_pins {
    uint32_t n_ports;
    uint32_t data[UM_RAM_PORTS];
    uint32_t addr[UM_RAM_PORTS];
    uint32_t we[UM_RAM_PORTS];
} um_pins;

static const um_pins k_single_pins = {1, {1, 0}, {2, 0}, {3, 0}};
static const um_pins k_dual_pins = {2, {2, 1}, {4, 3}, {6, 5}};

static const um_pins *pins_of(const um_ram *cell) {
    return cell->dual ? &k_dual_pins : &k_single_pins;
}

/* The structure the two summaries must share to be one memory. */
static int cmp_shape(const um_ram *left, const um_ram *right) {
    int order = sign_of(left->dual, right->dual);
    order = order != 0 ? order : sign_of(left->abits, right->abits);
    for (uint32_t i = 0; order == 0 && i < UM_RAM_PORTS; i++) {
        order = sign_of(left->read[i], right->read[i]);
        order = order != 0 ? order : sign_of(left->write[i], right->write[i]);
    }
    return order;
}

/* R-6: the clock, and the address and write enable of every port. */
static int cmp_control(const um_ctx *uc, const um_ram *left, const um_ram *right) {
    int order = cmp_shape(left, right);
    um_nodes nodes = {left->node, right->node};
    const um_pins *pins = pins_of(left);
    order = order != 0 ? order : cmp_nets(uc, nodes, 0);
    for (uint32_t i = 0; order == 0 && i < pins->n_ports; i++) {
        order = cmp_nets(uc, nodes, pins->addr[i]);
        order = order != 0 ? order : cmp_nets(uc, nodes, pins->we[i]);
    }
    return order;
}

static uint32_t origin_of(const um_ctx *uc, const um_ram *cell) {
    return odin3_node_prov(uc->module, cell->node).v;
}

/* Step 1 sorts by control, then origin, then node order. */
static int cmp_cells(const void *ctx, uint32_t left, uint32_t right) {
    const um_gctx *gc = ctx;
    const um_ram *lc = &gc->cells[left];
    const um_ram *rc = &gc->cells[right];
    int order = cmp_control(gc->uc, lc, rc);
    order = order != 0 ? order : sign_of(origin_of(gc->uc, lc), origin_of(gc->uc, rc));
    return order != 0 ? order : sign_of(left, right);
}

/* Cells of a run of equal control, equal-origin ones adjacent. */
static int cmp_control_only(const void *ctx, uint32_t left, uint32_t right) {
    const um_gctx *gc = ctx;
    return cmp_control(gc->uc, &gc->cells[left], &gc->cells[right]);
}

/* --- the sets ------------------------------------------------------------------------------ */

typedef struct um_wgroup {
    odin3_vec members; /* uint32_t: cell indices, ascending */
    uint32_t origin;   /* 0 when none */
    bool candidate;    /* in a candidate set of R-6 */
} um_wgroup;

typedef struct um_candset {
    odin3_vec members; /* uint32_t: cell indices */
    uint32_t lowest;
    const char *why;
} um_candset;

static void wgroups_free(odin3_vec *wgs) {
    um_wgroup *items = wgs->data;
    for (size_t i = 0; i < wgs->len; i++) {
        odin3_vec_free(&items[i].members);
    }
    odin3_vec_free(wgs);
}

static void candsets_free(odin3_vec *sets) {
    um_candset *items = sets->data;
    for (size_t i = 0; i < sets->len; i++) {
        odin3_vec_free(&items[i].members);
    }
    odin3_vec_free(sets);
}

/* Adds a width group to the list, holding the cells idx[from .. to). */
static odin3_status add_wgroup(odin3_vec *wgs, const odin3_vec *idx, um_range range,
                               uint32_t origin) {
    um_wgroup *group = odin3_vec_push(wgs);
    if (group == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(group, 0, sizeof *group);
    odin3_vec_init(&group->members, sizeof(uint32_t));
    group->origin = origin;
    const uint32_t *items = idx->data;
    for (size_t i = range.first; i < range.last; i++) {
        odin3_status st = push_u32(&group->members, items[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A candidate set made of the width groups wgs[from ..): every member cell, the groups marked. */
static odin3_status add_candset(odin3_vec *wgs, size_t from, odin3_vec *sets, const char *why) {
    um_candset *set = odin3_vec_push(sets);
    if (set == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(set, 0, sizeof *set);
    odin3_vec_init(&set->members, sizeof(uint32_t));
    set->why = why;
    set->lowest = UINT32_MAX;
    um_wgroup *groups = wgs->data;
    for (size_t gi = from; gi < wgs->len; gi++) {
        groups[gi].candidate = true;
        const uint32_t *cells = groups[gi].members.data;
        for (size_t i = 0; i < groups[gi].members.len; i++) {
            set->lowest = cells[i] < set->lowest ? cells[i] : set->lowest;
            odin3_status st = push_u32(&set->members, cells[i]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* The data bit `<n>` of the role "<type>:b<n>[.path]" that partial_map wrote on the cell; false
 * when the cell has no such role (a BLIF, or a cell of another tool). */
static bool role_bit(const um_ctx *uc, odin3_node_id node, uint32_t *bit) {
    const char *attr = um_attr_text(uc, node, "hardmap");
    const char *colon = attr != NULL ? strchr(attr, ':') : NULL;
    if (colon == NULL || colon[1] != 'b' || colon[2] < '0' || colon[2] > '9') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(colon + 2, &end, UM_DECIMAL);
    *bit = (uint32_t)value;
    return value <= UINT32_MAX;
}

/* R-6: the slices of a group go in the order of their roles `b<n>` when every one has it, else in
 * node-ID order (the order they are in). A stable insertion sort: the groups are small. */
static void order_by_role(const um_gctx *gc, um_wgroup *group) {
    uint32_t *cells = group->members.data;
    size_t count = group->members.len;
    for (size_t i = 0; i < count; i++) {
        uint32_t unused = 0;
        if (!role_bit(gc->uc, gc->cells[cells[i]].node, &unused)) {
            return;
        }
    }
    for (size_t i = 1; i < count; i++) {
        uint32_t held = cells[i];
        uint32_t key = 0;
        (void)role_bit(gc->uc, gc->cells[held].node, &key);
        size_t at = i;
        for (; at > 0; at--) {
            uint32_t before = 0;
            (void)role_bit(gc->uc, gc->cells[cells[at - 1]].node, &before);
            if (before <= key) {
                break;
            }
            cells[at] = cells[at - 1];
        }
        cells[at] = held;
    }
}

/* The units of one run idx[run] of equal control: each becomes a width group. */
static odin3_status run_units(const um_gctx *gc, const odin3_vec *idx, um_range run,
                              odin3_vec *wgs) {
    const uint32_t *items = idx->data;
    odin3_status st = ODIN3_OK;
    size_t unit = run.first;
    while (st == ODIN3_OK && unit < run.last) {
        uint32_t origin = origin_of(gc->uc, &gc->cells[items[unit]]);
        size_t end = unit + 1;
        while (origin != 0 && end < run.last &&
               origin_of(gc->uc, &gc->cells[items[end]]) == origin) {
            end++;
        }
        st = add_wgroup(wgs, idx, (um_range){unit, end}, origin);
        if (st == ODIN3_OK) {
            order_by_role(gc, (um_wgroup *)wgs->data + (wgs->len - 1));
        }
        unit = end;
    }
    return st;
}

/* R-6: width slices. The cells sorted by control, origin and node order; a run of equal control
 * holds the units (equal non-zero origin, or one cell with none); two units or more in a run have
 * no common origin and are only a candidate. */
static odin3_status width_groups(const um_gctx *gc, size_t count, odin3_vec *wgs, odin3_vec *sets) {
    odin3_vec idx;
    odin3_vec_init(&idx, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = push_u32(&idx, (uint32_t)i);
    }
    st = st == ODIN3_OK ? sort_idx(&idx, cmp_cells, gc) : st;
    const uint32_t *items = idx.data;
    size_t from = 0;
    while (st == ODIN3_OK && from < count) {
        size_t to = from + 1;
        while (to < count && cmp_control_only(gc, items[from], items[to]) == 0) {
            to++;
        }
        size_t first_unit = wgs->len;
        st = run_units(gc, &idx, (um_range){from, to}, wgs);
        if (st == ODIN3_OK && wgs->len - first_unit >= 2) {
            st = add_candset(wgs, first_unit, sets, "control");
        }
        from = to;
    }
    odin3_vec_free(&idx);
    return st;
}

/* --- R-7: replicas ------------------------------------------------------------------------- */

typedef struct um_rctx {
    const um_gctx *gc;
    const um_wgroup *wgs;
} um_rctx;

static uint32_t total_of(const um_gctx *gc, const um_wgroup *group) {
    uint32_t width = 0;
    const uint32_t *cells = group->members.data;
    for (size_t i = 0; i < group->members.len; i++) {
        width += gc->cells[cells[i]].width;
    }
    return width;
}

static bool has_write(const um_ram *cell) {
    return cell->write[0] || cell->write[1];
}

/* The write side of two slices: clock, and the write enable, address and data of each writer. */
static int cmp_write(const um_ctx *uc, const um_ram *left, const um_ram *right) {
    um_nodes nodes = {left->node, right->node};
    const um_pins *pins = pins_of(left);
    int order = cmp_nets(uc, nodes, 0);
    for (uint32_t i = 0; order == 0 && i < pins->n_ports; i++) {
        if (!left->write[i]) {
            continue;
        }
        order = cmp_nets(uc, nodes, pins->we[i]);
        order = order != 0 ? order : cmp_nets(uc, nodes, pins->addr[i]);
        order = order != 0 ? order : cmp_nets(uc, nodes, pins->data[i]);
    }
    return order;
}

/* What replicas share: shape, width, slice count and every slice's write side; a memory that
 * is never written has nothing to compare but its origin. */
typedef struct um_wpair {
    const um_wgroup *left;
    const um_wgroup *right;
} um_wpair;

static int cmp_replica_key(const um_rctx *rc, um_wpair pair) {
    const um_wgroup *lg = pair.left;
    const um_wgroup *rg = pair.right;
    const um_ctx *uc = rc->gc->uc;
    const uint32_t *lm = lg->members.data;
    const uint32_t *rm = rg->members.data;
    int order = sign_of((int64_t)lg->members.len, (int64_t)rg->members.len);
    order = order != 0 ? order : cmp_shape(&rc->gc->cells[lm[0]], &rc->gc->cells[rm[0]]);
    order = order != 0 ? order : sign_of(total_of(rc->gc, lg), total_of(rc->gc, rg));
    for (size_t i = 0; order == 0 && i < lg->members.len; i++) {
        order = cmp_write(uc, &rc->gc->cells[lm[i]], &rc->gc->cells[rm[i]]);
    }
    if (order == 0 && !has_write(&rc->gc->cells[lm[0]])) {
        order = sign_of(lg->origin, rg->origin);
    }
    return order;
}

static int cmp_replicas(const void *ctx, uint32_t left, uint32_t right) {
    const um_rctx *rc = ctx;
    int order = cmp_replica_key(rc, (um_wpair){&rc->wgs[left], &rc->wgs[right]});
    order = order != 0 ? order : sign_of(rc->wgs[left].origin, rc->wgs[right].origin);
    return order != 0 ? order
                      : sign_of(*(const uint32_t *)rc->wgs[left].members.data,
                                *(const uint32_t *)rc->wgs[right].members.data);
}

/* One final group: the width groups (by index) that are its replicas. */
static odin3_status add_final(odin3_vec *finals, const uint32_t *ids, um_range range) {
    odin3_vec *item = odin3_vec_push(finals);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(item, sizeof(uint32_t));
    for (size_t i = range.first; i < range.last; i++) {
        odin3_status st = push_u32(item, ids[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The number of units in a run: width groups of one non-zero origin, or one group with none. */
static size_t count_units(const um_rctx *rc, const uint32_t *ids, um_range run) {
    size_t units = 0;
    for (size_t i = run.first; i < run.last; i++) {
        uint32_t origin = rc->wgs[ids[i]].origin;
        bool same = i > run.first && origin != 0 && rc->wgs[ids[i - 1]].origin == origin;
        units += same ? 0 : 1;
    }
    return units;
}

/* A candidate set of every cell of the run's width groups. */
static odin3_status replica_candset(const um_rctx *rc, const uint32_t *ids, um_range run,
                                    odin3_vec *sets) {
    um_candset *set = odin3_vec_push(sets);
    if (set == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(set, 0, sizeof *set);
    odin3_vec_init(&set->members, sizeof(uint32_t));
    set->why = "write";
    set->lowest = UINT32_MAX;
    for (size_t gi = run.first; gi < run.last; gi++) {
        const um_wgroup *group = &rc->wgs[ids[gi]];
        const uint32_t *cells = group->members.data;
        for (size_t i = 0; i < group->members.len; i++) {
            set->lowest = cells[i] < set->lowest ? cells[i] : set->lowest;
            odin3_status st = push_u32(&set->members, cells[i]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* Where the results go: the final groups (each a vec of width group indices) and the sets. */
typedef struct um_out {
    odin3_vec *finals;
    odin3_vec *sets;
} um_out;

static odin3_status add_final_single(odin3_vec *finals, size_t index) {
    uint32_t id = (uint32_t)index;
    return add_final(finals, &id, (um_range){0, 1});
}

/* A run of width groups with equal replica key: units that share an origin are merged; with a
 * write side and two units or more nothing says they are one memory (a candidate set). */
static odin3_status replica_run(const um_rctx *rc, const uint32_t *ids, um_range run, um_out out) {
    const um_ram *probe = &rc->gc->cells[*(const uint32_t *)rc->wgs[ids[run.first]].members.data];
    bool ambiguous = has_write(probe) && count_units(rc, ids, run) >= 2;
    odin3_status st = ambiguous ? replica_candset(rc, ids, run, out.sets) : ODIN3_OK;
    size_t unit = run.first;
    while (st == ODIN3_OK && unit < run.last) {
        uint32_t origin = rc->wgs[ids[unit]].origin;
        size_t end = unit + 1;
        while (!ambiguous && origin != 0 && end < run.last && rc->wgs[ids[end]].origin == origin) {
            end++;
        }
        st = add_final(out.finals, ids, (um_range){unit, end});
        unit = end;
    }
    return st;
}

/* R-7 over the width groups that are not candidates; finals get one entry per merged unit. */
static odin3_status replica_groups(const um_gctx *gc, const odin3_vec *wgs, um_out out) {
    um_rctx rc = {gc, wgs->data};
    const um_wgroup *groups = wgs->data;
    odin3_vec ids;
    odin3_vec_init(&ids, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < wgs->len; i++) {
        /* a candidate group of R-6 is its own memory */
        st = groups[i].candidate ? add_final_single(out.finals, i) : push_u32(&ids, (uint32_t)i);
    }
    st = st == ODIN3_OK ? sort_idx(&ids, cmp_replicas, &rc) : st;
    const uint32_t *sorted = ids.data;
    size_t from = 0;
    while (st == ODIN3_OK && from < ids.len) {
        size_t to = from + 1;
        while (to < ids.len &&
               cmp_replica_key(&rc, (um_wpair){&groups[sorted[from]], &groups[sorted[to]]}) == 0) {
            to++;
        }
        st = replica_run(&rc, sorted, (um_range){from, to}, out);
        from = to;
    }
    odin3_vec_free(&ids);
    return st;
}

/* --- the result ---------------------------------------------------------------------------- */

typedef struct um_octx {
    const odin3_vec *finals; /* odin3_vec of uint32_t: width group indices */
    const odin3_vec *wgs;
    const odin3_vec *sets;
} um_octx;

static uint32_t first_cell_of(const um_octx *oc, uint32_t final_index) {
    const odin3_vec *final = (const odin3_vec *)oc->finals->data + final_index;
    uint32_t group = *(const uint32_t *) final->data;
    const um_wgroup *wg = (const um_wgroup *)oc->wgs->data + group;
    return *(const uint32_t *)wg->members.data;
}

static int cmp_finals(const void *ctx, uint32_t left, uint32_t right) {
    const um_octx *oc = ctx;
    return sign_of(first_cell_of(oc, left), first_cell_of(oc, right));
}

static int cmp_sets(const void *ctx, uint32_t left, uint32_t right) {
    const um_octx *oc = ctx;
    const um_candset *items = oc->sets->data;
    return sign_of(items[left].lowest, items[right].lowest);
}

/* P-3: `unmap_cand` = ram:N on the cells of every candidate set (N in node-ID order of the lowest
 * member) and one INFO line each. */
static odin3_status mark_candidates(um_ctx *uc, const um_gctx *gc, const odin3_vec *sets) {
    odin3_vec order;
    odin3_vec_init(&order, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sets->len; i++) {
        st = push_u32(&order, (uint32_t)i);
    }
    um_octx oc = {NULL, NULL, sets};
    st = st == ODIN3_OK ? sort_idx(&order, cmp_sets, &oc) : st;
    const um_candset *items = sets->data;
    const uint32_t *ranked = order.data;
    for (size_t rank = 0; st == ODIN3_OK && rank < order.len; rank++) {
        const um_candset *set = &items[ranked[rank]];
        char text[UM_CAND_CAP];
        (void)snprintf(text, sizeof text, "ram:%zu", rank);
        const uint32_t *cells = set->members.data;
        for (size_t i = 0; st == ODIN3_OK && i < set->members.len; i++) {
            st = um_set_cand(uc, gc->cells[cells[i]].node, text);
        }
        odin3_log(ODIN3_LOG_INFO,
                  "unmap: %zu RAM cells with equal %s nets and different origin kept apart "
                  "(candidate set %s)",
                  set->members.len, set->why, text);
        uc->stats.cands++;
    }
    odin3_vec_free(&order);
    return st;
}

static odin3_status copy_cells(const um_gctx *gc, const um_wgroup *wg, odin3_vec *cells) {
    const uint32_t *members = wg->members.data;
    for (size_t i = 0; i < wg->members.len; i++) {
        um_ram *slot = odin3_vec_push(cells);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = gc->cells[members[i]];
    }
    return ODIN3_OK;
}

/* One um_rgroup from a final: its width groups are the replicas, slice-major. */
static odin3_status make_group(const um_gctx *gc, const um_octx *oc, uint32_t final_index,
                               um_rgroup *group) {
    const odin3_vec *final = (const odin3_vec *)oc->finals->data + final_index;
    const uint32_t *ids = final->data;
    const um_wgroup *wgs = oc->wgs->data;
    odin3_vec_init(&group->cells, sizeof(um_ram));
    group->slices = (uint32_t)wgs[ids[0]].members.len;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < final->len; i++) {
        st = copy_cells(gc, &wgs[ids[i]], &group->cells);
    }
    if (final->len > 1) {
        group->rule = "ram.replica";
    } else {
        group->rule = group->slices > 1 ? "ram.width" : "ram";
    }
    return st;
}

static void finals_free(odin3_vec *finals) {
    odin3_vec *items = finals->data;
    for (size_t i = 0; i < finals->len; i++) {
        odin3_vec_free(&items[i]);
    }
    odin3_vec_free(finals);
}

void um_rgroups_free(odin3_vec *groups) {
    um_rgroup *items = groups->data;
    for (size_t i = 0; i < groups->len; i++) {
        odin3_vec_free(&items[i].cells);
    }
    odin3_vec_free(groups);
}

static odin3_status build_groups(const um_gctx *gc, const um_octx *oc, odin3_vec *groups) {
    odin3_vec order;
    odin3_vec_init(&order, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < oc->finals->len; i++) {
        st = push_u32(&order, (uint32_t)i);
    }
    st = st == ODIN3_OK ? sort_idx(&order, cmp_finals, oc) : st;
    const uint32_t *ranked = order.data;
    for (size_t i = 0; st == ODIN3_OK && i < order.len; i++) {
        um_rgroup *group = odin3_vec_push(groups);
        if (group == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
            break;
        }
        memset(group, 0, sizeof *group);
        st = make_group(gc, oc, ranked[i], group);
    }
    odin3_vec_free(&order);
    return st;
}

odin3_status um_ram_groups(um_ctx *uc, const odin3_vec *cells, odin3_vec *groups) {
    um_gctx gc = {uc, cells->data};
    odin3_vec wgs;
    odin3_vec sets;
    odin3_vec finals;
    odin3_vec_init(groups, sizeof(um_rgroup));
    odin3_vec_init(&wgs, sizeof(um_wgroup));
    odin3_vec_init(&sets, sizeof(um_candset));
    odin3_vec_init(&finals, sizeof(odin3_vec));
    odin3_status st = width_groups(&gc, cells->len, &wgs, &sets);
    st = st == ODIN3_OK ? replica_groups(&gc, &wgs, (um_out){&finals, &sets}) : st;
    st = st == ODIN3_OK ? mark_candidates(uc, &gc, &sets) : st;
    um_octx oc = {&finals, &wgs, &sets};
    st = st == ODIN3_OK ? build_groups(&gc, &oc, groups) : st;
    finals_free(&finals);
    candsets_free(&sets);
    wgroups_free(&wgs);
    return st;
}
