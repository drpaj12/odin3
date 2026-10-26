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
    odin3_vec stack;          /* frame */
    odin3_vec chains;         /* chain_node */
    odin3_vec leaves;         /* leaf_hit */
    odin3_u64map *seen;       /* (record << 32 | chain) */
    odin3_u64map *chain_ids;  /* (parent chain << 32 | record) -> chain ID */
    bool span_only;
} walk_state;

static uint64_t pair_key(uint32_t high, uint32_t low) {
    return ((uint64_t)high << 32) | low;
}

static void walk_free(walk_state *w) {
    odin3_vec_free(&w->stack);
    odin3_vec_free(&w->chains);
    odin3_vec_free(&w->leaves);
    odin3_u64map_destroy(w->seen);
    odin3_u64map_destroy(w->chain_ids);
    w->seen = NULL;
    w->chain_ids = NULL;
}

static odin3_status walk_init(walk_state *w, const odin3_design *design) {
    *w = (walk_state){.design = design};
    odin3_vec_init(&w->stack, sizeof(frame));
    odin3_vec_init(&w->chains, sizeof(chain_node));
    odin3_vec_init(&w->leaves, sizeof(leaf_hit));
    w->seen = odin3_u64map_create(0);
    w->chain_ids = odin3_u64map_create(0);
    if (w->seen == NULL || w->chain_ids == NULL) {
        walk_free(w);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* The chain `parent` extended by context record rec: an existing ID, else a new one. */
static odin3_status chain_extend(walk_state *w, uint32_t parent, uint32_t rec, uint32_t *out) {
    uint64_t have = 0;
    if (odin3_u64map_get(w->chain_ids, pair_key(parent, rec), &have)) {
        *out = (uint32_t)have;
        return ODIN3_OK;
    }
    chain_node *node = odin3_vec_push(&w->chains);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *node = (chain_node){parent, rec};
    uint32_t id = (uint32_t)w->chains.len;
    if (odin3_u64map_put(w->chain_ids, (odin3_kv){pair_key(parent, rec), id}) != ODIN3_OK) {
        odin3_vec_pop(&w->chains);
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = id;
    return ODIN3_OK;
}

static odin3_status push_frame(walk_state *w, uint32_t rec, uint32_t chain) {
    frame *slot = odin3_vec_push(&w->stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (frame){rec, chain};
    return ODIN3_OK;
}

/* A SOURCE or IMPORTED record reached: a construct leaf, or a span-only one. */
static odin3_status take_leaf(walk_state *w, uint32_t rec_id, uint32_t chain) {
    uint32_t row = odin3_construct_of_record(w->design, (odin3_prov_id){rec_id});
    if (row == 0) {
        w->span_only = true;
        return ODIN3_OK;
    }
    leaf_hit *hit = odin3_vec_push(&w->leaves);
    if (hit == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *hit = (leaf_hit){row, chain};
    return ODIN3_OK;
}

/* How many trailing parents of rec are context parents (the declared tail, clamped). */
static uint32_t tail_of(const odin3_design *design, uint32_t rec_id, uint32_t count) {
    odin3_opclass cls;
    if (!odin3_prov_class_of(design, (odin3_prov_id){rec_id}, &cls) ||
        cls.dir != ODIN3_DIR_CONTEXT) {
        return 0;
    }
    return cls.context_tail < count ? cls.context_tail : count;
}

/* A DERIVED record reached with chain: its tail extends the chain, its data parents are pushed. */
static odin3_status expand_derived(walk_state *w, uint32_t rec_id, const odin3_prov_record *rec,
                                   uint32_t chain) {
    uint32_t count = rec->parents.count;
    uint32_t data = count - tail_of(w->design, rec_id, count);
    uint32_t inner = chain;
    for (uint32_t k = data; k < count; k++) {
        odin3_status st = chain_extend(w, inner, rec->parents.ids[k].v, &inner);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    for (uint32_t k = data; k > 0; k--) {
        odin3_status st = push_frame(w, rec->parents.ids[k - 1U].v, inner);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status visit_frame(walk_state *w, frame fr) {
    const odin3_prov_record *rec = odin3_prov_get(w->design, (odin3_prov_id){fr.rec});
    if (rec->kind == ODIN3_PROV_DERIVED) {
        return expand_derived(w, fr.rec, rec, fr.chain);
    }
    return take_leaf(w, fr.rec, fr.chain);
}

/* Walks the data parents of record depth-first, filling the leaves and the span-only flag. */
static odin3_status walk_record(walk_state *w, uint32_t record) {
    odin3_status st = push_frame(w, record, 0);
    while (st == ODIN3_OK && w->stack.len > 0) {
        frame fr = *(const frame *)odin3_vec_cat(&w->stack, w->stack.len - 1U);
        odin3_vec_pop(&w->stack);
        uint64_t unused = 0;
        if (odin3_u64map_get(w->seen, pair_key(fr.rec, fr.chain), &unused)) {
            continue;
        }
        st = odin3_u64map_put(w->seen, (odin3_kv){pair_key(fr.rec, fr.chain), 1});
        if (st == ODIN3_OK) {
            st = visit_frame(w, fr);
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

/* The distinct anchors of the leaves into rows (uint32_t), in first-reach order. */
static odin3_status anchors_of(const walk_state *w, odin3_vec *rows, odin3_u64map *members) {
    for (size_t i = 0; i < w->leaves.len; i++) {
        uint32_t anchor = anchor_of(w->design, ((const leaf_hit *)odin3_vec_cat(&w->leaves, i))->row);
        uint64_t unused = 0;
        if (odin3_u64map_get(members, anchor, &unused)) {
            continue;
        }
        uint32_t *slot = odin3_vec_push(rows);
        if (slot == NULL || odin3_u64map_put(members, (odin3_kv){anchor, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = anchor;
    }
    return ODIN3_OK;
}

static int cmp_u32(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

/* Insertion-free ascending sort of a uint32_t vec (bottom-up merge, scratch of the same size). */
static odin3_status sort_u32(odin3_vec *vec) {
    size_t len = vec->len;
    odin3_vec scratch;
    odin3_vec_init(&scratch, sizeof(uint32_t));
    if (odin3_vec_reserve(&scratch, len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t *src = vec->data;
    uint32_t *dst = scratch.data;
    for (size_t width = 1; width < len; width *= 2) {
        for (size_t lo = 0; lo < len; lo += 2 * width) {
            size_t mid = lo + width < len ? lo + width : len;
            size_t hi = lo + (2 * width) < len ? lo + (2 * width) : len;
            size_t left = lo;
            size_t right = mid;
            for (size_t out = lo; out < hi; out++) {
                bool take_left = right >= hi || (left < mid && cmp_u32(src[left], src[right]) <= 0);
                dst[out] = take_left ? src[left++] : src[right++];
            }
        }
        uint32_t *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != vec->data) {
        memcpy(vec->data, src, len * sizeof(uint32_t));
    }
    odin3_vec_free(&scratch);
    return ODIN3_OK;
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

/* What one record's walk gives: the reduced anchors (ascending row numbers) and two flags. */
typedef struct leafset {
    odin3_vec rows;
    bool span_only;
    bool instance_join;
} leafset;

/* True when an anchor is reached with two different chains. */
static odin3_status find_instance_join(const walk_state *w, bool *found) {
    odin3_u64map *first = odin3_u64map_create(0);
    if (first == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < w->leaves.len && !*found; i++) {
        const leaf_hit *hit = odin3_vec_cat(&w->leaves, i);
        uint32_t anchor = anchor_of(w->design, hit->row);
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

static odin3_status leafset_of(const odin3_design *design, uint32_t record, bool want_instance,
                               leafset *out) {
    walk_state w;
    odin3_status st = walk_init(&w, design);
    odin3_u64map *members = st == ODIN3_OK ? odin3_u64map_create(0) : NULL;
    if (st == ODIN3_OK && members == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        st = walk_record(&w, record);
    }
    if (st == ODIN3_OK) {
        st = anchors_of(&w, &out->rows, members);
    }
    if (st == ODIN3_OK && want_instance) {
        st = find_instance_join(&w, &out->instance_join);
    }
    if (st == ODIN3_OK) {
        st = reduce_rows(design, &out->rows, members);
    }
    out->span_only = w.span_only;
    odin3_u64map_destroy(members);
    walk_free(&w);
    return st;
}

static void leafset_init(leafset *set) {
    *set = (leafset){0};
    odin3_vec_init(&set->rows, sizeof(uint32_t));
}

static bool u32_vecs_equal(const odin3_vec *lhs, const odin3_vec *rhs) {
    return lhs->len == rhs->len &&
           (lhs->len == 0 || memcmp(lhs->data, rhs->data, lhs->len * sizeof(uint32_t)) == 0);
}

/* True when a single data parent of record has the same reduced set J as the record. */
static odin3_status parent_has_same_set(const odin3_design *design, const odin3_prov_record *rec,
                                        uint32_t data, const odin3_vec *rows, bool *same) {
    for (uint32_t k = 0; k < data && !*same; k++) {
        leafset set;
        leafset_init(&set);
        odin3_status st = leafset_of(design, rec->parents.ids[k].v, false, &set);
        *same = st == ODIN3_OK && u32_vecs_equal(&set.rows, rows);
        odin3_vec_free(&set.rows);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
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
    const char *a = odin3_strtab_get(tab, lhs);
    const char *b = odin3_strtab_get(tab, rhs);
    return strcmp(a != NULL ? a : "", b != NULL ? b : "");
}

/* The order of two rows by (file, span, kind, name, part) (3.3). */
static int cmp_row(const odin3_design *design, uint32_t lhs, uint32_t rhs) {
    const odin3_construct_row *a = odin3_construct_get(design, lhs);
    const odin3_construct_row *b = odin3_construct_get(design, rhs);
    int order = cmp_str(design, a->file, b->file);
    if (order == 0) {
        order = cmp_u32(a->line, b->line);
    }
    if (order == 0) {
        order = cmp_u32(a->col, b->col);
    }
    if (order == 0) {
        order = cmp_u32(a->end_line, b->end_line);
    }
    if (order == 0) {
        order = cmp_u32(a->end_col, b->end_col);
    }
    if (order == 0) {
        order = cmp_u32(a->kind, b->kind);
    }
    if (order == 0) {
        order = cmp_str(design, a->name, b->name);
    }
    return order != 0 ? order : cmp_u32(a->part, b->part);
}

/* Chains compared from the module down; a chain that is a prefix of another comes first. */
static int cmp_member(const order_ctx *ctx, const member *lhs, const member *rhs) {
    const uint32_t *a = (const uint32_t *)ctx->path->data + lhs->off;
    const uint32_t *b = (const uint32_t *)ctx->path->data + rhs->off;
    uint32_t common = lhs->len < rhs->len ? lhs->len : rhs->len;
    for (uint32_t i = 0; i < common; i++) {
        int order = cmp_row(ctx->design, a[i], b[i]);
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

/* Sorts members ascending by cmp_member (bottom-up merge; stable). */
static odin3_status sort_members(const order_ctx *ctx, odin3_vec *members) {
    size_t len = members->len;
    odin3_vec scratch;
    odin3_vec_init(&scratch, sizeof(member));
    if (odin3_vec_reserve(&scratch, len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    member *src = members->data;
    member *dst = scratch.data;
    for (size_t width = 1; width < len; width *= 2) {
        for (size_t lo = 0; lo < len; lo += 2 * width) {
            size_t mid = lo + width < len ? lo + width : len;
            size_t hi = lo + (2 * width) < len ? lo + (2 * width) : len;
            size_t left = lo;
            size_t right = mid;
            for (size_t out = lo; out < hi; out++) {
                bool take_left =
                    right >= hi || (left < mid && cmp_member(ctx, &src[left], &src[right]) <= 0);
                dst[out] = take_left ? src[left++] : src[right++];
            }
        }
        member *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != members->data) {
        memcpy(members->data, src, len * sizeof(member));
    }
    odin3_vec_free(&scratch);
    return ODIN3_OK;
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
    if (st == ODIN3_OK) {
        st = sort_members(&ctx, &members);
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
    if (rec->kind != ODIN3_PROV_DERIVED || !odin3_prov_class_of(design, (odin3_prov_id){record}, &cls)) {
        return;
    }
    info->dir = cls.dir;
    info->from_level = cls.from_level;
    info->to_level = cls.to_level;
    info->n_context = tail_of(design, record, rec->parents.count);
    info->flags = cls.bulk ? ODIN3_PROV_F_LOSSY_BULK : 0U;
}

/* The join labels of a coarsen record from its leafset; fills label->joined. */
static odin3_status label_join(const odin3_design *design, const odin3_prov_record *rec,
                               leafset *set, odin3_prov_label *label) {
    odin3_prov_info *info = &label->info;
    bool same = false;
    uint32_t data = rec->parents.count - info->n_context;
    odin3_status st = parent_has_same_set(design, rec, data, &set->rows, &same);
    if (st == ODIN3_OK) {
        st = order_rows(design, &set->rows);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    info->n_joined = (uint32_t)set->rows.len;
    if (set->rows.len >= 2 && !same) {
        info->flags |= ODIN3_PROV_F_JOIN;
    }
    if (set->rows.len > ODIN3_PROV_JOIN_CAP) {
        info->flags |= ODIN3_PROV_F_LOSSY_CAP;
    }
    if (set->instance_join) {
        info->flags |= ODIN3_PROV_F_JOIN_INSTANCE;
    }
    odin3_vec_free(&label->joined);
    label->joined = set->rows;
    odin3_vec_init(&set->rows, sizeof(uint32_t));
    return ODIN3_OK;
}

odin3_status odin3_prov_label_compute(const odin3_design *design, uint32_t record,
                                      odin3_prov_label *label) {
    const odin3_prov_record *rec = design != NULL && label != NULL
                                       ? odin3_prov_get(design, (odin3_prov_id){record})
                                       : NULL;
    if (rec == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_label_compute: invalid argument (record %u)", record);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_label fresh;
    odin3_prov_label_init(&fresh);
    fill_class(design, record, rec, &fresh.info);
    fresh.info.construct = odin3_construct_of_record(design, (odin3_prov_id){record});
    leafset set;
    leafset_init(&set);
    bool coarsen = fresh.info.dir == ODIN3_DIR_COARSEN;
    odin3_status st = leafset_of(design, record, coarsen, &set);
    if (st == ODIN3_OK && set.span_only) {
        fresh.info.flags |= ODIN3_PROV_F_SPAN_ONLY;
    }
    if (st == ODIN3_OK && coarsen) {
        st = label_join(design, rec, &set, &fresh);
    }
    odin3_vec_free(&set.rows);
    if (st != ODIN3_OK) {
        odin3_prov_label_free(&fresh);
        return st;
    }
    odin3_prov_label_free(label);
    *label = fresh;
    return ODIN3_OK;
}
