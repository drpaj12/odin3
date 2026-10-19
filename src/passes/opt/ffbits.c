/*
 * ffbits.c — `opt_merge -ffbits` (PHASE3 #48, agent default): merges the bits of word flip-flops
 * that Yosys 0.55 merges only after `techmap`. Yosys's `techmap` turns a word `$dffe`/`$sdff`/...
 * into one `$_*DFF*_` cell per bit and `opt -fast` then runs opt_merge over them
 * (opt_merge.cc: an FF cell's key holds its type, parameters, inputs and the init of Q), so two
 * bits with the same clock, D bit, enable, reset, polarities, reset value and init become one.
 * Odin III's `lower` writes enable and sync reset as a mux in front of the D pin (D depends on its
 * own Q), after which no two bits are identical; the merge has to happen before `lower`.
 *
 * Per bit the key is: the FF kind (which controls it has, their polarities, `ce_over_srst`), the
 * nets of CLK, EN, SRST, ARST, ALOAD, SET/CLR and of D or AD for that bit, the ARST and SRST reset
 * values of the bit and its init. Constant nets are canonical (kit canon_consts), so a net ID
 * compares as a value. Bits with the same key form a class; its survivor is the lowest (node ID,
 * bit), or the first bit of a `keep` node, and every other bit of a node without `keep` is
 * dropped: its Q net is tied to the survivor's Q net. A node keeps its remaining bits as a word
 * cell of the same kind (odin3_ff_emit_shrunk, which keeps name and attributes and removes the
 * init of a dropped Q); a node with no bit left goes. The survivor's node is rebuilt with a DERIVED
 * record whose parents are [its own record, the records of the nodes its class dropped bits from,
 * in ID order]; a node that only loses bits gets emit_shrunk's DERIVED record of itself. Rounds
 * repeat until one drops nothing, because a drop changes the D net of the readers of the dropped
 * Q. Fine cells (`$_DFF_P_`, ...) are left to the ordinary opt_merge. FFs with an unconnected
 * control, D or Q pin, and with -keepdc those with an undefined init bit, are no candidates.
 *
 * Deviation from Yosys: a `keep` bit that meets an equal non-`keep` bit stays separate if it is
 * not the first of its class (Yosys lets the `keep` cell survive); rare, sound.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    KEY_WORDS = 11,
    K_FLAGS = 0,
    K_VALUES,
    K_CLK,
    K_CE,
    K_SRST,
    K_ARST,
    K_ALOAD,
    K_D,
    K_AD,
    K_SET,
    K_CLR,
    FLAG_CLK = 0,
    FLAG_CE,
    FLAG_SRST,
    FLAG_ARST,
    FLAG_ALOAD,
    FLAG_SR,
    FLAG_CE_OVER_SRST,
    FLAG_POL_CLK,
    FLAG_POL_CE,
    FLAG_POL_SRST,
    FLAG_POL_ARST,
    FLAG_POL_ALOAD,
    FLAG_POL_SET,
    FLAG_POL_CLR,
    FLAG_GCLK,
    VAL_SRST_SHIFT = 4,
    VAL_INIT_SHIFT = 8
};

/* One FF bit: its key, its node, its position and its Q net. */
typedef struct bitrec {
    uint64_t key[KEY_WORDS];
    uint32_t node, bit, q;
} bitrec;

/* One dropped bit: tie `drop_q` to `keep_q`, the survivor being a bit of `surv`. */
typedef struct dropped {
    uint32_t node, bit, drop_q, keep_q, surv;
    odin3_prov_id prov; /* the dropped node's record, read before anything changes */
} dropped;

typedef struct ffbits_state {
    odin3_opt_ctx *oc;
    odin3_vec recs;  /* bitrec */
    odin3_vec drops; /* dropped, sorted by node */
    odin3_vec inc;   /* dropped, sorted by surv */
    odin3_vec touch; /* uint32_t node IDs */
} ffbits_state;

static void state_init(ffbits_state *fs, odin3_opt_ctx *oc) {
    memset(fs, 0, sizeof *fs);
    fs->oc = oc;
    odin3_vec_init(&fs->recs, sizeof(bitrec));
    odin3_vec_init(&fs->drops, sizeof(dropped));
    odin3_vec_init(&fs->inc, sizeof(dropped));
    odin3_vec_init(&fs->touch, sizeof(uint32_t));
}

static void state_free(ffbits_state *fs) {
    odin3_vec_free(&fs->recs);
    odin3_vec_free(&fs->drops);
    odin3_vec_free(&fs->inc);
    odin3_vec_free(&fs->touch);
}

/* --- the keys -------------------------------------------------------------------------------- */

static uint64_t bit_flag(bool value, unsigned shift) {
    return value ? (UINT64_C(1) << shift) : 0U;
}

static uint64_t flags_word(const odin3_ffdata *ff) {
    uint64_t word = bit_flag(ff->has_clk, FLAG_CLK) | bit_flag(ff->has_ce, FLAG_CE) |
                    bit_flag(ff->has_srst, FLAG_SRST) | bit_flag(ff->has_arst, FLAG_ARST) |
                    bit_flag(ff->has_aload, FLAG_ALOAD) | bit_flag(ff->has_sr, FLAG_SR) |
                    bit_flag(ff->ce_over_srst, FLAG_CE_OVER_SRST) |
                    bit_flag(ff->has_gclk, FLAG_GCLK);
    word |= bit_flag(ff->has_clk && ff->pol_clk, FLAG_POL_CLK) |
            bit_flag(ff->has_ce && ff->pol_ce, FLAG_POL_CE) |
            bit_flag(ff->has_srst && ff->pol_srst, FLAG_POL_SRST) |
            bit_flag(ff->has_arst && ff->pol_arst, FLAG_POL_ARST) |
            bit_flag(ff->has_aload && ff->pol_aload, FLAG_POL_ALOAD) |
            bit_flag(ff->has_sr && ff->pol_set, FLAG_POL_SET) |
            bit_flag(ff->has_sr && ff->pol_clr, FLAG_POL_CLR);
    return word;
}

static uint64_t net_word(const odin3_vec *vec, uint32_t idx) {
    return (uint64_t)odin3_ff_net_at(vec, idx).v;
}

static uint64_t values_word(const odin3_ffdata *ff, uint32_t idx) {
    uint64_t arst = ff->has_arst ? (uint64_t)odin3_ff_bit_at(&ff->val_arst, idx) : 0U;
    uint64_t srst = ff->has_srst ? (uint64_t)odin3_ff_bit_at(&ff->val_srst, idx) : 0U;
    uint64_t init = (uint64_t)odin3_ff_bit_at(&ff->val_init, idx);
    return arst | (srst << VAL_SRST_SHIFT) | (init << VAL_INIT_SHIFT);
}

static void make_key(const odin3_ffdata *ff, uint32_t idx, uint64_t *key) {
    memset(key, 0, sizeof(uint64_t) * KEY_WORDS);
    key[K_FLAGS] = flags_word(ff);
    key[K_VALUES] = values_word(ff, idx);
    key[K_CLK] = ff->has_clk ? (uint64_t)ff->sig_clk.v : 0U;
    key[K_CE] = ff->has_ce ? (uint64_t)ff->sig_ce.v : 0U;
    key[K_SRST] = ff->has_srst ? (uint64_t)ff->sig_srst.v : 0U;
    key[K_ARST] = ff->has_arst ? (uint64_t)ff->sig_arst.v : 0U;
    key[K_ALOAD] = ff->has_aload ? (uint64_t)ff->sig_aload.v : 0U;
    key[K_D] = ff->has_clk || ff->has_gclk ? net_word(&ff->sig_d, idx) : 0U;
    key[K_AD] = ff->has_aload ? net_word(&ff->sig_ad, idx) : 0U;
    key[K_SET] = ff->has_sr ? net_word(&ff->sig_set, idx) : 0U;
    key[K_CLR] = ff->has_sr ? net_word(&ff->sig_clr, idx) : 0U;
}

static bool vec_closed(const odin3_vec *vec, uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
        if (!odin3_net_valid(odin3_ff_net_at(vec, i))) {
            return false;
        }
    }
    return true;
}

/* True when the single-net controls the FF uses are all connected. */
static bool controls_closed(const odin3_ffdata *ff) {
    const bool uses[] = {ff->has_clk, ff->has_ce, ff->has_srst, ff->has_arst, ff->has_aload};
    const odin3_net_id nets[] = {ff->sig_clk, ff->sig_ce, ff->sig_srst, ff->sig_arst,
                                 ff->sig_aload};
    for (size_t k = 0; k < sizeof uses / sizeof uses[0]; k++) {
        if (uses[k] && !odin3_net_valid(nets[k])) {
            return false;
        }
    }
    return true;
}

/* True when every pin the FF uses has a net. */
static bool ff_closed(const odin3_ffdata *ff) {
    uint32_t width = ff->width;
    const odin3_vec *per_bit[] = {&ff->sig_d, &ff->sig_ad, &ff->sig_set, &ff->sig_clr};
    const bool uses[] = {ff->has_clk || ff->has_gclk, ff->has_aload, ff->has_sr, ff->has_sr};
    if (!vec_closed(&ff->sig_q, width) || !controls_closed(ff)) {
        return false;
    }
    for (size_t k = 0; k < sizeof uses / sizeof uses[0]; k++) {
        if (uses[k] && !vec_closed(per_bit[k], width)) {
            return false;
        }
    }
    return true;
}

static bool init_defined(const odin3_ffdata *ff, uint32_t idx) {
    odin3_bit bit = odin3_ff_bit_at(&ff->val_init, idx);
    return bit == ODIN3_BIT_0 || bit == ODIN3_BIT_1;
}

/* --- scan: one record per bit ----------------------------------------------------------------- */

static odin3_status add_node_bits(ffbits_state *fs, odin3_node_id node, const odin3_ffdata *ff) {
    for (uint32_t i = 0; i < ff->width; i++) {
        if (fs->oc->opts->keepdc && !init_defined(ff, i)) {
            continue;
        }
        bitrec *rec = odin3_vec_push(&fs->recs);
        if (rec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        make_key(ff, i, rec->key);
        rec->node = node.v;
        rec->bit = i;
        rec->q = odin3_ff_net_at(&ff->sig_q, i).v;
    }
    return ODIN3_OK;
}

static bool is_word_ff(const odin3_opt_ctx *oc, odin3_node_id node) {
    if (!odin3_node_live(oc->module, node)) {
        return false;
    }
    odin3_celltype_id type = odin3_node_type(oc->module, node);
    const odin3_celltype_def *def = odin3_celltype_get(oc->design, type);
    return def != NULL && def->gran == ODIN3_GRAN_WORD && odin3_ff_is_ff(oc, type);
}

static odin3_status scan(ffbits_state *fs) {
    odin3_opt_ctx *oc = fs->oc;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(oc->module); id++) {
        odin3_node_id node = {id};
        if (!is_word_ff(oc, node)) {
            continue;
        }
        odin3_ffdata ff;
        st = odin3_ff_load(oc, node, &ff);
        if (st == ODIN3_OK && ff_closed(&ff)) {
            st = add_node_bits(fs, node, &ff);
        }
        odin3_ffdata_free(&ff);
    }
    return st;
}

/* --- classes -----------------------------------------------------------------------------------
 */

/* -1, 0 or 1 as lhs is below, equal to or above rhs. */
static int cmp_num(uint64_t lhs, uint64_t rhs) {
    return (lhs > rhs) - (lhs < rhs);
}

static int cmp_rec(const void *lhs, const void *rhs) {
    const bitrec *left = lhs;
    const bitrec *right = rhs;
    for (size_t k = 0; k < KEY_WORDS; k++) {
        if (left->key[k] != right->key[k]) {
            return cmp_num(left->key[k], right->key[k]);
        }
    }
    if (left->node != right->node) {
        return cmp_num(left->node, right->node);
    }
    return cmp_num(left->bit, right->bit);
}

static bool same_key(const bitrec *left, const bitrec *right) {
    return memcmp(left->key, right->key, sizeof left->key) == 0;
}

static bool node_keep(const ffbits_state *fs, uint32_t node) {
    return odin3_kit_keep(fs->oc, (odin3_objref){ODIN3_OBJ_NODE, node});
}

typedef struct class_range {
    size_t from, to;
} class_range;

/* The survivor of the class recs[from .. to): the first bit of a `keep` node, else the first. */
static size_t survivor_of(const ffbits_state *fs, const class_range *range) {
    const bitrec *recs = fs->recs.data;
    for (size_t k = range->from; k < range->to; k++) {
        if (node_keep(fs, recs[k].node)) {
            return k;
        }
    }
    return range->from;
}

static odin3_status add_drop(ffbits_state *fs, const bitrec *drop, const bitrec *surv) {
    dropped *slot = odin3_vec_push(&fs->drops);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->node = drop->node;
    slot->bit = drop->bit;
    slot->drop_q = drop->q;
    slot->keep_q = surv->q;
    slot->surv = surv->node;
    slot->prov = odin3_node_prov(fs->oc->module, (odin3_node_id){drop->node});
    return ODIN3_OK;
}

static odin3_status class_drops(ffbits_state *fs, const class_range *range) {
    const bitrec *recs = fs->recs.data;
    size_t surv = survivor_of(fs, range);
    odin3_status st = ODIN3_OK;
    for (size_t k = range->from; st == ODIN3_OK && k < range->to; k++) {
        if (k != surv && !node_keep(fs, recs[k].node)) {
            st = add_drop(fs, &recs[k], &recs[surv]);
        }
    }
    return st;
}

static odin3_status find_drops(ffbits_state *fs) {
    bitrec *recs = fs->recs.data;
    size_t total = fs->recs.len;
    if (total > 1) {
        qsort(recs, total, sizeof *recs, cmp_rec);
    }
    odin3_status st = ODIN3_OK;
    for (size_t from = 0; st == ODIN3_OK && from < total;) {
        size_t to = from + 1U;
        while (to < total && same_key(&recs[from], &recs[to])) {
            to++;
        }
        class_range range = {from, to};
        st = to - from > 1U ? class_drops(fs, &range) : ODIN3_OK;
        from = to;
    }
    return st;
}

/* --- the rewrite -------------------------------------------------------------------------------
 */

static int cmp_drop_node(const void *lhs, const void *rhs) {
    const dropped *left = lhs;
    const dropped *right = rhs;
    if (left->node != right->node) {
        return cmp_num(left->node, right->node);
    }
    return cmp_num(left->bit, right->bit);
}

static int cmp_drop_surv(const void *lhs, const void *rhs) {
    const dropped *left = lhs;
    const dropped *right = rhs;
    if (left->surv != right->surv) {
        return cmp_num(left->surv, right->surv);
    }
    return cmp_drop_node(lhs, rhs);
}

static int cmp_u32(const void *lhs, const void *rhs) {
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;
    return cmp_num(left, right);
}

/* touch = the node IDs that lose a bit or survive a class, sorted, once each; inc = drops by surv.
 */
static odin3_status plan_nodes(ffbits_state *fs) {
    const dropped *drops = fs->drops.data;
    odin3_status st = odin3_vec_reserve(&fs->inc, fs->drops.len);
    for (size_t k = 0; st == ODIN3_OK && k < fs->drops.len; k++) {
        dropped *slot = odin3_vec_push(&fs->inc);
        uint32_t *own = odin3_vec_push(&fs->touch);
        uint32_t *other = odin3_vec_push(&fs->touch);
        if (slot == NULL || own == NULL || other == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = drops[k];
        *own = drops[k].node;
        *other = drops[k].surv;
    }
    if (fs->inc.len > 1) {
        qsort(fs->inc.data, fs->inc.len, sizeof(dropped), cmp_drop_surv);
    }
    if (fs->touch.len > 1) {
        qsort(fs->touch.data, fs->touch.len, sizeof(uint32_t), cmp_u32);
    }
    size_t out = 0;
    uint32_t *ids = fs->touch.data;
    for (size_t k = 0; k < fs->touch.len; k++) {
        if (out == 0 || ids[out - 1U] != ids[k]) {
            ids[out++] = ids[k];
        }
    }
    fs->touch.len = out;
    return st;
}

/* The first index of the entries of `list` (sorted by key) whose key is not below node. */
static size_t lower_bound(const odin3_vec *list, uint32_t node, bool by_surv) {
    const dropped *items = list->data;
    size_t lo = 0;
    size_t hi = list->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        uint32_t key = by_surv ? items[mid].surv : items[mid].node;
        if (key < node) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The record for node: DERIVED of [its own record, the records dropped into it], or 0 for none
 * dropped into it. */
static odin3_status node_prov(ffbits_state *fs, uint32_t node, odin3_prov_id *out) {
    const dropped *inc = fs->inc.data;
    size_t first = lower_bound(&fs->inc, node, true);
    odin3_vec provs;
    odin3_vec_init(&provs, sizeof(odin3_prov_id));
    odin3_prov_id own = odin3_node_prov(fs->oc->module, (odin3_node_id){node});
    odin3_status st = ODIN3_OK;
    *out = (odin3_prov_id){0};
    for (size_t k = first; st == ODIN3_OK && k < fs->inc.len && inc[k].surv == node; k++) {
        odin3_prov_id *slot = provs.len == 0 && own.v != 0 ? odin3_vec_push(&provs) : NULL;
        if (slot != NULL) {
            *slot = own;
        }
        slot = inc[k].prov.v != 0 ? odin3_vec_push(&provs) : NULL;
        if (slot != NULL) {
            *slot = inc[k].prov;
        } else if (inc[k].prov.v != 0) {
            st = ODIN3_ERR_NO_MEMORY;
        }
    }
    if (st == ODIN3_OK && provs.len > 0) {
        st = odin3_prov_derive(fs->oc->pass, (odin3_prov_list){provs.data, (uint32_t)provs.len},
                               out);
    }
    odin3_vec_free(&provs);
    return st;
}

/* The ties and kept bits of node's drops (drops[first..last)): ties[i], kept[] as bit indexes. */
typedef struct shrink_plan {
    odin3_net_pair *ties;
    uint32_t *kept;
    uint32_t n_ties, n_kept;
} shrink_plan;

static odin3_status plan_shrink(const odin3_ffdata *ff, const dropped *list, size_t count,
                                shrink_plan *sp) {
    memset(sp, 0, sizeof *sp);
    sp->ties = odin3_util_calloc(sizeof *sp->ties * (count + 1U));
    sp->kept = odin3_util_calloc(sizeof *sp->kept * ((size_t)ff->width + 1U));
    if (sp->ties == NULL || sp->kept == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t next = 0;
    for (uint32_t bit = 0; bit < ff->width; bit++) {
        if (next < count && list[next].bit == bit) {
            sp->ties[sp->n_ties].keep = (odin3_net_id){list[next].keep_q};
            sp->ties[sp->n_ties].drop = (odin3_net_id){list[next].drop_q};
            sp->n_ties++;
            next++;
        } else {
            sp->kept[sp->n_kept++] = bit;
        }
    }
    return ODIN3_OK;
}

static void plan_free(shrink_plan *sp) {
    odin3_util_free(sp->ties);
    odin3_util_free(sp->kept);
}

/* Rebuilds node: the bits listed in its drops go, tied to their survivors, the record of a class
 * it survives is the new cell's. */
static odin3_status rewrite_node(ffbits_state *fs, uint32_t id) {
    odin3_opt_ctx *oc = fs->oc;
    odin3_node_id node = {id};
    const dropped *drops = fs->drops.data;
    size_t first = lower_bound(&fs->drops, id, false);
    size_t count = 0;
    while (first + count < fs->drops.len && drops[first + count].node == id) {
        count++;
    }
    odin3_ffdata ff;
    odin3_ffdata part;
    shrink_plan sp;
    odin3_kit_begin_rewrite(oc);
    odin3_status st = odin3_ff_load(oc, node, &ff);
    memset(&sp, 0, sizeof sp);
    odin3_ffdata_init(&part);
    st = st == ODIN3_OK ? plan_shrink(&ff, drops + first, count, &sp) : st;
    st = st == ODIN3_OK ? odin3_ff_slice(&ff, (odin3_ff_bits){sp.kept, sp.n_kept}, &part) : st;
    part.node = node;
    odin3_prov_id prov = {0};
    st = st == ODIN3_OK ? node_prov(fs, id, &prov) : st;
    part.prov = prov.v != 0 ? prov : part.prov;
    part.prov_final = prov.v != 0;
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? odin3_ff_emit_shrunk(oc, &part, sp.ties, sp.n_ties, &made) : st;
    plan_free(&sp);
    odin3_ffdata_free(&part);
    odin3_ffdata_free(&ff);
    return st;
}

static odin3_status apply(ffbits_state *fs) {
    odin3_status st = plan_nodes(fs);
    const uint32_t *ids = fs->touch.data;
    for (size_t k = 0; st == ODIN3_OK && k < fs->touch.len; k++) {
        st = rewrite_node(fs, ids[k]);
    }
    return st;
}

/* --- the module --------------------------------------------------------------------------------
 */

/* One round: scan, classes, rewrite. *dropped_bits is the number of bits dropped. */
static odin3_status round_run(odin3_opt_ctx *oc, uint32_t *dropped_bits) {
    ffbits_state fs;
    state_init(&fs, oc);
    odin3_status st = scan(&fs);
    st = st == ODIN3_OK ? find_drops(&fs) : st;
    if (st == ODIN3_OK && fs.drops.len > 0) {
        qsort(fs.drops.data, fs.drops.len, sizeof(dropped), cmp_drop_node);
        st = apply(&fs);
        *dropped_bits += (uint32_t)fs.drops.len;
    }
    state_free(&fs);
    return st;
}

odin3_status odin3_opt_ffbits_module(odin3_opt_ctx *oc, uint32_t *dropped_bits) {
    uint32_t round_bits = 1;
    odin3_status st = odin3_opt_canon_consts(oc);
    while (st == ODIN3_OK && round_bits > 0) {
        uint32_t before = *dropped_bits;
        st = round_run(oc, dropped_bits);
        round_bits = *dropped_bits - before;
    }
    return st;
}
