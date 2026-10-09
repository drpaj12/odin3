/*
 * common.c — helpers shared by the public ABI wrappers (argument resolution, strings).
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

const odin3_module *odin3_api_module(const odin3_design *design, uint32_t module) {
    if (design == NULL || module == 0 || module >= odin3_design_module_end(design)) {
        return NULL;
    }
    /* odin3_module_get only reads the design's module table (as the BLIF writer does). */
    return odin3_module_get((odin3_design *)design, (odin3_module_id){module});
}

uint32_t odin3_api_end(const odin3_module *module, odin3_api_store store) {
    switch (store) {
    case ODIN3_API_NODE:
        return odin3_module_node_end(module);
    case ODIN3_API_PIN:
        return odin3_module_pin_end(module);
    case ODIN3_API_NET:
        return odin3_module_net_end(module);
    default:
        return odin3_module_wire_end(module);
    }
}

static uint32_t live_nodes(const odin3_module *module) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        count += odin3_node_live(module, (odin3_node_id){id}) ? 1U : 0U;
    }
    return count;
}

static uint32_t live_nets(const odin3_module *module) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_net_end(module); id++) {
        count += odin3_net_live(module, (odin3_net_id){id}) ? 1U : 0U;
    }
    return count;
}

static uint32_t live_wires(const odin3_module *module) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_wire_end(module); id++) {
        count += odin3_wire_live(module, (odin3_wire_id){id}) ? 1U : 0U;
    }
    return count;
}

uint32_t odin3_api_live_count(const odin3_module *module, odin3_api_store store) {
    switch (store) {
    case ODIN3_API_NODE:
        return live_nodes(module);
    case ODIN3_API_NET:
        return live_nets(module);
    default:
        return live_wires(module);
    }
}

const odin3_module *odin3_api_ref(const odin3_design *design, odin3_ref ref,
                                  odin3_api_store store) {
    const odin3_module *module = odin3_api_module(design, ref.module);
    if (module == NULL || ref.id == 0 || ref.id >= odin3_api_end(module, store)) {
        return NULL;
    }
    return module;
}

odin3_status odin3_api_invalid(const char *fn) {
    odin3_log(ODIN3_LOG_ERROR,
              "%s: invalid argument (a NULL pointer, an unknown module, an ID or index out of "
              "range, or a bad kind)",
              fn);
    return ODIN3_ERR_INVALID_ARG;
}

const char *odin3_api_str(const odin3_design *design, uint32_t str) {
    const char *text = odin3_strtab_get(odin3_design_strtab(design), str);
    return text != NULL ? text : "";
}

odin3_status odin3_api_text(const odin3_design *design, odin3_bytes text, const char **out) {
    uint32_t str = 0;
    odin3_status st = odin3_strtab_intern(odin3_design_strtab(design), text, &str);
    if (st == ODIN3_OK) {
        *out = odin3_api_str(design, str);
    }
    return st;
}
