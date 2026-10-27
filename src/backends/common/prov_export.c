/*
 * prov_export.c — the labels of a provenance record: class, construct, joins (V1.2.0 3.1, 3.3).
 * The backward walk is iterative; a record is visited once per context chain it is reached with.
 */
#include "backends/common/prov_export.h"

#include "ast/kinds.h"
#include "backends/common/hdl_loc.h"
#include "backends/common/name_map.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { KEY_SHIFT = 32 };

/* One map key from a record and a chain (or a chain and a record). */
static uint64_t pair_key(uint32_t high, uint32_t low) {
    return ((uint64_t)high << KEY_SHIFT) | low;
}

/* How many trailing parents of rec are context parents (the declared tail, clamped). */
static uint32_t tail_of(const odin3_design *design, odin3_prov_id id, uint32_t count) {
    odin3_opclass cls;
    if (!odin3_prov_class_of(design, id, &cls) || cls.dir != ODIN3_DIR_CONTEXT) {
        return 0;
    }
    return cls.context_tail < count ? cls.context_tail : count;
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
 * the sets of the whole sub-graph. The join of instances needs the context chains: a record that
 * has a context record in its sub-graph also gets the set of (anchor, chain) pairs behind it,
 * built from its data parents' sets in the same bottom-up way and kept for the design, so no
 * record's sub-graph is walked twice (a chain is a hash-consed list of context records).
 */
enum { E_SPAN_ONLY = 1, E_HAS_CTX = 2 };

typedef struct anchor_entry {
    uint32_t off;
    uint32_t len;
    uint32_t flags;
} anchor_entry;

/* The (anchor, chain) pairs behind a record with a context record in its sub-graph. multi: some
 * anchor has two chains; the pairs are then not kept (every record above has it too). */
typedef struct ij_entry {
    uint32_t off;
    uint32_t len;
    bool multi;
} ij_entry;

/* Not thread safe: the cache of a design is filled lazily and without a lock; labels of one design
 * must be computed from one thread at a time (the writers are single threaded). `busy` catches an
 * overlapping use of an existing cache and fails it (logged) instead of racing. */
typedef struct label_cache {
    odin3_u64map *index;     /* record -> entry number + 1 */
    odin3_vec entries;       /* anchor_entry */
    odin3_vec pool;          /* uint32_t anchors */
    odin3_u64map *ij_index;  /* record -> ij entry number + 1 */
    odin3_vec ij_entries;    /* ij_entry */
    odin3_vec ij_pool;       /* uint64_t: anchor << 32 | chain */
    odin3_u64map *chain_ids; /* (first record << 32 | rest chain) -> chain ID (> 0; 0: empty) */
    uint32_t chain_count;
    atomic_flag busy;
} label_cache;

static void cache_destroy(void *data) {
    label_cache *cache = data;
    odin3_u64map_destroy(cache->index);
    odin3_u64map_destroy(cache->ij_index);
    odin3_u64map_destroy(cache->chain_ids);
    odin3_vec_free(&cache->entries);
    odin3_vec_free(&cache->pool);
    odin3_vec_free(&cache->ij_entries);
    odin3_vec_free(&cache->ij_pool);
    odin3_util_free(cache);
}

static void *cache_create(void) {
    label_cache *cache = odin3_util_calloc(sizeof *cache);
    if (cache == NULL) {
        return NULL;
    }
    odin3_vec_init(&cache->entries, sizeof(anchor_entry));
    odin3_vec_init(&cache->pool, sizeof(uint32_t));
    atomic_flag_clear(&cache->busy);
    odin3_vec_init(&cache->ij_entries, sizeof(ij_entry));
    odin3_vec_init(&cache->ij_pool, sizeof(uint64_t));
    cache->index = odin3_u64map_create(0);
    cache->ij_index = odin3_u64map_create(0);
    cache->chain_ids = odin3_u64map_create(0);
    if (cache->index == NULL || cache->ij_index == NULL || cache->chain_ids == NULL) {
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

/* NULL for an empty entry: the pool may still be unallocated, and an offset on NULL is UB. */
static const uint32_t *entry_anchors(const label_cache *cache, const anchor_entry *entry) {
    if (entry->len == 0) {
        return NULL;
    }
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

/* Adds the anchors of one entry not yet in seen to merged. */
static odin3_status merge_part(const label_cache *cache, const anchor_entry *part,
                               odin3_u64map *seen, odin3_vec *merged) {
    const uint32_t *list = entry_anchors(cache, part);
    for (uint32_t i = 0; i < part->len; i++) {
        uint64_t unused = 0;
        if (odin3_u64map_get(seen, list[i], &unused)) {
            continue;
        }
        uint32_t *slot = odin3_vec_push(merged);
        if (slot == NULL || odin3_u64map_put(seen, (odin3_kv){list[i], 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = list[i];
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
        entry->flags |= part->flags & (E_SPAN_ONLY | E_HAS_CTX);
        st = merge_part(cache, part, seen, &merged);
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
    if (*cache != NULL && atomic_flag_test_and_set(&(*cache)->busy)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_prov_label_compute: the label cache of this design is in use by another "
                  "thread (labels of one design are computed from one thread at a time)");
        *cache = NULL;
        *entry = NULL;
        return ODIN3_ERR_INVALID_ARG;
    }
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

/* The ordered set J of a record, and whether a single data parent has the same one. */
typedef struct same_probe {
    const odin3_vec *rows;
    bool same;
} same_probe;

/* Sets probe->same when a single data parent of rec has the same set J. */
static odin3_status parent_has_same_set(const odin3_design *design, const label_cache *cache,
                                        const odin3_prov_record *rec, uint32_t data,
                                        same_probe *probe) {
    for (uint32_t k = 0; k < data && !probe->same; k++) {
        odin3_vec theirs;
        odin3_vec_init(&theirs, sizeof(uint32_t));
        odin3_status st = entry_set(design, cache, entry_of(cache, rec->parents.ids[k].v), &theirs);
        probe->same = st == ODIN3_OK && sets_equal(design, &theirs, probe->rows);
        odin3_vec_free(&theirs);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The chain of context record `first` followed by chain `rest`: an existing ID, else a new one. */
static odin3_status chain_cons(label_cache *cache, uint32_t first, uint32_t rest, uint32_t *out) {
    uint64_t have = 0;
    uint64_t key = pair_key(first, rest);
    if (!odin3_u64map_get(cache->chain_ids, key, &have)) {
        have = ++cache->chain_count;
        if (odin3_u64map_put(cache->chain_ids, (odin3_kv){key, have}) != ODIN3_OK) {
            cache->chain_count--;
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *out = (uint32_t)have;
    return ODIN3_OK;
}

static int cmp_u64_pair(const void *ctx, const void *lhs, const void *rhs) {
    (void)ctx;
    uint64_t left = *(const uint64_t *)lhs;
    uint64_t right = *(const uint64_t *)rhs;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

static const ij_entry *ij_of(const label_cache *cache, uint32_t record) {
    uint64_t at = 0;
    return odin3_u64map_get(cache->ij_index, record, &at)
               ? odin3_vec_cat(&cache->ij_entries, at - 1U)
               : NULL;
}

/* Appends the pairs behind data parent `parent` to pairs; *multi when it has two chains already. */
static odin3_status add_parent_pairs(const label_cache *cache, uint32_t parent, odin3_vec *pairs,
                                     bool *multi) {
    const anchor_entry *pe = entry_of(cache, parent);
    const ij_entry *ij = (pe->flags & E_HAS_CTX) != 0 ? ij_of(cache, parent) : NULL;
    if (ij != NULL && ij->multi) {
        *multi = true;
        return ODIN3_OK;
    }
    size_t len = ij != NULL ? ij->len : pe->len;
    odin3_status st = odin3_vec_reserve(pairs, pairs->len + len);
    for (size_t i = 0; st == ODIN3_OK && i < len; i++) {
        *(uint64_t *)odin3_vec_push(pairs) =
            ij != NULL ? *(const uint64_t *)odin3_vec_cat(&cache->ij_pool, ij->off + i)
                       : (uint64_t)entry_anchors(cache, pe)[i] << KEY_SHIFT;
    }
    return st;
}

/* Puts the context tail (parents data .. count - 1) in front of the chain of every pair. */
static odin3_status prepend_tail(label_cache *cache, const odin3_prov_record *rec, uint32_t data,
                                 odin3_vec *pairs) {
    uint64_t *list = pairs->data;
    for (size_t i = 0; i < pairs->len; i++) {
        uint32_t chain = (uint32_t)list[i];
        for (uint32_t k = rec->parents.count; k > data; k--) {
            odin3_status st = chain_cons(cache, rec->parents.ids[k - 1U].v, chain, &chain);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        list[i] = (list[i] & ~(uint64_t)UINT32_MAX) | chain;
    }
    return ODIN3_OK;
}

/* Sorts pairs, drops equal ones, and sets *multi when an anchor is left with two chains. */
static odin3_status settle_pairs(odin3_vec *pairs, bool *multi) {
    const elem_ctx ec = {sizeof(uint64_t), cmp_u64_pair, NULL};
    odin3_status st = merge_sort(pairs, &ec);
    uint64_t *list = pairs->data;
    size_t keep = 0;
    for (size_t i = 0; st == ODIN3_OK && i < pairs->len; i++) {
        if (keep > 0 && list[i] == list[keep - 1U]) {
            continue;
        }
        *multi = *multi || (keep > 0 && (list[i] >> KEY_SHIFT) == (list[keep - 1U] >> KEY_SHIFT));
        list[keep++] = list[i];
    }
    pairs->len = keep;
    return st;
}

/* Stores pairs (nothing when multi) as the ij entry of record. */
static odin3_status ij_store(label_cache *cache, uint32_t record, const odin3_vec *pairs,
                             bool multi) {
    ij_entry entry = {(uint32_t)cache->ij_pool.len, multi ? 0U : (uint32_t)pairs->len, multi};
    odin3_status st = odin3_vec_reserve(&cache->ij_pool, cache->ij_pool.len + entry.len);
    for (size_t i = 0; st == ODIN3_OK && i < entry.len; i++) {
        *(uint64_t *)odin3_vec_push(&cache->ij_pool) = *(const uint64_t *)odin3_vec_cat(pairs, i);
    }
    ij_entry *slot = st == ODIN3_OK ? odin3_vec_push(&cache->ij_entries) : NULL;
    if (slot == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = entry;
    if (odin3_u64map_put(cache->ij_index, (odin3_kv){record, cache->ij_entries.len}) != ODIN3_OK) {
        odin3_vec_pop(&cache->ij_entries);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* Fills the ij entry of a record with a context record behind it, from its data parents'. */
static odin3_status ij_fill(const odin3_design *design, label_cache *cache, uint32_t record) {
    const odin3_prov_record *rec = odin3_prov_get(design, (odin3_prov_id){record});
    uint32_t data =
        rec->parents.count - tail_of(design, (odin3_prov_id){record}, rec->parents.count);
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint64_t));
    bool multi = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && !multi && k < data; k++) {
        st = add_parent_pairs(cache, rec->parents.ids[k].v, &pairs, &multi);
    }
    st = st == ODIN3_OK && !multi ? prepend_tail(cache, rec, data, &pairs) : st;
    st = st == ODIN3_OK && !multi ? settle_pairs(&pairs, &multi) : st;
    st = st == ODIN3_OK ? ij_store(cache, record, &pairs, multi) : st;
    odin3_vec_free(&pairs);
    return st;
}

/* Pushes the data parents of record that have a context record behind them and no ij entry. */
static odin3_status ij_parents_ready(const odin3_design *design, const label_cache *cache,
                                     odin3_vec *todo, uint32_t record, bool *ready) {
    const odin3_prov_record *rec = odin3_prov_get(design, (odin3_prov_id){record});
    uint32_t data =
        rec->parents.count - tail_of(design, (odin3_prov_id){record}, rec->parents.count);
    *ready = true;
    for (uint32_t k = data; k > 0; k--) {
        uint32_t parent = rec->parents.ids[k - 1U].v;
        if ((entry_of(cache, parent)->flags & E_HAS_CTX) == 0 || ij_of(cache, parent) != NULL) {
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

/* Whether an anchor behind the record is reached with two chains; the ij entries are cached. */
static odin3_status instance_join_of(const odin3_design *design, label_cache *cache,
                                     uint32_t record, bool *found) {
    *found = false;
    if ((entry_of(cache, record)->flags & E_HAS_CTX) == 0) {
        return ODIN3_OK; /* no context record behind it: one chain for everything */
    }
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(uint32_t));
    uint32_t *first = odin3_vec_push(&todo);
    odin3_status st = first != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (first != NULL) {
        *first = record;
    }
    while (st == ODIN3_OK && todo.len > 0) {
        uint32_t top = *(const uint32_t *)odin3_vec_cat(&todo, todo.len - 1U);
        bool ready = true;
        if (ij_of(cache, top) != NULL) {
            odin3_vec_pop(&todo);
            continue;
        }
        st = ij_parents_ready(design, cache, &todo, top, &ready);
        if (st == ODIN3_OK && ready) {
            odin3_vec_pop(&todo);
            st = ij_fill(design, cache, top);
        }
    }
    odin3_vec_free(&todo);
    *found = st == ODIN3_OK && ij_of(cache, record)->multi;
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
}

/* The join labels of a coarsen record from its set J; fills label->joined. */
static odin3_status label_join(const odin3_design *design, label_cache *cache,
                               const odin3_prov_record *rec, const anchor_entry *entry,
                               odin3_prov_label *label) {
    odin3_prov_info *info = &label->info;
    odin3_vec rows;
    odin3_vec_init(&rows, sizeof(uint32_t));
    same_probe probe = {&rows, false};
    bool instance = false;
    uint32_t data = rec->parents.count - info->n_context;
    odin3_status st = entry_set(design, cache, entry, &rows);
    if (st == ODIN3_OK) {
        st = parent_has_same_set(design, cache, rec, data, &probe);
    }
    if (st == ODIN3_OK) {
        st = instance_join_of(design, cache, info->record, &instance);
    }
    if (st != ODIN3_OK) {
        odin3_vec_free(&rows);
        return st;
    }
    info->n_joined = (uint32_t)rows.len;
    if (rows.len >= 2 && !probe.same) {
        info->flags |= ODIN3_PROV_F_JOIN;
    }
    if (rows.len > ODIN3_PROV_JOIN_CAP) {
        info->flags |= ODIN3_PROV_F_LOSSY_CAP;
    }
    odin3_opclass cls;
    if (odin3_prov_class_of(design, (odin3_prov_id){info->record}, &cls) && cls.bulk) {
        info->flags |= ODIN3_PROV_F_LOSSY_BULK; /* with the other lossy flags: one place */
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
    if (cache != NULL) {
        atomic_flag_clear(&cache->busy);
    }
    if (st != ODIN3_OK) {
        odin3_prov_label_free(&fresh);
        return st;
    }
    odin3_prov_label_free(label);
    *label = fresh;
    return ODIN3_OK;
}

/* --- the version 2 name map (3.2 to 3.5) ------------------------------------------------------ */

static const char *const MODE_WORDS[] = {"none", "refs", "constructs", "full"}; /* by enum */
static const char *const RANK_WORDS[] = {"none", "constructs", "refs", "full"}; /* by rank */
enum {
    MODE_RANK_MAX = 3,
    MODE_RANK_REFS = 2,
    ORIGIN_CAP = ODIN3_HDL_LOC_MAX /* one constant: the locations a record keeps */
};

static const char *level_word(unsigned level) {
    return odin3_level_word(level);
}

/* 0 for no snapshot, then constructs < refs < full (a larger mode keeps everything a smaller one
 * does). */
static int mode_rank(odin3_construct_mode mode) {
    switch (mode) {
    case ODIN3_CONSTRUCT_MODE_CONSTRUCTS:
        return 1;
    case ODIN3_CONSTRUCT_MODE_REFS:
        return MODE_RANK_REFS;
    case ODIN3_CONSTRUCT_MODE_FULL:
        return MODE_RANK_MAX;
    default:
        return 0;
    }
}

static odin3_construct_mode mode_of_rank(int rank) {
    static const odin3_construct_mode BY_RANK[] = {
        ODIN3_CONSTRUCT_MODE_OFF, ODIN3_CONSTRUCT_MODE_CONSTRUCTS, ODIN3_CONSTRUCT_MODE_REFS,
        ODIN3_CONSTRUCT_MODE_FULL};
    return BY_RANK[rank];
}

static int requested_rank(odin3_prov_map_mode requested) {
    switch (requested) {
    case ODIN3_PROV_MAP_CONSTRUCTS:
        return 1;
    case ODIN3_PROV_MAP_FULL:
        return MODE_RANK_MAX;
    default:
        return MODE_RANK_REFS; /* REFS, and ANY before it is clipped */
    }
}

odin3_status odin3_prov_map_resolve(const odin3_design *design, odin3_prov_map_mode requested,
                                    const char *who, odin3_construct_mode *effective) {
    int kept = 0;
    for (uint32_t run = 1; design != NULL && run < odin3_passrun_end(design); run++) {
        int rank = mode_rank(odin3_construct_run_mode(design, (odin3_passrun_id){run}));
        kept = rank > kept ? rank : kept;
    }
    if (design == NULL || requested == ODIN3_PROV_MAP_NONE) {
        odin3_log(ODIN3_LOG_ERROR, "%s: --prov-map: invalid argument", who);
        return ODIN3_ERR_INVALID_ARG;
    }
    int want = requested_rank(requested);
    if (requested == ODIN3_PROV_MAP_ANY) {
        want = want < kept ? want : kept;
    } else if (want > kept) {
        odin3_log(ODIN3_LOG_ERROR,
                  "%s: --prov-map %s needs more than the read kept (%s): read the design with "
                  "`read_verilog --prov-ast %s`",
                  who, RANK_WORDS[want], RANK_WORDS[kept], RANK_WORDS[want]);
        return ODIN3_ERR_INVALID_ARG;
    }
    *effective = mode_of_rank(want);
    return ODIN3_OK;
}

/* One record as numbered: the labels the R line and the O lines need. */
typedef struct pm_rec {
    uint32_t id;       /* the record ID */
    uint32_t flags;    /* ODIN3_PROV_F_* */
    uint32_t n_joined; /* |J| */
    uint32_t n_context;
    uint32_t cons_off; /* the construct numbers of `cons`, in the cons pool */
    uint32_t cons_len;
    uint32_t con;  /* S: the number of its construct, 0 none */
    uint32_t call; /* S: the number of its call row, 0 none */
    uint8_t kind;  /* odin3_prov_kind */
    uint8_t dir;
    uint8_t from;
    uint8_t level; /* the `level` field */
} pm_rec;

/* A construct as numbered: the first row seen with its chain, its number and its parent's. */
typedef struct pm_con {
    uint32_t row;
    uint32_t num; /* 0 until numbered */
    uint32_t parent_num;
    uint32_t next; /* the next entry with the same chain hash, + 1 */
} pm_con;

typedef struct pm {
    const odin3_prov_map_req *req;
    const odin3_design *design;
    odin3_vec recs;       /* pm_rec, in r order */
    odin3_u64map *num_of; /* record ID -> r number */
    odin3_vec cons;       /* pm_con */
    odin3_vec con_order;  /* uint32_t: index into cons, in c order */
    odin3_vec cons_pool;  /* uint32_t: construct numbers */
    odin3_vec hashes;     /* uint64_t per construct row: the hash of its chain */
    odin3_u64map *by_hash;
    odin3_u64map *file_num; /* strtab ID -> f number */
    odin3_vec file_ids;     /* uint32_t */
    odin3_u64map *run_num;  /* run -> t number */
    odin3_vec run_ids;      /* uint32_t */
    odin3_u64map *op_rank;  /* run << 32 | op -> the op number in the T row */
    size_t cons_skipped;    /* joined rows left out of a J list: no numbered construct */
} pm;

static void pm_free(pm *st) {
    odin3_vec_free(&st->recs);
    odin3_vec_free(&st->cons);
    odin3_vec_free(&st->con_order);
    odin3_vec_free(&st->cons_pool);
    odin3_vec_free(&st->hashes);
    odin3_vec_free(&st->file_ids);
    odin3_vec_free(&st->run_ids);
    odin3_u64map_destroy(st->num_of);
    odin3_u64map_destroy(st->by_hash);
    odin3_u64map_destroy(st->file_num);
    odin3_u64map_destroy(st->run_num);
    odin3_u64map_destroy(st->op_rank);
}

static odin3_status pm_init(pm *st, const odin3_prov_map_req *req) {
    *st = (pm){.req = req, .design = req->design};
    odin3_vec_init(&st->recs, sizeof(pm_rec));
    odin3_vec_init(&st->cons, sizeof(pm_con));
    odin3_vec_init(&st->con_order, sizeof(uint32_t));
    odin3_vec_init(&st->cons_pool, sizeof(uint32_t));
    odin3_vec_init(&st->hashes, sizeof(uint64_t));
    odin3_vec_init(&st->file_ids, sizeof(uint32_t));
    odin3_vec_init(&st->run_ids, sizeof(uint32_t));
    st->num_of = odin3_u64map_create(0);
    st->by_hash = odin3_u64map_create(0);
    st->file_num = odin3_u64map_create(0);
    st->run_num = odin3_u64map_create(0);
    st->op_rank = odin3_u64map_create(0);
    if (st->num_of == NULL || st->by_hash == NULL || st->file_num == NULL || st->run_num == NULL ||
        st->op_rank == NULL) {
        pm_free(st);
        *st = (pm){0};
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* --- numbering the records (post-order over the parents) ------------------------------------- */

typedef struct dfs_frame {
    uint32_t rec;
    uint32_t next; /* the next parent to visit */
} dfs_frame;

static odin3_status pm_push_dfs(odin3_vec *stack, uint32_t rec) {
    dfs_frame *slot = odin3_vec_push(stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (dfs_frame){rec, 0};
    return ODIN3_OK;
}

/* Gives rec the next number: appends its (not yet labelled) row. */
static odin3_status pm_number(pm *st, uint32_t rec) {
    pm_rec *slot = odin3_vec_push(&st->recs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (pm_rec){.id = rec};
    if (odin3_u64map_put(st->num_of, (odin3_kv){rec, st->recs.len}) != ODIN3_OK) {
        odin3_vec_pop(&st->recs);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* Numbers root and every record behind it that has no number yet: visit(r) visits the parents of
 * r in stored order, then numbers r. */
static odin3_status pm_visit(pm *st, uint32_t root) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(dfs_frame));
    odin3_status status =
        odin3_u64map_get(st->num_of, root, NULL) ? ODIN3_OK : pm_push_dfs(&stack, root);
    while (status == ODIN3_OK && stack.len > 0) {
        dfs_frame *top = odin3_vec_at(&stack, stack.len - 1U);
        const odin3_prov_record *rec = odin3_prov_get(st->design, (odin3_prov_id){top->rec});
        if (top->next < rec->parents.count) {
            uint32_t parent = rec->parents.ids[top->next++].v;
            status =
                odin3_u64map_get(st->num_of, parent, NULL) ? ODIN3_OK : pm_push_dfs(&stack, parent);
            continue;
        }
        uint32_t done = top->rec;
        odin3_vec_pop(&stack);
        status = pm_number(st, done);
    }
    odin3_vec_free(&stack);
    return status;
}

/* --- the constructs (numbered by chain identity, parents first) ------------------------------ */

/* The row itself, or in `constructs` mode its nearest ancestor that is not an expression; 0 when
 * the map holds no AST layer. */
static uint32_t pm_resolve(const pm *st, uint32_t row) {
    if (st->req->ast == ODIN3_CONSTRUCT_MODE_OFF) {
        return 0;
    }
    while (row != 0 && st->req->ast == ODIN3_CONSTRUCT_MODE_CONSTRUCTS &&
           odin3_construct_get(st->design, row)->level == (uint8_t)ODIN3_LEVEL_EXPR) {
        row = odin3_construct_get(st->design, row)->parent;
    }
    return row;
}

static const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME = 0x100000001b3ULL;

static uint64_t mix(uint64_t hash, uint64_t word) {
    return (hash ^ word) * FNV_PRIME;
}

/* The hash of every row's chain: its own key fields mixed into the parent's. Parents come first
 * (check rule 12), so one pass in row order fills the table. */
static odin3_status pm_hash_rows(pm *st) {
    uint32_t end = odin3_construct_end(st->design);
    odin3_status status = odin3_vec_reserve(&st->hashes, end);
    for (uint32_t row = 0; status == ODIN3_OK && row < end; row++) {
        uint64_t *slot = odin3_vec_push(&st->hashes);
        const odin3_construct_row *cur = row != 0 ? odin3_construct_get(st->design, row) : NULL;
        uint64_t hash = FNV_OFFSET;
        if (cur != NULL) {
            uint64_t up = cur->parent != 0 && cur->parent < row
                              ? *(const uint64_t *)odin3_vec_cat(&st->hashes, cur->parent)
                              : 0;
            hash = mix(mix(mix(mix(mix(up, cur->file), cur->line), cur->col), cur->end_line),
                       cur->end_col);
            hash = mix(mix(mix(hash, cur->kind), cur->name), cur->part);
        }
        *slot = hash;
    }
    return status;
}

/* The entry of the chain of row, made (number 0) when there is none. */
static odin3_status pm_canon(pm *st, uint32_t row, uint32_t *index) {
    uint64_t hash = *(const uint64_t *)odin3_vec_cat(&st->hashes, row);
    uint64_t head = 0;
    (void)odin3_u64map_get(st->by_hash, hash, &head);
    for (uint64_t at = head; at != 0;) {
        const pm_con *ent = odin3_vec_cat(&st->cons, at - 1U);
        if (same_chain(st->design, ent->row, row)) {
            *index = (uint32_t)(at - 1U);
            return ODIN3_OK;
        }
        at = ent->next;
    }
    pm_con *slot = odin3_vec_push(&st->cons);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (pm_con){.row = row, .next = (uint32_t)head};
    *index = (uint32_t)st->cons.len - 1U;
    if (odin3_u64map_put(st->by_hash, (odin3_kv){hash, st->cons.len}) != ODIN3_OK) {
        odin3_vec_pop(&st->cons);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

static uint32_t pm_con_num(const pm *st, uint32_t index) {
    return ((const pm_con *)odin3_vec_cat(&st->cons, index))->num;
}

/* The resolved parent row of the construct entry `at`, 0 for the top of the chain. */
static uint32_t pm_up_row(const pm *st, uint32_t at) {
    const pm_con *ent = odin3_vec_cat(&st->cons, at);
    return pm_resolve(st, odin3_construct_get(st->design, ent->row)->parent);
}

/* Appends to todo the entries from `index` up to the first numbered one, child first. */
static odin3_status pm_collect_chain(pm *st, uint32_t index, odin3_vec *todo) {
    uint32_t cur = index;
    while (pm_con_num(st, cur) == 0) {
        uint32_t *slot = odin3_vec_push(todo);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = cur;
        uint32_t up = pm_up_row(st, cur);
        if (up == 0) {
            break;
        }
        odin3_status status = pm_canon(st, up, &cur);
        if (status != ODIN3_OK) {
            return status;
        }
    }
    return ODIN3_OK;
}

/* Gives entry `at` the next number; its parent (if any) already has one. */
static odin3_status pm_number_entry(pm *st, uint32_t at) {
    uint32_t up = pm_up_row(st, at);
    uint32_t parent_num = 0;
    if (up != 0) {
        uint32_t up_index = 0;
        odin3_status status = pm_canon(st, up, &up_index);
        if (status != ODIN3_OK) {
            return status;
        }
        parent_num = pm_con_num(st, up_index);
    }
    uint32_t *order = odin3_vec_push(&st->con_order);
    if (order == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *order = at;
    pm_con *ent = odin3_vec_at(&st->cons, at);
    ent->parent_num = parent_num;
    ent->num = (uint32_t)st->con_order.len;
    return ODIN3_OK;
}

/* Numbers the construct of the chain of `index` and, first, any ancestor without a number. The
 * ancestors are found on a stack, so a deep chain does not recurse. */
static odin3_status pm_number_chain(pm *st, uint32_t index) {
    odin3_vec todo; /* indexes into cons, child first */
    odin3_vec_init(&todo, sizeof(uint32_t));
    odin3_status status = pm_collect_chain(st, index, &todo);
    for (size_t i = todo.len; status == ODIN3_OK && i > 0; i--) {
        status = pm_number_entry(st, *(const uint32_t *)odin3_vec_cat(&todo, i - 1U));
    }
    odin3_vec_free(&todo);
    return status;
}

/* The number of the construct of row (after resolution), numbering it and its chain on first use;
 * 0 for row 0 or when the map holds no AST layer. */
static odin3_status pm_con_of_row(pm *st, uint32_t row, uint32_t *num) {
    uint32_t use = pm_resolve(st, row);
    uint32_t index = 0;
    *num = 0;
    if (use == 0) {
        return ODIN3_OK;
    }
    odin3_status status = pm_canon(st, use, &index);
    status = status == ODIN3_OK && pm_con_num(st, index) == 0 ? pm_number_chain(st, index) : status;
    *num = status == ODIN3_OK ? pm_con_num(st, index) : 0;
    return status;
}

/* --- labelling the records (phase A) ---------------------------------------------------------- */

/* Appends the numbers of the first ODIN3_PROV_JOIN_CAP rows of J to the pool. */
static odin3_status pm_take_cons(pm *st, const odin3_prov_label *label, pm_rec *out) {
    uint32_t count = (uint32_t)label->joined.len;
    count = count < ODIN3_PROV_JOIN_CAP ? count : (uint32_t)ODIN3_PROV_JOIN_CAP;
    out->cons_off = (uint32_t)st->cons_pool.len;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t num = 0;
        odin3_status status =
            pm_con_of_row(st, *(const uint32_t *)odin3_vec_cat(&label->joined, i), &num);
        if (status == ODIN3_OK && num == 0) {
            st->cons_skipped++; /* ast=none: no construct is numbered, `c0` would not resolve */
            continue;
        }
        uint32_t *slot = status == ODIN3_OK ? odin3_vec_push(&st->cons_pool) : NULL;
        if (status != ODIN3_OK || slot == NULL) {
            return status != ODIN3_OK ? status : ODIN3_ERR_NO_MEMORY;
        }
        *slot = num;
        out->cons_len++;
    }
    return ODIN3_OK;
}

/* A SOURCE record's construct and, for a call-instance row, the number of its call row. */
static odin3_status pm_take_leaf(pm *st, const odin3_prov_label *label, pm_rec *out) {
    odin3_status status = pm_con_of_row(st, label->info.construct, &out->con);
    if (status == ODIN3_OK && label->info.construct != 0) {
        uint32_t call = odin3_construct_get(st->design, label->info.construct)->call;
        status = call != 0 ? pm_con_of_row(st, call, &out->call) : ODIN3_OK;
    }
    if (status == ODIN3_OK && out->con != 0) {
        uint32_t row = pm_resolve(st, label->info.construct);
        out->level = (uint8_t)odin3_construct_get(st->design, row)->level;
    }
    return status;
}

/* Fills the row of record `id` from its label. */
static odin3_status pm_label(pm *st, uint32_t id, pm_rec *out) {
    odin3_prov_label label;
    odin3_prov_label_init(&label);
    odin3_status status = odin3_prov_label_compute(st->design, id, &label);
    if (status == ODIN3_OK && label.info.kind == (uint32_t)ODIN3_PROV_DERIVED &&
        label.info.dir == ODIN3_DIR_ORIGIN) {
        /* A derived record with no operation class has no direction: the map would fail its own
         * check (`origin:` is for S and I records only), so the writer stops. */
        odin3_log(ODIN3_LOG_ERROR,
                  "name map: record %u (run %u, op %u) is derived but its operation has no class; "
                  "register a class for the operation before exporting a provenance map",
                  (unsigned)id, (unsigned)label.info.run, (unsigned)label.info.op);
        status = ODIN3_ERR_INVALID_ARG;
    }
    if (status == ODIN3_OK) {
        const odin3_prov_info *info = &label.info;
        *out = (pm_rec){.id = id,
                        .flags = info->flags,
                        .n_joined = info->n_joined,
                        .n_context = info->n_context,
                        .kind = (uint8_t)info->kind,
                        .dir = (uint8_t)info->dir,
                        .from = (uint8_t)info->from_level,
                        .level = (uint8_t)info->to_level};
        if (info->kind == (uint32_t)ODIN3_PROV_DERIVED) {
            bool listed = (info->flags & (ODIN3_PROV_F_JOIN | ODIN3_PROV_F_JOIN_INSTANCE |
                                          ODIN3_PROV_F_LOSSY_BULK | ODIN3_PROV_F_LOSSY_CAP)) != 0;
            status = listed ? pm_take_cons(st, &label, out) : ODIN3_OK;
        } else {
            status = pm_take_leaf(st, &label, out);
        }
    }
    odin3_prov_label_free(&label);
    return status;
}

/* Labels every numbered record in number order, which numbers the constructs as the algorithm of
 * 3.2 item 4 scans the R lines. */
static odin3_status pm_label_all(pm *st) {
    odin3_status status = pm_hash_rows(st);
    for (size_t i = 0; status == ODIN3_OK && i < st->recs.len; i++) {
        pm_rec fresh;
        status = pm_label(st, ((const pm_rec *)odin3_vec_cat(&st->recs, i))->id, &fresh);
        if (status == ODIN3_OK) {
            *(pm_rec *)odin3_vec_at(&st->recs, i) = fresh;
        }
    }
    return status;
}

/* `full`: the rows no record reached, after the others, in the key order (parents first). */
static odin3_status pm_add_all_rows(pm *st) {
    if (st->req->ast != ODIN3_CONSTRUCT_MODE_FULL) {
        return ODIN3_OK;
    }
    odin3_vec rows;
    odin3_vec_init(&rows, sizeof(uint32_t));
    uint32_t end = odin3_construct_end(st->design);
    odin3_status status = odin3_vec_reserve(&rows, end);
    for (uint32_t row = 1; status == ODIN3_OK && row < end; row++) {
        *(uint32_t *)odin3_vec_push(&rows) = row;
    }
    status = status == ODIN3_OK ? order_rows(st->design, &rows) : status;
    for (size_t i = 0; status == ODIN3_OK && i < rows.len; i++) {
        uint32_t unused = 0;
        status = pm_con_of_row(st, *(const uint32_t *)odin3_vec_cat(&rows, i), &unused);
    }
    odin3_vec_free(&rows);
    return status;
}

/* --- files and pass runs (first reference, scanning the R lines) ------------------------------ */

/* The f number of strtab string `id`, assigned on first use. */
static odin3_status pm_file(pm *st, uint32_t id, uint32_t *num) {
    uint64_t have = 0;
    if (odin3_u64map_get(st->file_num, id, &have)) {
        *num = (uint32_t)have;
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_push(&st->file_ids);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    *num = (uint32_t)st->file_ids.len;
    if (odin3_u64map_put(st->file_num, (odin3_kv){id, *num}) != ODIN3_OK) {
        odin3_vec_pop(&st->file_ids);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

static int cmp_u64_elem(const void *ctx, const void *lhs, const void *rhs) {
    (void)ctx;
    uint64_t left = *(const uint64_t *)lhs;
    uint64_t right = *(const uint64_t *)rhs;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* Numbers `run` as the next T row when it has no number yet. */
static odin3_status pm_note_run(pm *st, uint32_t run) {
    if (odin3_u64map_get(st->run_num, run, NULL)) {
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_push(&st->run_ids);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = run;
    return odin3_u64map_put(st->run_num, (odin3_kv){run, st->run_ids.len});
}

/* The operation numbers of each T row: the distinct operations of its run, from 1, in ascending
 * order of the original number. pairs holds (run << 32 | op), sorted. */
static odin3_status pm_rank_ops(pm *st, const odin3_vec *pairs) {
    uint64_t prev = 0;
    uint32_t rank = 0;
    for (size_t i = 0; i < pairs->len; i++) {
        uint64_t key = *(const uint64_t *)odin3_vec_cat(pairs, i);
        if (i > 0 && key == prev) {
            continue;
        }
        bool same_run = i > 0 && (key >> KEY_SHIFT) == (prev >> KEY_SHIFT);
        rank = same_run ? rank + 1U : 1U;
        prev = key;
        odin3_status status = odin3_u64map_put(st->op_rank, (odin3_kv){key, rank});
        if (status != ODIN3_OK) {
            return status;
        }
    }
    return ODIN3_OK;
}

/* Numbers the pass runs by first reference and ranks the operations each T row names. */
static odin3_status pm_runs(pm *st) {
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint64_t));
    odin3_status status = odin3_vec_reserve(&pairs, st->recs.len);
    for (size_t i = 0; status == ODIN3_OK && i < st->recs.len; i++) {
        const pm_rec *pr = odin3_vec_cat(&st->recs, i);
        const odin3_prov_record *rec = odin3_prov_get(st->design, (odin3_prov_id){pr->id});
        *(uint64_t *)odin3_vec_push(&pairs) = ((uint64_t)rec->run.v << KEY_SHIFT) | rec->op;
        status = pm_note_run(st, rec->run.v);
    }
    const elem_ctx sort = {sizeof(uint64_t), cmp_u64_elem, NULL};
    status = status == ODIN3_OK ? merge_sort(&pairs, &sort) : status;
    status = status == ODIN3_OK ? pm_rank_ops(st, &pairs) : status;
    odin3_vec_free(&pairs);
    return status;
}

/* --- emitting the lines (phase B) ------------------------------------------------------------- */

static odin3_nm_field fld_plain(odin3_bytes text) {
    return (odin3_nm_field){text, false};
}

static odin3_nm_field fld_raw(const odin3_strbuf *buf) {
    return (odin3_nm_field){{buf->data, buf->len}, true};
}

static odin3_bytes str_of(const odin3_design *design, uint32_t id) {
    const odin3_strtab *tab = odin3_design_strtab(design);
    const char *text = id != 0 ? odin3_strtab_get(tab, id) : NULL;
    return text != NULL ? (odin3_bytes){text, odin3_strtab_len(tab, id)} : (odin3_bytes){"", 0};
}

/* The scratch buffers of the line being written. */
typedef struct line_bufs {
    odin3_strbuf parents, origins, cons, flags;
    odin3_strbuf num[3]; /* the id, the step and the dir/span scratch fields */
} line_bufs;

static void bufs_init(line_bufs *bufs) {
    odin3_strbuf_init(&bufs->parents);
    odin3_strbuf_init(&bufs->origins);
    odin3_strbuf_init(&bufs->cons);
    odin3_strbuf_init(&bufs->flags);
    odin3_strbuf_init(&bufs->num[0]);
    odin3_strbuf_init(&bufs->num[1]);
    odin3_strbuf_init(&bufs->num[2]);
}

static void bufs_free(line_bufs *bufs) {
    odin3_strbuf_free(&bufs->parents);
    odin3_strbuf_free(&bufs->origins);
    odin3_strbuf_free(&bufs->cons);
    odin3_strbuf_free(&bufs->flags);
    odin3_strbuf_free(&bufs->num[0]);
    odin3_strbuf_free(&bufs->num[1]);
    odin3_strbuf_free(&bufs->num[2]);
}

/* Appends `<sep>` (when buf is not empty) and then the formatted text. */
static odin3_status append_item(odin3_strbuf *buf, char sep, const char *fmt, uint32_t value) {
    odin3_status status = buf->len > 0 ? odin3_strbuf_appendf(buf, "%c", sep) : ODIN3_OK;
    return status == ODIN3_OK ? odin3_strbuf_appendf(buf, fmt, (unsigned)value) : status;
}

/* parents: r<n>,r<n>,... in stored order. */
static odin3_status build_parents(const pm *st, const odin3_prov_record *rec, odin3_strbuf *out) {
    odin3_status status = ODIN3_OK;
    for (uint32_t k = 0; status == ODIN3_OK && k < rec->parents.count; k++) {
        uint64_t num = 0;
        (void)odin3_u64map_get(st->num_of, rec->parents.ids[k].v, &num);
        status = append_item(out, ',', "r%u", (uint32_t)num);
    }
    return status;
}

/* origins: f<n>:L.C-EL.EC joined by `|`, at most ORIGIN_CAP (`-` for a location with no file). */
static odin3_status build_origins(pm *st, const odin3_prov_record *rec, odin3_strbuf *out) {
    uint32_t count = rec->n_locs < ORIGIN_CAP ? rec->n_locs : (uint32_t)ORIGIN_CAP;
    odin3_status status = ODIN3_OK;
    for (uint32_t k = 0; status == ODIN3_OK && k < count; k++) {
        const odin3_srcloc *loc = &rec->locs[k];
        uint32_t num = 0;
        status = loc->file != 0 ? pm_file(st, loc->file, &num) : ODIN3_OK;
        status = status == ODIN3_OK && out->len > 0
                     ? odin3_strbuf_append(out, odin3_bytes_cstr("|"))
                     : status;
        if (status == ODIN3_OK && num != 0) {
            status = odin3_strbuf_appendf(out, "f%u", (unsigned)num);
        } else if (status == ODIN3_OK) {
            status = odin3_strbuf_append(out, odin3_bytes_cstr("-"));
        }
        status =
            status == ODIN3_OK
                ? odin3_strbuf_appendf(out, ":%u.%u-%u.%u", (unsigned)loc->line, (unsigned)loc->col,
                                       (unsigned)loc->end_line, (unsigned)loc->end_col)
                : status;
    }
    return status;
}

static odin3_status build_cons(const pm *st, const pm_rec *pr, odin3_strbuf *out) {
    odin3_status status = ODIN3_OK;
    for (uint32_t k = 0; status == ODIN3_OK && k < pr->cons_len; k++) {
        uint32_t num = *(const uint32_t *)odin3_vec_cat(&st->cons_pool, pr->cons_off + k);
        status = append_item(out, ',', "c%u", num);
    }
    return status;
}

/* One flag word, `,`-separated. */
static odin3_status add_flag(odin3_strbuf *out, const char *text) {
    odin3_status status = out->len > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr(",")) : ODIN3_OK;
    return status == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(text)) : status;
}

/* One flag word with a number, `,`-separated. */
static odin3_status add_flag_num(odin3_strbuf *out, const char *fmt, uint32_t value) {
    odin3_status status = out->len > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr(",")) : ODIN3_OK;
    return status == ODIN3_OK ? odin3_strbuf_appendf(out, fmt, (unsigned)value) : status;
}

/* join, join=instance, lossy=bulk, lossy=cap and the total n=. */
static odin3_status flags_join(const pm_rec *pr, odin3_strbuf *out) {
    static const struct {
        uint32_t bit;
        const char *word;
    } FIRST[] = {{ODIN3_PROV_F_JOIN, "join"},
                 {ODIN3_PROV_F_JOIN_INSTANCE, "join=instance"},
                 {ODIN3_PROV_F_LOSSY_BULK, "lossy=bulk"},
                 {ODIN3_PROV_F_LOSSY_CAP, "lossy=cap"}};
    odin3_status status = ODIN3_OK;
    for (size_t k = 0; status == ODIN3_OK && k < sizeof FIRST / sizeof FIRST[0]; k++) {
        status = (pr->flags & FIRST[k].bit) != 0 ? add_flag(out, FIRST[k].word) : status;
    }
    bool lossy = (pr->flags & (ODIN3_PROV_F_LOSSY_BULK | ODIN3_PROV_F_LOSSY_CAP)) != 0;
    return status == ODIN3_OK && lossy ? add_flag_num(out, "n=%u", pr->n_joined) : status;
}

/* chain=span, capped and hier=<text>. */
static odin3_status flags_origin(const pm_rec *pr, const odin3_prov_record *rec, const pm *st,
                                 odin3_strbuf *out) {
    odin3_status status =
        (pr->flags & ODIN3_PROV_F_SPAN_ONLY) != 0 ? add_flag(out, "chain=span") : ODIN3_OK;
    status = status == ODIN3_OK && rec->n_locs > ORIGIN_CAP ? add_flag(out, "capped") : status;
    if (status == ODIN3_OK && rec->hier != 0) {
        status = add_flag(out, "hier=");
        status =
            status == ODIN3_OK ? odin3_name_map_escape(out, str_of(st->design, rec->hier)) : status;
    }
    return status;
}

/* The flags of one record, in the order of 3.2: join, join=instance, lossy=bulk, lossy=cap,
 * n=<total>, chain=span, capped, hier=<text>, call=c<n>, ctx=<n>, bulk. (`cell=<type>` is not
 * written: a record does not know the type of the hard cell it made; agent default.) */
static odin3_status build_flags(const pm_rec *pr, const odin3_prov_record *rec, const pm *st,
                                odin3_strbuf *out) {
    odin3_status status = flags_join(pr, out);
    status = status == ODIN3_OK ? flags_origin(pr, rec, st, out) : status;
    status = status == ODIN3_OK && pr->call != 0 ? add_flag_num(out, "call=c%u", pr->call) : status;
    status = status == ODIN3_OK && pr->n_context != 0 ? add_flag_num(out, "ctx=%u", pr->n_context)
                                                      : status;
    return status == ODIN3_OK && (pr->flags & ODIN3_PROV_F_LOSSY_BULK) != 0 ? add_flag(out, "bulk")
                                                                            : status;
}

static const char *kind_letter(unsigned kind) {
    if (kind == (unsigned)ODIN3_PROV_SOURCE) {
        return "S";
    }
    return kind == (unsigned)ODIN3_PROV_IMPORTED ? "I" : "D";
}

/* The cons field: the members of J of a DERIVED record, else the construct of a SOURCE one. */
static odin3_status build_cons_field(const pm *st, const pm_rec *pr, odin3_strbuf *out) {
    if (pr->kind == (uint8_t)ODIN3_PROV_DERIVED) {
        return build_cons(st, pr, out);
    }
    return pr->con != 0 ? append_item(out, ',', "c%u", pr->con) : ODIN3_OK;
}

/* The dir field: `<dir>:<from level>` for a DERIVED record, `origin` for the others. */
static odin3_status build_dir(const pm_rec *pr, odin3_strbuf *out) {
    if (pr->kind == (uint8_t)ODIN3_PROV_DERIVED) {
        return odin3_strbuf_appendf(out, "%s:%s", odin3_dir_word(pr->dir), level_word(pr->from));
    }
    return odin3_strbuf_appendf(out, "origin");
}

/* The scratch fields of the R line of record number index + 1: id, step, dir and the lists. */
static odin3_status fill_record_bufs(pm *st, size_t index, line_bufs *bufs) {
    const pm_rec *pr = odin3_vec_cat(&st->recs, index);
    const odin3_prov_record *rec = odin3_prov_get(st->design, (odin3_prov_id){pr->id});
    uint64_t run = 0;
    uint64_t rank = 0;
    (void)odin3_u64map_get(st->run_num, rec->run.v, &run);
    (void)odin3_u64map_get(st->op_rank, ((uint64_t)rec->run.v << KEY_SHIFT) | rec->op, &rank);
    odin3_strbuf *all[] = {&bufs->num[0],  &bufs->num[1], &bufs->num[2], &bufs->parents,
                           &bufs->origins, &bufs->cons,   &bufs->flags};
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) {
        odin3_strbuf_clear(all[k]);
    }
    odin3_status status = odin3_strbuf_appendf(&bufs->num[0], "r%u", (unsigned)(index + 1U));
    status = status == ODIN3_OK
                 ? odin3_strbuf_appendf(&bufs->num[1], "t%u.%u", (unsigned)run, (unsigned)rank)
                 : status;
    status = status == ODIN3_OK ? build_dir(pr, &bufs->num[2]) : status;
    status = status == ODIN3_OK ? build_parents(st, rec, &bufs->parents) : status;
    if (status == ODIN3_OK && pr->kind != (uint8_t)ODIN3_PROV_DERIVED) {
        status = build_origins(st, rec, &bufs->origins);
    }
    status = status == ODIN3_OK ? build_cons_field(st, pr, &bufs->cons) : status;
    return status == ODIN3_OK ? build_flags(pr, rec, st, &bufs->flags) : status;
}

/* One R line. */
static odin3_status emit_record(pm *st, size_t index, line_bufs *bufs, odin3_strbuf *out) {
    odin3_status status = fill_record_bufs(st, index, bufs);
    if (status != ODIN3_OK) {
        return status;
    }
    const pm_rec *pr = odin3_vec_cat(&st->recs, index);
    const odin3_nm_field fields[] = {fld_raw(&bufs->num[0]),
                                     fld_plain(odin3_bytes_cstr(kind_letter(pr->kind))),
                                     fld_plain(odin3_bytes_cstr(level_word(pr->level))),
                                     fld_raw(&bufs->num[2]),
                                     fld_raw(&bufs->num[1]),
                                     fld_raw(&bufs->parents),
                                     fld_raw(&bufs->origins),
                                     fld_raw(&bufs->cons),
                                     fld_raw(&bufs->flags)};
    return odin3_name_map_write_line_v2(out, 'R', fields, sizeof fields / sizeof fields[0]);
}

/* The AST kind word of a construct row (the synthesized kinds have their own). */
static const char *kind_word(uint8_t kind) {
    switch (kind) {
    case ODIN3_CONSTRUCT_IF_THEN:
        return "IF_THEN";
    case ODIN3_CONSTRUCT_IF_ELSE:
        return "IF_ELSE";
    case ODIN3_CONSTRUCT_CALL_SITE:
        return "CALL_SITE";
    default:
        return odin3_ast_kind_name(kind);
    }
}

/* One C line (the c number is the position in the order). */
static odin3_status emit_construct(pm *st, size_t index, line_bufs *bufs, odin3_strbuf *out) {
    const pm_con *ent =
        odin3_vec_cat(&st->cons, *(const uint32_t *)odin3_vec_cat(&st->con_order, index));
    const odin3_construct_row *row = odin3_construct_get(st->design, ent->row);
    odin3_strbuf *ids = &bufs->num[0];
    odin3_strbuf *span = &bufs->num[1];
    odin3_strbuf_clear(ids);
    odin3_strbuf_clear(span);
    odin3_strbuf_clear(&bufs->parents);
    odin3_strbuf_clear(&bufs->origins);
    uint32_t file = 0;
    odin3_status status = odin3_strbuf_appendf(ids, "c%u", (unsigned)(index + 1U));
    status = status == ODIN3_OK && ent->parent_num != 0
                 ? odin3_strbuf_appendf(&bufs->parents, "c%u", (unsigned)ent->parent_num)
                 : status;
    status = status == ODIN3_OK && row->file != 0 ? pm_file(st, row->file, &file) : status;
    status = status == ODIN3_OK && file != 0
                 ? odin3_strbuf_appendf(&bufs->origins, "f%u", (unsigned)file)
                 : status;
    status = status == ODIN3_OK ? odin3_strbuf_appendf(span, "%u.%u-%u.%u", (unsigned)row->line,
                                                       (unsigned)row->col, (unsigned)row->end_line,
                                                       (unsigned)row->end_col)
                                : status;
    if (status == ODIN3_OK) {
        const odin3_nm_field fields[] = {fld_raw(ids),
                                         fld_plain(odin3_bytes_cstr(kind_word(row->kind))),
                                         fld_plain(odin3_bytes_cstr(level_word(row->level))),
                                         fld_raw(&bufs->parents),
                                         fld_raw(&bufs->origins),
                                         fld_raw(span),
                                         fld_plain(str_of(st->design, row->name))};
        status = odin3_name_map_write_line_v2(out, 'C', fields, sizeof fields / sizeof fields[0]);
    }
    return status;
}

static odin3_status emit_file(const pm *st, size_t index, line_bufs *bufs, odin3_strbuf *out) {
    odin3_strbuf *ids = &bufs->num[0];
    odin3_strbuf_clear(ids);
    uint32_t id = *(const uint32_t *)odin3_vec_cat(&st->file_ids, index);
    odin3_status status = odin3_strbuf_appendf(ids, "f%u", (unsigned)(index + 1U));
    if (status == ODIN3_OK) {
        const odin3_nm_field fields[] = {fld_raw(ids), fld_plain(str_of(st->design, id))};
        status = odin3_name_map_write_line_v2(out, 'F', fields, sizeof fields / sizeof fields[0]);
    }
    return status;
}

static odin3_status emit_run(const pm *st, size_t index, line_bufs *bufs, odin3_strbuf *out) {
    odin3_strbuf *ids = &bufs->num[0];
    odin3_strbuf_clear(ids);
    odin3_passrun_id run = {*(const uint32_t *)odin3_vec_cat(&st->run_ids, index)};
    odin3_status status = odin3_strbuf_appendf(ids, "t%u", (unsigned)(index + 1U));
    if (status == ODIN3_OK) {
        const odin3_nm_field fields[] = {
            fld_raw(ids), fld_plain(str_of(st->design, odin3_passrun_name(st->design, run))),
            fld_plain(str_of(st->design, odin3_passrun_args(st->design, run))),
            fld_plain(str_of(st->design, odin3_passrun_info(st->design, run)))};
        status = odin3_name_map_write_line_v2(out, 'T', fields, sizeof fields / sizeof fields[0]);
    }
    return status;
}

/* --- the objects ------------------------------------------------------------------------------ */

typedef struct obj_order {
    const odin3_prov_obj *objs;
} obj_order;

/* O lines before N lines, then the key of 3.2 item 1, then the input order (stable). */
static int cmp_obj_index(const void *ctx, const void *lhs, const void *rhs) {
    const obj_order *order = ctx;
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;
    const odin3_prov_obj *first = &order->objs[left];
    const odin3_prov_obj *second = &order->objs[right];
    if (first->kept != second->kept) {
        return first->kept ? 1 : -1;
    }
    int cmp = odin3_name_entry_cmp(&first->entry, &second->entry);
    return cmp != 0 ? cmp : cmp_u32(left, right);
}

/* The indexes of objs in the order of the lines. Two O lines with one key are an error; a later N
 * line with the key of an earlier one is dropped (agent default: the first name stays). */
static odin3_status sort_objects(const odin3_prov_obj *objs, size_t count, odin3_vec *sorted) {
    odin3_vec all;
    odin3_vec_init(&all, sizeof(uint32_t));
    odin3_status status = odin3_vec_reserve(&all, count);
    for (size_t i = 0; status == ODIN3_OK && i < count; i++) {
        *(uint32_t *)odin3_vec_push(&all) = (uint32_t)i;
    }
    size_t dropped = 0;
    const obj_order order = {objs};
    const elem_ctx sort = {sizeof(uint32_t), cmp_obj_index, &order};
    status = status == ODIN3_OK ? merge_sort(&all, &sort) : status;
    for (size_t i = 0; status == ODIN3_OK && i < all.len; i++) {
        uint32_t at = *(const uint32_t *)odin3_vec_cat(&all, i);
        const odin3_prov_obj *cur = &objs[at];
        bool dup = false;
        if (i > 0) {
            const odin3_prov_obj *prev = &objs[*(const uint32_t *)odin3_vec_cat(&all, i - 1U)];
            dup = prev->kept == cur->kept && odin3_name_entry_cmp(&prev->entry, &cur->entry) == 0;
        }
        if (dup && !cur->kept) {
            odin3_log(ODIN3_LOG_ERROR, "name map: two objects are named '%.*s' (module '%.*s')",
                      (int)cur->entry.short_name.len, (const char *)cur->entry.short_name.ptr,
                      (int)cur->entry.module.len, (const char *)cur->entry.module.ptr);
            status = ODIN3_ERR_INVALID_ARG;
        } else if (!dup) {
            *(uint32_t *)odin3_vec_push(sorted) = at;
        } else {
            dropped++;
        }
    }
    if (dropped > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "name map: %zu kept name(s) repeat a name already in the map and get no N line "
                  "(the first stays)",
                  dropped);
    }
    odin3_vec_free(&all);
    return status;
}

/* One O or N line of an object whose record is numbered (or 0). */
static odin3_status emit_object(const pm *st, const odin3_prov_obj *obj, line_bufs *bufs,
                                odin3_strbuf *out) {
    uint64_t num = 0;
    if (obj->record != 0) {
        (void)odin3_u64map_get(st->num_of, obj->record, &num);
    }
    const pm_rec *pr = num != 0 ? odin3_vec_cat(&st->recs, num - 1U) : NULL;
    odin3_strbuf *recs = &bufs->num[0];
    odin3_strbuf_clear(recs);
    odin3_status status = num != 0 ? odin3_strbuf_appendf(recs, "r%u", (unsigned)num) : ODIN3_OK;
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    status =
        status == ODIN3_OK ? odin3_strbuf_appendf(&line, "%u", (unsigned)obj->entry.line) : status;
    if (status != ODIN3_OK) {
        odin3_strbuf_free(&line);
        return status;
    }
    const odin3_nm_field level =
        fld_plain(odin3_bytes_cstr(level_word(pr != NULL ? pr->level : 0)));
    const odin3_name_entry *ent = &obj->entry;
    const odin3_nm_field o_fields[] = {fld_plain(ent->module),
                                       fld_plain(odin3_bytes_cstr(odin3_name_kind_str(ent->kind))),
                                       fld_plain(ent->short_name),
                                       fld_plain(ent->full),
                                       fld_plain(ent->hier),
                                       fld_plain(ent->file),
                                       fld_raw(&line),
                                       fld_raw(recs),
                                       level};
    const odin3_nm_field n_fields[] = {fld_plain(ent->module),
                                       fld_plain(odin3_bytes_cstr(odin3_name_kind_str(ent->kind))),
                                       fld_plain(ent->short_name), fld_raw(recs), level};
    status = obj->kept ? odin3_name_map_write_line_v2(out, 'N', n_fields,
                                                      sizeof n_fields / sizeof n_fields[0])
                       : odin3_name_map_write_line_v2(out, 'O', o_fields,
                                                      sizeof o_fields / sizeof o_fields[0]);
    odin3_strbuf_free(&line);
    return status;
}

/* Numbers the records behind the objects, labels them and the constructs. */
static odin3_status pm_prepare(pm *st, const odin3_prov_obj *objs, const odin3_vec *sorted) {
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && i < sorted->len; i++) {
        uint32_t record = objs[*(const uint32_t *)odin3_vec_cat(sorted, i)].record;
        status = record != 0 ? pm_visit(st, record) : ODIN3_OK;
    }
    status = status == ODIN3_OK ? pm_label_all(st) : status;
    status = status == ODIN3_OK ? pm_add_all_rows(st) : status;
    return status == ODIN3_OK ? pm_runs(st) : status;
}

/* The lines in file order; out is appended to. */
static odin3_status pm_emit(pm *st, const odin3_prov_obj *objs, const odin3_vec *sorted,
                            odin3_strbuf *out) {
    line_bufs bufs;
    bufs_init(&bufs);
    odin3_status status =
        odin3_name_map_write_header_v2(out, st->req->width, MODE_WORDS[st->req->ast]);
    for (size_t i = 0; status == ODIN3_OK && i < sorted->len; i++) {
        status = emit_object(st, &objs[*(const uint32_t *)odin3_vec_cat(sorted, i)], &bufs, out);
    }
    for (size_t i = 0; status == ODIN3_OK && i < st->recs.len; i++) {
        status = emit_record(st, i, &bufs, out);
    }
    for (size_t i = 0; status == ODIN3_OK && i < st->con_order.len; i++) {
        status = emit_construct(st, i, &bufs, out);
    }
    for (size_t i = 0; status == ODIN3_OK && i < st->file_ids.len; i++) {
        status = emit_file(st, i, &bufs, out);
    }
    for (size_t i = 0; status == ODIN3_OK && i < st->run_ids.len; i++) {
        status = emit_run(st, i, &bufs, out);
    }
    bufs_free(&bufs);
    return status;
}

/* True when every record of the objects is a record of the design. */
static bool records_valid(const odin3_design *design, const odin3_prov_obj *objs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (objs[i].record != 0 &&
            odin3_prov_get(design, (odin3_prov_id){objs[i].record}) == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "name map: record %u is not a record of the design",
                      (unsigned)objs[i].record);
            return false;
        }
    }
    return true;
}

/* Says how many joined rows were left out of the c lists (none numbered: ast=none). */
static void log_cons_skipped(size_t skipped) {
    if (skipped > 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "name map: %zu joined row(s) have no numbered construct (ast=none) and are left "
                  "out of the c lists, so n= can exceed a list's length",
                  skipped);
    }
}

odin3_status odin3_prov_map_text(const odin3_prov_map_req *req, const odin3_prov_obj *objs,
                                 size_t count, odin3_strbuf *out) {
    if (req == NULL || req->design == NULL || out == NULL || (objs == NULL && count > 0) ||
        !records_valid(req->design, objs, count)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t start = out->len;
    pm st;
    odin3_vec sorted;
    odin3_vec_init(&sorted, sizeof(uint32_t));
    double t_start = odin3_now_ms();
    odin3_status status = pm_init(&st, req);
    status = status == ODIN3_OK ? sort_objects(objs, count, &sorted) : status;
    double t_sorted = odin3_now_ms();
    status = status == ODIN3_OK ? pm_prepare(&st, objs, &sorted) : status;
    double t_prepared = odin3_now_ms();
    status = status == ODIN3_OK ? pm_emit(&st, objs, &sorted, out) : status;
    odin3_log(ODIN3_LOG_INFO, "name map: sort %.1f ms, number and label %.1f ms, emit %.1f ms",
              t_sorted - t_start, t_prepared - t_sorted, odin3_now_ms() - t_prepared);
    if (status != ODIN3_OK && out->len != start) {
        out->len = start;
        if (out->data != NULL) {
            out->data[start] = '\0';
        }
    }
    if (status == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "name map: %zu object(s), %zu record(s); defaults: BLIF N lines are nets only, "
                  "cell= not written, %u locations and %u joined rows at most",
                  count, st.recs.len, (unsigned)ORIGIN_CAP, (unsigned)ODIN3_PROV_JOIN_CAP);
        log_cons_skipped(st.cons_skipped);
    }
    odin3_vec_free(&sorted);
    if (st.num_of != NULL) {
        pm_free(&st);
    }
    return status;
}

bool odin3_prov_map_word(odin3_bytes word, odin3_prov_map_mode *mode) {
    static const struct {
        const char *word;
        odin3_prov_map_mode mode;
    } WORDS[] = {{"refs", ODIN3_PROV_MAP_REFS},
                 {"constructs", ODIN3_PROV_MAP_CONSTRUCTS},
                 {"full", ODIN3_PROV_MAP_FULL}};
    for (size_t k = 0; word.ptr != NULL && k < sizeof WORDS / sizeof WORDS[0]; k++) {
        if (word.len == strlen(WORDS[k].word) && memcmp(word.ptr, WORDS[k].word, word.len) == 0) {
            *mode = WORDS[k].mode;
            return true;
        }
    }
    return false;
}
