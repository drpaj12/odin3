/* module.c — modules, their object stores, name maps and ports (IR §3, IR-3, IR-7, IR-14). */
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

static const unsigned MODULE_SHIFT = ODIN3_MODULE_PAGE_SHIFT;
static const unsigned WIRE_SHIFT = ODIN3_WIRE_PAGE_SHIFT;

/* Widest port: its wire is [width-1:0] with an int32 msb. */
static const uint32_t MAX_PORT_WIDTH = UINT32_C(1) << 31;

/* --- stores -------------------------------------------------------------------------------- */

/* A store with the dummy record at index 0; NULL on out of memory. */
static odin3_pagevec *store_create(odin3_pagevec_spec spec) {
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

odin3_wire_rec *odin3_wire_rec_at(odin3_module *module, odin3_wire_id wire) {
    return store_at(module->wires, wire.v);
}

const odin3_wire_rec *odin3_wire_rec_cat(const odin3_module *module, odin3_wire_id wire) {
    return store_cat(module->wires, wire.v);
}

odin3_wire_rec *odin3_wire_live_rec(odin3_module *module, odin3_wire_id wire, const char *what) {
    odin3_wire_rec *rec = odin3_wire_rec_at(module, wire);
    if (rec == NULL || rec->dead) {
        odin3_log(ODIN3_LOG_ERROR, "%s: wire %u is not a live wire", what, wire.v);
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

odin3_wire_id odin3_module_find_wire(const odin3_module *module, uint32_t name_str) {
    return (odin3_wire_id){find_name(module->wire_names, name_str)};
}

/* --- module lifetime ----------------------------------------------------------------------- */

void odin3_module_dead_pins_free(odin3_module *module) {
    odin3_u64map_destroy(module->dead_pins);
    module->dead_pins = NULL;
    odin3_vec_free(&module->dead_pin_names);
}

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
    odin3_pagevec_destroy(module->wires);
    odin3_u64map_destroy(module->wire_names);
    odin3_vec_free(&module->aliases);
    odin3_vec_free(&module->ports);
    odin3_vec_free(&module->port_defs);
    odin3_u64map_destroy(module->attr_heads);
    odin3_vec_free(&module->attrs);
    odin3_module_dead_pins_free(module);
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
    odin3_vec_init(&module->aliases, sizeof(odin3_alias_rec));
    odin3_vec_init(&module->ports, sizeof(odin3_port_rec));
    odin3_vec_init(&module->port_defs, sizeof(odin3_port_def));
    odin3_vec_init(&module->attrs, sizeof(odin3_attr_rec));
    odin3_vec_init(&module->dead_pin_names, sizeof(uint32_t));
    module->arena = odin3_arena_create(ODIN3_MODULE_ARENA_CHUNK_BYTES);
    module->node_names = odin3_u64map_create(0);
    module->net_names = odin3_u64map_create(0);
    module->wire_names = odin3_u64map_create(0);
    module->attr_heads = odin3_u64map_create(0);
    module->nodes = store_create((odin3_pagevec_spec){sizeof(odin3_node_rec), MODULE_SHIFT});
    module->pins = store_create((odin3_pagevec_spec){sizeof(odin3_pin_rec), MODULE_SHIFT});
    module->nets = store_create((odin3_pagevec_spec){sizeof(odin3_net_rec), MODULE_SHIFT});
    module->wires = store_create((odin3_pagevec_spec){sizeof(odin3_wire_rec), WIRE_SHIFT});
    if (module->arena == NULL || module->node_names == NULL || module->net_names == NULL ||
        module->wire_names == NULL || module->attr_heads == NULL || module->nodes == NULL ||
        module->pins == NULL || module->nets == NULL || module->wires == NULL ||
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
    /* IR-7: the module's cell type, bound to type_def so module_add_port grows it in place. */
    module->type_def.name = odin3_strtab_get(odin3_design_strtab(design), name_str);
    module->type_def.gran = ODIN3_GRAN_MODULE;
    odin3_status st = odin3_celltype_add_local(design, &module->type_def, &module->type);
    if (st != ODIN3_OK) {
        module_destroy(module);
        return st;
    }
    odin3_celltype_bind_local(design, module->type, &module->type_def);
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

uint32_t odin3_module_node_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->nodes);
}

uint32_t odin3_module_pin_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->pins);
}

uint32_t odin3_module_net_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->nets);
}

uint32_t odin3_module_wire_end(const odin3_module *module) {
    return (uint32_t)odin3_pagevec_len(module->wires);
}

odin3_celltype_id odin3_module_celltype(const odin3_module *module) {
    return module->type;
}

/* --- ports (IR-3, IR-7) -------------------------------------------------------------------- */

uint32_t odin3_module_port_count(const odin3_module *module) {
    return (uint32_t)module->ports.len;
}

static const odin3_port_rec *port_at(const odin3_module *module, uint32_t index) {
    return index < module->ports.len ? odin3_vec_cat(&module->ports, index) : NULL;
}

odin3_node_id odin3_module_port(const odin3_module *module, uint32_t index) {
    const odin3_port_rec *port = port_at(module, index);
    return port != NULL ? port->node : (odin3_node_id){0};
}

odin3_wire_id odin3_module_port_wire(const odin3_module *module, uint32_t index) {
    const odin3_port_rec *port = port_at(module, index);
    return port != NULL ? port->wire : (odin3_wire_id){0};
}

/* NULL when the spec can be a new port (the name is checked by the wire), else the problem. */
static const char *port_spec_error(const odin3_module *module, const odin3_port_spec *spec) {
    if (spec == NULL) {
        return "no spec";
    }
    if (spec->name == 0 || spec->name >= odin3_strtab_count(odin3_design_strtab(module->design))) {
        return "the name is not a non-empty string ID";
    }
    if ((int)spec->dir < ODIN3_DIR_IN || (int)spec->dir > ODIN3_DIR_INOUT) {
        return "invalid direction";
    }
    if (spec->width == 0 || spec->width > MAX_PORT_WIDTH) {
        return "the width must be 1 .. 2^31";
    }
    if (spec->scalar && spec->width != 1) {
        return "a scalar port has width 1";
    }
    if (odin3_celltype_instances(module->design, module->type) > 0) {
        return "the module's cell type has instances (IR-7)";
    }
    return NULL;
}

/* Room for the port records, the type's port, the node and its pins and their pin blocks. */
static odin3_status port_reserve(odin3_module *module, uint32_t width) {
    odin3_status st = odin3_vec_reserve(&module->ports, module->ports.len + 1);
    if (st == ODIN3_OK) {
        st = odin3_vec_reserve(&module->port_defs, module->port_defs.len + 1);
        module->type_def.ports = module->port_defs.data; /* the array may have moved */
    }
    if (st == ODIN3_OK) {
        st = odin3_module_reserve(module->nodes, 1);
    }
    if (st == ODIN3_OK) {
        st = odin3_module_reserve(module->pins, width);
    }
    if (st == ODIN3_OK) {
        st = odin3_pinpool_reserve_class0(&module->pinpool, width); /* one pin per new net */
    }
    return st;
}

static odin3_status make_port_node(odin3_module *module, const odin3_port_spec *spec,
                                   odin3_node_id *node) {
    static const char *const types[] = {"$port_in", "$port_out", "$port_inout"};
    const odin3_strtab *strtab = odin3_design_strtab(module->design);
    uint32_t type_name = 0;
    odin3_node_spec node_spec = {{0}, 0, spec->prov, NULL, 1};
    bool found = odin3_strtab_find(strtab, odin3_bytes_cstr(types[spec->dir]), &type_name) &&
                 odin3_celltype_find(module->design, type_name, &node_spec.type);
    assert(found); /* built-in */
    (void)found;
    odin3_value width = odin3_value_int(spec->width);
    node_spec.params = &width;
    return odin3_node_create_any(module, &node_spec, node);
}

/* The infallible part of add_port: connections, port order, the cell type's new port. */
static void link_port(odin3_module *module, const odin3_port_spec *spec, odin3_node_id node,
                      odin3_wire_id wire) {
    odin3_wire_rec *wrec = odin3_wire_rec_at(module, wire);
    wrec->port_node = node;
    odin3_pinslice pins = odin3_node_pins(module, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_status st =
            odin3_pin_connect(module, (odin3_pin_id){pins.first.v + k}, wrec->nets[k]);
        assert(st == ODIN3_OK); /* pin blocks reserved */
        (void)st;
    }
    odin3_port_rec *port = odin3_vec_push(&module->ports);
    odin3_port_def *pdef = odin3_vec_push(&module->port_defs);
    assert(port != NULL && pdef != NULL); /* reserved */
    port->node = node;
    port->wire = wire;
    pdef->name = odin3_strtab_get(odin3_design_strtab(module->design), spec->name);
    pdef->dir = spec->dir;
    pdef->scalar = spec->scalar;
    pdef->width = spec->width;
    module->type_def.ports = module->port_defs.data;
    module->type_def.n_ports++;
}

odin3_status odin3_module_add_port(odin3_module *module, const odin3_port_spec *spec,
                                   odin3_node_id *port_node) {
    const char *err = port_spec_error(module, spec);
    if (err != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "module_add_port: %s", err);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_wire_spec wire_spec = {spec->name, (int32_t)(spec->width - 1), 0, false, spec->prov};
    odin3_wire_plan plan = {&wire_spec, NULL, NULL, 0, {0}};
    odin3_status st = odin3_wire_prepare(module, &plan, "module_add_port");
    if (st == ODIN3_OK) {
        st = port_reserve(module, spec->width);
    }
    odin3_name_change name = {module->wire_names, "module_add_port", plan.id.v, 0, spec->name};
    if (st == ODIN3_OK) {
        st = odin3_names_change(module, &name);
    }
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = make_port_node(module, spec, &node); /* last fallible step */
        if (st != ODIN3_OK) {
            (void)odin3_u64map_remove(module->wire_names, spec->name);
        }
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_wire_commit(module, &plan);
    link_port(module, spec, node, plan.id);
    if (port_node != NULL) {
        *port_node = node;
    }
    return ODIN3_OK;
}
