/*
 * pg_memw.c — writes to memories in processes (2D Task 24; EL-39 v4): `m[i] <= d` in a block
 * with one edge event becomes one write port of the memory's `$mem`, whose address, data and
 * enable are the values of up to three process-local variables at the end of the block, as Yosys
 * 0.55's simplify.cc 3150-3330 makes them: `addr` (the wider of the index and ABITS), `data` (the
 * word's width) and `en`, each assigned `x`, `x` and `0` at the start of the block and by the
 * write statement `i`, `d` and all ones. A constant index needs no `addr` and a constant
 * full-word value no `data`: the constant goes to the port. The variables have no sync action:
 * they are `O` pins of the `$proc` that `proc` drives combinationally (EL-33).
 *
 * The statement may be inside any number of open `if`/`case` switches whose targets the scan could
 * not know (a loop or a callee makes a variable per execution), so a variable joins every open
 * switch when it is made (pg_late): each level has case temporaries of its own, entry actions in
 * every case of the switch (those already built get them first in their lists), and an exit
 * action after the switch, exactly what a variable the scan had listed would have.
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MW_ADDR = 0, MW_DATA = 1, MW_EN = 2, MW_VARS = 3, MW_MIN_ABITS = 1, MW_ABITS_LIMIT = 31 };

/* One variable of a write: whether it exists, its width, the value the statement gives it, and
 * the first of its nets (the process-local variable). */
typedef struct mw_var {
    bool used;
    odin3_bit init;
    uint32_t width;
    odin3_net_id net;
    odin3_sig value;
} mw_var;

typedef struct mw_write {
    odin3_pg *pg;
    odin3_ast_id stmt;
    uint32_t mem;
    mw_var var[MW_VARS];
} mw_write;

/* --- the late variables (switch levels) ---------------------------------------------------------
 */

static uint32_t top_level(const odin3_pg *pg) {
    return (uint32_t)pg->sw_ctx.len - 1U;
}

uint32_t pg_late_count(const odin3_pg *pg) {
    uint32_t count = 0;
    for (size_t i = 0; pg->sw_ctx.len > 0 && i < pg->late.len; i++) {
        count += ((const pg_late *)odin3_vec_cat(&pg->late, i))->level == top_level(pg) ? 1U : 0U;
    }
    return count;
}

/* An action over `width` consecutive nets from lhs, with the right side bits rhs (or the constant
 * `fill` when rhs is NULL), in case cs. */
typedef struct late_act {
    uint32_t cs, width;
    odin3_sigbit lhs;
    const odin3_sigbit *rhs; /* width bits, or NULL */
    odin3_bit fill;
    bool front;
} late_act;

static odin3_status late_add(odin3_pg *pg, const late_act *la) {
    odin3_sigbit *lhs = odin3_arena_alloc(pg->e2->scratch, (size_t)la->width * 2U * sizeof *lhs);
    if (lhs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_sigbit *rhs = lhs + la->width;
    for (uint32_t k = 0; k < la->width; k++) {
        lhs[k] = la->lhs + k;
        rhs[k] = la->rhs != NULL ? la->rhs[k] : odin3_sb_of_bit(la->fill);
    }
    pg_act_req req = {la->cs, 0, lhs, rhs, la->width, pg->prov, la->front, false};
    return pg_act_add(pg, &req);
}

odin3_status pg_late_entry(odin3_pg *pg, bool x_entry) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < pg->late.len; i++) {
        const pg_late rec = *(const pg_late *)odin3_vec_cat(&pg->late, i);
        if (rec.level == top_level(pg)) {
            late_act la = {pg->cur, rec.width, rec.t1, NULL, x_entry ? ODIN3_BIT_X : rec.init,
                           false};
            st = late_add(pg, &la);
        }
    }
    return st;
}

/* The exit action of one late variable: its enclosing level's nets take its case temporaries, the
 * earlier actions on them in the current case removed. */
static odin3_status late_exit_one(odin3_pg *pg, const pg_late *rec) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < rec->width; k++) {
        st = pg_remove(pg, (odin3_net_id){rec->outer + k});
    }
    odin3_sigbit *right =
        st == ODIN3_OK ? odin3_arena_alloc(pg->e2->scratch, rec->width * sizeof *right) : NULL;
    if (st == ODIN3_OK && right == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < rec->width; k++) {
        right[k] = rec->t1 + k;
    }
    late_act la = {pg->cur, rec->width, rec->outer, right, ODIN3_BIT_X, false};
    return st != ODIN3_OK ? st : late_add(pg, &la);
}

/* After a switch: its late variables' exit actions in the (parent) current case, then they are
 * forgotten (the records of the level are removed). */
odin3_status pg_late_exit(odin3_pg *pg) {
    odin3_status st = ODIN3_OK;
    size_t kept = 0;
    for (size_t i = 0; i < pg->late.len; i++) {
        const pg_late rec = *(const pg_late *)odin3_vec_cat(&pg->late, i);
        if (rec.level != top_level(pg)) {
            *(pg_late *)odin3_vec_at(&pg->late, kept++) = rec;
        } else if (st == ODIN3_OK) {
            st = late_exit_one(pg, &rec);
        }
    }
    while (pg->late.len > kept) {
        odin3_vec_pop(&pg->late);
    }
    return st;
}

/* --- making the variables of one write ----------------------------------------------------------
 */

/* The case temporaries of every open switch for a variable, and the records. */
static odin3_status mw_levels(mw_write *mw, mw_var *var) {
    odin3_pg *pg = mw->pg;
    uint32_t depth = (uint32_t)pg->sw_ctx.len;
    odin3_sigbit outer = var->net.v;
    odin3_status st = ODIN3_OK;
    for (uint32_t level = 0; st == ODIN3_OK && level < depth; level++) {
        odin3_net_id first = {0};
        st = odin3_pg_temp(pg, var->width, mw->stmt, &first);
        pg_late *rec = st == ODIN3_OK ? odin3_vec_push(&pg->late) : NULL;
        if (st == ODIN3_OK && rec == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        }
        if (rec != NULL) {
            *rec = (pg_late){level, var->width, var->net.v, first.v, outer, var->init};
            outer = first.v;
        }
    }
    return st;
}

/* The nets the statement assigns: the innermost level's case temporaries, or the variable. */
static odin3_sigbit assign_net(const odin3_pg *pg, const mw_var *var) {
    odin3_sigbit net = var->net.v;
    for (size_t i = 0; i < pg->late.len; i++) {
        const pg_late *rec = odin3_vec_cat(&pg->late, i);
        if (rec->net == var->net.v && rec->level + 1U == pg->sw_ctx.len) {
            net = rec->t1;
        }
    }
    return net;
}

/* The start-of-block action of a variable (in the root) and the entry action of each level in
 * every case its switch has now, all at the front of their cases. */
static odin3_status mw_entries(mw_write *mw, const mw_var *var) {
    odin3_pg *pg = mw->pg;
    late_act root = {PG_ROOT, var->width, var->net.v, NULL, var->init, true};
    odin3_status st = late_add(pg, &root);
    for (size_t i = 0; st == ODIN3_OK && i < pg->late.len; i++) {
        const pg_late rec = *(const pg_late *)odin3_vec_cat(&pg->late, i);
        if (rec.net != var->net.v) {
            continue;
        }
        const pg_swctx *ctx = odin3_vec_cat(&pg->sw_ctx, rec.level);
        const pg_switch *sw = odin3_vec_cat(&pg->sws, ctx->sw - 1U);
        for (uint32_t cs = sw->first_case; st == ODIN3_OK && cs != 0;
             cs = ((const pg_case *)odin3_vec_cat(&pg->cases, cs - 1U))->next) {
            late_act la = {cs - 1U, rec.width, rec.t1, NULL, rec.init, true};
            st = late_add(pg, &la);
        }
    }
    return st;
}

/* The variable's nets and, per open switch, its case temporaries. */
static odin3_status mw_make(mw_write *mw, mw_var *var, odin3_bit init, uint32_t width) {
    var->used = true;
    var->init = init;
    var->width = width;
    odin3_status st = odin3_pg_temp(mw->pg, width, mw->stmt, &var->net);
    return st != ODIN3_OK ? st : mw_levels(mw, var);
}

/* The statement's own assignment of a variable: in the current case, the earlier actions on the
 * same nets there removed. */
static odin3_status mw_assign(mw_write *mw, const mw_var *var) {
    odin3_pg *pg = mw->pg;
    odin3_sigbit target = assign_net(pg, var);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < var->width; k++) {
        st = pg_remove(pg, (odin3_net_id){target + k});
    }
    odin3_prov_id prov = {0};
    st = st != ODIN3_OK ? st : pg_stmt_prov(pg, mw->stmt, &prov);
    odin3_sigbit *lhs =
        st == ODIN3_OK ? odin3_arena_alloc(pg->e2->scratch, var->width * sizeof *lhs) : NULL;
    if (st == ODIN3_OK && lhs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < var->width; k++) {
        lhs[k] = target + k;
    }
    pg_act_req req = {pg->cur, 0, lhs, var->value.bits, var->width, prov, false, false};
    return st != ODIN3_OK ? st : pg_act_add(pg, &req);
}

/* --- the values --------------------------------------------------------------------------------
 */

static bool sig_is_const(const odin3_sig *sig) {
    for (uint32_t k = 0; k < sig->width; k++) {
        if ((sig->bits[k] & ODIN3_SB_CONST) == 0) {
            return false;
        }
    }
    return true;
}

/* The smallest address width that numbers `size` words (Yosys meminfo: at least 1). */
static uint32_t abits_min(uint32_t size) {
    uint32_t bits = MW_MIN_ABITS;
    while (bits < MW_ABITS_LIMIT && (1ULL << bits) < size) {
        bits++;
    }
    return bits;
}

/* The word positions the target writes (lvs.nets: position + 1), whether it is the whole word in
 * order, and the lowest one (UINT32_MAX: none). */
typedef struct mw_target {
    bool full;
    uint32_t low;
} mw_target;

static mw_target target_of(const odin3_pg *pg, uint32_t width) {
    mw_target tg = {pg->lvs.nets.len == width, UINT32_MAX};
    for (uint32_t j = 0; j < pg->lvs.nets.len; j++) {
        uint32_t pos = ((const odin3_net_id *)odin3_vec_cat(&pg->lvs.nets, j))->v;
        tg.full = tg.full && pos == j + 1U;
        tg.low = pos != 0 && pos - 1U < tg.low ? pos - 1U : tg.low;
    }
    return tg;
}

/* data and en for the target: the value's bits at their positions, x below the lowest one (Yosys
 * pads the low end with x), zero above; en is 1 at the positions only. */
static odin3_status mw_data_en(mw_write *mw, const odin3_sig *rhs, uint32_t width,
                               const mw_target *tg) {
    odin3_pg *pg = mw->pg;
    mw_var *data = &mw->var[MW_DATA];
    mw_var *en = &mw->var[MW_EN];
    odin3_status st = odin3_elab2_sig_new(pg->e2, width, &data->value);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(pg->e2, width, &en->value);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        data->value.bits[k] = odin3_sb_of_bit(k < tg->low ? ODIN3_BIT_X : ODIN3_BIT_0);
    }
    for (uint32_t j = (uint32_t)pg->lvs.nets.len; st == ODIN3_OK && j > 0; j--) {
        uint32_t pos = ((const odin3_net_id *)odin3_vec_cat(&pg->lvs.nets, j - 1U))->v;
        if (pos != 0) { /* a bit written twice keeps its first value */
            data->value.bits[pos - 1U] = rhs->bits[j - 1U];
            en->value.bits[pos - 1U] = odin3_sb_of_bit(ODIN3_BIT_1);
        }
    }
    return st;
}

/* The address: the raw index (a flattened array's row-major address). */
static odin3_status mw_address(mw_write *mw, odin3_sig *addr) {
    odin3_pg *pg = mw->pg;
    odin3_elab2 *e2 = pg->e2;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, mw->mem);
    odin3_sig *idx = odin3_arena_alloc(e2->scratch, ((size_t)mem->n_dims + 1U) * sizeof *idx);
    if (idx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_elab2_reads reads = pg_reads(pg);
    odin3_ast_id word = pg->lvs.memw.node;
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < mem->n_dims; k++) {
        odin3_elab2_expr_req req = {
            odin3_elab2_mem_word_index(e2, word, k), {0, false, false}, NULL, &reads};
        st = odin3_elab2_expr(e2, &req, &idx[k]);
    }
    odin3_elab2_mem_rd rd = {mw->mem, word, idx, mem->n_dims};
    return st != ODIN3_OK ? st : odin3_elab2_mem_addr(e2, &rd, addr);
}

/* --- the port -----------------------------------------------------------------------------------
 */

/* The signal a port pin group gets: the variable's nets, or the constant value. */
static odin3_status port_sig(mw_write *mw, const mw_var *var, odin3_sig *out) {
    if (!var->used) {
        *out = var->value;
        return ODIN3_OK;
    }
    odin3_status st = odin3_elab2_sig_new(mw->pg->e2, var->width, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < var->width; k++) {
        out->bits[k] = var->net.v + k;
    }
    return st;
}

/* The clock of the block: its one edge sync, or false. */
static bool mw_clock(const odin3_pg *pg, odin3_sigbit *signal, bool *posedge) {
    if (pg->syncs.len != 1) {
        return false;
    }
    const pg_sync *sync = odin3_vec_cat(&pg->syncs, 0);
    *signal = sync->signal;
    *posedge = sync->type == ODIN3_SYNC_POSEDGE;
    return sync->type == ODIN3_SYNC_POSEDGE || sync->type == ODIN3_SYNC_NEGEDGE;
}

static odin3_status mw_port(mw_write *mw, odin3_sigbit clock, bool posedge) {
    odin3_pg *pg = mw->pg;
    odin3_sig addr;
    odin3_sig data;
    odin3_sig en;
    odin3_net_id clk = {0};
    odin3_status st = port_sig(mw, &mw->var[MW_ADDR], &addr);
    st = st != ODIN3_OK ? st : port_sig(mw, &mw->var[MW_DATA], &data);
    st = st != ODIN3_OK ? st : port_sig(mw, &mw->var[MW_EN], &en);
    st = st != ODIN3_OK ? st : odin3_elab2_to_nets(pg->e2, &(odin3_sig){&clock, 1, false}, &clk);
    odin3_elab2_mem_wr wr = {mw->mem, &addr, &data, &en, clk, posedge, mw->stmt};
    return st != ODIN3_OK ? st : odin3_elab2_mem_write(pg->e2, &wr);
}

/* --- the write ----------------------------------------------------------------------------------
 */

/* The variables the statement needs, made in Yosys's order (addr, data, en). */
static odin3_status mw_variables(mw_write *mw, const odin3_elab_mem *mem, const mw_target *tg) {
    mw_var *addr = &mw->var[MW_ADDR];
    mw_var *data = &mw->var[MW_DATA];
    odin3_status st = ODIN3_OK;
    if (!sig_is_const(&addr->value)) {
        uint32_t width =
            addr->value.width > abits_min(mem->size) ? addr->value.width : abits_min(mem->size);
        st = odin3_elab2_extend(mw->pg->e2, &addr->value, width, addr->value.is_signed,
                                &addr->value);
        st = st != ODIN3_OK ? st : mw_make(mw, addr, ODIN3_BIT_X, width);
    }
    if (st == ODIN3_OK && !(tg->full && sig_is_const(&data->value))) {
        st = mw_make(mw, data, ODIN3_BIT_X, mem->width);
    }
    return st != ODIN3_OK ? st : mw_make(mw, &mw->var[MW_EN], ODIN3_BIT_0, mem->width);
}

/* The start-of-block actions, last variable first so that addr, data, en end up in that order at
 * the front of the root. */
static odin3_status mw_start(mw_write *mw) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = MW_VARS; st == ODIN3_OK && k > 0; k--) {
        st = mw->var[k - 1U].used ? mw_entries(mw, &mw->var[k - 1U]) : ODIN3_OK;
    }
    return st;
}

/* --- T9 on a memory word (PR 2D-7b review I3) ---------------------------------------------------
 *
 * The net records of T9 (pg.c) see nothing of a word whose address is a variable, so a memory
 * keeps records of its own under keys with bit 63 set: one per constant word, one for the
 * variable-address writes, and one for every write. A constant-address write is checked against
 * its word's and the variable-address writes' record of the other kind, a variable-address write
 * against any write of the other kind. */

enum {
    MK_FLAG = 63,
    MK_CLASS_SHIFT = 61,
    MK_MEM_SHIFT = 29,
    MK_WORD_BITS = 28,
    MK_MEM_ID_SHIFT = 32
};
typedef enum { MK_WORD = 0, MK_VAR = 1, MK_ANY = 2 } mk_class;

static uint64_t mem_key(uint32_t mem, mk_class cls, uint32_t word) {
    return ((uint64_t)1 << MK_FLAG) | ((uint64_t)cls << MK_CLASS_SHIFT) |
           ((uint64_t)mem << MK_MEM_SHIFT) | ((uint64_t)word << 1);
}

/* The constant word an address names (false: it has a bit that is not 0 or 1, or is too large). */
static bool const_word(const odin3_sig *addr, uint32_t *word) {
    uint64_t value = 0;
    for (uint32_t k = 0; addr != NULL && k < addr->width; k++) {
        odin3_sigbit sb = addr->bits[k];
        if (!odin3_sb_is_const(sb) ||
            (odin3_sb_bit(sb) != ODIN3_BIT_0 && odin3_sb_bit(sb) != ODIN3_BIT_1)) {
            return false;
        }
        if (odin3_sb_bit(sb) == ODIN3_BIT_1) {
            if (k >= MK_WORD_BITS) {
                return false;
            }
            value |= (uint64_t)1 << k;
        }
    }
    *word = (uint32_t)value;
    return addr != NULL;
}

odin3_status pg_mem_check_mixed(odin3_pg *pg, const pg_mem_ref *ref) {
    const odin3_elab2 *e2 = pg->e2;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref->mem);
    uint32_t word = 0;
    bool known = const_word(ref->addr, &word);
    uint64_t mine = ref->blocking ? 1U : 0U;
    pg_mixed_subject subj = {
        ((uint64_t)2 << MK_MEM_ID_SHIFT) | ref->mem,
        odin3_strtab_get(odin3_design_strtab(e2->design), odin3_symbol_get(e2->st, mem->sym)->name),
        ref->stmt};
    uint64_t own = known ? mem_key(ref->mem, MK_WORD, word) : mem_key(ref->mem, MK_VAR, 0);
    odin3_status st = pg_mixed_remember(pg, ref->stmt, own | mine);
    st =
        st != ODIN3_OK ? st : pg_mixed_remember(pg, ref->stmt, mem_key(ref->mem, MK_ANY, 0) | mine);
    if (st == ODIN3_OK && !known) {
        return pg_mixed_check(pg, &subj, (mem_key(ref->mem, MK_ANY, 0)) | (mine ^ 1U));
    }
    st = st != ODIN3_OK ? st : pg_mixed_check(pg, &subj, own | (mine ^ 1U));
    return st != ODIN3_OK ? st
                          : pg_mixed_check(pg, &subj, mem_key(ref->mem, MK_VAR, 0) | (mine ^ 1U));
}

odin3_status pg_mem_apply(odin3_pg *pg, const odin3_pg_assign_req *req) {
    odin3_elab2 *e2 = pg->e2;
    odin3_sigbit clock = 0;
    bool posedge = false;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, pg->lvs.memw.mem);
    if (!mw_clock(pg, &clock, &posedge)) {
        const odin3_symbol *rec = odin3_symbol_get(e2->st, mem->sym);
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, req->stmt),
                          "array '%s' cannot be a memory (it is written in a block without exactly "
                          "one edge) and (* nomem2reg *) is set",
                          odin3_strtab_get(odin3_design_strtab(e2->design), rec->name));
        return ODIN3_ERR_PARSE;
    }
    mw_target tg = target_of(pg, mem->width);
    if (tg.low == UINT32_MAX) {
        return ODIN3_OK; /* nothing of the word is written (the warning was the lvalue's) */
    }
    mw_write mw = {pg, req->stmt, pg->lvs.memw.mem, {{0}}};
    odin3_status st = mw_address(&mw, &mw.var[MW_ADDR].value);
    pg_mem_ref tref = {mw.mem, &mw.var[MW_ADDR].value, req->stmt, req->blocking};
    st = st != ODIN3_OK ? st : pg_mem_check_mixed(pg, &tref);
    st = st != ODIN3_OK ? st : mw_data_en(&mw, req->rhs, mem->width, &tg);
    st = st != ODIN3_OK ? st : mw_variables(&mw, mem, &tg);
    st = st != ODIN3_OK ? st : mw_start(&mw);
    for (uint32_t k = 0; st == ODIN3_OK && k < MW_VARS; k++) {
        st = mw.var[k].used ? mw_assign(&mw, &mw.var[k]) : ODIN3_OK;
    }
    return st != ODIN3_OK ? st : mw_port(&mw, clock, posedge);
}

odin3_status odin3_pg_mem_write(odin3_pg *pg, const odin3_pg_assign_req *req) {
    if (req == NULL || req->rhs == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_mem_write: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = pg_take_target_of(pg, req->lhs);
    if (st == ODIN3_OK && pg->lvs.memw.node.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_mem_write: the target is not a word of a memory");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK && req->rhs->width != pg->lvs.nets.len) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_mem_write: the value is %u bits; the target %zu",
                  req->rhs->width, pg->lvs.nets.len);
        return ODIN3_ERR_INVALID_ARG;
    }
    return st != ODIN3_OK ? st : pg_mem_apply(pg, req);
}

/* --- concatenation targets with a memory word ----------------------------------------------------
 */

/* Whether node is a word of a memory, or a select of one. */
static bool part_is_memw(const odin3_pg *pg, odin3_ast_id part) {
    const odin3_elab2 *e2 = pg->e2;
    odin3_elab2_wordref ref;
    odin3_ast_id word = part;
    if (odin3_ast_kind_of(e2->elab, part) != ODIN3_AST_SELECT) {
        return false;
    }
    if (!odin3_elab2_mem_word_of(e2, word, &ref)) {
        word = odin3_ast_child(e2->elab, part, 0);
        if (odin3_ast_kind_of(e2->elab, word) != ODIN3_AST_SELECT ||
            !odin3_elab2_mem_word_of(e2, word, &ref)) {
            return false;
        }
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref.mem);
    return mem != NULL && !mem->to_regs;
}

bool pg_lhs_has_memw(const odin3_pg *pg, odin3_ast_id lhs) {
    const odin3_ast *ast = pg->e2->elab;
    if (lhs.v == 0 || odin3_ast_kind_of(ast, lhs) != ODIN3_AST_CONCAT) {
        return false;
    }
    odin3_ast_span parts = odin3_ast_children(ast, lhs);
    for (uint32_t i = 0; i < parts.n; i++) {
        if (parts.ids[i].v != 0 && part_is_memw(pg, parts.ids[i])) {
            return true;
        }
    }
    return false;
}
