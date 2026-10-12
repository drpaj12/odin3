/*
 * kit.c — the opt family's rewrite kit (2F spec OP-2, OP-15; plan Task 1): the opt context, the
 * canonical constant nets, kit_connect, kit_new_cell, kit_rebuild_node, provenance, the invert
 * map, topological order, `keep` and readers; and the family's test hooks (opt_test.h).
 */
#include "passes/opt/kit.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- test hooks (opt_test.h) ---------------------------------------------------------------- */

static const odin3_opt_limits k_default_limits = {ODIN3_OPT_MAX_ITERATIONS,
                                                  ODIN3_OPT_MUXTREE_MAX_EVALS};
static odin3_opt_limits g_limits = {ODIN3_OPT_MAX_ITERATIONS, ODIN3_OPT_MUXTREE_MAX_EVALS};
static odin3_opt_test_hooks g_hooks;

static uint32_t clamp_cap(uint32_t value, uint32_t dflt) {
    if (value < 1) {
        return 1;
    }
    return value > dflt ? dflt : value;
}

void odin3_opt_test_set_limits(const odin3_opt_limits *limits) {
    if (limits == NULL) {
        g_limits = k_default_limits;
        return;
    }
    g_limits.max_iterations = clamp_cap(limits->max_iterations, ODIN3_OPT_MAX_ITERATIONS);
    g_limits.muxtree_max_evals = clamp_cap(limits->muxtree_max_evals, ODIN3_OPT_MUXTREE_MAX_EVALS);
}

odin3_opt_limits odin3_opt_limits_now(void) {
    return g_limits;
}

void odin3_opt_test_set_hooks(const odin3_opt_test_hooks *hooks) {
    if (hooks == NULL) {
        memset(&g_hooks, 0, sizeof g_hooks);
        return;
    }
    g_hooks = *hooks;
}

const odin3_opt_test_hooks *odin3_opt_test_hooks_now(void) {
    return &g_hooks;
}

/* --- small helpers -------------------------------------------------------------------------- */

odin3_celltype_id odin3_kit_type_named(const odin3_design *design, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, &id)) {
        return (odin3_celltype_id){0};
    }
    return id;
}

odin3_status odin3_kit_push_net(odin3_vec *vec, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

int odin3_kit_cmp_u64(const void *lhs, const void *rhs) {
    uint64_t left = *(const uint64_t *)lhs;
    uint64_t right = *(const uint64_t *)rhs;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

uint32_t odin3_kit_port_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    return ODIN3_KIT_NONE;
}

uint32_t odin3_kit_param_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return i;
        }
    }
    return ODIN3_KIT_NONE;
}

odin3_status odin3_kit_push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

odin3_status odin3_kit_push_u64(odin3_vec *vec, uint64_t value) {
    uint64_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

void odin3_kit_truncate(odin3_vec *vec, size_t len) {
    while (vec->len > len) {
        odin3_vec_pop(vec);
    }
}

void odin3_kit_sort_unify(odin3_vec *vec, size_t from) {
    if (vec->data == NULL || vec->len - from < 2) {
        return; /* nothing to sort (and no pointer arithmetic on a null storage) */
    }
    uint64_t *data = (uint64_t *)vec->data + from;
    size_t count = vec->len - from;
    qsort(data, count, sizeof *data, odin3_kit_cmp_u64);
    size_t kept = 1;
    for (size_t i = 1; i < count; i++) {
        if (data[i] != data[kept - 1]) {
            data[kept++] = data[i];
        }
    }
    odin3_kit_truncate(vec, from + kept);
}

void odin3_kit_node_label(const odin3_opt_ctx *oc, odin3_node_id node, char *out, size_t size) {
    uint32_t name = odin3_node_name(oc->module, node);
    const char *text = name == 0 ? NULL : odin3_strtab_get(odin3_design_strtab(oc->design), name);
    if (text != NULL) {
        (void)snprintf(out, size, "%s", text);
    } else {
        (void)snprintf(out, size, "$c%" PRIu32, node.v);
    }
}

/* The net on bit 0 of port `port` of node, none when the port is empty or unconnected. */
static odin3_net_id port_net0(const odin3_module *module, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(module, node, port);
    return pins.count > 0 ? odin3_pin_net(module, pins.first) : (odin3_net_id){0};
}

/* The definition of a live node that is neither a port node nor a $proc; NULL (logged) else. */
static const odin3_celltype_def *rewritable(const odin3_opt_ctx *oc, odin3_node_id node,
                                            const char *what) {
    const odin3_celltype_def *def =
        odin3_node_live(oc->module, node)
            ? odin3_celltype_get(oc->design, odin3_node_type(oc->module, node))
            : NULL;
    if (def == NULL || def->gran == ODIN3_GRAN_PORT || (def->flags & ODIN3_CT_PROCESS) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "opt: %s: node %u is not a live cell the kit may rewrite", what,
                  node.v);
        return NULL;
    }
    return def;
}

/* --- the context ---------------------------------------------------------------------------- */

odin3_status odin3_opt_ctx_init(odin3_opt_ctx *oc, odin3_pass_ctx *pass, odin3_module *module,
                                const odin3_opt_opts *opts) {
    if (oc == NULL || pass == NULL || module == NULL || opts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ctx_init: no context, pass run, module or options");
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(oc, 0, sizeof *oc);
    oc->pass = pass;
    oc->design = odin3_module_design(module);
    oc->module = module;
    oc->opts = opts;
    odin3_vec_init(&oc->invert, sizeof(odin3_net_id));
    odin3_vec_init(&oc->dirty, sizeof(uint32_t));
    return ODIN3_OK;
}

void odin3_opt_ctx_free(odin3_opt_ctx *oc) {
    if (oc == NULL) {
        return;
    }
    odin3_vec_free(&oc->invert);
    odin3_vec_free(&oc->dirty);
}

/* A failure after a rewrite's destructive step: the IR stays valid but may no longer compute what
 * the input did, so the caller must discard the design (logged distinctly from an undone failure).
 */
static odin3_status half_done(const odin3_opt_ctx *oc, odin3_status st) {
    if (st != ODIN3_OK) {
        const odin3_strtab *tab = odin3_design_strtab(oc->design);
        odin3_log(ODIN3_LOG_ERROR,
                  "opt: out of memory in the middle of a rewrite in module '%s': the design no "
                  "longer matches its input; discard it",
                  odin3_strtab_get(tab, odin3_module_name(oc->module)));
    }
    return st;
}

/* --- constants (OP-2) ----------------------------------------------------------------------- */

static const char *const k_const_types[ODIN3_OPT_N_CONST] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_",
                                                             "$_CONSTZ_"};
static const odin3_const k_const_values[ODIN3_OPT_N_CONST] = {ODIN3_CONST_0, ODIN3_CONST_1,
                                                              ODIN3_CONST_X, ODIN3_CONST_Z};

/* The odin3_bit a constant type drives, ODIN3_OPT_N_CONST for any other type. */
static uint32_t const_index(const odin3_celltype_id *types, odin3_celltype_id type) {
    for (uint32_t i = 0; i < ODIN3_OPT_N_CONST; i++) {
        if (types[i].v != 0 && types[i].v == type.v) {
            return i;
        }
    }
    return ODIN3_OPT_N_CONST;
}

/* A duplicate constant node: deleted, its net merged into keep (a no-op merge when they agree). */
static odin3_status drop_const(odin3_opt_ctx *oc, odin3_node_id node, odin3_net_id keep,
                               odin3_net_id net) {
    odin3_status st = odin3_node_delete(oc->module, node);
    if (st == ODIN3_OK && net.v != keep.v) {
        st = half_done(oc, odin3_net_merge(oc->module, (odin3_net_pair){keep, net}));
    }
    return st;
}

odin3_status odin3_opt_canon_consts(odin3_opt_ctx *oc) {
    odin3_celltype_id types[ODIN3_OPT_N_CONST];
    odin3_net_id kept[ODIN3_OPT_N_CONST] = {{0}};
    for (uint32_t i = 0; i < ODIN3_OPT_N_CONST; i++) {
        types[i] = odin3_kit_type_named(oc->design, k_const_types[i]);
    }
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_module_node_end(oc->module);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(oc->module, node)) {
            continue;
        }
        uint32_t idx = const_index(types, odin3_node_type(oc->module, node));
        if (idx >= ODIN3_OPT_N_CONST) {
            continue;
        }
        odin3_net_id net = port_net0(oc->module, node, 0);
        if (!odin3_net_valid(net)) {
            continue;
        }
        if (!odin3_net_valid(kept[idx])) {
            kept[idx] = net;
            continue;
        }
        st = drop_const(oc, node, kept[idx], net);
    }
    for (uint32_t i = 0; i < ODIN3_OPT_N_CONST; i++) {
        if (odin3_net_valid(kept[i])) {
            oc->const_net[i] = kept[i];
        }
    }
    return st;
}

/* The net on Y of the first live node of type in ID order whose Y is connected. */
static odin3_net_id first_const_net(const odin3_module *module, odin3_celltype_id type) {
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(module, node) && odin3_node_type(module, node).v == type.v) {
            odin3_net_id net = port_net0(module, node, 0);
            if (odin3_net_valid(net)) {
                return net;
            }
        }
    }
    return (odin3_net_id){0};
}

/* A new node of constant type `type` driving a new unnamed net, both with the module's record. */
static odin3_status new_const_net(odin3_module *module, odin3_celltype_id type, odin3_net_id *out) {
    odin3_prov_id prov = odin3_module_prov(module);
    odin3_net_id net = {0};
    odin3_status st = odin3_net_create(module, 0, prov, &net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_spec spec = {type, 0, prov, NULL, 0};
    odin3_netvec port = {&net, 1};
    odin3_node_id node = {0};
    st = odin3_node_create_connected(module, &spec, &port, &node);
    if (st != ODIN3_OK) {
        (void)odin3_net_delete(module, net); /* pinless and wireless: never fails */
        return st;
    }
    *out = net;
    return ODIN3_OK;
}

odin3_status odin3_opt_const_net(odin3_opt_ctx *oc, odin3_bit bit, odin3_net_id *out) {
    uint32_t idx = (uint32_t)bit;
    if (idx >= ODIN3_OPT_N_CONST || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: const_net: %u is not 0, 1, x or z", idx);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id cached = oc->const_net[idx];
    if (odin3_net_valid(cached) && odin3_net_live(oc->module, cached) &&
        odin3_net_const_value(oc->module, cached) == k_const_values[idx]) {
        *out = cached;
        return ODIN3_OK;
    }
    odin3_celltype_id type = odin3_kit_type_named(oc->design, k_const_types[idx]);
    if (!odin3_celltype_valid(type)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: cell type '%s' is not registered", k_const_types[idx]);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id net = first_const_net(oc->module, type);
    odin3_status st = odin3_net_valid(net) ? ODIN3_OK : new_const_net(oc->module, type, &net);
    if (st == ODIN3_OK) {
        oc->const_net[idx] = net;
        *out = net;
    }
    return st;
}

/* --- kit_connect ---------------------------------------------------------------------------- */

/* The only non-input port of def, def->n_ports when it has none or several. */
static uint32_t sole_output(const odin3_celltype_def *def) {
    uint32_t found = def->n_ports;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (def->ports[i].dir == ODIN3_DIR_IN) {
            continue;
        }
        if (found != def->n_ports) {
            return def->n_ports;
        }
        found = i;
    }
    return found;
}

static odin3_status connect_check(const odin3_opt_ctx *oc, const odin3_kit_conn *conn) {
    const odin3_celltype_def *def = rewritable(oc, conn->node, "kit_connect");
    if (def == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (conn->port >= def->n_ports || sole_output(def) != conn->port ||
        def->ports[conn->port].dir != ODIN3_DIR_OUT) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_connect: port %u is not the only output of %s",
                  conn->port, def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_node_port(oc->module, conn->node, conn->port).count != conn->width ||
        (conn->width > 0 && conn->src == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_connect: %s output is not %u bits wide", def->name,
                  conn->width);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t k = 0; k < conn->width; k++) {
        if (!odin3_net_valid(conn->src[k]) || !odin3_net_live(oc->module, conn->src[k])) {
            odin3_log(ODIN3_LOG_ERROR, "opt: kit_connect: source bit %u is not a live net", k);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

/* The live net that net stands for after the merges in `merged` (pairs keep/drop); none if lost. */
static odin3_net_id resolve(const odin3_module *module, const odin3_vec *merged, odin3_net_id net) {
    while (odin3_net_valid(net) && !odin3_net_live(module, net)) {
        odin3_net_id next = {0};
        for (size_t i = 0; i < merged->len && !odin3_net_valid(next); i++) {
            const odin3_net_pair *pair = odin3_vec_cat(merged, i);
            next = pair->drop.v == net.v ? pair->keep : next;
        }
        net = next;
    }
    return net;
}

/* The merges of kit_connect after the delete: ynets[k] into conn->src[k], recorded in merged. */
static odin3_status merge_outputs(odin3_opt_ctx *oc, const odin3_kit_conn *conn,
                                  const odin3_net_id *ynets, odin3_vec *merged) {
    for (uint32_t k = 0; k < conn->width; k++) {
        odin3_net_id drop = resolve(oc->module, merged, ynets[k]);
        odin3_net_id keep = resolve(oc->module, merged, conn->src[k]);
        if (!odin3_net_valid(drop) || !odin3_net_valid(keep) || drop.v == keep.v) {
            continue; /* unconnected, or fed back to itself: left undriven */
        }
        odin3_status st = odin3_net_merge(oc->module, (odin3_net_pair){keep, drop});
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_net_pair *slot = odin3_vec_push(merged); /* reserved: never NULL */
        if (slot != NULL) {
            *slot = (odin3_net_pair){keep, drop};
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_kit_connect(odin3_opt_ctx *oc, const odin3_kit_conn *conn) {
    if (oc == NULL || conn == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_connect: no context or request");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = connect_check(oc, conn);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pinslice pins = odin3_node_port(oc->module, conn->node, conn->port);
    odin3_net_id *ynets = odin3_util_malloc(sizeof *ynets * ((size_t)conn->width + 1));
    odin3_vec merged;
    odin3_vec_init(&merged, sizeof(odin3_net_pair));
    st = ynets != NULL ? odin3_vec_reserve(&merged, (size_t)conn->width + 1) : ODIN3_ERR_NO_MEMORY;
    for (uint32_t k = 0; st == ODIN3_OK && k < conn->width; k++) {
        ynets[k] = odin3_pin_net(oc->module, (odin3_pin_id){pins.first.v + k});
    }
    st = st == ODIN3_OK ? odin3_node_delete(oc->module, conn->node) : st;
    if (st == ODIN3_OK) {
        oc->changed = true;
        st = half_done(oc, merge_outputs(oc, conn, ynets, &merged));
    }
    odin3_vec_free(&merged);
    odin3_util_free(ynets);
    return st;
}

/* --- building cells (kit_new_cell, kit_rebuild_node) ---------------------------------------- */

/* A cell being built: its definition, parameters, one netvec per port and its fresh nets. */
typedef struct cell_plan {
    const odin3_celltype_def *def;
    const odin3_value *params; /* the cell's, or own_params (the defaults) */
    odin3_value *own_params;
    odin3_netvec *ports;
    uint32_t *widths;    /* per port, for the parameters */
    odin3_net_id *fresh; /* the fresh output nets made so far */
    uint32_t n_fresh;
    uint64_t n_pins, fresh_needed;
} cell_plan;

static void plan_close(cell_plan *plan) {
    odin3_util_free(plan->own_params);
    odin3_util_free(plan->ports);
    odin3_util_free(plan->widths);
    odin3_util_free(plan->fresh);
    memset(plan, 0, sizeof *plan);
}

/* The definition and parameters of cell (the defaults for NULL); INVALID_ARG (logged) else. */
static odin3_status plan_params(const odin3_opt_ctx *oc, const odin3_kit_cell *cell,
                                cell_plan *plan) {
    plan->def = odin3_celltype_get(oc->design, cell->type);
    if (plan->def == NULL || (cell->params != NULL && cell->n_params != plan->def->n_params)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit: no cell type %u or wrong parameters", cell->type.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (cell->params != NULL || plan->def->n_params == 0) {
        plan->params = cell->params;
        return ODIN3_OK;
    }
    plan->own_params = odin3_util_malloc(sizeof *plan->own_params * plan->def->n_params);
    if (plan->own_params == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < plan->def->n_params; i++) {
        plan->own_params[i] = plan->def->params[i].dflt;
    }
    plan->params = plan->own_params;
    return ODIN3_OK;
}

/* Sizes every port and copies the given netvecs (ports may be NULL: all entries empty). */
static odin3_status plan_open(const odin3_opt_ctx *oc, const odin3_kit_cell *cell,
                              cell_plan *plan) {
    memset(plan, 0, sizeof *plan);
    odin3_status st = plan_params(oc, cell, plan);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t n_ports = plan->def->n_ports;
    plan->ports = odin3_util_calloc(sizeof *plan->ports * ((size_t)n_ports + 1));
    plan->widths = odin3_util_calloc(sizeof *plan->widths * ((size_t)n_ports + 1));
    if (plan->ports == NULL || plan->widths == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < n_ports; i++) {
        plan->widths[i] = odin3_celltype_port_width(oc->design, cell->type, plan->params, i);
        plan->n_pins += plan->widths[i];
        if (cell->ports != NULL) {
            plan->ports[i] = cell->ports[i];
        }
        plan->fresh_needed += plan->def->ports[i].dir != ODIN3_DIR_IN ? plan->widths[i] : 0;
    }
    if (plan->n_pins > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit: %s has too many pins", plan->def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    plan->fresh = odin3_util_calloc(sizeof *plan->fresh * ((size_t)plan->fresh_needed + 1));
    return plan->fresh != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Fresh unnamed nets (record prov) for every output entry without nets. */
static odin3_status plan_fresh(odin3_opt_ctx *oc, cell_plan *plan, odin3_prov_id prov) {
    for (uint32_t i = 0; i < plan->def->n_ports; i++) {
        if (plan->def->ports[i].dir == ODIN3_DIR_IN || plan->ports[i].nets != NULL) {
            continue;
        }
        odin3_net_id *nets = plan->fresh + plan->n_fresh;
        for (uint32_t k = 0; k < plan->widths[i]; k++) {
            odin3_status st = odin3_net_create(oc->module, 0, prov, &nets[k]);
            if (st != ODIN3_OK) {
                return st;
            }
            plan->n_fresh++;
        }
        plan->ports[i] = (odin3_netvec){nets, plan->widths[i]};
    }
    return ODIN3_OK;
}

/* Deletes the fresh nets that have no pins left (a node that was not made, or was deleted). */
static void plan_undo(odin3_opt_ctx *oc, const cell_plan *plan) {
    for (uint32_t k = 0; k < plan->n_fresh; k++) {
        odin3_net_id net = plan->fresh[k];
        if (odin3_net_live(oc->module, net) && odin3_net_pins(oc->module, net).count == 0) {
            (void)odin3_net_delete(oc->module, net); /* pinless and wireless: never fails */
        }
    }
}

/* Adds pins to pins_created; on crossing a multiple of ODIN3_OPT_BUDGET_PINS checks the budget. */
static odin3_status budget_tick(odin3_opt_ctx *oc, uint64_t pins) {
    uint64_t before = oc->pins_created;
    oc->pins_created += pins;
    if (before / ODIN3_OPT_BUDGET_PINS == oc->pins_created / ODIN3_OPT_BUDGET_PINS) {
        return ODIN3_OK;
    }
    uint64_t extra = (uint64_t)oc->invert.cap * oc->invert.elem_size +
                     (uint64_t)oc->dirty.cap * oc->dirty.elem_size;
    odin3_status st = odin3_budget_check(oc->pass, oc->design, extra);
    if (st != ODIN3_ERR_PARSE) {
        return st;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    odin3_status built = odin3_budget_message(oc->pass, &msg);
    if (built == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return built == ODIN3_OK ? ODIN3_ERR_PARSE : built;
}

odin3_status odin3_kit_new_cell(odin3_opt_ctx *oc, const odin3_kit_cell *cell, odin3_node_id *out) {
    if (oc == NULL || cell == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_new_cell: no context, cell or output");
        return ODIN3_ERR_INVALID_ARG;
    }
    cell_plan plan;
    odin3_status st = plan_open(oc, cell, &plan);
    st = st == ODIN3_OK ? budget_tick(oc, plan.n_pins) : st;
    st = st == ODIN3_OK ? plan_fresh(oc, &plan, cell->prov) : st;
    if (st == ODIN3_OK) {
        odin3_node_spec spec = {cell->type, 0, cell->prov, plan.params, plan.def->n_params};
        st = odin3_node_create_connected(oc->module, &spec, plan.ports, out);
    }
    if (st != ODIN3_OK) {
        plan_undo(oc, &plan);
    } else {
        oc->changed = true;
    }
    plan_close(&plan);
    return st;
}

/* --- kit_rebuild_node ----------------------------------------------------------------------- */

/* One rebuild: the request, the old node's definition and pin nets, the plan and the new node. */
typedef struct rebuild_job {
    odin3_opt_ctx *oc;
    const odin3_kit_rebuild *req;
    const odin3_celltype_def *old_def;
    odin3_net_id *old_nets; /* one per pin of the old node, read before any change */
    odin3_pinslice old_pins;
    cell_plan plan;
    bool same; /* the replace path */
    odin3_prov_id prov;
    odin3_node_id node;
} rebuild_job;

/* The old node's nets of port `port`, none (count 0) when the old type has no such port. */
static odin3_netvec old_port_nets(const rebuild_job *job, uint32_t port) {
    const odin3_module *module = job->oc->module;
    if (port >= job->old_def->n_ports) {
        return (odin3_netvec){NULL, 0};
    }
    odin3_pinslice pins = odin3_node_port(module, job->req->old_node, port);
    if (pins.count == 0) {
        return (odin3_netvec){NULL, 0};
    }
    return (odin3_netvec){job->old_nets + (pins.first.v - job->old_pins.first.v), pins.count};
}

static bool netvec_equal(odin3_netvec lhs, odin3_netvec rhs) {
    if (lhs.count != rhs.count) {
        return false;
    }
    for (uint32_t k = 0; k < lhs.count; k++) {
        if (lhs.nets[k].v != rhs.nets[k].v) {
            return false;
        }
    }
    return true;
}

/* The k-th non-input port of def from port `from` on, def->n_ports when there is none. */
static uint32_t next_output(const odin3_celltype_def *def, uint32_t from) {
    while (from < def->n_ports && def->ports[from].dir == ODIN3_DIR_IN) {
        from++;
    }
    return from;
}

/* An output entry: given nets must be those of the old node's paired output (outputs pair in
 * order, as rb_merge_back merges them); afterwards it is empty (fresh or handed over). */
static odin3_status rb_output(rebuild_job *job, uint32_t port, odin3_netvec old) {
    cell_plan *plan = &job->plan;
    if (plan->ports[port].nets != NULL && !netvec_equal(plan->ports[port], old)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_rebuild_node: output %s is not the old node's",
                  plan->def->ports[port].name);
        return ODIN3_ERR_INVALID_ARG;
    }
    plan->ports[port] = (odin3_netvec){NULL, 0};
    job->same = job->same && old.count == plan->widths[port];
    return ODIN3_OK;
}

/* An input entry: without nets it takes the old node's (same width, an input there too). */
static odin3_status rb_input(rebuild_job *job, uint32_t port, odin3_netvec old) {
    cell_plan *plan = &job->plan;
    if (plan->ports[port].nets == NULL) {
        bool old_input =
            port < job->old_def->n_ports && job->old_def->ports[port].dir == ODIN3_DIR_IN;
        if (old.count != plan->widths[port] || (old.count > 0 && !old_input)) {
            odin3_log(ODIN3_LOG_ERROR, "opt: kit_rebuild_node: no nets for input %s",
                      plan->def->ports[port].name);
            return ODIN3_ERR_INVALID_ARG;
        }
        plan->ports[port] = old.count > 0 ? old : (odin3_netvec){job->old_nets, 0};
    }
    job->same =
        job->same && old.count == plan->widths[port] && netvec_equal(plan->ports[port], old);
    return ODIN3_OK;
}

/* Fills empty input entries from the old node, checks output entries and sets job->same. */
static odin3_status rb_ports(rebuild_job *job) {
    cell_plan *plan = &job->plan;
    job->same = job->req->cell.type.v == odin3_node_type(job->oc->module, job->req->old_node).v &&
                plan->def->n_ports == job->old_def->n_ports;
    odin3_status st = ODIN3_OK;
    uint32_t j = next_output(job->old_def, 0);
    for (uint32_t i = 0; st == ODIN3_OK && i < plan->def->n_ports; i++) {
        if (plan->def->ports[i].dir == ODIN3_DIR_IN) {
            st = rb_input(job, i, old_port_nets(job, i));
        } else {
            st = rb_output(job, i, old_port_nets(job, j));
            j = next_output(job->old_def, j + 1);
        }
    }
    return st;
}

/* The fresh path pairs output ports in order: same count, equal widths (INVALID_ARG else). */
static odin3_status rb_outputs_match(const rebuild_job *job) {
    const odin3_celltype_def *def = job->plan.def;
    uint32_t i = next_output(def, 0);
    uint32_t j = next_output(job->old_def, 0);
    while (i < def->n_ports && j < job->old_def->n_ports) {
        if (job->plan.widths[i] != old_port_nets(job, j).count) {
            break;
        }
        i = next_output(def, i + 1);
        j = next_output(job->old_def, j + 1);
    }
    if (i < def->n_ports || j < job->old_def->n_ports) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_rebuild_node: the outputs of %s and %s differ",
                  job->old_def->name, def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The record of the new node: the request's, else DERIVED from the old node's (OP-15). */
static odin3_status rb_prov(rebuild_job *job) {
    job->prov = job->req->cell.prov;
    if (job->prov.v != 0) {
        return ODIN3_OK;
    }
    if (job->oc->pass->op == 0) {
        odin3_prov_begin_op(job->oc->pass);
    }
    return odin3_kit_prov(job->oc, &job->req->old_node, 1, &job->prov);
}

/* One attribute of the old node, gathered before any is set (odin3_attr_foreach's rule). */
typedef struct attr_item {
    uint32_t key;
    const odin3_value *value;
} attr_item;

static odin3_status gather_attr(void *ctx, uint32_t key_str, const odin3_value *value) {
    attr_item *item = odin3_vec_push(ctx);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    item->key = key_str;
    item->value = value;
    return ODIN3_OK;
}

odin3_status odin3_kit_copy_attrs(odin3_module *module, odin3_node_id from, odin3_node_id to) {
    odin3_vec items;
    odin3_vec_init(&items, sizeof(attr_item));
    odin3_status st =
        odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, from.v}, gather_attr, &items);
    for (size_t i = 0; st == ODIN3_OK && i < items.len; i++) {
        const attr_item *item = odin3_vec_cat(&items, i);
        st = odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, to.v}, item->key, item->value);
    }
    odin3_vec_free(&items);
    return st;
}

/* Reads the old node's pin nets and opens the plan; validates the request. */
static odin3_status rb_open(rebuild_job *job) {
    const odin3_module *module = job->oc->module;
    job->old_def = rewritable(job->oc, job->req->old_node, "kit_rebuild_node");
    if (job->old_def == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    job->old_pins = odin3_node_pins(module, job->req->old_node);
    job->old_nets = odin3_util_calloc(sizeof *job->old_nets * ((size_t)job->old_pins.count + 1));
    if (job->old_nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < job->old_pins.count; k++) {
        job->old_nets[k] = odin3_pin_net(module, (odin3_pin_id){job->old_pins.first.v + k});
    }
    odin3_status st = plan_open(job->oc, &job->req->cell, &job->plan);
    st = st == ODIN3_OK ? rb_ports(job) : st;
    return st == ODIN3_OK && !job->same ? rb_outputs_match(job) : st;
}

/* Builds the new node (unconnected for the replace path) with the old node's attributes. */
static odin3_status rb_build(rebuild_job *job) {
    odin3_opt_ctx *oc = job->oc;
    odin3_status st = rb_prov(job);
    st = st == ODIN3_OK ? budget_tick(oc, job->plan.n_pins) : st;
    odin3_node_spec spec = {job->req->cell.type, 0, job->prov, job->plan.params,
                            job->plan.def->n_params};
    if (st == ODIN3_OK && job->same) {
        st = odin3_node_create(oc->module, &spec, &job->node);
    } else if (st == ODIN3_OK) {
        st = plan_fresh(oc, &job->plan, job->prov);
        st = st == ODIN3_OK
                 ? odin3_node_create_connected(oc->module, &spec, job->plan.ports, &job->node)
                 : st;
    }
    return st == ODIN3_OK ? odin3_kit_copy_attrs(oc->module, job->req->old_node, job->node) : st;
}

/* Undoes rb_build: the new node and its fresh nets go again (best effort under OOM). */
static void rb_undo(rebuild_job *job) {
    if (odin3_node_valid(job->node) && odin3_node_live(job->oc->module, job->node)) {
        (void)odin3_node_delete(job->oc->module, job->node);
    }
    plan_undo(job->oc, &job->plan);
}

/* Fresh path, after the old node's delete: each fresh output net merges into the old one. */
static odin3_status rb_merge_back(rebuild_job *job) {
    odin3_module *module = job->oc->module;
    const odin3_celltype_def *def = job->plan.def;
    uint32_t j = next_output(job->old_def, 0);
    for (uint32_t i = next_output(def, 0); i < def->n_ports; i = next_output(def, i + 1)) {
        odin3_netvec old = old_port_nets(job, j);
        for (uint32_t k = 0; k < old.count; k++) {
            odin3_net_id fresh = job->plan.ports[i].nets[k];
            if (!odin3_net_valid(old.nets[k])) {
                continue; /* the old pin was unconnected: the fresh net stays */
            }
            odin3_status st = odin3_net_merge(module, (odin3_net_pair){old.nets[k], fresh});
            if (st != ODIN3_OK) {
                return st;
            }
        }
        j = next_output(job->old_def, j + 1);
    }
    return ODIN3_OK;
}

/* The destructive steps: replace or delete (the one atomic step), the name, the merges. */
static odin3_status rb_commit(rebuild_job *job) {
    odin3_module *module = job->oc->module;
    uint32_t name = odin3_node_name(module, job->req->old_node);
    odin3_status st =
        job->same ? odin3_node_replace(module, (odin3_node_pair){job->req->old_node, job->node})
                  : odin3_node_delete(module, job->req->old_node);
    if (st != ODIN3_OK) {
        rb_undo(job);
        return st;
    }
    job->oc->changed = true;
    st = job->same ? ODIN3_OK : rb_merge_back(job);
    /* the name left the map with the old node, so re-inserting it does not grow the map */
    st = st == ODIN3_OK && name != 0 ? odin3_node_rename(module, job->node, name) : st;
    return half_done(job->oc, st);
}

odin3_status odin3_kit_rebuild_node(odin3_opt_ctx *oc, const odin3_kit_rebuild *req,
                                    odin3_node_id *out) {
    if (oc == NULL || req == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: kit_rebuild_node: no context, request or output");
        return ODIN3_ERR_INVALID_ARG;
    }
    rebuild_job job;
    memset(&job, 0, sizeof job);
    job.oc = oc;
    job.req = req;
    odin3_status st = rb_open(&job);
    if (st == ODIN3_OK) {
        st = rb_build(&job);
        if (st != ODIN3_OK) {
            rb_undo(&job);
        } else {
            st = rb_commit(&job);
        }
    }
    if (st == ODIN3_OK) {
        *out = job.node;
    }
    plan_close(&job.plan);
    odin3_util_free(job.old_nets);
    return st;
}

/* --- provenance (OP-15) --------------------------------------------------------------------- */

enum { PROV_STACK = 8 };

void odin3_kit_begin_rewrite(odin3_opt_ctx *oc) {
    odin3_prov_begin_op(oc->pass);
}

odin3_status odin3_kit_prov(const odin3_opt_ctx *oc, const odin3_node_id *parents, uint32_t n,
                            odin3_prov_id *out) {
    *out = (odin3_prov_id){0};
    if (n == 0) {
        return ODIN3_OK;
    }
    odin3_prov_id stack[PROV_STACK];
    odin3_prov_id *ids = n <= PROV_STACK ? stack : odin3_util_malloc(sizeof *ids * n);
    if (ids == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        odin3_prov_id prov = odin3_node_prov(oc->module, parents[i]);
        if (prov.v != 0) {
            ids[kept++] = prov;
        }
    }
    odin3_status st =
        kept > 0 ? odin3_prov_derive(oc->pass, (odin3_prov_list){ids, kept}, out) : ODIN3_OK;
    if (ids != stack) {
        odin3_util_free(ids);
    }
    return st;
}

/* --- the invert map ------------------------------------------------------------------------- */

enum { INV_UNARY = 3, INV_MUX = 2 };

/* The net node inverts (Y -> A of a 1-bit unary inverter), none otherwise. */
static odin3_net_id unary_source(const odin3_module *module, const odin3_celltype_def *def,
                                 odin3_node_id node) {
    uint32_t port_a = odin3_kit_port_index(def, "A");
    uint32_t port_y = odin3_kit_port_index(def, "Y");
    if (odin3_node_port(module, node, port_a).count != 1 ||
        odin3_node_port(module, node, port_y).count != 1) {
        return (odin3_net_id){0};
    }
    return port_net0(module, node, port_a);
}

/* S of a 1-bit mux with A = 1 and B = 0 (Y = !S), none otherwise. */
static odin3_net_id mux_source(const odin3_module *module, const odin3_celltype_def *def,
                               odin3_node_id node) {
    uint32_t port_a = odin3_kit_port_index(def, "A");
    uint32_t port_b = odin3_kit_port_index(def, "B");
    if (odin3_node_port(module, node, port_a).count != 1 ||
        odin3_node_port(module, node, port_b).count != 1 ||
        odin3_net_const_value(module, port_net0(module, node, port_a)) != ODIN3_CONST_1 ||
        odin3_net_const_value(module, port_net0(module, node, port_b)) != ODIN3_CONST_0) {
        return (odin3_net_id){0};
    }
    return port_net0(module, node, odin3_kit_port_index(def, "S"));
}

/* The source net of node when it is an inverter of the map (*y_net its Y), none otherwise. */
static odin3_net_id invert_source(const odin3_module *module, const odin3_celltype_id *types,
                                  odin3_node_id node, odin3_net_id *y_net) {
    odin3_celltype_id type = odin3_node_type(module, node);
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(module), type);
    for (uint32_t i = 0; i < INV_UNARY + INV_MUX; i++) {
        if (types[i].v != 0 && types[i].v == type.v) {
            *y_net = port_net0(module, node, odin3_kit_port_index(def, "Y"));
            return i < INV_UNARY ? unary_source(module, def, node) : mux_source(module, def, node);
        }
    }
    return (odin3_net_id){0};
}

odin3_status odin3_kit_invert_map(odin3_opt_ctx *oc) {
    static const char *const names[INV_UNARY + INV_MUX] = {"$not", "$logic_not", "$_NOT_", "$mux",
                                                           "$_MUX_"};
    odin3_celltype_id types[INV_UNARY + INV_MUX];
    for (uint32_t i = 0; i < INV_UNARY + INV_MUX; i++) {
        types[i] = odin3_kit_type_named(oc->design, names[i]);
    }
    size_t end = odin3_module_net_end(oc->module);
    odin3_vec_clear(&oc->invert);
    if (odin3_vec_reserve(&oc->invert, end) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (oc->invert.len < end) {
        (void)odin3_vec_push(&oc->invert); /* reserved; zeroed */
    }
    odin3_net_id *map = oc->invert.data;
    uint32_t node_end = odin3_module_node_end(oc->module);
    for (uint32_t id = 1; id < node_end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(oc->module, node)) {
            continue;
        }
        odin3_net_id y_net = {0};
        odin3_net_id src = invert_source(oc->module, types, node, &y_net);
        if (odin3_net_valid(src) && odin3_net_valid(y_net) && y_net.v < end) {
            map[y_net.v] = src;
        }
    }
    return ODIN3_OK;
}

odin3_net_id odin3_kit_inverse(const odin3_opt_ctx *oc, odin3_net_id net) {
    if (net.v == 0 || net.v >= oc->invert.len) {
        return (odin3_net_id){0};
    }
    return *(const odin3_net_id *)odin3_vec_cat(&oc->invert, net.v);
}

/* --- topological order ---------------------------------------------------------------------- */

/* Yosys 0.55's evaluable cells (CellTypes::setup_internals_eval, setup_stdcells_eval) that the
 * registry has. */
static const char *const k_evaluable[] = {
    "$not",         "$neg",       "$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor",
    "$reduce_bool", "$logic_not", "$lut",        "$sop",       "$and",        "$or",
    "$xor",         "$xnor",      "$shl",        "$shr",       "$sshr",       "$shiftx",
    "$lt",          "$le",        "$eq",         "$ne",        "$ge",         "$gt",
    "$add",         "$sub",       "$mul",        "$div",       "$mod",        "$pow",
    "$logic_and",   "$logic_or",  "$mux",        "$pmux",      "$_BUF_",      "$_NOT_",
    "$_AND_",       "$_NAND_",    "$_OR_",       "$_NOR_",     "$_XOR_",      "$_XNOR_",
    "$_MUX_"};

enum { N_EVALUABLE = sizeof k_evaluable / sizeof k_evaluable[0] };

/* The state of one topological sort: per node ID marks, in-degrees and successor ranges. */
typedef struct topo_state {
    const odin3_module *module;
    uint32_t end;    /* node end */
    uint8_t *eval;   /* per node ID: 1 for a live evaluable node */
    uint32_t *indeg; /* per node ID */
    uint32_t *first; /* per node ID + 1: successor range start (CSR) */
    uint32_t *succ;  /* successors, n_edges entries */
    uint32_t *heap;  /* min-heap of ready node IDs */
    uint32_t n_heap, n_eval, n_edges;
} topo_state;

/* Marks the live evaluable nodes and counts them. */
static void topo_mark(topo_state *ts, const odin3_design *design) {
    odin3_celltype_id types[N_EVALUABLE];
    for (uint32_t i = 0; i < N_EVALUABLE; i++) {
        types[i] = odin3_kit_type_named(design, k_evaluable[i]);
    }
    for (uint32_t id = 1; id < ts->end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(ts->module, node)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(ts->module, node);
        for (uint32_t i = 0; i < N_EVALUABLE; i++) {
            if (types[i].v != 0 && types[i].v == type.v) {
                ts->eval[id] = 1;
                ts->n_eval++;
                break;
            }
        }
    }
}

/* The evaluable node driving the net of pin, 0 for none. */
static uint32_t topo_driver(const topo_state *ts, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(ts->module, pin);
    odin3_pin_id driver =
        odin3_net_valid(net) ? odin3_net_driver(ts->module, net) : (odin3_pin_id){0};
    uint32_t node = odin3_pin_valid(driver) ? odin3_pin_node(ts->module, driver).v : 0;
    return node < ts->end && ts->eval[node] != 0 ? node : 0;
}

/* Visits every edge (driver -> reader); fill false counts them into first, true fills succ. */
static void topo_edges(topo_state *ts, bool fill) {
    for (uint32_t id = 1; id < ts->end; id++) {
        if (ts->eval[id] == 0) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(ts->module, (odin3_node_id){id});
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_pin_id pin = {pins.first.v + k};
            uint32_t from =
                odin3_pin_dir(ts->module, pin) == ODIN3_DIR_IN ? topo_driver(ts, pin) : 0;
            if (from == 0) {
                continue;
            }
            if (fill) {
                ts->succ[ts->first[from]++] = id;
            } else {
                ts->first[from + 1]++;
                ts->indeg[id]++;
                ts->n_edges++;
            }
        }
    }
}

static void heap_push(topo_state *ts, uint32_t id) {
    uint32_t at = ts->n_heap++;
    while (at > 0 && ts->heap[(at - 1) / 2] > id) {
        ts->heap[at] = ts->heap[(at - 1) / 2];
        at = (at - 1) / 2;
    }
    ts->heap[at] = id;
}

static uint32_t heap_pop(topo_state *ts) {
    uint32_t top = ts->heap[0];
    uint32_t last = ts->heap[--ts->n_heap];
    uint32_t at = 0;
    for (;;) {
        uint32_t child = 2 * at + 1;
        if (child >= ts->n_heap) {
            break;
        }
        if (child + 1 < ts->n_heap && ts->heap[child + 1] < ts->heap[child]) {
            child++;
        }
        if (ts->heap[child] >= last) {
            break;
        }
        ts->heap[at] = ts->heap[child];
        at = child;
    }
    if (ts->n_heap > 0) {
        ts->heap[at] = last;
    }
    return top;
}

/* Appends to order (reserved for every evaluable node) the evaluable nodes in ID order. */
static void topo_id_order(const topo_state *ts, odin3_vec *order) {
    for (uint32_t id = 1; id < ts->end; id++) {
        odin3_node_id *slot = ts->eval[id] != 0 ? odin3_vec_push(order) : NULL;
        if (slot != NULL) {
            slot->v = id;
        }
    }
}

/* Kahn's algorithm into order (reserved); *sorted false (order in ID order) when a loop remains. */
static void topo_kahn(topo_state *ts, odin3_vec *order, bool *sorted) {
    for (uint32_t id = 1; id < ts->end; id++) {
        if (ts->eval[id] != 0 && ts->indeg[id] == 0) {
            heap_push(ts, id);
        }
    }
    while (ts->n_heap > 0) {
        uint32_t id = heap_pop(ts);
        odin3_node_id *slot = odin3_vec_push(order); /* reserved: never NULL */
        if (slot != NULL) {
            slot->v = id;
        }
        for (uint32_t i = ts->first[id]; i < ts->first[id + 1]; i++) {
            if (--ts->indeg[ts->succ[i]] == 0) {
                heap_push(ts, ts->succ[i]);
            }
        }
    }
    *sorted = order->len == ts->n_eval;
    if (!*sorted) {
        odin3_vec_clear(order);
        topo_id_order(ts, order);
    }
}

/* Allocates the per-node arrays, marks, counts and fills the edges. */
static odin3_status topo_build(topo_state *ts, const odin3_design *design) {
    ts->eval = odin3_util_calloc((size_t)ts->end + 1);
    ts->indeg = odin3_util_calloc(sizeof *ts->indeg * ((size_t)ts->end + 1));
    ts->first = odin3_util_calloc(sizeof *ts->first * ((size_t)ts->end + 2));
    if (ts->eval == NULL || ts->indeg == NULL || ts->first == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    topo_mark(ts, design);
    topo_edges(ts, false);
    for (uint32_t id = 1; id <= ts->end; id++) { /* first[id] = start of id's range */
        ts->first[id] += ts->first[id - 1];
    }
    ts->succ = odin3_util_calloc(sizeof *ts->succ * ((size_t)ts->n_edges + 1));
    ts->heap = odin3_util_calloc(sizeof *ts->heap * ((size_t)ts->n_eval + 1));
    if (ts->succ == NULL || ts->heap == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    topo_edges(ts, true);                       /* advances first[id] to the end of id's range */
    for (uint32_t id = ts->end; id > 0; id--) { /* shift back: first[id] is its start again */
        ts->first[id] = ts->first[id - 1];
    }
    ts->first[0] = 0;
    return ODIN3_OK;
}

odin3_status odin3_kit_topo(const odin3_opt_ctx *oc, odin3_vec *order, bool *sorted) {
    topo_state ts;
    memset(&ts, 0, sizeof ts);
    ts.module = oc->module;
    ts.end = odin3_module_node_end(oc->module);
    odin3_vec_clear(order);
    *sorted = false;
    odin3_status st = topo_build(&ts, oc->design);
    st = st == ODIN3_OK ? odin3_vec_reserve(order, (size_t)ts.n_eval + 1) : st;
    if (st == ODIN3_OK) {
        topo_kahn(&ts, order, sorted);
    }
    if (st != ODIN3_OK) {
        odin3_vec_clear(order);
        *sorted = false;
    }
    odin3_util_free(ts.eval);
    odin3_util_free(ts.indeg);
    odin3_util_free(ts.first);
    odin3_util_free(ts.succ);
    odin3_util_free(ts.heap);
    return st;
}

/* --- keep and readers ----------------------------------------------------------------------- */

bool odin3_kit_keep(const odin3_opt_ctx *oc, odin3_objref obj) {
    const odin3_strtab *strtab = odin3_design_strtab(oc->design);
    uint32_t key = 0;
    if (!odin3_strtab_find(strtab, odin3_bytes_cstr("keep"), &key)) {
        return false; /* never interned: nothing has it */
    }
    const odin3_value *val = odin3_attr_get(oc->module, obj, key);
    if (val == NULL) {
        return false;
    }
    switch (val->kind) {
    case ODIN3_VAL_INT:
        return val->i != 0;
    case ODIN3_VAL_BITS:
        for (uint32_t k = 0; k < val->len; k++) {
            if (val->bits[k] == ODIN3_BIT_1) {
                return true;
            }
        }
        return false;
    case ODIN3_VAL_STRING: {
        const char *text = odin3_strtab_get(strtab, val->str);
        return text != NULL && text[0] != '\0';
    }
    default:
        return false;
    }
}

uint32_t odin3_kit_readers(const odin3_opt_ctx *oc, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(oc->module, net);
    uint32_t readers = 0;
    for (uint32_t i = 0; i < pins.count; i++) {
        readers += odin3_pin_reads(oc->module, pins.pins[i]) ? 1U : 0U;
    }
    return readers;
}
