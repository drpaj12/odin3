/* prov.c — provenance lineage: records, pass runs, hash-consing, navigation, tombstones. */
#include "ir/prov.h"

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
    if (ctx == NULL || pass_name_str == 0 || !is_str(design, pass_name_str)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pass_run_begin: no context or %u is not a pass name",
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
    odin3_prov_store *store = design->prov; /* walk scratch only (IR-17: one thread) */
    if (record_cat(store, id.v) == NULL || visit == NULL) {
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

static bool tombstone_valid(const odin3_design *design, const odin3_tombstone *tomb) {
    if (tomb == NULL || tomb->module.v == 0 || tomb->module.v >= odin3_design_module_end(design)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: no tombstone or unknown module");
        return false;
    }
    if (tomb->kind != ODIN3_OBJ_NODE && tomb->kind != ODIN3_OBJ_NET &&
        tomb->kind != ODIN3_OBJ_WIRE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: kind %d is not a node, net or wire",
                  (int)tomb->kind);
        return false;
    }
    if (tomb->prov.v != 0 && record_cat(design->prov, tomb->prov.v) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_tombstone_add: prov %u is not a record", tomb->prov.v);
        return false;
    }
    return true;
}

odin3_status odin3_tombstone_add(odin3_design *design, const odin3_tombstone *tomb) {
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
 * Compressed rows: row r's hits are hits[first[r] .. first[r + 1]). carried holds the objects
 * grouped by the record they carry; rec_* gives, per record, the objects whose ancestry includes
 * it; loc_* gives, per location key, the objects with that file and line among their leaves.
 */
struct odin3_prov_index {
    odin3_vec carried_first; /* uint32_t, records + 1 */
    odin3_vec carried;       /* odin3_prov_hit */
    odin3_vec rec_first;     /* uint32_t, records + 1 */
    odin3_vec rec_hits;      /* odin3_prov_hit */
    odin3_u64map *loc_keys;  /* file << 32 | line -> row of loc_first */
    odin3_vec loc_first;     /* uint32_t, keys + 1 */
    odin3_vec loc_hits;      /* odin3_prov_hit */
};

/* A swept object and the record it carries. */
typedef struct swept_obj {
    odin3_prov_hit hit;
    uint32_t prov;
} swept_obj;

/* Row `row` of a compressed table receives the objects carrying record `rec`. */
typedef struct row_pair {
    uint32_t row;
    uint32_t rec;
} row_pair;

typedef struct index_build {
    odin3_prov_index *ix;
    odin3_prov_store *store;
    odin3_vec objs;       /* swept_obj in sweep order */
    odin3_vec rec_pairs;  /* row_pair: ancestor <- carried record */
    odin3_vec loc_pairs;  /* row_pair: location key <- carried record */
    odin3_vec key_stamps; /* uint32_t per location key: the last carried record that added it */
    uint32_t current;     /* the carried record being walked */
} index_build;

/* Grows vec to count zeroed elements (it holds at most count). */
static odin3_status zero_fill(odin3_vec *vec, size_t count) {
    if (odin3_vec_reserve(vec, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (vec->len < count) {
        (void)odin3_vec_push(vec); /* reserved */
    }
    return ODIN3_OK;
}

static odin3_status sweep_add(index_build *build, odin3_prov_hit hit, odin3_prov_id prov) {
    if (record_cat(build->store, prov.v) == NULL) {
        return ODIN3_OK; /* no lineage (0) or a bad ID (check rule 7 reports it) */
    }
    swept_obj *slot = odin3_vec_push(&build->objs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->hit = hit;
    slot->prov = prov.v;
    return ODIN3_OK;
}

static odin3_prov_hit hit_of(odin3_module_id module, odin3_objref obj, bool live) {
    odin3_prov_hit hit = {module, obj, live, 0};
    return hit;
}

/* The module itself, then its nodes, nets and wires in ID order, live and dead (IR-6). */
static odin3_status sweep_module(index_build *build, const odin3_module *module) {
    odin3_module_id mid = odin3_module_id_of(module);
    odin3_objref self = {ODIN3_OBJ_MODULE, mid.v};
    odin3_status status = sweep_add(build, hit_of(mid, self, true), odin3_module_prov(module));
    for (uint32_t i = 1; status == ODIN3_OK && i < odin3_module_node_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_NODE, i};
        odin3_node_id node = {i};
        status = sweep_add(build, hit_of(mid, obj, odin3_node_live(module, node)),
                           odin3_node_prov(module, node));
    }
    for (uint32_t i = 1; status == ODIN3_OK && i < odin3_module_net_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_NET, i};
        odin3_net_id net = {i};
        status = sweep_add(build, hit_of(mid, obj, odin3_net_live(module, net)),
                           odin3_net_prov(module, net));
    }
    for (uint32_t i = 1; status == ODIN3_OK && i < odin3_module_wire_end(module); i++) {
        odin3_objref obj = {ODIN3_OBJ_WIRE, i};
        odin3_wire_id wire = {i};
        status = sweep_add(build, hit_of(mid, obj, odin3_wire_live(module, wire)),
                           odin3_wire_prov(module, wire));
    }
    return status;
}

static odin3_status sweep_design(index_build *build, odin3_design *design) {
    odin3_status status = ODIN3_OK;
    for (uint32_t i = 1; status == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        status = sweep_module(build, odin3_module_get(design, (odin3_module_id){i}));
    }
    const odin3_vec *tombstones = &build->store->tombstones;
    for (uint32_t i = 1; status == ODIN3_OK && i < tombstones->len; i++) {
        const odin3_tombstone *tomb = odin3_vec_cat(tombstones, i);
        odin3_objref obj = {tomb->kind, 0};
        odin3_prov_hit hit = {tomb->module, obj, false, i};
        status = sweep_add(build, hit, tomb->prov);
    }
    return status;
}

static uint32_t row_start(const odin3_vec *first, uint32_t row) {
    return *(const uint32_t *)odin3_vec_cat(first, row);
}

static uint32_t row_count(const odin3_vec *first, uint32_t row) {
    return row_start(first, row + 1) - row_start(first, row);
}

/* Groups the swept objects by record (stable: sweep order within a record). */
static odin3_status group_by_record(index_build *build) {
    odin3_prov_index *ix = build->ix;
    size_t count = build->objs.len;
    if (count > UINT32_MAX ||
        zero_fill(&ix->carried_first, (size_t)record_end(build->store) + 1) != ODIN3_OK ||
        zero_fill(&ix->carried, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t *first = ix->carried_first.data;
    const swept_obj *objs = build->objs.data;
    for (size_t i = 0; i < count; i++) {
        first[objs[i].prov]++;
    }
    uint32_t sum = 0;
    for (size_t row = 0; row < ix->carried_first.len; row++) {
        sum += first[row];
        first[row] = sum; /* end of the row; the fill below moves it to the start */
    }
    odin3_prov_hit *carried = ix->carried.data;
    for (size_t i = count; i > 0; i--) {
        carried[--first[objs[i - 1].prov]] = objs[i - 1].hit;
    }
    return ODIN3_OK;
}

static odin3_status push_pair(odin3_vec *pairs, row_pair pair) {
    row_pair *slot = odin3_vec_push(pairs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = pair;
    return ODIN3_OK;
}

/* The row of a location (file and line), added on first sight; one pair per carried record. */
static odin3_status index_loc(index_build *build, const odin3_srcloc *loc) {
    uint64_t key = (uint64_t)loc->file << LOC_KEY_SHIFT | loc->line; /* a u64map key */
    uint64_t row = 0;
    if (!odin3_u64map_get(build->ix->loc_keys, key, &row)) {
        row = build->key_stamps.len;
        odin3_kv entry = {key, row};
        if (row >= UINT32_MAX || odin3_vec_push(&build->key_stamps) == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (odin3_u64map_put(build->ix->loc_keys, entry) != ODIN3_OK) {
            odin3_vec_pop(&build->key_stamps);
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    uint32_t *stamp = odin3_vec_at(&build->key_stamps, row);
    if (*stamp == build->current) {
        return ODIN3_OK;
    }
    *stamp = build->current;
    row_pair pair = {(uint32_t)row, build->current};
    return push_pair(&build->loc_pairs, pair);
}

/* walk_visit: ancestor id of the carried record (and, for a leaf, its locations). */
static odin3_status index_visit(void *user, uint32_t id) {
    index_build *build = user;
    row_pair pair = {id, build->current};
    odin3_status status = push_pair(&build->rec_pairs, pair);
    const odin3_prov_record *rec = record_cat(build->store, id);
    if (rec->kind == ODIN3_PROV_DERIVED) {
        return status;
    }
    for (uint32_t i = 0; status == ODIN3_OK && i < rec->n_locs; i++) {
        status = index_loc(build, &rec->locs[i]);
    }
    return status;
}

/* Walks each carried record once (the memo: objects sharing a record share its walk). */
static odin3_status walk_carried(index_build *build) {
    odin3_status status = ODIN3_OK;
    uint32_t end = record_end(build->store);
    for (uint32_t rec = 1; status == ODIN3_OK && rec < end; rec++) {
        if (row_count(&build->ix->carried_first, rec) > 0) {
            build->current = rec;
            status = walk(build->store, rec, index_visit, build);
        }
    }
    return status;
}

/* A compressed table to fill from pairs: rows rows. */
typedef struct row_table {
    odin3_vec *first;
    odin3_vec *hits;
    uint32_t rows;
} row_table;

/* Fills the table: row r gets, pair by pair in order, the objects carrying the pair's record. */
static odin3_status table_fill(const index_build *build, const odin3_vec *pairs, row_table out) {
    const odin3_vec *carried_first = &build->ix->carried_first;
    const row_pair *list = pairs->data;
    if (zero_fill(out.first, (size_t)out.rows + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t *first = out.first->data;
    uint64_t total = 0;
    for (size_t i = 0; i < pairs->len; i++) {
        uint32_t count = row_count(carried_first, list[i].rec);
        first[list[i].row] += count;
        total += count;
    }
    if (total > UINT32_MAX || zero_fill(out.hits, (size_t)total) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t sum = 0;
    for (uint32_t row = 0; row <= out.rows; row++) {
        sum += first[row];
        first[row] = sum;
    }
    const odin3_prov_hit *carried = build->ix->carried.data;
    odin3_prov_hit *hits = out.hits->data;
    for (size_t i = pairs->len; i > 0; i--) {
        const row_pair *pair = &list[i - 1];
        uint32_t start = row_start(carried_first, pair->rec);
        for (uint32_t k = row_count(carried_first, pair->rec); k > 0; k--) {
            hits[--first[pair->row]] = carried[start + k - 1];
        }
    }
    return ODIN3_OK;
}

static odin3_prov_index *index_new(void) {
    odin3_prov_index *ix = odin3_util_calloc(sizeof *ix);
    if (ix == NULL) {
        return NULL;
    }
    odin3_vec_init(&ix->carried_first, sizeof(uint32_t));
    odin3_vec_init(&ix->carried, sizeof(odin3_prov_hit));
    odin3_vec_init(&ix->rec_first, sizeof(uint32_t));
    odin3_vec_init(&ix->rec_hits, sizeof(odin3_prov_hit));
    odin3_vec_init(&ix->loc_first, sizeof(uint32_t));
    odin3_vec_init(&ix->loc_hits, sizeof(odin3_prov_hit));
    ix->loc_keys = odin3_u64map_create(0);
    if (ix->loc_keys == NULL) {
        odin3_util_free(ix);
        return NULL;
    }
    return ix;
}

static odin3_status index_fill(index_build *build, odin3_design *design) {
    odin3_status status = sweep_design(build, design);
    if (status == ODIN3_OK) {
        status = group_by_record(build);
    }
    if (status == ODIN3_OK) {
        status = walk_carried(build);
    }
    if (status == ODIN3_OK) {
        row_table recs = {&build->ix->rec_first, &build->ix->rec_hits, record_end(build->store)};
        status = table_fill(build, &build->rec_pairs, recs);
    }
    if (status == ODIN3_OK) {
        row_table locs = {&build->ix->loc_first, &build->ix->loc_hits,
                          (uint32_t)build->key_stamps.len};
        status = table_fill(build, &build->loc_pairs, locs);
    }
    return status;
}

odin3_prov_index *odin3_prov_index_build(odin3_design *design) {
    index_build build = {index_new(), design->prov, {0}, {0}, {0}, {0}, 0};
    if (build.ix == NULL) {
        return NULL;
    }
    odin3_vec_init(&build.objs, sizeof(swept_obj));
    odin3_vec_init(&build.rec_pairs, sizeof(row_pair));
    odin3_vec_init(&build.loc_pairs, sizeof(row_pair));
    odin3_vec_init(&build.key_stamps, sizeof(uint32_t));
    odin3_status status = index_fill(&build, design);
    odin3_vec_free(&build.objs);
    odin3_vec_free(&build.rec_pairs);
    odin3_vec_free(&build.loc_pairs);
    odin3_vec_free(&build.key_stamps);
    if (status != ODIN3_OK) {
        odin3_prov_index_destroy(build.ix);
        return NULL;
    }
    return build.ix;
}

static odin3_prov_hits table_row(const odin3_vec *first, const odin3_prov_hit *hits, uint64_t row) {
    odin3_prov_hits out = {NULL, 0};
    if (row + 1 < first->len) {
        uint32_t row32 = (uint32_t)row;
        out.count = row_count(first, row32);
        out.hits = out.count > 0 ? hits + row_start(first, row32) : NULL;
    }
    return out;
}

odin3_prov_hits odin3_prov_index_by_loc(const odin3_prov_index *ix, odin3_srcloc loc) {
    uint64_t key = (uint64_t)loc.file << LOC_KEY_SHIFT | loc.line;
    uint64_t row = 0;
    if (!odin3_u64map_get(ix->loc_keys, key, &row)) {
        odin3_prov_hits none = {NULL, 0};
        return none;
    }
    return table_row(&ix->loc_first, ix->loc_hits.data, row);
}

odin3_prov_hits odin3_prov_index_by_record(const odin3_prov_index *ix, odin3_prov_id rec) {
    return table_row(&ix->rec_first, ix->rec_hits.data, rec.v);
}

void odin3_prov_index_destroy(odin3_prov_index *ix) {
    if (ix == NULL) {
        return;
    }
    odin3_vec_free(&ix->carried_first);
    odin3_vec_free(&ix->carried);
    odin3_vec_free(&ix->rec_first);
    odin3_vec_free(&ix->rec_hits);
    odin3_u64map_destroy(ix->loc_keys);
    odin3_vec_free(&ix->loc_first);
    odin3_vec_free(&ix->loc_hits);
    odin3_util_free(ix);
}
