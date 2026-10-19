/*
 * mem.c — an inferred `$mem` to `altsyncram` (5.1 spec §8, R-1..R-9). The `$mem` is copied into
 * hardmap's model (mem_load) and R-1 is hardmap's R-8 flip-flop merge on that copy (mem_merge: the
 * design is untouched, so a `$mem` that ends soft keeps its flip-flops). Mapped: one write port and
 * one read port, one rising-edge clock, a write enable that is one net, a read enable that is
 * constant 1, no INIT, no OFFSET. Read and write on the same address is a SINGLE_PORT block; two
 * addresses is a simple DUAL_PORT block (write on port a, read on port b). Every other `$mem` stays
 * soft with a warning. The tie-offs of the unused ports are those of the Quartus wizard and the
 * Yosys M9K techmap (spec R-3, R-4).
 */
#include "passes/lpmmap/lm_kit.h"

#include "ir/celltype.h"
#include "passes/ffinit.h"
#include "passes/hardmap/hm_kit.h"
#include "passes/hardmap/mem.h"

#include <inttypes.h>
#include <string.h>

enum { P_SIZE = 1, ABITS_MIN = 1, LM_SCALAR_NETS = 8 };

typedef enum lm_verdict {
    LV_OK,
    LV_PATTERN,
    LV_OFFSET,
    LV_INIT,
    LV_ASYNC,
    LV_EDGE,
    LV_CLOCKS,
    LV_WEN,
    LV_REN,
    LV_TRANSPARENT,
    LV_RDINIT
} lm_verdict;

static const mem_rport *rd0(const mem_model *mm) {
    return odin3_vec_cat(&mm->rd, 0);
}

static const mem_wport *wr0(const mem_model *mm) {
    return odin3_vec_cat(&mm->wr, 0);
}

/* The same net, or two constants of the same 0/1 value, or both open. */
static bool same_net(const lm_ctx *lc, odin3_net_id lhs, odin3_net_id rhs) {
    if (!odin3_net_valid(lhs) || !odin3_net_valid(rhs)) {
        return !odin3_net_valid(lhs) && !odin3_net_valid(rhs);
    }
    if (lhs.v == rhs.v) {
        return true;
    }
    odin3_const value = odin3_net_const_value(lc->module, lhs);
    return value == odin3_net_const_value(lc->module, rhs) &&
           (value == ODIN3_CONST_0 || value == ODIN3_CONST_1);
}

static bool same_addr(const lm_ctx *lc, const mem_model *mm) {
    for (uint32_t i = 0; i < mm->abits; i++) {
        if (!same_net(lc, rd0(mm)->addr[i], wr0(mm)->addr[i])) {
            return false;
        }
    }
    return true;
}

/* True when the read port reads every cycle: no enable, or an enable that is constant active. */
static bool read_always(const lm_ctx *lc, const mem_rport *rp) {
    if (rp->en_always) {
        return true;
    }
    return odin3_net_valid(rp->en) && odin3_net_const_value(lc->module, rp->en) ==
                                          (rp->en_pos ? ODIN3_CONST_1 : ODIN3_CONST_0);
}

/* R-1 and R-6: a flip-flop the merge removes must not carry an initial value (the RAM has none). */
static bool dead_ff_has_init(const lm_ctx *lc, const mem_model *mm) {
    for (size_t i = 0; i < mm->dead.len; i++) {
        odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&mm->dead, i);
        odin3_pinslice pins = odin3_node_pins(lc->module, node);
        for (uint32_t pin = 0; pin < pins.count; pin++) {
            odin3_pin_id id = {pins.first.v + pin};
            odin3_net_id net = odin3_pin_net(lc->module, id);
            if (odin3_pin_drives(lc->module, id) && odin3_net_valid(net) &&
                odin3_ffinit_get(lc->module, net) != ODIN3_BIT_X) {
                return true;
            }
        }
    }
    return false;
}

/* R-6: the checks of the two ports' clocks, edges and enables. */
static lm_verdict port_verdict(const lm_ctx *lc, const mem_rport *rp, const mem_wport *wp) {
    if (!rp->sync) {
        return LV_ASYNC;
    }
    if (!wp->sync || !odin3_net_valid(rp->clk) || !odin3_net_valid(wp->clk)) {
        return LV_PATTERN;
    }
    if (rp->clk.v != wp->clk.v) {
        return LV_CLOCKS;
    }
    if (!rp->posedge || !wp->posedge) {
        return LV_EDGE;
    }
    if (!wp->en_ok) {
        return LV_WEN;
    }
    return read_always(lc, rp) ? LV_OK : LV_REN;
}

/* R-2, R-5, R-6: the verdict on the merged model. */
static lm_verdict verdict_of(const lm_ctx *lc, const mem_model *mm) {
    if (mm->offset != 0) {
        return LV_OFFSET;
    }
    if (mm->too_wide || mm->n_rd != 1 || mm->n_wr != 1) {
        return LV_PATTERN;
    }
    if (mm->init_defined) {
        return LV_INIT;
    }
    if (dead_ff_has_init(lc, mm)) {
        return LV_RDINIT;
    }
    lm_verdict ports = port_verdict(lc, rd0(mm), wr0(mm));
    if (ports != LV_OK) {
        return ports;
    }
    return rd0(mm)->transparent && !same_addr(lc, mm) ? LV_TRANSPARENT : LV_OK;
}

static const char *mem_name(const lm_ctx *lc, const mem_model *mm) {
    const char *name = odin3_strtab_get(odin3_design_strtab(lc->design), mm->name);
    name = name != NULL ? name : "";
    return name[0] == '\\' ? name + 1 : name;
}

static void warn_soft(const lm_ctx *lc, const mem_model *mm, lm_verdict verdict) {
    static const char *const k_reason[] = {
        [LV_INIT] = "has initial contents",
        [LV_ASYNC] = "has an asynchronous read port",
        [LV_EDGE] = "has a port on the falling clock edge",
        [LV_CLOCKS] = "has read and write ports on different clocks",
        [LV_WEN] = "has a write enable that is not one net",
        [LV_REN] = "has a read enable that is not constant 1",
        [LV_TRANSPARENT] = "has a transparent read across two ports",
        [LV_RDINIT] = "has a read register with an initial value"};
    const char *name = mem_name(lc, mm);
    if (verdict == LV_OFFSET) {
        lm_diag(lc, ODIN3_LOG_WARN, mm->node,
                "lpmmap: memory '%s' has OFFSET %" PRId64 "; left soft", name, mm->offset);
    } else if (verdict == LV_PATTERN) {
        lm_diag(lc, ODIN3_LOG_WARN, mm->node,
                "lpmmap: memory '%s' (%" PRIu32 " read, %" PRIu32
                " write ports) has no 5.1 mapping; left soft",
                name, mm->n_rd, mm->n_wr);
    } else {
        lm_diag(lc, ODIN3_LOG_WARN, mm->node, "lpmmap: memory '%s' %s; left soft", name,
                k_reason[verdict]);
    }
}

/* --- building ------------------------------------------------------------------------------ */

/* The nets of one block: fresh read data, gnd arrays and the scalars of the tie-offs. */
typedef struct ram_nets {
    odin3_vec pool; /* odin3_net_id: reserved up front, so the arrays below stay put */
    odin3_net_id *q;
    odin3_net_id *gdata;
    odin3_net_id *gaddr;
    odin3_net_id gnd, vcc, clk;
    uint32_t awid;
} ram_nets;

static odin3_status nets_make(lm_ctx *lc, const mem_model *mm, ram_nets *rn) {
    memset(rn, 0, sizeof *rn);
    odin3_vec_init(&rn->pool, sizeof(odin3_net_id));
    rn->awid = mm->abits > 0 ? mm->abits : ABITS_MIN;
    size_t total = (size_t)2 * mm->width + rn->awid + LM_SCALAR_NETS;
    odin3_status st = odin3_vec_reserve(&rn->pool, total);
    st = st == ODIN3_OK ? lm_net_array(&rn->pool, mm->width, &rn->q) : st;
    st = st == ODIN3_OK ? lm_net_array(&rn->pool, mm->width, &rn->gdata) : st;
    st = st == ODIN3_OK ? lm_net_array(&rn->pool, rn->awid, &rn->gaddr) : st;
    st = st == ODIN3_OK ? lm_const(lc, LM_GND, &rn->gnd) : st;
    st = st == ODIN3_OK ? lm_const(lc, LM_VCC, &rn->vcc) : st;
    st = st == ODIN3_OK ? lm_fresh_nets(lc, mm->width, rn->q) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i < mm->width; i++) {
        rn->gdata[i] = rn->gnd;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < rn->awid; i++) {
        rn->gaddr[i] = rn->gnd;
    }
    rn->clk = rd0(mm)->clk;
    return st;
}

/* The address nets of a port: its own ABITS bits, or one gnd bit when ABITS is 0. */
static const odin3_net_id *addr_of(const ram_nets *rn, const odin3_net_id *addr, uint32_t abits) {
    return abits > 0 ? addr : rn->gaddr;
}

/* Everything the two modes share, after the ports that differ (R-3, R-4). */
static odin3_status new_block(lm_ctx *lc, const mem_model *mm, ram_nets *rn, bool single) {
    const mem_rport *rp = rd0(mm);
    const mem_wport *wp = wr0(mm);
    int64_t size = odin3_node_param(lc->module, mm->node, P_SIZE)->i;
    uint32_t awid = rn->awid;
    const char *rdw = rp->transparent ? "NEW_DATA_NO_NBE_READ" : "OLD_DATA";
    const lm_pset params[] = {
        {"width_a", (int64_t)mm->width, NULL},
        {"width_b", (int64_t)mm->width, NULL},
        {"widthad_a", (int64_t)awid, NULL},
        {"widthad_b", (int64_t)awid, NULL},
        {"numwords_a", size, NULL},
        {"numwords_b", size, NULL},
        {"operation_mode", 0, single ? "SINGLE_PORT" : "DUAL_PORT"},
        {"intended_device_family", 0, lc->family},
        {single ? "read_during_write_mode_port_a" : "read_during_write_mode_mixed_ports", 0, rdw},
        {"address_reg_b", 0, single ? "CLOCK1" : "CLOCK0"},
        {"outdata_reg_b", 0, "UNREGISTERED"}};
    const odin3_net_id *wr_addr = addr_of(rn, wp->addr, mm->abits);
    const odin3_net_id *rd_addr = addr_of(rn, rp->addr, mm->abits);
    const lm_pconn ports[] = {{"clock0", &rn->clk, 1},
                              {"clock1", &rn->vcc, 1},
                              {"address_a", wr_addr, awid},
                              {"data_a", odin3_vec_cat(&wp->data, 0), mm->width},
                              {"rden_a", &rn->vcc, 1},
                              {"wren_a", &wp->en, 1},
                              {"q_a", single ? rn->q : NULL, single ? mm->width : 0},
                              {"addressstall_a", &rn->gnd, 1},
                              {"address_b", single ? rn->gaddr : rd_addr, awid},
                              {"data_b", rn->gdata, mm->width},
                              {"rden_b", single ? &rn->gnd : &rn->vcc, 1},
                              {"wren_b", &rn->gnd, 1},
                              {"q_b", single ? NULL : rn->q, single ? 0 : mm->width},
                              {"addressstall_b", &rn->gnd, 1},
                              {"clocken0", &rn->vcc, 1},
                              {"clocken1", &rn->vcc, 1},
                              {"clocken2", &rn->vcc, 1},
                              {"clocken3", &rn->vcc, 1},
                              {"aclr0", &rn->gnd, 1},
                              {"aclr1", &rn->gnd, 1}};
    odin3_node_id node = {0};
    return lm_new_node(lc,
                       &(lm_node_req){LM_ALTSYNCRAM, params, sizeof params / sizeof params[0],
                                      ports, sizeof ports / sizeof ports[0], single ? "sp" : "sdp"},
                       &node);
}

/* The ties: each RD_DATA bit's net takes the block's read output. */
static odin3_status make_ties(const mem_model *mm, const ram_nets *rn, odin3_vec *ties) {
    const mem_rport *rp = rd0(mm);
    odin3_status st = odin3_vec_reserve(ties, mm->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < mm->width; i++) {
        lm_tie *tie = odin3_vec_push(ties); /* reserved: never NULL */
        *tie = (lm_tie){*(const odin3_net_id *)odin3_vec_cat(&rp->data, i), rn->q[i]};
    }
    return st;
}

typedef struct map_job {
    const mem_model *mm;
    bool single;
} map_job;

static odin3_status build_job(lm_ctx *lc, const map_job *job, odin3_vec *ties) {
    ram_nets rn;
    memset(&rn, 0, sizeof rn);
    odin3_status st = lm_begin_cell(lc, job->mm->node);
    for (size_t i = 0; st == ODIN3_OK && i < job->mm->dead.len; i++) {
        st = lm_doom(lc, *(const odin3_node_id *)odin3_vec_cat(&job->mm->dead, i));
    }
    st = st == ODIN3_OK ? nets_make(lc, job->mm, &rn) : st;
    if (st == ODIN3_OK) {
        st = new_block(lc, job->mm, &rn, job->single);
        st = st == ODIN3_OK ? make_ties(job->mm, &rn, ties) : st;
    }
    odin3_vec_free(&rn.pool);
    return st;
}

static odin3_status commit_job(lm_ctx *lc, const map_job *job) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(lm_tie));
    odin3_status st = build_job(lc, job, &ties);
    if (st != ODIN3_OK) {
        lm_abort(lc);
    } else {
        st = lm_commit(lc, odin3_vec_cat(&ties, 0), ties.len);
    }
    odin3_vec_free(&ties);
    return st;
}

odin3_status lm_map_mem(lm_ctx *lc, odin3_node_id src) {
    hm_ctx hc;
    hm_init(&hc, lc->pass, lc->design);
    hm_set_module(&hc, lc->module);
    mem_model mm;
    odin3_status st = mem_load(&hc, src, &mm);
    st = st == ODIN3_OK && !mm.too_wide ? mem_merge(&hc, &mm) : st;
    lm_verdict verdict = st == ODIN3_OK ? verdict_of(lc, &mm) : LV_PATTERN;
    if (st == ODIN3_OK && verdict == LV_OK) {
        odin3_celltype_id type = {0};
        st = lm_need(lc, LM_ALTSYNCRAM, src, &type);
        st = st == ODIN3_OK ? commit_job(lc, &(map_job){&mm, same_addr(lc, &mm)}) : st;
        lc->stats.ram += st == ODIN3_OK ? 1U : 0U;
    } else if (st == ODIN3_OK) {
        warn_soft(lc, &mm, verdict);
        lc->stats.soft++;
    }
    mem_model_free(&mm);
    hm_free(&hc);
    return st;
}
