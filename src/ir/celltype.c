/* celltype.c — global cell-type registry, per-design table, black boxes (IR-7b, IR-11). */
#include "ir/celltype.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
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
    if (!odin3_value_valid(&param->dflt)) {
        return "parameter default is malformed (missing payload or partial cover rows)";
    }
    for (uint32_t i = 0; i < idx; i++) {
        if (strcmp(def->params[i].name, param->name) == 0) {
            return "duplicate parameter name";
        }
    }
    return NULL;
}

/*
 * NULL when the port's width rule (the first that applies, celltype.h) is valid for def. A width
 * expression's reason is written to why, which the result then points to.
 */
static const char *width_rule_error(const odin3_celltype_def *def, const odin3_port_def *port,
                                    odin3_width_why *why) {
    if (port->width_fn != NULL) {
        return NULL;
    }
    if (port->width_expr != NULL) {
        const odin3_width_expr *wexpr = port->width_expr;
        if (wexpr->check == NULL || wexpr->eval == NULL) {
            return "width expression without its hooks";
        }
        return wexpr->check(wexpr, def, why) ? NULL : why->text;
    }
    if (port->width_param != NULL) {
        int pidx = find_param(def, port->width_param);
        if (pidx == NO_PARAM || def->params[pidx].kind != ODIN3_VAL_INT) {
            return "width parameter is not an int parameter of the type";
        }
    }
    return NULL;
}

static const char *port_error(const odin3_celltype_def *def, uint32_t idx, odin3_width_why *why) {
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
    return width_rule_error(def, port, why);
}

/* NULL when the simulation flags are consistent (celltype.h); def's ports are present. */
static const char *sim_flags_error(const odin3_celltype_def *def) {
    if ((def->flags & ODIN3_CT_SEQ_EDGE) != 0 && (def->flags & ODIN3_CT_SEQ_LEVEL) != 0) {
        return "SEQ_EDGE and SEQ_LEVEL are exclusive";
    }
    if ((def->flags & ODIN3_CT_CLOCK_PIN0) == 0) {
        return NULL;
    }
    const odin3_port_def *pin = def->n_ports > 0 ? &def->ports[0] : NULL;
    if (pin == NULL || pin->dir != ODIN3_DIR_IN || !pin->scalar || pin->width != 1 ||
        pin->width_fn != NULL || pin->width_expr != NULL || pin->width_param != NULL) {
        return "CLOCK_PIN0 needs port 0 to be a scalar 1-bit input";
    }
    return NULL;
}

/*
 * NULL when def is valid (rules in celltype.h), else a description of the first problem (which
 * may point into why).
 */
static const char *def_error(const odin3_celltype_def *def, odin3_width_why *why) {
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
        err = port_error(def, i, why);
    }
    return err != NULL ? err : sim_flags_error(def);
}

/* For assertions (inline: unused when they compile out). */
static inline bool def_valid(const odin3_celltype_def *def) {
    odin3_width_why why = {""};
    return def_error(def, &why) == NULL;
}

static bool def_check(const odin3_celltype_def *def, const char *what) {
    odin3_width_why why = {""};
    const char *err = def_error(def, &why);
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
    entry->lib = NULL;
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
    odin3_vec_init(&design->declared, sizeof(odin3_declared_entry));
    design->celltype_names = odin3_u64map_create(count);
    if (design->celltype_names == NULL ||
        odin3_vec_reserve(&design->celltypes, (size_t)count + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    (void)odin3_vec_push(&design->celltypes); /* slot 0: reserved dummy */
    for (uint32_t i = 0; i < count; i++) {
        assert(def_valid(global_at(i)));
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

/*
 * Width of port `port` of the valid definition def under params, without logging. On
 * ODIN3_ERR_INVALID_ARG why holds the reason: a width expression's own, or for a width parameter
 * "parameter NAME is not an int in 0..UINT32_MAX".
 */
static odin3_status width_quiet(const odin3_celltype_def *def, const odin3_value *params,
                                uint32_t port, uint32_t *width, odin3_width_why *why) {
    const odin3_port_def *pdef = &def->ports[port];
    if (pdef->width_fn != NULL) {
        *width = pdef->width_fn(params, port);
        return ODIN3_OK;
    }
    if (pdef->width_expr != NULL) {
        const odin3_width_args args = {def, params, why};
        return pdef->width_expr->eval(pdef->width_expr, &args, width);
    }
    if (pdef->width_param == NULL) {
        *width = pdef->width;
        return ODIN3_OK;
    }
    int pidx = find_param(def, pdef->width_param);
    assert(pidx != NO_PARAM); /* definitions are validated */
    const odin3_value *val = &params[pidx];
    if (val->kind != ODIN3_VAL_INT || val->i < 0 || val->i > (int64_t)UINT32_MAX) {
        (void)snprintf(why->text, sizeof why->text, "parameter %s is not an int in 0..%u",
                       pdef->width_param, UINT32_MAX);
        return ODIN3_ERR_INVALID_ARG;
    }
    *width = (uint32_t)val->i;
    return ODIN3_OK;
}

odin3_status odin3_celltype_port_width_checked(const odin3_design *design,
                                               const odin3_port_query *query, uint32_t *width) {
    const odin3_celltype_entry *entry = entry_at(design, query->type);
    if (entry == NULL || query->port >= entry->def->n_ports) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: no port %u on cell type %u", query->port,
                  query->type.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_celltype_def *def = entry->def;
    if (query->params == NULL && def->n_params > 0) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: cell type '%s' needs parameter values", def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_width_why why = {""};
    odin3_status st = width_quiet(def, query->params, query->port, width, &why);
    if (st != ODIN3_ERR_INVALID_ARG) {
        return st;
    }
    const odin3_port_def *pdef = &def->ports[query->port];
    if (pdef->width_expr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "port_width: port '%s' of cell type '%s': %s", pdef->name,
                  def->name, why.text);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "port_width: %s", why.text);
    }
    return st;
}

uint32_t odin3_celltype_port_width(const odin3_design *design, odin3_celltype_id id,
                                   const odin3_value *params, uint32_t port) {
    odin3_port_query query = {id, params, port};
    uint32_t width = 0;
    return odin3_celltype_port_width_checked(design, &query, &width) == ODIN3_OK ? width : 0;
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

odin3_arena *odin3_celltype_arena(const odin3_design *design) {
    return design->arena;
}

odin3_status odin3_celltype_set_lib(odin3_design *design, odin3_celltype_id id,
                                    const odin3_techlib_cell *lib) {
    odin3_celltype_entry *entry = entry_mut(design, id);
    if (entry == NULL || !entry->local) {
        odin3_log(ODIN3_LOG_ERROR, "set_lib: cell type %u is not a local type", id.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    entry->lib = lib;
    return ODIN3_OK;
}

const odin3_techlib_cell *odin3_celltype_lib(const odin3_design *design, odin3_celltype_id id) {
    const odin3_celltype_entry *entry = entry_at(design, id);
    return entry != NULL ? entry->lib : NULL;
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

void odin3_celltype_bind_local(odin3_design *design, odin3_celltype_id id,
                               const odin3_celltype_def *def) {
    odin3_celltype_entry *entry = entry_mut(design, id);
    assert(entry != NULL && entry->local && def_valid(def));
    assert(strcmp(def->name, entry->def->name) == 0);
    entry->def = def;
}

/* --- black boxes (IR-7b) -------------------------------------------------------------------- */

/* Index of the port of def named name, or def->n_ports when there is none. */
static uint32_t port_named(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    return def->n_ports;
}

/* The width seen on port `port` of a type (0: none). */
typedef uint32_t (*seen_fn)(const void *ctx, uint32_t port);

static uint32_t seen_in_array(const void *ctx, uint32_t port) {
    return ((const uint32_t *)ctx)[port];
}

/* A declaration seen through the registered type's port numbering. */
typedef struct decl_view {
    const odin3_celltype_def *have;
    const odin3_celltype_def *decl;
} decl_view;

static uint32_t seen_in_decl(const void *ctx, uint32_t port) {
    const decl_view *view = ctx;
    uint32_t at = port_named(view->decl, view->have->ports[port].name);
    return at < view->decl->n_ports ? view->decl->ports[at].width : 0;
}

/* params[i]: the largest nonzero seen width of a port sized by INT parameter i, else default. */
static void infer(const odin3_celltype_def *def, seen_fn seen, const void *ctx,
                  odin3_value *params) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
        uint32_t best = 0;
        for (uint32_t port = 0; def->params[i].kind == ODIN3_VAL_INT && port < def->n_ports;
             port++) {
            const char *wparam = def->ports[port].width_param;
            uint32_t width =
                wparam != NULL && strcmp(wparam, def->params[i].name) == 0 ? seen(ctx, port) : 0;
            best = width > best ? width : best;
        }
        if (best > 0) {
            params[i] = odin3_value_int(best);
        }
    }
}

odin3_status odin3_celltype_infer_params(const odin3_design *design, odin3_celltype_id type,
                                         const uint32_t *seen, odin3_value *params) {
    const odin3_celltype_entry *entry = entry_at(design, type);
    if (entry == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "infer_params: no cell type %u", type.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    infer(entry->def, seen_in_array, seen, params);
    return ODIN3_OK;
}

static odin3_status why_fail(odin3_width_why *why, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static odin3_status why_fail(odin3_width_why *why, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(why->text, sizeof why->text, fmt, args);
    va_end(args);
    return ODIN3_ERR_INVALID_ARG;
}

/* Every declared port is a port of have with the same direction and a constant width, and every
 * port of have is declared (names are unique, so this is a bijection). */
static odin3_status names_match(const odin3_celltype_def *have, const odin3_celltype_def *decl,
                                odin3_width_why *why) {
    if (have->gran == ODIN3_GRAN_PORT) {
        return why_fail(why, "a port cell type is never a black box");
    }
    for (uint32_t i = 0; i < decl->n_ports; i++) {
        const odin3_port_def *port = &decl->ports[i];
        uint32_t at = port_named(have, port->name);
        if (at == have->n_ports) {
            return why_fail(why, "the cell type has no port '%s'", port->name);
        }
        if (have->ports[at].dir != port->dir) {
            return why_fail(why, "port '%s' has another direction", port->name);
        }
        if (port->width_param != NULL || port->width_fn != NULL || port->width_expr != NULL) {
            return why_fail(why, "declared port '%s' has no constant width", port->name);
        }
    }
    for (uint32_t i = 0; i < have->n_ports; i++) {
        if (port_named(decl, have->ports[i].name) == decl->n_ports) {
            return why_fail(why, "port '%s' of the cell type is not declared", have->ports[i].name);
        }
    }
    return ODIN3_OK;
}

/* Every port of have, sized by params, has its declared width (names already match). */
static odin3_status widths_match(const odin3_celltype_def *have, const odin3_celltype_def *decl,
                                 const odin3_value *params, odin3_width_why *why) {
    for (uint32_t port = 0; port < have->n_ports; port++) {
        const char *name = have->ports[port].name;
        uint32_t want = decl->ports[port_named(decl, name)].width;
        uint32_t width = 0;
        odin3_width_why inner = {""};
        odin3_status st = width_quiet(have, params, port, &width, &inner);
        if (st == ODIN3_ERR_INVALID_ARG) {
            return why_fail(why, "port '%s': %s", name, inner.text);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        if (width != want) {
            return why_fail(why, "port '%s' has %u bit%s, the cell type gives it %u", name, want,
                            want == 1 ? "" : "s", width);
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_celltype_blackbox_match(const odin3_design *design,
                                           const odin3_blackbox_match *match,
                                           odin3_width_why *why) {
    const odin3_celltype_entry *entry = entry_at(design, match->type);
    if (entry == NULL || !def_valid(match->decl)) {
        return why_fail(why, "no such cell type or an invalid declaration");
    }
    const odin3_celltype_def *have = entry->def;
    odin3_status st = names_match(have, match->decl, why);
    if (st != ODIN3_OK) {
        return st;
    }
    const decl_view view = {have, match->decl};
    infer(have, seen_in_decl, &view, match->params);
    return widths_match(have, match->decl, match->params, why);
}

/* The declared-model entry of a declaration of the registered type entry->type. */
static odin3_status declare_existing(odin3_design *design, const odin3_celltype_def *def,
                                     odin3_declared_entry *entry) {
    const odin3_celltype_def *have = odin3_celltype_get(design, entry->type);
    odin3_value *params = NULL;
    if (have->n_params > 0) {
        params = odin3_arena_alloc(design->arena, sizeof *params * have->n_params);
        if (params == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_width_why why = {""};
    const odin3_blackbox_match match = {entry->type, def, params};
    odin3_status st = odin3_celltype_blackbox_match(design, &match, &why);
    if (st == ODIN3_ERR_INVALID_ARG) {
        odin3_log(ODIN3_LOG_ERROR,
                  "declare_blackbox: '%s' does not match the registered cell type: %s", def->name,
                  why.text);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    entry->decl = copy_def(design->arena, def);
    entry->params = params;
    return entry->decl != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The declared-model entry of a new local black box (its parameters keep their defaults). */
static odin3_status declare_new(odin3_design *design, const odin3_celltype_def *def,
                                odin3_declared_entry *entry) {
    odin3_value *params = NULL;
    if (def->n_params > 0) {
        params = odin3_arena_alloc(design->arena, sizeof *params * def->n_params);
        if (params == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_celltype_def blackbox = *def;
    blackbox.gran = ODIN3_GRAN_BLACKBOX;
    odin3_status st = add_copy(design, &blackbox, &entry->type);
    if (st != ODIN3_OK) {
        return st;
    }
    entry->decl = odin3_celltype_get(design, entry->type);
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = entry->decl->params[i].dflt;
    }
    entry->params = params;
    return ODIN3_OK;
}

odin3_status odin3_celltype_declare_blackbox(odin3_design *design, const odin3_celltype_def *def,
                                             odin3_celltype_id *out) {
    if (!def_check(def, "declare_blackbox")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_vec_reserve(&design->declared, design->declared.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_declared_entry entry = {{0}, NULL, NULL};
    odin3_status st = lookup(design, def->name, &entry.type) ? declare_existing(design, def, &entry)
                                                             : declare_new(design, def, &entry);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_declared_entry *slot = odin3_vec_push(&design->declared);
    assert(slot != NULL); /* reserved above */
    *slot = entry;
    if (out != NULL) {
        *out = entry.type;
    }
    return ODIN3_OK;
}

static const odin3_declared_entry *declared_at(const odin3_design *design, uint32_t index) {
    return index < design->declared.len ? odin3_vec_cat(&design->declared, index) : NULL;
}

uint32_t odin3_design_declared_model_count(const odin3_design *design) {
    return (uint32_t)design->declared.len;
}

odin3_celltype_id odin3_design_declared_model(const odin3_design *design, uint32_t index) {
    const odin3_declared_entry *entry = declared_at(design, index);
    return entry != NULL ? entry->type : (odin3_celltype_id){0};
}

const odin3_value *odin3_design_declared_model_params(const odin3_design *design, uint32_t index) {
    const odin3_declared_entry *entry = declared_at(design, index);
    return entry != NULL ? entry->params : NULL;
}

const odin3_celltype_def *odin3_design_declared_model_decl(const odin3_design *design,
                                                           uint32_t index) {
    const odin3_declared_entry *entry = declared_at(design, index);
    return entry != NULL ? entry->decl : NULL;
}
