/*
 * merge.c — opt_merge (2F spec OP-6, §6; plan Task 8): common-subexpression elimination, Yosys
 * 0.55 opt_merge.cc over Odin III's nodes.
 *
 * A candidate's key is a run of 64-bit words: its type, its parameters, its input operands (a net
 * ID, or a constant value when the net carries one) with §6's normalisations, and the init of Q
 * for an FF. A 64-bit hash of the words indexes a u64map whose value heads a chain of entries;
 * equality compares the words, so a collision costs time and never correctness.
 *
 * Sweeps visit the nodes in ID order; a node whose key is already in the table is dropped into the
 * earlier one (the later one when only it has `keep`), and the sweeps repeat until one drops
 * nothing. Then each class is closed with one rebuild of its survivor whose DERIVED record has the
 * parents [survivor, drops in ID order] (plan point (p), spec §25 item 4).
 *
 * Differences from Yosys (spec §17): Yosys walks its cell dictionary newest first and so keeps the
 * later of two equal cells, §6 the earlier (a spec choice, row 7). Nodes with no output port, or
 * with more than one, are never candidates, because kit_connect redirects one output port: this
 * misses Yosys's merges of `$memwr` (row 13). The commutative swap applies only when A_SIGNED ==
 * B_SIGNED (row 14): a mixed-sign cell is unsigned (#57), so a swap would be sound, but such
 * cells are rare and kept apart; Yosys swaps them too.
 * A node with an unconnected input pin is no candidate (row 16), as in opt_reduce.
 *
 * An FF merge never loses an init (OP-2): the dropped Q's init bits are only read before
 * kit_connect, and only after it succeeds is the survivor's net settled to one init bit.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the key hash and its test hook ----------------------------------------------------------- */

static uint64_t (*g_key_hash)(const void *key, size_t len);

void odin3_opt_merge_test_set_key_hash(uint64_t (*fn)(const void *key, size_t len)) {
    g_key_hash = fn;
}

static uint64_t hash_key(const uint64_t *words, size_t n) {
    if (g_key_hash != NULL) {
        return g_key_hash(words, n * sizeof *words);
    }
    return odin3_hash_bytes((odin3_bytes){words, n * sizeof *words}, ODIN3_HASH_SEED);
}

/* --- cell classes ----------------------------------------------------------------------------- */

typedef enum key_class {
    CLS_SKIP,
    CLS_PLAIN, /* every input operand vector, in port order */
    CLS_COMM,  /* A and B ordered */
    CLS_RXOR,  /* A sorted */
    CLS_RSET,  /* A sorted and de-duplicated */
    CLS_PMUX   /* the (S bit, B slice) pairs sorted */
} key_class;

enum {
    CLS_MASK = 0x0F,
    CLS_FF = 0x10,    /* Q's init is part of the key */
    CLS_CONST = 0x20, /* the type has a const_value hook: a constant node is not a candidate */
    CLS_MUX = 0x40,   /* $mux, $pmux: not under -nomux */
    CLS_UNKNOWN = 0xFF,
    MAX_PARAMS = 16,
    KIND_SHIFT = 62,
    PACK_BYTES = 8
};

typedef struct class_name {
    const char *name;
    key_class cls;
} class_name;

static const class_name k_class_names[] = {
    {"$and", CLS_COMM},         {"$or", CLS_COMM},         {"$xor", CLS_COMM},
    {"$xnor", CLS_COMM},        {"$add", CLS_COMM},        {"$mul", CLS_COMM},
    {"$logic_and", CLS_COMM},   {"$logic_or", CLS_COMM},   {"$_AND_", CLS_COMM},
    {"$_OR_", CLS_COMM},        {"$_XOR_", CLS_COMM},      {"$reduce_xor", CLS_RXOR},
    {"$reduce_xnor", CLS_RXOR}, {"$reduce_and", CLS_RSET}, {"$reduce_or", CLS_RSET},
    {"$reduce_bool", CLS_RSET}, {"$pmux", CLS_PMUX},
};

/* Types that are never candidates: Yosys skips $mem, erases $tribuf and $_TBUF_ from its set. */
static const char *const k_never[] = {"$mem", "$tribuf", "$_TBUF_", "$proc"};

static bool name_in(const char *name, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static key_class class_by_name(const char *name) {
    for (size_t i = 0; i < sizeof k_class_names / sizeof k_class_names[0]; i++) {
        if (strcmp(k_class_names[i].name, name) == 0) {
            return k_class_names[i].cls;
        }
    }
    return CLS_PLAIN;
}

static uint32_t count_outputs(const odin3_celltype_def *def) {
    uint32_t outs = 0;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        outs += def->ports[i].dir == ODIN3_DIR_OUT ? 1U : 0U;
    }
    return outs;
}

static uint32_t out_port(const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (def->ports[i].dir == ODIN3_DIR_OUT) {
            return i;
        }
    }
    return ODIN3_KIT_NONE;
}

/* The class byte of a type: CLS_SKIP unless the type is a candidate. */
static uint8_t classify(const odin3_opt_ctx *oc, odin3_celltype_id type,
                        const odin3_celltype_def *def) {
    if (def == NULL || (def->gran != ODIN3_GRAN_WORD && def->gran != ODIN3_GRAN_BIT) ||
        name_in(def->name, k_never, sizeof k_never / sizeof k_never[0]) ||
        count_outputs(def) != 1 || def->n_params > MAX_PARAMS) {
        return CLS_SKIP;
    }
    uint8_t cls = (uint8_t)class_by_name(def->name);
    cls |= odin3_ff_is_ff(oc, type) ? CLS_FF : 0U;
    cls |= def->const_value != NULL ? CLS_CONST : 0U;
    cls |= strcmp(def->name, "$mux") == 0 || strcmp(def->name, "$pmux") == 0 ? CLS_MUX : 0U;
    return cls;
}

/* --- the worker ---------------------------------------------------------------------------------
 */

typedef struct entry {
    uint32_t node;
    uint32_t next; /* index + 1 of the next entry of the chain, 0 ends it */
    uint32_t len;  /* key words */
    uint64_t off;  /* first key word in store */
} entry;

typedef struct merge_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    bool oom;
    uint32_t node_end, removed;
    odin3_vec classes;         /* uint8_t per cell type ID, CLS_UNKNOWN until looked up */
    odin3_vec key, ta, tb;     /* uint64_t: the current key and two operand scratch vectors */
    odin3_vec idx, idx2, bits; /* uint32_t (pmux pair order and its merge buffer); odin3_bit */
    odin3_vec store, ents;     /* uint64_t key words of every entry; entry */
    odin3_vec srcs;            /* odin3_net_id: the survivor's output nets */
    odin3_u64map *map;
    uint32_t *into; /* per node: the node it was dropped into (0: not dropped) */
    odin3_prov_id *dprov;
} merge_state;

static void put(merge_state *ms, odin3_vec *vec, uint64_t word) {
    uint64_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        ms->oom = true;
        return;
    }
    *slot = word;
}

static const uint64_t *words(const odin3_vec *vec) {
    return vec->data;
}

/* --- operands -----------------------------------------------------------------------------------
 */

#define OPERAND_NET (UINT64_C(1) << KIND_SHIFT)
#define OPERAND_NONE (UINT64_C(2) << KIND_SHIFT)

/* A constant value (kind 0), a net ID (kind 1) or an unconnected pin (kind 2), as one word. */
static uint64_t operand(const odin3_module *module, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(module, pin);
    if (!odin3_net_valid(net)) {
        return OPERAND_NONE;
    }
    odin3_const cv = odin3_net_const_value(module, net);
    if (cv != ODIN3_CONST_NONE) {
        return (uint64_t)cv - (uint64_t)ODIN3_CONST_0;
    }
    return OPERAND_NET | net.v;
}

static void port_operands(merge_state *ms, odin3_node_id node, uint32_t port, odin3_vec *out) {
    odin3_pinslice ps = odin3_node_port(ms->m, node, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        put(ms, out, operand(ms->m, (odin3_pin_id){ps.first.v + k}));
    }
}

/* Shorter first, then lexicographic. */
static int cmp_run(const uint64_t *lhs, size_t nl, const uint64_t *rhs, size_t nr) {
    if (nl != nr) {
        return nl < nr ? -1 : 1;
    }
    for (size_t i = 0; i < nl; i++) {
        if (lhs[i] != rhs[i]) {
            return lhs[i] < rhs[i] ? -1 : 1;
        }
    }
    return 0;
}

/* Sorts vec's words, a multiset. */
static void sort_words(odin3_vec *vec) {
    if (vec->len > 1) {
        qsort(vec->data, vec->len, sizeof(uint64_t), odin3_kit_cmp_u64);
    }
}

/* --- parameters ---------------------------------------------------------------------------------
 */

static void put_bytes(merge_state *ms, const uint8_t *bytes, uint32_t len) {
    put(ms, &ms->key, len);
    for (uint32_t at = 0; at < len; at += PACK_BYTES) {
        uint64_t word = 0;
        uint32_t take = len - at < PACK_BYTES ? len - at : PACK_BYTES;
        memcpy(&word, bytes + at, take);
        put(ms, &ms->key, word);
    }
}

/* One parameter: its kind, then its value (INT, STRING ID, bits, cover rows). */
static void put_param(merge_state *ms, const odin3_value *val) {
    put(ms, &ms->key, (uint64_t)val->kind);
    switch (val->kind) {
    case ODIN3_VAL_INT:
        put(ms, &ms->key, (uint64_t)val->i);
        break;
    case ODIN3_VAL_STRING:
        put(ms, &ms->key, val->str);
        break;
    case ODIN3_VAL_COVER:
        put(ms, &ms->key, val->cover_inputs);
        put_bytes(ms, val->bits, val->len);
        break;
    default: /* BITS (PROC nodes are never candidates) */
        put_bytes(ms, val->bits, val->len);
        break;
    }
}

/* --- the key ------------------------------------------------------------------------------------
 */

/* Every input operand vector of the ports not in skip, in port order. */
static void put_inputs(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def,
                       const uint32_t skip[2]) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (def->ports[i].dir == ODIN3_DIR_OUT || i == skip[0] || i == skip[1]) {
            continue;
        }
        put(ms, &ms->key, odin3_node_port(ms->m, node, i).count);
        port_operands(ms, node, i, &ms->key);
    }
}

static void put_run(merge_state *ms, const odin3_vec *run) {
    put(ms, &ms->key, run->len);
    for (size_t i = 0; i < run->len; i++) {
        put(ms, &ms->key, words(run)[i]);
    }
}

/* True unless A_SIGNED and B_SIGNED both exist and differ (conservative: a mixed-sign
 * cell is unsigned, so a swap would be sound, but it keeps its operand order). */
static bool signs_agree(const merge_state *ms, odin3_node_id node, const odin3_celltype_def *def) {
    uint32_t sa = odin3_kit_param_index(def, "A_SIGNED");
    uint32_t sb = odin3_kit_param_index(def, "B_SIGNED");
    if (sa == ODIN3_KIT_NONE || sb == ODIN3_KIT_NONE) {
        return true;
    }
    return odin3_node_param(ms->m, node, sa)->i == odin3_node_param(ms->m, node, sb)->i;
}

/* $and, $add, ...: the A and B operand vectors, the smaller one first (equal signs only). */
static void key_commutative(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def) {
    uint32_t pa = odin3_kit_port_index(def, "A");
    uint32_t pb = odin3_kit_port_index(def, "B");
    odin3_vec_clear(&ms->ta);
    odin3_vec_clear(&ms->tb);
    port_operands(ms, node, pa, &ms->ta);
    port_operands(ms, node, pb, &ms->tb);
    bool swap = signs_agree(ms, node, def) &&
                cmp_run(words(&ms->ta), ms->ta.len, words(&ms->tb), ms->tb.len) > 0;
    put_run(ms, swap ? &ms->tb : &ms->ta);
    put_run(ms, swap ? &ms->ta : &ms->tb);
    const uint32_t skip[2] = {pa, pb};
    put_inputs(ms, node, def, skip);
}

/* $reduce_*: A as a multiset (unify false) or a set (true). */
static void key_reduce(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def,
                       bool unify) {
    uint32_t pa = odin3_kit_port_index(def, "A");
    odin3_vec_clear(&ms->ta);
    port_operands(ms, node, pa, &ms->ta);
    if (unify) {
        odin3_kit_sort_unify(&ms->ta, 0);
    } else {
        sort_words(&ms->ta);
    }
    put_run(ms, &ms->ta);
    const uint32_t skip[2] = {pa, ODIN3_KIT_NONE};
    put_inputs(ms, node, def, skip);
}

/* The pairs of a $pmux: pair i is S[i] with B[i*w .. i*w + w). */
typedef struct pair_ctx {
    const uint64_t *sel, *data;
    size_t width;
} pair_ctx;

static int cmp_pair(const pair_ctx *pc, uint32_t left, uint32_t right) {
    if (pc->sel[left] != pc->sel[right]) {
        return pc->sel[left] < pc->sel[right] ? -1 : 1;
    }
    return cmp_run(pc->data + (size_t)left * pc->width, pc->width,
                   pc->data + (size_t)right * pc->width, pc->width);
}

/* The sorted merge of idx[lo, mid) and idx[mid, hi) into tmp[lo, hi). */
static void merge_runs(const pair_ctx *pc, const uint32_t *idx, uint32_t *tmp,
                       const size_t bounds[3]) {
    size_t left = bounds[0];
    size_t right = bounds[1];
    size_t out = bounds[0];
    while (left < bounds[1] && right < bounds[2]) {
        bool take_right = cmp_pair(pc, idx[right], idx[left]) < 0;
        tmp[out++] = take_right ? idx[right++] : idx[left++];
    }
    while (left < bounds[1]) {
        tmp[out++] = idx[left++];
    }
    while (right < bounds[2]) {
        tmp[out++] = idx[right++];
    }
}

/* Bottom-up merge sort of the pair indices in idx (tmp is scratch of the same length). */
static void sort_pairs(const pair_ctx *pc, uint32_t *idx, uint32_t *tmp, size_t count) {
    for (size_t run = 1; run < count; run *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * run) {
            size_t mid = lo + run < count ? lo + run : count;
            size_t hi = lo + 2 * run < count ? lo + 2 * run : count;
            const size_t bounds[3] = {lo, mid, hi};
            merge_runs(pc, idx, tmp, bounds);
        }
        memcpy(idx, tmp, count * sizeof *idx);
    }
}

static void key_pmux(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def) {
    uint32_t pb = odin3_kit_port_index(def, "B");
    uint32_t ps = odin3_kit_port_index(def, "S");
    const uint32_t skip[2] = {pb, ps};
    put_inputs(ms, node, def, skip);
    odin3_vec_clear(&ms->ta);
    odin3_vec_clear(&ms->tb);
    port_operands(ms, node, ps, &ms->ta);
    port_operands(ms, node, pb, &ms->tb);
    size_t count = ms->ta.len;
    size_t width = count == 0 ? 0 : ms->tb.len / count;
    odin3_vec_clear(&ms->idx);
    odin3_vec_clear(&ms->idx2);
    for (size_t i = 0; i < count; i++) {
        uint32_t *slot = odin3_vec_push(&ms->idx);
        const uint32_t *spare = odin3_vec_push(&ms->idx2);
        if (slot == NULL || spare == NULL) {
            ms->oom = true;
            return;
        }
        *slot = (uint32_t)i;
    }
    pair_ctx pc = {words(&ms->ta), words(&ms->tb), width};
    sort_pairs(&pc, ms->idx.data, ms->idx2.data, count);
    put(ms, &ms->key, count);
    for (size_t i = 0; i < count; i++) {
        uint32_t at = ((const uint32_t *)ms->idx.data)[i];
        put(ms, &ms->key, pc.sel[at]);
        for (size_t k = 0; k < width; k++) {
            put(ms, &ms->key, pc.data[(size_t)at * width + k]);
        }
    }
}

/* Q's init, bit by bit, after the operands. */
static void key_init(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def) {
    odin3_pinslice ps = odin3_node_port(ms->m, node, out_port(def));
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(ms->m, (odin3_pin_id){ps.first.v + k});
        put(ms, &ms->key,
            odin3_net_valid(net) ? (uint64_t)odin3_ffinit_get(ms->m, net) : (uint64_t)ODIN3_BIT_X);
    }
}

/* ms->key = node's key; false on out of memory. */
static bool build_key(merge_state *ms, odin3_node_id node, const odin3_celltype_def *def,
                      uint8_t cls) {
    odin3_vec_clear(&ms->key);
    put(ms, &ms->key, odin3_node_type(ms->m, node).v);
    for (uint32_t i = 0; i < def->n_params; i++) {
        put_param(ms, odin3_node_param(ms->m, node, i));
    }
    const uint32_t none[2] = {ODIN3_KIT_NONE, ODIN3_KIT_NONE};
    switch ((key_class)(cls & CLS_MASK)) {
    case CLS_COMM:
        key_commutative(ms, node, def);
        break;
    case CLS_RXOR:
        key_reduce(ms, node, def, false);
        break;
    case CLS_RSET:
        key_reduce(ms, node, def, true);
        break;
    case CLS_PMUX:
        key_pmux(ms, node, def);
        break;
    default:
        put_inputs(ms, node, def, none);
        break;
    }
    if ((cls & CLS_FF) != 0) {
        key_init(ms, node, def);
    }
    return !ms->oom;
}

/* --- candidates ---------------------------------------------------------------------------------
 */

/* The class byte of node's type (looked up once per type); CLS_SKIP on out of memory. */
static uint8_t class_of(merge_state *ms, odin3_celltype_id type, const odin3_celltype_def **def) {
    *def = odin3_celltype_get(ms->oc->design, type);
    while (ms->classes.len <= type.v) {
        uint8_t *slot = odin3_vec_push(&ms->classes);
        if (slot == NULL) {
            ms->oom = true;
            return CLS_SKIP;
        }
        *slot = CLS_UNKNOWN;
    }
    uint8_t *cls = odin3_vec_at(&ms->classes, type.v);
    if (*cls == CLS_UNKNOWN) {
        *cls = classify(ms->oc, type, *def);
    }
    return *cls;
}

/* True when node is a constant node (its type has a const_value hook that answers). */
static bool node_is_const(const merge_state *ms, odin3_node_id node,
                          const odin3_celltype_def *def) {
    odin3_value params[MAX_PARAMS];
    if (def->n_params > MAX_PARAMS) {
        return false;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = *odin3_node_param(ms->m, node, i);
    }
    return def->const_value(params) != ODIN3_CONST_NONE;
}

/* Under -keepdc an FF whose Q init is not fully 0/1 is no candidate. */
static bool init_defined(const merge_state *ms, odin3_node_id node, const odin3_celltype_def *def) {
    odin3_pinslice ps = odin3_node_port(ms->m, node, out_port(def));
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(ms->m, (odin3_pin_id){ps.first.v + k});
        odin3_bit bit = odin3_net_valid(net) ? odin3_ffinit_get(ms->m, net) : ODIN3_BIT_X;
        if (bit != ODIN3_BIT_0 && bit != ODIN3_BIT_1) {
            return false;
        }
    }
    return true;
}

/* True when an input pin of node has no net: such a cell is no candidate (spec 17 row 16). */
static bool has_open_input(const merge_state *ms, odin3_node_id node,
                           const odin3_celltype_def *def) {
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (def->ports[port].dir == ODIN3_DIR_OUT) {
            continue;
        }
        odin3_pinslice ps = odin3_node_port(ms->m, node, port);
        for (uint32_t k = 0; k < ps.count; k++) {
            if (!odin3_net_valid(odin3_pin_net(ms->m, (odin3_pin_id){ps.first.v + k}))) {
                return true;
            }
        }
    }
    return false;
}

/* The class byte when node is a candidate, else CLS_SKIP. */
static uint8_t candidate(merge_state *ms, odin3_node_id node, const odin3_celltype_def **def) {
    if (!odin3_node_live(ms->m, node)) {
        return CLS_SKIP;
    }
    uint8_t cls = class_of(ms, odin3_node_type(ms->m, node), def);
    if ((cls & CLS_MASK) == CLS_SKIP) {
        return CLS_SKIP;
    }
    const odin3_opt_opts *opts = ms->oc->opts;
    bool skip = ((cls & CLS_MUX) != 0 && opts->nomux) ||
                ((cls & CLS_CONST) != 0 && node_is_const(ms, node, *def)) ||
                ((cls & CLS_FF) != 0 && opts->keepdc && !init_defined(ms, node, *def)) ||
                has_open_input(ms, node, *def);
    return skip ? (uint8_t)CLS_SKIP : cls;
}

/* --- the table ----------------------------------------------------------------------------------
 */

/* Index + 1 of the entry in the chain of hash whose key equals ms->key, 0 for none. */
static uint32_t find_entry(const merge_state *ms, uint64_t hash) {
    uint64_t head = 0;
    if (!odin3_u64map_get(ms->map, hash, &head)) {
        return 0;
    }
    const uint64_t *store = ms->store.data;
    for (uint32_t at = (uint32_t)head; at != 0;) {
        const entry *ent = odin3_vec_cat(&ms->ents, at - 1U);
        if (ent->len == ms->key.len &&
            memcmp(store + ent->off, ms->key.data, (size_t)ent->len * sizeof(uint64_t)) == 0) {
            return at;
        }
        at = ent->next;
    }
    return 0;
}

static odin3_status insert_entry(merge_state *ms, uint64_t hash, odin3_node_id node) {
    uint64_t head = 0;
    (void)odin3_u64map_get(ms->map, hash, &head);
    entry *ent = odin3_vec_push(&ms->ents);
    if (ent == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ent->node = node.v;
    ent->next = (uint32_t)head;
    ent->len = (uint32_t)ms->key.len;
    ent->off = ms->store.len;
    for (size_t i = 0; i < ms->key.len; i++) {
        put(ms, &ms->store, words(&ms->key)[i]);
    }
    odin3_status st = ms->oom ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(ms->map, (odin3_kv){hash, ms->ents.len});
    }
    return st;
}

static odin3_status reset_table(merge_state *ms) {
    odin3_u64map_destroy(ms->map);
    ms->map = odin3_u64map_create(0);
    odin3_vec_clear(&ms->store);
    odin3_vec_clear(&ms->ents);
    return ms->map == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

/* --- one merge ----------------------------------------------------------------------------------
 */

enum { LABEL_MAX = 128 };

/* ms->srcs = survivor's output nets; an unconnected pin gets a fresh net first. */
static odin3_status survivor_nets(merge_state *ms, odin3_node_id surv, uint32_t port) {
    odin3_vec_clear(&ms->srcs);
    odin3_pinslice ps = odin3_node_port(ms->m, surv, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_pin_id pin = {ps.first.v + k};
        odin3_net_id net = odin3_pin_net(ms->m, pin);
        if (!odin3_net_valid(net)) {
            odin3_status st = odin3_net_create(ms->m, 0, odin3_node_prov(ms->m, surv), &net);
            st = st == ODIN3_OK ? odin3_pin_connect(ms->m, pin, net) : st;
            if (st != ODIN3_OK) {
                return st;
            }
        }
        odin3_net_id *slot = odin3_vec_push(&ms->srcs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = net;
    }
    return ODIN3_OK;
}

/* The dropped FF's Q init bits into ms->bits (x for an unconnected pin). Reads only. */
static odin3_status read_drop_inits(merge_state *ms, odin3_node_id drop, uint32_t port) {
    odin3_vec_clear(&ms->bits);
    odin3_pinslice ps = odin3_node_port(ms->m, drop, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(ms->m, (odin3_pin_id){ps.first.v + k});
        odin3_bit *slot = odin3_vec_push(&ms->bits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_net_valid(net) ? odin3_ffinit_get(ms->m, net) : ODIN3_BIT_X;
    }
    return ODIN3_OK;
}

/*
 * After kit_connect: where the dropped Q had a defined init, the survivor's net now has it twice
 * (the keys are equal, so the bits agree); set_init writes it once and clears the copy, Yosys's
 * remove_init on the dropped Q. Each step keeps the net's init value, so a failure loses nothing.
 */
static odin3_status settle_inits(merge_state *ms) {
    const odin3_net_id *nets = ms->srcs.data;
    const odin3_bit *bits = ms->bits.data;
    for (size_t k = 0; k < ms->srcs.len && k < ms->bits.len; k++) {
        if ((bits[k] != ODIN3_BIT_0 && bits[k] != ODIN3_BIT_1) || !odin3_net_live(ms->m, nets[k])) {
            continue;
        }
        odin3_status st = odin3_ffinit_set(ms->m, nets[k], odin3_ffinit_get(ms->m, nets[k]));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static void log_merge(const merge_state *ms, const char *drop_label, const char *keep_label) {
    odin3_log(ODIN3_LOG_DEBUG, "opt_merge: module '%s': '%s' is identical to '%s'",
              odin3_strtab_get(odin3_design_strtab(ms->oc->design), odin3_module_name(ms->m)),
              drop_label, keep_label);
}

/*
 * Drops `drop` into `surv`: its outputs go to the survivor's nets, the node is deleted. Everything
 * that allocates runs before kit_connect, the first destructive step (OP-2).
 */
static odin3_status merge_pair(merge_state *ms, odin3_node_id surv, odin3_node_id drop,
                               bool is_ff) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ms->oc->design, odin3_node_type(ms->m, drop));
    uint32_t port = out_port(def);
    odin3_status st = survivor_nets(ms, surv, port);
    st = st == ODIN3_OK && is_ff ? read_drop_inits(ms, drop, port) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    char keep_label[LABEL_MAX];
    char drop_label[LABEL_MAX];
    odin3_kit_node_label(ms->oc, surv, keep_label, sizeof keep_label);
    odin3_kit_node_label(ms->oc, drop, drop_label, sizeof drop_label);
    odin3_prov_id drop_prov = odin3_node_prov(ms->m, drop);
    odin3_kit_conn conn = {drop, port, ms->srcs.data, (uint32_t)ms->srcs.len};
    st = odin3_kit_connect(ms->oc, &conn);
    if (!odin3_node_live(ms->m, drop)) { /* deleted, even if a later merge step failed */
        log_merge(ms, drop_label, keep_label);
        ms->into[drop.v] = surv.v;
        ms->dprov[drop.v] = drop_prov;
        ms->removed++;
    }
    return st == ODIN3_OK && is_ff ? settle_inits(ms) : st;
}

/* --- the sweeps ---------------------------------------------------------------------------------
 */

/* Looks node up; on a hit merges the pair (the later node survives only when it alone has keep). */
static odin3_status visit(merge_state *ms, odin3_node_id node, uint32_t *merged) {
    const odin3_celltype_def *def = NULL;
    uint8_t cls = candidate(ms, node, &def);
    if (cls == CLS_SKIP) {
        return ms->oom ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    }
    if (!build_key(ms, node, def, cls)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint64_t hash = hash_key(ms->key.data, ms->key.len);
    uint32_t at = find_entry(ms, hash);
    if (at == 0) {
        return insert_entry(ms, hash, node);
    }
    entry *ent = odin3_vec_at(&ms->ents, at - 1U);
    odin3_node_id other = {ent->node};
    odin3_objref cur_ref = {ODIN3_OBJ_NODE, node.v};
    odin3_objref other_ref = {ODIN3_OBJ_NODE, other.v};
    bool cur_keep = odin3_kit_keep(ms->oc, cur_ref);
    if (cur_keep && odin3_kit_keep(ms->oc, other_ref)) {
        return ODIN3_OK;
    }
    if (cur_keep) {
        ent->node = node.v; /* the entry now stands for the later node */
    }
    (*merged)++;
    return cur_keep ? merge_pair(ms, node, other, (cls & CLS_FF) != 0)
                    : merge_pair(ms, other, node, (cls & CLS_FF) != 0);
}

static odin3_status sweep(merge_state *ms, uint32_t *merged) {
    *merged = 0;
    odin3_status st = reset_table(ms);
    for (uint32_t id = 1; st == ODIN3_OK && id < ms->node_end; id++) {
        st = visit(ms, (odin3_node_id){id}, merged);
    }
    return st;
}

/* --- provenance: one rebuild per class -----------------------------------------------------------
 */

/* The survivor of each dropped node (following the chain, compressing it) into into[]. */
static void resolve_roots(merge_state *ms) {
    for (uint32_t id = 1; id < ms->node_end; id++) {
        uint32_t root = id;
        while (ms->into[root] != 0) {
            root = ms->into[root];
        }
        uint32_t at = id;
        while (ms->into[at] != 0 && ms->into[at] != root) {
            uint32_t next = ms->into[at];
            ms->into[at] = root;
            at = next;
        }
    }
}

/* The classes: flat[start[r] .. start[r + 1]) are the nodes dropped into survivor r, in ID order.
 */
typedef struct classes {
    uint32_t *start, *flat;
} classes;

static odin3_status group_drops(const merge_state *ms, classes *cl) {
    cl->start = odin3_util_calloc(sizeof *cl->start * ((size_t)ms->node_end + 1U));
    cl->flat = odin3_util_calloc(sizeof *cl->flat * ((size_t)ms->removed + 1U));
    if (cl->start == NULL || cl->flat == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 1; id < ms->node_end; id++) {
        cl->start[ms->into[id]]++;
    }
    cl->start[0] = 0; /* nodes that were not dropped */
    uint32_t sum = 0;
    for (uint32_t cls = 0; cls <= ms->node_end; cls++) {
        uint32_t count = cl->start[cls];
        cl->start[cls] = sum;
        sum += count;
    }
    for (uint32_t id = 1; id < ms->node_end; id++) {
        if (ms->into[id] != 0) {
            cl->flat[cl->start[ms->into[id]]++] = id;
        }
    }
    /* start[cls] now holds the end of class cls: shift back to the starts */
    for (uint32_t cls = ms->node_end; cls > 0; cls--) {
        cl->start[cls] = cl->start[cls - 1];
    }
    cl->start[0] = 0;
    return ODIN3_OK;
}

/* The record for survivor: DERIVED of [its record, the drops' records], 0 when none has one. */
static odin3_status class_prov(merge_state *ms, odin3_node_id surv, const classes *cl,
                               odin3_prov_id *out) {
    odin3_vec provs;
    odin3_vec_init(&provs, sizeof(odin3_prov_id));
    odin3_prov_id own = odin3_node_prov(ms->m, surv);
    odin3_status st = ODIN3_OK;
    odin3_prov_id *slot = own.v != 0 ? odin3_vec_push(&provs) : NULL;
    if (own.v != 0 && slot == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    } else if (slot != NULL) {
        *slot = own;
    }
    for (uint32_t at = cl->start[surv.v]; st == ODIN3_OK && at < cl->start[surv.v + 1U]; at++) {
        odin3_prov_id rec = ms->dprov[cl->flat[at]];
        slot = rec.v != 0 ? odin3_vec_push(&provs) : NULL;
        if (rec.v != 0 && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else if (slot != NULL) {
            *slot = rec;
        }
    }
    *out = (odin3_prov_id){0};
    if (st == ODIN3_OK && provs.len > 0) {
        st = odin3_prov_derive(ms->oc->pass, (odin3_prov_list){provs.data, (uint32_t)provs.len},
                               out);
    }
    odin3_vec_free(&provs);
    return st;
}

/* One rebuild of survivor: the same type, parameters and nets, the class's record. */
static odin3_status close_class(merge_state *ms, odin3_node_id surv, const classes *cl) {
    odin3_kit_begin_rewrite(ms->oc);
    odin3_kit_rebuild req;
    memset(&req, 0, sizeof req);
    req.old_node = surv;
    req.cell.type = odin3_node_type(ms->m, surv);
    odin3_status st = class_prov(ms, surv, cl, &req.cell.prov);
    const odin3_celltype_def *def = odin3_celltype_get(ms->oc->design, req.cell.type);
    odin3_value params[MAX_PARAMS];
    if (st == ODIN3_OK && def->n_params > 0 && def->n_params <= MAX_PARAMS) {
        for (uint32_t i = 0; i < def->n_params; i++) {
            params[i] = *odin3_node_param(ms->m, surv, i);
        }
        req.cell.params = params;
        req.cell.n_params = def->n_params;
    } else if (st == ODIN3_OK && def->n_params > MAX_PARAMS) {
        return ODIN3_ERR_INVALID_ARG; /* classify never lets such a type in; keep it explicit */
    }
    odin3_node_id fresh = {0};
    return st == ODIN3_OK ? odin3_kit_rebuild_node(ms->oc, &req, &fresh) : st;
}

static odin3_status close_all(merge_state *ms) {
    classes cl = {NULL, NULL};
    resolve_roots(ms);
    odin3_status st = group_drops(ms, &cl);
    for (uint32_t id = 1; st == ODIN3_OK && id < ms->node_end; id++) {
        if (cl.start[id + 1U] > cl.start[id]) {
            st = close_class(ms, (odin3_node_id){id}, &cl);
        }
    }
    odin3_util_free(cl.start);
    odin3_util_free(cl.flat);
    return st;
}

/* --- one module, the pass ------------------------------------------------------------------------
 */

static void state_free(merge_state *ms) {
    odin3_vec_free(&ms->classes);
    odin3_vec_free(&ms->key);
    odin3_vec_free(&ms->ta);
    odin3_vec_free(&ms->tb);
    odin3_vec_free(&ms->idx);
    odin3_vec_free(&ms->idx2);
    odin3_vec_free(&ms->bits);
    odin3_vec_free(&ms->store);
    odin3_vec_free(&ms->ents);
    odin3_vec_free(&ms->srcs);
    odin3_u64map_destroy(ms->map);
    odin3_util_free(ms->into);
    odin3_util_free(ms->dprov);
}

static void state_init(merge_state *ms, odin3_opt_ctx *oc) {
    memset(ms, 0, sizeof *ms);
    ms->oc = oc;
    ms->m = oc->module;
    ms->node_end = odin3_module_node_end(oc->module);
    odin3_vec_init(&ms->classes, sizeof(uint8_t));
    odin3_vec_init(&ms->key, sizeof(uint64_t));
    odin3_vec_init(&ms->ta, sizeof(uint64_t));
    odin3_vec_init(&ms->tb, sizeof(uint64_t));
    odin3_vec_init(&ms->idx, sizeof(uint32_t));
    odin3_vec_init(&ms->idx2, sizeof(uint32_t));
    odin3_vec_init(&ms->bits, sizeof(odin3_bit));
    odin3_vec_init(&ms->store, sizeof(uint64_t));
    odin3_vec_init(&ms->ents, sizeof(entry));
    odin3_vec_init(&ms->srcs, sizeof(odin3_net_id));
    ms->into = odin3_util_calloc(sizeof *ms->into * ((size_t)ms->node_end + 1U));
    ms->dprov = odin3_util_calloc(sizeof *ms->dprov * ((size_t)ms->node_end + 1U));
}

/* Sweeps to a fixed point, then closes the classes. Adds the drops into *removed. */
static odin3_status merge_module(odin3_opt_ctx *oc, uint32_t *removed) {
    merge_state ms;
    state_init(&ms, oc);
    odin3_status st = ms.into == NULL || ms.dprov == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    st = st == ODIN3_OK ? odin3_opt_canon_consts(oc) : st;
    uint32_t merged = 1;
    while (st == ODIN3_OK && merged > 0) {
        st = sweep(&ms, &merged);
    }
    st = st == ODIN3_OK && ms.removed > 0 ? close_all(&ms) : st;
    *removed += ms.removed;
    state_free(&ms);
    return st;
}

odin3_status odin3_opt_merge_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_merge: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t removed = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = merge_module(&oc, &removed);
        odin3_opt_ctx_free(&oc);
    }
    counts->merge_removed += removed;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "opt_merge: removed %" PRIu32 " cells", removed);
    }
    return st;
}
