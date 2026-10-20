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
} mm_job;

typedef struct mm_count {
    uint32_t data;
    uint32_t addr;
    bool blocked; /* a merged port the pass cannot write (active-low enable) */
} mm_count;

static bool bit_set(const odin3_value *val, uint32_t index) {
    return index < val->len && val->bits[index] == ODIN3_BIT_1;
}

/* Read port pn is synchronous in the model and was not in the cell. */
static bool port_merged(const mm_job *job, uint32_t pn) {
    const mem_rport *rp = odin3_vec_cat(&job->model.rd, pn);
    return rp->sync && !bit_set(&job->prm[MP_RD_CLK_ENABLE], pn);
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

static odin3_status write_merge(const mm_job *job) {
    mm_nets mn;
    mm_params mp;
    memset(&mn, 0, sizeof mn);
    memset(&mp, 0, sizeof mp);
    odin3_status st = load_nets(job, &mn);
    st = st == ODIN3_OK ? load_params(job, &mp) : st;
    if (st == ODIN3_OK) {
        overlay_ports(job, &mn);
        st = replace_mem(job, &mn, &mp);
    }
    free_params(&mp);
    odin3_vec_free(&mn.nets);
    return st;
}

/* --- the pass ------------------------------------------------------------------------------ */

/* One `$mem`: the merge on the model, then the write unless the pass leaves the cell as it is. */
static odin3_status merge_one(hm_ctx *hc, odin3_node_id src, pm_merge_stats *stats) {
    mm_job job = {.hc = hc, .src = src, .prm = odin3_node_param(hc->module, src, 0)};
    pm_memattr_warn(hc, src, "memory_merge");
    odin3_status st = mem_load(hc, src, &job.model);
    st = st == ODIN3_OK && !job.model.too_wide ? mem_merge(hc, &job.model) : st;
    mm_count cnt = st == ODIN3_OK ? count_merged(&job) : (mm_count){0, 0, false};
    if (st == ODIN3_OK && cnt.data + cnt.addr > 0 && !cnt.blocked && !dead_ff_has_init(&job)) {
        st = write_merge(&job);
        stats->data += st == ODIN3_OK ? cnt.data : 0U;
        stats->addr += st == ODIN3_OK ? cnt.addr : 0U;
    }
    mem_model_free(&job.model);
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
