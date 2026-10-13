/*
 * expr_ffpol.c — opt_expr's FF polarity folding (2F spec §4.4; plan Task 7; Yosys 0.55
 * opt_expr.cc handle_polarity_inv and handle_clkpol_celltype_swap).
 *
 * Once per round, after the invert map is taken, every word FF, $mem, $memrd and $memwr and every
 * fine FF is visited in node-ID order: a CLK, EN, ARST, SRST, ALOAD, SET or CLR input driven by an
 * inverter's output takes the inverter's input and flips its *_POLARITY parameter; a $mem's RD_CLK
 * and WR_CLK bits flip the matching bit of RD_CLK_POLARITY / WR_CLK_POLARITY; a $_DFF_P_ /
 * $_DFF_N_ or $_DLATCH_P_ / $_DLATCH_N_ swaps its type. A node is rebuilt once however many of its
 * inputs change, through odin3_kit_rebuild_node (OP-2): it builds before its first destructive
 * step, and its new outputs are merged into the old nets, so the Q nets (and the init on them)
 * stay.
 *
 * Not the FfData round trip of ff.c: FfData::emit would pick the type again (a latch whose D is
 * constant becomes an async reset), and Yosys changes the type of nothing here.
 */
#include "passes/opt/expr.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/arena.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    MAX_PORTS = 8,     /* the most any of these types has ($mem) */
    MAX_PARAMS = 16,   /* ... and 13 parameters ($mem) */
    TABLE_MAX = 32,    /* the types below */
    RING_STEPS = 1024, /* how far an inverter chain is followed to find a ring */
    ARENA_CHUNK = 4096,
    NO_SLOT = -1,  /* a port without a polarity */
    SWAP_SLOT = -2 /* a fine FF's port: the type is the polarity */
};

/* A polarity-carrying port and the parameter that holds its polarity. */
typedef struct pol_port {
    const char *port;
    const char *param;
} pol_port;

static const pol_port k_pol_ports[] = {
    {"CLK", "CLK_POLARITY"},   {"EN", "EN_POLARITY"},         {"ARST", "ARST_POLARITY"},
    {"SRST", "SRST_POLARITY"}, {"ALOAD", "ALOAD_POLARITY"},   {"SET", "SET_POLARITY"},
    {"CLR", "CLR_POLARITY"},   {"RD_CLK", "RD_CLK_POLARITY"}, {"WR_CLK", "WR_CLK_POLARITY"},
};

/* The types whose polarity parameters fold: the word FFs of the registry (IR change D), the
 * memories (Yosys: $memrd and $memwr; Odin III's $mem holds a bit per port). */
static const char *const k_word_types[] = {
    "$dff",     "$dffe",     "$adff",   "$adffe", "$aldff",  "$aldffe",
    "$sdff",    "$sdffe",    "$sdffce", "$dffsr", "$dffsre", "$dlatch",
    "$adlatch", "$dlatchsr", "$sr",     "$mem",   "$memrd",  "$memwr",
};

/* A fine FF, its other polarity, and the port whose inverter swaps them. */
typedef struct fine_pair {
    const char *name;
    const char *swap;
    const char *port;
} fine_pair;

static const fine_pair k_fine_types[] = {
    {"$_DFF_P_", "$_DFF_N_", "C"},
    {"$_DFF_N_", "$_DFF_P_", "C"},
    {"$_DLATCH_P_", "$_DLATCH_N_", "E"},
    {"$_DLATCH_N_", "$_DLATCH_P_", "E"},
};

typedef struct pol_type {
    odin3_celltype_id id;
    odin3_celltype_id swap; /* a fine FF's other type, 0 for the rest */
    const char *swap_port;
} pol_type;

typedef struct pol_table {
    pol_type entry[TABLE_MAX];
    uint32_t n;
} pol_table;

/* The types of this design's registry that fold, resolved once per call. */
static void table_init(const odin3_design *design, pol_table *tab) {
    tab->n = 0;
    for (size_t i = 0; i < sizeof k_word_types / sizeof k_word_types[0]; i++) {
        odin3_celltype_id id = odin3_kit_type_named(design, k_word_types[i]);
        if (id.v != 0) {
            tab->entry[tab->n++] = (pol_type){id, {0}, NULL};
        }
    }
    for (size_t i = 0; i < sizeof k_fine_types / sizeof k_fine_types[0]; i++) {
        odin3_celltype_id id = odin3_kit_type_named(design, k_fine_types[i].name);
        odin3_celltype_id swap = odin3_kit_type_named(design, k_fine_types[i].swap);
        if (id.v != 0 && swap.v != 0) {
            tab->entry[tab->n++] = (pol_type){id, swap, k_fine_types[i].port};
        }
    }
}

static const pol_type *table_find(const pol_table *tab, odin3_celltype_id type) {
    for (uint32_t i = 0; i < tab->n; i++) {
        if (tab->entry[i].id.v == type.v) {
            return &tab->entry[i];
        }
    }
    return NULL;
}

/* --- what to flip ----------------------------------------------------------------------------- */

/* The slot of port `port` of def: the index of its polarity parameter, SWAP_SLOT for a fine FF's
 * clock or enable, NO_SLOT for a port without a polarity ($memrd's EN, a data port, an output). */
static int32_t polarity_slot(const odin3_celltype_def *def, const pol_type *type, uint32_t port) {
    const odin3_port_def *pdef = &def->ports[port];
    if (pdef->dir != ODIN3_DIR_IN) {
        return NO_SLOT;
    }
    if (type->swap_port != NULL) {
        return strcmp(pdef->name, type->swap_port) == 0 ? SWAP_SLOT : NO_SLOT;
    }
    for (size_t i = 0; i < sizeof k_pol_ports / sizeof k_pol_ports[0]; i++) {
        if (strcmp(pdef->name, k_pol_ports[i].port) == 0) {
            uint32_t index = odin3_kit_param_index(def, k_pol_ports[i].param);
            return index != ODIN3_KIT_NONE ? (int32_t)index : NO_SLOT;
        }
    }
    return NO_SLOT;
}

/* True when net is on a ring of inverters: following the invert map from it comes back to it. The
 * ring has no defined value; Yosys flips it once and its invert.double then removes it, here it is
 * left alone (§17 #28). */
static bool in_ring(const odin3_opt_ctx *oc, odin3_net_id net) {
    odin3_net_id walk = net;
    for (uint32_t step = 0; step < RING_STEPS; step++) {
        walk = odin3_kit_inverse(oc, walk);
        if (walk.v == 0) {
            return false;
        }
        if (walk.v == net.v) {
            return true;
        }
    }
    return false;
}

/* The net that replaces net as an input (the inverter's input), none when it is not to flip. */
static odin3_net_id flipped(const odin3_opt_ctx *oc, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return (odin3_net_id){0};
    }
    odin3_net_id inv = odin3_kit_inverse(oc, net);
    return inv.v != 0 && !in_ring(oc, net) ? inv : (odin3_net_id){0};
}

/* Whether the pins of a polarity port are each a signal of their own: a $mem's RD_CLK and WR_CLK
 * bits are, every other port is one signal (Yosys's invert map is keyed by whole signals, so a
 * WIDTH-bit SET or CLR matches only when WIDTH is 1). */
static bool bitwise(const odin3_celltype_def *def, int32_t slot) {
    return slot >= 0 && def->params[slot].kind == ODIN3_VAL_BITS;
}

/* --- one node --------------------------------------------------------------------------------- */

typedef struct pol_job {
    odin3_opt_ctx *oc;
    const odin3_celltype_def *def;
    const pol_type *type;
    odin3_node_id node;
    odin3_arena *arena;
    odin3_celltype_id new_type;
    odin3_netvec ports[MAX_PORTS];
    odin3_value params[MAX_PARAMS];
    uint32_t flips;
} pol_job;

/* One input to flip: the slot of its polarity (a parameter index, or SWAP_SLOT) and which bit of
 * a BITS polarity it is. */
typedef struct pol_ref {
    int32_t slot;
    uint32_t bit;
} pol_ref;

/* Flips the polarity parameter of ref (an INT is one bit). A BITS parameter is copied into the
 * arena first, once. */
static odin3_status flip_param(pol_job *job, pol_ref ref) {
    odin3_value *val = &job->params[ref.slot];
    if (val->kind == ODIN3_VAL_INT) {
        val->i = val->i != 0 ? 0 : 1;
        return ODIN3_OK;
    }
    if (val->kind != ODIN3_VAL_BITS || ref.bit >= val->len) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_value *old = odin3_node_param(job->oc->module, job->node, (uint32_t)ref.slot);
    if (val->bits == old->bits) {
        uint8_t *copy = odin3_arena_alloc(job->arena, (size_t)val->len + 1);
        if (copy == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memcpy(copy, val->bits, val->len);
        val->bits = copy;
    }
    uint8_t *bits = (uint8_t *)val->bits;
    bits[ref.bit] =
        bits[ref.bit] == (uint8_t)ODIN3_BIT_1 ? (uint8_t)ODIN3_BIT_0 : (uint8_t)ODIN3_BIT_1;
    return ODIN3_OK;
}

/* Counts the flip and makes it: a fine FF takes its other type, the rest flip a parameter. */
static odin3_status flip_one(pol_job *job, pol_ref ref) {
    job->flips++;
    if (ref.slot == SWAP_SLOT) {
        job->new_type = job->type->swap;
        return ODIN3_OK;
    }
    return flip_param(job, ref);
}

/* Whether port is one the node can flip (a polarity port whose pins are each a signal: see
 * bitwise), with its slot and pins. */
static bool port_eligible(const pol_job *job, uint32_t port, int32_t *slot, odin3_pinslice *pins) {
    *slot = polarity_slot(job->def, job->type, port);
    *pins = odin3_node_port(job->oc->module, job->node, port);
    return *slot != NO_SLOT && pins->count > 0 && (pins->count == 1 || bitwise(job->def, *slot));
}

static odin3_net_id pin_net(const pol_job *job, odin3_pinslice pins, uint32_t bit) {
    return odin3_pin_net(job->oc->module, (odin3_pin_id){pins.first.v + bit});
}

/* The nets of port, copied into the arena and given to the job's plan (NULL when out of memory). */
static odin3_net_id *port_nets(pol_job *job, uint32_t port, odin3_pinslice pins) {
    odin3_net_id *nets = odin3_arena_alloc(job->arena, sizeof *nets * pins.count);
    for (uint32_t bit = 0; nets != NULL && bit < pins.count; bit++) {
        nets[bit] = pin_net(job, pins, bit);
    }
    if (nets != NULL) {
        job->ports[port] = (odin3_netvec){nets, pins.count};
    }
    return nets;
}

/* Port `port` of the job: when an inverter drives some of its pins, its nets are copied into the
 * arena with those replaced by the inverter's input, and the polarity is flipped to match. A port
 * left alone stays without nets, which the kit reads as the old node's. */
static odin3_status plan_port(pol_job *job, uint32_t port) {
    int32_t slot = NO_SLOT;
    odin3_pinslice pins = {0};
    if (!port_eligible(job, port, &slot, &pins)) {
        return ODIN3_OK;
    }
    odin3_net_id *nets = NULL;
    for (uint32_t bit = 0; bit < pins.count; bit++) {
        odin3_net_id inv = flipped(job->oc, pin_net(job, pins, bit));
        if (inv.v == 0) {
            continue;
        }
        nets = nets != NULL ? nets : port_nets(job, port, pins);
        if (nets == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        nets[bit] = inv;
        odin3_status st = flip_one(job, (pol_ref){slot, pins.count == 1 ? 0 : bit});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The plan of the whole node: parameters as they are, then each port. */
static odin3_status plan_node(pol_job *job) {
    const odin3_module *module = job->oc->module;
    for (uint32_t i = 0; i < job->def->n_params; i++) {
        job->params[i] = *odin3_node_param(module, job->node, i);
    }
    job->new_type = odin3_node_type(module, job->node);
    odin3_status st = ODIN3_OK;
    for (uint32_t port = 0; st == ODIN3_OK && port < job->def->n_ports; port++) {
        st = plan_port(job, port);
    }
    return st;
}

/* True when some polarity input of the node is driven by an inverter (no allocation). */
static bool has_flip(const pol_job *job) {
    for (uint32_t port = 0; port < job->def->n_ports; port++) {
        int32_t slot = NO_SLOT;
        odin3_pinslice pins = {0};
        if (!port_eligible(job, port, &slot, &pins)) {
            continue;
        }
        for (uint32_t bit = 0; bit < pins.count; bit++) {
            if (flipped(job->oc, pin_net(job, pins, bit)).v != 0) {
                return true;
            }
        }
    }
    return false;
}

/* The counters and DEBUG lines of the flips of a rebuilt node (read before it went). */
static void account_flips(const pol_job *job, const char *type_name, uint32_t name_str) {
    odin3_opt_expr_counts *counts = &job->oc->counts->expr;
    for (uint32_t i = 0; i < job->flips; i++) {
        counts->rule[ODIN3_OPT_RULE_CLKINV]++;
        counts->rewritten++;
        odin3_expr_log_rewrite(job->oc, odin3_opt_rule_name(ODIN3_OPT_RULE_CLKINV), type_name,
                               name_str, job->node);
    }
}

static odin3_status fold_node(odin3_opt_ctx *oc, const pol_type *type, odin3_node_id node) {
    pol_job job;
    memset(&job, 0, sizeof job);
    job.oc = oc;
    job.type = type;
    job.node = node;
    job.def = odin3_celltype_get(oc->design, odin3_node_type(oc->module, node));
    if (job.def == NULL || job.def->n_ports > MAX_PORTS || job.def->n_params > MAX_PARAMS ||
        !has_flip(&job)) {
        return ODIN3_OK;
    }
    const char *type_name = job.def->name;
    uint32_t name_str = odin3_node_name(oc->module, node);
    job.arena = odin3_arena_create(ARENA_CHUNK);
    odin3_status st = job.arena != NULL ? plan_node(&job) : ODIN3_ERR_NO_MEMORY;
    odin3_node_id made = {0};
    if (st == ODIN3_OK) {
        odin3_kit_begin_rewrite(oc);
        odin3_kit_rebuild req = {node,
                                 {job.new_type, job.params, job.def->n_params, job.ports, {0}}};
        st = odin3_kit_rebuild_node(oc, &req, &made);
    }
    if (st == ODIN3_OK) {
        account_flips(&job, type_name, name_str);
    }
    odin3_arena_destroy(job.arena);
    return st;
}

odin3_status odin3_opt_expr_ffpol(odin3_opt_ctx *oc) {
    pol_table tab;
    table_init(oc->design, &tab);
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_module_node_end(oc->module);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        const pol_type *type = NULL;
        if (odin3_node_live(oc->module, node)) {
            type = table_find(&tab, odin3_node_type(oc->module, node));
        }
        st = type != NULL ? fold_node(oc, type, node) : ODIN3_OK;
    }
    return st;
}
