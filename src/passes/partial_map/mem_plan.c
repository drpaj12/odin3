/*
 * mem_plan.c — what hardmap does with an inferred `$mem` after R-8 (3C spec R-9, R-10, R-11): the
 * verdict (hard, or soft and why) and, for hard, the RAMs to build: the port patterns of the R-10
 * table with `r` read and `w` write ports. Nothing here changes the design. Plan agent defaults:
 * OFFSET != 0 and a falling-edge port stay soft (e, f); a write enable that is not one net, an
 * asynchronous write port and a read-during-write port stay soft (R-11); more than 31 address
 * bits stay soft (R-11).
 */
#include "passes/partial_map/mem.h"

#include "ir/celltype.h"

enum { NONE = -1 };

static const mem_rport *rd_at(const mem_model *mm, uint32_t idx) {
    return odin3_vec_cat(&mm->rd, idx);
}

static const mem_wport *wr_at(const mem_model *mm, uint32_t idx) {
    return odin3_vec_cat(&mm->wr, idx);
}

/* The same net, or two constants of the same 0/1 value, or both open. */
static bool same_net(const hm_ctx *hc, odin3_net_id lhs, odin3_net_id rhs) {
    if (!odin3_net_valid(lhs) || !odin3_net_valid(rhs)) {
        return !odin3_net_valid(lhs) && !odin3_net_valid(rhs);
    }
    if (lhs.v == rhs.v) {
        return true;
    }
    odin3_const lhs_value = odin3_net_const_value(hc->module, lhs);
    return lhs_value == odin3_net_const_value(hc->module, rhs) &&
           (lhs_value == ODIN3_CONST_0 || lhs_value == ODIN3_CONST_1);
}

static bool same_addr(const hm_ctx *hc, const odin3_net_id *lhs, const odin3_net_id *rhs,
                      uint32_t abits) {
    for (uint32_t i = 0; i < abits; i++) {
        if (!same_net(hc, lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

/* --- the verdict --------------------------------------------------------------------------- */

typedef struct clock_dom {
    bool set;
    bool posedge;
    odin3_net_id clk;
} clock_dom;

/* Notes one clocked port; false when it is not in the domain of the ones before. */
static bool dom_note(clock_dom *dom, odin3_net_id clk, bool posedge) {
    if (!dom->set) {
        *dom = (clock_dom){true, posedge, clk};
        return true;
    }
    return dom->clk.v == clk.v && dom->posedge == posedge;
}

/* R-9: a read port that is still asynchronous, or ports on more than one clock domain; an
 * asynchronous or unclocked write port is R-11; a single falling-edge domain is agent default f. */
static mem_verdict clock_verdict(const mem_model *mm) {
    clock_dom dom = {false, true, {0}};
    bool one = true;
    for (uint32_t i = 0; i < mm->n_rd; i++) {
        const mem_rport *rp = rd_at(mm, i);
        if (!rp->sync) {
            return MEM_SOFT_ASYNC;
        }
        if (!odin3_net_valid(rp->clk)) {
            return MEM_SOFT_PATTERN;
        }
        one = dom_note(&dom, rp->clk, rp->posedge) && one;
    }
    for (uint32_t i = 0; i < mm->n_wr; i++) {
        const mem_wport *wp = wr_at(mm, i);
        if (!wp->sync || !odin3_net_valid(wp->clk)) {
            return MEM_SOFT_PATTERN;
        }
        one = dom_note(&dom, wp->clk, wp->posedge) && one;
    }
    if (!one) {
        return MEM_SOFT_ASYNC;
    }
    return !dom.posedge ? MEM_SOFT_EDGE : MEM_HARD;
}

/* True when the port reads every cycle: no enable, or an enable that is constant active. */
static bool read_always(const hm_ctx *hc, const mem_rport *rp) {
    if (rp->en_always) {
        return true;
    }
    if (!odin3_net_valid(rp->en)) {
        return false;
    }
    return odin3_net_const_value(hc->module, rp->en) ==
           (rp->en_pos ? ODIN3_CONST_1 : ODIN3_CONST_0);
}

/* The checks that need no allocation. */
static mem_verdict early_verdict(const mem_model *mm) {
    if (mm->offset != 0) {
        return MEM_SOFT_OFFSET;
    }
    if (mm->too_wide) {
        return MEM_SOFT_PATTERN;
    }
    mem_verdict clocks = clock_verdict(mm);
    if (clocks != MEM_HARD) {
        return clocks;
    }
    for (uint32_t i = 0; i < mm->n_rd; i++) {
        const mem_rport *rp = rd_at(mm, i);
        if (rp->transparent && mm->n_wr > 0) {
            return MEM_SOFT_PATTERN; /* the arch's RAMs are read-first */
        }
    }
    for (uint32_t i = 0; i < mm->n_wr; i++) {
        if (!wr_at(mm, i)->en_ok) {
            return MEM_SOFT_PATTERN;
        }
    }
    return MEM_HARD;
}

/* --- the pattern table (R-10) -------------------------------------------------------------- */

typedef struct pattern {
    const hm_ctx *hc;
    mem_model *mm;
    bool fits;
} pattern;

static odin3_status add_ram(pattern *pat, hm_hard kind, const int32_t rd[2], const int32_t wr[2]) {
    mem_ram *slot = odin3_vec_push(&pat->mm->rams);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (mem_ram){kind, {rd[0], rd[1]}, {wr[0], wr[1]}};
    return ODIN3_OK;
}

/* Read port rd and write port wr have the same address. */
static bool same_rw(const pattern *pat, int32_t rd, int32_t wr) {
    const mem_model *mm = pat->mm;
    return same_addr(pat->hc, rd_at(mm, (uint32_t)rd)->addr, wr_at(mm, (uint32_t)wr)->addr,
                     mm->abits);
}

/* w = 0: a ROM. One SP for one port, one DP for two, one SP replica per port above two. */
static odin3_status pat_rom(pattern *pat) {
    const int32_t no_wr[2] = {NONE, NONE};
    uint32_t reads = pat->mm->n_rd;
    odin3_status st = ODIN3_OK;
    if (reads == 2) {
        const int32_t rd[2] = {0, 1};
        return add_ram(pat, HM_DP_RAM, rd, no_wr);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < reads; i++) {
        const int32_t rd[2] = {(int32_t)i, NONE};
        st = add_ram(pat, HM_SP_RAM, rd, no_wr);
    }
    return st;
}

/* w = 1, r = 1: SP when the addresses are identical, else DP (write on port 1, read on port 2). */
static odin3_status pat_1w_1r(pattern *pat) {
    const int32_t wr[2] = {0, NONE};
    if (same_rw(pat, 0, 0)) {
        const int32_t rd[2] = {0, NONE};
        return add_ram(pat, HM_SP_RAM, rd, wr);
    }
    const int32_t rd[2] = {NONE, 0};
    return add_ram(pat, HM_DP_RAM, rd, wr);
}

/* w = 1, r = 2: the read with the identical address shares port 1 with the write; else one DP
 * replica per read. r > 2: one DP replica per read (D8). */
static odin3_status pat_1w_nr(pattern *pat) {
    const int32_t wr[2] = {0, NONE};
    uint32_t reads = pat->mm->n_rd;
    for (uint32_t i = 0; reads == 2 && i < 2; i++) {
        if (same_rw(pat, (int32_t)i, 0)) {
            const int32_t rd[2] = {(int32_t)i, (int32_t)(1 - i)};
            return add_ram(pat, HM_DP_RAM, rd, wr);
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < reads; i++) {
        const int32_t rd[2] = {NONE, (int32_t)i};
        st = add_ram(pat, HM_DP_RAM, rd, wr);
    }
    return st;
}

/* w = 2, r = 1: the read goes on the port whose write address is identical. */
static odin3_status pat_2w_1r(pattern *pat) {
    const int32_t wr[2] = {0, 1};
    for (int32_t k = 0; k < 2; k++) {
        if (same_rw(pat, 0, k)) {
            const int32_t rd[2] = {k == 0 ? 0 : NONE, k == 1 ? 0 : NONE};
            return add_ram(pat, HM_DP_RAM, rd, wr);
        }
    }
    pat->fits = false;
    return ODIN3_OK;
}

/* w = 2, r = 2: each port pairs a write with the read of identical address. */
static odin3_status pat_2w_2r(pattern *pat) {
    const int32_t wr[2] = {0, 1};
    if (same_rw(pat, 0, 0) && same_rw(pat, 1, 1)) {
        const int32_t rd[2] = {0, 1};
        return add_ram(pat, HM_DP_RAM, rd, wr);
    }
    if (same_rw(pat, 0, 1) && same_rw(pat, 1, 0)) {
        const int32_t rd[2] = {1, 0};
        return add_ram(pat, HM_DP_RAM, rd, wr);
    }
    pat->fits = false;
    return ODIN3_OK;
}

static odin3_status mem_pattern(pattern *pat) {
    uint32_t reads = pat->mm->n_rd;
    uint32_t writes = pat->mm->n_wr;
    if (reads == 0 || writes > 2 || (writes == 2 && reads > 2)) {
        pat->fits = false;
        return ODIN3_OK;
    }
    if (writes == 0) {
        return pat_rom(pat);
    }
    if (writes == 1) {
        return reads == 1 ? pat_1w_1r(pat) : pat_1w_nr(pat);
    }
    return reads == 1 ? pat_2w_1r(pat) : pat_2w_2r(pat);
}

odin3_status mem_classify(const hm_ctx *hc, mem_model *mm, mem_verdict *verdict) {
    *verdict = early_verdict(mm);
    if (*verdict != MEM_HARD) {
        return ODIN3_OK;
    }
    for (uint32_t i = 0; i < mm->n_rd; i++) {
        mem_rport *rp = odin3_vec_at(&mm->rd, i);
        rp->emu_en = !read_always(hc, rp); /* the RAM has no read enable: emulated (R-10) */
    }
    pattern pat = {hc, mm, true};
    odin3_status st = mem_pattern(&pat);
    for (size_t i = 0; st == ODIN3_OK && pat.fits && i < mm->rams.len; i++) {
        /* R-6: a RAM the arch has no model for (SP only, or DP only) is R-11: no mapping */
        const mem_ram *ram = odin3_vec_cat(&mm->rams, i);
        pat.fits = ram->kind == HM_SP_RAM ? hc->rules.have_sp : hc->rules.have_dp;
    }
    if (st == ODIN3_OK && !pat.fits) {
        *verdict = MEM_SOFT_PATTERN;
    }
    return st;
}
