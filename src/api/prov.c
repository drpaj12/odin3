/*
 * prov.c — the public ABI's provenance group: an object's source locations (backward) and the
 * objects of a source line (forward), over src/ir/prov.h.
 */
#include "api/api.h"

#include "backends/common/prov_export.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The provenance record of ref in module (a module's own for ODIN3_OBJ_MODULE). */
static odin3_prov_id obj_prov(const odin3_module *module, odin3_objref ref) {
    switch (ref.kind) {
    case ODIN3_OBJ_NODE:
        return odin3_node_prov(module, (odin3_node_id){ref.id});
    case ODIN3_OBJ_NET:
        return odin3_net_prov(module, (odin3_net_id){ref.id});
    case ODIN3_OBJ_WIRE:
        return odin3_wire_prov(module, (odin3_wire_id){ref.id});
    default:
        return odin3_module_prov(module);
    }
}

/* What the backward walk hands each leaf record to. */
typedef struct source_walk {
    const odin3_design *design;
    odin3_source_visit visit;
    void *user;
} source_walk;

/* Visits every location of one SOURCE or IMPORTED record. */
static void visit_leaf(void *user, odin3_prov_id leaf) {
    const source_walk *walk = user;
    const odin3_prov_record *rec = odin3_prov_get(walk->design, leaf);
    for (uint32_t i = 0; rec != NULL && i < rec->n_locs; i++) {
        const odin3_srcloc *loc = &rec->locs[i];
        odin3_source src = {odin3_api_str(walk->design, loc->file),
                            loc->line,
                            loc->col,
                            loc->end_line,
                            loc->end_col,
                            loc->loc};
        walk->visit(&src, walk->user);
    }
}

ODIN3_EXPORT odin3_status odin3_prov_visit_sources(const odin3_design *design, odin3_obj obj,
                                                   odin3_source_visit visit, void *user) {
    odin3_objref ref = {ODIN3_OBJ_MODULE, 0};
    const odin3_module *module = odin3_api_obj(design, obj, &ref);
    if (module == NULL || visit == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_prov_id prov = obj_prov(module, ref);
    if (odin3_prov_get(design, prov) == NULL) {
        return ODIN3_OK; /* no lineage: nothing to visit */
    }
    source_walk walk = {design, visit, user};
    return odin3_prov_sources(design, prov, visit_leaf, &walk);
}

ODIN3_EXPORT odin3_status odin3_prov_visit_objects(const odin3_design *design, const char *file,
                                                   uint32_t line, odin3_prov_object_visit visit,
                                                   void *user) {
    if (design == NULL || file == NULL || visit == NULL) {
        return odin3_api_invalid(__func__);
    }
    uint32_t file_str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(file), &file_str) ||
        file_str == 0) {
        return ODIN3_OK; /* a file the design never named */
    }
    /* odin3_prov_index_build only reads the design (it snapshots it into the index). */
    odin3_prov_index *ix = odin3_prov_index_build((odin3_design *)design);
    if (ix == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_srcloc loc = {file_str, line, 0, 0, 0, 0};
    odin3_prov_hits hits = odin3_prov_index_by_loc(ix, loc);
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        odin3_prov_object found = {{hit->module.v, (uint32_t)hit->obj.kind, hit->obj.id},
                                   hit->live};
        visit(&found, user);
    }
    odin3_prov_index_destroy(ix);
    return ODIN3_OK;
}

/* --- ABI v7: construct table and chain (docs/V1.2.0.md 4.3) -------------------------------- */

static void copy_construct(const odin3_design *design, uint32_t index,
                           const odin3_construct_row *row, odin3_construct *out) {
    *out = (odin3_construct){index,
                             row->parent,
                             row->kind,
                             (odin3_level)row->level,
                             odin3_api_str(design, row->file),
                             row->line,
                             row->col,
                             row->end_line,
                             row->end_col,
                             odin3_api_str(design, row->name)};
}

ODIN3_EXPORT uint32_t odin3_design_construct_count(const odin3_design *design) {
    return design != NULL ? odin3_construct_end(design) : 0;
}

ODIN3_EXPORT odin3_status odin3_design_get_construct(const odin3_design *design, uint32_t index,
                                                     odin3_construct *out) {
    const odin3_construct_row *row = odin3_construct_get(design, index);
    if (row == NULL || out == NULL) {
        return odin3_api_invalid(__func__);
    }
    copy_construct(design, index, row, out);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_obj_get_prov(const odin3_design *design, odin3_obj obj,
                                             uint32_t *record) {
    odin3_objref ref = {ODIN3_OBJ_MODULE, 0};
    const odin3_module *module = odin3_api_obj(design, obj, &ref);
    if (module == NULL || record == NULL) {
        return odin3_api_invalid(__func__);
    }
    *record = obj_prov(module, ref).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_prov_get_info(const odin3_design *design, uint32_t record,
                                              odin3_prov_info *out) {
    if (design == NULL || out == NULL || odin3_prov_get(design, (odin3_prov_id){record}) == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_prov_label label;
    odin3_prov_label_init(&label);
    odin3_status st = odin3_prov_label_compute(design, record, &label);
    if (st == ODIN3_OK) {
        *out = label.info;
    }
    odin3_prov_label_free(&label);
    return st;
}

ODIN3_EXPORT odin3_status odin3_prov_visit_parents(const odin3_design *design, uint32_t record,
                                                   odin3_u32_visit visit, void *user) {
    const odin3_prov_record *rec =
        design != NULL ? odin3_prov_get(design, (odin3_prov_id){record}) : NULL;
    if (rec == NULL || visit == NULL) {
        return odin3_api_invalid(__func__);
    }
    for (uint32_t i = 0; i < rec->parents.count; i++) {
        visit(rec->parents.ids[i].v, user);
    }
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_prov_visit_joined(const odin3_design *design, uint32_t record,
                                                  odin3_u32_visit visit, void *user) {
    if (design == NULL || visit == NULL ||
        odin3_prov_get(design, (odin3_prov_id){record}) == NULL) {
        return odin3_api_invalid(__func__);
    }
    uint32_t rows[ODIN3_PROV_JOIN_CAP];
    odin3_prov_label label;
    odin3_prov_label_init(&label);
    odin3_status st = odin3_prov_label_compute(design, record, &label);
    uint32_t count = 0;
    for (; st == ODIN3_OK && count < label.joined.len && count < ODIN3_PROV_JOIN_CAP; count++) {
        rows[count] = *(const uint32_t *)odin3_vec_cat(&label.joined, count);
    }
    odin3_prov_label_free(&label);
    for (uint32_t i = 0; i < count; i++) {
        visit(rows[i], user);
    }
    return st;
}

/* A construct reached by the chain walk and the leaf that reached it first. */
typedef struct chain_hit {
    uint32_t row;
    uint32_t leaf;
} chain_hit;

typedef struct chain_walk {
    const odin3_design *design;
    odin3_vec leaves; /* uint32_t record IDs, in visit order */
    odin3_vec hits;   /* chain_hit */
    odin3_u64map *reported;
    odin3_status status;
} chain_walk;

static void chain_leaf(void *user, odin3_prov_id leaf) {
    chain_walk *walk = user;
    uint32_t *slot = walk->status == ODIN3_OK ? odin3_vec_push(&walk->leaves) : NULL;
    if (slot == NULL) {
        walk->status = ODIN3_ERR_NO_MEMORY;
        return;
    }
    *slot = leaf.v;
}

/* Reports row and its ancestors (module last) not yet reported, each with leaf. */
static odin3_status chain_up(chain_walk *walk, uint32_t row, uint32_t leaf) {
    uint64_t seen = 0;
    /* parent is an earlier row (construct_add, check rule 12): the walk ends at row 0; the bound
     * keeps a corrupt table from looping. */
    const odin3_construct_row *cur = odin3_construct_get(walk->design, row);
    for (uint32_t steps = odin3_construct_end(walk->design); cur != NULL && steps > 0;
         steps--, row = cur->parent, cur = odin3_construct_get(walk->design, row)) {
        if (odin3_u64map_get(walk->reported, row, &seen)) {
            continue;
        }
        chain_hit *hit = odin3_vec_push(&walk->hits);
        if (hit == NULL || odin3_u64map_put(walk->reported, (odin3_kv){row, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *hit = (chain_hit){row, leaf};
    }
    return ODIN3_OK;
}

static odin3_status chain_collect(chain_walk *walk, odin3_prov_id start) {
    odin3_status st = odin3_prov_sources(walk->design, start, chain_leaf, walk);
    st = st != ODIN3_OK ? st : walk->status;
    for (size_t i = 0; st == ODIN3_OK && i < walk->leaves.len; i++) {
        uint32_t leaf = *(const uint32_t *)odin3_vec_cat(&walk->leaves, i);
        uint32_t row = odin3_construct_of_record(walk->design, (odin3_prov_id){leaf});
        st = chain_up(walk, row, leaf);
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_prov_visit_chain(const odin3_design *design, odin3_obj obj,
                                                 odin3_construct_visit visit, void *user) {
    odin3_objref ref = {ODIN3_OBJ_MODULE, 0};
    const odin3_module *module = odin3_api_obj(design, obj, &ref);
    if (module == NULL || visit == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_prov_id prov = obj_prov(module, ref);
    if (odin3_prov_get(design, prov) == NULL) {
        return ODIN3_OK; /* no lineage: nothing to visit */
    }
    chain_walk walk = {.design = design, .reported = odin3_u64map_create(0)};
    odin3_vec_init(&walk.leaves, sizeof(uint32_t));
    odin3_vec_init(&walk.hits, sizeof(chain_hit));
    odin3_status st = walk.reported != NULL ? chain_collect(&walk, prov) : ODIN3_ERR_NO_MEMORY;
    for (size_t i = 0; st == ODIN3_OK && i < walk.hits.len; i++) {
        const chain_hit *hit = odin3_vec_cat(&walk.hits, i);
        odin3_construct found;
        copy_construct(design, hit->row, odin3_construct_get(design, hit->row), &found);
        visit(&found, hit->leaf, user);
    }
    odin3_vec_free(&walk.leaves);
    odin3_vec_free(&walk.hits);
    odin3_u64map_destroy(walk.reported);
    return st;
}
