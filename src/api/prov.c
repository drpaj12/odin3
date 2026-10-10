/*
 * prov.c — the public ABI's provenance group: an object's source locations (backward) and the
 * objects of a source line (forward), over src/ir/prov.h.
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

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
        odin3_source src = {odin3_api_str(walk->design, loc->file), loc->line, loc->col,
                            loc->end_line, loc->end_col};
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
    odin3_srcloc loc = {file_str, line, 0, 0, 0};
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
