/*
 * prov_export.c — the labels of a provenance record: class, construct, joins (V1.2.0 3.1, 3.3).
 * The backward walk is iterative; a record is visited once per context chain it is reached with.
 */
#include "backends/common/prov_export.h"

#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { SOURCE_KIND = 0, IMPORTED_KIND = 1, DERIVED_KIND = 2 };

/* A pending record with the context chain it is reached with (chain 0: none). */
typedef struct frame {
    uint32_t rec;
    uint32_t chain;
} frame;

/* A context chain: the parent chain and the context record appended (chain ID n is index n - 1). */
typedef struct chain_node {
    uint32_t parent;
    uint32_t rec;
} chain_node;

/* A construct leaf: its row and the chain it was reached with. */
typedef struct leaf_hit {
    uint32_t row;
    uint32_t chain;
} leaf_hit;

typedef struct walk_state {
    const odin3_design *design;
    odin3_vec stack;         /* frame */
    odin3_vec chains;        /* chain_node */
    odin3_vec leaves;        /* leaf_hit */
    odin3_u64map *seen;      /* (record << 32 | chain) */
    odin3_u64map *chain_ids; /* (parent chain << 32 | record) -> chain ID */
    bool span_only;
} walk_state;

enum { KEY_SHIFT = 32 };

/* One map key from a record and a chain (or a chain and a record). */
static uint64_t pair_key(uint32_t high, uint32_t low) {
    return ((uint64_t)high << KEY_SHIFT) | low;
}

static void walk_free(walk_state *walk) {
    odin3_vec_free(&walk->stack);
    odin3_vec_free(&walk->chains);
    odin3_vec_free(&walk->leaves);
    odin3_u64map_destroy(walk->seen);
    odin3_u64map_destroy(walk->chain_ids);
    walk->seen = NULL;
    walk->chain_ids = NULL;
}

static odin3_status walk_init(walk_state *walk, const odin3_design *design) {
    *walk = (walk_state){.design = design};
    odin3_vec_init(&walk->stack, sizeof(frame));
    odin3_vec_init(&walk->chains, sizeof(chain_node));
    odin3_vec_init(&walk->leaves, sizeof(leaf_hit));
    walk->seen = odin3_u64map_create(0);
    walk->chain_ids = odin3_u64map_create(0);
    if (walk->seen == NULL || walk->chain_ids == NULL) {
        walk_free(walk);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* The chain `parent` extended by context record rec: an existing ID, else a new one. */
static odin3_status chain_extend(walk_state *walk, uint32_t parent, uint32_t rec, uint32_t *out) {
    uint64_t have = 0;
    if (odin3_u64map_get(walk->chain_ids, pair_key(parent, rec), &have)) {
        *out = (uint32_t)have;
        return ODIN3_OK;
    }
    chain_node *node = odin3_vec_push(&walk->chains);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *node = (chain_node){parent, rec};
    uint32_t id = (uint32_t)walk->chains.len;
    if (odin3_u64map_put(walk->chain_ids, (odin3_kv){pair_key(parent, rec), id}) != ODIN3_OK) {
        odin3_vec_pop(&walk->chains);
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = id;
    return ODIN3_OK;
}

static odin3_status push_frame(walk_state *walk, uint32_t rec, uint32_t chain) {
    frame *slot = odin3_vec_push(&walk->stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (frame){rec, chain};
    return ODIN3_OK;
}

/* A SOURCE or IMPORTED record reached: a construct leaf, or a span-only one. */
static odin3_status take_leaf(walk_state *walk, frame fr) {
    uint32_t row = odin3_construct_of_record(walk->design, (odin3_prov_id){fr.rec});
    if (row == 0) {
        walk->span_only = true;
        return ODIN3_OK;
    }
    leaf_hit *hit = odin3_vec_push(&walk->leaves);
    if (hit == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *hit = (leaf_hit){row, fr.chain};
    return ODIN3_OK;
}

/* How many trailing parents of rec are context parents (the declared tail, clamped). */
static uint32_t tail_of(const odin3_design *design, odin3_prov_id id, uint32_t count) {
    odin3_opclass cls;
    if (!odin3_prov_class_of(design, id, &cls) || cls.dir != ODIN3_DIR_CONTEXT) {
        return 0;
    }
    return cls.context_tail < count ? cls.context_tail : count;
}

/* A DERIVED record reached with chain: its tail extends the chain, its data parents are pushed. */
static odin3_status expand_derived(walk_state *walk, uint32_t rec_id, const odin3_prov_record *rec,
                                   uint32_t chain) {
    uint32_t count = rec->parents.count;
    uint32_t data = count - tail_of(walk->design, (odin3_prov_id){rec_id}, count);
    uint32_t inner = chain;
    for (uint32_t k = data; k < count; k++) {
        odin3_status st = chain_extend(walk, inner, rec->parents.ids[k].v, &inner);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    for (uint32_t k = data; k > 0; k--) {
        odin3_status st = push_frame(walk, rec->parents.ids[k - 1U].v, inner);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status visit_frame(walk_state *walk, frame fr) {
    const odin3_prov_record *rec = odin3_prov_get(walk->design, (odin3_prov_id){fr.rec});
    if (rec->kind == ODIN3_PROV_DERIVED) {
        return expand_derived(walk, fr.rec, rec, fr.chain);
    }
    return take_leaf(walk, fr);
}

/* Walks the data parents of record depth-first, filling the leaves and the span-only flag. */
static odin3_status walk_record(walk_state *walk, uint32_t record) {
    odin3_status st = push_frame(walk, record, 0);
    while (st == ODIN3_OK && walk->stack.len > 0) {
        frame fr = *(const frame *)odin3_vec_cat(&walk->stack, walk->stack.len - 1U);
        odin3_vec_pop(&walk->stack);
        uint64_t unused = 0;
        if (odin3_u64map_get(walk->seen, pair_key(fr.rec, fr.chain), &unused)) {
            continue;
        }
        st = odin3_u64map_put(walk->seen, (odin3_kv){pair_key(fr.rec, fr.chain), 1});
        if (st == ODIN3_OK) {
            st = visit_frame(walk, fr);
        }
    }
    return st;
}

/* --- anchors and the reduced set ------------------------------------------------------------- */

/* The construct itself when its level is stmt or coarser, else its nearest such ancestor. */
static uint32_t anchor_of(const odin3_design *design, uint32_t row) {
    for (;;) {
        const odin3_construct_row *cur = odin3_construct_get(design, row);
        if (cur->level <= (uint8_t)ODIN3_LEVEL_STMT || cur->parent == 0) {
            return row;
        }
        row = cur->parent;
    }
}

/* Puts every ancestor of row that is in members into covered. */
static odin3_status cover_ancestors(const odin3_design *design, uint32_t row,
                                    const odin3_u64map *members, odin3_u64map *covered) {
    uint64_t unused = 0;
    for (uint32_t up = odin3_construct_get(design, row)->parent; up != 0;
         up = odin3_construct_get(design, up)->parent) {
        if (odin3_u64map_get(members, up, &unused) &&
            odin3_u64map_put(covered, (odin3_kv){up, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

static int cmp_u32(uint32_t lhs, uint32_t rhs) {
    if (lhs == rhs) {
        return 0;
    }
    return lhs < rhs ? -1 : 1;
}

/* Compares two elements of a vec being sorted. */
typedef int (*elem_cmp)(const void *ctx, const void *lhs, const void *rhs);

/* How to sort a vec: the element size, the comparison and its context. */
typedef struct elem_ctx {
    size_t size;
    elem_cmp cmp;
    const void *ctx;
} elem_ctx;

/* Merges src[lo, mid) and src[mid, hi) into dst[lo, hi): stable, elements of size elem bytes. */
static void merge_runs(const unsigned char *src, unsigned char *dst, const size_t bounds[3],
                       const elem_ctx *ec) {
    size_t left = bounds[0];
    size_t right = bounds[1];
    for (size_t out = bounds[0]; out < bounds[2]; out++) {
        bool take_left = right >= bounds[2] ||
                         (left < bounds[1] &&
                          ec->cmp(ec->ctx, src + (left * ec->size), src + (right * ec->size)) <= 0);
        size_t from = take_left ? left++ : right++;
        memcpy(dst + (out * ec->size), src + (from * ec->size), ec->size);
    }
}

/* Sorts vec ascending by ec->cmp (bottom-up merge, stable). NO_MEMORY leaves vec as it was. */
static odin3_status merge_sort(odin3_vec *vec, const elem_ctx *ec) {
    size_t len = vec->len;
    odin3_vec scratch;
    odin3_vec_init(&scratch, ec->size);
    if (odin3_vec_reserve(&scratch, len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    unsigned char *src = vec->data;
    unsigned char *dst = scratch.data;
    for (size_t width = 1; width < len; width *= 2) {
        for (size_t lo = 0; lo < len; lo += 2 * width) {
            size_t mid = lo + width < len ? lo + width : len;
            size_t hi = lo + (2 * width) < len ? lo + (2 * width) : len;
            merge_runs(src, dst, (const size_t[3]){lo, mid, hi}, ec);
        }
        unsigned char *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != vec->data) {
        memcpy(vec->data, src, len * ec->size);
    }
    odin3_vec_free(&scratch);
    return ODIN3_OK;
}

static int cmp_u32_elem(const void *ctx, const void *lhs, const void *rhs) {
    (void)ctx;
    return cmp_u32(*(const uint32_t *)lhs, *(const uint32_t *)rhs);
}

static odin3_status sort_u32(odin3_vec *vec) {
    const elem_ctx ec = {sizeof(uint32_t), cmp_u32_elem, NULL};
    return merge_sort(vec, &ec);
}

/* Removes from rows (sorted afterwards ascending) every anchor that is an ancestor of another. */
static odin3_status reduce_rows(const odin3_design *design, odin3_vec *rows,
                                const odin3_u64map *members) {
    odin3_u64map *covered = odin3_u64map_create(0);
    if (covered == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rows->len; i++) {
        st = cover_ancestors(design, *(const uint32_t *)odin3_vec_cat(rows, i), members, covered);
    }
    size_t keep = 0;
    uint32_t *data = rows->data;
    uint64_t unused = 0;
    for (size_t i = 0; st == ODIN3_OK && i < rows->len; i++) {
        if (!odin3_u64map_get(covered, data[i], &unused)) {
            data[keep++] = data[i];
        }
    }
    if (st == ODIN3_OK) {
        rows->len = keep;
        st = sort_u32(rows);
    }
    odin3_u64map_destroy(covered);
    return st;
}

/* True when an anchor is reached with two different chains. */
static odin3_status find_instance_join(const walk_state *walk, bool *found) {
    odin3_u64map *first = odin3_u64map_create(0);
    if (first == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < walk->leaves.len && !*found; i++) {
        const leaf_hit *hit = odin3_vec_cat(&walk->leaves, i);
        uint32_t anchor = anchor_of(walk->design, hit->row);
        uint64_t have = 0;
        if (!odin3_u64map_get(first, anchor, &have)) {
            st = odin3_u64map_put(first, (odin3_kv){anchor, (uint64_t)hit->chain + 1U});
        } else {
            *found = have != (uint64_t)hit->chain + 1U;
        }
    }
    odin3_u64map_destroy(first);
    return st;
}

/* --- the key order of J (3.3, "one cap rule") ------------------------------------------------ */

/* A member of J with its chain from the module down: path[off .. off + len). */
typedef struct member {
    uint32_t row;
    uint32_t off;
    uint32_t len;
} member;

typedef struct order_ctx {
    const odin3_design *design;
    const odin3_vec *path; /* uint32_t */
} order_ctx;

static int cmp_str(const odin3_design *design, uint32_t lhs, uint32_t rhs) {
    const odin3_strtab *tab = odin3_design_strtab(design);
    const char *lhs_text = odin3_strtab_get(tab, lhs);
    const char *rhs_text = odin3_strtab_get(tab, rhs);
    return strcmp(lhs_text != NULL ? lhs_text : "", rhs_text != NULL ? rhs_text : "");
}

/* The order of two rows by (file, span, kind, name, part) (3.3). */
static int cmp_row(const odin3_design *design, uint32_t lhs, uint32_t rhs) {
    const odin3_construct_row *left = odin3_construct_get(design, lhs);
    const odin3_construct_row *right = odin3_construct_get(design, rhs);
    int order = cmp_str(design, left->file, right->file);
    if (order == 0) {
        order = cmp_u32(left->line, right->line);
    }
    if (order == 0) {
        order = cmp_u32(left->col, right->col);
    }
    if (order == 0) {
        order = cmp_u32(left->end_line, right->end_line);
    }
    if (order == 0) {
        order = cmp_u32(left->end_col, right->end_col);
    }
    if (order == 0) {
        order = cmp_u32(left->kind, right->kind);
    }
    if (order == 0) {
        order = cmp_str(design, left->name, right->name);
    }
    return order != 0 ? order : cmp_u32(left->part, right->part);
}

/* Chains compared from the module down; a chain that is a prefix of another comes first. */
static int cmp_member(const order_ctx *ctx, const member *lhs, const member *rhs) {
    const uint32_t *left = (const uint32_t *)ctx->path->data + lhs->off;
    const uint32_t *right = (const uint32_t *)ctx->path->data + rhs->off;
    uint32_t common = lhs->len < rhs->len ? lhs->len : rhs->len;
    for (uint32_t i = 0; i < common; i++) {
        int order = cmp_row(ctx->design, left[i], right[i]);
        if (order != 0) {
            return order;
        }
    }
    return cmp_u32(lhs->len, rhs->len);
}

/* Appends the chain of row, module first, to path; sets the member. */
static odin3_status build_member(const odin3_design *design, uint32_t row, odin3_vec *path,
                                 member *out) {
    uint32_t depth = 0;
    for (uint32_t up = row; up != 0; up = odin3_construct_get(design, up)->parent) {
        depth++;
    }
    size_t off = path->len;
    if (odin3_vec_reserve(path, off + depth) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < depth; i++) {
        (void)odin3_vec_push(path); /* reserved */
    }
    uint32_t *slots = path->data;
    uint32_t at = depth;
    for (uint32_t up = row; up != 0; up = odin3_construct_get(design, up)->parent) {
        slots[off + (--at)] = up;
    }
    *out = (member){row, (uint32_t)off, depth};
    return ODIN3_OK;
}

static int cmp_member_elem(const void *ctx, const void *lhs, const void *rhs) {
    return cmp_member(ctx, lhs, rhs);
}

/* Replaces rows (any order) by the same rows in key order, rows with an identical chain once. */
static odin3_status order_rows(const odin3_design *design, odin3_vec *rows) {
    odin3_vec path;
    odin3_vec members;
    odin3_vec_init(&path, sizeof(uint32_t));
    odin3_vec_init(&members, sizeof(member));
    odin3_status st = odin3_vec_reserve(&members, rows->len);
    for (size_t i = 0; st == ODIN3_OK && i < rows->len; i++) {
        member *slot = odin3_vec_push(&members); /* reserved */
        st = build_member(design, *(const uint32_t *)odin3_vec_cat(rows, i), &path, slot);
    }
    order_ctx ctx = {design, &path};
    const elem_ctx ec = {sizeof(member), cmp_member_elem, &ctx};
    if (st == ODIN3_OK) {
        st = merge_sort(&members, &ec);
    }
    if (st == ODIN3_OK) {
        size_t keep = 0;
        uint32_t *out = rows->data;
        const member *list = members.data;
        for (size_t i = 0; i < members.len; i++) {
            if (keep == 0 || cmp_member(&ctx, &list[i], &list[i - 1U]) != 0) {
                out[keep++] = list[i].row;
            }
        }
        rows->len = keep;
    }
    odin3_vec_free(&members);
    odin3_vec_free(&path);
    return st;
}

/* --- the per-design cache of anchor sets ------------------------------------------------------ */

/*
 * For each record, the distinct anchors (3.3: the construct of a leaf, lifted to stmt or coarser)
 * of every SOURCE or IMPORTED leaf behind its data parents, in ascending row order, shared by a
 * record with one data parent. That set does not depend on the context chains a walk meets, so a
 * record's set is the union of its data parents' sets and one pass over the record graph fills
 * the sets of the whole sub-graph. Only the join of instances needs the chains; it is found by a
 * walk, once per coarsen record whose sub-graph holds a context record.
 */
enum { E_SPAN_ONLY = 1, E_HAS_CTX = 2, E_IJ_KNOWN = 4, E_IJ = 8 };

typedef struct anchor_entry {
    uint32_t off;
    uint32_t len;
    uint32_t flags;
} anchor_entry;

typedef struct label_cache {
    odin3_u64map *index; /* record -> entry number + 1 */
    odin3_vec entries;   /* anchor_entry */
    odin3_vec pool;      /* uint32_t anchors */
} label_cache;

static void cache_destroy(void *data) {
    label_cache *cache = data;
    odin3_u64map_destroy(cache->index);
    odin3_vec_free(&cache->entries);
    odin3_vec_free(&cache->pool);
    odin3_util_free(cache);
}

static void *cache_create(void) {
    label_cache *cache = odin3_util_calloc(sizeof *cache);
    if (cache == NULL) {
        return NULL;
    }
    odin3_vec_init(&cache->entries, sizeof(anchor_entry));
    odin3_vec_init(&cache->pool, sizeof(uint32_t));
    cache->index = odin3_u64map_create(0);
    if (cache->index == NULL) {
        cache_destroy(cache);
        return NULL;
    }
    return cache;
}

static const anchor_entry *entry_of(const label_cache *cache, uint32_t record) {
    uint64_t at = 0;
    return odin3_u64map_get(cache->index, record, &at) ? odin3_vec_cat(&cache->entries, at - 1U)
                                                       : NULL;
}

static odin3_status entry_add(label_cache *cache, uint32_t record, anchor_entry entry) {
    anchor_entry *slot = odin3_vec_push(&cache->entries);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = entry;
    if (odin3_u64map_put(cache->index, (odin3_kv){record, cache->entries.len}) != ODIN3_OK) {
        odin3_vec_pop(&cache->entries);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

static const uint32_t *entry_anchors(const label_cache *cache, const anchor_entry *entry) {
    return (const uint32_t *)cache->pool.data + entry->off;
}

/* The entry of a SOURCE or IMPORTED record: its one anchor, or none and span-only. */
static odin3_status leaf_entry(const odin3_design *design, label_cache *cache, uint32_t record) {
    uint32_t row = odin3_construct_of_record(design, (odin3_prov_id){record});
    anchor_entry entry = {(uint32_t)cache->pool.len, 0, E_SPAN_ONLY};
    if (row != 0) {
        uint32_t *slot = odin3_vec_push(&cache->pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = anchor_of(design, row);
        entry = (anchor_entry){entry.off, 1, 0};
    }
    return entry_add(cache, record, entry);
}

/* True when every data parent of rec has an entry; else pushes the ones that have none. */
static odin3_status parents_ready(const label_cache *cache, odin3_vec *todo,
                                  const odin3_prov_record *rec, uint32_t data, bool *ready) {
    *ready = true;
    for (uint32_t k = data; k > 0; k--) {
        uint32_t parent = rec->parents.ids[k - 1U].v;
        if (entry_of(cache, parent) != NULL) {
            continue;
        }
        uint32_t *slot = odin3_vec_push(todo);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = parent;
        *ready = false;
    }
    return ODIN3_OK;
}

/* The union of the anchors of the data parents into the pool; entry gets its range. */
static odin3_status union_entry(label_cache *cache, const odin3_prov_record *rec, uint32_t data,
                                anchor_entry *entry) {
    odin3_u64map *seen = odin3_u64map_create(0);
    odin3_vec merged;
    odin3_vec_init(&merged, sizeof(uint32_t));
    odin3_status st = seen != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t k = 0; st == ODIN3_OK && k < data; k++) {
        const anchor_entry *part = entry_of(cache, rec->parents.ids[k].v);
        const uint32_t *list = entry_anchors(cache, part);
        entry->flags |= part->flags & (E_SPAN_ONLY | E_HAS_CTX);
        for (uint32_t i = 0; st == ODIN3_OK && i < part->len; i++) {
            uint64_t unused = 0;
            uint32_t *slot =
                odin3_u64map_get(seen, list[i], &unused) ? NULL : odin3_vec_push(&merged);
            if (slot != NULL) {
                *slot = list[i];
                st = odin3_u64map_put(seen, (odin3_kv){list[i], 1});
            }
        }
    }
    if (st == ODIN3_OK) {
        st = sort_u32(&merged);
    }
    if (st == ODIN3_OK) {
        st = odin3_vec_reserve(&cache->pool, cache->pool.len + merged.len);
    }
    if (st == ODIN3_OK) {
        entry->off = (uint32_t)cache->pool.len;
        entry->len = (uint32_t)merged.len;
        for (size_t i = 0; i < merged.len; i++) {
            *(uint32_t *)odin3_vec_push(&cache->pool) =
                *(const uint32_t *)odin3_vec_cat(&merged, i);
        }
    }
    odin3_u64map_destroy(seen);
    odin3_vec_free(&merged);
    return st;
}

/* The entry of a DERIVED record whose data parents all have one. */
static odin3_status derived_entry(const odin3_design *design, label_cache *cache, uint32_t record,
                                  const odin3_prov_record *rec) {
    uint32_t count = rec->parents.count;
    uint32_t data = count - tail_of(design, (odin3_prov_id){record}, count);
    anchor_entry entry = {0, 0, data < count ? (uint32_t)E_HAS_CTX : 0U};
    if (data == 1) { /* one data parent: its anchors, shared */
        const anchor_entry *only = entry_of(cache, rec->parents.ids[0].v);
        entry = (anchor_entry){only->off, only->len, only->flags & (E_SPAN_ONLY | E_HAS_CTX)};
        entry.flags |= data < count ? (uint32_t)E_HAS_CTX : 0U;
    } else {
        odin3_status st = union_entry(cache, rec, data, &entry);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return entry_add(cache, record, entry);
}

/* Fills the entries of record and of every record behind it (iterative: the graph may be deep). */
static odin3_status cache_fill(const odin3_design *design, label_cache *cache, uint32_t record) {
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(uint32_t));
    uint32_t *first = odin3_vec_push(&todo);
    odin3_status st = first != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (first != NULL) {
        *first = record;
    }
    while (st == ODIN3_OK && todo.len > 0) {
        uint32_t top = *(const uint32_t *)odin3_vec_cat(&todo, todo.len - 1U);
        const odin3_prov_record *rec = odin3_prov_get(design, (odin3_prov_id){top});
        bool ready = true;
        if (entry_of(cache, top) != NULL) {
            odin3_vec_pop(&todo);
            continue;
        }
        if (rec->kind == ODIN3_PROV_DERIVED) {
            uint32_t count = rec->parents.count;
            uint32_t data = count - tail_of(design, (odin3_prov_id){top}, count);
            st = parents_ready(cache, &todo, rec, data, &ready);
        }
        if (st == ODIN3_OK && ready) {
            odin3_vec_pop(&todo);
            st = rec->kind == ODIN3_PROV_DERIVED ? derived_entry(design, cache, top, rec)
                                                 : leaf_entry(design, cache, top);
        }
    }
    odin3_vec_free(&todo);
    return st;
}

/* The cache of design, with the entry of record in it. */
static odin3_status cache_entry(const odin3_design *design, uint32_t record, label_cache **cache,
                                const anchor_entry **entry) {
    *cache = odin3_construct_derived(design, cache_create, cache_destroy);
    odin3_status st = *cache != NULL ? cache_fill(design, *cache, record) : ODIN3_ERR_NO_MEMORY;
    *entry = st == ODIN3_OK ? entry_of(*cache, record) : NULL;
    return st;
}

/* rows := the reduced anchors of entry (3.3) in the key order, a chain once. */
static odin3_status entry_set(const odin3_design *design, const label_cache *cache,
                              const anchor_entry *entry, odin3_vec *rows) {
    odin3_u64map *members = odin3_u64map_create(0);
    odin3_status st = members != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    const uint32_t *list = entry_anchors(cache, entry);
    for (uint32_t i = 0; st == ODIN3_OK && i < entry->len; i++) {
        uint32_t *slot = odin3_vec_push(rows);
        st = slot != NULL ? odin3_u64map_put(members, (odin3_kv){list[i], 1}) : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = list[i];
        }
    }
    if (st == ODIN3_OK) {
        st = reduce_rows(design, rows, members);
    }
    if (st == ODIN3_OK) {
        st = order_rows(design, rows);
    }
    odin3_u64map_destroy(members);
    return st;
}

/* True when two rows have the same chain from the module down (the same construct, 3.3). */
static bool same_chain(const odin3_design *design, uint32_t lhs, uint32_t rhs) {
    while (lhs != 0 && rhs != 0) {
        if (cmp_row(design, lhs, rhs) != 0) {
            return false;
        }
        lhs = odin3_construct_get(design, lhs)->parent;
        rhs = odin3_construct_get(design, rhs)->parent;
    }
    return lhs == 0 && rhs == 0;
}

/* True when the ordered sets hold the same constructs, one by one. */
static bool sets_equal(const odin3_design *design, const odin3_vec *lhs, const odin3_vec *rhs) {
    if (lhs->len != rhs->len) {
        return false;
    }
    for (size_t i = 0; i < lhs->len; i++) {
        if (!same_chain(design, *(const uint32_t *)odin3_vec_cat(lhs, i),
                        *(const uint32_t *)odin3_vec_cat(rhs, i))) {
            return false;
        }
    }
    return true;
}

/* True when a single data parent of rec has the same set J as the ordered set rows. */
static odin3_status parent_has_same_set(const odin3_design *design, const label_cache *cache,
                                        const odin3_prov_record *rec, uint32_t data,
                                        const odin3_vec *rows, bool *same) {
    for (uint32_t k = 0; k < data && !*same; k++) {
        odin3_vec theirs;
        odin3_vec_init(&theirs, sizeof(uint32_t));
        odin3_status st = entry_set(design, cache, entry_of(cache, rec->parents.ids[k].v), &theirs);
        *same = st == ODIN3_OK && sets_equal(design, &theirs, rows);
        odin3_vec_free(&theirs);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Whether an anchor of the record's walk is reached with two chains; cached in the entry. */
static odin3_status instance_join_of(const odin3_design *design, label_cache *cache,
                                     uint32_t record, bool *found) {
    uint64_t at = 0;
    (void)odin3_u64map_get(cache->index, record, &at); /* filled by cache_entry */
    anchor_entry *entry = odin3_vec_at(&cache->entries, at - 1U);
    *found = (entry->flags & E_IJ) != 0;
    if ((entry->flags & (E_IJ_KNOWN | E_HAS_CTX)) != E_HAS_CTX) {
        return ODIN3_OK; /* known, or no context record behind it: one chain for everything */
    }
    walk_state walk;
    odin3_status st = walk_init(&walk, design);
    if (st == ODIN3_OK) {
        st = walk_record(&walk, record);
    }
    if (st == ODIN3_OK) {
        st = find_instance_join(&walk, found);
    }
    walk_free(&walk);
    if (st == ODIN3_OK) { /* the walk adds no entry: the pointer still holds */
        entry->flags |= E_IJ_KNOWN | (*found ? (uint32_t)E_IJ : 0U);
    }
    return st;
}

/* --- the label ------------------------------------------------------------------------------- */

void odin3_prov_label_init(odin3_prov_label *label) {
    *label = (odin3_prov_label){0};
    odin3_vec_init(&label->joined, sizeof(uint32_t));
}

void odin3_prov_label_free(odin3_prov_label *label) {
    odin3_vec_free(&label->joined);
}

/* Record, run, operation, parent count, hier and the class fields. */
static void fill_class(const odin3_design *design, uint32_t record, const odin3_prov_record *rec,
                       odin3_prov_info *info) {
    *info = (odin3_prov_info){.record = record,
                              .kind = (uint32_t)rec->kind,
                              .run = rec->run.v,
                              .op = rec->op,
                              .n_parents = rec->parents.count,
                              .hier = odin3_strtab_get(odin3_design_strtab(design), rec->hier)};
    odin3_opclass cls;
    if (rec->kind != ODIN3_PROV_DERIVED ||
        !odin3_prov_class_of(design, (odin3_prov_id){record}, &cls)) {
        return;
    }
    info->dir = cls.dir;
    info->from_level = cls.from_level;
    info->to_level = cls.to_level;
    info->n_context = tail_of(design, (odin3_prov_id){record}, rec->parents.count);
    info->flags = cls.bulk ? ODIN3_PROV_F_LOSSY_BULK : 0U;
}

/* The join labels of a coarsen record from its set J; fills label->joined. */
static odin3_status label_join(const odin3_design *design, label_cache *cache,
                               const odin3_prov_record *rec, const anchor_entry *entry,
                               odin3_prov_label *label) {
    odin3_prov_info *info = &label->info;
    odin3_vec rows;
    odin3_vec_init(&rows, sizeof(uint32_t));
    bool same = false;
    bool instance = false;
    uint32_t data = rec->parents.count - info->n_context;
    odin3_status st = entry_set(design, cache, entry, &rows);
    if (st == ODIN3_OK) {
        st = parent_has_same_set(design, cache, rec, data, &rows, &same);
    }
    if (st == ODIN3_OK) {
        st = instance_join_of(design, cache, info->record, &instance);
    }
    if (st != ODIN3_OK) {
        odin3_vec_free(&rows);
        return st;
    }
    info->n_joined = (uint32_t)rows.len;
    if (rows.len >= 2 && !same) {
        info->flags |= ODIN3_PROV_F_JOIN;
    }
    if (rows.len > ODIN3_PROV_JOIN_CAP) {
        info->flags |= ODIN3_PROV_F_LOSSY_CAP;
    }
    if (instance) {
        info->flags |= ODIN3_PROV_F_JOIN_INSTANCE;
    }
    odin3_vec_free(&label->joined);
    label->joined = rows;
    return ODIN3_OK;
}

odin3_status odin3_prov_label_compute(const odin3_design *design, uint32_t record,
                                      odin3_prov_label *label) {
    const odin3_prov_record *rec =
        design != NULL && label != NULL ? odin3_prov_get(design, (odin3_prov_id){record}) : NULL;
    if (rec == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_label_compute: invalid argument (record %u)",
                  record);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_label fresh;
    odin3_prov_label_init(&fresh);
    fill_class(design, record, rec, &fresh.info);
    fresh.info.construct = odin3_construct_of_record(design, (odin3_prov_id){record});
    label_cache *cache = NULL;
    const anchor_entry *entry = NULL;
    odin3_status st = cache_entry(design, record, &cache, &entry);
    if (st == ODIN3_OK && (entry->flags & E_SPAN_ONLY) != 0) {
        fresh.info.flags |= ODIN3_PROV_F_SPAN_ONLY;
    }
    if (st == ODIN3_OK && fresh.info.dir == ODIN3_DIR_COARSEN) {
        st = label_join(design, cache, rec, entry, &fresh);
    }
    if (st != ODIN3_OK) {
        odin3_prov_label_free(&fresh);
        return st;
    }
    odin3_prov_label_free(label);
    *label = fresh;
    return ODIN3_OK;
}
