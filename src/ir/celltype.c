/* celltype.c — global cell-type registry, per-design table, black boxes (IR-7b, IR-11). */
#include "ir/celltype.h"
#include "ir/ir_internal.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

enum { NO_PARAM = -1 };

/* Plugin-registered definitions (global_slot), numbered after the built-ins. */
typedef struct global_slot {
    const odin3_celltype_def *def;
} global_slot;
static odin3_vec g_globals;
static bool g_globals_ready = false;

static uint32_t global_count(void) {
    return odin3_builtin_celltype_count + (uint32_t)g_globals.len;
}

static const odin3_celltype_def *global_at(uint32_t idx) {
    if (idx < odin3_builtin_celltype_count) {
        return odin3_builtin_celltypes[idx];
    }
    const global_slot *slot = odin3_vec_cat(&g_globals, idx - odin3_builtin_celltype_count);
    return slot->def;
}

static bool global_exists(const char *name) {
    for (uint32_t i = 0; i < global_count(); i++) {
        if (strcmp(global_at(i)->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* --- definition validation ----------------------------------------------------------------- */

static bool name_ok(const char *name) {
    return name != NULL && name[0] != '\0';
}

static bool enum_ok(int val, int max) {
    return val >= 0 && val <= max;
}

static int find_param(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return NO_PARAM;
}

static const char *param_error(const odin3_celltype_def *def, uint32_t idx) {
    const odin3_param_def *param = &def->params[idx];
    if (!name_ok(param->name)) {
        return "parameter without a name";
    }
    if (!enum_ok((int)param->kind, ODIN3_VAL_COVER) || param->dflt.kind != param->kind) {
        return "parameter kind invalid or different from its default's";
    }
    for (uint32_t i = 0; i < idx; i++) {
        if (strcmp(def->params[i].name, param->name) == 0) {
            return "duplicate parameter name";
        }
    }
    return NULL;
}

static const char *port_error(const odin3_celltype_def *def, uint32_t idx) {
    const odin3_port_def *port = &def->ports[idx];
    if (!name_ok(port->name)) {
        return "port without a name";
    }
    if (!enum_ok((int)port->dir, ODIN3_DIR_INOUT)) {
        return "invalid port direction";
    }
    for (uint32_t i = 0; i < idx; i++) {
        if (strcmp(def->ports[i].name, port->name) == 0) {
            return "duplicate port name";
        }
    }
    if (port->width_fn == NULL && port->width_param != NULL) {
        int pidx = find_param(def, port->width_param);
        if (pidx == NO_PARAM || def->params[pidx].kind != ODIN3_VAL_INT) {
            return "width parameter is not an int parameter of the type";
        }
    }
    return NULL;
}

/* NULL when def is valid (rules in celltype.h), else a description of the first problem. */
static const char *def_error(const odin3_celltype_def *def) {
    if (def == NULL || !name_ok(def->name)) {
        return "no definition or no name";
    }
    if (!enum_ok((int)def->gran, ODIN3_GRAN_PORT)) {
        return "invalid granularity";
    }
    if ((def->n_ports > 0 && def->ports == NULL) || (def->n_params > 0 && def->params == NULL)) {
        return "NULL port or parameter array";
    }
    const char *err = NULL;
    for (uint32_t i = 0; err == NULL && i < def->n_params; i++) {
        err = param_error(def, i);
    }
    for (uint32_t i = 0; err == NULL && i < def->n_ports; i++) {
        err = port_error(def, i);
    }
    return err;
}

static bool def_check(const odin3_celltype_def *def, const char *what) {
    const char *err = def_error(def);
    if (err != NULL) {
        const char *name = def != NULL && def->name != NULL ? def->name : "";
        odin3_log(ODIN3_LOG_ERROR, "%s: cell type '%s': %s", what, name, err);
        return false;
    }
    return true;
}

/* --- deep copy into a design arena --------------------------------------------------------- */

/* Copies src (may be NULL) into the arena; false on out of memory. */
static bool copy_str(odin3_arena *arena, const char *src, const char **dst) {
    if (src == NULL) {
        *dst = NULL;
        return true;
    }
    char *mem = odin3_arena_strndup(arena, src, strlen(src));
    *dst = mem;
    return mem != NULL;
}

static bool copy_ports(odin3_arena *arena, const odin3_celltype_def *src, odin3_celltype_def *dst) {
    dst->ports = NULL;
    if (src->n_ports == 0) {
        return true;
    }
    odin3_port_def *ports = odin3_arena_alloc(arena, sizeof *ports * src->n_ports);
    if (ports == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < src->n_ports; i++) {
        ports[i] = src->ports[i];
        if (!copy_str(arena, src->ports[i].name, &ports[i].name) ||
            !copy_str(arena, src->ports[i].width_param, &ports[i].width_param)) {
            return false;
        }
    }
    dst->ports = ports;
    return true;
}

static bool copy_params(odin3_arena *arena, const odin3_celltype_def *src,
                        odin3_celltype_def *dst) {
    dst->params = NULL;
    if (src->n_params == 0) {
        return true;
    }
    odin3_param_def *params = odin3_arena_alloc(arena, sizeof *params * src->n_params);
    if (params == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < src->n_params; i++) {
        params[i] = src->params[i];
        if (!copy_str(arena, src->params[i].name, &params[i].name) ||
            odin3_value_copy(arena, &src->params[i].dflt, &params[i].dflt) != ODIN3_OK) {
            return false;
        }
    }
    dst->params = params;
    return true;
}

/* Deep copy of a valid definition; NULL on out of memory (the arena keeps the partial copy). */
static const odin3_celltype_def *copy_def(odin3_arena *arena, const odin3_celltype_def *src) {
    odin3_celltype_def *dst = odin3_arena_alloc(arena, sizeof *dst);
    if (dst == NULL) {
        return NULL;
    }
    *dst = *src;
    if (!copy_str(arena, src->name, &dst->name) || !copy_ports(arena, src, dst) ||
        !copy_params(arena, src, dst)) {
        return NULL;
    }
    return dst;
}

/* --- per-design table ---------------------------------------------------------------------- */

static const odin3_celltype_entry *entry_at(const odin3_design *design, odin3_celltype_id id) {
    if (id.v == 0 || id.v >= design->celltypes.len) {
        return NULL;
    }
    return odin3_vec_cat(&design->celltypes, id.v);
}

static odin3_celltype_entry *entry_mut(odin3_design *design, odin3_celltype_id id) {
    if (id.v == 0 || id.v >= design->celltypes.len) {
        return NULL;
    }
    return odin3_vec_at(&design->celltypes, id.v);
}

/* Appends def under a name the design does not have yet; the table is unchanged on failure. */
static odin3_status append_entry(odin3_design *design, const odin3_celltype_def *def, bool local,
                                 odin3_celltype_id *out) {
    uint32_t name = 0;
    odin3_status st = odin3_strtab_intern(design->strtab, odin3_bytes_cstr(def->name), &name);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t idx = design->celltypes.len;
    if (idx >= UINT32_MAX || odin3_vec_reserve(&design->celltypes, idx + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    st = odin3_u64map_put(design->celltype_names, (odin3_kv){name, idx});
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_celltype_entry *entry = odin3_vec_push(&design->celltypes);
    assert(entry != NULL); /* reserved above */
    entry->def = def;
    entry->name = name;
    entry->local = local;
    if (out != NULL) {
        out->v = (uint32_t)idx;
    }
    return ODIN3_OK;
}

static bool lookup(const odin3_design *design, const char *name, odin3_celltype_id *out) {
    uint32_t str = 0;
    if (!odin3_strtab_find(design->strtab, odin3_bytes_cstr(name), &str)) {
        return false;
    }
    return odin3_celltype_find(design, str, out);
}

odin3_status odin3_celltype_table_init(odin3_design *design) {
    uint32_t count = global_count();
    odin3_vec_init(&design->celltypes, sizeof(odin3_celltype_entry));
    odin3_vec_init(&design->declared, sizeof(odin3_celltype_id));
    design->celltype_names = odin3_u64map_create(count);
    if (design->celltype_names == NULL ||
        odin3_vec_reserve(&design->celltypes, (size_t)count + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    (void)odin3_vec_push(&design->celltypes); /* slot 0: reserved dummy */
    for (uint32_t i = 0; i < count; i++) {
        assert(def_error(global_at(i)) == NULL);
        odin3_status st = append_entry(design, global_at(i), false, NULL);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

void odin3_celltype_table_free(odin3_design *design) {
    odin3_vec_free(&design->celltypes);
    odin3_vec_free(&design->declared);
    odin3_u64map_destroy(design->celltype_names);
    design->celltype_names = NULL;
}

/* --- public API ---------------------------------------------------------------------------- */

odin3_status odin3_celltype_register_global(const odin3_celltype_def *def) {
    if (!def_check(def, "register_global")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (global_exists(def->name)) {
        odin3_log(ODIN3_LOG_ERROR, "register_global: cell type '%s' already registered", def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!g_globals_ready) {
        odin3_vec_init(&g_globals, sizeof(global_slot));
        g_globals_ready = true;
    }
    global_slot *slot = odin3_vec_push(&g_globals);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->def = def;
    return ODIN3_OK;
}

bool odin3_celltype_find(const odin3_design *design, uint32_t name_str, odin3_celltype_id *out) {
    uint64_t val = 0;
    if (!odin3_u64map_get(design->celltype_names, name_str, &val)) {
        return false;
    }
    if (out != NULL) {
        out->v = (uint32_t)val;
    }
    return true;
}

const odin3_celltype_def *odin3_celltype_get(const odin3_design *design, odin3_celltype_id id) {
    const odin3_celltype_entry *entry = entry_at(design, id);
    return entry != NULL ? entry->def : NULL;
}

static uint32_t int_width(const odin3_value *val, const char *param) {
    if (val->kind != ODIN3_VAL_INT || val->i < 0 || val->i > (int64_t)UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: parameter %s is not an int in 0..%u", param,
                  UINT32_MAX);
        return 0;
    }
    return (uint32_t)val->i;
}

uint32_t odin3_celltype_port_width(const odin3_design *design, odin3_celltype_id id,
                                   const odin3_value *params, uint32_t port) {
    const odin3_celltype_entry *entry = entry_at(design, id);
    if (entry == NULL || port >= entry->def->n_ports) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: no port %u on cell type %u", port, id.v);
        return 0;
    }
    const odin3_celltype_def *def = entry->def;
    const odin3_port_def *pdef = &def->ports[port];
    if (params == NULL && def->n_params > 0) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: cell type '%s' needs parameter values", def->name);
        return 0;
    }
    if (pdef->width_fn != NULL) {
        return pdef->width_fn(params, port);
    }
    if (pdef->width_param == NULL) {
        return pdef->width;
    }
    int pidx = find_param(def, pdef->width_param);
    assert(pidx != NO_PARAM); /* definitions are validated */
    return int_width(&params[pidx], pdef->width_param);
}

uint32_t odin3_celltype_instances(const odin3_design *design, odin3_celltype_id id) {
    const odin3_celltype_entry *entry = entry_at(design, id);
    return entry != NULL ? entry->instances : 0;
}

void odin3_celltype_instances_inc(odin3_design *design, odin3_celltype_id id) {
    odin3_celltype_entry *entry = entry_mut(design, id);
    assert(entry != NULL && entry->instances < UINT32_MAX);
    entry->instances++;
}

void odin3_celltype_instances_dec(odin3_design *design, odin3_celltype_id id) {
    odin3_celltype_entry *entry = entry_mut(design, id);
    assert(entry != NULL && entry->instances > 0);
    entry->instances--;
}

/* Adds a deep copy of a valid definition whose name the design does not have. */
static odin3_status add_copy(odin3_design *design, const odin3_celltype_def *def,
                             odin3_celltype_id *out) {
    const odin3_celltype_def *copy = copy_def(design->arena, def);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return append_entry(design, copy, true, out);
}

odin3_status odin3_celltype_add_local(odin3_design *design, const odin3_celltype_def *def,
                                      odin3_celltype_id *out) {
    if (!def_check(def, "add_local")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (lookup(design, def->name, NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "add_local: cell type '%s' already exists", def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    return add_copy(design, def, out);
}

odin3_status odin3_celltype_replace_local(odin3_design *design, odin3_celltype_id id,
                                          const odin3_celltype_def *def) {
    odin3_celltype_entry *entry = entry_mut(design, id);
    if (entry == NULL || !entry->local) {
        odin3_log(ODIN3_LOG_ERROR, "replace_local: %u is not a local cell type", id.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!def_check(def, "replace_local")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (strcmp(def->name, entry->def->name) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "replace_local: cannot rename '%s' to '%s'", entry->def->name,
                  def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_celltype_def *copy = copy_def(design->arena, def);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    entry->def = copy;
    return ODIN3_OK;
}

static bool fixed_width(const odin3_port_def *port) {
    return port->width_fn == NULL && port->width_param == NULL;
}

/* IR-7b: same port count, and per port the same name, direction and constant width. */
static bool blackbox_compatible(const odin3_celltype_def *have, const odin3_celltype_def *decl) {
    if (have->n_ports != decl->n_ports) {
        return false;
    }
    for (uint32_t i = 0; i < have->n_ports; i++) {
        const odin3_port_def *lhs = &have->ports[i];
        const odin3_port_def *rhs = &decl->ports[i];
        if (strcmp(lhs->name, rhs->name) != 0 || lhs->dir != rhs->dir || !fixed_width(lhs) ||
            !fixed_width(rhs) || lhs->width != rhs->width) {
            return false;
        }
    }
    return true;
}

odin3_status odin3_celltype_declare_blackbox(odin3_design *design, const odin3_celltype_def *def,
                                             odin3_celltype_id *out) {
    if (!def_check(def, "declare_blackbox")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_vec_reserve(&design->declared, design->declared.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_celltype_id id = {0};
    if (lookup(design, def->name, &id)) {
        if (!blackbox_compatible(odin3_celltype_get(design, id), def)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "declare_blackbox: '%s' does not match the registered cell type's ports",
                      def->name);
            return ODIN3_ERR_INVALID_ARG;
        }
    } else {
        odin3_celltype_def blackbox = *def;
        blackbox.gran = ODIN3_GRAN_BLACKBOX;
        odin3_status st = add_copy(design, &blackbox, &id);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_celltype_id *slot = odin3_vec_push(&design->declared);
    assert(slot != NULL); /* reserved above */
    *slot = id;
    if (out != NULL) {
        *out = id;
    }
    return ODIN3_OK;
}

uint32_t odin3_design_declared_model_count(const odin3_design *design) {
    return (uint32_t)design->declared.len;
}

odin3_celltype_id odin3_design_declared_model(const odin3_design *design, uint32_t index) {
    if (index >= design->declared.len) {
        return (odin3_celltype_id){0};
    }
    const odin3_celltype_id *slot = odin3_vec_cat(&design->declared, index);
    return *slot;
}
