/*
 * memory_merge.c — the `memory_merge` pass (4.2 spec section 7, G-1..G-5; plan Task 5). The R-8
 * merge of mem_merge.c runs on the model of each `$mem` (mem_load, mem_merge: unchanged, the code
 * partial_map uses), and this file writes the result into the design: a `$mem` has no parameter
 * setter, so the merged cell is a new `$mem` built in one hm_kit transaction that replaces the old
 * one (name, attributes, one DERIVED record) and removes the flip-flops the merge made redundant.
 */
#include "passes/partial_map/memory_merge.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "passes/ffinit.h"
#include "passes/partial_map/hm_kit.h"
#include "passes/partial_map/mem.h"
#include "util/log.h"

#include <inttypes.h>
#include <string.h>

enum { MM_N_PORTS = 8 };

typedef struct mm_job {
    hm_ctx *hc;
    odin3_node_id src;
    const odin3_value *prm; /* the source's parameters */
    mem_model model;
    odin3_vec skip; /* uint8_t per read port: 1 = the pass leaves the port as it is */
} mm_job;

typedef struct mm_count {
    uint32_t data;
    uint32_t addr;
    bool blocked; /* a merged port the pass cannot write (active-low enable): see mark_blocked */
} mm_count;

static bool bit_set(const odin3_value *val, uint32_t index) {
    return index < val->len && val->bits[index] == ODIN3_BIT_1;
}

/* Read port pn is synchronous in the model and was not in the cell. */
static bool port_merged(const mm_job *job, uint32_t pn) {
    const mem_rport *rp = odin3_vec_cat(&job->model.rd, pn);
    bool skipped = pn < job->skip.len && *(const uint8_t *)odin3_vec_cat(&job->skip, pn) != 0;
    return rp->sync && !skipped && !bit_set(&job->prm[MP_RD_CLK_ENABLE], pn);
}

static mm_count count_merged(const mm_job *job) {
    mm_count cnt = {0, 0, false};
    for (uint32_t pn = 0; pn < job->model.n_rd; pn++) {
        const mem_rport *rp = odin3_vec_cat(&job->model.rd, pn);
        if (!port_merged(job, pn)) {
            continue;
        }
        cnt.blocked = cnt.blocked || (!rp->en_always && !rp->en_pos);
        cnt.addr += rp->addr_merged ? 1U : 0U;
        cnt.data += rp->addr_merged ? 0U : 1U;
    }
    return cnt;
}

/* A flip-flop that would go carries an initial value (the RAM's read register has none). */
static bool dead_ff_has_init(const mm_job *job) {
    const odin3_module *mod = job->hc->module;
    for (size_t i = 0; i < job->model.dead.len; i++) {
        odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&job->model.dead, i);
        odin3_pinslice pins = odin3_node_pins(mod, node);
        for (uint32_t pin = 0; pin < pins.count; pin++) {
            odin3_pin_id id = {pins.first.v + pin};
            odin3_net_id net = odin3_pin_net(mod, id);
            if (odin3_pin_drives(mod, id) && odin3_net_valid(net) &&
                odin3_ffinit_get(mod, net) != ODIN3_BIT_X) {
                return true;
            }
        }
    }
    return false;
}

/* --- the replacement cell ------------------------------------------------------------------ */

typedef struct mm_nets {
    odin3_vec nets; /* odin3_net_id: every pin of the source, port by port */
    uint32_t first[MM_N_PORTS];
    odin3_netvec port[MM_N_PORTS];
} mm_nets;

static odin3_net_id *net_at(mm_nets *mn, uint32_t port, uint32_t index) {
    return odin3_vec_at(&mn->nets, mn->first[port] + index);
}

/* The nets on the source's pins (nothing is allocated after the reserve, so the netvecs hold). */
static odin3_status load_nets(const mm_job *job, mm_nets *mn) {
    const odin3_module *mod = job->hc->module;
    odin3_vec_init(&mn->nets, sizeof(odin3_net_id));
    odin3_status st =
        odin3_vec_reserve(&mn->nets, (size_t)odin3_node_pins(mod, job->src).count + 1);
    for (uint32_t port = 0; st == ODIN3_OK && port < MM_N_PORTS; port++) {
        odin3_pinslice pins = odin3_node_port(mod, job->src, port);
        mn->first[port] = (uint32_t)mn->nets.len;
        for (uint32_t i = 0; i < pins.count; i++) {
            *(odin3_net_id *)odin3_vec_push(&mn->nets) =
                odin3_pin_net(mod, (odin3_pin_id){pins.first.v + i});
        }
        mn->port[port] =
            (odin3_netvec){pins.count > 0 ? net_at(mn, port, 0) : mn->nets.data, pins.count};
    }
    return st;
}

/* Puts the merged ports' clock, enable, address and data nets over the source's. */
static void overlay_ports(const mm_job *job, mm_nets *mn) {
    const mem_model *mm = &job->model;
    for (uint32_t pn = 0; pn < mm->n_rd; pn++) {
        const mem_rport *rp = odin3_vec_cat(&mm->rd, pn);
        if (!port_merged(job, pn)) {
            continue;
        }
        *net_at(mn, MEM_P_RD_CLK, pn) = rp->clk;
        if (!rp->en_always) {
            *net_at(mn, MEM_P_RD_EN, pn) = rp->en;
        }
        for (uint32_t i = 0; i < mm->abits; i++) {
            *net_at(mn, MEM_P_RD_ADDR, pn * mm->abits + i) = rp->addr[i];
        }
        for (uint32_t i = 0; i < mm->width; i++) {
            *net_at(mn, MEM_P_RD_DATA, pn * mm->width + i) =
                *(const odin3_net_id *)odin3_vec_cat(&rp->data, i);
        }
    }
}

/* The three read-port bit parameters of the new cell: the source's, with the merged ports set. */
typedef struct mm_params {
    odin3_value val[MP_N_PARAMS];
    odin3_vec bits[3]; /* uint8_t: RD_CLK_ENABLE, RD_CLK_POLARITY, RD_TRANSPARENT */
} mm_params;

static const uint32_t k_bit_params[3] = {MP_RD_CLK_ENABLE, MP_RD_CLK_POLARITY, MP_RD_TRANSPARENT};

static void free_params(mm_params *mp) {
    for (uint32_t i = 0; i < 3; i++) {
        odin3_vec_free(&mp->bits[i]);
    }
}

static odin3_status load_params(const mm_job *job, mm_params *mp) {
    odin3_status st = ODIN3_OK;
    memcpy(mp->val, job->prm, sizeof mp->val);
    for (uint32_t k = 0; k < 3; k++) {
        odin3_vec_init(&mp->bits[k], sizeof(uint8_t));
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < 3; k++) {
        const odin3_value *old = &job->prm[k_bit_params[k]];
        st = odin3_vec_reserve(&mp->bits[k], (size_t)old->len + 1);
        for (uint32_t i = 0; st == ODIN3_OK && i < old->len; i++) {
            *(uint8_t *)odin3_vec_push(&mp->bits[k]) = old->bits[i];
        }
        mp->val[k_bit_params[k]].bits = mp->bits[k].data;
    }
    for (uint32_t pn = 0; st == ODIN3_OK && pn < job->model.n_rd; pn++) {
        const mem_rport *rp = odin3_vec_cat(&job->model.rd, pn);
        if (port_merged(job, pn)) {
            *(uint8_t *)odin3_vec_at(&mp->bits[0], pn) = ODIN3_BIT_1;
            *(uint8_t *)odin3_vec_at(&mp->bits[1], pn) = rp->posedge ? ODIN3_BIT_1 : ODIN3_BIT_0;
            *(uint8_t *)odin3_vec_at(&mp->bits[2], pn) = ODIN3_BIT_0;
        }
    }
    return st;
}

/* The transaction: the new `$mem`, the flip-flops doomed, the source replaced. */
static odin3_status replace_mem(const mm_job *job, const mm_nets *mn, const mm_params *mp) {
    hm_ctx *hc = job->hc;
    odin3_celltype_id type = odin3_node_type(hc->module, job->src);
    odin3_node_id made = {0};
    odin3_status st = hm_begin_cell(hc, job->src);
    for (size_t i = 0; st == ODIN3_OK && i < job->model.dead.len; i++) {
        st = hm_doom(hc, *(const odin3_node_id *)odin3_vec_cat(&job->model.dead, i));
    }
    hm_node_req req = {type, mp->val, MP_N_PARAMS, mn->port, "merge"};
    st = st == ODIN3_OK ? hm_new_node(hc, &req, &made) : st;
    if (st != ODIN3_OK) {
        hm_abort(hc);
        return st;
    }
    return hm_commit(hc, NULL, 0);
}

/* The async data nets of the data-merged ports (before the overlay): after the merge nothing
 * drives or reads them. */
static odin3_status collect_orphans(const mm_job *job, const mm_nets *mn, odin3_vec *orphans) {
    const mem_model *mm = &job->model;
    odin3_status st = odin3_vec_reserve(orphans, (size_t)mm->n_rd * mm->width + 1);
    for (uint32_t pn = 0; st == ODIN3_OK && pn < mm->n_rd; pn++) {
        const mem_rport *rp = odin3_vec_cat(&mm->rd, pn);
        if (!port_merged(job, pn) || rp->addr_merged) {
            continue;
        }
        for (uint32_t i = 0; i < mm->width; i++) {
            odin3_net_id *slot = odin3_vec_push(orphans);
            *slot = *(const odin3_net_id *)odin3_vec_cat(&mn->nets, mn->first[MEM_P_RD_DATA] +
                                                                        pn * mm->width + i);
        }
    }
    return st;
}

static bool net_unused(const odin3_module *mod, odin3_net_id net) {
    return odin3_net_live(mod, net) && odin3_net_pins(mod, net).count == 0;
}

/* A wire that is not a port and none of whose bits has a pin. */
static bool wire_unused(const odin3_module *mod, odin3_wire_id wire) {
    if (!odin3_wire_live(mod, wire) || odin3_node_valid(odin3_wire_port_node(mod, wire))) {
        return false;
    }
    for (uint32_t bit = 0; bit < odin3_wire_width(mod, wire); bit++) {
        if (!net_unused(mod, odin3_wire_net(mod, wire, bit))) {
            return false;
        }
    }
    return true;
}

/* Removes the nets (and a wire only they made up) that the merge left without a driver and a
 * reader, so write_rtlil shows no undriven wire for the old read data. Best effort: a net that
 * still has an alias or a partly used wire stays. */
static void drop_orphans(hm_ctx *hc, const odin3_vec *orphans) {
    odin3_module *mod = hc->module;
    for (size_t i = 0; i < orphans->len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(orphans, i);
        if (!net_unused(mod, net)) {
            continue;
        }
        odin3_wirebit prim = odin3_net_primary(mod, net);
        if (odin3_wire_valid(prim.wire) && wire_unused(mod, prim.wire)) {
            (void)odin3_wire_delete(mod, prim.wire);
        }
        (void)odin3_net_delete(mod, net);
    }
}

static odin3_status write_merge(const mm_job *job) {
    mm_nets mn;
    mm_params mp;
    odin3_vec orphans;
    memset(&mn, 0, sizeof mn);
    memset(&mp, 0, sizeof mp);
    odin3_vec_init(&orphans, sizeof(odin3_net_id));
    odin3_status st = load_nets(job, &mn);
    st = st == ODIN3_OK ? load_params(job, &mp) : st;
    st = st == ODIN3_OK ? collect_orphans(job, &mn, &orphans) : st;
    if (st == ODIN3_OK) {
        overlay_ports(job, &mn);
        st = replace_mem(job, &mn, &mp);
    }
    if (st == ODIN3_OK) {
        drop_orphans(job->hc, &orphans);
    }
    odin3_vec_free(&orphans);
    free_params(&mp);
    odin3_vec_free(&mn.nets);
    return st;
}

/* --- the pass ------------------------------------------------------------------------------ */

/* The model of the cell with R-8 applied; the ports in job->skip are marked synchronous first, so
 * the merge leaves them (and their flip-flops) alone. */
static odin3_status load_merged(mm_job *job) {
    odin3_status st = mem_load(job->hc, job->src, &job->model);
    for (uint32_t pn = 0; st == ODIN3_OK && pn < job->skip.len; pn++) {
        if (*(const uint8_t *)odin3_vec_cat(&job->skip, pn) != 0) {
            ((mem_rport *)odin3_vec_at(&job->model.rd, pn))->sync = true;
        }
    }
    return st == ODIN3_OK && !job->model.too_wide ? mem_merge(job->hc, &job->model) : st;
}

/* A merged port whose enable is active low would need an inverter: mark just that port to be left
 * as it is (the other ports of the cell still merge, as in partial_map). */
static odin3_status mark_blocked(mm_job *job) {
    odin3_status st = odin3_vec_reserve(&job->skip, (size_t)job->model.n_rd + 1);
    for (uint32_t pn = 0; st == ODIN3_OK && pn < job->model.n_rd; pn++) {
        const mem_rport *rp = odin3_vec_cat(&job->model.rd, pn);
        uint8_t *slot = odin3_vec_push(&job->skip);
        *slot = port_merged(job, pn) && !rp->en_always && !rp->en_pos ? 1U : 0U;
    }
    return st;
}

/* The three bit parameters hold one bit per read port (the replacement indexes them by port). */
static bool params_cover_ports(const mm_job *job) {
    const odin3_value *ports = &job->prm[MP_RD_PORTS];
    for (uint32_t k = 0; k < 3; k++) {
        if ((int64_t)job->prm[k_bit_params[k]].len < ports->i) {
            return false;
        }
    }
    return true;
}

static odin3_status merge_job(mm_job *job, pm_merge_stats *stats) {
    odin3_status st = load_merged(job);
    mm_count cnt = st == ODIN3_OK ? count_merged(job) : (mm_count){0, 0, false};
    if (st == ODIN3_OK && cnt.blocked) {
        st = mark_blocked(job);
        mem_model_free(&job->model);
        st = st == ODIN3_OK ? load_merged(job) : st;
        cnt = st == ODIN3_OK ? count_merged(job) : cnt;
    }
    if (st == ODIN3_OK && cnt.data + cnt.addr > 0 && !dead_ff_has_init(job)) {
        st = write_merge(job);
        stats->data += st == ODIN3_OK ? cnt.data : 0U;
        stats->addr += st == ODIN3_OK ? cnt.addr : 0U;
    }
    mem_model_free(&job->model);
    return st;
}

/* One `$mem`: the merge on the model, then the write unless the pass leaves the cell as it is. */
static odin3_status merge_one(hm_ctx *hc, odin3_node_id src, pm_merge_stats *stats) {
    mm_job job = {.hc = hc, .src = src, .prm = odin3_node_param(hc->module, src, 0)};
    if (!params_cover_ports(&job)) {
        hm_diag(hc, ODIN3_LOG_ERROR, src,
                "memory_merge: a read-port parameter of the `$mem` has fewer bits than RD_PORTS");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec_init(&job.skip, sizeof(uint8_t));
    odin3_status st = merge_job(&job, stats);
    odin3_vec_free(&job.skip);
    return st;
}

static odin3_status merge_module(hm_ctx *hc, odin3_module *mod, odin3_celltype_id mem,
                                 pm_merge_stats *stats) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == mem.v) {
            st = merge_one(hc, node, stats);
        }
    }
    return st;
}

/* Read ports still asynchronous, over every live `$mem`. */
static uint64_t count_async(odin3_design *design, odin3_celltype_id mem) {
    uint64_t found = 0;
    for (uint32_t mi = 1; mi < odin3_design_module_end(design); mi++) {
        const odin3_module *mod = odin3_module_get(design, (odin3_module_id){mi});
        for (uint32_t id = 1; mod != NULL && id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (!odin3_node_live(mod, node) || odin3_node_type(mod, node).v != mem.v) {
                continue;
            }
            const odin3_value *prm = odin3_node_param(mod, node, 0);
            for (uint32_t pn = 0; pn < (uint32_t)prm[MP_RD_PORTS].i; pn++) {
                found += bit_set(&prm[MP_RD_CLK_ENABLE], pn) ? 0U : 1U;
            }
        }
    }
    return found;
}

odin3_status pm_mem_merge(odin3_pass_ctx *ctx, odin3_design *design, pm_merge_stats *stats) {
    pm_merge_stats local = {0, 0, 0};
    odin3_celltype_id mem = {0};
    hm_ctx hc;
    hm_init(&hc, ctx, design);
    odin3_status st = ODIN3_OK;
    if (hm_find_type(design, "$mem", &mem)) {
        for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(design); mi++) {
            odin3_module *mod = odin3_module_get(design, (odin3_module_id){mi});
            st = mod != NULL ? merge_module(&hc, mod, mem, &local) : st;
        }
        local.async = count_async(design, mem);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "memory_merge: %" PRIu64 " read ports merged (%" PRIu64 " data, %" PRIu64
                  " address), %" PRIu64 " left asynchronous",
                  local.data + local.addr, local.data, local.addr, local.async);
    }
    if (stats != NULL) {
        *stats = local;
    }
    hm_free(&hc);
    return st;
}
