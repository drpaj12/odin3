/*
 * opt.c — fsm_opt (spec O-0 to O-5; Yosys 0.55 passes/fsm/fsm_opt.cc:31-314) on an extracted FSM:
 * unreachable states, unused outputs, aliased inputs, feedback inputs, don't-care merging and
 * constant or unused inputs. Arrays of a transition are edited in place (they belong to it);
 * the transition table of the don't-care stage is rebuilt in the order of Yosys's std::map.
 */
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- helpers -----------------------------------------------------------------------------------
 */

/* Removes element idx of a byte array of `count` elements in place. */
static void erase_bit(uint8_t *bits, int32_t count, int32_t idx) {
    memmove(bits + idx, bits + idx + 1, (size_t)(count - idx - 1));
}

/* Removes element idx of a vec of odin3_fsm_sig. */
static void erase_sig(odin3_vec *vec, size_t idx) {
    odin3_fsm_sig *bits = vec->data;
    memmove(bits + idx, bits + idx + 1, (vec->len - idx - 1) * sizeof *bits);
    odin3_kit_truncate(vec, vec->len - 1);
}

/* Removes input idx from every transition and from ctrl_in. */
static void remove_input(odin3_fsm_inst *inst, int32_t idx) {
    odin3_fsm_data *data = &inst->data;
    for (size_t tr = 0; tr < data->trans.len; tr++) {
        erase_bit(odin3_fsm_data_trans(data, tr)->ctrl_in, data->num_inputs, idx);
    }
    erase_sig(&inst->ctrl_in, (size_t)idx);
    data->num_inputs--;
}

/* Drops the transitions for which keep says no (stable); the vec shrinks. */
typedef bool (*trans_pred)(const odin3_fsm_trans *tr, const void *ctx);

static void filter_trans(odin3_fsm_data *data, trans_pred keep, const void *ctx) {
    size_t kept = 0;
    for (size_t i = 0; i < data->trans.len; i++) {
        odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        if (keep(tr, ctx)) {
            *odin3_fsm_data_trans(data, kept++) = *tr;
        }
    }
    odin3_kit_truncate(&data->trans, kept);
}

/* A comparison with a context (a width), for the stable merge sort below. */
typedef int (*cmp_fn)(const void *lhs, const void *rhs, const void *ctx);

/* A sort request: the elements, how many, how big, and how they compare. */
typedef struct sort_req {
    void *base;
    size_t count, elem;
    cmp_fn cmp;
    const void *ctx;
} sort_req;

/* Merges the runs [lo, mid) and [mid, hi) of src into dst (stable). */
static void merge_runs(const sort_req *req, const char *src, char *dst, const size_t bounds[3]) {
    size_t left = bounds[0];
    size_t right = bounds[1];
    size_t out = bounds[0];
    const size_t elem = req->elem;
    while (left < bounds[1] || right < bounds[2]) {
        bool take_right =
            left >= bounds[1] ||
            (right < bounds[2] && req->cmp(src + right * elem, src + left * elem, req->ctx) < 0);
        memcpy(dst + out * elem, src + (take_right ? right++ : left++) * elem, elem);
        out++;
    }
}

/* Stable bottom-up merge sort of req->base. NO_MEMORY with the elements untouched. */
static odin3_status msort(const sort_req *req) {
    char *tmp = odin3_util_malloc(req->count * req->elem + 1);
    if (tmp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    char *src = req->base;
    char *dst = tmp;
    for (size_t width = 1; width < req->count; width *= 2) {
        for (size_t lo = 0; lo < req->count; lo += 2 * width) {
            size_t mid = lo + width < req->count ? lo + width : req->count;
            size_t hi = lo + 2 * width < req->count ? lo + 2 * width : req->count;
            size_t bounds[3] = {lo, mid, hi};
            merge_runs(req, src, dst, bounds);
        }
        char *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != (char *)req->base) {
        memcpy(req->base, src, req->count * req->elem);
    }
    odin3_util_free(tmp);
    return ODIN3_OK;
}

/* --- O-0: unreachable states -------------------------------------------------------------------
 */

/* Marks the states that are no transition's target (except the reset state) in dead[]. */
static size_t mark_unreachable(const odin3_fsm_data *data, uint8_t *dead) {
    size_t n_states = data->states.len;
    for (size_t i = 0; i < n_states; i++) {
        dead[i] = (int32_t)i != data->reset_state;
    }
    for (size_t i = 0; i < data->trans.len; i++) {
        int32_t out = odin3_fsm_data_trans(data, i)->state_out;
        if (out >= 0) {
            dead[out] = 0;
        }
    }
    size_t count = 0;
    for (size_t i = 0; i < n_states; i++) {
        count += dead[i];
    }
    return count;
}

/* Drops the dead states and their outgoing transitions, renumbering the rest. */
static void drop_states(odin3_fsm_data *data, const uint8_t *dead, int32_t *remap) {
    size_t kept = 0;
    uint8_t **codes = (uint8_t **)data->states.data;
    for (size_t i = 0; i < data->states.len; i++) {
        if (dead[i]) {
            odin3_log(ODIN3_LOG_DEBUG, "fsm: removing unreachable state %zu", i);
            remap[i] = -1;
            continue;
        }
        remap[i] = (int32_t)kept;
        codes[kept++] = codes[i];
    }
    odin3_kit_truncate(&data->states, kept);
    size_t kept_tr = 0;
    for (size_t i = 0; i < data->trans.len; i++) {
        odin3_fsm_trans tr = *odin3_fsm_data_trans(data, i);
        if (remap[tr.state_in] < 0) {
            continue;
        }
        tr.state_in = remap[tr.state_in];
        tr.state_out = remap[tr.state_out];
        *odin3_fsm_data_trans(data, kept_tr++) = tr;
    }
    odin3_kit_truncate(&data->trans, kept_tr);
    if (data->reset_state != -1) {
        data->reset_state = remap[data->reset_state];
    }
}

odin3_status odin3_fsm_opt_unreachable(odin3_fsm_inst *inst) {
    odin3_fsm_data *data = &inst->data;
    size_t count = 1;
    while (count > 0) {
        size_t n_states = data->states.len;
        uint8_t *dead = odin3_util_calloc(n_states + 1);
        int32_t *remap = odin3_util_calloc((n_states + 1) * sizeof *remap);
        if (dead == NULL || remap == NULL) {
            odin3_util_free(dead);
            odin3_util_free(remap);
            return ODIN3_ERR_NO_MEMORY;
        }
        count = mark_unreachable(data, dead);
        if (count > 0) {
            drop_states(data, dead, remap);
        }
        odin3_util_free(dead);
        odin3_util_free(remap);
    }
    return ODIN3_OK;
}

/* --- O-1: unused outputs ------------------------------------------------------------------------
 */

void odin3_fsm_opt_unused_outputs(odin3_fsm_inst *inst, const uint8_t *unused) {
    odin3_fsm_data *data = &inst->data;
    if (unused == NULL) {
        return;
    }
    int32_t index = 0; /* position among the outputs that are left */
    int32_t original = data->num_outputs;
    for (int32_t out = 0; out < original; out++) {
        if (unused[out] == 0) {
            index++;
            continue;
        }
        odin3_log(ODIN3_LOG_DEBUG, "fsm: removing unused output signal %d", out);
        erase_sig(&inst->ctrl_out, (size_t)index);
        for (size_t tr = 0; tr < data->trans.len; tr++) {
            erase_bit(odin3_fsm_data_trans(data, tr)->ctrl_out, data->num_outputs, index);
        }
        data->num_outputs--;
    }
}

/* --- O-2: aliased inputs ------------------------------------------------------------------------
 */

/* Two indices: the aliased inputs, or an input and an output. */
typedef struct pair_ctx {
    int32_t first, second;
} pair_ctx;

/* Alias: the two bits must agree once a don't care takes the other's value (:159-194). */
static bool alias_keep(const odin3_fsm_trans *tr, const void *ctx) {
    const pair_ctx *pc = ctx;
    uint8_t *in_i = &tr->ctrl_in[pc->first];
    uint8_t *in_j = &tr->ctrl_in[pc->second];
    if (*in_i > ODIN3_FSM_BIT_1) {
        *in_i = *in_j;
    } else if (*in_j > ODIN3_FSM_BIT_1) {
        *in_j = *in_i;
    }
    return *in_i == *in_j;
}

/* Finds the first pair (i, j > i) of equal control inputs, scanning from *pair (inclusive). */
static bool find_alias(const odin3_fsm_inst *inst, pair_ctx *pair) {
    const odin3_fsm_sig *in = inst->ctrl_in.data;
    int32_t count = (int32_t)inst->ctrl_in.len;
    for (int32_t i = pair->first; i < count; i++) {
        for (int32_t j = (i == pair->first ? pair->second : i + 1); j < count; j++) {
            if (in[i] == in[j]) {
                pair->first = i;
                pair->second = j;
                return true;
            }
        }
    }
    return false;
}

void odin3_fsm_opt_alias_inputs(odin3_fsm_inst *inst) {
    odin3_fsm_data *data = &inst->data;
    pair_ctx pair = {0, 1};
    while (find_alias(inst, &pair)) {
        odin3_log(ODIN3_LOG_DEBUG, "fsm: input signals %d and %d are one signal", pair.first,
                  pair.second);
        filter_trans(data, alias_keep, &pair);
        remove_input(inst, pair.second);
        /* Yosys: ctrl_in.remove(j--, 1); the loop's j++ then retries the same index */
    }
}

/* --- O-3: feedback inputs ----------------------------------------------------------------------
 */

/* Feedback: the input must be a don't care or equal the output (:196-227). */
static bool feedback_keep(const odin3_fsm_trans *tr, const void *ctx) {
    const pair_ctx *pc = ctx;
    uint8_t in_bit = tr->ctrl_in[pc->first];
    return in_bit > ODIN3_FSM_BIT_1 || in_bit == tr->ctrl_out[pc->second];
}

/* The first input at index `pair->first` or later that is output `pair->second`, or -1. */
static int32_t find_feedback(const odin3_fsm_inst *inst, pair_ctx from) {
    const odin3_fsm_sig *in = inst->ctrl_in.data;
    const odin3_fsm_sig *outs = inst->ctrl_out.data;
    for (int32_t i = from.first; i < (int32_t)inst->ctrl_in.len; i++) {
        if (in[i] == outs[from.second]) {
            return i;
        }
    }
    return -1;
}

void odin3_fsm_opt_feedback_inputs(odin3_fsm_inst *inst) {
    odin3_fsm_data *data = &inst->data;
    for (int32_t out = 0; out < (int32_t)inst->ctrl_out.len; out++) {
        pair_ctx pc = {0, out}; /* first: where the search for the input resumes */
        for (pc.first = find_feedback(inst, pc); pc.first >= 0;
             pc.first = find_feedback(inst, pc)) {
            odin3_log(ODIN3_LOG_DEBUG, "fsm: signal is input %d and output %d", pc.first, out);
            filter_trans(data, feedback_keep, &pc);
            remove_input(inst, pc.first);
        }
    }
}

/* --- O-4: don't-care merging ---------------------------------------------------------------------
 */

/* A pattern: ctrl_in bits of a transition (or a merged copy). */
typedef struct pat_ref {
    const uint8_t *bits;
} pat_ref;

/* A list of patterns kept sorted by the Const order, without repeats. */
typedef struct pat_list {
    odin3_vec items; /* pat_ref */
    uint32_t width;
} pat_list;

static int cmp_pat(const void *lhs, const void *rhs, const void *ctx) {
    uint32_t width = *(const uint32_t *)ctx;
    return odin3_fsm_const_cmp(((const pat_ref *)lhs)->bits, width, ((const pat_ref *)rhs)->bits,
                               width);
}

/* Sorts the list and drops the repeats. NO_MEMORY with the list unsorted. */
static odin3_status sort_unique(pat_list *list) {
    sort_req req = {list->items.data, list->items.len, sizeof(pat_ref), cmp_pat, &list->width};
    odin3_status st = msort(&req);
    pat_ref *items = list->items.data;
    size_t kept = 0;
    for (size_t i = 0; st == ODIN3_OK && i < list->items.len; i++) {
        bool repeat = kept > 0 && cmp_pat(&items[kept - 1], &items[i], &list->width) == 0;
        if (!repeat) {
            items[kept++] = items[i];
        }
    }
    if (st == ODIN3_OK) {
        odin3_kit_truncate(&list->items, kept);
    }
    return st;
}

/* True when `pattern` is in the sorted list. */
static bool has_pattern(const pat_list *list, const uint8_t *pattern) {
    const pat_ref *items = list->items.data;
    size_t lo = 0;
    size_t hi = list->items.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = odin3_fsm_const_cmp(items[mid].bits, list->width, pattern, list->width);
        if (cmp == 0) {
            return true;
        }
        lo = cmp < 0 ? mid + 1 : lo;
        hi = cmp < 0 ? hi : mid;
    }
    return false;
}

/* What the worker for one bit needs. */
typedef struct merge_req {
    odin3_fsm_data *data;
    const pat_list *list;
    int32_t bit;
} merge_req;

/* The pattern that replaces `pattern` in the worker for the bit (:229-262): itself, or the merge
 * of the pair it forms with its partner. *merged says which. NO_MEMORY. */
static odin3_status merge_one(const merge_req *req, const uint8_t *pattern, pat_ref *chosen,
                              bool *merged) {
    chosen->bits = pattern;
    *merged = false;
    if (pattern[req->bit] > ODIN3_FSM_BIT_1) {
        return ODIN3_OK;
    }
    uint8_t *copy = odin3_fsm_data_bits(req->data, req->list->width);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, pattern, req->list->width);
    copy[req->bit] = pattern[req->bit] == ODIN3_FSM_BIT_1 ? ODIN3_FSM_BIT_0 : ODIN3_FSM_BIT_1;
    if (has_pattern(req->list, copy)) {
        copy[req->bit] = ODIN3_FSM_BIT_DC;
        chosen->bits = copy;
        *merged = true;
    }
    return ODIN3_OK;
}

/* One worker call on `bit`; the new list replaces `list`. *did when a pair merged. */
static odin3_status merge_bit(odin3_fsm_data *data, pat_list *list, int32_t bit, bool *did) {
    pat_list next;
    next.width = list->width;
    odin3_vec_init(&next.items, sizeof(pat_ref));
    const pat_ref *items = list->items.data;
    const merge_req req = {data, list, bit};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < list->items.len; i++) {
        pat_ref chosen;
        bool merged = false;
        st = merge_one(&req, items[i].bits, &chosen, &merged);
        pat_ref *slot = st == ODIN3_OK ? odin3_vec_push(&next.items) : NULL;
        st = st == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : st;
        if (slot != NULL) {
            *slot = chosen;
            *did = *did || merged;
        }
    }
    st = st == ODIN3_OK ? sort_unique(&next) : st;
    if (st == ODIN3_OK) {
        odin3_vec_free(&list->items);
        list->items = next.items;
    } else {
        odin3_vec_free(&next.items);
    }
    return st;
}

/* The merge loop of one group (:276-286): bits in order, repeated to a fixpoint. */
static odin3_status merge_group(odin3_fsm_data *data, pat_list *list) {
    bool did = true;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && did) {
        did = false;
        for (int32_t bit = 0; st == ODIN3_OK && bit < data->num_inputs; bit++) {
            st = merge_bit(data, list, bit, &did);
        }
    }
    return st;
}

/* Transition key order: state_in, state_out, then ctrl_out in the Const order. */
static int cmp_key(const void *lhs, const void *rhs, const void *ctx) {
    const odin3_fsm_trans *lhs_tr = lhs;
    const odin3_fsm_trans *rhs_tr = rhs;
    uint32_t n_out = *(const uint32_t *)ctx;
    int cmp = (lhs_tr->state_in > rhs_tr->state_in) - (lhs_tr->state_in < rhs_tr->state_in);
    if (cmp == 0) {
        cmp = (lhs_tr->state_out > rhs_tr->state_out) - (lhs_tr->state_out < rhs_tr->state_out);
    }
    return cmp != 0 ? cmp : odin3_fsm_const_cmp(lhs_tr->ctrl_out, n_out, rhs_tr->ctrl_out, n_out);
}

/* Appends one transition per merged pattern of the group, with a private copy of ctrl_out. */
static odin3_status emit_group(odin3_fsm_data *data, const odin3_fsm_trans *key,
                               const pat_list *list, odin3_vec *out) {
    const pat_ref *items = list->items.data;
    for (size_t i = 0; i < list->items.len; i++) {
        odin3_fsm_trans tr = *key;
        tr.ctrl_in = (uint8_t *)items[i].bits;
        tr.ctrl_out = odin3_fsm_data_bits(data, (size_t)data->num_outputs);
        odin3_fsm_trans *slot = tr.ctrl_out != NULL ? odin3_vec_push(out) : NULL;
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memcpy(tr.ctrl_out, key->ctrl_out, (size_t)data->num_outputs);
        *slot = tr;
    }
    return ODIN3_OK;
}

/* One group of count transitions sharing a key: its patterns, merged, emitted. */
static odin3_status do_group(odin3_fsm_data *data, const odin3_fsm_trans *group, size_t count,
                             odin3_vec *out) {
    pat_list list;
    list.width = (uint32_t)data->num_inputs;
    odin3_vec_init(&list.items, sizeof(pat_ref));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        pat_ref *slot = odin3_vec_push(&list.items);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            slot->bits = group[i].ctrl_in;
        }
    }
    st = st == ODIN3_OK ? sort_unique(&list) : st;
    st = st == ODIN3_OK ? merge_group(data, &list) : st;
    st = st == ODIN3_OK ? emit_group(data, group, &list, out) : st;
    odin3_vec_free(&list.items);
    return st;
}

odin3_status odin3_fsm_opt_dont_care(odin3_fsm_inst *inst) {
    odin3_fsm_data *data = &inst->data;
    size_t count = data->trans.len;
    odin3_fsm_trans *items = data->trans.data;
    uint32_t n_out = (uint32_t)data->num_outputs;
    sort_req req = {items, count, sizeof *items, cmp_key, &n_out};
    odin3_status st = msort(&req);
    odin3_vec out;
    odin3_vec_init(&out, sizeof(odin3_fsm_trans));
    for (size_t lo = 0; st == ODIN3_OK && lo < count;) {
        size_t hi = lo + 1;
        while (hi < count && cmp_key(&items[lo], &items[hi], &n_out) == 0) {
            hi++;
        }
        st = do_group(data, &items[lo], hi - lo, &out);
        lo = hi;
    }
    if (st == ODIN3_OK) { /* the new table replaces the old one */
        odin3_vec_free(&data->trans);
        data->trans = out;
    } else {
        odin3_vec_free(&out);
    }
    return st;
}

/* --- O-5: constant and unused inputs ------------------------------------------------------------
 */

/* Scans one transition against the inputs (:99-137): true when it contradicts a constant
 * input; used[] notes the non-constant inputs it constrains (also when it is dropped). */
static bool contradicts_const(const odin3_fsm_inst *inst, const odin3_fsm_trans *tr,
                              uint8_t *used) {
    const odin3_fsm_sig *in = inst->ctrl_in.data;
    for (int32_t i = 0; i < inst->data.num_inputs; i++) {
        bool defined = tr->ctrl_in[i] <= ODIN3_FSM_BIT_1;
        if (!odin3_fsm_sig_is_const(in[i])) {
            used[i] = used[i] || defined;
        } else if (defined && odin3_fsm_sig_of_bit(tr->ctrl_in[i]) != in[i]) {
            return true;
        }
    }
    return false;
}

odin3_status odin3_fsm_opt_const_inputs(odin3_fsm_inst *inst) {
    odin3_fsm_data *data = &inst->data;
    int32_t n_in = data->num_inputs;
    uint8_t *used = odin3_util_calloc((size_t)n_in + 1);
    if (used == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t kept = 0;
    for (size_t tr = 0; tr < data->trans.len; tr++) {
        const odin3_fsm_trans *cur = odin3_fsm_data_trans(data, tr);
        if (!contradicts_const(inst, cur, used)) {
            *odin3_fsm_data_trans(data, kept++) = *cur;
        }
    }
    odin3_kit_truncate(&data->trans, kept);
    for (int32_t i = n_in - 1; i >= 0; i--) {
        if (used[i] == 0) {
            odin3_log(ODIN3_LOG_DEBUG, "fsm: removing unused input signal %d", i);
            remove_input(inst, i);
        }
    }
    odin3_util_free(used);
    return ODIN3_OK;
}

/* --- the stages in order -------------------------------------------------------------------------
 */

odin3_status odin3_fsm_opt(odin3_fsm_inst *inst, const uint8_t *unused) {
    odin3_status st = odin3_fsm_opt_unreachable(inst);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_fsm_opt_unused_outputs(inst, unused);
    odin3_fsm_opt_alias_inputs(inst);
    odin3_fsm_opt_feedback_inputs(inst);
    st = odin3_fsm_opt_dont_care(inst);
    return st == ODIN3_OK ? odin3_fsm_opt_const_inputs(inst) : st;
}
