/*
 * pg.c — the process generator's builder (2D Task 21; EL-35 steps 2, 5 and 6): the generator's
 * lifetime, the `lv` and `rv` maps with their undo log, the pool of signal bits, actions with the
 * per-bit slot index that makes removal cost the number of earlier assignments to a bit, the
 * switch and case primitives, temporaries, and the assignment itself (chunks, v4.2). The block's
 * outputs, syncs and statements are pg_always.c; the renumbering and the node pg_emit.c.
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GROUP_SHIFT = 32 };

/* --- lifetime -------------------------------------------------------------------------------- */

/* The maps a block fills, with the address of each. */
static odin3_u64map **pg_maps(odin3_pg *pg, size_t index) {
    odin3_u64map **const all[] = {&pg->out_of,    &pg->group,     &pg->lv,    &pg->rv,
                                  &pg->slot_head, &pg->dead_nets, &pg->reads, &pg->listed,
                                  &pg->seen,      &pg->swl,       &pg->folds, &pg->active, &pg->assigned};
    return all[index];
}

enum { PG_MAP_COUNT = 13 };

/* An empty map: the same one when it is empty already. */
static odin3_status map_fresh(odin3_u64map **map) {
    if (*map != NULL && odin3_u64map_count(*map) == 0) {
        return ODIN3_OK;
    }
    odin3_u64map_destroy(*map);
    *map = odin3_u64map_create(0);
    return *map != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

static void vecs_init(odin3_pg *pg) {
    odin3_vec_init(&pg->outs, sizeof(pg_out));
    odin3_vec_init(&pg->chunks, sizeof(pg_chunk));
    odin3_vec_init(&pg->undo, sizeof(pg_undo));
    odin3_vec_init(&pg->pool, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->dead, sizeof(uint8_t));
    odin3_vec_init(&pg->cmps, sizeof(odin3_pspan));
    odin3_vec_init(&pg->acts, sizeof(pg_act));
    odin3_vec_init(&pg->cases, sizeof(pg_case));
    odin3_vec_init(&pg->sws, sizeof(pg_switch));
    odin3_vec_init(&pg->syncs, sizeof(pg_sync));
    odin3_vec_init(&pg->slots, sizeof(pg_slot));
    odin3_vec_init(&pg->frames, sizeof(pg_frame));
    odin3_vec_init(&pg->work, sizeof(odin3_ast_id));
    odin3_vec_init(&pg->items, sizeof(uint32_t));
    odin3_vec_init(&pg->runs, sizeof(uint32_t));
    odin3_vec_init(&pg->run_l, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->run_r, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->blk, sizeof(uint32_t));
    odin3_vec_init(&pg->blk_marks, sizeof(uint32_t));
    odin3_vec_init(&pg->sw_pool, sizeof(uint32_t));
    odin3_vec_init(&pg->sw_ctx, sizeof(pg_swctx));
    odin3_vec_init(&pg->late, sizeof(pg_late));
    odin3_vec_init(&pg->sw_nets, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->sw_t1, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->sw_rv, sizeof(odin3_sigbit));
    odin3_vec_init(&pg->cmp_sigs, sizeof(odin3_sig));
    odin3_vec_init(&pg->loops, sizeof(pg_loop));
    odin3_vec_init(&pg->calls, sizeof(pg_callrec));
    odin3_vec_init(&pg->fsyms, sizeof(uint32_t));
}

static void vecs_free(odin3_pg *pg) {
    odin3_vec *const all[] = {&pg->outs,   &pg->chunks,  &pg->undo,  &pg->pool,      &pg->dead,
                              &pg->cmps,   &pg->acts,    &pg->cases, &pg->sws,       &pg->syncs,
                              &pg->slots,  &pg->frames,  &pg->work,  &pg->items,     &pg->runs,
                              &pg->run_l,  &pg->run_r,   &pg->blk,   &pg->blk_marks, &pg->sw_pool,
                              &pg->sw_ctx, &pg->sw_nets, &pg->sw_t1, &pg->sw_rv,     &pg->cmp_sigs,
                              &pg->loops,  &pg->calls,   &pg->fsyms, &pg->late};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
}

void odin3_pg_destroy(odin3_pg *pg) {
    if (pg == NULL) {
        return;
    }
    for (size_t i = 0; i < PG_MAP_COUNT; i++) {
        odin3_u64map_destroy(*pg_maps(pg, i));
    }
    odin3_u64map_destroy(pg->kinds);
    odin3_u64map_destroy(pg->loopout);
    odin3_u64map_destroy(pg->tvis);
    vecs_free(pg);
    odin3_elab2_lvalue_free(&pg->lvs);
    odin3_elab2_lvalue_free(&pg->slv);
    odin3_util_free(pg);
}

void odin3_elab2_pg_free(odin3_elab2 *e2) {
    odin3_pg_destroy(e2->pg);
    e2->pg = NULL;
}

odin3_status pg_block_reset(odin3_pg *pg) {
    pg_frames_unwind(pg);
    odin3_vec *const lists[] = {
        &pg->outs,  &pg->chunks,    &pg->undo,    &pg->pool,   &pg->dead,    &pg->cmps,
        &pg->acts,  &pg->cases,     &pg->sws,     &pg->syncs,  &pg->slots,   &pg->frames,
        &pg->blk,   &pg->blk_marks, &pg->sw_pool, &pg->sw_ctx, &pg->sw_nets, &pg->sw_t1,
        &pg->sw_rv, &pg->cmp_sigs,  &pg->loops,   &pg->calls,  &pg->fsyms,   &pg->late};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        odin3_vec_clear(lists[i]);
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < PG_MAP_COUNT; i++) {
        st = map_fresh(pg_maps(pg, i));
    }
    pg->always = (odin3_ast_id){0};
    pg->prov = (odin3_prov_id){0};
    pg->edge = false;
    pg->track_reads = false;
    pg->failed = false;
    pg->scan_frame = false;
    pg->scanning = false;
    pg->has_memw = false;
    pg->groups = 0;
    odin3_u64map_destroy(pg->e2->bound);
    pg->e2->bound = NULL;
    pg->cur = PG_ROOT;
    if (st == ODIN3_OK && odin3_vec_push(&pg->cases) == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

odin3_status odin3_pg_create(odin3_elab2 *e2, odin3_pg **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (e2 == NULL || out == NULL || e2->mod == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_create: bad argument, or no module is emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pg *pg = odin3_util_calloc(sizeof *pg);
    if (pg == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    pg->e2 = e2;
    vecs_init(pg);
    odin3_elab2_lvalue_init(&pg->lvs);
    odin3_elab2_lvalue_init(&pg->slv);
    pg->lvs.procedural = true;
    pg->kinds = odin3_u64map_create(0);
    odin3_status st = pg->kinds != NULL ? pg_block_reset(pg) : ODIN3_ERR_NO_MEMORY;
    if (st != ODIN3_OK) {
        odin3_pg_destroy(pg);
        return st;
    }
    *out = pg;
    return ODIN3_OK;
}

/* --- lv and rv, with the undo log ------------------------------------------------------------ */

static odin3_status map_set(odin3_pg *pg, uint8_t which, uint64_t key, uint64_t value) {
    odin3_u64map *map = which == PG_UNDO_LV ? pg->lv : pg->rv;
    uint64_t old = 0;
    bool had = odin3_u64map_get(map, key, &old);
    pg_undo *entry = odin3_vec_push(&pg->undo);
    if (entry == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *entry = (pg_undo){which, had, key, old};
    odin3_status st = odin3_u64map_put(map, (odin3_kv){key, value});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&pg->undo);
    }
    return st;
}

bool pg_lv_get(const odin3_pg *pg, odin3_net_id net, odin3_net_id *out) {
    uint64_t value = 0;
    bool found = odin3_u64map_get(pg->lv, net.v, &value);
    out->v = found ? (uint32_t)value : 0;
    return found;
}

odin3_status pg_lv_set(odin3_pg *pg, odin3_net_id net, odin3_net_id target) {
    return map_set(pg, PG_UNDO_LV, net.v, target.v);
}

bool pg_rv_get(const odin3_pg *pg, odin3_net_id net, odin3_sigbit *out) {
    uint64_t value = 0;
    if (!odin3_u64map_get(pg->rv, net.v, &value)) {
        return false;
    }
    *out = (odin3_sigbit)value;
    return true;
}

odin3_status pg_rv_set(odin3_pg *pg, odin3_net_id net, odin3_sigbit value) {
    return map_set(pg, PG_UNDO_RV, net.v, value);
}

uint32_t odin3_pg_save(const odin3_pg *pg) {
    return (uint32_t)pg->undo.len;
}

void odin3_pg_restore(odin3_pg *pg, uint32_t mark) {
    while (pg->undo.len > mark) {
        const pg_undo *entry = odin3_vec_cat(&pg->undo, pg->undo.len - 1);
        odin3_u64map *map = entry->which == PG_UNDO_LV ? pg->lv : pg->rv;
        if (entry->had) {
            (void)odin3_u64map_put(map,
                                   (odin3_kv){entry->key, entry->old}); /* a live key: no growth */
        } else {
            (void)odin3_u64map_remove(map, entry->key);
        }
        odin3_vec_pop(&pg->undo);
    }
}

/* --- the pool, actions, slots and removal ---------------------------------------------------- */

odin3_status pg_pool_add(odin3_pg *pg, const odin3_sigbit *bits, uint32_t n, odin3_pspan *span) {
    odin3_status st = odin3_vec_reserve(&pg->pool, pg->pool.len + n);
    st = st != ODIN3_OK ? st : odin3_vec_reserve(&pg->dead, pg->dead.len + n);
    if (st != ODIN3_OK) {
        return st;
    }
    *span = (odin3_pspan){(uint32_t)pg->pool.len, n};
    for (uint32_t i = 0; i < n; i++) { /* room is reserved: the pushes cannot fail */
        *(odin3_sigbit *)odin3_vec_push(&pg->pool) = bits[i];
        (void)odin3_vec_push(&pg->dead);
    }
    return ODIN3_OK;
}

/* Index of the slot of lhs position pos of action act on net. */
static odin3_status slot_add(odin3_pg *pg, odin3_net_id net, uint32_t pos, uint32_t act) {
    uint64_t head = 0;
    (void)odin3_u64map_get(pg->slot_head, net.v, &head);
    pg_slot *slot = odin3_vec_push(&pg->slots);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (pg_slot){pos, act, (uint32_t)head};
    odin3_status st = odin3_u64map_put(pg->slot_head, (odin3_kv){net.v, pg->slots.len});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&pg->slots);
    }
    return st;
}

/* Links action number act (0-based) at the end of the list that starts at *first, ends at *last. */
static void list_link(odin3_pg *pg, uint32_t *first, uint32_t *last, uint32_t act) {
    if (*last != 0) {
        ((pg_act *)odin3_vec_at(&pg->acts, *last - 1U))->next = act + 1U;
    } else {
        *first = act + 1U;
    }
    *last = act + 1U;
}

/* Links action number act (0-based) at the start of the list. */
static void list_link_front(odin3_pg *pg, uint32_t *first, uint32_t *last, uint32_t act) {
    ((pg_act *)odin3_vec_at(&pg->acts, act))->next = *first;
    *first = act + 1U;
    *last = *last == 0 ? act + 1U : *last;
}

/* The slots of an action's lhs nets: each lhs net bit is assigned here. */
static odin3_status act_slots(odin3_pg *pg, const pg_act *act, uint32_t index) {
    const odin3_sigbit *lhs = odin3_vec_cat(&pg->pool, act->lhs.first);
    for (uint32_t i = 0; i < act->lhs.len; i++) {
        if ((lhs[i] & ODIN3_SB_CONST) != 0) {
            continue;
        }
        odin3_status st = slot_add(pg, (odin3_net_id){lhs[i]}, act->lhs.first + i, index);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The nets a user assignment's action drives: emit drops the outputs no such action drives, so a
 * variable-indexed loop write holds and claims only the bits it writes (PR 2D-6 review I2). */
static odin3_status note_assigned(odin3_pg *pg, const pg_act_req *req) {
    for (uint32_t i = 0; i < req->n; i++) {
        if ((req->lhs[i] & ODIN3_SB_CONST) == 0) {
            odin3_status st = odin3_u64map_put(pg->assigned, (odin3_kv){req->lhs[i], 1});
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status pg_act_add(odin3_pg *pg, const pg_act_req *req) {
    pg_act act = {{0, 0}, {0, 0}, req->prov, 0, req->cs};
    uint32_t index = (uint32_t)pg->acts.len;
    odin3_status st = pg_pool_add(pg, req->lhs, req->n, &act.lhs);
    st = st != ODIN3_OK ? st : pg_pool_add(pg, req->rhs, req->n, &act.rhs);
    pg_act *slot = st == ODIN3_OK ? odin3_vec_push(&pg->acts) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st; /* the pool keeps the bits: unreferenced, freed with the block */
    }
    *slot = act;
    if (!req->synth) {
        st = note_assigned(pg, req);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (req->cs == PG_SYNC_OWNER) {
        pg_sync *sync = odin3_vec_at(&pg->syncs, req->sync);
        list_link(pg, &sync->first_act, &sync->last_act, index);
        return ODIN3_OK;
    }
    pg_case *cs = odin3_vec_at(&pg->cases, req->cs);
    if (req->front) {
        list_link_front(pg, &cs->first_act, &cs->last_act, index);
    } else {
        list_link(pg, &cs->first_act, &cs->last_act, index);
    }
    return act_slots(pg, &act, index);
}

/* Whether case cs is root or inside root's subtree (a switch of root, a case of it, ...). */
static bool in_subtree(const odin3_pg *pg, uint32_t cs, uint32_t root) {
    while (cs != root) {
        if (cs == PG_ROOT || cs == PG_SYNC_OWNER) {
            return false;
        }
        cs = ((const pg_case *)odin3_vec_cat(&pg->cases, cs))->parent;
    }
    return true;
}

odin3_status pg_remove(odin3_pg *pg, odin3_net_id net) {
    uint64_t head = 0;
    if (!odin3_u64map_get(pg->slot_head, net.v, &head)) {
        return ODIN3_OK;
    }
    uint32_t kept_head = (uint32_t)head;
    uint32_t prev = 0;
    for (uint32_t at = (uint32_t)head; at != 0;) {
        const pg_slot *slot = odin3_vec_cat(&pg->slots, at - 1U);
        uint32_t next = slot->next;
        const pg_act *act = odin3_vec_cat(&pg->acts, slot->act);
        if (in_subtree(pg, act->cs, pg->cur)) {
            *(uint8_t *)odin3_vec_at(&pg->dead, slot->pos) = 1;
            if (prev == 0) {
                kept_head = next;
            } else {
                ((pg_slot *)odin3_vec_at(&pg->slots, prev - 1U))->next = next;
            }
        } else {
            prev = at;
        }
        at = next;
    }
    if (kept_head == (uint32_t)head) {
        return ODIN3_OK;
    }
    if (kept_head == 0) {
        (void)odin3_u64map_remove(pg->slot_head, net.v);
        return ODIN3_OK;
    }
    return odin3_u64map_put(pg->slot_head, (odin3_kv){net.v, kept_head});
}

/* --- switches and cases ---------------------------------------------------------------------- */

odin3_status pg_stmt_prov(odin3_pg *pg, odin3_ast_id stmt, odin3_prov_id *out) {
    if (stmt.v == 0) {
        *out = pg->prov;
        return ODIN3_OK;
    }
    odin3_elab2_prov_req req = {stmt, {0}, pg->e2->scope.v};
    return odin3_elab2_prov(pg->e2, &req, out);
}

odin3_status odin3_pg_switch(odin3_pg *pg, const odin3_pg_switch_req *req) {
    if (req == NULL || req->signal == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_switch: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    pg_switch sw = {{0, 0}, 0, 0, 0, req->flags, {0}};
    odin3_status st = pg_stmt_prov(pg, req->stmt, &sw.prov);
    st = st != ODIN3_OK ? st : pg_pool_add(pg, req->signal->bits, req->signal->width, &sw.signal);
    pg_switch *slot = st == ODIN3_OK ? odin3_vec_push(&pg->sws) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *slot = sw;
    uint32_t index = (uint32_t)pg->sws.len; /* 1-based */
    pg_case *cs = odin3_vec_at(&pg->cases, pg->cur);
    if (cs->last_sw != 0) {
        ((pg_switch *)odin3_vec_at(&pg->sws, cs->last_sw - 1U))->next = index;
    } else {
        cs->first_sw = index;
    }
    cs->last_sw = index;
    return ODIN3_OK;
}

/* Adds the compares of req to the pool and the compare list; *first is where the case's start. */
static odin3_status case_compares(odin3_pg *pg, const odin3_pg_case_req *req, uint32_t width,
                                  pg_case *cs) {
    cs->cmp_first = (uint32_t)pg->cmps.len;
    cs->cmp_n = req->n_compares;
    for (uint32_t i = 0; i < req->n_compares; i++) {
        if (req->compares[i].width != width) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_pg_case: a compare is %u bits; the signal is %u",
                      req->compares[i].width, width);
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_pspan span;
        odin3_status st = pg_pool_add(pg, req->compares[i].bits, width, &span);
        odin3_pspan *slot = st == ODIN3_OK ? odin3_vec_push(&pg->cmps) : NULL;
        if (st == ODIN3_OK && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        }
        if (st != ODIN3_OK) {
            return st;
        }
        *slot = span;
    }
    return ODIN3_OK;
}

odin3_status odin3_pg_case(odin3_pg *pg, const odin3_pg_case_req *req) {
    uint32_t sw_index = ((const pg_case *)odin3_vec_cat(&pg->cases, pg->cur))->last_sw;
    if (req == NULL || (req->n_compares > 0 && req->compares == NULL) || sw_index == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_case: bad argument, or no switch to join");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t width = ((const pg_switch *)odin3_vec_cat(&pg->sws, sw_index - 1U))->signal.len;
    pg_case cs = {pg->cur, 0, 0, 0, 0, 0, 0, 0, {0}};
    odin3_status st = pg_stmt_prov(pg, req->stmt, &cs.prov);
    st = st != ODIN3_OK ? st : case_compares(pg, req, width, &cs);
    pg_case *slot = st == ODIN3_OK ? odin3_vec_push(&pg->cases) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *slot = cs;
    uint32_t index = (uint32_t)pg->cases.len - 1U; /* 0-based; stored 1-based in the switch */
    pg_switch *sw = odin3_vec_at(&pg->sws, sw_index - 1U);
    if (sw->last_case != 0) {
        ((pg_case *)odin3_vec_at(&pg->cases, sw->last_case - 1U))->next = index + 1U;
    } else {
        sw->first_case = index + 1U;
    }
    sw->last_case = index + 1U;
    pg->cur = index;
    return ODIN3_OK;
}

void odin3_pg_case_end(odin3_pg *pg) {
    if (pg->cur != PG_ROOT) {
        pg->cur = ((const pg_case *)odin3_vec_cat(&pg->cases, pg->cur))->parent;
    }
}

/* --- temporaries ----------------------------------------------------------------------------- */

bool pg_group_of(const odin3_pg *pg, odin3_net_id net, pg_grp *out) {
    uint64_t value = 0;
    if (!odin3_u64map_get(pg->group, net.v, &value)) {
        return false;
    }
    *out = (pg_grp){(uint32_t)(value >> GROUP_SHIFT), (uint32_t)(value & UINT32_MAX)};
    return true;
}

static odin3_status temp_one(odin3_pg *pg, odin3_prov_id prov, uint32_t group, uint32_t pos,
                             odin3_net_id *net) {
    odin3_status st = odin3_net_create(pg->e2->mod, 0, prov, net);
    uint64_t key = ((uint64_t)group << GROUP_SHIFT) | pos;
    return st != ODIN3_OK ? st : odin3_u64map_put(pg->group, (odin3_kv){net->v, key});
}

odin3_status odin3_pg_temp(odin3_pg *pg, uint32_t width, odin3_ast_id why, odin3_net_id *first) {
    odin3_elab2 *e2 = pg->e2;
    if (width == 0 || first == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_temp: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_id prov = {0};
    odin3_status st = odin3_elab2_reserve_nets(e2, width, odin3_ast_loc(e2->elab, why));
    st = st != ODIN3_OK ? st : pg_stmt_prov(pg, why, &prov);
    uint32_t group = ++pg->groups;
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        odin3_net_id net = {0};
        st = temp_one(pg, prov, group, i, &net);
        first->v = st == ODIN3_OK && i == 0 ? net.v : first->v;
    }
    odin3_elab2_counts made = {0, 0, width, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* --- assignments (EL-35 step 5; chunks, v4.2) ------------------------------------------------- */

/* The kept bits of the lvalue: not dropped, and the first occurrence of their net. */
static odin3_status asg_keep(odin3_pg *pg, const odin3_elab2_lvalue *lv) {
    odin3_vec_clear(&pg->items);
    pg->serial++;
    for (uint32_t i = 0; i < lv->nets.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&lv->nets, i);
        uint64_t seen = 0;
        if (net.v == 0 || (odin3_u64map_get(pg->seen, net.v, &seen) && seen == pg->serial)) {
            continue;
        }
        uint32_t *item = odin3_vec_push(&pg->items);
        if (item == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *item = i;
        odin3_status st = odin3_u64map_put(pg->seen, (odin3_kv){net.v, pg->serial});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A target bit: the temporary an assignment to net writes (lv), when the process has one. */
static bool asg_target(const odin3_pg *pg, const odin3_elab2_lvalue *lv, uint32_t item,
                       odin3_net_id *target) {
    return pg_lv_get(pg, *(const odin3_net_id *)odin3_vec_cat(&lv->nets, item), target);
}

bool pg_run_continues(const odin3_pg *pg, odin3_net_id prev, odin3_net_id target) {
    pg_grp before = {0, 0};
    pg_grp after = {0, 0};
    return pg_group_of(pg, prev, &before) && pg_group_of(pg, target, &after) &&
           before.group == after.group && after.pos == before.pos + 1U;
}

/* Splits the targeted items into runs; pg->runs gets each run's first index into the targeted
 * list, then the count. The targeted items replace pg->items. */
static odin3_status asg_runs(odin3_pg *pg, const pg_asg *asg) {
    uint32_t kept = 0;
    odin3_net_id prev = {0};
    odin3_vec_clear(&pg->runs);
    for (uint32_t k = 0; k < pg->items.len; k++) {
        uint32_t item = *(const uint32_t *)odin3_vec_cat(&pg->items, k);
        odin3_net_id target = {0};
        if (!asg_target(pg, asg->lv, item, &target)) {
            continue;
        }
        *(uint32_t *)odin3_vec_at(&pg->items, kept) = item;
        if (kept == 0 || !pg_run_continues(pg, prev, target)) {
            uint32_t *run = odin3_vec_push(&pg->runs);
            if (run == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *run = kept;
        }
        prev = target;
        kept++;
    }
    uint32_t *end = odin3_vec_push(&pg->runs);
    if (end == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *end = kept;
    return ODIN3_OK;
}

/* One run's action: the targets' temporaries and the value bits, items [from, to). */
static odin3_status asg_run_action(odin3_pg *pg, const pg_asg *asg, uint32_t from, uint32_t to,
                                   odin3_prov_id prov) {
    odin3_vec_clear(&pg->run_l);
    odin3_vec_clear(&pg->run_r);
    for (uint32_t k = from; k < to; k++) {
        uint32_t item = *(const uint32_t *)odin3_vec_cat(&pg->items, k);
        odin3_net_id target = {0};
        (void)asg_target(pg, asg->lv, item, &target);
        odin3_sigbit *left = odin3_vec_push(&pg->run_l);
        odin3_sigbit *right = left != NULL ? odin3_vec_push(&pg->run_r) : NULL;
        if (right == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *left = target.v;
        *right = asg->rhs->bits[item];
    }
    pg_act_req req = {pg->cur, 0, pg->run_l.data, pg->run_r.data, to - from, prov, false, false};
    return pg_act_add(pg, &req);
}

/* "f.v:L:C" of an AST node. */
static odin3_status place_of(const odin3_pg *pg, odin3_ast_id node, odin3_strbuf *out) {
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(pg->e2->design, &sm);
    return st != ODIN3_OK ? st
                          : odin3_srcman_format(sm, odin3_ast_loc(pg->e2->elab, node), out,
                                                odin3_design_diag_paths(pg->e2->design));
}

/* T9, once per statement: `'r' is assigned with both '=' and '<=' (also at f.v:L:C)`. */
static odin3_status mixed_error(odin3_pg *pg, const pg_asg *asg, uint32_t bit, odin3_ast_id first) {
    odin3_elab2 *e2 = pg->e2;
    odin3_wirebit wb = ((const odin3_elab2_lvbit *)odin3_vec_cat(&asg->lv->bits, bit))->wb;
    odin3_strbuf place;
    odin3_strbuf_init(&place);
    odin3_status st = place_of(pg, first, &place);
    if (st == ODIN3_OK) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, asg->stmt),
                          "'%s' is assigned with both '=' and '<=' (also at %s)",
                          wb.wire.v != 0 ? odin3_strtab_get(odin3_design_strtab(e2->design),
                                                            odin3_wire_name(e2->mod, wb.wire))
                                         : "net",
                          place.data != NULL ? place.data : "?");
        st = ODIN3_ERR_PARSE;
    }
    odin3_strbuf_free(&place);
    return st;
}

/* T9 (EL-41): a bit assigned with both `=` and `<=`, in one block or several. The first
 * assignment of each bit is remembered for the module. */
odin3_status pg_check_mixed(odin3_pg *pg, const pg_asg *asg) {
    uint64_t mine = (uint64_t)(asg->blocking ? 2U : 1U) << GROUP_SHIFT;
    for (uint32_t i = 0; i < asg->lv->nets.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&asg->lv->nets, i);
        uint64_t prev = 0;
        if (net.v == 0) {
            continue;
        }
        if (!odin3_u64map_get(pg->kinds, net.v, &prev)) {
            odin3_status st = odin3_u64map_put(pg->kinds, (odin3_kv){net.v, mine | asg->stmt.v});
            if (st != ODIN3_OK) {
                return st;
            }
        } else if ((prev & ~(uint64_t)UINT32_MAX) != mine) {
            return mixed_error(pg, asg, i, (odin3_ast_id){(uint32_t)prev});
        }
    }
    return ODIN3_OK;
}

odin3_status pg_apply(odin3_pg *pg, const pg_asg *asg) {
    odin3_status st = asg->raw ? ODIN3_OK : pg_check_mixed(pg, asg);
    st = st != ODIN3_OK ? st : asg_keep(pg, asg->lv);
    for (uint32_t k = 0; st == ODIN3_OK && asg->blocking && k < pg->items.len; k++) {
        uint32_t item = *(const uint32_t *)odin3_vec_cat(&pg->items, k);
        st = pg_rv_set(pg, *(const odin3_net_id *)odin3_vec_cat(&asg->lv->nets, item),
                       asg->rhs->bits[item]);
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < pg->items.len; k++) {
        odin3_net_id target = {0};
        if (asg_target(pg, asg->lv, *(const uint32_t *)odin3_vec_cat(&pg->items, k), &target)) {
            st = pg_remove(pg, target);
        }
    }
    st = st != ODIN3_OK ? st : asg_runs(pg, asg);
    odin3_prov_id prov = {0};
    st = st != ODIN3_OK ? st : pg_stmt_prov(pg, asg->stmt, &prov);
    /* the leftmost part of the target first: the runs from the last to the first */
    for (size_t run = pg->runs.len; st == ODIN3_OK && run-- > 1;) {
        uint32_t from = *(const uint32_t *)odin3_vec_cat(&pg->runs, run - 1U);
        uint32_t to = *(const uint32_t *)odin3_vec_cat(&pg->runs, run);
        st = asg_run_action(pg, asg, from, to, prov);
    }
    return st;
}

odin3_status pg_take_target_of(odin3_pg *pg, odin3_ast_id lhs) {
    odin3_elab2_lvalue *lv = &pg->lvs;
    odin3_vec_clear(&lv->nets);
    odin3_vec_clear(&lv->bits);
    odin3_vec_clear(&lv->work);
    lv->dropped = false;
    pg->hooks = pg_reads(pg);
    lv->reads = &pg->hooks;
    lv->scan_only = pg->scan_local;
    lv->scan_calls = pg->scanning;
    return odin3_elab2_lvalue_make(pg->e2, lhs, lv);
}

odin3_status odin3_pg_assign(odin3_pg *pg, const odin3_pg_assign_req *req) {
    if (req == NULL || req->rhs == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_assign: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = pg_take_target_of(pg, req->lhs);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab2_lvalue *lv = &pg->lvs;
    if (lv->var.node.v != 0) {
        return pg_select_apply(pg, req);
    }
    if (lv->memw.node.v != 0 && req->rhs->width == lv->nets.len) {
        return pg_mem_apply(pg, req);
    }
    if (req->rhs->width != lv->nets.len) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_assign: the value is %u bits; the target %zu",
                  req->rhs->width, lv->nets.len);
        return ODIN3_ERR_INVALID_ARG;
    }
    pg_asg asg = {lv, req->rhs, req->stmt, req->blocking, false};
    return pg_apply(pg, &asg);
}

odin3_status odin3_pg_select_write(odin3_pg *pg, const odin3_pg_assign_req *req) {
    if (req == NULL || req->rhs == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_select_write: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = pg_take_target_of(pg, req->lhs);
    if (st == ODIN3_OK && pg->lvs.var.node.v == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_pg_select_write: the target is not a select with a non-constant index");
        return ODIN3_ERR_INVALID_ARG;
    }
    return st != ODIN3_OK ? st : pg_select_apply(pg, req);
}
