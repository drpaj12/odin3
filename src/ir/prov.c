/* prov.c — provenance lineage: records, pass runs, hash-consing, navigation, tombstones. */
#include "ir/prov.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/idindex.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 1024 records (~56 KiB) per page of the record store. */
enum { PROV_PAGE_SHIFT = 10 };

/* Bits a file ID is shifted by in a forward-index location key (file << 32 | line). */
enum { LOC_KEY_SHIFT = 32 };

/* --- the store ----------------------------------------------------------------------------- */

odin3_status odin3_prov_store_init(odin3_design *design) {
    odin3_prov_store *store = odin3_util_calloc(sizeof *store);
    if (store == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    design->prov = store; /* odin3_prov_store_free cleans up a partial store */
    odin3_vec_init(&store->runs, sizeof(uint32_t));
    odin3_vec_init(&store->tombstones, sizeof(odin3_tombstone));
    odin3_vec_init(&store->marks, sizeof(uint32_t));
    odin3_vec_init(&store->stack, sizeof(uint32_t));
    odin3_vec_init(&store->scratch, sizeof(odin3_prov_id));
    odin3_pagevec_spec spec = {sizeof(odin3_prov_record), PROV_PAGE_SHIFT};
    store->records = odin3_pagevec_create_paged(spec);
    store->arena = odin3_arena_create(0);
    store->index = odin3_idindex_create(0);
    if (store->records == NULL || store->arena == NULL || store->index == NULL ||
        odin3_pagevec_push(store->records, NULL) == NULL || odin3_vec_push(&store->runs) == NULL ||
        odin3_vec_push(&store->tombstones) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

void odin3_prov_store_free(odin3_design *design) {
    odin3_prov_store *store = design->prov;
    if (store == NULL) {
        return;
    }
    odin3_pagevec_destroy(store->records);
    odin3_arena_destroy(store->arena);
    odin3_idindex_destroy(store->index);
    odin3_vec_free(&store->runs);
    odin3_vec_free(&store->tombstones);
    odin3_vec_free(&store->marks);
    odin3_vec_free(&store->stack);
    odin3_vec_free(&store->scratch);
    odin3_util_free(store);
    design->prov = NULL;
}

static uint32_t record_end(const odin3_prov_store *store) {
    return (uint32_t)odin3_pagevec_len(store->records);
}

static const odin3_prov_record *record_cat(const odin3_prov_store *store, uint32_t id) {
    return id != 0 && id < record_end(store) ? odin3_pagevec_cat(store->records, id) : NULL;
}

static bool is_str(const odin3_design *design, uint32_t str) {
    return str < odin3_strtab_count(odin3_design_strtab(design));
}

/* --- hash-consing -------------------------------------------------------------------------- */

static uint64_t hash_loc(uint64_t hash, const odin3_srcloc *loc) {
    hash = odin3_hash_combine(hash, loc->file);
    hash = odin3_hash_combine(hash, loc->line);
    hash = odin3_hash_combine(hash, loc->col);
    hash = odin3_hash_combine(hash, loc->end_line);
    return odin3_hash_combine(hash, loc->end_col);
}

/* The identity hash: every field, except op for SOURCE and IMPORTED (IR-12). */
static uint64_t record_hash(const odin3_prov_record *rec) {
    uint64_t hash = odin3_hash_combine(ODIN3_HASH_SEED, (uint64_t)rec->kind);
    hash = odin3_hash_combine(hash, rec->run.v);
    if (rec->kind == ODIN3_PROV_DERIVED) {
        hash = odin3_hash_combine(hash, rec->op);
    }
    hash = odin3_hash_combine(hash, rec->ast);
    hash = odin3_hash_combine(hash, rec->hier);
    hash = odin3_hash_combine(hash, rec->n_locs);
    for (uint32_t i = 0; i < rec->n_locs; i++) {
        hash = hash_loc(hash, &rec->locs[i]);
    }
    hash = odin3_hash_combine(hash, rec->parents.count);
    for (uint32_t i = 0; i < rec->parents.count; i++) {
        hash = odin3_hash_combine(hash, rec->parents.ids[i].v);
    }
    return hash;
}

static bool locs_equal(const odin3_srcloc *have, const odin3_srcloc *want, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (have[i].file != want[i].file || have[i].line != want[i].line ||
            have[i].col != want[i].col || have[i].end_line != want[i].end_line ||
            have[i].end_col != want[i].end_col) {
            return false;
        }
    }
    return true;
}

static bool parents_equal(odin3_prov_list have, odin3_prov_list want) {
    if (have.count != want.count) {
        return false;
    }
    for (uint32_t i = 0; i < have.count; i++) {
        if (have.ids[i].v != want.ids[i].v) {
            return false;
        }
    }
    return true;
}

static bool scalars_equal(const odin3_prov_record *have, const odin3_prov_record *want) {
    if (have->kind != want->kind || have->run.v != want->run.v || have->ast != want->ast ||
        have->hier != want->hier || have->n_locs != want->n_locs) {
        return false;
    }
    return want->kind != ODIN3_PROV_DERIVED || have->op == want->op;
}

/* odin3_id_equals: the stored record id against a candidate record (the probe). */
static bool record_equals(const void *ctx, uint32_t id, odin3_bytes probe) {
    const odin3_prov_record *have = record_cat(ctx, id);
    const odin3_prov_record *want = probe.ptr;
    return have != NULL && scalars_equal(have, want) &&
           locs_equal(have->locs, want->locs, want->n_locs) &&
           parents_equal(have->parents, want->parents);
}

/* Points rec's arrays at copies in the store's arena. */
static odin3_status copy_arrays(odin3_prov_store *store, odin3_prov_record *rec) {
    if (rec->n_locs > 0) {
        size_t bytes = sizeof(odin3_srcloc) * rec->n_locs;
        odin3_srcloc *locs = odin3_arena_alloc(store->arena, bytes);
        if (locs == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memcpy(locs, rec->locs, bytes);
        rec->locs = locs;
    }
    if (rec->parents.count > 0) {
        size_t bytes = sizeof(odin3_prov_id) * rec->parents.count;
        odin3_prov_id *ids = odin3_arena_alloc(store->arena, bytes);
        if (ids == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memcpy(ids, rec->parents.ids, bytes);
        rec->parents.ids = ids;
    }
    return ODIN3_OK;
}

/* The ID of the record equal to probe, appending it when new. Nothing changes on failure. */
static odin3_status record_intern(odin3_prov_store *store, const odin3_prov_record *probe,
                                  odin3_prov_id *out) {
    uint64_t hash = record_hash(probe);
    odin3_idcmp cmp = {hash, {probe, sizeof *probe}, record_equals, store};
    uint32_t id = 0;
    if (odin3_idindex_find(store->index, &cmp, &id)) {
        out->v = id;
        return ODIN3_OK;
    }
    uint32_t next = record_end(store);
    if (next == UINT32_MAX || odin3_pagevec_reserve(store->records, 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_prov_record rec = *probe;
    odin3_status status = copy_arrays(store, &rec); /* arena bytes of a failure stay unused */
    if (status == ODIN3_OK) {
        odin3_identry entry = {hash, next};
        status = odin3_idindex_insert(store->index, entry);
    }
    if (status != ODIN3_OK) {
        return status;
    }
    odin3_prov_record *slot = odin3_pagevec_push(store->records, NULL); /* reserved */
    assert(slot != NULL);
    *slot = rec;
    out->v = next;
    return ODIN3_OK;
}

/* --- pass runs and operations -------------------------------------------------------------- */

odin3_status odin3_pass_run_begin(odin3_design *design, uint32_t pass_name_str,
                                  odin3_pass_ctx *ctx) {
    if (design == NULL || ctx == NULL || pass_name_str == 0 || !is_str(design, pass_name_str)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_pass_run_begin: no design, no context or %u is not a pass name",
                  pass_name_str);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_store *store = design->prov;
    if (store->runs.len >= UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t *slot = odin3_vec_push(&store->runs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = pass_name_str;
    ctx->design = design;
    ctx->run.v = (uint32_t)(store->runs.len - 1);
    ctx->op = 0;
    return ODIN3_OK;
}

void odin3_prov_begin_op(odin3_pass_ctx *ctx) {
    assert(ctx->op < UINT32_MAX);
    ctx->op++;
}

uint32_t odin3_passrun_name(const odin3_design *design, odin3_passrun_id run) {
    const odin3_vec *runs = &design->prov->runs;
    return run.v != 0 && run.v < runs->len ? *(const uint32_t *)odin3_vec_cat(runs, run.v) : 0;
}

uint32_t odin3_passrun_end(const odin3_design *design) {
    return (uint32_t)design->prov->runs.len;
}

/* The store of a usable pass context; NULL (logged for `what`) otherwise. */
static odin3_prov_store *ctx_store(const odin3_pass_ctx *ctx, const char *what) {
    if (ctx == NULL || ctx->design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: no pass context", what);
        return NULL;
    }
    odin3_prov_store *store = ctx->design->prov;
    if (ctx->run.v == 0 || ctx->run.v >= store->runs.len) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %u is not a pass run", what, ctx->run.v);
        return NULL;
    }
    return store;
}

/* --- SOURCE and IMPORTED ------------------------------------------------------------------- */

static bool origin_valid(const odin3_design *design, const odin3_prov_origin *origin,
                         const char *what) {
    if (origin == NULL || (origin->n_locs > 0 && origin->locs == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: no origin or no locations", what);
        return false;
    }
    if (!is_str(design, origin->hier)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: hier %u is not a string ID", what, origin->hier);
        return false;
    }
    for (uint32_t i = 0; i < origin->n_locs; i++) {
        if (!is_str(design, origin->locs[i].file)) {
            odin3_log(ODIN3_LOG_ERROR, "%s: file %u is not a string ID", what,
                      origin->locs[i].file);
            return false;
        }
    }
    return true;
}

static odin3_status leaf_record(const odin3_pass_ctx *ctx, const odin3_prov_origin *origin,
                                odin3_prov_kind kind, odin3_prov_id *out) {
    const char *what = kind == ODIN3_PROV_SOURCE ? "odin3_prov_source" : "odin3_prov_imported";
    odin3_prov_store *store = ctx_store(ctx, what);
    if (store == NULL || !origin_valid(ctx->design, origin, what)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_record probe = {kind,           ctx->run,    ctx->op,      origin->locs,
                               origin->n_locs, origin->ast, origin->hier, {NULL, 0}};
    return record_intern(store, &probe, out);
}

odin3_status odin3_prov_source(const odin3_pass_ctx *ctx, const odin3_prov_origin *origin,
                               odin3_prov_id *out) {
    return leaf_record(ctx, origin, ODIN3_PROV_SOURCE, out);
}

odin3_status odin3_prov_imported(const odin3_pass_ctx *ctx, const odin3_prov_origin *origin,
                                 odin3_prov_id *out) {
    return leaf_record(ctx, origin, ODIN3_PROV_IMPORTED, out);
}

/* --- visit marks and the backward walk ----------------------------------------------------- */

/* Starts a new mark generation, with a mark for every record. */
static odin3_status marks_begin(odin3_prov_store *store) {
    size_t end = odin3_pagevec_len(store->records);
    if (store->marks.len < end) {
        if (odin3_vec_reserve(&store->marks, end) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        while (store->marks.len < end) {
            (void)odin3_vec_push(&store->marks); /* reserved; zero is never a generation */
        }
    }
    store->gen++;
    if (store->gen == 0) { /* wrapped: forget every old mark */
        memset(store->marks.data, 0, store->marks.len * sizeof(uint32_t));
        store->gen = 1;
    }
    return ODIN3_OK;
}

static bool marked(const odin3_prov_store *store, uint32_t id) {
    return *(const uint32_t *)odin3_vec_cat(&store->marks, id) == store->gen;
}

static void mark(odin3_prov_store *store, uint32_t id) {
    *(uint32_t *)odin3_vec_at(&store->marks, id) = store->gen;
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

typedef odin3_status (*walk_visit)(void *user, uint32_t id);

/* Depth-first from root over parents in order (an explicit worklist, no recursion); visit runs
 * once per reachable record, root first. */
static odin3_status walk(odin3_prov_store *store, uint32_t root, walk_visit visit, void *user) {
    odin3_vec *stack = &store->stack;
    odin3_vec_clear(stack);
    odin3_status status = marks_begin(store);
    if (status == ODIN3_OK) {
        status = push_u32(stack, root);
    }
    while (status == ODIN3_OK && stack->len > 0) {
        uint32_t id = *(const uint32_t *)odin3_vec_cat(stack, stack->len - 1);
        odin3_vec_pop(stack);
        if (marked(store, id)) {
            continue;
        }
        mark(store, id);
        status = visit(user, id);
        odin3_prov_list parents = record_cat(store, id)->parents;
        for (uint32_t k = parents.count; status == ODIN3_OK && k > 0; k--) {
            if (!marked(store, parents.ids[k - 1].v)) {
                status = push_u32(stack, parents.ids[k - 1].v);
            }
        }
    }
    return status;
}

const odin3_prov_record *odin3_prov_get(const odin3_design *design, odin3_prov_id id) {
    return record_cat(design->prov, id.v);
}

uint32_t odin3_prov_end(const odin3_design *design) {
    return record_end(design->prov);
}

odin3_prov_list odin3_prov_parents(const odin3_design *design, odin3_prov_id id) {
    const odin3_prov_record *rec = record_cat(design->prov, id.v);
    odin3_prov_list none = {NULL, 0};
    return rec != NULL ? rec->parents : none;
}

typedef struct leaf_list {
    const odin3_prov_store *store;
    odin3_vec ids; /* uint32_t */
} leaf_list;

static odin3_status collect_leaf(void *user, uint32_t id) {
    leaf_list *leaves = user;
    if (record_cat(leaves->store, id)->kind == ODIN3_PROV_DERIVED) {
        return ODIN3_OK;
    }
    return push_u32(&leaves->ids, id);
}

odin3_status odin3_prov_sources(const odin3_design *design, odin3_prov_id id,
                                odin3_prov_visit visit, void *user) {
    if (design == NULL || visit == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_sources: no design or no visit callback");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_store *store = design->prov; /* walk scratch only (IR-17: one thread) */
    if (record_cat(store, id.v) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_sources: %u is not a record", id.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    leaf_list leaves = {store, {0}};
    odin3_vec_init(&leaves.ids, sizeof(uint32_t));
    odin3_status status = walk(store, id.v, collect_leaf, &leaves);
    for (size_t i = 0; status == ODIN3_OK && i < leaves.ids.len; i++) {
        odin3_prov_id leaf = {*(const uint32_t *)odin3_vec_cat(&leaves.ids, i)};
        visit(user, leaf); /* after the walk: visit may navigate too */
    }
    odin3_vec_free(&leaves.ids);
    return status;
}

/* --- DERIVED ------------------------------------------------------------------------------- */

static bool parents_valid(const odin3_prov_store *store, odin3_prov_list parents) {
    if (parents.count == 0 || parents.ids == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_derive: a derived record needs parents");
        return false;
    }
    for (uint32_t i = 0; i < parents.count; i++) {
        if (record_cat(store, parents.ids[i].v) == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_prov_derive: parent %u is not a record",
                      parents.ids[i].v);
            return false;
        }
    }
    return true;
}

/* parents without repeats (first occurrences, in order), in the store's scratch vector. */
static odin3_status parents_unique(odin3_prov_store *store, odin3_prov_list parents,
                                   odin3_prov_list *unique) {
    odin3_vec_clear(&store->scratch);
    if (odin3_vec_reserve(&store->scratch, parents.count) != ODIN3_OK ||
        marks_begin(store) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < parents.count; i++) {
        if (!marked(store, parents.ids[i].v)) {
            mark(store, parents.ids[i].v);
            *(odin3_prov_id *)odin3_vec_push(&store->scratch) = parents.ids[i]; /* reserved */
        }
    }
    unique->ids = store->scratch.data;
    unique->count = (uint32_t)store->scratch.len;
    return ODIN3_OK;
}

odin3_status odin3_prov_derive(const odin3_pass_ctx *ctx, odin3_prov_list parents,
                               odin3_prov_id *out) {
    odin3_prov_store *store = ctx_store(ctx, "odin3_prov_derive");
    if (store == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (ctx->op == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_derive: no operation begun (odin3_prov_begin_op)");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!parents_valid(store, parents)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_list unique = parents;
    if (parents.count > 1 && parents_unique(store, parents, &unique) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_prov_record probe = {ODIN3_PROV_DERIVED, ctx->run, ctx->op, NULL, 0, 0, 0, unique};
    return record_intern(store, &probe, out);
}

/* --- tombstones ---------------------------------------------------------------------------- */

/* A node's type must be a cell type of the design; nets and wires have none. */
static bool tombstone_kind_valid(const odin3_design *design, const odin3_tombstone *tomb) {
    switch (tomb->kind) {
    case ODIN3_OBJ_NODE:
        return odin3_celltype_get(design, tomb->type) != NULL;
    case ODIN3_OBJ_NET:
    case ODIN3_OBJ_WIRE:
        return !odin3_celltype_valid(tomb->type);
    default:
        return false;
    }
}

static bool tombstone_valid(const odin3_design *design, const odin3_tombstone *tomb) {
    if (tomb == NULL || tomb->module.v == 0 || tomb->module.v >= odin3_design_module_end(design)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: no tombstone or unknown module");
        return false;
    }
    if (!tombstone_kind_valid(design, tomb)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_tombstone_add: kind %d with type %u is not a node of a known type, "
                  "a net or a wire",
                  (int)tomb->kind, tomb->type.v);
        return false;
    }
    if (tomb->prov.v != 0 && record_cat(design->prov, tomb->prov.v) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: prov %u is not a record", tomb->prov.v);
        return false;
    }
    return true;
}

odin3_status odin3_tombstone_add(odin3_design *design, const odin3_tombstone *tomb) {
    if (design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: no design");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!tombstone_valid(design, tomb)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec *tombstones = &design->prov->tombstones;
    if (tombstones->len >= UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_tombstone *slot = odin3_vec_push(tombstones);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *tomb;
    return ODIN3_OK;
}

const odin3_tombstone *odin3_tombstone_get(const odin3_design *design, uint32_t id) {
    const odin3_vec *tombstones = &design->prov->tombstones;
    return id != 0 && id < tombstones->len ? odin3_vec_cat(tombstones, id) : NULL;
}

uint32_t odin3_tombstone_end(const odin3_design *design) {
    return (uint32_t)design->prov->tombstones.len;
}

/* --- forward index ------------------------------------------------------------------------- */

/*
 * Everything is O(records + parent edges + objects), sized exactly at build. Rows: row r of a
 * (first, items) pair is items[first[r] .. first[r + 1]). Queries walk children breadth-first
 * with generation marks and write into result; they never allocate.
 */
struct odin3_prov_index {
    uint32_t n_records;      /* record IDs 0 .. n_records - 1 at build time */
    uint32_t n_hits;         /* swept objects with lineage */
    uint32_t n_edges;        /* parent edges */
    uint32_t n_keys;         /* distinct (file, line) of leaf locations */
    uint32_t n_key_leaves;   /* (key, leaf) entries */
    uint32_t gen;            /* current query generation */
    uint32_t *carried_first; /* n_records + 1 */
    odin3_prov_hit *carried; /* n_hits: objects by the record they carry, sweep order within */
    uint32_t *child_first;   /* n_records + 1 */
    uint32_t *children;      /* n_edges: child record IDs, ascending per parent */
    uint64_t *keys;          /* n_keys: file << 32 | line, ascending */
    uint32_t *key_first;     /* n_keys + 1 */
    uint32_t *key_leaves;    /* n_key_leaves: leaf record IDs, ascending per key */
    uint32_t *marks;         /* n_records: the query generation that reached the record */
    uint32_t *queue;         /* n_records: query worklist, each record at most once */
    odin3_prov_hit *result;  /* n_hits: the last query's hits */
};

/* A swept object and the record it carries. */
typedef struct swept_obj {
    odin3_prov_hit hit;
    uint32_t prov;
} swept_obj;

/* A leaf location: (file, line) key and the leaf record. */
typedef struct key_leaf {
    uint64_t key;
    uint32_t leaf;
} key_leaf;

/* Index arrays: count elements of size bytes (at least one element), zeroed; NULL on OOM. */
static void *array_new(size_t count, size_t size) {
    return odin3_util_calloc((count > 0 ? count : 1) * size);
}

static size_t array_bytes(size_t count, size_t size) {
    return (count > 0 ? count : 1) * size;
}

static uint64_t loc_key(const odin3_srcloc *loc) {
    return (uint64_t)loc->file << LOC_KEY_SHIFT | loc->line;
}

/* Slots the sweep visits: each module, its nodes, nets and wires, and every tombstone. */
static size_t sweep_slots(odin3_design *design) {
    size_t slots = design->prov->tombstones.len;
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){i});
        slots += 1 + (size_t)odin3_module_node_end(module) + odin3_module_net_end(module) +
                 odin3_module_wire_end(module);
    }
    return slots;
}

typedef struct sweep {
    const odin3_prov_store *store;
    swept_obj *objs; /* room for every slot */
    uint32_t count;
} sweep;

static void sweep_add(sweep *sw, odin3_prov_hit hit, odin3_prov_id prov) {
    if (record_cat(sw->store, prov.v) == NULL) {
        return; /* no lineage (0) or a bad ID (check rule 7 reports it) */
    }
    sw->objs[sw->count].hit = hit;
    sw->objs[sw->count].prov = prov.v;
    sw->count++;
}

static odin3_prov_hit hit_of(odin3_module_id module, odin3_objref obj, bool live) {
    odin3_prov_hit hit = {module, obj, live, 0};
    return hit;
}

/* The module itself, then its nodes, nets and wires in ID order, live and dead (IR-6). */
static void sweep_module(sweep *sw, const odin3_module *module) {
    odin3_module_id mid = odin3_module_id_of(module);
    odin3_objref self = {ODIN3_OBJ_MODULE, mid.v};
    sweep_add(sw, hit_of(mid, self, true), odin3_module_prov(module));
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_NODE, i};
        odin3_node_id node = {i};
        sweep_add(sw, hit_of(mid, obj, odin3_node_live(module, node)),
                  odin3_node_prov(module, node));
    }
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_NET, i};
        odin3_net_id net = {i};
        sweep_add(sw, hit_of(mid, obj, odin3_net_live(module, net)), odin3_net_prov(module, net));
    }
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_WIRE, i};
        odin3_wire_id wire = {i};
        sweep_add(sw, hit_of(mid, obj, odin3_wire_live(module, wire)),
                  odin3_wire_prov(module, wire));
    }
}

static void sweep_design(sweep *sw, odin3_design *design) {
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        sweep_module(sw, odin3_module_get(design, (odin3_module_id){i}));
    }
    const odin3_vec *tombstones = &sw->store->tombstones;
    for (uint32_t i = 1; i < tombstones->len; i++) {
        const odin3_tombstone *tomb = odin3_vec_cat(tombstones, i);
        odin3_objref obj = {tomb->kind, 0};
        odin3_prov_hit hit = {tomb->module, obj, false, i};
        sweep_add(sw, hit, tomb->prov);
    }
}

/* Turns per-row counts in first[0 .. rows) into end offsets (first[rows] = total). */
static void rows_end(uint32_t *first, uint32_t rows) {
    uint32_t sum = 0;
    for (uint32_t row = 0; row <= rows; row++) {
        sum += first[row];
        first[row] = sum;
    }
}

/* Sweeps the design and groups the objects by record (stable: sweep order within a record). */
static odin3_status index_carried(odin3_prov_index *ix, odin3_design *design) {
    size_t slots = sweep_slots(design);
    sweep sw = {design->prov, NULL, 0};
    if (slots <= UINT32_MAX) {
        sw.objs = array_new(slots, sizeof(swept_obj));
    }
    if (sw.objs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sweep_design(&sw, design);
    ix->n_hits = sw.count;
    ix->carried_first = array_new((size_t)ix->n_records + 1, sizeof(uint32_t));
    ix->carried = array_new(ix->n_hits, sizeof(odin3_prov_hit));
    if (ix->carried_first == NULL || ix->carried == NULL) {
        odin3_util_free(sw.objs);
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < sw.count; i++) {
        ix->carried_first[sw.objs[i].prov]++;
    }
    rows_end(ix->carried_first, ix->n_records);
    for (uint32_t i = sw.count; i > 0; i--) {
        ix->carried[--ix->carried_first[sw.objs[i - 1].prov]] = sw.objs[i - 1].hit;
    }
    odin3_util_free(sw.objs);
    return ODIN3_OK;
}

/* The children table: the reverse of every record's parents. */
static odin3_status index_children(odin3_prov_index *ix, const odin3_prov_store *store) {
    uint64_t edges = 0;
    for (uint32_t rec = 1; rec < ix->n_records; rec++) {
        edges += record_cat(store, rec)->parents.count;
    }
    if (edges > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ix->n_edges = (uint32_t)edges;
    ix->child_first = array_new((size_t)ix->n_records + 1, sizeof(uint32_t));
    ix->children = array_new(ix->n_edges, sizeof(uint32_t));
    if (ix->child_first == NULL || ix->children == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t rec = 1; rec < ix->n_records; rec++) {
        odin3_prov_list parents = record_cat(store, rec)->parents;
        for (uint32_t k = 0; k < parents.count; k++) {
            ix->child_first[parents.ids[k].v]++;
        }
    }
    rows_end(ix->child_first, ix->n_records);
    for (uint32_t rec = ix->n_records - 1; rec > 0; rec--) {
        odin3_prov_list parents = record_cat(store, rec)->parents;
        for (uint32_t k = 0; k < parents.count; k++) {
            ix->children[--ix->child_first[parents.ids[k].v]] = rec;
        }
    }
    return ODIN3_OK;
}

static int key_leaf_cmp(const void *lhs, const void *rhs) {
    const key_leaf *one = lhs;
    const key_leaf *other = rhs;
    if (one->key != other->key) {
        return one->key < other->key ? -1 : 1;
    }
    if (one->leaf != other->leaf) {
        return one->leaf < other->leaf ? -1 : 1;
    }
    return 0;
}

/* Every (file, line) of a leaf location with a known file, and the leaf; count in *count. */
static key_leaf *leaf_locations(const odin3_prov_index *ix, const odin3_prov_store *store,
                                uint32_t *count) {
    uint64_t total = 0;
    for (uint32_t rec = 1; rec < ix->n_records; rec++) {
        total += record_cat(store, rec)->n_locs;
    }
    key_leaf *list = total <= UINT32_MAX ? array_new(total, sizeof(key_leaf)) : NULL;
    *count = 0;
    for (uint32_t rec = 1; list != NULL && rec < ix->n_records; rec++) {
        const odin3_prov_record *leaf = record_cat(store, rec);
        for (uint32_t k = 0; k < leaf->n_locs; k++) {
            if (leaf->locs[k].file != 0) {
                list[*count].key = loc_key(&leaf->locs[k]);
                list[(*count)++].leaf = rec;
            }
        }
    }
    return list;
}

/* Lays out sorted, de-duplicated (key, leaf) entries as keys and per-key leaf rows. */
static odin3_status index_keys_fill(odin3_prov_index *ix, const key_leaf *list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        ix->n_keys += i == 0 || list[i].key != list[i - 1].key ? 1U : 0U;
    }
    ix->keys = array_new(ix->n_keys, sizeof(uint64_t));
    ix->key_first = array_new((size_t)ix->n_keys + 1, sizeof(uint32_t));
    ix->key_leaves = array_new(count, sizeof(uint32_t));
    if (ix->keys == NULL || ix->key_first == NULL || ix->key_leaves == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t key = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && list[i].key != list[i - 1].key) {
            key++;
        }
        ix->keys[key] = list[i].key;
        ix->key_leaves[i] = list[i].leaf;
        ix->key_first[key + 1] = i + 1;
    }
    ix->n_key_leaves = count;
    return ODIN3_OK;
}

/* The location table: (file, line) -> the leaf records with such a location. */
static odin3_status index_keys(odin3_prov_index *ix, const odin3_prov_store *store) {
    uint32_t count = 0;
    key_leaf *list = leaf_locations(ix, store, &count);
    if (list == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    qsort(list, count, sizeof *list, key_leaf_cmp);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) { /* one entry per (key, leaf) */
        if (kept == 0 || key_leaf_cmp(&list[kept - 1], &list[i]) != 0) {
            list[kept++] = list[i];
        }
    }
    odin3_status status = index_keys_fill(ix, list, kept);
    odin3_util_free(list);
    return status;
}

static odin3_status index_fill(odin3_prov_index *ix, odin3_design *design) {
    ix->n_records = record_end(design->prov);
    odin3_status status = index_carried(ix, design);
    if (status == ODIN3_OK) {
        status = index_children(ix, design->prov);
    }
    if (status == ODIN3_OK) {
        status = index_keys(ix, design->prov);
    }
    if (status == ODIN3_OK) {
        ix->marks = array_new(ix->n_records, sizeof(uint32_t));
        ix->queue = array_new(ix->n_records, sizeof(uint32_t));
        ix->result = array_new(ix->n_hits, sizeof(odin3_prov_hit));
        if (ix->marks == NULL || ix->queue == NULL || ix->result == NULL) {
            status = ODIN3_ERR_NO_MEMORY;
        }
    }
    return status;
}

odin3_prov_index *odin3_prov_index_build(odin3_design *design) {
    if (design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_prov_index_build: no design");
        return NULL;
    }
    odin3_prov_index *ix = odin3_util_calloc(sizeof *ix);
    if (ix == NULL) {
        return NULL;
    }
    if (index_fill(ix, design) != ODIN3_OK) {
        odin3_prov_index_destroy(ix);
        return NULL;
    }
    return ix;
}

size_t odin3_prov_index_bytes(const odin3_prov_index *ix) {
    size_t rows = (size_t)ix->n_records + 1;
    return sizeof *ix + 2 * array_bytes(rows, sizeof(uint32_t)) +
           2 * array_bytes(ix->n_hits, sizeof(odin3_prov_hit)) +
           array_bytes(ix->n_edges, sizeof(uint32_t)) + array_bytes(ix->n_keys, sizeof(uint64_t)) +
           array_bytes((size_t)ix->n_keys + 1, sizeof(uint32_t)) +
           array_bytes(ix->n_key_leaves, sizeof(uint32_t)) +
           2 * array_bytes(ix->n_records, sizeof(uint32_t));
}

/* --- forward queries ----------------------------------------------------------------------- */

static void query_begin(odin3_prov_index *ix) {
    ix->gen++;
    if (ix->gen == 0) { /* wrapped: forget every old mark */
        memset(ix->marks, 0, array_bytes(ix->n_records, sizeof(uint32_t)));
        ix->gen = 1;
    }
}

/* Queues rec unless this query reached it already; returns the new queue length. */
static uint32_t query_enqueue(odin3_prov_index *ix, uint32_t rec, uint32_t tail) {
    if (ix->marks[rec] == ix->gen) {
        return tail;
    }
    ix->marks[rec] = ix->gen;
    ix->queue[tail] = rec;
    return tail + 1;
}

/* Breadth-first over children from the queued records; the hits of every record reached. */
static odin3_prov_hits query_run(odin3_prov_index *ix, uint32_t tail) {
    for (uint32_t head = 0; head < tail; head++) {
        uint32_t rec = ix->queue[head];
        for (uint32_t k = ix->child_first[rec]; k < ix->child_first[rec + 1]; k++) {
            tail = query_enqueue(ix, ix->children[k], tail);
        }
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < tail; i++) {
        uint32_t rec = ix->queue[i];
        for (uint32_t k = ix->carried_first[rec]; k < ix->carried_first[rec + 1]; k++) {
            ix->result[count++] = ix->carried[k];
        }
    }
    odin3_prov_hits hits = {count > 0 ? ix->result : NULL, count};
    return hits;
}

/* Row of key in the sorted key table, or n_keys when absent. */
static uint32_t key_row(const odin3_prov_index *ix, uint64_t key) {
    uint32_t low = 0;
    uint32_t high = ix->n_keys;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (ix->keys[mid] < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low < ix->n_keys && ix->keys[low] == key ? low : ix->n_keys;
}

odin3_prov_hits odin3_prov_index_by_loc(odin3_prov_index *ix, odin3_srcloc loc) {
    odin3_prov_hits none = {NULL, 0};
    uint32_t row = loc.file != 0 ? key_row(ix, loc_key(&loc)) : ix->n_keys;
    if (row == ix->n_keys) {
        return none;
    }
    query_begin(ix);
    uint32_t tail = 0;
    for (uint32_t k = ix->key_first[row]; k < ix->key_first[row + 1]; k++) {
        tail = query_enqueue(ix, ix->key_leaves[k], tail);
    }
    return query_run(ix, tail);
}

odin3_prov_hits odin3_prov_index_by_record(odin3_prov_index *ix, odin3_prov_id rec) {
    if (rec.v == 0 || rec.v >= ix->n_records) {
        odin3_prov_hits none = {NULL, 0};
        return none;
    }
    query_begin(ix);
    return query_run(ix, query_enqueue(ix, rec.v, 0));
}

void odin3_prov_index_destroy(odin3_prov_index *ix) {
    if (ix == NULL) {
        return;
    }
    odin3_util_free(ix->carried_first);
    odin3_util_free(ix->carried);
    odin3_util_free(ix->child_first);
    odin3_util_free(ix->children);
    odin3_util_free(ix->keys);
    odin3_util_free(ix->key_first);
    odin3_util_free(ix->key_leaves);
    odin3_util_free(ix->marks);
    odin3_util_free(ix->queue);
    odin3_util_free(ix->result);
    odin3_util_free(ix);
}
