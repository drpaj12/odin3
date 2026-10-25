/*
 * kit.c — net classes, type lookup and constants of the unmap pass (plan Tasks 2 and 3).
 */
#include "passes/unmap/kit.h"

#include "ast/srcman.h"
#include "ir/prov.h"
#include "passes/opt/kit.h"
#include "passes/unmap/unmap.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The class of an unmap operation: the same logic regrouped, not blended (IR-12a). */
static const odin3_opclass unmap_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                          false, 0};

enum { UM_MAX_PORTS = 16, UM_TEXT_CAP = 256, UM_MSG_CAP = 1024 };

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

/* --- groups and the replacement transaction (S-5, P-1, P-2) -------------------------------- */

void um_group_init(um_group *group) {
    odin3_vec_init(&group->nodes, sizeof(odin3_node_id));
}

void um_group_free(um_group *group) {
    odin3_vec_free(&group->nodes);
}

odin3_status um_group_add(um_group *group, odin3_node_id node) {
    odin3_node_id *slot = odin3_vec_push(&group->nodes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

static int cmp_node(const void *lhs, const void *rhs) {
    uint32_t left = ((const odin3_node_id *)lhs)->v;
    uint32_t right = ((const odin3_node_id *)rhs)->v;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* The group's node IDs in ascending order (a sorted copy; the caller frees it). */
static odin3_status sorted_nodes(const um_group *group, odin3_vec *out) {
    odin3_vec_init(out, sizeof(odin3_node_id));
    odin3_status st = odin3_vec_copy(out, &group->nodes);
    if (st == ODIN3_OK && out->len > 1) {
        qsort(out->data, out->len, sizeof(odin3_node_id), cmp_node);
    }
    return st;
}

/* S-5 step 1: one DERIVED record over the records of the old nodes (node-ID order, a repeated
 * record once); 0 when no old node has one. */
static odin3_status make_prov(um_ctx *uc, const odin3_vec *sorted, odin3_prov_id *out) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    const odin3_node_id *nodes = sorted->data;
    odin3_status st = ODIN3_OK;
    *out = (odin3_prov_id){0};
    for (size_t i = 0; st == ODIN3_OK && i < sorted->len; i++) {
        odin3_prov_id prov = odin3_node_prov(uc->module, nodes[i]);
        odin3_prov_id *slot = prov.v != 0 ? odin3_vec_push(&parents) : NULL;
        if (prov.v != 0 && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else if (slot != NULL) {
            *slot = prov;
        }
    }
    if (st == ODIN3_OK && parents.len > 0) {
        odin3_prov_begin_op_class(uc->pass, &unmap_class);
        st = odin3_prov_derive(uc->pass, (odin3_prov_list){parents.data, (uint32_t)parents.len},
                               out);
    }
    odin3_vec_free(&parents);
    return st;
}

static odin3_status set_attr(um_ctx *uc, odin3_node_id node, const char *key,
                             const odin3_value *value) {
    uint32_t key_str = 0;
    odin3_status st = odin3_design_intern(uc->design, odin3_bytes_cstr(key), &key_str);
    return st == ODIN3_OK
               ? odin3_attr_set(uc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key_str, value)
               : st;
}

/* A string value for set_attr. */
static odin3_status str_value(um_ctx *uc, const char *text, odin3_value *out) {
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(uc->design, odin3_bytes_cstr(text), &str);
    *out = (odin3_value){ODIN3_VAL_STRING, 0, NULL, 0, str, 0};
    return st;
}

odin3_status um_set_note(um_ctx *uc, odin3_node_id node, const char *text) {
    odin3_value value;
    odin3_status st = str_value(uc, text, &value);
    return st == ODIN3_OK ? set_attr(uc, node, "unmap_note", &value) : st;
}

odin3_status um_set_cand(um_ctx *uc, odin3_node_id node, const char *text) {
    odin3_value value;
    odin3_status st = str_value(uc, text, &value);
    return st == ODIN3_OK ? set_attr(uc, node, "unmap_cand", &value) : st;
}

static odin3_status set_int(um_ctx *uc, odin3_node_id node, const char *key, int64_t num) {
    odin3_value value = odin3_value_int(num);
    return set_attr(uc, node, key, &value);
}

static odin3_status drop_attr(um_ctx *uc, odin3_node_id node, const char *key) {
    uint32_t key_str = 0;
    odin3_status st = odin3_design_intern(uc->design, odin3_bytes_cstr(key), &key_str);
    return st == ODIN3_OK
               ? odin3_attr_remove(uc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key_str)
               : st;
}

/* S-5 step 3 and P-2: the namesake's attributes, without `hardmap` and the nocarry marker, then
 * the unmap attributes. */
static odin3_status set_attrs(um_ctx *uc, const um_group *group, const um_new *req,
                              odin3_node_id node) {
    odin3_status st = odin3_kit_copy_attrs(uc->module, req->namesake, node);
    st = st == ODIN3_OK ? drop_attr(uc, node, "hardmap") : st;
    st = st == ODIN3_OK ? drop_attr(uc, node, "partial_map_nocarry") : st;
    odin3_value rule;
    st = st == ODIN3_OK ? str_value(uc, req->rule, &rule) : st;
    st = st == ODIN3_OK ? set_attr(uc, node, "unmap", &rule) : st;
    st = st == ODIN3_OK ? set_int(uc, node, "unmap_pieces", (int64_t)group->nodes.len) : st;
    st = st == ODIN3_OK && req->origin != 0 ? set_int(uc, node, "unmap_origin", req->origin) : st;
    return st == ODIN3_OK && req->w != 0 ? set_int(uc, node, "unmap_w", req->w) : st;
}

/* S-5 step 4: deletes the group; *done counts the nodes gone when it fails. */
static odin3_status delete_group(um_ctx *uc, const odin3_vec *sorted, size_t *done) {
    const odin3_node_id *nodes = sorted->data;
    for (*done = 0; *done < sorted->len; (*done)++) {
        odin3_status st = odin3_node_delete(uc->module, nodes[*done]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status um_replace(um_ctx *uc, const um_group *group, const um_new *req, odin3_node_id *out) {
    odin3_vec sorted;
    odin3_prov_id prov = {0};
    odin3_node_id node = {0};
    odin3_status st = sorted_nodes(group, &sorted);
    st = st == ODIN3_OK ? make_prov(uc, &sorted, &prov) : st;
    odin3_node_spec spec = {req->type, 0, prov, req->params, req->n_params};
    st = st == ODIN3_OK ? odin3_node_create_connected(uc->module, &spec, req->ports, &node) : st;
    if (st != ODIN3_OK) {
        odin3_vec_free(&sorted);
        return st;
    }
    uint32_t name = odin3_node_name(uc->module, req->namesake);
    st = set_attrs(uc, group, req, node);
    size_t done = 0;
    st = st == ODIN3_OK ? delete_group(uc, &sorted, &done) : st;
    if (st != ODIN3_OK && done == 0) {
        /* nothing of the group is gone: take the new cell back so it never double-drives */
        (void)odin3_node_delete(uc->module, node);
    } else if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR,
                  "unmap: replacing a group failed (%d): the module is partly changed", (int)st);
    }
    st = st == ODIN3_OK && name != 0 ? odin3_node_rename(uc->module, node, name) : st;
    odin3_vec_free(&sorted);
    *out = node;
    return st;
}

/* --- declined groups (P-3) ----------------------------------------------------------------- */

static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *slot = user;
    if (slot->v == 0) {
        *slot = leaf;
    }
}

static const char *level_word(odin3_log_level level) {
    switch (level) {
    case ODIN3_LOG_ERROR:
        return "error";
    case ODIN3_LOG_WARN:
        return "warning";
    case ODIN3_LOG_INFO:
        return "info";
    default:
        return "debug";
    }
}

static const char *str_text(const um_ctx *uc, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(uc->design), str);
}

/* A diagnostic located at the first source leaf of the node's provenance, else bare text. */
static void diag_at(const um_ctx *uc, odin3_log_level level, odin3_node_id node, const char *msg) {
    odin3_prov_id leaf = {0};
    odin3_prov_id prov = odin3_node_prov(uc->module, node);
    if (prov.v != 0) {
        (void)odin3_prov_sources(uc->design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(uc->design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(uc->design, level, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        odin3_log(level, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", str_text(uc, rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, level_word(level), msg);
    } else {
        odin3_log(level, "%s", msg);
    }
}

odin3_status um_decline(um_ctx *uc, const odin3_node_id *cells, size_t count, um_why why) {
    char note[UM_TEXT_CAP];
    (void)snprintf(note, sizeof note, "%s:%s", why.kind, why.reason);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = um_set_note(uc, cells[i], note);
    }
    if (st != ODIN3_OK || count == 0) {
        return st;
    }
    char label[UM_TEXT_CAP];
    uint32_t name = odin3_node_name(uc->module, cells[0]);
    if (name != 0) {
        (void)snprintf(label, sizeof label, "%s", str_text(uc, name));
    } else {
        (void)snprintf(label, sizeof label, "$c%" PRIu32, cells[0].v);
    }
    char msg[UM_MSG_CAP];
    (void)snprintf(msg, sizeof msg, "unmap: module '%s': %s at '%s' not recovered: %s",
                   str_text(uc, odin3_module_name(uc->module)), why.kind, label, why.reason);
    diag_at(uc, ODIN3_LOG_WARN, cells[0], msg);
    uc->stats.declined++;
    return ODIN3_OK;
}

/* --- attribute reads ----------------------------------------------------------------------- */

static const odin3_value *attr_value(const um_ctx *uc, odin3_node_id node, const char *key) {
    uint32_t key_str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(uc->design), odin3_bytes_cstr(key), &key_str)) {
        return NULL;
    }
    return odin3_attr_get(uc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key_str);
}

const char *um_attr_text(const um_ctx *uc, odin3_node_id node, const char *key) {
    const odin3_value *value = attr_value(uc, node, key);
    return value != NULL && value->kind == ODIN3_VAL_STRING ? str_text(uc, value->str) : NULL;
}

bool um_attr_int(const um_ctx *uc, odin3_node_id node, const char *key, int64_t *out) {
    const odin3_value *value = attr_value(uc, node, key);
    if (value == NULL || value->kind != ODIN3_VAL_INT) {
        return false;
    }
    *out = value->i;
    return true;
}
