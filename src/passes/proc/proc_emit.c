/*
 * proc_emit.c — the module context of the proc pass and its shared constant nets (2E spec §3,
 * §17; IR-4); the net map, connections and merges (§7); the cell helper (§14); provenance (§14,
 * plan point (i)); orphan nets (§12) and the byte budget at a process boundary (§16).
 */
#include "passes/proc/proc_internal.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The constant cell types and the constants they drive, indexed by bit & ~ODIN3_PW_CONST. */
static const char *const k_const_types[ODIN3_PROC_N_CONST_NETS] = {"$_CONST0_", "$_CONST1_",
                                                                   "$_CONSTX_", "$_CONSTZ_"};
static const odin3_const k_const_values[ODIN3_PROC_N_CONST_NETS] = {ODIN3_CONST_0, ODIN3_CONST_1,
                                                                    ODIN3_CONST_X, ODIN3_CONST_Z};

/* The types proc creates (odin3_proc_type's cache) and the kind each counts as (§17, point (j)). */
static const char *const k_types[ODIN3_PROC_N_TYPES] = {"$dff",    "$adff", "$aldff",    "$dffsr",
                                                        "$dlatch", "$mux",  "$pmux",     "$eq",
                                                        "$not",    "$and",  "$reduce_or"};
static const odin3_proc_kind k_type_kinds[ODIN3_PROC_N_TYPES] = {
    ODIN3_PROC_K_DFF,    ODIN3_PROC_K_ADFF,   ODIN3_PROC_K_ALDFF, ODIN3_PROC_K_DFFSR,
    ODIN3_PROC_K_DLATCH, ODIN3_PROC_K_MUX,    ODIN3_PROC_K_PMUX,  ODIN3_PROC_K_EQ,
    ODIN3_PROC_K_HELPER, ODIN3_PROC_K_HELPER, ODIN3_PROC_K_HELPER};

/* The registered type named name, none when the design has none (no allocation, no log). */
static odin3_celltype_id find_type(const odin3_design *design, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, &id)) {
        return (odin3_celltype_id){0};
    }
    return id;
}

odin3_status odin3_proc_mctx_init(odin3_proc_mctx *mctx, odin3_pass_ctx *ctx,
                                  odin3_module *module) {
    if (mctx == NULL || ctx == NULL || module == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: mctx_init: no context, pass run or module");
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(mctx, 0, sizeof *mctx);
    mctx->ctx = ctx;
    mctx->design = odin3_module_design(module);
    mctx->module = module;
    odin3_vec_init(&mctx->pws, sizeof(odin3_pw));
    odin3_vec_init(&mctx->scratch, sizeof(uint32_t));
    odin3_vec_init(&mctx->touched, sizeof(uint32_t));
    odin3_vec_init(&mctx->netmap, sizeof(odin3_pw_bit));
    odin3_vec_init(&mctx->pending, sizeof(odin3_proc_conn));
    for (uint32_t i = 0; i < ODIN3_PROC_N_TYPES; i++) {
        mctx->types[i] = find_type(mctx->design, k_types[i]);
    }
    return ODIN3_OK;
}

void odin3_proc_mctx_free(odin3_proc_mctx *mctx) {
    if (mctx == NULL) {
        return;
    }
    for (size_t i = 0; i < mctx->pws.len; i++) {
        odin3_pw_free(odin3_vec_at(&mctx->pws, i));
    }
    odin3_vec_free(&mctx->pws);
    odin3_vec_free(&mctx->scratch);
    odin3_vec_free(&mctx->touched);
    odin3_vec_free(&mctx->netmap);
    odin3_vec_free(&mctx->pending);
}

/* The cell type named name (a built-in, so always registered). */
static odin3_status type_named(const odin3_design *design, const char *name,
                               odin3_celltype_id *out) {
    uint32_t str = 0;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) &&
        odin3_celltype_find(design, str, out)) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR, "proc: cell type '%s' is not registered", name);
    return ODIN3_ERR_INVALID_ARG;
}

/* The net on the Y pin of the first live node of type in ID order whose pin is connected. */
static odin3_net_id first_const_net(const odin3_module *module, odin3_celltype_id type) {
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node) || odin3_node_type(module, node).v != type.v) {
            continue;
        }
        odin3_net_id net = odin3_pin_net(module, odin3_node_port(module, node, 0).first);
        if (odin3_net_valid(net)) {
            return net;
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

odin3_status odin3_proc_const_net(odin3_proc_mctx *mctx, odin3_pw_bit code, odin3_net_id *out) {
    if (code < ODIN3_PW_S0 || code > ODIN3_PW_SZ) {
        odin3_log(ODIN3_LOG_ERROR, "proc: const_net: 0x%08x is not 0, 1, x or z", (unsigned)code);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t idx = code & ~ODIN3_PW_CONST;
    odin3_net_id cached = mctx->const_net[idx];
    if (odin3_net_valid(cached) && odin3_net_live(mctx->module, cached) &&
        odin3_net_const_value(mctx->module, cached) == k_const_values[idx]) {
        *out = cached;
        return ODIN3_OK;
    }
    odin3_celltype_id type = {0};
    odin3_status st = type_named(mctx->design, k_const_types[idx], &type);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id net = first_const_net(mctx->module, type);
    if (!odin3_net_valid(net)) {
        st = new_const_net(mctx->module, type, &net);
    }
    if (st == ODIN3_OK) {
        mctx->const_net[idx] = net;
        *out = net;
    }
    return st;
}

odin3_celltype_id odin3_proc_type(const odin3_proc_mctx *mctx, const char *name) {
    for (uint32_t i = 0; i < ODIN3_PROC_N_TYPES; i++) {
        if (strcmp(k_types[i], name) == 0 && odin3_celltype_valid(mctx->types[i])) {
            return mctx->types[i];
        }
    }
    odin3_celltype_id id = find_type(mctx->design, name);
    if (!odin3_celltype_valid(id)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: cell type '%s' is not registered", name);
    }
    return id;
}

/* --- the net map (§7) ---------------------------------------------------------------------- */

static bool is_const(odin3_pw_bit bit) {
    return (bit & ODIN3_PW_CONST) != 0;
}

odin3_pw_bit odin3_proc_find(odin3_proc_mctx *mctx, odin3_pw_bit bit) {
    odin3_pw_bit *map = mctx->netmap.data;
    size_t len = mctx->netmap.len;
    odin3_pw_bit root = bit;
    while (!is_const(root) && root < len && map[root] != 0) {
        root = map[root];
    }
    for (odin3_pw_bit cur = bit; cur != root;) { /* every non-root on the path is mapped */
        odin3_pw_bit next = map[cur];
        map[cur] = root;
        cur = next;
    }
    return root;
}

odin3_status odin3_proc_connect(odin3_proc_mctx *mctx, odin3_pw_bit lhs, odin3_pw_bit rhs) {
    odin3_proc_conn *conn = odin3_vec_push(&mctx->pending);
    if (conn == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    conn->lhs = lhs;
    conn->rhs = rhs;
    return ODIN3_OK;
}

/* Grows the net map to the module's net end (new entries 0: their own roots). */
static odin3_status netmap_grow(odin3_proc_mctx *mctx) {
    size_t end = odin3_module_net_end(mctx->module);
    if (odin3_vec_reserve(&mctx->netmap, end) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (mctx->netmap.len < end) {
        (void)odin3_vec_push(&mctx->netmap); /* reserved; zeroed */
    }
    return ODIN3_OK;
}

/* 2 for a net whose primary wire is a port wire, 1 for another primary wire, else 0. */
static int keep_rank(const odin3_module *module, odin3_net_id net) {
    odin3_wirebit primary = odin3_net_primary(module, net);
    if (!odin3_wire_valid(primary.wire)) {
        return 0;
    }
    return odin3_node_valid(odin3_wire_port_node(module, primary.wire)) ? 2 : 1;
}

/* Logs "merging two driven nets" for the classes of lhs and rhs; ODIN3_ERR_CHECK (or NO_MEMORY). */
static odin3_status two_driven(const odin3_proc_mctx *mctx, odin3_pw_bit lhs, odin3_pw_bit rhs) {
    odin3_strbuf lhs_name;
    odin3_strbuf rhs_name;
    odin3_strbuf_init(&lhs_name);
    odin3_strbuf_init(&rhs_name);
    odin3_status st = odin3_proc_name_bits(mctx, &lhs, 1, &lhs_name);
    st = st == ODIN3_OK ? odin3_proc_name_bits(mctx, &rhs, 1, &rhs_name) : st;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: merging two driven nets '%s' and '%s'",
                  lhs_name.data != NULL ? lhs_name.data : "",
                  rhs_name.data != NULL ? rhs_name.data : "");
        st = ODIN3_ERR_CHECK;
    }
    odin3_strbuf_free(&lhs_name);
    odin3_strbuf_free(&rhs_name);
    return st;
}

/* The two roots of one connection: the lhs's and the rhs's. */
typedef struct root_pair {
    odin3_pw_bit lhs, rhs;
} root_pair;

/* keep and drop for a merge with a constant class: keep is the shared constant net. */
static odin3_status const_pair(odin3_proc_mctx *mctx, root_pair roots, odin3_net_pair *pair,
                               odin3_pw_bit *entry) {
    bool lhs_const = is_const(roots.lhs);
    *entry = lhs_const ? roots.lhs : roots.rhs;
    pair->drop = (odin3_net_id){lhs_const ? roots.rhs : roots.lhs};
    return odin3_proc_const_net(mctx, *entry, &pair->keep);
}

/* keep and drop for two nets (§7, point (u)): a port wire's net, else a wire's, else the lower
 * ID; a tie at either level keeps the lower ID. */
static odin3_net_pair net_pair(const odin3_module *module, root_pair roots) {
    int rank_lhs = keep_rank(module, (odin3_net_id){roots.lhs});
    int rank_rhs = keep_rank(module, (odin3_net_id){roots.rhs});
    bool keep_lhs = rank_lhs != rank_rhs ? rank_lhs > rank_rhs : roots.lhs < roots.rhs;
    odin3_net_pair pair = {{keep_lhs ? roots.lhs : roots.rhs}, {keep_lhs ? roots.rhs : roots.lhs}};
    return pair;
}

/* One merge of resolved, distinct roots; drop's entry becomes keep (or the constant). */
static odin3_status merge_roots(odin3_proc_mctx *mctx, root_pair roots) {
    if (is_const(roots.lhs) && is_const(roots.rhs)) {
        return two_driven(mctx, roots.lhs, roots.rhs); /* two different constants */
    }
    odin3_net_pair pair = {{0}, {0}};
    odin3_pw_bit entry = 0;
    if (is_const(roots.lhs) || is_const(roots.rhs)) {
        odin3_status st = const_pair(mctx, roots, &pair, &entry);
        if (st != ODIN3_OK) {
            return st;
        }
    } else {
        pair = net_pair(mctx->module, roots);
        entry = pair.keep.v;
    }
    assert(odin3_net_live(mctx->module, pair.drop) && odin3_net_live(mctx->module, pair.keep));
#ifndef NDEBUG
    if (odin3_net_driver_count(mctx->module, pair.keep) > 0 &&
        odin3_net_driver_count(mctx->module, pair.drop) > 0) {
        return two_driven(mctx, roots.lhs, roots.rhs);
    }
#endif
    odin3_status st = odin3_net_merge(mctx->module, pair);
    if (st == ODIN3_OK) {
        *(odin3_pw_bit *)odin3_vec_at(&mctx->netmap, pair.drop.v) = entry;
    }
    return st;
}

odin3_status odin3_proc_apply_merges(odin3_proc_mctx *mctx) {
    odin3_status st = netmap_grow(mctx);
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pending.len; i++) {
        const odin3_proc_conn *conn = odin3_vec_cat(&mctx->pending, i);
        root_pair roots = {odin3_proc_find(mctx, conn->lhs), odin3_proc_find(mctx, conn->rhs)};
        if (roots.lhs != roots.rhs) {
            st = merge_roots(mctx, roots);
        }
    }
    odin3_vec_clear(&mctx->pending);
    return st;
}

/* --- cells (§14) --------------------------------------------------------------------------- */

/* One cell being built: its definition, port widths and the nets of every pin. */
typedef struct cell_build {
    odin3_proc_mctx *mctx;
    const odin3_proc_cell *cell;
    const odin3_celltype_def *def;
    odin3_net_id *nets;  /* one per pin, port after port */
    odin3_netvec *ports; /* one per port, into nets */
    uint32_t n_pins;
    uint32_t n_fresh; /* fresh output nets made so far (deleted again on failure) */
    uint32_t n_out;   /* output pins */
} cell_build;

/* The resolved net of bit (a constant: the shared constant net). */
static odin3_status resolve_net(odin3_proc_mctx *mctx, odin3_pw_bit bit, odin3_net_id *out) {
    odin3_pw_bit root = odin3_proc_find(mctx, bit);
    if (!is_const(root)) {
        *out = (odin3_net_id){root};
        return ODIN3_OK;
    }
    return odin3_proc_const_net(mctx, root, out);
}

/* Sizes every port for the cell's parameters; INVALID_ARG (logged) for a mismatched input. */
static odin3_status cb_size(cell_build *cb) {
    const odin3_proc_cell *cell = cb->cell;
    uint32_t input = 0;
    uint64_t pins = 0;
    for (uint32_t port = 0; port < cb->def->n_ports; port++) {
        uint32_t width =
            odin3_celltype_port_width(cb->mctx->design, cell->type, cell->params, port);
        cb->ports[port].count = width;
        pins += width;
        if (cb->def->ports[port].dir == ODIN3_DIR_OUT) {
            cb->n_out += width;
            continue;
        }
        if (input >= cell->n_inputs || cell->inputs[input].len != width) {
            odin3_log(ODIN3_LOG_ERROR, "proc: add_cell: %s: input %u does not have %u bits",
                      cb->def->name, input, width);
            return ODIN3_ERR_INVALID_ARG;
        }
        input++;
    }
    if (input != cell->n_inputs || pins > UINT32_MAX ||
        (cell->output != NULL && cell->output->len != cb->n_out)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: add_cell: %s: wrong inputs or output", cb->def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    cb->n_pins = (uint32_t)pins;
    return ODIN3_OK;
}

/* The nets of one output port: the given output's (resolved) or fresh nets with the cell's prov. */
static odin3_status cb_outputs(cell_build *cb, odin3_net_id *nets, uint32_t width,
                               uint32_t *taken) {
    const odin3_proc_cell *cell = cb->cell;
    for (uint32_t k = 0; k < width; k++) {
        odin3_status st = ODIN3_OK;
        if (cell->output == NULL) {
            st = odin3_net_create(cb->mctx->module, 0, cell->prov, &nets[k]);
            cb->n_fresh += st == ODIN3_OK ? 1 : 0;
        } else {
            odin3_pw_bit bit = odin3_pw_sig_bits(cell->pw, *cell->output)[*taken + k];
            odin3_pw_bit root = odin3_proc_find(cb->mctx, bit);
            if (is_const(root)) {
                odin3_log(ODIN3_LOG_ERROR, "proc: add_cell: %s: a constant output", cb->def->name);
                st = ODIN3_ERR_INVALID_ARG;
            }
            nets[k] = (odin3_net_id){root};
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    *taken += width;
    return ODIN3_OK;
}

/* Fills every pin's net, input ports from the inputs, output ports by cb_outputs. */
static odin3_status cb_nets(cell_build *cb) {
    uint32_t input = 0;
    uint32_t taken = 0;
    uint32_t at = 0;
    for (uint32_t port = 0; port < cb->def->n_ports; port++) {
        uint32_t width = cb->ports[port].count;
        odin3_net_id *nets = cb->nets + at;
        cb->ports[port].nets = nets;
        odin3_status st = ODIN3_OK;
        if (cb->def->ports[port].dir == ODIN3_DIR_OUT) {
            st = cb_outputs(cb, nets, width, &taken);
        } else {
            const odin3_pw_bit *bits = odin3_pw_sig_bits(cb->cell->pw, cb->cell->inputs[input++]);
            for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
                st = resolve_net(cb->mctx, bits[k], &nets[k]);
            }
        }
        if (st != ODIN3_OK) {
            return st;
        }
        at += width;
    }
    return ODIN3_OK;
}

/* Deletes the fresh output nets of a cell that was not made (pinless, wireless: never fails). */
static void cb_undo(const cell_build *cb) {
    uint32_t left = cb->cell->output == NULL ? cb->n_fresh : 0;
    for (uint32_t port = 0, at = 0; left > 0 && port < cb->def->n_ports; port++) {
        uint32_t width = cb->ports[port].count;
        for (uint32_t k = 0; cb->def->ports[port].dir == ODIN3_DIR_OUT && left > 0 && k < width;
             k++, left--) {
            (void)odin3_net_delete(cb->mctx->module, cb->nets[at + k]);
        }
        at += width;
    }
}

/* The cell's attributes: the working form's (registers, latches) and the switch flags (§14). */
static odin3_status cb_attrs(const cell_build *cb, odin3_node_id node) {
    odin3_objref ref = {ODIN3_OBJ_NODE, node.v};
    const odin3_pw *from = cb->cell->attrs_from;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; from != NULL && st == ODIN3_OK && i < from->attrs.len; i++) {
        const odin3_pw_attr *attr = odin3_vec_cat(&from->attrs, i);
        st = odin3_attr_set(cb->mctx->module, ref, attr->key, attr->value);
    }
    static const char *const k_flag_names[] = {"full_case", "parallel_case"};
    static const uint16_t k_flags[] = {ODIN3_SWITCH_FULL_CASE, ODIN3_SWITCH_PARALLEL_CASE};
    odin3_value one = odin3_value_int(1);
    for (size_t i = 0; st == ODIN3_OK && i < sizeof k_flags / sizeof k_flags[0]; i++) {
        uint32_t key = 0;
        if ((cb->cell->switch_flags & k_flags[i]) == 0) {
            continue;
        }
        st = odin3_design_intern(cb->mctx->design, odin3_bytes_cstr(k_flag_names[i]), &key);
        st = st == ODIN3_OK ? odin3_attr_set(cb->mctx->module, ref, key, &one) : st;
    }
    return st;
}

/* Counts the cell's kind (§17) and its pins for the budget, checking it every budget_pins pins. */
static odin3_status cb_account(cell_build *cb) {
    odin3_proc_mctx *mctx = cb->mctx;
    mctx->pins_since_budget += cb->n_pins;
    if (mctx->pins_since_budget < odin3_proc_limits_now().budget_pins) {
        return ODIN3_OK;
    }
    mctx->pins_since_budget = 0;
    const odin3_pw *pw = cb->cell->pw;
    return odin3_proc_budget(mctx, pw->prov.v != 0 ? pw->prov : cb->cell->prov);
}

static void cb_count_kind(const cell_build *cb) {
    if (cb->mctx->stats == NULL) {
        return;
    }
    for (uint32_t i = 0; i < ODIN3_PROC_N_TYPES; i++) {
        if (cb->mctx->types[i].v == cb->cell->type.v) {
            cb->mctx->stats->cells[k_type_kinds[i]]++;
            return;
        }
    }
}

/* *out = the output nets, output port after output port, as a new run of pw->bits. */
static odin3_status cb_out_sig(const cell_build *cb, odin3_pw_sig *out) {
    odin3_pw_bit *bits = odin3_util_malloc(sizeof *bits * ((size_t)cb->n_out + 1));
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t taken = 0;
    for (uint32_t port = 0; port < cb->def->n_ports; port++) {
        for (uint32_t k = 0; cb->def->ports[port].dir == ODIN3_DIR_OUT && k < cb->ports[port].count;
             k++) {
            bits[taken++] = cb->ports[port].nets[k].v;
        }
    }
    odin3_status st = odin3_pw_sig_new(cb->cell->pw, bits, cb->n_out, out);
    odin3_util_free(bits);
    return st;
}

/*
 * The output run first (pw->bits only grows, so a later failure leaves garbage, never a half-made
 * cell), then the node, its kind and its attributes; only setting an attribute can fail after the
 * node exists, and the IR is valid then.
 */
static odin3_status cb_create(cell_build *cb, odin3_pw_sig *out) {
    const odin3_proc_cell *cell = cb->cell;
    odin3_status st = cb_out_sig(cb, out);
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        odin3_node_spec spec = {cell->type, 0, cell->prov, cell->params, cell->n_params};
        st = odin3_node_create_connected(cb->mctx->module, &spec, cb->ports, &node);
    }
    if (st != ODIN3_OK) {
        cb_undo(cb);
        return st;
    }
    cb_count_kind(cb);
    return cb_attrs(cb, node);
}

odin3_status odin3_proc_add_cell(odin3_proc_mctx *mctx, const odin3_proc_cell *cell,
                                 odin3_pw_sig *out) {
    cell_build cb = {mctx, cell, odin3_celltype_get(mctx->design, cell->type), NULL, NULL, 0, 0, 0};
    if (cb.def == NULL || cell->pw == NULL || (cell->n_inputs > 0 && cell->inputs == NULL) ||
        cell->n_params != cb.def->n_params || (cell->n_params > 0 && cell->params == NULL)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "proc: add_cell: no cell type, working form, inputs or parameters");
        return ODIN3_ERR_INVALID_ARG;
    }
    cb.ports = odin3_util_calloc(sizeof *cb.ports * (cb.def->n_ports + 1));
    odin3_status st = cb.ports != NULL ? cb_size(&cb) : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? cb_account(&cb) : st;
    if (st == ODIN3_OK) {
        cb.nets = odin3_util_calloc(sizeof *cb.nets * ((size_t)cb.n_pins + 1));
        st = cb.nets != NULL ? cb_nets(&cb) : ODIN3_ERR_NO_MEMORY;
        if (st != ODIN3_OK && cb.nets != NULL) {
            cb_undo(&cb);
        }
    }
    st = st == ODIN3_OK ? cb_create(&cb, out) : st;
    odin3_util_free(cb.nets);
    odin3_util_free(cb.ports);
    return st;
}

/* --- provenance (§14) ---------------------------------------------------------------------- */

enum { PROV_STACK = 8 };

odin3_status odin3_proc_prov(const odin3_proc_mctx *mctx, const odin3_pw *pw,
                             const odin3_prov_id *parents, uint32_t n, odin3_prov_id *out) {
    odin3_prov_id stack[PROV_STACK];
    uint32_t count = n > 0 ? n : 1;
    odin3_prov_id *ids = count <= PROV_STACK ? stack : odin3_util_malloc(sizeof *ids * count);
    if (ids == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        odin3_prov_id parent = n > 0 ? parents[i] : (odin3_prov_id){0};
        parent = parent.v != 0 ? parent : pw->prov;
        if (parent.v != 0) { /* a record-less process: its 0 is left out (review I2) */
            ids[kept++] = parent;
        }
    }
    *out = (odin3_prov_id){0};
    odin3_status st =
        kept > 0 ? odin3_prov_derive(mctx->ctx, (odin3_prov_list){ids, kept}, out) : ODIN3_OK;
    if (ids != stack) {
        odin3_util_free(ids);
    }
    return st;
}

odin3_status odin3_proc_begin_lowering(odin3_proc_mctx *mctx, odin3_pw *pw) {
    odin3_prov_begin_op(mctx->ctx);
    pw->lowered = (odin3_prov_id){0};
    if (pw->prov.v == 0) {
        return ODIN3_OK; /* a node without a record: nothing to derive from */
    }
    return odin3_prov_derive(mctx->ctx, (odin3_prov_list){&pw->prov, 1}, &pw->lowered);
}

/* --- step E and the budget (§12, §16) ------------------------------------------------------ */

/* True for a net step E deletes: live, pinless, unnamed, no primary wire, no aliases. */
static bool is_orphan(const odin3_module *module, odin3_net_id net) {
    return odin3_net_valid(net) && odin3_net_live(module, net) &&
           odin3_net_pins(module, net).count == 0 && odin3_net_name(module, net) == 0 &&
           !odin3_wire_valid(odin3_net_primary(module, net).wire) &&
           odin3_net_alias_count(module, net) == 0;
}

odin3_status odin3_proc_orphans(odin3_proc_mctx *mctx) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        const odin3_pw *pw = odin3_vec_cat(&mctx->pws, i);
        for (size_t j = 0; st == ODIN3_OK && j < pw->in_nets.len; j++) {
            odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&pw->in_nets, j);
            if (is_orphan(mctx->module, net)) {
                st = odin3_net_delete(mctx->module, net);
            }
        }
    }
    return st;
}

odin3_status odin3_proc_budget(odin3_proc_mctx *mctx, odin3_prov_id at) {
    uint64_t extra = mctx->pw_bytes + (uint64_t)mctx->scratch.cap * mctx->scratch.elem_size +
                     (uint64_t)mctx->netmap.cap * mctx->netmap.elem_size +
                     (uint64_t)mctx->pending.cap * mctx->pending.elem_size;
    odin3_status st = odin3_budget_check(mctx->ctx, mctx->design, extra);
    if (st != ODIN3_ERR_PARSE) {
        return st;
    }
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    odin3_status built = odin3_budget_message(mctx->ctx, &msg);
    if (built == ODIN3_OK) {
        odin3_proc_error(mctx, at, "%s", msg.data);
    }
    odin3_strbuf_free(&msg);
    mctx->stopped = true;
    return built == ODIN3_OK ? ODIN3_ERR_PARSE : built;
}
