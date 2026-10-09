/*
 * attr.c — the public ABI's attribute group: string attributes of nodes, nets, wires and modules.
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The module of obj and its internal reference; NULL for a bad design, module, kind or ID. */
static const odin3_module *resolve(const odin3_design *design, odin3_obj obj, odin3_objref *ref) {
    static const odin3_api_store k_store[] = {
        [ODIN3_OBJ_NODE] = ODIN3_API_NODE,
        [ODIN3_OBJ_NET] = ODIN3_API_NET,
        [ODIN3_OBJ_WIRE] = ODIN3_API_WIRE,
    };
    if (obj.kind > (uint32_t)ODIN3_OBJ_MODULE) {
        return NULL;
    }
    if (obj.kind == (uint32_t)ODIN3_OBJ_MODULE) {
        *ref = (odin3_objref){ODIN3_OBJ_MODULE, obj.module};
        return odin3_api_module(design, obj.module);
    }
    *ref = (odin3_objref){(odin3_objkind)obj.kind, obj.id};
    return odin3_api_ref(design, (odin3_ref){obj.module, obj.id}, k_store[obj.kind]);
}

/* True when the object ref of mod is live (a module always is). */
static bool live(const odin3_module *mod, odin3_objref ref) {
    switch (ref.kind) {
    case ODIN3_OBJ_NODE:
        return odin3_node_live(mod, (odin3_node_id){ref.id});
    case ODIN3_OBJ_NET:
        return odin3_net_live(mod, (odin3_net_id){ref.id});
    case ODIN3_OBJ_WIRE:
        return odin3_wire_live(mod, (odin3_wire_id){ref.id});
    default:
        return true;
    }
}

ODIN3_EXPORT odin3_status odin3_attr_get_string(const odin3_design *design, odin3_obj obj,
                                                const char *key, const char **value) {
    odin3_objref ref = {ODIN3_OBJ_MODULE, 0};
    const odin3_module *mod = resolve(design, obj, &ref);
    if (mod == NULL || key == NULL || value == NULL) {
        return odin3_api_invalid(__func__);
    }
    uint32_t key_str = 0;
    const odin3_value *val = NULL;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(key), &key_str)) {
        val = odin3_attr_get(mod, ref, key_str);
    }
    if (val != NULL && val->kind != ODIN3_VAL_STRING) {
        return odin3_api_invalid(__func__);
    }
    *value = val != NULL ? odin3_api_str(design, val->str) : NULL;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_attr_set_string(odin3_design *design, odin3_obj obj,
                                                const char *key, const char *value) {
    odin3_objref ref = {ODIN3_OBJ_MODULE, 0};
    const odin3_module *found = resolve(design, obj, &ref);
    if (found == NULL || !live(found, ref) || key == NULL || key[0] == '\0' || value == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_module *mod = odin3_module_get(design, (odin3_module_id){obj.module});
    uint32_t key_str = 0;
    uint32_t value_str = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &key_str);
    if (st == ODIN3_OK) {
        st = odin3_design_intern(design, odin3_bytes_cstr(value), &value_str);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, value_str, 0};
    return odin3_attr_set(mod, ref, key_str, &val);
}
