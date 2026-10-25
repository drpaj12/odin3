/*
 * hm_kit.c — hardmap's local kit (hm_kit.h): the context, H-3's type lookup, the transaction
 * (begin, build on fresh nets, commit or abort), node creation with agent default (b)'s names and
 * attributes, constants, 1-bit glue and located diagnostics.
 */
#include "passes/partial_map/hm_kit.h"

#include "ast/srcman.h"
#include "ir/celltype.h"
#include "passes/budget.h"
#include "passes/opt/kit.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The class of a hard-mapping operation: a cell rewritten as a hard cell (IR-12a). */
static const odin3_opclass hardmap_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                            false, 0};

/* The design budget is checked every this many created pins (H-9). */
enum { HM_BUDGET_PINS = 1 << 16, HM_MAX_PORTS = 16, HM_MAX_NAME_TRIES = 1 << 20 };

/* The ports and parameters of a hard cell that hardmap indexes by position (H-3). */
typedef struct hm_shape {
    const char *type;
    uint32_t n_ports;
    const char *ports[HM_MAX_PORTS];
    uint32_t n_params;
    const char *params[2];
} hm_shape;

static const hm_shape k_shapes[HM_N_HARD] = {
    [HM_ADDER] = {"adder", 5, {"a", "b", "cin", "cout", "sumout"}, 0, {NULL, NULL}},
    [HM_MULTIPLY] = {"multiply", 3, {"a", "b", "out"}, 1, {"WIDTH", NULL}},
    [HM_SP_RAM] = {"single_port_ram",
                   5,
                   {"clk", "data", "addr", "we", "out"},
                   2,
                   {"ADDR_WIDTH", "DATA_WIDTH"}},
    [HM_DP_RAM] = {"dual_port_ram",
                   9,
                   {"clk", "data2", "data1", "addr2", "addr1", "we2", "we1", "out2", "out1"},
                   2,
                   {"ADDR_WIDTH", "DATA_WIDTH"}},
};

static const char *const k_glue_names[HM_N_GLUE] = {"$and", "$not", "$mux", "$dff"};
static const char *const k_const_names[HM_N_CONST] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_"};
static const odin3_const k_const_values[HM_N_CONST] = {ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_X};

/* --- context ------------------------------------------------------------------------------- */

void hm_init(hm_ctx *hc, odin3_pass_ctx *pass, odin3_design *design) {
    memset(hc, 0, sizeof *hc);
    hc->pass = pass;
    hc->design = design;
    hc->rules = pm_rules_k6_fixed();
    odin3_vec_init(&hc->made_nodes, sizeof(odin3_node_id));
    odin3_vec_init(&hc->made_nets, sizeof(odin3_net_id));
    odin3_vec_init(&hc->merged, sizeof(odin3_net_pair));
    odin3_vec_init(&hc->doomed, sizeof(odin3_node_id));
    odin3_strbuf_init(&hc->text);
}

void hm_free(hm_ctx *hc) {
    odin3_vec_free(&hc->made_nodes);
    odin3_vec_free(&hc->made_nets);
    odin3_vec_free(&hc->merged);
    odin3_vec_free(&hc->doomed);
    odin3_strbuf_free(&hc->text);
}

void hm_set_module(hm_ctx *hc, odin3_module *module) {
    hc->module = module;
    memset(hc->consts, 0, sizeof hc->consts);
}

static const char *str_of(const hm_ctx *hc, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(hc->design), str);
}

bool hm_find_type(const odin3_design *design, const char *name, odin3_celltype_id *out) {
    uint32_t str = 0;
    return odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) &&
           odin3_celltype_find(design, str, out);
}

/* --- diagnostics --------------------------------------------------------------------------- */

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

void hm_diag(const hm_ctx *hc, odin3_log_level level, odin3_node_id node, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_prov_id leaf = {0};
    odin3_prov_id prov = node.v != 0 ? odin3_node_prov(hc->module, node) : (odin3_prov_id){0};
    if (prov.v != 0) {
        (void)odin3_prov_sources(hc->design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(hc->design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(hc->design, level, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        odin3_log(level, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", str_of(hc, rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, level_word(level), msg);
    } else {
        odin3_log(level, "%s", msg);
    }
}

void hm_label(const hm_ctx *hc, odin3_node_id node, char *out, size_t size) {
    uint32_t name = odin3_node_name(hc->module, node);
    if (name != 0) {
        (void)snprintf(out, size, "%s", str_of(hc, name));
    } else {
        (void)snprintf(out, size, "$c%" PRIu32, node.v);
    }
}

odin3_status hm_budget_check(const hm_ctx *hc) {
    if (odin3_budget_check(hc->pass, hc->design, 0) == ODIN3_OK) {
        return ODIN3_OK;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    if (odin3_budget_message(hc->pass, &msg) == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return ODIN3_ERR_PARSE;
}

/* Counts pins about to be created; a multiple of 2^16 crossed checks the budget. */
static odin3_status budget_tick(hm_ctx *hc, uint64_t pins) {
    uint64_t before = hc->pins_created / HM_BUDGET_PINS;
    hc->pins_created += pins;
    return hc->pins_created / HM_BUDGET_PINS == before ? ODIN3_OK : hm_budget_check(hc);
}

odin3_status hm_budget_work(hm_ctx *hc, uint64_t units) {
    return budget_tick(hc, units);
}

/* --- hard cell types (H-3) ----------------------------------------------------------------- */

static bool shape_matches(const odin3_celltype_def *def, const hm_shape *shape) {
    if (def->n_ports != shape->n_ports || def->n_params != shape->n_params) {
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

odin3_status hm_need(hm_ctx *hc, hm_hard which, odin3_node_id at, odin3_celltype_id *out) {
    if (hc->hard[which].v != 0) {
        *out = hc->hard[which];
        return ODIN3_OK;
    }
    const hm_shape *shape = &k_shapes[which];
    odin3_celltype_id id = {0};
    const odin3_celltype_def *def =
        hm_find_type(hc->design, shape->type, &id) ? odin3_celltype_get(hc->design, id) : NULL;
    const char *why = NULL;
    if (def == NULL) {
        why = "is not registered";
    } else if (def->gran != ODIN3_GRAN_HARD) {
        why = "is not a hard block";
    } else if (!shape_matches(def, shape)) {
        why = "does not have the expected ports and parameters";
    }
    if (why != NULL) {
        hm_diag(hc, ODIN3_LOG_ERROR, at,
                "partial_map: cell type '%s' %s; read_techlib lib/vtr.o3lib first", shape->type,
                why);
        return ODIN3_ERR_INVALID_ARG;
    }
    hc->hard[which] = id;
    *out = id;
    return ODIN3_OK;
}

/* --- the transaction ----------------------------------------------------------------------- */

odin3_status hm_begin_cell(hm_ctx *hc, odin3_node_id src) {
    hc->src = src;
    hc->first = (odin3_node_id){0};
    hc->src_name = odin3_node_name(hc->module, src);
    hc->prov = (odin3_prov_id){0};
    odin3_vec_clear(&hc->made_nodes);
    odin3_vec_clear(&hc->made_nets);
    odin3_vec_clear(&hc->doomed);
    odin3_prov_begin_op_class(hc->pass, &hardmap_class);
    odin3_prov_id parent = odin3_node_prov(hc->module, src);
    if (parent.v == 0) {
        return ODIN3_OK;
    }
    return odin3_prov_derive(hc->pass, (odin3_prov_list){&parent, 1}, &hc->prov);
}

odin3_status hm_doom(hm_ctx *hc, odin3_node_id node) {
    odin3_node_id *slot = odin3_vec_push(&hc->doomed);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

odin3_status hm_fresh_nets(hm_ctx *hc, uint32_t n, odin3_net_id *out) {
    odin3_status st = odin3_vec_reserve(&hc->made_nets, hc->made_nets.len + n);
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = odin3_net_create(hc->module, 0, hc->prov, &out[i]);
        odin3_net_id *slot = st == ODIN3_OK ? odin3_vec_push(&hc->made_nets) : NULL;
        if (slot != NULL) {
            *slot = out[i];
        }
    }
    return st;
}

/* Sets the string attribute `hardmap` = "<type>:<role>" on node. */
static odin3_status set_hardmap_attr(hm_ctx *hc, odin3_node_id node, const char *type,
                                     const char *role) {
    uint32_t key = 0;
    uint32_t val = 0;
    odin3_strbuf_clear(&hc->text);
    odin3_status st = odin3_strbuf_appendf(&hc->text, "%s:%s", type, role);
    st = st == ODIN3_OK ? odin3_design_intern(hc->design, odin3_bytes_cstr(hc->text.data), &val)
                        : st;
    st = st == ODIN3_OK ? odin3_design_intern(hc->design, odin3_bytes_cstr("hardmap"), &key) : st;
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, val, 0};
    return st == ODIN3_OK
               ? odin3_attr_set(hc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &value)
               : st;
}

#define NOCARRY_KEY "partial_map_nocarry"

odin3_status hm_mark_nocarry(hm_ctx *hc, odin3_node_id node) {
    uint32_t key = 0;
    odin3_status st = odin3_design_intern(hc->design, odin3_bytes_cstr(NOCARRY_KEY), &key);
    odin3_value value = odin3_value_int(1);
    return st == ODIN3_OK
               ? odin3_attr_set(hc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &value)
               : st;
}

bool hm_is_nocarry(const hm_ctx *hc, odin3_node_id node) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(hc->design), odin3_bytes_cstr(NOCARRY_KEY), &key)) {
        return false;
    }
    const odin3_value *value =
        odin3_attr_get(hc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
    return value != NULL && value->kind == ODIN3_VAL_INT && value->i == 1;
}

/* The marker is internal: no node of the output carries it (the namesake adder copies the source's
 * attributes, and a split `$add` left soft by `--min-add` keeps it). */
void hm_strip_nocarry(hm_ctx *hc) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(hc->design), odin3_bytes_cstr(NOCARRY_KEY), &key)) {
        return;
    }
    for (uint32_t mi = 1; mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = odin3_module_get(hc->design, (odin3_module_id){mi});
        for (uint32_t id = 1; mod != NULL && id < odin3_module_node_end(mod); id++) {
            if (odin3_node_live(mod, (odin3_node_id){id})) {
                (void)odin3_attr_remove(mod, (odin3_objref){ODIN3_OBJ_NODE, id}, key);
            }
        }
    }
}

static bool is_glue_type(const hm_ctx *hc, odin3_celltype_id type) {
    for (uint32_t i = 0; i < HM_N_GLUE; i++) {
        if (hc->glue[i].v == type.v) {
            return true;
        }
    }
    return false;
}

/* The namesake of a transaction is the first node it makes that is not glue: it takes the
 * source's name and attributes at commit. Glue made before it is named like any other node. */
static bool is_namesake(const hm_ctx *hc, odin3_celltype_id type) {
    return hc->first.v == 0 && !is_glue_type(hc, type);
}

/* The name of a node other than the namesake: `<source name>.<role>`, then `_1`, `_2` ... while
 * taken; 0 for the namesake (it takes the source's name at commit). */
static odin3_status node_name(hm_ctx *hc, const char *role, bool namesake, uint32_t *out) {
    *out = 0;
    if (namesake) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(&hc->text);
    odin3_status st = hc->src_name != 0
                          ? odin3_strbuf_appendf(&hc->text, "%s.%s", str_of(hc, hc->src_name), role)
                          : odin3_strbuf_appendf(&hc->text, "$c%" PRIu32 ".%s", hc->src.v, role);
    size_t base = hc->text.len;
    for (uint32_t tries = 0; st == ODIN3_OK && tries < HM_MAX_NAME_TRIES; tries++) {
        st = tries > 0 ? odin3_strbuf_appendf(&hc->text, "_%" PRIu32, tries) : st;
        st = st == ODIN3_OK ? odin3_design_intern(hc->design, odin3_bytes_cstr(hc->text.data), out)
                            : st;
        bool taken = st == ODIN3_OK && odin3_node_valid(odin3_module_find_node(hc->module, *out));
        hc->text.len = base;
        hc->text.data[base] = '\0';
        if (!taken) {
            break;
        }
    }
    return st;
}

static uint64_t request_pins(const hm_ctx *hc, const hm_node_req *req) {
    const odin3_celltype_def *def = odin3_celltype_get(hc->design, req->type);
    uint64_t pins = 0;
    for (uint32_t i = 0; def != NULL && i < def->n_ports; i++) {
        pins += req->ports[i].count;
    }
    return pins;
}

/* The attributes of a new node: the source's (the namesake only) and `hardmap`. */
static odin3_status node_attrs(hm_ctx *hc, odin3_node_id node, const hm_node_req *req,
                               bool namesake) {
    odin3_status st = ODIN3_OK;
    if (namesake) {
        st = odin3_kit_copy_attrs(hc->module, hc->src, node);
    }
    const odin3_celltype_def *def = odin3_celltype_get(hc->design, req->type);
    return st == ODIN3_OK ? set_hardmap_attr(hc, node, def->name, req->role) : st;
}

odin3_status hm_new_node(hm_ctx *hc, const hm_node_req *req, odin3_node_id *out) {
    uint32_t name = 0;
    bool namesake = is_namesake(hc, req->type);
    odin3_status st = budget_tick(hc, request_pins(hc, req));
    st = st == ODIN3_OK ? odin3_vec_reserve(&hc->made_nodes, hc->made_nodes.len + 1) : st;
    st = st == ODIN3_OK ? node_name(hc, req->role, namesake, &name) : st;
    odin3_node_spec spec = {req->type, name, hc->prov, req->params, req->n_params};
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(hc->module, &spec, req->ports, &node) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id *slot = odin3_vec_push(&hc->made_nodes);
    *slot = node; /* reserved above: never NULL */
    st = node_attrs(hc, node, req, namesake);
    if (st != ODIN3_OK) {
        return st; /* hm_abort deletes the node: it is in made_nodes */
    }
    if (namesake) {
        hc->first = node;
    }
    *out = node;
    return ODIN3_OK;
}

void hm_abort(hm_ctx *hc) {
    for (size_t i = hc->made_nodes.len; i > 0; i--) {
        const odin3_node_id *node = odin3_vec_cat(&hc->made_nodes, i - 1);
        if (odin3_node_live(hc->module, *node)) {
            (void)odin3_node_delete(hc->module, *node);
        }
    }
    for (size_t i = hc->made_nets.len; i > 0; i--) {
        const odin3_net_id *net = odin3_vec_cat(&hc->made_nets, i - 1);
        if (odin3_net_live(hc->module, *net)) {
            (void)odin3_net_delete(hc->module, *net);
        }
    }
    odin3_vec_clear(&hc->made_nodes);
    odin3_vec_clear(&hc->made_nets);
    hc->first = (odin3_node_id){0};
}

/* The live net that `net` stands for after the merges made so far (none: gone). */
static odin3_net_id resolve(const hm_ctx *hc, odin3_net_id net) {
    while (odin3_net_valid(net) && !odin3_net_live(hc->module, net)) {
        odin3_net_id next = {0};
        for (size_t i = 0; i < hc->merged.len && !odin3_net_valid(next); i++) {
            const odin3_net_pair *pair = odin3_vec_cat(&hc->merged, i);
            next = pair->drop.v == net.v ? pair->keep : next;
        }
        net = next;
    }
    return net;
}

static odin3_status make_ties(hm_ctx *hc, const hm_tie *ties, size_t n_ties) {
    for (size_t i = 0; i < n_ties; i++) {
        odin3_net_id keep = resolve(hc, ties[i].keep);
        odin3_net_id drop = resolve(hc, ties[i].drop);
        if (!odin3_net_valid(keep) || !odin3_net_valid(drop) || keep.v == drop.v) {
            continue;
        }
        odin3_status st = odin3_net_merge(hc->module, (odin3_net_pair){keep, drop});
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_net_pair *slot = odin3_vec_push(&hc->merged); /* reserved: never NULL */
        *slot = (odin3_net_pair){keep, drop};
    }
    return ODIN3_OK;
}

/* After the delete: the first node's name, then the ties. */
static odin3_status finish_commit(hm_ctx *hc, const hm_tie *ties, size_t n_ties) {
    odin3_status st = ODIN3_OK;
    if (hc->first.v != 0 && hc->src_name != 0) {
        st = odin3_node_rename(hc->module, hc->first, hc->src_name);
    }
    return st == ODIN3_OK ? make_ties(hc, ties, n_ties) : st;
}

static void log_discard(const hm_ctx *hc) {
    odin3_log(ODIN3_LOG_ERROR,
              "partial_map: out of memory in the middle of a mapping in module '%s': the design "
              "no longer matches its input; discard it",
              str_of(hc, odin3_module_name(hc->module)));
}

/* The nodes hm_doom listed (the flip-flops R-8 merges). They are dead once the source is gone:
 * their pins leave their nets (that never allocates), which makes the new RAM the only driver of
 * the merged nets. The delete itself records the pin nets and can run out of memory; that is
 * harmless here (a dead flip-flop with no connections stays for `opt`, H-2), so it is the last
 * step, after the one delete that must not fail midway (H-9). */
static void release_doomed(hm_ctx *hc) {
    for (size_t i = 0; i < hc->doomed.len; i++) {
        odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&hc->doomed, i);
        if (!odin3_node_live(hc->module, node)) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(hc->module, node);
        for (uint32_t pin = 0; pin < pins.count; pin++) {
            (void)odin3_pin_disconnect(hc->module, (odin3_pin_id){pins.first.v + pin});
        }
        (void)odin3_node_delete(hc->module, node);
    }
}

odin3_status hm_commit(hm_ctx *hc, const hm_tie *ties, size_t n_ties) {
    odin3_vec_clear(&hc->merged);
    odin3_status st = odin3_vec_reserve(&hc->merged, n_ties);
    /* The source goes first and alone: its delete is the only step that can fail and leave
     * the design as it was (H-9); everything after it cannot fail for lack of memory. */
    st = st == ODIN3_OK ? odin3_node_delete(hc->module, hc->src) : st;
    if (st != ODIN3_OK) {
        hm_abort(hc);
        return st;
    }
    release_doomed(hc);
    st = finish_commit(hc, ties, n_ties);
    if (st != ODIN3_OK) {
        log_discard(hc);
    }
    odin3_vec_clear(&hc->made_nodes);
    odin3_vec_clear(&hc->made_nets);
    return st;
}

/* --- constants (H-5) ----------------------------------------------------------------------- */

static bool const_type_known(hm_ctx *hc, hm_bit which) {
    return hc->konst[which].v != 0 ||
           hm_find_type(hc->design, k_const_names[which], &hc->konst[which]);
}

/* The Y net of live node when it is a constant node of the given value with a net, else none. */
static odin3_net_id const_net_of(const hm_ctx *hc, odin3_node_id node, hm_bit which) {
    if (!odin3_node_live(hc->module, node) ||
        odin3_node_type(hc->module, node).v != hc->konst[which].v) {
        return (odin3_net_id){0};
    }
    odin3_pinslice pins = odin3_node_pins(hc->module, node);
    return pins.count == 1 ? odin3_pin_net(hc->module, pins.first) : (odin3_net_id){0};
}

static odin3_net_id scan_const(const hm_ctx *hc, hm_bit which) {
    for (uint32_t i = 1; i < odin3_module_node_end(hc->module); i++) {
        odin3_net_id net = const_net_of(hc, (odin3_node_id){i}, which);
        if (odin3_net_valid(net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

/* A new constant node and net. They join the transaction's made lists, so hm_abort undoes them; a
 * commit keeps them. */
static odin3_status make_const(hm_ctx *hc, hm_bit which, odin3_net_id *out) {
    odin3_prov_id prov = odin3_module_prov(hc->module);
    odin3_net_id net = {0};
    odin3_status st = odin3_vec_reserve(&hc->made_nets, hc->made_nets.len + 1);
    st = st == ODIN3_OK ? odin3_vec_reserve(&hc->made_nodes, hc->made_nodes.len + 1) : st;
    st = st == ODIN3_OK ? odin3_net_create(hc->module, 0, prov, &net) : st;
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {hc->konst[which], 0, prov, NULL, 0};
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(hc->module, &spec, &port, &node) : st;
    if (st != ODIN3_OK) {
        if (odin3_net_valid(net)) {
            (void)odin3_net_delete(hc->module, net);
        }
        return st;
    }
    odin3_net_id *net_slot = odin3_vec_push(&hc->made_nets);    /* reserved: never NULL */
    odin3_node_id *node_slot = odin3_vec_push(&hc->made_nodes); /* likewise */
    *net_slot = net;
    *node_slot = node;
    *out = net;
    return ODIN3_OK;
}

odin3_status hm_const(hm_ctx *hc, hm_bit which, odin3_net_id *out) {
    if (odin3_net_valid(hc->consts[which]) && odin3_net_live(hc->module, hc->consts[which]) &&
        odin3_net_const_value(hc->module, hc->consts[which]) == k_const_values[which]) {
        *out = hc->consts[which];
        return ODIN3_OK;
    }
    if (!const_type_known(hc, which)) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: built-in cell type '%s' is missing",
                  k_const_names[which]);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id net = scan_const(hc, which);
    odin3_status st = odin3_net_valid(net) ? ODIN3_OK : make_const(hc, which, &net);
    if (st == ODIN3_OK) {
        hc->consts[which] = net;
        *out = net;
    }
    return st;
}

/* --- glue (H-6) ---------------------------------------------------------------------------- */

static odin3_status glue_type(hm_ctx *hc, hm_glue which, odin3_celltype_id *out) {
    if (hc->glue[which].v == 0 &&
        !hm_find_type(hc->design, k_glue_names[which], &hc->glue[which])) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: built-in cell type '%s' is missing",
                  k_glue_names[which]);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = hc->glue[which];
    return ODIN3_OK;
}

/* A width-1 glue node with the input nets given (one per port but the last), a fresh output. */
static odin3_status glue_node(hm_ctx *hc, hm_glue which, const char *role, const odin3_net_id *in,
                              odin3_net_id *out) {
    static const uint32_t k_n_in[HM_N_GLUE] = {2, 1, 3, 2};
    odin3_celltype_id type = {0};
    odin3_net_id fresh = {0};
    odin3_status st = glue_type(hc, which, &type);
    st = st == ODIN3_OK ? hm_fresh_nets(hc, 1, &fresh) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_netvec ports[HM_MAX_PORTS];
    uint32_t n_in = k_n_in[which];
    for (uint32_t i = 0; i < n_in; i++) {
        ports[i] = (odin3_netvec){&in[i], 1};
    }
    ports[n_in] = (odin3_netvec){&fresh, 1};
    odin3_node_id node = {0};
    st = hm_new_node(hc, &(hm_node_req){type, NULL, 0, ports, role}, &node);
    if (st == ODIN3_OK) {
        *out = fresh;
    }
    return st;
}

odin3_status hm_not(hm_ctx *hc, const char *role, odin3_net_id in, odin3_net_id *out) {
    return glue_node(hc, HM_G_NOT, role, &in, out);
}

odin3_status hm_and(hm_ctx *hc, const char *role, const odin3_net_id in[2], odin3_net_id *out) {
    return glue_node(hc, HM_G_AND, role, in, out);
}

odin3_status hm_mux(hm_ctx *hc, const char *role, odin3_net_id sel, const odin3_net_id in[2],
                    odin3_net_id *out) {
    const odin3_net_id nets[3] = {in[0], in[1], sel}; /* $mux ports: A, B, S */
    return glue_node(hc, HM_G_MUX, role, nets, out);
}

odin3_status hm_dff(hm_ctx *hc, const char *role, const odin3_net_id in[2], odin3_net_id *out) {
    return glue_node(hc, HM_G_DFF, role, in, out); /* $dff ports: CLK, D, Q */
}
