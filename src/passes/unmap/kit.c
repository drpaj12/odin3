/*
 * kit.c — net classes, type lookup and constants of the unmap pass (plan Tasks 2 and 3).
 */
#include "passes/unmap/kit.h"

#include "ir/prov.h"
#include "passes/opt/kit.h"
#include "passes/unmap/unmap.h"
#include "util/log.h"
#include "util/str.h"

#include <stddef.h>
#include <string.h>

enum { UM_MAX_PORTS = 16 };

/* --- net classes (N-1, N-2) ---------------------------------------------------------------- */

/* The `unconn` pad of read_blif: net `unconn` driven by one zero-input $sop with an empty cover
 * (the test of odin3_abc_net_undefined in src/passes/abc/region.c). */
static bool is_unconn(const odin3_module *module, odin3_net_id net) {
    uint32_t name = odin3_net_name(module, net);
    const odin3_design *design = odin3_module_design(module);
    if (name == 0 || strcmp(odin3_strtab_get(odin3_design_strtab(design), name), "unconn") != 0 ||
        odin3_net_driver_count(module, net) != 1) {
        return false;
    }
    odin3_node_id node = odin3_pin_node(module, odin3_net_driver(module, net));
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
    if (def == NULL || strcmp(def->name, "$sop") != 0) {
        return false;
    }
    const odin3_value *width = odin3_node_param(module, node, 0);
    const odin3_value *cover = odin3_node_param(module, node, 1);
    return width != NULL && cover != NULL && width->i == 0 && cover->len == 0;
}

um_cls um_net_class(const odin3_module *module, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return UM_NONE;
    }
    odin3_const value = odin3_net_const_value(module, net);
    if (value == ODIN3_CONST_X || is_unconn(module, net)) {
        return UM_PAD;
    }
    if (value == ODIN3_CONST_0) {
        return UM_GND;
    }
    return value == ODIN3_CONST_1 ? UM_VCC : UM_SIG;
}

bool um_observed(const odin3_module *module, odin3_net_id net) {
    return odin3_net_valid(net) && odin3_net_sinks(module, net).count > 0;
}

odin3_net_id um_port_net(const odin3_module *module, odin3_node_id node, um_pos pos) {
    odin3_pinslice pins = odin3_node_port(module, node, pos.port);
    if (pos.index >= pins.count) {
        return (odin3_net_id){0};
    }
    odin3_pin_id pin = {pins.first.v + pos.index};
    return odin3_pin_net(module, pin);
}

/* --- types (E-2, E-3) ---------------------------------------------------------------------- */

typedef struct um_shape {
    const char *type;
    uint32_t n_ports;
    const char *ports[UM_MAX_PORTS];
    uint32_t n_params;
    const char *params[2];
} um_shape;

/* The shapes of partial_map's k_shapes (src/passes/partial_map/hm_kit.c). */
static const um_shape k_shapes[] = {
    {"adder", 5, {"a", "b", "cin", "cout", "sumout"}, 0, {NULL, NULL}},
    {"multiply", 3, {"a", "b", "out"}, 1, {"WIDTH", NULL}},
    {"single_port_ram", 5, {"clk", "data", "addr", "we", "out"}, 2, {"ADDR_WIDTH", "DATA_WIDTH"}},
    {"dual_port_ram",
     9,
     {"clk", "data2", "data1", "addr2", "addr1", "we2", "we1", "out2", "out1"},
     2,
     {"ADDR_WIDTH", "DATA_WIDTH"}},
};

static bool find_type(const odin3_design *design, const char *name, odin3_celltype_id *out) {
    uint32_t str = 0;
    return odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) &&
           odin3_celltype_find(design, str, out);
}

static bool shape_matches(const odin3_celltype_def *def, const um_shape *shape) {
    if (def->gran != ODIN3_GRAN_HARD || def->n_ports != shape->n_ports ||
        def->n_params != shape->n_params) {
        return false;
    }
    for (uint32_t i = 0; i < shape->n_ports; i++) {
        if (strcmp(def->ports[i].name, shape->ports[i]) != 0) {
            return false;
        }
    }
    for (uint32_t i = 0; i < shape->n_params; i++) {
        if (strcmp(def->params[i].name, shape->params[i]) != 0) {
            return false;
        }
    }
    return true;
}

static odin3_status find_hard(const odin3_design *design, const um_shape *shape,
                              odin3_celltype_id *out) {
    odin3_celltype_id id = {0};
    *out = (odin3_celltype_id){0};
    if (!find_type(design, shape->type, &id)) {
        return ODIN3_OK;
    }
    if (!shape_matches(odin3_celltype_get(design, id), shape)) {
        odin3_log(ODIN3_LOG_ERROR, "unmap: cell type '%s' does not have the expected ports",
                  shape->type);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = id;
    return ODIN3_OK;
}

static odin3_status need_type(const odin3_design *design, const char *name,
                              odin3_celltype_id *out) {
    if (find_type(design, name, out)) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR, "unmap: built-in cell type '%s' is missing", name);
    return ODIN3_ERR_INVALID_ARG;
}

odin3_status um_types_find(const odin3_design *design, um_types *types) {
    memset(types, 0, sizeof *types);
    odin3_celltype_id *hard[] = {&types->adder, &types->multiply, &types->sp_ram, &types->dp_ram};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sizeof hard / sizeof hard[0]; i++) {
        st = find_hard(design, &k_shapes[i], hard[i]);
    }
    struct {
        const char *name;
        odin3_celltype_id *slot;
    } const words[] = {{"$add", &types->add},         {"$sub", &types->sub},
                       {"$mul", &types->mul},         {"$mem", &types->mem},
                       {"$not", &types->not_},        {"$and", &types->and_},
                       {"$mux", &types->mux},         {"$dff", &types->dff},
                       {"$_CONST0_", &types->konst0}, {"$_CONST1_", &types->konst1}};
    for (size_t i = 0; st == ODIN3_OK && i < sizeof words / sizeof words[0]; i++) {
        st = need_type(design, words[i].name, words[i].slot);
    }
    return st;
}

/* --- constants ----------------------------------------------------------------------------- */

/* The Y net of the lowest-ID live node of type `type` that has one pin on a net, else none. */
static odin3_net_id scan_const(const odin3_module *module, odin3_celltype_id type) {
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node) || odin3_node_type(module, node).v != type.v) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(module, node);
        odin3_net_id net = pins.count == 1 ? odin3_pin_net(module, pins.first) : (odin3_net_id){0};
        if (odin3_net_valid(net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

odin3_status um_const_net(um_ctx *uc, bool one, odin3_net_id *out) {
    odin3_net_id *cache = &uc->konst[one ? 1 : 0];
    odin3_celltype_id type = one ? uc->types.konst1 : uc->types.konst0;
    if (odin3_net_valid(*cache) && odin3_net_live(uc->module, *cache) &&
        odin3_net_const_value(uc->module, *cache) == (one ? ODIN3_CONST_1 : ODIN3_CONST_0)) {
        *out = *cache;
        return ODIN3_OK;
    }
    odin3_net_id net = scan_const(uc->module, type);
    if (!odin3_net_valid(net)) {
        odin3_prov_id prov = odin3_module_prov(uc->module);
        odin3_status st = odin3_net_create(uc->module, 0, prov, &net);
        odin3_netvec port = {&net, 1};
        odin3_node_spec spec = {type, 0, prov, NULL, 0};
        odin3_node_id node = {0};
        st = st == ODIN3_OK ? odin3_node_create_connected(uc->module, &spec, &port, &node) : st;
        if (st != ODIN3_OK) {
            if (odin3_net_valid(net)) {
                (void)odin3_net_delete(uc->module, net);
            }
            return st;
        }
    }
    *cache = net;
    *out = net;
    return ODIN3_OK;
}
