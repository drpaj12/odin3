/*
 * dfflibmap.c — the `dfflibmap <library>` pass (gatemap spec 6, G-6; plan Task 9): maps every
 * $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_ onto the state cell odin3_dlm_choose picks from
 * the library. Every module is planned (module-ID order, node-ID order) before any change, so a
 * cell with no match, or a flip-flop or latch of another type, fails the pass with the IR
 * untouched. Then each cell is replaced: the new cell takes the old one's nets, provenance, name
 * and attributes (provenance-to-HDL and graph goals; GM-14); a $_DFF_N_ on a posedge cell gets the
 * library inverter on its clock, one per clock net per module, with the union of the provenance
 * of every flip-flop that uses it.
 */
#include "passes/dfflibmap/dfflibmap.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "techlib/libcells.h"
#include "techlib/reader.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Ports of the four source types: C or E, D, Q. */
enum { SRC_CLOCK, SRC_DATA, SRC_Q };

/* Their INIT parameter. */
enum { SRC_INIT = 0 };

/* A library cell dfflibmap creates has at most this many ports (a state cell three, INV two). */
enum { DLM_MAX_PORTS = 3 };

/* Size of a `$c<ID>` label. */
enum { LABEL_MAX = 24 };

/* The bits of a 64-bit map key above the net ID: the module ID. */
enum { KEY_SHIFT = 32 };

static const char *const k_kind_names[ODIN3_DLM_KINDS] = {"$_DFF_P_", "$_DFF_N_", "$_DLATCH_P_",
                                                          "$_DLATCH_N_"};
static const char *const k_kind_trigger[ODIN3_DLM_KINDS] = {"posedge", "negedge", "high", "low"};

/* The word flip-flops and latches (no SEQ flag; 2E IR change D): refused by name. */
static const char *const k_word_state[] = {"$dff",    "$dffe",   "$adff",    "$sdff",     "$adffe",
                                           "$sdffe",  "$sdffce", "$aldff",   "$aldffe",   "$dffsr",
                                           "$dffsre", "$dlatch", "$adlatch", "$dlatchsr", "$sr"};

/* One planned replacement. */
typedef struct dlm_item {
    uint32_t module, node;
    odin3_dlm_choice choice;
} dlm_item;

/* A flip-flop that needs the inverted clock net of its module: the (module, clock) key and its
 * provenance. */
typedef struct dlm_user {
    uint64_t key;
    odin3_prov_id prov;
} dlm_user;

typedef struct attr_copy {
    odin3_module *mod;
    odin3_node_id src, dst;
} attr_copy;

typedef struct dlm_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    const char *library;
    odin3_libcells cells;
    odin3_celltype_id kinds[ODIN3_DLM_KINDS]; /* {0} when the design lacks the type */
    odin3_vec items;                          /* dlm_item, module then node order */
    odin3_vec users;                          /* dlm_user, in item order */
    odin3_u64map *clocks; /* apply: (module ID << 32 | clock net) -> its inverted net */
    uint64_t init_lost;
    uint64_t inverters;
} dlm_ctx;

/* --- planning ------------------------------------------------------------------------------ */

static odin3_net_id pin_net(const odin3_module *mod, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    return pins.count == 1 ? odin3_pin_net(mod, pins.first) : (odin3_net_id){0};
}

/* The key of clock net clock in mod. */
static uint64_t clock_key(const odin3_module *mod, odin3_net_id clock) {
    return ((uint64_t)odin3_module_id_of(mod).v << KEY_SHIFT) | clock.v;
}

static const char *str_of(const odin3_design *design, uint32_t str) {
    const char *text = odin3_strtab_get(odin3_design_strtab(design), str);
    return text != NULL ? text : "";
}

/* The node's name, or `$c<ID>` in buf when it has none. */
static const char *node_label(const odin3_module *mod, odin3_node_id node, char *buf, size_t n) {
    uint32_t name = odin3_node_name(mod, node);
    if (name != 0) {
        return str_of(odin3_module_design(mod), name);
    }
    (void)snprintf(buf, n, "$c%" PRIu32, node.v);
    return buf;
}

/* The kind of type, or ODIN3_DLM_KINDS when it is none of the four. */
static odin3_dlm_kind kind_of(const dlm_ctx *dc, odin3_celltype_id type) {
    for (int k = 0; k < ODIN3_DLM_KINDS; k++) {
        if (dc->kinds[k].v != 0 && dc->kinds[k].v == type.v) {
            return (odin3_dlm_kind)k;
        }
    }
    return ODIN3_DLM_KINDS;
}

/* True when type is a flip-flop or latch dfflibmap cannot map: a SEQ_EDGE or SEQ_LEVEL bit or
 * word cell without library data, or a word flip-flop or latch. */
static bool other_state(const odin3_design *design, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    if (def == NULL || (def->gran != ODIN3_GRAN_BIT && def->gran != ODIN3_GRAN_WORD) ||
        odin3_celltype_lib(design, type) != NULL) {
        return false;
    }
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof k_word_state / sizeof k_word_state[0]; i++) {
        if (strcmp(def->name, k_word_state[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int64_t node_init(const odin3_module *mod, odin3_node_id node) {
    const odin3_value *init = odin3_node_param(mod, node, SRC_INIT);
    return init != NULL && init->kind == ODIN3_VAL_INT ? init->i : INIT_UNKNOWN;
}

static odin3_status refuse_other(const odin3_module *mod, odin3_node_id node) {
    const odin3_design *design = odin3_module_design(mod);
    char buf[LABEL_MAX];
    odin3_log(ODIN3_LOG_ERROR,
              "dfflibmap: module '%s': cell '%s' of type %s is a flip-flop or latch dfflibmap "
              "cannot map (only $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_)",
              str_of(design, odin3_module_name(mod)), node_label(mod, node, buf, sizeof buf),
              odin3_celltype_get(design, odin3_node_type(mod, node))->name);
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status refuse_no_match(const dlm_ctx *dc, const odin3_module *mod, odin3_node_id node,
                                    odin3_dlm_kind kind) {
    char buf[LABEL_MAX];
    odin3_log(ODIN3_LOG_ERROR,
              "dfflibmap: module '%s': no cell of library '%s' implements %s '%s' (%s, INIT "
              "%" PRId64 ")",
              str_of(dc->design, odin3_module_name(mod)), dc->library, k_kind_names[kind],
              node_label(mod, node, buf, sizeof buf), k_kind_trigger[kind], node_init(mod, node));
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status plan_node(dlm_ctx *dc, const odin3_module *mod, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(mod, node);
    odin3_dlm_kind kind = kind_of(dc, type);
    if (kind == ODIN3_DLM_KINDS) {
        return other_state(dc->design, type) ? refuse_other(mod, node) : ODIN3_OK;
    }
    int64_t init = node_init(mod, node);
    odin3_dlm_choice choice = {0, false};
    if (!odin3_dlm_choose(dc->design, &dc->cells, kind, init, &choice)) {
        return refuse_no_match(dc, mod, node, kind);
    }
    dlm_item *item = odin3_vec_push(&dc->items);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *item = (dlm_item){odin3_module_id_of(mod).v, node.v, choice};
    odin3_net_id clock = pin_net(mod, node, SRC_CLOCK);
    if (choice.invert_clock && odin3_net_valid(clock)) {
        dlm_user *user = odin3_vec_push(&dc->users);
        if (user == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *user = (dlm_user){clock_key(mod, clock), odin3_node_prov(mod, node)};
    }
    const odin3_libstate *state = odin3_vec_cat(&dc->cells.states, choice.state);
    dc->init_lost += (init == 0 || init == 1) && state->init < 0 ? 1U : 0U;
    return ODIN3_OK;
}

static odin3_status plan(dlm_ctx *dc) {
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(dc->design); mi++) {
        const odin3_module *mod = odin3_module_get(dc->design, (odin3_module_id){mi});
        uint32_t end = mod != NULL ? odin3_module_node_end(mod) : 0;
        for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
            odin3_node_id node = {id};
            st = odin3_node_live(mod, node) ? plan_node(dc, mod, node) : st;
        }
    }
    return st;
}

/* --- applying ------------------------------------------------------------------------------ */

/* Creates a library cell of type (no parameters, 1-bit ports) with nets[p] on port p. */
static odin3_status create_cell(odin3_module *mod, odin3_celltype_id type, odin3_prov_id prov,
                                const odin3_net_id *nets, odin3_node_id *out) {
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(mod), type);
    if (def == NULL || def->n_ports > DLM_MAX_PORTS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_netvec ports[DLM_MAX_PORTS];
    for (uint32_t port = 0; port < def->n_ports; port++) {
        ports[port] = (odin3_netvec){&nets[port], 1};
    }
    odin3_node_spec spec = {type, 0, prov, NULL, 0};
    return odin3_node_create_connected(mod, &spec, ports, out);
}

/* The gate entry of the library inverter (its port indices); NULL when there is none. */
static const odin3_libgate *inv_gate(const odin3_libcells *cells) {
    for (size_t i = 0; cells->inv.v != 0 && i < cells->gates.len; i++) {
        const odin3_libgate *gate = odin3_vec_cat(&cells->gates, i);
        if (gate->type.v == cells->inv.v) {
            return gate;
        }
    }
    return NULL;
}

/* A new net driven by a new library inverter of clock; both take prov. */
static odin3_status make_inverter(dlm_ctx *dc, odin3_module *mod, odin3_net_id clock,
                                  odin3_prov_id prov, odin3_net_id *out) {
    const odin3_libgate *gate = inv_gate(&dc->cells);
    if (gate == NULL || gate->in_port[0] >= DLM_MAX_PORTS || gate->out_port >= DLM_MAX_PORTS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_net_create(mod, 0, prov, out);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id nets[DLM_MAX_PORTS] = {{0}};
    nets[gate->in_port[0]] = clock;
    nets[gate->out_port] = *out;
    odin3_node_id inv = {0};
    st = create_cell(mod, dc->cells.inv, prov, nets, &inv);
    if (st != ODIN3_OK) {
        (void)odin3_net_delete(mod, *out);
        return st;
    }
    dc->inverters++;
    return ODIN3_OK;
}

/* The provenance of the inverter of clock key: the one flip-flop's, or a DERIVED record (one
 * operation) with the provenance of every flip-flop that uses it as parents. */
static odin3_status inverter_prov(dlm_ctx *dc, uint64_t key, odin3_prov_id *out) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < dc->users.len; i++) {
        const dlm_user *user = odin3_vec_cat(&dc->users, i);
        if (user->key != key) {
            continue;
        }
        odin3_prov_id *slot = odin3_vec_push(&parents);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = user->prov;
        }
    }
    if (st == ODIN3_OK && parents.len == 1) {
        *out = *(const odin3_prov_id *)odin3_vec_cat(&parents, 0);
    } else if (st == ODIN3_OK) {
        odin3_prov_begin_op(dc->pass);
        st = odin3_prov_derive(dc->pass, (odin3_prov_list){parents.data, (uint32_t)parents.len},
                               out);
    }
    odin3_vec_free(&parents);
    return st;
}

/* The inverted clock net of clock in mod: the one made before, else a new inverter's. */
static odin3_status inverted_clock(dlm_ctx *dc, odin3_module *mod, odin3_net_id clock,
                                   odin3_net_id *out) {
    uint64_t key = clock_key(mod, clock);
    uint64_t have = 0;
    if (odin3_u64map_get(dc->clocks, key, &have)) {
        *out = (odin3_net_id){(uint32_t)have};
        return ODIN3_OK;
    }
    odin3_prov_id prov = {0};
    odin3_status st = inverter_prov(dc, key, &prov);
    st = st == ODIN3_OK ? make_inverter(dc, mod, clock, prov, out) : st;
    return st == ODIN3_OK ? odin3_u64map_put(dc->clocks, (odin3_kv){key, out->v}) : st;
}

/* The state cell that replaces old: clock on its clock port, old's D and Q nets on the others,
 * old's provenance. */
static odin3_status make_state(odin3_module *mod, odin3_node_id old, const odin3_libstate *state,
                               odin3_net_id clock, odin3_node_id *out) {
    if (state->clock_port >= DLM_MAX_PORTS || state->data_port >= DLM_MAX_PORTS ||
        state->q_port >= DLM_MAX_PORTS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id nets[DLM_MAX_PORTS] = {{0}};
    nets[state->clock_port] = clock;
    nets[state->data_port] = pin_net(mod, old, SRC_DATA);
    nets[state->q_port] = pin_net(mod, old, SRC_Q);
    return create_cell(mod, state->type, odin3_node_prov(mod, old), nets, out);
}

/* Copies every attribute of node old to node made (same module), as flatten's copy.c does. */
static odin3_status copy_one_attr(void *user, uint32_t key, const odin3_value *value) {
    attr_copy *job = user;
    return odin3_attr_set(job->mod, (odin3_objref){ODIN3_OBJ_NODE, job->dst.v}, key, value);
}

static odin3_status copy_attrs(attr_copy job) {
    return odin3_attr_foreach(job.mod, (odin3_objref){ODIN3_OBJ_NODE, job.src.v}, copy_one_attr,
                              &job);
}

/* Replaces one planned cell: create the new one, copy the attributes, delete the old, give the
 * new its name. */
static odin3_status apply_item(dlm_ctx *dc, const dlm_item *item) {
    odin3_module *mod = odin3_module_get(dc->design, (odin3_module_id){item->module});
    odin3_node_id old = {item->node};
    const odin3_libstate *state = odin3_vec_cat(&dc->cells.states, item->choice.state);
    odin3_net_id clock = pin_net(mod, old, SRC_CLOCK);
    odin3_status st = ODIN3_OK;
    if (item->choice.invert_clock && odin3_net_valid(clock)) {
        st = inverted_clock(dc, mod, clock, &clock);
    }
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? make_state(mod, old, state, clock, &made) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    st = copy_attrs((attr_copy){mod, old, made});
    if (st != ODIN3_OK) {
        (void)odin3_node_delete(mod, made);
        return st;
    }
    uint32_t name = odin3_node_name(mod, old);
    st = odin3_node_delete(mod, old);
    if (st != ODIN3_OK) {
        (void)odin3_node_delete(mod, made);
        return st;
    }
    return name != 0 ? odin3_node_rename(mod, made, name) : ODIN3_OK;
}

static odin3_status apply(dlm_ctx *dc) {
    dc->clocks = odin3_u64map_create(0);
    if (dc->clocks == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < dc->items.len; i++) {
        st = apply_item(dc, odin3_vec_cat(&dc->items, i));
    }
    odin3_u64map_destroy(dc->clocks);
    dc->clocks = NULL;
    return st;
}

/* --- logs and the pass --------------------------------------------------------------------- */

/* Appends `<cell> x<k>` for every state cell used, in library order, comma-separated. */
static odin3_status append_counts(const dlm_ctx *dc, odin3_strbuf *buf) {
    odin3_status st = ODIN3_OK;
    bool first = true;
    for (uint32_t si = 0; st == ODIN3_OK && si < dc->cells.states.len; si++) {
        uint64_t used = 0;
        for (size_t i = 0; i < dc->items.len; i++) {
            const dlm_item *item = odin3_vec_cat(&dc->items, i);
            used += item->choice.state == si ? 1U : 0U;
        }
        if (used == 0) {
            continue;
        }
        const odin3_libstate *state = odin3_vec_cat(&dc->cells.states, si);
        st = odin3_strbuf_appendf(buf, "%s%s x%" PRIu64, first ? "" : ", ",
                                  odin3_celltype_get(dc->design, state->type)->name, used);
        first = false;
    }
    return st;
}

static odin3_status log_result(const dlm_ctx *dc) {
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    odin3_status st = append_counts(dc, &list);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "dfflibmap: library '%s': %zu cells mapped%s%s%s, %" PRIu64 " clock inverters",
                  dc->library, dc->items.len, dc->items.len > 0 ? " (" : "",
                  list.data != NULL ? list.data : "", dc->items.len > 0 ? ")" : "", dc->inverters);
    }
    if (st == ODIN3_OK && dc->init_lost > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "dfflibmap: %" PRIu64 " flip-flops or latches with INIT 0 or 1 became cells "
                  "whose init is x; their initial value is lost",
                  dc->init_lost);
    }
    odin3_strbuf_free(&list);
    return st;
}

/* The four source types the design knows. */
static void find_kinds(dlm_ctx *dc) {
    for (int k = 0; k < ODIN3_DLM_KINDS; k++) {
        uint32_t str = 0;
        if (odin3_strtab_find(odin3_design_strtab(dc->design), odin3_bytes_cstr(k_kind_names[k]),
                              &str)) {
            (void)odin3_celltype_find(dc->design, str, &dc->kinds[k]);
        }
    }
}

static odin3_status run_library(odin3_pass_ctx *ctx, odin3_design *design, const char *library) {
    dlm_ctx dc = {.pass = ctx, .design = design, .library = library};
    odin3_vec_init(&dc.items, sizeof(dlm_item));
    odin3_vec_init(&dc.users, sizeof(dlm_user));
    odin3_status st = odin3_libcells_collect(design, library, &dc.cells);
    if (st == ODIN3_OK && dc.cells.n_cells == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "dfflibmap: library '%s' is not loaded; read it with read_techlib first",
                  library);
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK) {
        find_kinds(&dc);
        st = plan(&dc);
    }
    st = st == ODIN3_OK ? apply(&dc) : st;
    st = st == ODIN3_OK ? log_result(&dc) : st;
    odin3_vec_free(&dc.items);
    odin3_vec_free(&dc.users);
    odin3_libcells_free(&dc.cells);
    return st;
}

static odin3_status dlm_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                 void *user) {
    (void)user;
    odin3_bytes rest = args;
    odin3_bytes word = odin3_pass_arg_next(&rest);
    if (word.ptr == NULL || word.len == 0 || odin3_pass_arg_next(&rest).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "dfflibmap: expects one library name");
        return ODIN3_ERR_INVALID_ARG;
    }
    char *library = odin3_util_malloc(word.len + 1);
    if (library == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(library, word.ptr, word.len);
    library[word.len] = '\0';
    odin3_status st = run_library(ctx, design, library);
    odin3_util_free(library);
    return st;
}

const odin3_pass_def odin3_pass_dfflibmap = {
    "dfflibmap",
    "dfflibmap <library>: map $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_ onto the library's "
    "flip-flops and latches",
    dlm_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
