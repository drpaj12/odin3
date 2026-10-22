/*
 * region.c — roles, constants, CIs, COs, clock domains and the Kahn order of one module and one
 * clock domain (3D spec §5, AB-10, AB-11, AB-12; plan Task 15; points (k), (r)).
 */
#include "passes/abc/region.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "passes/abc/abc.h"
#include "passes/abc/run.h"
#include "sim/gate_truth.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Net flags (one byte per net while a region is built). */
enum {
    NF_DRIVEN = 1U << 0, /* driven by a soft cell or a register */
    NF_CI = 1U << 1,
    NF_CO = 1U << 2,
    NF_CUT = 1U << 3, /* a loop-cut net: a CI to its readers and a CO of its driver */
};

enum {
    PORT_C = 0,      /* a flip-flop's clock pin, port 0 of $_DFF_P_ and $_DFF_N_ */
    PORT_D_FF = 1,   /* ... and its D */
    PORT_Q_DFF = 2,  /* ... and its Q */
    PORT_D_GCLK = 0, /* $_FF_'s D */
    PORT_Q_GCLK = 1, /* ... and its Q */
    TEXT_BYTES = 192,
    LUT_EST_CAP = 1 << 24, /* the AIG estimate of one wide $lut stops here */
    DOMAIN_KEY_SHIFT = 2,
    EDGE_KEY_BASE = 4, /* edge keys start here; the global key is 2 */
    ROLE_OOM = 0xFF,   /* classify ran out of memory (not a role) */
    PARAM_INIT = 0,    /* a flip-flop's INIT is its first parameter */
    INIT_ONE = 1
};

/* --- text ---------------------------------------------------------------------------------- */

static const char *strtab_text(const odin3_module *module, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), str);
}

/* Own name, else primary wire bit, else "" (the sort key of point (k)). */
static void net_key(const odin3_module *module, odin3_net_id net, char *out, size_t cap) {
    out[0] = '\0';
    if (!odin3_net_valid(net)) {
        return;
    }
    uint32_t name = odin3_net_name(module, net);
    if (name != 0) {
        (void)snprintf(out, cap, "%s", strtab_text(module, name));
        return;
    }
    odin3_wirebit primary = odin3_net_primary(module, net);
    if (!odin3_wire_valid(primary.wire)) {
        return;
    }
    const char *wire = strtab_text(module, odin3_wire_name(module, primary.wire));
    if (odin3_wire_width(module, primary.wire) == 1) {
        (void)snprintf(out, cap, "%s", wire != NULL ? wire : "");
    } else {
        (void)snprintf(out, cap, "%s[%d]", wire != NULL ? wire : "",
                       (int)odin3_wire_index(module, primary.wire, primary.bit));
    }
}

void odin3_abc_net_text(const odin3_module *module, odin3_net_id net, char *out, size_t cap) {
    net_key(module, net, out, cap);
    if (out[0] == '\0') {
        (void)snprintf(out, cap, "$n%" PRIu32, net.v);
    }
}

void odin3_abc_node_text(const odin3_module *module, odin3_node_id node, char *out, size_t cap) {
    uint32_t name = odin3_node_name(module, node);
    if (name != 0) {
        (void)snprintf(out, cap, "%s", strtab_text(module, name));
    } else {
        (void)snprintf(out, cap, "$c%" PRIu32, node.v);
    }
}

void odin3_abc_domain_text(const odin3_module *module, const odin3_abc_domain *domain, char *out,
                           size_t cap) {
    if (domain->kind == ODIN3_ABC_DOM_NONE) {
        (void)snprintf(out, cap, "no flip-flops");
    } else if (domain->kind == ODIN3_ABC_DOM_GLOBAL) {
        (void)snprintf(out, cap, "global clock");
    } else {
        char net[TEXT_BYTES];
        odin3_abc_net_text(module, domain->clock, net, sizeof net);
        (void)snprintf(out, cap, "clock '%s' %s", net, domain->negedge ? "negedge" : "posedge");
    }
}

/* --- keep ---------------------------------------------------------------------------------- */

static bool keep_value(const odin3_module *module, const odin3_value *val) {
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
        const char *text = strtab_text(module, val->str);
        return text != NULL && text[0] != '\0';
    }
    default:
        return false;
    }
}

/* A non-zero `keep` attribute (Yosys get_bool_attribute) on obj; key 0: never interned. */
static bool has_keep(const odin3_module *module, uint32_t key, odin3_objref obj) {
    const odin3_value *val = key == 0 ? NULL : odin3_attr_get(module, obj, key);
    return val != NULL && keep_value(module, val);
}

/* `keep` on the net, or on a wire that holds it (primary or alias). */
static bool net_keep(const odin3_module *module, uint32_t key, odin3_net_id net) {
    if (key == 0) {
        return false;
    }
    if (has_keep(module, key, (odin3_objref){ODIN3_OBJ_NET, net.v})) {
        return true;
    }
    odin3_wirebit primary = odin3_net_primary(module, net);
    if (odin3_wire_valid(primary.wire) &&
        has_keep(module, key, (odin3_objref){ODIN3_OBJ_WIRE, primary.wire.v})) {
        return true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire) &&
            has_keep(module, key, (odin3_objref){ODIN3_OBJ_WIRE, alias.wb.wire.v})) {
            return true;
        }
    }
    return false;
}

/* --- constants ----------------------------------------------------------------------------- */

bool odin3_abc_net_undefined(const odin3_module *module, odin3_net_id net) {
    uint32_t name = odin3_net_name(module, net);
    if (name == 0 || strcmp(strtab_text(module, name), "unconn") != 0 ||
        odin3_net_driver_count(module, net) != 1) {
        return false;
    }
    odin3_node_id node = odin3_pin_node(module, odin3_net_driver(module, net));
    const odin3_design *design = odin3_module_design(module);
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
    if (def == NULL || strcmp(def->name, "$sop") != 0) {
        return false;
    }
    const odin3_value *width = odin3_node_param(module, node, 0);
    const odin3_value *cover = odin3_node_param(module, node, 1);
    return width != NULL && cover != NULL && width->i == 0 && cover->len == 0;
}

/* The value of a library TIE cell that is the one driver of net (its type classified TK_TIE,
 * gatemap review), or -1. */
static int tie_literal(const odin3_abc_run *run, odin3_net_id net) {
    if (run->gate_index == NULL || odin3_net_driver_count(run->module, net) != 1) {
        return -1;
    }
    odin3_node_id node = odin3_pin_node(run->module, odin3_net_driver(run->module, net));
    uint64_t index = 0;
    if (!odin3_u64map_get(run->gate_index, odin3_node_type(run->module, node).v, &index)) {
        return -1;
    }
    const odin3_abc_gate *entry = odin3_vec_cat(&run->gates, (size_t)index);
    return entry->kind == ODIN3_ABC_TK_TIE ? (int)entry->table : -1;
}

int odin3_abc_pin_literal(const odin3_abc_run *run, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return -1;
    }
    switch (odin3_net_const_value(run->module, net)) {
    case ODIN3_CONST_1:
        return 1;
    case ODIN3_CONST_0:
    case ODIN3_CONST_X:
    case ODIN3_CONST_Z:
        return 0;
    default:
        return tie_literal(run, net);
    }
}

/* A constant that soft logic reads as 0 without it being a real 0: x, z, or an undefined net. */
static bool reads_as_unknown(const odin3_abc_run *run, odin3_net_id net) {
    odin3_const val = odin3_net_const_value(run->module, net);
    return val == ODIN3_CONST_X || val == ODIN3_CONST_Z ||
           odin3_abc_net_undefined(run->module, net);
}

/* --- cell types ---------------------------------------------------------------------------- */

_Static_assert(ODIN3_GATE_TRUTH_MAX_INPUTS - ODIN3_ABC_GATE_MAX_INPUTS == 0,
               "the gate truth helper and the abc pass bound a gate's inputs alike");

static uint8_t type_kind(const odin3_celltype_def *def) {
    static const struct {
        const char *name;
        uint8_t kind;
    } k_named[] = {{"$sop", ODIN3_ABC_TK_SOP},
                   {"$lut", ODIN3_ABC_TK_LUT},
                   {"$_DFF_P_", ODIN3_ABC_TK_DFF_P},
                   {"$_DFF_N_", ODIN3_ABC_TK_DFF_N},
                   {"$_FF_", ODIN3_ABC_TK_FF}};
    for (size_t i = 0; i < sizeof k_named / sizeof k_named[0]; i++) {
        if (strcmp(def->name, k_named[i].name) == 0) {
            return k_named[i].kind;
        }
    }
    return odin3_gate_truth_eligible(def) ? ODIN3_ABC_TK_GATE : ODIN3_ABC_TK_OTHER;
}

/* A library cell whose fn is a constant (a TIE cell) is a constant generator, not a cut cell: its
 * value from its fn (odin3_gate_const_value). Leaves *entry as it is for any other type. */
static odin3_status tie_entry(const odin3_abc_run *run, odin3_celltype_id type,
                              const odin3_celltype_def *def, odin3_abc_gate *entry) {
    const odin3_techlib_cell *lib = def == NULL ? NULL : odin3_celltype_lib(run->design, type);
    int value = 0;
    odin3_status st =
        lib == NULL ? ODIN3_ERR_INVALID_ARG : odin3_gate_const_value(def, lib, &value);
    if (st == ODIN3_OK) {
        entry->kind = ODIN3_ABC_TK_TIE;
        entry->table = value != 0 ? 1U : 0U;
    }
    return st == ODIN3_ERR_NO_MEMORY ? st : ODIN3_OK;
}

/* The cached entry of a type, made on first use. NULL on out of memory. */
static const odin3_abc_gate *type_entry(odin3_abc_run *run, odin3_celltype_id type) {
    uint64_t index = 0;
    if (odin3_u64map_get(run->gate_index, type.v, &index)) {
        return odin3_vec_cat(&run->gates, (size_t)index);
    }
    const odin3_celltype_def *def = odin3_celltype_get(run->design, type);
    odin3_abc_gate entry = {type.v, def == NULL ? ODIN3_ABC_TK_OTHER : type_kind(def), 0, 0};
    if (entry.kind == ODIN3_ABC_TK_GATE) {
        odin3_status st =
            odin3_gate_truth(def, odin3_celltype_lib(run->design, type), &entry.table);
        if (st == ODIN3_ERR_NO_MEMORY) {
            return NULL;
        }
        /* A type the simulator cannot size (its scratch hook refuses it) is a cut cell. */
        entry.kind = st == ODIN3_OK ? ODIN3_ABC_TK_GATE : ODIN3_ABC_TK_OTHER;
        entry.n_inputs = st == ODIN3_OK ? odin3_gate_truth_inputs(def) : 0;
    } else if (entry.kind == ODIN3_ABC_TK_OTHER && tie_entry(run, type, def, &entry) != ODIN3_OK) {
        return NULL;
    }
    odin3_abc_gate *slot = odin3_vec_push(&run->gates);
    if (slot == NULL) {
        return NULL;
    }
    *slot = entry;
    if (odin3_u64map_put(run->gate_index, (odin3_kv){type.v, run->gates.len - 1}) != ODIN3_OK) {
        odin3_vec_pop(&run->gates);
        return NULL;
    }
    return slot;
}

odin3_status odin3_abc_gate_table(odin3_abc_run *run, odin3_celltype_id type, uint64_t *table,
                                  uint32_t *n_inputs) {
    const odin3_abc_gate *entry = type_entry(run, type);
    if (entry == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (entry->kind != ODIN3_ABC_TK_GATE) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *table = entry->table;
    *n_inputs = entry->n_inputs;
    return ODIN3_OK;
}

/* --- vectors ------------------------------------------------------------------------------- */

/* A vec of elem_size-byte elements, count of them, zeroed. */
static odin3_status vec_zeros(odin3_vec *vec, size_t elem_size, size_t count) {
    odin3_vec_init(vec, elem_size);
    odin3_status st = odin3_vec_reserve(vec, count == 0 ? 1 : count);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, elem_size * (count == 0 ? 1 : count));
        vec->len = count;
    }
    return st;
}

static odin3_status vec_push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static uint32_t vec_get_u32(const odin3_vec *vec, size_t index) {
    return ((const uint32_t *)vec->data)[index];
}

void odin3_abc_region_free(odin3_abc_region *region) {
    odin3_vec_free(&region->soft);
    odin3_vec_free(&region->regs);
    odin3_vec_free(&region->cis);
    odin3_vec_free(&region->cos);
    odin3_vec_free(&region->reg_d);
    odin3_vec_free(&region->role);
    odin3_vec_free(&region->ci_of);
    odin3_vec_free(&region->co_of);
    odin3_vec_free(&region->q_of);
    *region = (odin3_abc_region){0};
}

void odin3_abc_domain_free(odin3_abc_domain *domain) {
    odin3_vec_free(&domain->ffs);
    *domain = (odin3_abc_domain){0};
}

/* --- clock domains ------------------------------------------------------------------------- */

/* The Q net of a flip-flop node of the given kind. */
static odin3_net_id flop_q(const odin3_module *module, odin3_node_id node, uint8_t kind) {
    uint32_t port = kind == ODIN3_ABC_TK_FF ? PORT_Q_GCLK : PORT_Q_DFF;
    odin3_pinslice pins = odin3_node_port(module, node, port);
    odin3_net_id none = {0};
    return pins.count == 0 ? none : odin3_pin_net(module, pins.first);
}

static odin3_net_id flop_clock(const odin3_module *module, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_port(module, node, PORT_C);
    odin3_net_id none = {0};
    return pins.count == 0 ? none : odin3_pin_net(module, pins.first);
}

/* A flip-flop that belongs to some domain: no keep, and its Q net has no other driver. */
static bool flop_in_a_domain(const odin3_module *module, uint32_t keep_key, odin3_node_id node,
                             uint8_t kind) {
    if (has_keep(module, keep_key, (odin3_objref){ODIN3_OBJ_NODE, node.v})) {
        return false;
    }
    odin3_net_id qnet = flop_q(module, node, kind);
    return !odin3_net_valid(qnet) || odin3_net_driver_count(module, qnet) == 1;
}

static uint32_t keep_key_of(const odin3_module *module) {
    uint32_t key = 0;
    const odin3_strtab *tab = odin3_design_strtab(odin3_module_design(module));
    return odin3_strtab_find(tab, odin3_bytes_cstr("keep"), &key) ? key : 0;
}

bool odin3_abc_net_keep(const odin3_module *module, odin3_net_id net) {
    return net_keep(module, keep_key_of(module), net);
}

/* The group of key in domains (made when missing; the map holds key -> index). */
static odin3_abc_domain *domain_group(odin3_vec *domains, odin3_u64map *index,
                                      const odin3_abc_domain *proto, uint64_t key) {
    uint64_t at = 0;
    if (odin3_u64map_get(index, key, &at)) {
        return odin3_vec_at(domains, (size_t)at);
    }
    odin3_abc_domain *slot = odin3_vec_push(domains);
    if (slot == NULL) {
        return NULL;
    }
    *slot = *proto;
    odin3_vec_init(&slot->ffs, sizeof(odin3_node_id));
    if (odin3_u64map_put(index, (odin3_kv){key, domains->len - 1}) != ODIN3_OK) {
        odin3_vec_pop(domains);
        return NULL;
    }
    return slot;
}

/* Sort order of §5.3: clock name bytes, P before N, net ID; the global domain last. */
static int domain_compare(const odin3_module *module, const odin3_abc_domain *lhs,
                          const odin3_abc_domain *rhs) {
    if ((lhs->kind == ODIN3_ABC_DOM_GLOBAL) != (rhs->kind == ODIN3_ABC_DOM_GLOBAL)) {
        return lhs->kind == ODIN3_ABC_DOM_GLOBAL ? 1 : -1;
    }
    if (lhs->kind == ODIN3_ABC_DOM_GLOBAL) {
        return 0;
    }
    char lkey[TEXT_BYTES];
    char rkey[TEXT_BYTES];
    net_key(module, lhs->clock, lkey, sizeof lkey);
    net_key(module, rhs->clock, rkey, sizeof rkey);
    int cmp = strcmp(lkey, rkey);
    if (cmp != 0) {
        return cmp;
    }
    if (lhs->negedge != rhs->negedge) {
        return lhs->negedge ? 1 : -1;
    }
    if (lhs->clock.v == rhs->clock.v) {
        return 0;
    }
    return lhs->clock.v < rhs->clock.v ? -1 : 1;
}

static void domains_sort(const odin3_module *module, odin3_vec *domains) {
    odin3_abc_domain *items = domains->data;
    for (size_t i = 1; i < domains->len; i++) {
        odin3_abc_domain moving = items[i];
        size_t j = i;
        while (j > 0 && domain_compare(module, &items[j - 1], &moving) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = moving;
    }
}

void odin3_abc_domains_free(odin3_vec *domains) {
    for (size_t i = 0; i < domains->len; i++) {
        odin3_abc_domain_free(odin3_vec_at(domains, i));
    }
    odin3_vec_free(domains);
}

/* Adds the flip-flop node to its group; false on out of memory. */
static bool domain_add(odin3_vec *domains, odin3_u64map *index, const odin3_module *module,
                       odin3_node_id node, uint8_t kind) {
    odin3_abc_domain proto = {ODIN3_ABC_DOM_GLOBAL, false, {0}, {0}};
    uint64_t key = (uint64_t)ODIN3_ABC_DOM_GLOBAL;
    if (kind != ODIN3_ABC_TK_FF) {
        proto.kind = ODIN3_ABC_DOM_EDGE;
        proto.negedge = kind == ODIN3_ABC_TK_DFF_N;
        proto.clock = flop_clock(module, node);
        key = ((uint64_t)proto.clock.v << DOMAIN_KEY_SHIFT) | (proto.negedge ? 1U : 0U);
        key += EDGE_KEY_BASE; /* never equal to the global key */
    }
    odin3_abc_domain *group = domain_group(domains, index, &proto, key);
    return group != NULL && vec_push_u32(&group->ffs, node.v) == ODIN3_OK;
}

/* Groups the module's flip-flops into *domains (unsorted). */
static odin3_status domains_group(odin3_abc_run *run, const odin3_module *module,
                                  odin3_vec *domains) {
    odin3_u64map *index = odin3_u64map_create(0);
    if (index == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t key = keep_key_of(module);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; id < odin3_module_node_end(module) && st == ODIN3_OK; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        const odin3_abc_gate *entry = type_entry(run, odin3_node_type(module, node));
        bool grouped = entry != NULL && entry->kind >= ODIN3_ABC_TK_DFF_P &&
                       flop_in_a_domain(module, key, node, entry->kind);
        if (entry == NULL || (grouped && !domain_add(domains, index, module, node, entry->kind))) {
            st = ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_u64map_destroy(index);
    return st;
}

odin3_status odin3_abc_domains(odin3_abc_run *run, const odin3_module *module, odin3_vec *domains) {
    odin3_vec_init(domains, sizeof(odin3_abc_domain));
    odin3_status st = domains_group(run, module, domains);
    uint32_t cap = odin3_abc_run_limits_get()->domains;
    if (st == ODIN3_OK && domains->len > cap) {
        odin3_log(ODIN3_LOG_ERROR, "abc: module '%s' has %zu clock domains; at most %u",
                  strtab_text(module, odin3_module_name(module)), domains->len, (unsigned)cap);
        st = ODIN3_ERR_PARSE;
    }
    if (st == ODIN3_OK && domains->len == 0) {
        odin3_abc_domain *none = odin3_vec_push(domains);
        if (none == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            *none = (odin3_abc_domain){ODIN3_ABC_DOM_NONE, false, {0}, {0}};
            odin3_vec_init(&none->ffs, sizeof(odin3_node_id));
        }
    }
    if (st != ODIN3_OK) {
        odin3_abc_domains_free(domains);
        odin3_vec_init(domains, sizeof(odin3_abc_domain));
        return st;
    }
    domains_sort(module, domains);
    return ODIN3_OK;
}

/* --- classification ------------------------------------------------------------------------ */

/* The state of one region build (the scan context). */
typedef struct rscan {
    odin3_abc_run *run;
    odin3_module *module;
    odin3_abc_region *region;
    const odin3_abc_domain *domain;
    uint32_t keep_key;
    odin3_vec nflag; /* uint8_t per net */
    odin3_vec pos;   /* uint32_t per node: 1 + index into region->soft (build order) */
} rscan;

static uint8_t *role_at(rscan *sc, uint32_t node) {
    return (uint8_t *)sc->region->role.data + node;
}

static uint8_t *nflag_at(rscan *sc, uint32_t net) {
    return (uint8_t *)sc->nflag.data + net;
}

/* Every output pin of node sits on a net it alone drives (or on none). */
static bool sole_driver(const rscan *sc, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(sc->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(sc->module, pin);
        if (odin3_pin_drives(sc->module, pin) && odin3_net_valid(net) &&
            odin3_net_driver_count(sc->module, net) != 1) {
            return false;
        }
    }
    return true;
}

/* The const_value of a node: its type's hook over the node's parameters (up to 2 here). */
static odin3_const node_const(const rscan *sc, const odin3_celltype_def *def, odin3_node_id node) {
    if (def->const_value == NULL || def->n_params > 2) {
        return ODIN3_CONST_NONE;
    }
    odin3_value params[2];
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = *odin3_node_param(sc->module, node, i);
    }
    return def->const_value(params);
}

/* Counts a cut cell of the given kind (-1: not counted). */
static void count_cut(rscan *sc, int kind) {
    if (kind >= 0) {
        sc->region->cut_counts[kind]++;
    }
}

/* The cut kind of a non-soft, non-register cell. */
static int cut_kind_of(const odin3_celltype_def *def) {
    if (def->gran == ODIN3_GRAN_PORT) {
        return -1;
    }
    if ((def->flags & ODIN3_CT_TRISTATE) != 0) {
        return ODIN3_ABC_CUT_TRISTATE;
    }
    if ((def->flags & ODIN3_CT_SEQ_LEVEL) != 0) {
        return ODIN3_ABC_CUT_LATCH;
    }
    if (def->gran == ODIN3_GRAN_HARD || def->gran == ODIN3_GRAN_BLACKBOX) {
        return ODIN3_ABC_CUT_HARD;
    }
    return def->gran == ODIN3_GRAN_MODULE ? ODIN3_ABC_CUT_INSTANCE : ODIN3_ABC_CUT_OTHER;
}

/* A flip-flop's role: REG when it is in the run's domain (marked beforehand), else a cut cell
 * (other domains' flip-flops are not counted). */
static uint8_t flop_role(rscan *sc, odin3_node_id node, uint8_t kind) {
    if (flop_in_a_domain(sc->module, sc->keep_key, node, kind)) {
        return *role_at(sc, node.v) == ODIN3_ABC_ROLE_REG ? ODIN3_ABC_ROLE_REG : ODIN3_ABC_ROLE_CUT;
    }
    count_cut(sc, ODIN3_ABC_CUT_OTHER);
    return ODIN3_ABC_ROLE_CUT;
}

/* Soft candidates: $sop, $lut, gates; keep or a bus output makes them cut cells. */
static uint8_t logic_role(rscan *sc, odin3_node_id node, const odin3_celltype_def *def) {
    if (node_const(sc, def, node) != ODIN3_CONST_NONE && sole_driver(sc, node)) {
        return ODIN3_ABC_ROLE_CONST;
    }
    if (has_keep(sc->module, sc->keep_key, (odin3_objref){ODIN3_OBJ_NODE, node.v})) {
        count_cut(sc, ODIN3_ABC_CUT_OTHER);
        return ODIN3_ABC_ROLE_CUT;
    }
    if (!sole_driver(sc, node)) {
        count_cut(sc, ODIN3_ABC_CUT_TRISTATE);
        return ODIN3_ABC_ROLE_CUT;
    }
    return ODIN3_ABC_ROLE_SOFT;
}

static uint8_t classify(rscan *sc, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(sc->module, node);
    const odin3_abc_gate *entry = type_entry(sc->run, type);
    const odin3_celltype_def *def = odin3_celltype_get(sc->run->design, type);
    if (entry == NULL || def == NULL) {
        return ROLE_OOM;
    }
    switch (entry->kind) {
    case ODIN3_ABC_TK_SOP:
    case ODIN3_ABC_TK_LUT:
    case ODIN3_ABC_TK_GATE:
        return logic_role(sc, node, def);
    case ODIN3_ABC_TK_DFF_P:
    case ODIN3_ABC_TK_DFF_N:
    case ODIN3_ABC_TK_FF:
        return flop_role(sc, node, entry->kind);
    case ODIN3_ABC_TK_TIE:
        if (sole_driver(sc, node)) {
            return ODIN3_ABC_ROLE_CONST;
        }
        break;
    default:
        break;
    }
    if ((def->flags & ODIN3_CT_ANYVIEW) != 0 && node_const(sc, def, node) != ODIN3_CONST_NONE &&
        sole_driver(sc, node)) {
        return ODIN3_ABC_ROLE_CONST;
    }
    count_cut(sc, cut_kind_of(def));
    return ODIN3_ABC_ROLE_CUT;
}

/* GM-6: under abc --gates a flip-flop whose INIT is 1 stays a cut point (a library gate cannot be
 * flipped as restore.c flips a LUT), counted as an other cell. */
static bool gates_cut_init_one(rscan *sc, uint32_t node) {
    if (!sc->run->gates_mode) {
        return false;
    }
    const odin3_value *init = odin3_node_param(sc->module, (odin3_node_id){node}, PARAM_INIT);
    if (init == NULL || init->kind != ODIN3_VAL_INT || init->i != INIT_ONE) {
        return false;
    }
    count_cut(sc, ODIN3_ABC_CUT_OTHER);
    return true;
}

/* Marks the run's flip-flops REG, then classifies every live node into region->role. */
static odin3_status classify_all(rscan *sc) {
    odin3_abc_region *region = sc->region;
    uint32_t end = odin3_module_node_end(sc->module);
    odin3_status st = vec_zeros(&region->role, 1, end);
    for (size_t i = 0; st == ODIN3_OK && sc->domain != NULL && i < sc->domain->ffs.len; i++) {
        uint32_t node = vec_get_u32(&sc->domain->ffs, i);
        if (!gates_cut_init_one(sc, node)) {
            *role_at(sc, node) = ODIN3_ABC_ROLE_REG;
        }
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(sc->module, node)) {
            *role_at(sc, id) = ODIN3_ABC_ROLE_CUT;
            continue;
        }
        uint8_t role = classify(sc, node);
        if (role == ROLE_OOM) {
            st = ODIN3_ERR_NO_MEMORY;
            break;
        }
        *role_at(sc, id) = role;
        if (role == ODIN3_ABC_ROLE_SOFT) {
            st = vec_push_u32(&region->soft, id);
        } else if (role == ODIN3_ABC_ROLE_REG) {
            st = vec_push_u32(&region->regs, id);
        }
    }
    return st;
}

/* --- nets: driven, CI, CO ------------------------------------------------------------------- */

static void mark_driven(rscan *sc, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(sc->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(sc->module, pin);
        if (odin3_pin_drives(sc->module, pin) && odin3_net_valid(net)) {
            *nflag_at(sc, net.v) |= NF_DRIVEN;
        }
    }
}

/* One data-pin read (a soft input or a register's D): constants count, free nets become CIs. */
static void read_pin(rscan *sc, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return;
    }
    if (odin3_abc_pin_literal(sc->run, net) >= 0) {
        sc->region->zero_pins += reads_as_unknown(sc->run, net) ? 1U : 0U;
        return;
    }
    if ((*nflag_at(sc, net.v) & NF_DRIVEN) == 0 || (*nflag_at(sc, net.v) & NF_CUT) != 0) {
        *nflag_at(sc, net.v) |= NF_CI;
    }
}

/* The upper bound of a soft cell's AIG objects (point (r)). */
static uint64_t soft_estimate(const rscan *sc, odin3_node_id node, const odin3_abc_gate *entry) {
    if (entry->kind == ODIN3_ABC_TK_SOP) {
        const odin3_value *cover = odin3_node_param(sc->module, node, 1);
        return cover == NULL ? 0 : cover->len;
    }
    uint32_t inputs = entry->n_inputs;
    if (entry->kind == ODIN3_ABC_TK_LUT) {
        const odin3_value *width = odin3_node_param(sc->module, node, 0);
        inputs = width == NULL ? 0 : (uint32_t)width->i;
    }
    uint64_t rows = 1ULL << (inputs == 0 ? 0 : inputs - 1);
    uint64_t est = rows * (inputs + 1ULL);
    return est > LUT_EST_CAP ? LUT_EST_CAP : est;
}

static void add_sat(uint64_t *total, uint64_t more) {
    *total = *total > UINT64_MAX - more ? UINT64_MAX : *total + more;
}

static odin3_status scan_soft_reads(rscan *sc) {
    for (size_t idx = 0; idx < sc->region->soft.len; idx++) {
        odin3_node_id node = {vec_get_u32(&sc->region->soft, idx)};
        const odin3_abc_gate *entry = type_entry(sc->run, odin3_node_type(sc->module, node));
        if (entry == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        odin3_pinslice pins = odin3_node_pins(sc->module, node);
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            if (odin3_pin_dir(sc->module, pin) == ODIN3_DIR_IN) {
                read_pin(sc, odin3_pin_net(sc->module, pin));
            }
        }
        add_sat(&sc->region->objs_est, soft_estimate(sc, node, entry));
    }
    return ODIN3_OK;
}

static odin3_net_id reg_d_net(const rscan *sc, odin3_node_id node) {
    const odin3_abc_gate *entry = type_entry(sc->run, odin3_node_type(sc->module, node));
    uint32_t port = entry != NULL && entry->kind == ODIN3_ABC_TK_FF ? PORT_D_GCLK : PORT_D_FF;
    odin3_pinslice pins = odin3_node_port(sc->module, node, port);
    odin3_net_id none = {0};
    return pins.count == 0 ? none : odin3_pin_net(sc->module, pins.first);
}

static odin3_status scan_reg_reads(rscan *sc) {
    for (size_t idx = 0; idx < sc->region->regs.len; idx++) {
        odin3_node_id node = {vec_get_u32(&sc->region->regs, idx)};
        odin3_net_id dnet = reg_d_net(sc, node);
        odin3_status st = vec_push_u32(&sc->region->reg_d, dnet.v);
        if (st != ODIN3_OK) {
            return st;
        }
        read_pin(sc, dnet);
    }
    return ODIN3_OK;
}

/* True when a pin of net makes it a CO: a reader that is not soft logic, or a register's clock. */
static bool read_by_cut(const rscan *sc, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(sc->module, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = pins.pins[i];
        if (!odin3_pin_reads(sc->module, pin)) {
            continue;
        }
        odin3_node_id node = odin3_pin_node(sc->module, pin);
        uint8_t role = ((const uint8_t *)sc->region->role.data)[node.v];
        if (role == ODIN3_ABC_ROLE_SOFT) {
            continue;
        }
        if (role != ODIN3_ABC_ROLE_REG || odin3_pin_port(sc->module, pin) == PORT_C) {
            return true;
        }
    }
    return false;
}

/* One ascending sweep over the nets: the CI and CO lists and their index maps. */
static odin3_status build_net_lists(rscan *sc) {
    odin3_abc_region *region = sc->region;
    uint32_t end = odin3_module_net_end(sc->module);
    odin3_status st = vec_zeros(&region->ci_of, sizeof(uint32_t), end);
    st = st == ODIN3_OK ? vec_zeros(&region->co_of, sizeof(uint32_t), end) : st;
    st = st == ODIN3_OK ? vec_zeros(&region->q_of, sizeof(uint32_t), end) : st;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_net_id net = {id};
        if (!odin3_net_live(sc->module, net)) {
            continue;
        }
        uint8_t flags = *nflag_at(sc, id);
        bool is_co = (flags & NF_DRIVEN) != 0 && ((flags & NF_CUT) != 0 || read_by_cut(sc, net) ||
                                                  net_keep(sc->module, sc->keep_key, net));
        if ((flags & NF_CI) != 0) {
            ((uint32_t *)region->ci_of.data)[id] = (uint32_t)region->cis.len + 1;
            st = vec_push_u32(&region->cis, id);
        }
        if (st == ODIN3_OK && is_co) {
            ((uint32_t *)region->co_of.data)[id] = (uint32_t)region->cos.len + 1;
            st = vec_push_u32(&region->cos, id);
        }
    }
    return st;
}

/* --- Kahn order ---------------------------------------------------------------------------- */

static void heap_swap(uint32_t *lhs, uint32_t *rhs) {
    uint32_t tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

odin3_status odin3_abc_heap_push(odin3_vec *heap, uint32_t value) {
    odin3_status st = vec_push_u32(heap, value);
    uint32_t *items = heap->data;
    for (size_t at = heap->len - 1; st == ODIN3_OK && at > 0;) {
        size_t parent = (at - 1) / 2;
        if (items[parent] <= items[at]) {
            break;
        }
        heap_swap(&items[parent], &items[at]);
        at = parent;
    }
    return st;
}

uint32_t odin3_abc_heap_pop(odin3_vec *heap) {
    uint32_t *items = heap->data;
    uint32_t top = items[0];
    items[0] = items[--heap->len];
    for (size_t at = 0;;) {
        size_t child = 2 * at + 1;
        if (child >= heap->len) {
            break;
        }
        child += (child + 1 < heap->len && items[child + 1] < items[child]) ? 1U : 0U;
        if (items[at] <= items[child]) {
            break;
        }
        heap_swap(&items[at], &items[child]);
        at = child;
    }
    return top;
}

/* Kahn bookkeeping: in-degree and emitted flag per soft index. */
typedef struct kahn {
    odin3_vec indeg; /* uint32_t per soft index */
    odin3_vec done;  /* uint8_t per soft index */
    odin3_vec heap;  /* soft indices as node IDs: min-heap */
    odin3_vec order; /* the result */
} kahn;

static uint32_t soft_index(const rscan *sc, uint32_t node) {
    return vec_get_u32(&sc->pos, node) - 1;
}

/* The soft node driving net, or 0. */
static uint32_t soft_driver(const rscan *sc, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(sc->module, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(sc->module, odin3_net_driver(sc->module, net));
    return ((const uint8_t *)sc->region->role.data)[node.v] == ODIN3_ABC_ROLE_SOFT ? node.v : 0;
}

/* One edge from a cut or emitted driver to a soft reader pin is gone. */
static odin3_status release_readers(const rscan *sc, kahn *kh, odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(sc->module, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        uint32_t reader = odin3_pin_node(sc->module, sinks.pins[i]).v;
        if (((const uint8_t *)sc->region->role.data)[reader] != ODIN3_ABC_ROLE_SOFT) {
            continue;
        }
        uint32_t idx = soft_index(sc, reader);
        uint32_t *deg = (uint32_t *)kh->indeg.data + idx;
        if (((uint8_t *)kh->done.data)[idx] != 0 || *deg == 0) {
            continue;
        }
        *deg -= 1U;
        if (*deg == 0) {
            odin3_status st = odin3_abc_heap_push(&kh->heap, reader);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* The output net of a soft node (Y, its only output pin), none when unconnected. */
static odin3_net_id soft_out_net(const rscan *sc, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(sc->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(sc->module, pin)) {
            return odin3_pin_net(sc->module, pin);
        }
    }
    return (odin3_net_id){0};
}

static odin3_status kahn_init(rscan *sc, kahn *kh) {
    size_t count = sc->region->soft.len;
    odin3_vec_init(&kh->indeg, sizeof(uint32_t));
    odin3_vec_init(&kh->done, 1);
    odin3_vec_init(&kh->heap, sizeof(uint32_t));
    odin3_vec_init(&kh->order, sizeof(uint32_t));
    odin3_status st = vec_zeros(&kh->indeg, sizeof(uint32_t), count);
    st = st == ODIN3_OK ? vec_zeros(&kh->done, 1, count) : st;
    st = st == ODIN3_OK ? vec_zeros(&sc->pos, sizeof(uint32_t), odin3_module_node_end(sc->module))
                        : st;
    for (size_t idx = 0; st == ODIN3_OK && idx < count; idx++) {
        ((uint32_t *)sc->pos.data)[vec_get_u32(&sc->region->soft, idx)] = (uint32_t)idx + 1;
    }
    for (size_t idx = 0; st == ODIN3_OK && idx < count; idx++) {
        odin3_node_id node = {vec_get_u32(&sc->region->soft, idx)};
        odin3_pinslice pins = odin3_node_pins(sc->module, node);
        uint32_t deg = 0;
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            deg += odin3_pin_dir(sc->module, pin) == ODIN3_DIR_IN &&
                           soft_driver(sc, odin3_pin_net(sc->module, pin)) != 0
                       ? 1U
                       : 0U;
        }
        ((uint32_t *)kh->indeg.data)[idx] = deg;
        st = deg == 0 ? odin3_abc_heap_push(&kh->heap, node.v) : ODIN3_OK;
    }
    return st;
}

static void kahn_free(kahn *kh) {
    odin3_vec_free(&kh->indeg);
    odin3_vec_free(&kh->done);
    odin3_vec_free(&kh->heap);
    odin3_vec_free(&kh->order);
}

/* Emits node: appends it and releases the readers of its output net. */
static odin3_status kahn_emit(rscan *sc, kahn *kh, uint32_t node) {
    ((uint8_t *)kh->done.data)[soft_index(sc, node)] = 1;
    odin3_status st = vec_push_u32(&kh->order, node);
    odin3_net_id out = soft_out_net(sc, (odin3_node_id){node});
    return st == ODIN3_OK && odin3_net_valid(out) && (*nflag_at(sc, out.v) & NF_CUT) == 0
               ? release_readers(sc, kh, out)
               : st;
}

/* Cuts the output net of the lowest-ID cell not yet emitted (§5.2 Loops) and releases its readers.
 */
static odin3_status kahn_cut(rscan *sc, kahn *kh, size_t *scan) {
    const uint8_t *done = kh->done.data;
    while (done[*scan] != 0) {
        (*scan)++;
    }
    odin3_node_id node = {vec_get_u32(&sc->region->soft, *scan)};
    odin3_net_id out = soft_out_net(sc, node);
    if (!odin3_net_valid(out)) {
        /* nothing reads it: it only sits behind the loop, so it is emitted without a cut */
        return kahn_emit(sc, kh, node.v);
    }
    char node_text[TEXT_BYTES];
    char net_text[TEXT_BYTES];
    odin3_abc_node_text(sc->module, node, node_text, sizeof node_text);
    odin3_abc_net_text(sc->module, out, net_text, sizeof net_text);
    const odin3_celltype_def *def =
        odin3_celltype_get(sc->run->design, odin3_node_type(sc->module, node));
    odin3_log(ODIN3_LOG_WARN,
              "abc: module '%s': combinational loop through %s '%s'; its output %s is a cut point",
              strtab_text(sc->module, odin3_module_name(sc->module)), def->name, node_text,
              net_text);
    *nflag_at(sc, out.v) |= NF_CUT;
    return release_readers(sc, kh, out);
}

static odin3_status kahn_order(rscan *sc) {
    kahn kh;
    odin3_status st = kahn_init(sc, &kh);
    size_t scan = 0;
    while (st == ODIN3_OK && kh.order.len < sc->region->soft.len) {
        if (kh.heap.len > 0) {
            st = kahn_emit(sc, &kh, odin3_abc_heap_pop(&kh.heap));
        } else {
            st = kahn_cut(sc, &kh, &scan);
        }
    }
    if (st == ODIN3_OK) {
        odin3_vec_free(&sc->region->soft);
        sc->region->soft = kh.order;
        odin3_vec_init(&kh.order, sizeof(uint32_t));
    }
    kahn_free(&kh);
    return st;
}

/* --- the region ---------------------------------------------------------------------------- */

static void region_init(odin3_abc_region *region) {
    *region = (odin3_abc_region){0};
    odin3_vec_init(&region->soft, sizeof(uint32_t));
    odin3_vec_init(&region->regs, sizeof(uint32_t));
    odin3_vec_init(&region->cis, sizeof(uint32_t));
    odin3_vec_init(&region->cos, sizeof(uint32_t));
    odin3_vec_init(&region->reg_d, sizeof(uint32_t));
    odin3_vec_init(&region->role, 1);
    odin3_vec_init(&region->ci_of, sizeof(uint32_t));
    odin3_vec_init(&region->co_of, sizeof(uint32_t));
    odin3_vec_init(&region->q_of, sizeof(uint32_t));
}

static odin3_status mark_all_driven(rscan *sc) {
    odin3_status st = vec_zeros(&sc->nflag, 1, odin3_module_net_end(sc->module));
    for (size_t idx = 0; st == ODIN3_OK && idx < sc->region->soft.len; idx++) {
        mark_driven(sc, (odin3_node_id){vec_get_u32(&sc->region->soft, idx)});
    }
    for (size_t idx = 0; st == ODIN3_OK && idx < sc->region->regs.len; idx++) {
        mark_driven(sc, (odin3_node_id){vec_get_u32(&sc->region->regs, idx)});
    }
    return st;
}

static odin3_status build_steps(rscan *sc) {
    odin3_status st = classify_all(sc);
    st = st == ODIN3_OK ? mark_all_driven(sc) : st;
    st = st == ODIN3_OK ? kahn_order(sc) : st; /* loop cuts first: they decide the CIs */
    st = st == ODIN3_OK ? scan_soft_reads(sc) : st;
    st = st == ODIN3_OK ? scan_reg_reads(sc) : st;
    st = st == ODIN3_OK ? build_net_lists(sc) : st;
    for (size_t idx = 0; st == ODIN3_OK && idx < sc->region->regs.len; idx++) {
        odin3_node_id reg = {vec_get_u32(&sc->region->regs, idx)};
        const odin3_abc_gate *entry = type_entry(sc->run, odin3_node_type(sc->module, reg));
        odin3_net_id qnet = flop_q(sc->module, reg, entry == NULL ? ODIN3_ABC_TK_FF : entry->kind);
        if (odin3_net_valid(qnet)) {
            ((uint32_t *)sc->region->q_of.data)[qnet.v] = (uint32_t)idx + 1;
        }
    }
    if (st == ODIN3_OK) {
        add_sat(&sc->region->objs_est, 1ULL + sc->region->cis.len + sc->region->regs.len +
                                           sc->region->cos.len + sc->region->regs.len);
    }
    return st;
}

odin3_status odin3_abc_region_build(odin3_abc_run *run, const odin3_abc_domain *domain,
                                    odin3_abc_region *out) {
    odin3_abc_region region;
    region_init(&region);
    rscan sc = {run, run->module, &region, domain, keep_key_of(run->module), {0}, {0}};
    odin3_vec_init(&sc.nflag, 1);
    odin3_vec_init(&sc.pos, sizeof(uint32_t));
    odin3_status st = build_steps(&sc);
    odin3_vec_free(&sc.nflag);
    odin3_vec_free(&sc.pos);
    if (st != ODIN3_OK) {
        odin3_abc_region_free(&region);
        *out = (odin3_abc_region){0};
        return st;
    }
    *out = region;
    return ODIN3_OK;
}
