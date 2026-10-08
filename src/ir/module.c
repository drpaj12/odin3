/* module.c — modules, their object stores and name maps (IR §3, IR-7, IR-14, IR-18). */
#include "ir/module.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ir_internal.h"
#include "ir/pinpool.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* Small chunks keep an empty module cheap; oversize requests get a chunk of their own. */
enum { MODULE_ARENA_CHUNK_BYTES = 2048 };

/* --- stores -------------------------------------------------------------------------------- */

/* A store with the dummy record at index 0; NULL on out of memory. */
static odin3_pagevec *store_create(size_t elem_size) {
    odin3_pagevec_spec spec = {elem_size, ODIN3_MODULE_PAGE_SHIFT};
    odin3_pagevec *store = odin3_pagevec_create_paged(spec);
    if (store != NULL && odin3_pagevec_push(store, NULL) == NULL) {
        odin3_pagevec_destroy(store);
        return NULL;
    }
    return store;
}

odin3_status odin3_module_reserve(odin3_pagevec *store, uint32_t count) {
    if ((uint64_t)odin3_pagevec_len(store) + count > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY; /* ID space exhausted */
    }
    return odin3_pagevec_reserve(store, count);
}

static void *store_at(odin3_pagevec *store, uint32_t idx) {
    return idx != 0 && idx < odin3_pagevec_len(store) ? odin3_pagevec_at(store, idx) : NULL;
}

static const void *store_cat(const odin3_pagevec *store, uint32_t idx) {
    return idx != 0 && idx < odin3_pagevec_len(store) ? odin3_pagevec_cat(store, idx) : NULL;
}

/* --- records ------------------------------------------------------------------------------- */

odin3_node_rec *odin3_node_rec_at(odin3_module *module, odin3_node_id node) {
    return store_at(module->nodes, node.v);
}

const odin3_node_rec *odin3_node_rec_cat(const odin3_module *module, odin3_node_id node) {
    return store_cat(module->nodes, node.v);
}

odin3_pin_rec *odin3_pin_rec_at(odin3_module *module, odin3_pin_id pin) {
    return store_at(module->pins, pin.v);
}

const odin3_pin_rec *odin3_pin_rec_cat(const odin3_module *module, odin3_pin_id pin) {
    return store_cat(module->pins, pin.v);
}

odin3_net_rec *odin3_net_rec_at(odin3_module *module, odin3_net_id net) {
    return store_at(module->nets, net.v);
}

const odin3_net_rec *odin3_net_rec_cat(const odin3_module *module, odin3_net_id net) {
    return store_cat(module->nets, net.v);
}

odin3_node_rec *odin3_node_live_rec(odin3_module *module, odin3_node_id node, const char *what) {
    odin3_node_rec *rec = odin3_node_rec_at(module, node);
    if (rec == NULL || rec->dead) {
        odin3_log(ODIN3_LOG_ERROR, "%s: node %u is not a live node", what, node.v);
        return NULL;
    }
    return rec;
}

odin3_pin_rec *odin3_pin_live_rec(odin3_module *module, odin3_pin_id pin, const char *what) {
    odin3_pin_rec *rec = odin3_pin_rec_at(module, pin);
    if (rec == NULL || !odin3_node_live(module, rec->node)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: pin %u is not a live pin", what, pin.v);
        return NULL;
    }
    return rec;
}

odin3_net_rec *odin3_net_live_rec(odin3_module *module, odin3_net_id net, const char *what) {
    odin3_net_rec *rec = odin3_net_rec_at(module, net);
    if (rec == NULL || rec->dead) {
        odin3_log(ODIN3_LOG_ERROR, "%s: net %u is not a live net", what, net.v);
        return NULL;
    }
    return rec;
}

/* --- names --------------------------------------------------------------------------------- */

bool odin3_names_available(const odin3_module *module, const odin3_name_change *change) {
    if (change->to == 0 || change->to == change->from) {
        return true;
    }
    const odin3_strtab *strtab = odin3_design_strtab(module->design);
    if (change->to >= odin3_strtab_count(strtab)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %u is not a string ID", change->what, change->to);
        return false;
    }
    uint64_t owner = 0;
    if (odin3_u64map_get(change->map, change->to, &owner) && owner != change->id) {
        odin3_log(ODIN3_LOG_ERROR, "%s: name '%s' is already used", change->what,
                  odin3_strtab_get(strtab, change->to));
        return false;
    }
    return true;
}

odin3_status odin3_names_change(const odin3_module *module, const odin3_name_change *change) {
    if (!odin3_names_available(module, change)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (change->to == change->from) {
        return ODIN3_OK;
    }
    if (change->to != 0) {
        odin3_status st = odin3_u64map_put(change->map, (odin3_kv){change->to, change->id});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (change->from != 0) {
        (void)odin3_u64map_remove(change->map, change->from);
    }
    return ODIN3_OK;
}

static uint32_t find_name(const odin3_u64map *map, uint32_t name_str) {
    uint64_t id = 0;
    if (name_str == 0 || !odin3_u64map_get(map, name_str, &id)) {
        return 0;
    }
    return (uint32_t)id;
}

odin3_node_id odin3_module_find_node(const odin3_module *module, uint32_t name_str) {
    return (odin3_node_id){find_name(module->node_names, name_str)};
}

odin3_net_id odin3_module_find_net(const odin3_module *module, uint32_t name_str) {
    return (odin3_net_id){find_name(module->net_names, name_str)};
}

/* --- module lifetime ----------------------------------------------------------------------- */

static void module_destroy(odin3_module *module) {
    if (module == NULL) {
        return;
    }
    odin3_pagevec_destroy(module->nodes);
    odin3_pagevec_destroy(module->pins);
    odin3_pagevec_destroy(module->nets);
    odin3_u64map_destroy(module->node_names);
    odin3_u64map_destroy(module->net_names);
    odin3_pinpool_destroy(&module->pinpool);
    odin3_arena_destroy(module->arena);
    odin3_util_free(module);
}

/* An empty module with every store and map allocated; NULL on out of memory. */
static odin3_module *module_alloc(odin3_design *design) {
    odin3_module *module = odin3_util_calloc(sizeof *module);
    if (module == NULL) {
        return NULL;
    }
    module->design = design;
    module->arena = odin3_arena_create(MODULE_ARENA_CHUNK_BYTES);
    module->node_names = odin3_u64map_create(0);
    module->net_names = odin3_u64map_create(0);
    module->nodes = store_create(sizeof(odin3_node_rec));
    module->pins = store_create(sizeof(odin3_pin_rec));
    module->nets = store_create(sizeof(odin3_net_rec));
    if (module->arena == NULL || module->node_names == NULL || module->net_names == NULL ||
        module->nodes == NULL || module->pins == NULL || module->nets == NULL ||
        odin3_pinpool_init(&module->pinpool) != ODIN3_OK) {
        module_destroy(module);
        return NULL;
    }
    return module;
}

odin3_status odin3_module_table_init(odin3_design *design) {
    odin3_vec_init(&design->modules, sizeof(odin3_module *));
    return odin3_vec_push(&design->modules) != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY; /* slot 0 */
}

void odin3_module_table_free(odin3_design *design) {
    for (size_t i = 1; i < design->modules.len; i++) {
        odin3_module *const *slot = (odin3_module *const *)odin3_vec_cat(&design->modules, i);
        module_destroy(*slot);
    }
    odin3_vec_free(&design->modules);
}

static bool module_name_ok(const odin3_design *design, uint32_t name_str) {
    const odin3_strtab *strtab = odin3_design_strtab(design);
    if (name_str == 0 || name_str >= odin3_strtab_count(strtab)) {
        odin3_log(ODIN3_LOG_ERROR, "module_create: %u is not a non-empty string ID", name_str);
        return false;
    }
    if (odin3_celltype_find(design, name_str, NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "module_create: '%s' is already a cell type",
                  odin3_strtab_get(strtab, name_str));
        return false;
    }
    return true;
}

odin3_status odin3_module_create(odin3_design *design, uint32_t name_str, odin3_prov_id prov,
                                 odin3_module_id *out) {
    if (!module_name_ok(design, name_str)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t idx = design->modules.len;
    if (idx >= UINT32_MAX || odin3_vec_reserve(&design->modules, idx + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_module *module = module_alloc(design);
    if (module == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    /* IR-7: the module's cell type; module_add_port rebuilds its ports. */
    odin3_celltype_def def = {0};
    def.name = odin3_strtab_get(odin3_design_strtab(design), name_str);
    def.gran = ODIN3_GRAN_MODULE;
    odin3_status st = odin3_celltype_add_local(design, &def, &module->type);
    if (st != ODIN3_OK) {
        module_destroy(module);
        return st;
    }
    odin3_module **slot = (odin3_module **)odin3_vec_push(&design->modules);
    assert(slot != NULL); /* reserved above */
    *slot = module;
    module->id.v = (uint32_t)idx;
    module->name = name_str;
    module->prov = prov;
    if (out != NULL) {
        *out = module->id;
    }
    return ODIN3_OK;
}

/* --- module accessors ---------------------------------------------------------------------- */

odin3_module *odin3_module_get(odin3_design *design, odin3_module_id id) {
    if (id.v == 0 || id.v >= design->modules.len) {
        return NULL;
    }
    odin3_module **slot = (odin3_module **)odin3_vec_at(&design->modules, id.v);
    return *slot;
}

uint32_t odin3_design_module_end(const odin3_design *design) {
    return (uint32_t)design->modules.len;
}

odin3_design *odin3_module_design(const odin3_module *module) {
    return module->design;
}

odin3_module_id odin3_module_id_of(const odin3_module *module) {
    return module->id;
}

uint32_t odin3_module_name(const odin3_module *module) {
    return module->name;
}

odin3_prov_id odin3_module_prov(const odin3_module *module) {
    return module->prov;
}

odin3_celltype_id odin3_module_type(const odin3_module *module) {
    return module->type;
}

uint32_t odin3_module_node_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->nodes);
}

uint32_t odin3_module_pin_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->pins);
}

uint32_t odin3_module_net_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->nets);
}
