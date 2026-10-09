/*
 * module.c — the public ABI's module group: name, ID ends and live counts, ports, name lookups.
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a module query returns through its uint32_t output. */
typedef enum module_query { Q_END, Q_LIVE_COUNT } module_query;

/* *out gets the end or the live count of store in module; INVALID_ARG (not logged) for bad
 * input. */
static odin3_status store_query(const odin3_design *design, uint32_t module, uint32_t *out,
                                odin3_api_store store, module_query query) {
    const odin3_module *mod = odin3_api_module(design, module);
    if (mod == NULL || out == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = query == Q_END ? odin3_api_end(mod, store) : odin3_api_live_count(mod, store);
    return ODIN3_OK;
}

/* st, with a failure logged as the ABI function fn's. */
static odin3_status logged_as(const char *fn, odin3_status st) {
    return st == ODIN3_OK ? st : odin3_api_invalid(fn);
}

ODIN3_EXPORT odin3_status odin3_module_get_name(const odin3_design *design, uint32_t module,
                                                const char **name) {
    const odin3_module *mod = odin3_api_module(design, module);
    if (mod == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_api_str(design, odin3_module_name(mod));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_module_get_node_end(const odin3_design *design, uint32_t module,
                                                    uint32_t *end) {
    return logged_as(__func__, store_query(design, module, end, ODIN3_API_NODE, Q_END));
}

ODIN3_EXPORT odin3_status odin3_module_get_net_end(const odin3_design *design, uint32_t module,
                                                   uint32_t *end) {
    return logged_as(__func__, store_query(design, module, end, ODIN3_API_NET, Q_END));
}

ODIN3_EXPORT odin3_status odin3_module_get_wire_end(const odin3_design *design, uint32_t module,
                                                    uint32_t *end) {
    return logged_as(__func__, store_query(design, module, end, ODIN3_API_WIRE, Q_END));
}

ODIN3_EXPORT odin3_status odin3_module_get_node_count(const odin3_design *design, uint32_t module,
                                                      uint32_t *count) {
    return logged_as(__func__, store_query(design, module, count, ODIN3_API_NODE, Q_LIVE_COUNT));
}

ODIN3_EXPORT odin3_status odin3_module_get_net_count(const odin3_design *design, uint32_t module,
                                                     uint32_t *count) {
    return logged_as(__func__, store_query(design, module, count, ODIN3_API_NET, Q_LIVE_COUNT));
}

ODIN3_EXPORT odin3_status odin3_module_get_wire_count(const odin3_design *design, uint32_t module,
                                                      uint32_t *count) {
    return logged_as(__func__, store_query(design, module, count, ODIN3_API_WIRE, Q_LIVE_COUNT));
}

ODIN3_EXPORT odin3_status odin3_module_get_port_count(const odin3_design *design, uint32_t module,
                                                      uint32_t *count) {
    const odin3_module *mod = odin3_api_module(design, module);
    if (mod == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_module_port_count(mod);
    return ODIN3_OK;
}

/* The module of port when port.id is a port index of it, else NULL. */
static const odin3_module *port_module(const odin3_design *design, odin3_ref port) {
    const odin3_module *mod = odin3_api_module(design, port.module);
    return mod != NULL && port.id < odin3_module_port_count(mod) ? mod : NULL;
}

ODIN3_EXPORT odin3_status odin3_module_get_port_node(const odin3_design *design, odin3_ref port,
                                                     uint32_t *node) {
    const odin3_module *mod = port_module(design, port);
    if (mod == NULL || node == NULL) {
        return odin3_api_invalid(__func__);
    }
    *node = odin3_module_port(mod, port.id).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_module_get_port_wire(const odin3_design *design, odin3_ref port,
                                                     uint32_t *wire) {
    const odin3_module *mod = port_module(design, port);
    if (mod == NULL || wire == NULL) {
        return odin3_api_invalid(__func__);
    }
    *wire = odin3_module_port_wire(mod, port.id).v;
    return ODIN3_OK;
}

/* What to look up: the object of store named name in module. */
typedef struct name_query {
    uint32_t module;
    const char *name;
    odin3_api_store store;
} name_query;

/* The live object of query.store named str (a strtab ID) in mod, 0 when none. */
static uint32_t find_named(const odin3_module *mod, const name_query *query, uint32_t str) {
    switch (query->store) {
    case ODIN3_API_NODE:
        return odin3_module_find_node(mod, str).v;
    case ODIN3_API_NET:
        return odin3_module_find_net(mod, str).v;
    default:
        return odin3_module_find_wire(mod, str).v;
    }
}

/* *id gets the live object query names, 0 when none; INVALID_ARG (not logged) for bad input. */
static odin3_status lookup(const odin3_design *design, name_query query, uint32_t *id) {
    const odin3_module *mod = odin3_api_module(design, query.module);
    if (mod == NULL || query.name == NULL || id == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t str = 0;
    uint32_t found = 0;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(query.name), &str)) {
        found = find_named(mod, &query, str);
    }
    *id = found;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_module_lookup_node(const odin3_design *design, uint32_t module,
                                                   const char *name, uint32_t *id) {
    return logged_as(__func__, lookup(design, (name_query){module, name, ODIN3_API_NODE}, id));
}

ODIN3_EXPORT odin3_status odin3_module_lookup_net(const odin3_design *design, uint32_t module,
                                                  const char *name, uint32_t *id) {
    return logged_as(__func__, lookup(design, (name_query){module, name, ODIN3_API_NET}, id));
}

ODIN3_EXPORT odin3_status odin3_module_lookup_wire(const odin3_design *design, uint32_t module,
                                                   const char *name, uint32_t *id) {
    return logged_as(__func__, lookup(design, (name_query){module, name, ODIN3_API_WIRE}, id));
}
