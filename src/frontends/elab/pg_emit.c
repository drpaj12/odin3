/*
 * pg_emit.c — step 7 of the process generator (2D Task 21; EL-33 emission, EL-35 step 7): the
 * driver claims of the outputs, the builder renumbered breadth first into IR-19's canonical layout
 * (dead positions and empty actions dropped), the `O` and `I` pins in net-ID order with the
 * constants as `I` pins on the module's shared constant nets, and the one
 * odin3_node_create_connected that makes the `$proc` node whole.
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* --- the canonical layout --------------------------------------------------------------------- */

void pg_laid_init(pg_laid *lay) {
    odin3_vec_init(&lay->sbits, sizeof(odin3_sigbit));
    odin3_vec_init(&lay->spans_cmp, sizeof(odin3_pspan));
    odin3_vec_init(&lay->acts, sizeof(odin3_proc_action));
    odin3_vec_init(&lay->cases, sizeof(odin3_proc_case));
    odin3_vec_init(&lay->sws, sizeof(odin3_proc_switch));
    odin3_vec_init(&lay->syncs, sizeof(odin3_proc_sync));
    odin3_vec_init(&lay->sync_sig, sizeof(odin3_sigbit));
    odin3_vec_init(&lay->case_order, sizeof(uint32_t));
    odin3_vec_init(&lay->sw_order, sizeof(uint32_t));
}

void pg_laid_free(pg_laid *lay) {
    odin3_vec_free(&lay->sbits);
    odin3_vec_free(&lay->spans_cmp);
    odin3_vec_free(&lay->acts);
    odin3_vec_free(&lay->cases);
    odin3_vec_free(&lay->sws);
    odin3_vec_free(&lay->syncs);
    odin3_vec_free(&lay->sync_sig);
    odin3_vec_free(&lay->case_order);
    odin3_vec_free(&lay->sw_order);
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* Appends the cases of switch `sw` (0-based) to the case order; *count is how many. */
static odin3_status order_cases(const odin3_pg *pg, pg_laid *lay, uint32_t sw, uint32_t *count) {
    const pg_switch *src = odin3_vec_cat(&pg->sws, sw);
    *count = 0;
    for (uint32_t k = src->first_case; k != 0;
         k = ((const pg_case *)odin3_vec_cat(&pg->cases, k - 1U))->next) {
        odin3_status st = push_u32(&lay->case_order, k - 1U);
        if (st != ODIN3_OK) {
            return st;
        }
        (*count)++;
    }
    return ODIN3_OK;
}

/* Builder case `owner`'s switches are numbered next, each with its cases appended to the queue. */
static odin3_status order_switches(const odin3_pg *pg, pg_laid *lay, uint32_t owner,
                                   odin3_proc_case *out) {
    out->first_switch = (uint32_t)lay->sws.len;
    for (uint32_t at = ((const pg_case *)odin3_vec_cat(&pg->cases, owner))->first_sw; at != 0;
         at = ((const pg_switch *)odin3_vec_cat(&pg->sws, at - 1U))->next) {
        const pg_switch *src = odin3_vec_cat(&pg->sws, at - 1U);
        odin3_proc_switch *sw = odin3_vec_push(&lay->sws);
        uint32_t first = (uint32_t)lay->case_order.len;
        uint32_t count = 0;
        odin3_status st = sw != NULL ? push_u32(&lay->sw_order, at - 1U) : ODIN3_ERR_NO_MEMORY;
        st = st != ODIN3_OK ? st : order_cases(pg, lay, at - 1U, &count);
        if (st != ODIN3_OK) {
            return st;
        }
        sw = odin3_vec_at(&lay->sws, lay->sws.len - 1);
        *sw = (odin3_proc_switch){{0, 0}, first, count, src->flags, src->prov};
        out->n_switches++;
    }
    return ODIN3_OK;
}

/* The breadth-first order: lay->case_order and sw_order, and the switch side of each case. */
static odin3_status lay_order(const odin3_pg *pg, pg_laid *lay) {
    odin3_status st = push_u32(&lay->case_order, PG_ROOT);
    for (size_t head = 0; st == ODIN3_OK && head < lay->case_order.len; head++) {
        uint32_t owner = *(const uint32_t *)odin3_vec_cat(&lay->case_order, head);
        odin3_proc_case *out = odin3_vec_push(&lay->cases);
        if (out == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st = order_switches(pg, lay, owner, out);
    }
    return st;
}

/* Whether position `bit` of an action survives: not removed by a later assignment, not the lhs of a
 * conflicting output. */
static bool position_kept(const odin3_pg *pg, const pg_act *act, uint32_t bit) {
    const uint8_t *dead = odin3_vec_cat(&pg->dead, act->lhs.first + bit);
    const odin3_sigbit *lhs = odin3_vec_cat(&pg->pool, act->lhs.first + bit);
    return *dead == 0 && !odin3_u64map_get(pg->dead_nets, *lhs, NULL);
}

/* Appends the surviving positions of one side of an action (the pool span `side`); *kept counts
 * them. */
static odin3_status lay_side(const odin3_pg *pg, pg_laid *lay, const pg_act *act, odin3_pspan side,
                             uint32_t *kept) {
    *kept = 0;
    for (uint32_t i = 0; i < act->lhs.len; i++) {
        if (!position_kept(pg, act, i)) {
            continue;
        }
        odin3_sigbit *slot = odin3_vec_push(&lay->sbits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *(const odin3_sigbit *)odin3_vec_cat(&pg->pool, side.first + i);
        (*kept)++;
    }
    return ODIN3_OK;
}

/* Appends the kept positions of an action, lhs then rhs (a removed position leaves with the
 * right-side bit paired with it); an action with nothing left is not made. */
static odin3_status lay_action(const odin3_pg *pg, pg_laid *lay, const pg_act *act) {
    uint32_t first = (uint32_t)lay->sbits.len;
    uint32_t kept = 0;
    uint32_t again = 0;
    odin3_status st = lay_side(pg, lay, act, act->lhs, &kept);
    st = st != ODIN3_OK ? st : lay_side(pg, lay, act, act->rhs, &again);
    if (st != ODIN3_OK || kept == 0) {
        return st;
    }
    odin3_proc_action *out = odin3_vec_push(&lay->acts);
    if (out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = (odin3_proc_action){{first, kept}, {first + kept, kept}, act->prov};
    return ODIN3_OK;
}

/* The actions of the list starting at first (1-based), in order. */
static odin3_status lay_actions(const odin3_pg *pg, pg_laid *lay, uint32_t first) {
    for (uint32_t at = first; at != 0;
         at = ((const pg_act *)odin3_vec_cat(&pg->acts, at - 1U))->next) {
        odin3_status st = lay_action(pg, lay, odin3_vec_cat(&pg->acts, at - 1U));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Copies the pool span into the positions; *out is the new span. */
static odin3_status lay_span(const odin3_pg *pg, pg_laid *lay, odin3_pspan src, odin3_pspan *out) {
    *out = (odin3_pspan){(uint32_t)lay->sbits.len, src.len};
    for (uint32_t i = 0; i < src.len; i++) {
        odin3_sigbit *slot = odin3_vec_push(&lay->sbits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *(const odin3_sigbit *)odin3_vec_cat(&pg->pool, src.first + i);
    }
    return ODIN3_OK;
}

/* The compares of the case at new index `index`. */
static odin3_status lay_compares(const odin3_pg *pg, pg_laid *lay, const pg_case *src) {
    for (uint32_t k = 0; k < src->cmp_n; k++) {
        odin3_pspan span;
        odin3_status st = lay_span(
            pg, lay, *(const odin3_pspan *)odin3_vec_cat(&pg->cmps, src->cmp_first + k), &span);
        odin3_pspan *slot = st == ODIN3_OK ? odin3_vec_push(&lay->spans_cmp) : NULL;
        if (st != ODIN3_OK || slot == NULL) {
            return st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
        }
        *slot = span;
    }
    return ODIN3_OK;
}

/* The compares and actions of the case at new index `index`. */
static odin3_status lay_case(const odin3_pg *pg, pg_laid *lay, uint32_t index) {
    uint32_t owner = *(const uint32_t *)odin3_vec_cat(&lay->case_order, index);
    const pg_case *src = odin3_vec_cat(&pg->cases, owner);
    odin3_proc_case *out = odin3_vec_at(&lay->cases, index);
    out->prov = src->prov;
    out->first_compare = (uint32_t)lay->spans_cmp.len;
    out->n_compares = src->cmp_n;
    uint32_t before = (uint32_t)lay->acts.len;
    odin3_status st = lay_compares(pg, lay, src);
    st = st != ODIN3_OK ? st : lay_actions(pg, lay, src->first_act);
    out = odin3_vec_at(&lay->cases, index);
    out->first_action = before;
    out->n_actions = (uint32_t)lay->acts.len - before;
    return st;
}

/* The syncs follow the case actions, one list each. */
static odin3_status lay_syncs(const odin3_pg *pg, pg_laid *lay) {
    for (uint32_t i = 0; i < pg->syncs.len; i++) {
        const pg_sync *src = odin3_vec_cat(&pg->syncs, i);
        uint32_t before = (uint32_t)lay->acts.len;
        odin3_status st = lay_actions(pg, lay, src->first_act);
        odin3_proc_sync *out = st == ODIN3_OK ? odin3_vec_push(&lay->syncs) : NULL;
        odin3_sigbit *sig = out != NULL ? odin3_vec_push(&lay->sync_sig) : NULL;
        if (st != ODIN3_OK || sig == NULL) {
            return st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
        }
        *out = (odin3_proc_sync){src->type, 0, before, (uint32_t)lay->acts.len - before};
        *sig = src->signal;
    }
    return ODIN3_OK;
}

odin3_status pg_layout(const odin3_pg *pg, pg_laid *lay) {
    odin3_status st = lay_order(pg, lay);
    for (uint32_t i = 0; st == ODIN3_OK && i < lay->cases.len; i++) {
        st = lay_case(pg, lay, i);
    }
    for (uint32_t j = 0; st == ODIN3_OK && j < lay->sws.len; j++) {
        uint32_t src = *(const uint32_t *)odin3_vec_cat(&lay->sw_order, j);
        odin3_proc_switch *sw = odin3_vec_at(&lay->sws, j);
        st = lay_span(pg, lay, ((const pg_switch *)odin3_vec_cat(&pg->sws, src))->signal,
                      &sw->signal);
    }
    return st != ODIN3_OK ? st : lay_syncs(pg, lay);
}

/* --- the driver claims (step 7) --------------------------------------------------------------- */

/* The outputs of chunk `index` as a lvalue; wired nets are left out and their actions dropped (the
 * pre-scan reported them). */
static odin3_status claim_lvalue(odin3_pg *pg, uint32_t index, odin3_elab2_lvalue *lv) {
    const pg_out *outs = pg->outs.data;
    const pg_chunk *chunk = odin3_vec_cat(&pg->chunks, index);
    odin3_vec_clear(&lv->nets);
    odin3_vec_clear(&lv->bits);
    for (uint32_t i = chunk->first; i < chunk->first + chunk->len; i++) {
        if (odin3_elab2_net_is_wired(pg->e2, outs[i].net)) {
            odin3_status st = odin3_u64map_put(pg->dead_nets, (odin3_kv){outs[i].net.v, 1});
            st = st != ODIN3_OK ? st : odin3_u64map_put(pg->dead_nets, (odin3_kv){outs[i].t0.v, 1});
            if (st != ODIN3_OK) {
                return st;
            }
            continue;
        }
        odin3_net_id *net = odin3_vec_push(&lv->nets);
        odin3_elab2_lvbit *bit = net != NULL ? odin3_vec_push(&lv->bits) : NULL;
        if (bit == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *net = outs[i].net;
        *bit = (odin3_elab2_lvbit){outs[i].wb, outs[i].node, outs[i].whole};
    }
    return ODIN3_OK;
}

/* A refused chunk: its outputs and their temporaries are dropped from the body. */
static odin3_status drop_chunk(odin3_pg *pg, uint32_t index) {
    const pg_out *outs = pg->outs.data;
    const pg_chunk *chunk = odin3_vec_cat(&pg->chunks, index);
    for (uint32_t i = chunk->first; i < chunk->first + chunk->len; i++) {
        odin3_status st = odin3_u64map_put(pg->dead_nets, (odin3_kv){outs[i].net.v, 1});
        st = st != ODIN3_OK ? st : odin3_u64map_put(pg->dead_nets, (odin3_kv){outs[i].t0.v, 1});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Every output claimed as a process driver, a chunk at a time (a conflict reports the chunk's
 * first conflicting bit and drops the chunk, so the node never makes a second driver). */
static odin3_status emit_claims(odin3_pg *pg, odin3_ast_id node) {
    odin3_elab2_lvalue lv;
    odin3_elab2_lvalue_init(&lv);
    odin3_status st = ODIN3_OK;
    for (uint32_t chunk = 0; st == ODIN3_OK && chunk < pg->chunks.len; chunk++) {
        st = claim_lvalue(pg, chunk, &lv);
        bool granted = false;
        odin3_elab2_lvclaim req = {&lv, ODIN3_DRV_PROCESS, node, true, NULL, NULL};
        st = st != ODIN3_OK || lv.nets.len == 0 ? st
                                                : odin3_elab2_lvalue_claim(pg->e2, &req, &granted);
        if (st == ODIN3_OK && lv.nets.len > 0 && !granted) {
            st = drop_chunk(pg, chunk);
        }
    }
    odin3_elab2_lvalue_free(&lv);
    return st;
}

/* --- the pins ----------------------------------------------------------------------------------
 */

/* Sorts and removes the duplicates of a vec of uint32_t. */
static void unique_u32(odin3_vec *vec) {
    if (vec->len == 0) {
        return;
    }
    qsort(vec->data, vec->len, sizeof(uint32_t), pg_u32_compare);
    uint32_t *items = vec->data;
    size_t kept = 1;
    for (size_t i = 1; i < vec->len; i++) {
        if (items[i] != items[kept - 1U]) {
            items[kept++] = items[i];
        }
    }
    vec->len = kept;
}

/* The net of a signal bit: a net as the merges left it, a constant its shared net. */
static odin3_status bit_net(odin3_elab2 *e2, odin3_sigbit bit, odin3_net_id *net) {
    if ((bit & ODIN3_SB_CONST) == 0) {
        *net = odin3_elab2_resolve(e2, (odin3_net_id){bit});
        return ODIN3_OK;
    }
    return odin3_elab2_const_net(e2, odin3_sb_is_const(bit) ? odin3_sb_bit(bit) : ODIN3_BIT_X, net);
}

/* The pins of a laid body: O = the nets some action assigns, I = every other net read. */
typedef struct pg_pins {
    odin3_vec onets, inets; /* uint32_t, sorted */
    odin3_vec pbits;        /* odin3_pbit per position */
} pg_pins;

static void pins_free(pg_pins *pins) {
    odin3_vec_free(&pins->onets);
    odin3_vec_free(&pins->inets);
    odin3_vec_free(&pins->pbits);
}

/* Index of key in the sorted vec, or -1. */
static int64_t find_u32(const odin3_vec *vec, uint32_t key) {
    size_t lo = 0;
    size_t hi = vec->len;
    const uint32_t *items = vec->data;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (items[mid] == key) {
            return (int64_t)mid;
        }
        if (items[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}

/* The O nets: every lhs position. */
static odin3_status pins_outputs(odin3_pg *pg, const pg_laid *lay, pg_pins *pins) {
    const odin3_sigbit *sbits = lay->sbits.data;
    for (uint32_t act_index = 0; act_index < lay->acts.len; act_index++) {
        const odin3_proc_action *act = odin3_vec_cat(&lay->acts, act_index);
        for (uint32_t i = 0; i < act->lhs.len; i++) {
            odin3_net_id net = {0};
            odin3_status st = bit_net(pg->e2, sbits[act->lhs.first + i], &net);
            st = st != ODIN3_OK ? st : push_u32(&pins->onets, net.v);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    unique_u32(&pins->onets);
    return ODIN3_OK;
}

/* One position's pin: ANY stays ANY, a net assigned here OUT | k, any other IN | k. */
static odin3_status pins_one(odin3_pg *pg, const pg_pins *pins, odin3_sigbit bit, odin3_pbit *out) {
    if (bit == ODIN3_SB_ANY) {
        *out = ODIN3_PBIT_ANY;
        return ODIN3_OK;
    }
    odin3_net_id net = {0};
    odin3_status st = bit_net(pg->e2, bit, &net);
    int64_t at = st == ODIN3_OK ? find_u32(&pins->onets, net.v) : -1;
    if (at >= 0) {
        *out = ODIN3_PBIT_OUT | (uint32_t)at;
        return ODIN3_OK;
    }
    at = st == ODIN3_OK ? find_u32(&pins->inets, net.v) : -1;
    *out = ODIN3_PBIT_IN | (uint32_t)(at >= 0 ? at : 0);
    return st;
}

/* One bit read: a net no action assigns is an input (a constant on its shared net). */
static odin3_status input_bit(odin3_pg *pg, pg_pins *pins, odin3_sigbit bit) {
    if (bit == ODIN3_SB_ANY) {
        return ODIN3_OK;
    }
    odin3_net_id net = {0};
    odin3_status st = bit_net(pg->e2, bit, &net);
    return st == ODIN3_OK && find_u32(&pins->onets, net.v) < 0 ? push_u32(&pins->inets, net.v) : st;
}

/* The I nets: every net read that no action assigns, constants on their shared nets. */
static odin3_status pins_inputs(odin3_pg *pg, const pg_laid *lay, pg_pins *pins) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < lay->sbits.len; i++) {
        st = input_bit(pg, pins, *(const odin3_sigbit *)odin3_vec_cat(&lay->sbits, i));
    }
    for (size_t k = 0; st == ODIN3_OK && k < lay->syncs.len; k++) {
        if (((const odin3_proc_sync *)odin3_vec_cat(&lay->syncs, k))->type != ODIN3_SYNC_ALWAYS) {
            st = input_bit(pg, pins, *(const odin3_sigbit *)odin3_vec_cat(&lay->sync_sig, k));
        }
    }
    unique_u32(&pins->inets);
    return st;
}

/* Every position's pin, and the syncs' signals. */
static odin3_status pins_map(odin3_pg *pg, pg_laid *lay, pg_pins *pins) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < lay->sbits.len; i++) {
        odin3_pbit *slot = odin3_vec_push(&pins->pbits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st = pins_one(pg, pins, *(const odin3_sigbit *)odin3_vec_cat(&lay->sbits, i), slot);
    }
    for (size_t k = 0; st == ODIN3_OK && k < lay->syncs.len; k++) {
        odin3_proc_sync *sync = odin3_vec_at(&lay->syncs, k);
        if (sync->type != ODIN3_SYNC_ALWAYS) {
            st = pins_one(pg, pins, *(const odin3_sigbit *)odin3_vec_cat(&lay->sync_sig, k),
                          &sync->signal);
        }
    }
    return st;
}

static odin3_status pins_make(odin3_pg *pg, pg_laid *lay, pg_pins *pins) {
    odin3_status st = pins_outputs(pg, lay, pins);
    st = st != ODIN3_OK ? st : pins_inputs(pg, lay, pins);
    return st != ODIN3_OK ? st : pins_map(pg, lay, pins);
}

/* --- the node --------------------------------------------------------------------------------- */

/* The nets of the pins of one port, in the arena (never NULL, also for none). */
static odin3_status port_nets(const odin3_vec *keys, odin3_arena *arena, odin3_netvec *out) {
    odin3_net_id *nets = odin3_arena_alloc(arena, (keys->len + 1U) * sizeof *nets);
    if (nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < keys->len; i++) {
        nets[i] = (odin3_net_id){*(const uint32_t *)odin3_vec_cat(keys, i)};
    }
    *out = (odin3_netvec){nets, (uint32_t)keys->len};
    return ODIN3_OK;
}

/* The node whole from the laid body and its pins. */
static odin3_status make_node(odin3_pg *pg, odin3_ast_id prov_node, const pg_laid *lay,
                              const pg_pins *pins, odin3_node_id *out) {
    odin3_elab2 *e2 = pg->e2;
    odin3_arena *arena = odin3_arena_create(0);
    if (arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_proc_body body = {pins->pbits.data,    (uint32_t)pins->pbits.len,
                            lay->spans_cmp.data, (uint32_t)lay->spans_cmp.len,
                            lay->acts.data,      (uint32_t)lay->acts.len,
                            lay->cases.data,     (uint32_t)lay->cases.len,
                            lay->sws.data,       (uint32_t)lay->sws.len,
                            lay->syncs.data,     (uint32_t)lay->syncs.len};
    odin3_value params[3] = {[ODIN3_PROC_P_I_WIDTH] = odin3_value_int((int64_t)pins->inets.len),
                             [ODIN3_PROC_P_O_WIDTH] = odin3_value_int((int64_t)pins->onets.len)};
    odin3_netvec ports[2];
    odin3_elab2_prov_req preq = {prov_node, {0}, e2->scope.v};
    odin3_node_spec spec = {.type = e2->types[ODIN3_E2T_PROC], .params = params, .n_params = 3};
    odin3_status st = odin3_elab2_prov(e2, &preq, &spec.prov);
    st = st != ODIN3_OK ? st : odin3_proc_body_pack(arena, &body, &params[ODIN3_PROC_P_BODY]);
    st = st != ODIN3_OK ? st : port_nets(&pins->inets, arena, &ports[ODIN3_PROC_PORT_I]);
    st = st != ODIN3_OK ? st : port_nets(&pins->onets, arena, &ports[ODIN3_PROC_PORT_O]);
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, ports, out);
    if (st == ODIN3_OK) {
        st = odin3_elab2_attrs(e2, prov_node, (odin3_objref){ODIN3_OBJ_NODE, out->v});
        if (st != ODIN3_OK) {
            (void)odin3_node_delete(e2->mod, *out); /* the node is whole or not there */
        }
    }
    odin3_arena_destroy(arena);
    return st;
}

odin3_status odin3_pg_emit(odin3_pg *pg, odin3_ast_id prov_node, odin3_node_id *out) {
    odin3_elab2 *e2 = pg->e2;
    *out = (odin3_node_id){0};
    pg_laid lay;
    pg_pins pins;
    pg_laid_init(&lay);
    odin3_vec_init(&pins.onets, sizeof(uint32_t));
    odin3_vec_init(&pins.inets, sizeof(uint32_t));
    odin3_vec_init(&pins.pbits, sizeof(odin3_pbit));
    odin3_status st = emit_claims(pg, prov_node);
    st = st != ODIN3_OK ? st : pg_layout(pg, &lay);
    if (st == ODIN3_OK && lay.acts.len > 0) {
        st = pins_make(pg, &lay, &pins);
        /* point (aa): the pins of the node; fatal, nothing has been made */
        st = st != ODIN3_OK ? st
                            : odin3_elab2_pins_cap(e2, odin3_ast_loc(e2->elab, prov_node),
                                                   (uint64_t)pins.inets.len + pins.onets.len);
        st = st != ODIN3_OK ? st : make_node(pg, prov_node, &lay, &pins, out);
        odin3_elab2_counts made = {1, (uint32_t)(pins.inets.len + pins.onets.len), 0, 0};
        st = st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
    }
    pins_free(&pins);
    pg_laid_free(&lay);
    return st;
}
