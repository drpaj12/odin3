/*
 * map_pattern.c — implement_pattern_cache (Yosys 0.55 passes/fsm/fsm_map.cc:45-155): the logic
 * that is true when the machine is in one of the states a pattern needs and the control inputs
 * match the pattern, OR-ed over the patterns. Every output cell is a $reduce_or (Yosys connects
 * a single signal or a constant with `connect` instead; a one-bit $reduce_or is that wire after
 * the next opt, and needs no net merge here).
 */
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/map_int.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One pattern of ctrl_in and the states that need it. */
enum { BINARY_PARAMS = 5 };

typedef struct pat_entry {
    const uint8_t *pattern;
    uint8_t *states; /* per state: needs this pattern */
} pat_entry;

typedef struct pat_cache {
    odin3_vec entries; /* pat_entry, sorted by the Const order of the pattern */
    uint8_t *full;     /* per state: still in the fullstate cache */
    uint8_t *scratch;  /* per state, for complete_count */
    uint32_t n_states, width;
} pat_cache;

static void cache_free(pat_cache *cache) {
    pat_entry *entries = cache->entries.data;
    for (size_t i = 0; i < cache->entries.len; i++) {
        odin3_util_free(entries[i].states);
    }
    odin3_vec_free(&cache->entries);
    odin3_util_free(cache->full);
    odin3_util_free(cache->scratch);
}

/* The entry for `pattern`, created in order when new; NULL on out of memory. */
static pat_entry *cache_entry(pat_cache *cache, const uint8_t *pattern) {
    pat_entry *entries = cache->entries.data;
    size_t lo = 0;
    size_t hi = cache->entries.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = odin3_fsm_const_cmp(entries[mid].pattern, cache->width, pattern, cache->width);
        if (cmp == 0) {
            return &entries[mid];
        }
        lo = cmp < 0 ? mid + 1 : lo;
        hi = cmp < 0 ? hi : mid;
    }
    uint8_t *states = odin3_util_calloc((size_t)cache->n_states + 1);
    if (states == NULL || odin3_vec_push(&cache->entries) == NULL) {
        odin3_util_free(states);
        return NULL;
    }
    entries = cache->entries.data;
    memmove(entries + lo + 1, entries + lo, (cache->entries.len - 1 - lo) * sizeof *entries);
    entries[lo].pattern = pattern;
    entries[lo].states = states;
    return &entries[lo];
}

/* Collects the transitions of the target into the cache (:230-245, :298-312). */
static odin3_status build_cache(const odin3_fsm_mapper *mp, const odin3_fsm_target *target,
                                pat_cache *cache) {
    const odin3_fsm_data *data = &mp->inst->data;
    memset(cache, 0, sizeof *cache);
    odin3_vec_init(&cache->entries, sizeof(pat_entry));
    cache->n_states = mp->n_states;
    cache->width = (uint32_t)data->num_inputs;
    cache->full = odin3_util_malloc((size_t)mp->n_states + 1);
    cache->scratch = odin3_util_malloc((size_t)mp->n_states + 1);
    if (cache->full == NULL || cache->scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(cache->full, 1, mp->n_states);
    for (size_t i = 0; i < data->trans.len; i++) {
        const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        bool hit = target->kind == ODIN3_FSM_TARGET_NEXT_STATE
                       ? tr->state_out == (int32_t)target->index
                       : tr->ctrl_out[target->index] == ODIN3_FSM_BIT_1;
        if (!hit) {
            cache->full[tr->state_in] = 0;
            continue;
        }
        pat_entry *entry = cache_entry(cache, tr->ctrl_in);
        if (entry == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        entry->states[tr->state_in] = 1;
    }
    return ODIN3_OK;
}

/* pattern_is_subset(super, sub): every defined bit of sub is defined and equal in super. */
static bool pattern_is_subset(const uint8_t *super_pattern, const uint8_t *sub_pattern,
                              uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
        if (!odin3_fsm_bit_defined(sub_pattern[i])) {
            continue;
        }
        if (!odin3_fsm_bit_defined(super_pattern[i]) || super_pattern[i] != sub_pattern[i]) {
            return false;
        }
    }
    return true;
}

/* The number of states in `states` plus those of every entry whose pattern `pattern` includes. */
static uint32_t complete_count(const pat_cache *cache, const pat_entry *entry) {
    uint8_t *seen = cache->scratch;
    memcpy(seen, entry->states, cache->n_states);
    const pat_entry *entries = cache->entries.data;
    for (size_t k = 0; k < cache->entries.len; k++) {
        if (!pattern_is_subset(entry->pattern, entries[k].pattern, cache->width)) {
            continue;
        }
        for (uint32_t state = 0; state < cache->n_states; state++) {
            seen[state] = seen[state] || entries[k].states[state];
        }
    }
    uint32_t count = 0;
    for (uint32_t state = 0; state < cache->n_states; state++) {
        count += seen[state] != 0 ? 1U : 0U;
    }
    return count;
}

/* Appends net to a vec of nets. */
static odin3_status push_net(odin3_vec *vec, odin3_net_id net) {
    return odin3_kit_push_net(vec, net);
}

/* A cell of `type` over inputs and a fresh output net (appended to cases or returned). */
static odin3_status binary_cell(odin3_fsm_mapper *mp, const char *type, const odin3_netvec ins[2],
                                odin3_net_id *out) {
    odin3_value params[BINARY_PARAMS] = {
        odin3_value_int(0), odin3_value_int(0), odin3_value_int((int64_t)ins[0].count),
        odin3_value_int((int64_t)ins[1].count), odin3_value_int(1)};
    odin3_status st = odin3_fsm_map_net(mp, out);
    odin3_netvec ports[3] = {ins[0], ins[1], {out, 1}};
    odin3_fsm_cell_req req = {type, params, ODIN3_FSM_COUNT_OF(params), ports};
    odin3_node_id node = {0};
    return st == ODIN3_OK ? odin3_fsm_map_cell(mp, &req, &node) : st;
}

/* The $eq of the pattern's defined bits against their values (:62-74). */
static odin3_status pattern_eq(odin3_fsm_mapper *mp, const uint8_t *pattern, odin3_net_id *out) {
    const odin3_fsm_inst *inst = mp->inst;
    const odin3_fsm_sig *ctrl = inst->ctrl_in.data;
    odin3_vec lhs;
    odin3_vec rhs;
    odin3_vec_init(&lhs, sizeof(odin3_net_id));
    odin3_vec_init(&rhs, sizeof(odin3_net_id));
    odin3_status st = ODIN3_OK;
    for (int32_t j = 0; st == ODIN3_OK && j < inst->data.num_inputs; j++) {
        if (!odin3_fsm_bit_defined(pattern[j])) {
            continue;
        }
        odin3_net_id net = {0};
        st = odin3_fsm_map_sig_net(mp, ctrl[j], &net);
        st = st == ODIN3_OK ? push_net(&lhs, net) : st;
        st =
            st == ODIN3_OK ? odin3_fsm_map_sig_net(mp, odin3_fsm_sig_of_bit(pattern[j]), &net) : st;
        st = st == ODIN3_OK ? push_net(&rhs, net) : st;
    }
    if (st == ODIN3_OK) {
        const odin3_netvec ins[2] = {{lhs.data, (uint32_t)lhs.len}, {rhs.data, (uint32_t)rhs.len}};
        st = binary_cell(mp, "$eq", ins, out);
    }
    odin3_vec_free(&lhs);
    odin3_vec_free(&rhs);
    return st;
}

/* The OR of the states that need the pattern and are not in the fullstate cache. */
static odin3_status states_or(const odin3_fsm_mapper *mp, const pat_cache *cache,
                              const pat_entry *entry, odin3_vec *or_sig) {
    for (uint32_t state = 0; state < cache->n_states; state++) {
        if (entry->states[state] != 0 && cache->full[state] == 0) {
            odin3_status st = push_net(or_sig, mp->state_sig[state]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* A $reduce_or over the nets of `ins` into a fresh net (or the given one). */
static odin3_status reduce_or(odin3_fsm_mapper *mp, const odin3_vec *ins, odin3_net_id *out) {
    odin3_status st = odin3_fsm_map_net(mp, out);
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int((int64_t)ins->len),
                             odin3_value_int(1)};
    odin3_netvec ports[2] = {{ins->data, (uint32_t)ins->len}, {out, 1}};
    odin3_fsm_cell_req req = {"$reduce_or", params, ODIN3_FSM_COUNT_OF(params), ports};
    odin3_node_id node = {0};
    return st == ODIN3_OK ? odin3_fsm_map_cell(mp, &req, &node) : st;
}

/* True when the pattern constrains some control input. */
static bool has_defined_bit(const pat_cache *cache, const pat_entry *entry) {
    for (uint32_t j = 0; j < cache->width; j++) {
        if (odin3_fsm_bit_defined(entry->pattern[j])) {
            return true;
        }
    }
    return false;
}

/* The state part of a term: a lone state net, or an OR of the states (:97-117). */
static odin3_status term_states(odin3_fsm_mapper *mp, const odin3_vec *or_sig, odin3_vec *and_sig) {
    odin3_net_id net = ((const odin3_net_id *)or_sig->data)[0];
    odin3_status st = or_sig->len == 1 ? ODIN3_OK : reduce_or(mp, or_sig, &net);
    return st == ODIN3_OK ? push_net(and_sig, net) : st;
}

/* The AND of the one or two parts into a case (:119-141). */
static odin3_status term_and(odin3_fsm_mapper *mp, const odin3_vec *and_sig, odin3_vec *cases) {
    const odin3_net_id *terms = and_sig->data;
    odin3_net_id net = {0};
    odin3_status st = ODIN3_OK;
    if (and_sig->len == 2) {
        const odin3_netvec ins[2] = {{&terms[0], 1}, {&terms[1], 1}};
        st = binary_cell(mp, "$and", ins, &net);
    } else if (and_sig->len == 1) {
        net = terms[0];
    } else {
        st = odin3_fsm_map_sig_net(mp, odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_1), &net);
    }
    return st == ODIN3_OK ? push_net(cases, net) : st;
}

/* One pattern's term (:62-142): eq AND (OR of states); appended to cases. */
static odin3_status pattern_term(odin3_fsm_mapper *mp, const pat_cache *cache,
                                 const pat_entry *entry, odin3_vec *cases) {
    odin3_vec or_sig;
    odin3_vec and_sig;
    odin3_vec_init(&or_sig, sizeof(odin3_net_id));
    odin3_vec_init(&and_sig, sizeof(odin3_net_id));
    odin3_status st = states_or(mp, cache, entry, &or_sig);
    if (st == ODIN3_OK && or_sig.len > 0) {
        odin3_net_id net = {0};
        if (has_defined_bit(cache, entry)) {
            st = pattern_eq(mp, entry->pattern, &net);
            st = st == ODIN3_OK ? push_net(&and_sig, net) : st;
        }
        if (st == ODIN3_OK && complete_count(cache, entry) < cache->n_states) {
            st = term_states(mp, &or_sig, &and_sig);
        }
        st = st == ODIN3_OK ? term_and(mp, &and_sig, cases) : st;
    }
    odin3_vec_free(&or_sig);
    odin3_vec_free(&and_sig);
    return st;
}

/* The final OR into the target (:143-154): a $reduce_or into out_net, or an open pin. */
static odin3_status finish_target(odin3_fsm_mapper *mp, const odin3_fsm_target *target,
                                  odin3_vec *cases) {
    odin3_net_id zero = {0};
    if (cases->len == 0) {
        odin3_status st = odin3_fsm_map_sig_net(mp, odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_0), &zero);
        st = st == ODIN3_OK ? push_net(cases, zero) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_net_id out = target->out_net;
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int((int64_t)cases->len),
                             odin3_value_int(1)};
    odin3_netvec ports[2] = {{cases->data, (uint32_t)cases->len}, {&out, 1}};
    odin3_fsm_cell_req req = {"$reduce_or", params, ODIN3_FSM_COUNT_OF(params), ports};
    odin3_node_id node = {0};
    odin3_status st = odin3_fsm_map_cell(mp, &req, &node);
    if (st == ODIN3_OK && out.v == 0) { /* the open Y pin waits for the old drivers to go */
        odin3_pin_id pin = odin3_node_port(mp->fm->module, node, 1).first;
        st = odin3_kit_push_u32(&mp->pending, pin.v);
        st = st == ODIN3_OK ? odin3_kit_push_u32(&mp->pending, target->out_ctrl) : st;
    }
    return st;
}

odin3_status odin3_fsm_map_target(odin3_fsm_mapper *mp, const odin3_fsm_target *target) {
    pat_cache cache;
    odin3_vec cases;
    odin3_vec_init(&cases, sizeof(odin3_net_id));
    odin3_status st = build_cache(mp, target, &cache);
    for (uint32_t state = 0; st == ODIN3_OK && state < mp->n_states; state++) {
        st = cache.full[state] != 0 ? push_net(&cases, mp->state_sig[state]) : ODIN3_OK;
    }
    const pat_entry *entries = cache.entries.data;
    for (size_t i = 0; st == ODIN3_OK && i < cache.entries.len; i++) {
        st = pattern_term(mp, &cache, &entries[i], &cases);
    }
    st = st == ODIN3_OK ? finish_target(mp, target, &cases) : st;
    odin3_vec_free(&cases);
    cache_free(&cache);
    return st;
}
