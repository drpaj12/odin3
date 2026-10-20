/*
 * mem.c — inferred `$mem` cells mapped to the arch's RAMs (3C spec §6.2, R-8..R-12; plan Task 6).
 * The `$mem` is copied into a model (mem_load), R-8 rewrites the copy (mem_merge.c), the verdict
 * and the RAM plan come from mem_plan.c, and the RAMs are built through hm_build_ram (ram.c) in
 * one transaction with the `$mem` and the flip-flops R-8 made redundant (H-7, H-9). A `$mem` that
 * stays soft is not touched and gets one warning.
 */
#include "passes/partial_map/mem.h"

#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* --- the model ----------------------------------------------------------------------------- */

static bool bit_of(const odin3_value *val, uint32_t index) {
    return index < val->len && val->bits[index] == ODIN3_BIT_1;
}

void mem_model_free(mem_model *mm) {
    for (size_t i = 0; i < mm->rd.len; i++) {
        odin3_vec_free(&((mem_rport *)odin3_vec_at(&mm->rd, i))->data);
    }
    for (size_t i = 0; i < mm->wr.len; i++) {
        odin3_vec_free(&((mem_wport *)odin3_vec_at(&mm->wr, i))->data);
    }
    odin3_vec_free(&mm->rd);
    odin3_vec_free(&mm->wr);
    odin3_vec_free(&mm->dead);
    odin3_vec_free(&mm->rams);
}

/* Appends the nets of pins [first, first + count) of port to data. */
static odin3_status load_nets(const hm_ctx *hc, const mem_model *mm, odin3_vec *data, uint32_t port,
                              uint32_t first) {
    odin3_status st = odin3_vec_reserve(data, mm->width);
    for (uint32_t j = 0; st == ODIN3_OK && j < mm->width; j++) {
        *(odin3_net_id *)odin3_vec_push(data) =
            mem_pin_net(hc, mm->node, (mem_pin){port, first + j});
    }
    return st;
}

static odin3_status load_rport(const hm_ctx *hc, mem_model *mm, const odin3_value *prm,
                               uint32_t pn) {
    mem_rport *rp = odin3_vec_push(&mm->rd);
    if (rp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(rp, 0, sizeof *rp);
    odin3_vec_init(&rp->data, sizeof(odin3_net_id));
    rp->sync = bit_of(&prm[MP_RD_CLK_ENABLE], pn);
    rp->posedge = bit_of(&prm[MP_RD_CLK_POLARITY], pn);
    rp->transparent = bit_of(&prm[MP_RD_TRANSPARENT], pn);
    rp->en_pos = true;
    rp->clk = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_RD_CLK, pn});
    rp->en = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_RD_EN, pn});
    for (uint32_t i = 0; i < mm->abits; i++) {
        rp->addr[i] = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_RD_ADDR, pn * mm->abits + i});
    }
    return load_nets(hc, mm, &rp->data, MEM_P_RD_DATA, pn * mm->width);
}

/* The WIDTH enable bits of write port pn are one valid net. */
static bool enable_of(const hm_ctx *hc, const mem_model *mm, uint32_t pn, odin3_net_id *en) {
    *en = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_WR_EN, pn * mm->width});
    for (uint32_t j = 1; j < mm->width; j++) {
        if (mem_pin_net(hc, mm->node, (mem_pin){MEM_P_WR_EN, pn * mm->width + j}).v != en->v) {
            return false;
        }
    }
    return odin3_net_valid(*en);
}

static odin3_status load_wport(const hm_ctx *hc, mem_model *mm, const odin3_value *prm,
                               uint32_t pn) {
    mem_wport *wp = odin3_vec_push(&mm->wr);
    if (wp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(wp, 0, sizeof *wp);
    odin3_vec_init(&wp->data, sizeof(odin3_net_id));
    wp->sync = bit_of(&prm[MP_WR_CLK_ENABLE], pn);
    wp->posedge = bit_of(&prm[MP_WR_CLK_POLARITY], pn);
    wp->clk = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_WR_CLK, pn});
    wp->en_ok = enable_of(hc, mm, pn, &wp->en);
    for (uint32_t i = 0; i < mm->abits; i++) {
        wp->addr[i] = mem_pin_net(hc, mm->node, (mem_pin){MEM_P_WR_ADDR, pn * mm->abits + i});
    }
    return load_nets(hc, mm, &wp->data, MEM_P_WR_DATA, pn * mm->width);
}

static bool init_has_defined(const odin3_value *init) {
    for (uint32_t i = 0; i < init->len; i++) {
        if (init->bits[i] == ODIN3_BIT_0 || init->bits[i] == ODIN3_BIT_1) {
            return true;
        }
    }
    return false;
}

/* Copies the `$mem` into mm. A memory of more address bits than hardmap splits is not loaded. */
odin3_status mem_load(const hm_ctx *hc, odin3_node_id node, mem_model *mm) {
    memset(mm, 0, sizeof *mm);
    odin3_vec_init(&mm->rd, sizeof(mem_rport));
    odin3_vec_init(&mm->wr, sizeof(mem_wport));
    odin3_vec_init(&mm->dead, sizeof(odin3_node_id));
    odin3_vec_init(&mm->rams, sizeof(mem_ram));
    const odin3_value *prm = odin3_node_param(hc->module, node, 0);
    mm->node = node;
    mm->name = prm[MP_MEMID].str;
    mm->offset = prm[MP_OFFSET].i;
    mm->abits = (uint32_t)prm[MP_ABITS].i;
    mm->width = (uint32_t)prm[MP_WIDTH].i;
    mm->n_rd = (uint32_t)prm[MP_RD_PORTS].i;
    mm->n_wr = (uint32_t)prm[MP_WR_PORTS].i;
    mm->init_defined = init_has_defined(&prm[MP_INIT]);
    mm->too_wide = mm->abits > MEM_MAX_ABITS;
    odin3_status st = ODIN3_OK;
    for (uint32_t pn = 0; st == ODIN3_OK && !mm->too_wide && pn < mm->n_rd; pn++) {
        st = load_rport(hc, mm, prm, pn);
    }
    for (uint32_t pn = 0; st == ODIN3_OK && !mm->too_wide && pn < mm->n_wr; pn++) {
        st = load_wport(hc, mm, prm, pn);
    }
    return st;
}

/* --- diagnostics --------------------------------------------------------------------------- */

/* The memory's name for messages: MEMID without the RTLIL backslash. */
static const char *mem_name(const hm_ctx *hc, const mem_model *mm) {
    const char *name = odin3_strtab_get(odin3_design_strtab(hc->design), mm->name);
    name = name != NULL ? name : "";
    return name[0] == '\\' ? name + 1 : name;
}

/* Why a verdict left the memory soft, for the large-memory warning. */
static const char *soft_reason(mem_verdict verdict) {
    switch (verdict) {
    case MEM_SOFT_OFFSET:
        return "its OFFSET is not 0";
    case MEM_SOFT_ASYNC:
        return "it has an asynchronous read port or ports on more than one clock";
    case MEM_SOFT_EDGE:
        return "it has a port on the falling clock edge";
    default:
        return "it has no fixed-subset mapping to the arch's RAMs";
    }
}

void pm_mem_warn_big(const hm_ctx *hc, odin3_node_id node, const char *reason) {
    const odin3_value *prm = odin3_node_param(hc->module, node, 0);
    uint64_t bits = (uint64_t)prm[MP_SIZE].i * (uint64_t)prm[MP_WIDTH].i;
    if (bits <= PM_SOFT_FF_WARN) {
        return;
    }
    const char *name = odin3_strtab_get(odin3_design_strtab(hc->design), prm[MP_MEMID].str);
    name = name != NULL ? name : "";
    hm_diag(hc, ODIN3_LOG_WARN, node,
            "partial_map: memory '%s' of %" PRIu64
            " bits (width x depth) stays soft because %s; lowering it creates that many "
            "flip-flops (more than %u)",
            name[0] == '\\' ? name + 1 : name, bits, reason, (unsigned)PM_SOFT_FF_WARN);
}

static void warn_soft(const hm_ctx *hc, const mem_model *mm, mem_verdict verdict) {
    const char *name = mem_name(hc, mm);
    if (verdict == MEM_SOFT_OFFSET) {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s' has OFFSET %" PRId64 "; left to lower", name, mm->offset);
    } else if (verdict == MEM_SOFT_ASYNC) {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s' has an asynchronous read port; left to lower as "
                "flip-flops",
                name);
    } else if (verdict == MEM_SOFT_EDGE) {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s' has a port on the falling clock edge; left to lower",
                name);
    } else {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s' (%" PRIu32 " read, %" PRIu32
                " write ports) has no fixed-subset mapping; left to lower",
                name, mm->n_rd, mm->n_wr);
    }
}

/* --- building ------------------------------------------------------------------------------ */

/* The nets behind the pin vecs of one hm_ram_desc. */
typedef struct ram_nets {
    odin3_net_id clk;
    odin3_net_id none;
    odin3_net_id gnd;
    odin3_vec pad; /* odin3_net_id x WIDTH: the pad, for the data pins of a port without a write */
    odin3_vec raw[RAM_MAX_PORTS]; /* odin3_net_id x WIDTH: the RAM's output of a port with an
                                     emulated read enable (R-10), else empty */
} ram_nets;

/* The pins of RAM port port_idx from the read and write port it carries. */
static void fill_port(const mem_model *mm, const mem_ram *ram, ram_nets *rn, hm_ram_desc *desc,
                      uint32_t port_idx) {
    const ram_layout *lay = ram_layout_of(ram->kind);
    const mem_rport *rp =
        ram->rd[port_idx] >= 0 ? odin3_vec_cat(&mm->rd, (size_t)ram->rd[port_idx]) : NULL;
    const mem_wport *wp =
        ram->wr[port_idx] >= 0 ? odin3_vec_cat(&mm->wr, (size_t)ram->wr[port_idx]) : NULL;
    const odin3_net_id *pad = odin3_vec_cat(&rn->pad, 0);
    desc->read[port_idx] = rp != NULL;
    desc->pin[lay->addr[port_idx]] = (odin3_netvec){wp != NULL ? wp->addr : rp->addr, mm->abits};
    desc->pin[lay->data[port_idx]] =
        (odin3_netvec){wp != NULL ? odin3_vec_cat(&wp->data, 0) : pad, mm->width};
    desc->pin[lay->we[port_idx]] = (odin3_netvec){wp != NULL ? &wp->en : &rn->gnd, 1};
    desc->pin[lay->out[port_idx]] = (odin3_netvec){
        rp != NULL ? odin3_vec_cat(&rp->data, 0) : &rn->none, rp != NULL ? mm->width : 0};
}

enum { ROLE_NAME = 48 };

/* A read port with an enable the RAM cannot take (R-10, the arm_core case `if (we) out <=
 * RAM[addr]`): the RAM reads every cycle, so the enable is registered with the read, and the
 * output is the RAM's word when that registered enable is 1 and else the previous output. As
 * Yosys's memory_libmap `emu_en` does, exactly: out(t+1) = en(t) ? mem[addr(t)] : out(t). */
typedef struct emu_port {
    const mem_rport *rp;
    const odin3_net_id *raw; /* the RAM's output nets, one per data bit */
    uint32_t tag;            /* the read port's number, for names */
} emu_port;

static odin3_status emu_bit(hm_ctx *hc, const emu_port *ep, odin3_net_id en_r, uint32_t bit,
                            odin3_vec *ties) {
    char role[ROLE_NAME];
    odin3_net_id hold = {0};
    odin3_net_id next = {0};
    odin3_net_id held = {0};
    (void)snprintf(role, sizeof role, "rd%" PRIu32 ".hold%" PRIu32, ep->tag, bit);
    odin3_status st = hm_fresh_nets(hc, 1, &hold);
    const odin3_net_id mux_in[2] = {hold, ep->raw[bit]};
    (void)snprintf(role, sizeof role, "rd%" PRIu32 ".sel%" PRIu32, ep->tag, bit);
    st = st == ODIN3_OK ? hm_mux(hc, role, en_r, mux_in, &next) : st;
    const odin3_net_id dff_in[2] = {ep->rp->clk, next};
    (void)snprintf(role, sizeof role, "rd%" PRIu32 ".hff%" PRIu32, ep->tag, bit);
    st = st == ODIN3_OK ? hm_dff(hc, role, dff_in, &held) : st;
    if (st == ODIN3_OK) {
        /* reserved by emulate_en */
        *(hm_tie *)odin3_vec_push(ties) = (hm_tie){hold, held};
        *(hm_tie *)odin3_vec_push(ties) =
            (hm_tie){*(const odin3_net_id *)odin3_vec_cat(&ep->rp->data, bit), next};
    }
    return st;
}

static odin3_status emulate_en(hm_ctx *hc, const emu_port *ep, odin3_vec *ties) {
    char role[ROLE_NAME];
    odin3_net_id en = ep->rp->en;
    odin3_net_id en_r = {0};
    odin3_status st = odin3_vec_reserve(ties, ties->len + 2U * ep->rp->data.len);
    if (st == ODIN3_OK && !ep->rp->en_pos) {
        (void)snprintf(role, sizeof role, "rd%" PRIu32 ".enn", ep->tag);
        st = hm_not(hc, role, en, &en);
    }
    const odin3_net_id en_in[2] = {ep->rp->clk, en};
    (void)snprintf(role, sizeof role, "rd%" PRIu32 ".enq", ep->tag);
    st = st == ODIN3_OK ? hm_dff(hc, role, en_in, &en_r) : st;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < ep->rp->data.len; bit++) {
        st = emu_bit(hc, ep, en_r, bit, ties);
    }
    return st;
}

/* The read port that RAM port port_idx carries when its enable is emulated, else NULL. */
static const mem_rport *emulated_port(const mem_model *mm, const mem_ram *ram, uint32_t port_idx) {
    if (ram->rd[port_idx] < 0) {
        return NULL;
    }
    const mem_rport *rp = odin3_vec_cat(&mm->rd, (size_t)ram->rd[port_idx]);
    return rp->emu_en ? rp : NULL;
}

/* The RAM drives fresh nets raw[port_idx] instead of the port's data nets (emulate_en puts the
 * hold logic between them). */
static odin3_status setup_raw(hm_ctx *hc, const mem_model *mm, ram_nets *rn, hm_ram_desc *desc,
                              uint32_t pin) {
    odin3_vec *raw = &rn->raw[pin];
    odin3_status st = odin3_vec_reserve(raw, mm->width);
    for (uint32_t j = 0; st == ODIN3_OK && j < mm->width; j++) {
        *(odin3_net_id *)odin3_vec_push(raw) = (odin3_net_id){0};
    }
    st = st == ODIN3_OK ? hm_fresh_nets(hc, mm->width, odin3_vec_at(raw, 0)) : st;
    if (st == ODIN3_OK) {
        desc->pin[ram_layout_of(desc->kind)->out[pin]] =
            (odin3_netvec){odin3_vec_at(raw, 0), mm->width};
    }
    return st;
}

static odin3_status build_desc(hm_ctx *hc, const mem_model *mm, const mem_ram *ram, ram_nets *rn,
                               hm_ram_desc *desc) {
    const ram_layout *lay = ram_layout_of(ram->kind);
    const mem_rport *first = odin3_vec_cat(&mm->rd, 0);
    odin3_status st = ODIN3_OK;
    memset(desc, 0, sizeof *desc);
    desc->kind = ram->kind;
    desc->addr_bits = mm->abits;
    desc->data_bits = mm->width;
    rn->clk = first->clk;
    desc->pin[lay->clk] = (odin3_netvec){&rn->clk, 1};
    for (uint32_t port_idx = 0; st == ODIN3_OK && port_idx < lay->n_ports; port_idx++) {
        fill_port(mm, ram, rn, desc, port_idx);
        st = emulated_port(mm, ram, port_idx) != NULL ? setup_raw(hc, mm, rn, desc, port_idx) : st;
    }
    return st;
}

/* The hold logic of every emulated read enable of the RAM, after the RAM itself is built. */
static odin3_status emulate_ports(hm_ctx *hc, const mem_model *mm, const mem_ram *ram,
                                  const ram_nets *rn, odin3_vec *ties) {
    odin3_status st = ODIN3_OK;
    for (uint32_t port_idx = 0; st == ODIN3_OK && port_idx < RAM_MAX_PORTS; port_idx++) {
        const mem_rport *rp = emulated_port(mm, ram, port_idx);
        if (rp != NULL) {
            const emu_port ep = {rp, odin3_vec_cat(&rn->raw[port_idx], 0),
                                 (uint32_t)ram->rd[port_idx]};
            st = emulate_en(hc, &ep, ties);
        }
    }
    return st;
}

/* Builds one planned RAM into the open transaction. */
static odin3_status build_ram(hm_ctx *hc, const mem_model *mm, const mem_ram *ram, odin3_vec *ties,
                              uint64_t *slices) {
    ram_nets rn;
    memset(&rn, 0, sizeof rn);
    odin3_vec_init(&rn.pad, sizeof(odin3_net_id));
    for (uint32_t i = 0; i < RAM_MAX_PORTS; i++) {
        odin3_vec_init(&rn.raw[i], sizeof(odin3_net_id));
    }
    odin3_net_id pad = {0};
    odin3_status st = hm_const(hc, HM_PAD, &pad);
    st = st == ODIN3_OK ? hm_const(hc, HM_GND, &rn.gnd) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&rn.pad, mm->width) : st;
    for (uint32_t j = 0; st == ODIN3_OK && j < mm->width; j++) {
        *(odin3_net_id *)odin3_vec_push(&rn.pad) = pad;
    }
    hm_ram_desc desc;
    st = st == ODIN3_OK ? build_desc(hc, mm, ram, &rn, &desc) : st;
    st = st == ODIN3_OK ? hm_build_ram(hc, &desc, ties, slices) : st;
    st = st == ODIN3_OK ? emulate_ports(hc, mm, ram, &rn, ties) : st;
    odin3_vec_free(&rn.pad);
    for (uint32_t i = 0; i < RAM_MAX_PORTS; i++) {
        odin3_vec_free(&rn.raw[i]);
    }
    return st;
}

/* All the RAMs of the plan, then the commit. */
static odin3_status map_hard(hm_ctx *hc, const mem_model *mm) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(hm_tie));
    uint64_t slices[HM_N_HARD] = {0};
    odin3_status st = hm_begin_cell(hc, mm->node);
    for (size_t i = 0; st == ODIN3_OK && i < mm->dead.len; i++) {
        st = hm_doom(hc, *(const odin3_node_id *)odin3_vec_cat(&mm->dead, i));
    }
    for (size_t i = 0; st == ODIN3_OK && i < mm->rams.len; i++) {
        const mem_ram *ram = odin3_vec_cat(&mm->rams, i);
        st = build_ram(hc, mm, ram, &ties, &slices[ram->kind]);
    }
    if (st != ODIN3_OK) {
        hm_abort(hc);
        odin3_vec_free(&ties);
        return st;
    }
    if (mm->init_defined) {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s': initial contents dropped (the arch's RAMs have no init)",
                mem_name(hc, mm));
    }
    for (uint32_t i = 0; i < mm->n_rd; i++) {
        if (((const mem_rport *)odin3_vec_cat(&mm->rd, i))->emu_en) {
            hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                    "partial_map: memory '%s': read port %" PRIu32
                    " has an enable that is not constant 1; the RAM has none, so the enable is "
                    "emulated in logic (a register of the enable and a hold register on the "
                    "output), as Yosys's memory_libmap emu_en does",
                    mem_name(hc, mm), i);
        }
    }
    if (mm->n_wr > 1) {
        hm_diag(hc, ODIN3_LOG_WARN, mm->node,
                "partial_map: memory '%s': a same-address write collision of its write ports is "
                "undefined in the arch's RAM (the `$mem` lets the later port win)",
                mem_name(hc, mm));
    }
    st = hm_commit(hc, odin3_vec_cat(&ties, 0), ties.len);
    if (st == ODIN3_OK) {
        hc->stats.sp_slices += slices[HM_SP_RAM];
        hc->stats.dp_slices += slices[HM_DP_RAM];
    }
    odin3_vec_free(&ties);
    return st;
}

odin3_status hm_map_mem(hm_ctx *hc, odin3_node_id src) {
    mem_model mm;
    mem_verdict verdict = MEM_SOFT_PATTERN;
    pm_memattr_warn(hc, src, "partial_map");
    odin3_status st = mem_load(hc, src, &mm);
    st = st == ODIN3_OK && !mm.too_wide ? mem_merge(hc, &mm) : st;
    st = st == ODIN3_OK ? mem_classify(hc, &mm, &verdict) : st;
    if (st == ODIN3_OK && verdict == MEM_HARD) {
        st = map_hard(hc, &mm);
    } else if (st == ODIN3_OK) {
        warn_soft(hc, &mm, verdict);
        pm_mem_warn_big(hc, src, soft_reason(verdict));
        hc->stats.soft++;
    }
    mem_model_free(&mm);
    return st;
}
