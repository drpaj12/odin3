/*
 * design.c — the public ABI's design group: run a pass, top module, modules by index and name.
 * (odin3_design_create and odin3_design_destroy are exported from src/ir/design.c as they are.)
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stddef.h>
#include <stdint.h>

ODIN3_EXPORT odin3_status odin3_design_run_pass(odin3_design *design, const char *name,
                                                const char *args) {
    return odin3_pass_run(design, name, odin3_bytes_cstr(args));
}

ODIN3_EXPORT odin3_status odin3_design_get_top_module(const odin3_design *design,
                                                      uint32_t *module) {
    if (design == NULL || module == NULL) {
        return odin3_api_invalid(__func__);
    }
    *module = odin3_design_top(design).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_set_top_module(odin3_design *design, uint32_t module) {
    if (design == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_design_set_top(design, (odin3_module_id){module});
}

ODIN3_EXPORT odin3_status odin3_design_get_module_count(const odin3_design *design,
                                                        uint32_t *count) {
    if (design == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_design_module_end(design) - 1;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_get_module_at(const odin3_design *design, uint32_t index,
                                                     uint32_t *module) {
    if (design == NULL || module == NULL || index >= odin3_design_module_end(design) - 1) {
        return odin3_api_invalid(__func__);
    }
    *module = index + 1; /* modules are never deleted: IDs 1 .. count in creation order */
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_lookup_module(const odin3_design *design, const char *name,
                                                     uint32_t *module) {
    if (design == NULL || name == NULL || module == NULL) {
        return odin3_api_invalid(__func__);
    }
    *module = 0;
    uint32_t str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str)) {
        return ODIN3_OK;
    }
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t id = 1; id < end; id++) {
        if (odin3_module_name(odin3_api_module(design, id)) == str) {
            *module = id;
            break;
        }
    }
    return ODIN3_OK;
}
