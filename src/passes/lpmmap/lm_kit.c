/*
 * lm_kit.c — lpmmap's local kit (lm_kit.h): the context, P-2's type lookup, the transaction
 * (begin, build on fresh nets, commit or abort), node creation by port and parameter name with
 * the names and attributes of H-6/H-7, constants, operand extension (H-2) and diagnostics.
 */
#include "passes/lpmmap/lm_kit.h"

#include "ast/srcman.h"
#include "ir/celltype.h"
#include "passes/budget.h"
#include "passes/opt/kit.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The design budget is checked every this many created pins (H-8). */
enum {
    LM_BUDGET_PINS = 1 << 16,
    LM_MAX_PORTS = 32,
    LM_MAX_PARAMS = 40,
    LM_MAX_NAME_TRIES = 1 << 20
};

static const char *const k_type_names[LM_N_TYPES] = {"lpm_add_sub", "lpm_mult", "lpm_compare",
                                                     "altsyncram"};
static const char *const k_const_names[LM_N_CONST] = {"$_CONST0_", "$_CONST1_"};
static const odin3_const k_const_values[LM_N_CONST] = {ODIN3_CONST_0, ODIN3_CONST_1};

const char *lm_type_name(lm_type which) {
    return k_type_names[which];
}

/* --- context ------------------------------------------------------------------------------- */

void lm_init(lm_ctx *lc, odin3_pass_ctx *pass, odin3_design *design, const char *family) {
    memset(lc, 0, sizeof *lc);
    lc->pass = pass;
    lc->design = design;
    lc->family = family != NULL ? family : ODIN3_LPMMAP_DEFAULT_FAMILY;
    odin3_vec_init(&lc->made_nodes, sizeof(odin3_node_id));
    odin3_vec_init(&lc->made_nets, sizeof(odin3_net_id));
    odin3_vec_init(&lc->merged, sizeof(odin3_net_pair));
    odin3_vec_init(&lc->doomed, sizeof(odin3_node_id));
    odin3_vec_init(&lc->scratch, sizeof(odin3_net_id));
    odin3_strbuf_init(&lc->text);
}

void lm_free(lm_ctx *lc) {
    odin3_vec_free(&lc->made_nodes);
    odin3_vec_free(&lc->made_nets);
    odin3_vec_free(&lc->merged);
    odin3_vec_free(&lc->doomed);
    odin3_vec_free(&lc->scratch);
    odin3_strbuf_free(&lc->text);
}

void lm_set_module(lm_ctx *lc, odin3_module *module) {
    lc->module = module;
    memset(lc->consts, 0, sizeof lc->consts);
}

static const char *str_of(const lm_ctx *lc, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(lc->design), str);
}

bool lm_find_type(const odin3_design *design, const char *name, odin3_celltype_id *out) {
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

void lm_diag(const lm_ctx *lc, odin3_log_level level, odin3_node_id node, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_prov_id leaf = {0};
    odin3_prov_id prov = node.v != 0 ? odin3_node_prov(lc->module, node) : (odin3_prov_id){0};
    if (prov.v != 0) {
        (void)odin3_prov_sources(lc->design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(lc->design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(lc->design, level, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        odin3_log(level, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", str_of(lc, rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, level_word(level), msg);
    } else {
        odin3_log(level, "%s", msg);
    }
}

void lm_label(const lm_ctx *lc, odin3_node_id node, char *out, size_t size) {
    uint32_t name = odin3_node_name(lc->module, node);
    if (name != 0) {
        (void)snprintf(out, size, "%s", str_of(lc, name));
    } else {
        (void)snprintf(out, size, "$c%" PRIu32, node.v);
    }
}

odin3_status lm_budget_check(const lm_ctx *lc) {
    if (odin3_budget_check(lc->pass, lc->design, 0) == ODIN3_OK) {
        return ODIN3_OK;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    if (odin3_budget_message(lc->pass, &msg) == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    return ODIN3_ERR_PARSE;
}

/* Counts pins about to be created; a multiple of 2^16 crossed checks the budget. */
static odin3_status budget_tick(lm_ctx *lc, uint64_t pins) {
    uint64_t before = lc->pins_created / LM_BUDGET_PINS;
    lc->pins_created += pins;
    return lc->pins_created / LM_BUDGET_PINS == before ? ODIN3_OK : lm_budget_check(lc);
}

/* --- the LPM types (P-2) ------------------------------------------------------------------- */

odin3_status lm_need(lm_ctx *lc, lm_type which, odin3_node_id at, odin3_celltype_id *out) {
    if (lc->types[which].v != 0) {
        *out = lc->types[which];
        return ODIN3_OK;
    }
    odin3_celltype_id id = {0};
    const odin3_celltype_def *def = lm_find_type(lc->design, k_type_names[which], &id)
                                        ? odin3_celltype_get(lc->design, id)
                                        : NULL;
    const char *why = NULL;
    if (def == NULL) {
        why = "is not registered";
    } else if (def->gran != ODIN3_GRAN_BLACKBOX) {
        why = "is not a black box";
    }
    if (why != NULL) {
        lm_diag(lc, ODIN3_LOG_ERROR, at,
                "lpmmap: cell type '%s' %s; read_techlib lib/lpm.o3lib first", k_type_names[which],
                why);
        return ODIN3_ERR_INVALID_ARG;
    }
    lc->types[which] = id;
    *out = id;
    return ODIN3_OK;
}

/* --- the transaction ----------------------------------------------------------------------- */

odin3_status lm_begin_cell(lm_ctx *lc, odin3_node_id src) {
    lc->src = src;
    lc->first = (odin3_node_id){0};
    lc->src_name = odin3_node_name(lc->module, src);
    lc->prov = (odin3_prov_id){0};
    odin3_vec_clear(&lc->made_nodes);
    odin3_vec_clear(&lc->made_nets);
    odin3_vec_clear(&lc->doomed);
    odin3_prov_begin_op(lc->pass);
    odin3_prov_id parent = odin3_node_prov(lc->module, src);
    if (parent.v == 0) {
        return ODIN3_OK;
    }
    return odin3_prov_derive(lc->pass, (odin3_prov_list){&parent, 1}, &lc->prov);
}

odin3_status lm_doom(lm_ctx *lc, odin3_node_id node) {
    odin3_node_id *slot = odin3_vec_push(&lc->doomed);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

odin3_status lm_fresh_nets(lm_ctx *lc, uint32_t n, odin3_net_id *out) {
    odin3_status st = odin3_vec_reserve(&lc->made_nets, lc->made_nets.len + n);
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = odin3_net_create(lc->module, 0, lc->prov, &out[i]);
        odin3_net_id *slot = st == ODIN3_OK ? odin3_vec_push(&lc->made_nets) : NULL;
        if (slot != NULL) {
            *slot = out[i];
        }
    }
    return st;
}

/* Sets the string attribute `lpmmap` = "<type>:<role>" on node. */
static odin3_status set_lpmmap_attr(lm_ctx *lc, odin3_node_id node, const char *type,
                                    const char *role) {
    uint32_t key = 0;
    uint32_t val = 0;
    odin3_strbuf_clear(&lc->text);
    odin3_status st = odin3_strbuf_appendf(&lc->text, "%s:%s", type, role);
    st = st == ODIN3_OK ? odin3_design_intern(lc->design, odin3_bytes_cstr(lc->text.data), &val)
                        : st;
    st = st == ODIN3_OK ? odin3_design_intern(lc->design, odin3_bytes_cstr("lpmmap"), &key) : st;
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, val, 0};
    return st == ODIN3_OK
               ? odin3_attr_set(lc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &value)
               : st;
}

/* The name of a node other than the first: `<source name>.<role>`, then `_1`, `_2` ... while taken;
 * 0 for the first (it takes the source's name at commit). */
static odin3_status node_name(lm_ctx *lc, const char *role, bool first, uint32_t *out) {
    *out = 0;
    if (first) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(&lc->text);
    odin3_status st = lc->src_name != 0
                          ? odin3_strbuf_appendf(&lc->text, "%s.%s", str_of(lc, lc->src_name), role)
                          : odin3_strbuf_appendf(&lc->text, "$c%" PRIu32 ".%s", lc->src.v, role);
    size_t base = lc->text.len;
    for (uint32_t tries = 0; st == ODIN3_OK && tries < LM_MAX_NAME_TRIES; tries++) {
        st = tries > 0 ? odin3_strbuf_appendf(&lc->text, "_%" PRIu32, tries) : st;
        st = st == ODIN3_OK ? odin3_design_intern(lc->design, odin3_bytes_cstr(lc->text.data), out)
                            : st;
        bool taken = st == ODIN3_OK && odin3_node_valid(odin3_module_find_node(lc->module, *out));
        lc->text.len = base;
        lc->text.data[base] = '\0';
        if (!taken) {
            break;
        }
    }
    return st;
}

static int index_of_param(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int index_of_port(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The parameter values: the library defaults, then the ones the request sets by name. */
static odin3_status param_values(lm_ctx *lc, const odin3_celltype_def *def, const lm_node_req *req,
                                 odin3_value *vals) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        vals[i] = def->params[i].dflt;
    }
    for (size_t i = 0; i < req->n_params; i++) {
        const lm_pset *set = &req->params[i];
        int idx = index_of_param(def, set->name);
        if (idx < 0) {
            odin3_log(ODIN3_LOG_ERROR, "lpmmap: cell type '%s' has no parameter '%s'", def->name,
                      set->name);
            return ODIN3_ERR_INVALID_ARG;
        }
        if (set->sval == NULL) {
            vals[idx] = odin3_value_int(set->ival);
            continue;
        }
        uint32_t id = 0;
        odin3_status st = odin3_design_intern(lc->design, odin3_bytes_cstr(set->sval), &id);
        if (st != ODIN3_OK) {
            return st;
        }
        vals[idx] = (odin3_value){ODIN3_VAL_STRING, 0, NULL, 0, id, 0};
    }
    return ODIN3_OK;
}

/* What one lm_new_node makes: the type, its definition, the parameter values and the request. */
typedef struct lm_make {
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    const odin3_value *vals;
    const lm_node_req *req;
} lm_make;

/* The netvec of every port: its width for the values, the nets the request lists, then open. */
static odin3_status port_nets(lm_ctx *lc, const lm_make *mk, odin3_netvec *out, uint64_t *pins) {
    const odin3_celltype_def *def = mk->def;
    const lm_node_req *req = mk->req;
    uint32_t widths[LM_MAX_PORTS];
    uint64_t total = 0;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const odin3_port_query query = {mk->type, mk->vals, i};
        odin3_status st = odin3_celltype_port_width_checked(lc->design, &query, &widths[i]);
        if (st != ODIN3_OK) {
            return st;
        }
        total += widths[i];
    }
    odin3_vec_clear(&lc->scratch);
    odin3_status st = odin3_vec_reserve(&lc->scratch, (size_t)total + 1U);
    for (uint64_t i = 0; st == ODIN3_OK && i < total + 1U; i++) {
        odin3_net_id *slot = odin3_vec_push(&lc->scratch); /* reserved: never NULL */
        *slot = (odin3_net_id){0};
    }
    uint64_t at = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        out[i] = (odin3_netvec){odin3_vec_cat(&lc->scratch, (size_t)at), widths[i]};
        at += widths[i];
    }
    for (size_t i = 0; st == ODIN3_OK && i < req->n_ports; i++) {
        const lm_pconn *conn = &req->ports[i];
        int idx = index_of_port(def, conn->name);
        if (idx < 0 || conn->count > widths[idx]) {
            odin3_log(ODIN3_LOG_ERROR, "lpmmap: cell type '%s' has no port '%s' of %u bits",
                      def->name, conn->name, (unsigned)conn->count);
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_net_id *dst = (odin3_net_id *)out[idx].nets;
        memcpy(dst, conn->nets, sizeof *dst * conn->count);
    }
    *pins = total;
    return st;
}

odin3_status lm_new_node(lm_ctx *lc, const lm_node_req *req, odin3_node_id *out) {
    odin3_celltype_id type = {0};
    odin3_status st = lm_need(lc, req->type, (odin3_node_id){0}, &type);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_celltype_def *def = odin3_celltype_get(lc->design, type);
    odin3_value vals[LM_MAX_PARAMS];
    odin3_netvec ports[LM_MAX_PORTS];
    if (def->n_params > LM_MAX_PARAMS || def->n_ports > LM_MAX_PORTS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t pins = 0;
    st = param_values(lc, def, req, vals);
    st = st == ODIN3_OK ? port_nets(lc, &(lm_make){type, def, vals, req}, ports, &pins) : st;
    bool first = lc->first.v == 0;
    uint32_t name = 0;
    st = st == ODIN3_OK ? budget_tick(lc, pins) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&lc->made_nodes, lc->made_nodes.len + 1) : st;
    st = st == ODIN3_OK ? node_name(lc, req->role, first, &name) : st;
    odin3_node_spec spec = {type, name, lc->prov, vals, def->n_params};
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(lc->module, &spec, ports, &node) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id *slot = odin3_vec_push(&lc->made_nodes);
    *slot = node; /* reserved above: never NULL */
    st = first ? odin3_kit_copy_attrs(lc->module, lc->src, node) : ODIN3_OK;
    st = st == ODIN3_OK ? set_lpmmap_attr(lc, node, def->name, req->role) : st;
    if (st != ODIN3_OK) {
        return st; /* lm_abort deletes the node: it is in made_nodes */
    }
    lc->first = first ? node : lc->first;
    *out = node;
    return ODIN3_OK;
}

void lm_abort(lm_ctx *lc) {
    for (size_t i = lc->made_nodes.len; i > 0; i--) {
        const odin3_node_id *node = odin3_vec_cat(&lc->made_nodes, i - 1);
        if (odin3_node_live(lc->module, *node)) {
            (void)odin3_node_delete(lc->module, *node);
        }
    }
    for (size_t i = lc->made_nets.len; i > 0; i--) {
        const odin3_net_id *net = odin3_vec_cat(&lc->made_nets, i - 1);
        if (odin3_net_live(lc->module, *net)) {
            (void)odin3_net_delete(lc->module, *net);
        }
    }
    odin3_vec_clear(&lc->made_nodes);
    odin3_vec_clear(&lc->made_nets);
    lc->first = (odin3_node_id){0};
}

/* The live net that `net` stands for after the merges made so far (none: gone). */
static odin3_net_id resolve(const lm_ctx *lc, odin3_net_id net) {
    while (odin3_net_valid(net) && !odin3_net_live(lc->module, net)) {
        odin3_net_id next = {0};
        for (size_t i = 0; i < lc->merged.len && !odin3_net_valid(next); i++) {
            const odin3_net_pair *pair = odin3_vec_cat(&lc->merged, i);
            next = pair->drop.v == net.v ? pair->keep : next;
        }
        net = next;
    }
    return net;
}

static odin3_status make_ties(lm_ctx *lc, const lm_tie *ties, size_t n_ties) {
    for (size_t i = 0; i < n_ties; i++) {
        odin3_net_id keep = resolve(lc, ties[i].keep);
        odin3_net_id drop = resolve(lc, ties[i].drop);
        if (!odin3_net_valid(keep) || !odin3_net_valid(drop) || keep.v == drop.v) {
            continue;
        }
        odin3_status st = odin3_net_merge(lc->module, (odin3_net_pair){keep, drop});
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_net_pair *slot = odin3_vec_push(&lc->merged); /* reserved: never NULL */
        *slot = (odin3_net_pair){keep, drop};
    }
    return ODIN3_OK;
}

/* After the delete: the first node's name, then the ties. */
static odin3_status finish_commit(lm_ctx *lc, const lm_tie *ties, size_t n_ties) {
    odin3_status st = ODIN3_OK;
    if (lc->first.v != 0 && lc->src_name != 0) {
        st = odin3_node_rename(lc->module, lc->first, lc->src_name);
    }
    return st == ODIN3_OK ? make_ties(lc, ties, n_ties) : st;
}

static void log_discard(const lm_ctx *lc) {
    odin3_log(ODIN3_LOG_ERROR,
              "lpmmap: out of memory in the middle of a mapping in module '%s': the design "
              "no longer matches its input; discard it",
              str_of(lc, odin3_module_name(lc->module)));
}

/* The nodes lm_doom listed (the flip-flops R-1 merges): dead once the source is gone. Their pins
 * leave their nets (never allocates); the delete records the pin nets and can run out of memory,
 * which is harmless (a dead flip-flop with no connections stays for `opt`). */
static void release_doomed(lm_ctx *lc) {
    for (size_t i = 0; i < lc->doomed.len; i++) {
        odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&lc->doomed, i);
        if (!odin3_node_live(lc->module, node)) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(lc->module, node);
        for (uint32_t pin = 0; pin < pins.count; pin++) {
            (void)odin3_pin_disconnect(lc->module, (odin3_pin_id){pins.first.v + pin});
        }
        (void)odin3_node_delete(lc->module, node);
    }
}

odin3_status lm_commit(lm_ctx *lc, const lm_tie *ties, size_t n_ties) {
    odin3_vec_clear(&lc->merged);
    odin3_status st = odin3_vec_reserve(&lc->merged, n_ties);
    /* The source goes first and alone: its delete is the only step that can fail and leave the
     * design as it was (H-8); everything after it cannot fail for lack of memory. */
    st = st == ODIN3_OK ? odin3_node_delete(lc->module, lc->src) : st;
    if (st != ODIN3_OK) {
        lm_abort(lc);
        return st;
    }
    release_doomed(lc);
    st = finish_commit(lc, ties, n_ties);
    if (st != ODIN3_OK) {
        log_discard(lc);
    }
    odin3_vec_clear(&lc->made_nodes);
    odin3_vec_clear(&lc->made_nets);
    return st;
}

odin3_status lm_transact(lm_ctx *lc, odin3_node_id src, lm_build_fn build) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(lm_tie));
    odin3_status st = build(lc, src, &ties);
    if (st != ODIN3_OK) {
        lm_abort(lc);
    } else {
        st = lm_commit(lc, odin3_vec_cat(&ties, 0), ties.len);
    }
    odin3_vec_free(&ties);
    return st;
}

/* --- constants (H-3) ----------------------------------------------------------------------- */

static bool const_type_known(lm_ctx *lc, lm_bit which) {
    return lc->konst[which].v != 0 ||
           lm_find_type(lc->design, k_const_names[which], &lc->konst[which]);
}

/* The Y net of live node when it is a constant node of the given value with a net, else none. */
static odin3_net_id const_net_of(const lm_ctx *lc, odin3_node_id node, lm_bit which) {
    if (!odin3_node_live(lc->module, node) ||
        odin3_node_type(lc->module, node).v != lc->konst[which].v) {
        return (odin3_net_id){0};
    }
    odin3_pinslice pins = odin3_node_pins(lc->module, node);
    return pins.count == 1 ? odin3_pin_net(lc->module, pins.first) : (odin3_net_id){0};
}

static odin3_net_id scan_const(const lm_ctx *lc, lm_bit which) {
    for (uint32_t i = 1; i < odin3_module_node_end(lc->module); i++) {
        odin3_net_id net = const_net_of(lc, (odin3_node_id){i}, which);
        if (odin3_net_valid(net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

/* A new constant node and net. They join the transaction's made lists, so lm_abort undoes them; a
 * commit keeps them. */
static odin3_status make_const(lm_ctx *lc, lm_bit which, odin3_net_id *out) {
    odin3_prov_id prov = odin3_module_prov(lc->module);
    odin3_net_id net = {0};
    odin3_status st = odin3_vec_reserve(&lc->made_nets, lc->made_nets.len + 1);
    st = st == ODIN3_OK ? odin3_vec_reserve(&lc->made_nodes, lc->made_nodes.len + 1) : st;
    st = st == ODIN3_OK ? odin3_net_create(lc->module, 0, prov, &net) : st;
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {lc->konst[which], 0, prov, NULL, 0};
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(lc->module, &spec, &port, &node) : st;
    if (st != ODIN3_OK) {
        if (odin3_net_valid(net)) {
            (void)odin3_net_delete(lc->module, net);
        }
        return st;
    }
    odin3_net_id *net_slot = odin3_vec_push(&lc->made_nets);    /* reserved: never NULL */
    odin3_node_id *node_slot = odin3_vec_push(&lc->made_nodes); /* likewise */
    *net_slot = net;
    *node_slot = node;
    *out = net;
    return ODIN3_OK;
}

odin3_status lm_const(lm_ctx *lc, lm_bit which, odin3_net_id *out) {
    if (odin3_net_valid(lc->consts[which]) && odin3_net_live(lc->module, lc->consts[which]) &&
        odin3_net_const_value(lc->module, lc->consts[which]) == k_const_values[which]) {
        *out = lc->consts[which];
        return ODIN3_OK;
    }
    if (!const_type_known(lc, which)) {
        odin3_log(ODIN3_LOG_ERROR, "lpmmap: built-in cell type '%s' is missing",
                  k_const_names[which]);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id net = scan_const(lc, which);
    odin3_status st = odin3_net_valid(net) ? ODIN3_OK : make_const(lc, which, &net);
    if (st == ODIN3_OK) {
        lc->consts[which] = net;
        *out = net;
    }
    return st;
}

/* --- operand extension (H-2) --------------------------------------------------------------- */

odin3_status lm_extend(lm_ctx *lc, odin3_pinslice port, uint32_t width, bool is_signed,
                       odin3_net_id *out) {
    odin3_net_id gnd = {0};
    odin3_status st = port.count < width && (!is_signed || port.count == 0)
                          ? lm_const(lc, LM_GND, &gnd)
                          : ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        if (i < port.count) {
            out[i] = odin3_pin_net(lc->module, (odin3_pin_id){port.first.v + i});
        } else if (is_signed && port.count > 0) {
            out[i] = out[port.count - 1U];
        } else {
            out[i] = gnd;
        }
    }
    return st;
}

/* --- arrays -------------------------------------------------------------------------------- */

odin3_status lm_net_array(odin3_vec *v, uint32_t n, odin3_net_id **first) {
    size_t at = v->len;
    odin3_status st = odin3_vec_reserve(v, v->len + n + 1U);
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        odin3_net_id *slot = odin3_vec_push(v); /* reserved: never NULL */
        *slot = (odin3_net_id){0};
    }
    if (st == ODIN3_OK) {
        *first = odin3_vec_at(v, at);
    }
    return st;
}
