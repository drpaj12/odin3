/*
 * mem_merge.c — R-8 (3C spec §6.2): the flip-flop merge into the read ports of an inferred `$mem`
 * (Yosys's memory_dff, subset). For each port that is still asynchronous:
 *  - data merge: every RD_DATA bit feeds exactly one `$dff`, or `$dffe`, and the flip-flops share
 *    one clock, polarity and enable and have no other D bit: they go, the port is synchronous with
 *    RD_TRANSPARENT 0 and its output is the flip-flops' Q;
 *  - address merge, otherwise: every RD_ADDR bit is the Q of a `$dff` on one clock: the port reads
 *    the D nets. Plan divergence D12 (agent default d): only when no write port is on that clock,
 *    because the arch's RAMs are read-first and the merged read of a registered address would
 *    return the old word after a same-address write where the RTL returns the new one. A flip-flop
 *    whose Q feeds anything but the merged address pins stays.
 * This works on the copy in mem_model: the design is untouched, so a `$mem` that ends soft keeps
 * its flip-flops (agent default d).
 */
#include "passes/hardmap/mem.h"

#include "ir/celltype.h"

#include <string.h>

enum {
    FF_CLK = 0,
    DFF_D = 1,
    DFF_Q = 2,
    DFFE_EN = 1,
    DFFE_D = 2,
    DFFE_Q = 3,
    FF_CLK_POL = 1,
    FFE_EN_POL = 2
};

/* A `$dff` or `$dffe` seen as a read-port register. */
typedef struct ff_view {
    odin3_node_id node;
    bool enabled;
    bool posedge;
    bool en_pos;
    uint32_t width;
    uint32_t d_port;
    uint32_t q_port;
    odin3_net_id clk;
    odin3_net_id en;
} ff_view;

odin3_net_id mem_pin_net(const hm_ctx *hc, odin3_node_id node, mem_pin pin) {
    odin3_pinslice pins = odin3_node_port(hc->module, node, pin.port);
    if (pin.index >= pins.count) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(hc->module, (odin3_pin_id){pins.first.v + pin.index});
}

static bool ff_view_of(const hm_ctx *hc, odin3_node_id node, ff_view *out) {
    const odin3_celltype_def *def =
        odin3_celltype_get(hc->design, odin3_node_type(hc->module, node));
    bool dff = def != NULL && strcmp(def->name, "$dff") == 0;
    bool dffe = def != NULL && strcmp(def->name, "$dffe") == 0;
    if (!dff && !dffe) {
        return false;
    }
    const odin3_value *polarity = odin3_node_param(hc->module, node, FF_CLK_POL);
    const odin3_value *width = odin3_node_param(hc->module, node, 0);
    const odin3_value *en_pol = dffe ? odin3_node_param(hc->module, node, FFE_EN_POL) : NULL;
    *out = (ff_view){.node = node,
                     .enabled = dffe,
                     .posedge = polarity->i != 0,
                     .en_pos = en_pol == NULL || en_pol->i != 0,
                     .width = (uint32_t)width->i,
                     .d_port = dffe ? DFFE_D : DFF_D,
                     .q_port = dffe ? DFFE_Q : DFF_Q,
                     .clk = mem_pin_net(hc, node, (mem_pin){FF_CLK, 0}),
                     .en = dffe ? mem_pin_net(hc, node, (mem_pin){DFFE_EN, 0}) : (odin3_net_id){0}};
    return true;
}

/* The same clock, polarity and enable. */
static bool same_domain(const ff_view *lhs, const ff_view *rhs) {
    return lhs->enabled == rhs->enabled && lhs->posedge == rhs->posedge &&
           lhs->clk.v == rhs->clk.v &&
           (!lhs->enabled || (lhs->en.v == rhs->en.v && lhs->en_pos == rhs->en_pos));
}

static bool has_node(const odin3_vec *nodes, odin3_node_id node) {
    for (size_t i = 0; i < nodes->len; i++) {
        if (((const odin3_node_id *)odin3_vec_cat(nodes, i))->v == node.v) {
            return true;
        }
    }
    return false;
}

/* Adds node to the list unless it is there. */
static odin3_status add_node(odin3_vec *nodes, odin3_node_id node) {
    if (has_node(nodes, node)) {
        return ODIN3_OK;
    }
    odin3_node_id *slot = odin3_vec_push(nodes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

/* --- data merge ---------------------------------------------------------------------------- */

typedef struct data_scan {
    const hm_ctx *hc;
    ff_view first;
    bool have;
    uint32_t covered; /* the widths of the distinct flip-flops found */
    odin3_vec found;  /* odin3_node_id */
    odin3_vec qpins;  /* odin3_net_id: the Q net of each data bit */
} data_scan;

/* Result of a scan step: ODIN3_OK (continue), INVALID_ARG (not mergeable), NO_MEMORY. */
static odin3_status scan_bit(data_scan *ds, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pinlist sinks = odin3_net_sinks(ds->hc->module, net);
    ff_view view;
    if (sinks.count != 1 ||
        !ff_view_of(ds->hc, odin3_pin_node(ds->hc->module, sinks.pins[0]), &view) ||
        odin3_pin_port(ds->hc->module, sinks.pins[0]) != view.d_port ||
        (ds->have && !same_domain(&ds->first, &view))) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!ds->have) {
        ds->first = view;
        ds->have = true;
    }
    if (!has_node(&ds->found, view.node)) {
        ds->covered += view.width;
    }
    odin3_status st = add_node(&ds->found, view.node);
    odin3_net_id *slot = st == ODIN3_OK ? odin3_vec_push(&ds->qpins) : NULL;
    if (slot == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = mem_pin_net(ds->hc, view.node,
                        (mem_pin){view.q_port, odin3_pin_bit(ds->hc->module, sinks.pins[0])});
    return ODIN3_OK;
}

/* Rewrites the port from a successful scan and queues the flip-flops. */
static odin3_status apply_data(mem_model *mm, mem_rport *rp, const data_scan *ds) {
    for (size_t i = 0; i < ds->qpins.len; i++) {
        *(odin3_net_id *)odin3_vec_at(&rp->data, i) =
            *(const odin3_net_id *)odin3_vec_cat(&ds->qpins, i);
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ds->found.len; i++) {
        st = add_node(&mm->dead, *(const odin3_node_id *)odin3_vec_cat(&ds->found, i));
    }
    rp->sync = true;
    rp->posedge = ds->first.posedge;
    rp->transparent = false;
    rp->clk = ds->first.clk;
    rp->en = ds->first.en;
    rp->en_pos = ds->first.en_pos;
    rp->en_always = !ds->first.enabled;
    return st;
}

/* R-8 data merge of one port (rp->sync says whether it applied). */
static odin3_status merge_data(const hm_ctx *hc, mem_model *mm, mem_rport *rp) {
    data_scan ds = {.hc = hc};
    odin3_vec_init(&ds.found, sizeof(odin3_node_id));
    odin3_vec_init(&ds.qpins, sizeof(odin3_net_id));
    odin3_status st = odin3_vec_reserve(&ds.qpins, rp->data.len);
    for (size_t i = 0; st == ODIN3_OK && i < rp->data.len; i++) {
        st = scan_bit(&ds, *(const odin3_net_id *)odin3_vec_cat(&rp->data, i));
    }
    if (st == ODIN3_ERR_INVALID_ARG) {
        st = ODIN3_OK;
    } else if (st == ODIN3_OK && ds.covered == rp->data.len && odin3_net_valid(ds.first.clk) &&
               (!ds.first.enabled || odin3_net_valid(ds.first.en))) {
        st = apply_data(mm, rp, &ds);
    }
    odin3_vec_free(&ds.found);
    odin3_vec_free(&ds.qpins);
    return st;
}

/* --- address merge ------------------------------------------------------------------------- */

typedef struct addr_scan {
    const hm_ctx *hc;
    const mem_model *mm;
    ff_view first;
    bool have;
    odin3_net_id d[MEM_MAX_ABITS];
    odin3_vec found; /* odin3_node_id */
} addr_scan;

/* True when the net is driven by this `$mem`: an address FF fed by the memory's own read data
 * would read an async net that has no driver once the `$mem` is gone (review M-1). */
static bool fed_by_mem(const addr_scan *as, odin3_net_id net) {
    odin3_pin_id drv = odin3_net_driver(as->hc->module, net);
    return odin3_pin_valid(drv) && odin3_pin_node(as->hc->module, drv).v == as->mm->node.v;
}

/* Result as scan_bit's. */
static odin3_status scan_addr_bit(addr_scan *as, odin3_net_id net, uint32_t index) {
    odin3_pin_id drv =
        odin3_net_valid(net) ? odin3_net_driver(as->hc->module, net) : (odin3_pin_id){0};
    ff_view view;
    if (!odin3_pin_valid(drv) || odin3_net_driver_count(as->hc->module, net) != 1 ||
        !ff_view_of(as->hc, odin3_pin_node(as->hc->module, drv), &view) || view.enabled ||
        odin3_pin_port(as->hc->module, drv) != view.q_port ||
        (as->have && !same_domain(&as->first, &view))) {
        return ODIN3_ERR_INVALID_ARG;
    }
    as->first = as->have ? as->first : view;
    as->have = true;
    as->d[index] =
        mem_pin_net(as->hc, view.node, (mem_pin){view.d_port, odin3_pin_bit(as->hc->module, drv)});
    if (!odin3_net_valid(as->d[index]) || fed_by_mem(as, as->d[index]) ||
        has_node(&as->mm->dead, view.node)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return add_node(&as->found, view.node);
}

/* D12: a write port on the read clock (or one that is not clocked) forbids the address merge. */
static bool write_on_clock(const mem_model *mm, const ff_view *ff) {
    for (uint32_t i = 0; i < mm->n_wr; i++) {
        const mem_wport *wp = odin3_vec_cat(&mm->wr, i);
        if (!wp->sync || (wp->clk.v == ff->clk.v && wp->posedge == ff->posedge)) {
            return true;
        }
    }
    return false;
}

/* The Q nets of ff feed only the merged RD_ADDR pins of mm (so the flip-flop can go). */
static bool only_feeds_merged(const hm_ctx *hc, const mem_model *mm, odin3_node_id ff) {
    ff_view view;
    if (!ff_view_of(hc, ff, &view)) {
        return false;
    }
    odin3_pinslice qpins = odin3_node_port(hc->module, ff, view.q_port);
    for (uint32_t bit = 0; bit < qpins.count; bit++) {
        odin3_net_id net = odin3_pin_net(hc->module, (odin3_pin_id){qpins.first.v + bit});
        odin3_pinlist sinks =
            odin3_net_valid(net) ? odin3_net_sinks(hc->module, net) : (odin3_pinlist){NULL, 0};
        for (uint32_t snk = 0; snk < sinks.count; snk++) {
            odin3_pin_id pin = sinks.pins[snk];
            uint32_t port_no = mm->abits == 0 ? 0 : odin3_pin_bit(hc->module, pin) / mm->abits;
            if (odin3_pin_node(hc->module, pin).v != mm->node.v ||
                odin3_pin_port(hc->module, pin) != MEM_P_RD_ADDR || port_no >= mm->n_rd ||
                !((const mem_rport *)odin3_vec_cat(&mm->rd, port_no))->addr_merged) {
                return false;
            }
        }
    }
    return true;
}

/* R-8 address merge of one port; the flip-flops found are appended to cands. */
static odin3_status merge_addr(const hm_ctx *hc, const mem_model *mm, mem_rport *rp,
                               odin3_vec *cands) {
    addr_scan as = {.hc = hc, .mm = mm};
    odin3_vec_init(&as.found, sizeof(odin3_node_id));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < mm->abits; i++) {
        st = scan_addr_bit(&as, rp->addr[i], i);
    }
    if (st == ODIN3_OK && as.have && odin3_net_valid(as.first.clk) &&
        !write_on_clock(mm, &as.first)) {
        memcpy(rp->addr, as.d, mm->abits * sizeof as.d[0]);
        rp->sync = true;
        rp->addr_merged = true;
        rp->posedge = as.first.posedge;
        rp->transparent = false;
        rp->clk = as.first.clk;
        rp->en_always = true;
        for (size_t i = 0; st == ODIN3_OK && i < as.found.len; i++) {
            st = add_node(cands, *(const odin3_node_id *)odin3_vec_cat(&as.found, i));
        }
    }
    odin3_vec_free(&as.found);
    return st == ODIN3_ERR_INVALID_ARG ? ODIN3_OK : st;
}

odin3_status mem_merge(const hm_ctx *hc, mem_model *mm) {
    odin3_vec cands;
    odin3_vec_init(&cands, sizeof(odin3_node_id));
    odin3_status st = ODIN3_OK;
    for (uint32_t pn = 0; st == ODIN3_OK && pn < mm->n_rd; pn++) {
        mem_rport *rp = odin3_vec_at(&mm->rd, pn);
        if (!rp->sync) {
            st = merge_data(hc, mm, rp);
        }
        if (st == ODIN3_OK && !rp->sync && mm->abits > 0) {
            st = merge_addr(hc, mm, rp, &cands);
        }
    }
    for (size_t i = 0; st == ODIN3_OK && i < cands.len; i++) {
        odin3_node_id ff = *(const odin3_node_id *)odin3_vec_cat(&cands, i);
        st = only_feeds_merged(hc, mm, ff) ? add_node(&mm->dead, ff) : st;
    }
    odin3_vec_free(&cands);
    return st;
}
