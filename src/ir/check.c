/* check.c — the IR invariant checker (IR §9): rules 1–11, fast and full, in linear time. */
#include "ir/check.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* IR §9 rule numbers. */
enum check_rule_num {
    RULE_PINS = 1,
    RULE_PIN_NET,
    RULE_PARTITION,
    RULE_DRIVERS,
    RULE_NODES,
    RULE_NAMES,
    RULE_PROV,
    RULE_WIRES,
    RULE_PORTS,
    RULE_VIEW,
    RULE_DANGLING,
    RULE_LAST = RULE_DANGLING
};

enum {
    SHOWN_MAX = 10,  /* violations logged per rule and module; the rest are summed */
    DETAIL_BUF = 512 /* formatted detail of one violation */
};

/* A rule and the severity of one of its clauses (E: error, W: warning). */
typedef struct check_rule {
    enum check_rule_num num;
    odin3_log_level level;
} check_rule;

static const check_rule R1 = {RULE_PINS, ODIN3_LOG_ERROR};
static const check_rule R2 = {RULE_PIN_NET, ODIN3_LOG_ERROR};
static const check_rule R3 = {RULE_PARTITION, ODIN3_LOG_ERROR};
static const check_rule R4_E = {RULE_DRIVERS, ODIN3_LOG_ERROR};
static const check_rule R4_W = {RULE_DRIVERS, ODIN3_LOG_WARN};
static const check_rule R5 = {RULE_NODES, ODIN3_LOG_ERROR};
static const check_rule R6 = {RULE_NAMES, ODIN3_LOG_ERROR};
static const check_rule R7 = {RULE_PROV, ODIN3_LOG_ERROR};
static const check_rule R8 = {RULE_WIRES, ODIN3_LOG_ERROR};
static const check_rule R9 = {RULE_PORTS, ODIN3_LOG_ERROR};
static const check_rule R10 = {RULE_VIEW, ODIN3_LOG_ERROR};
static const check_rule R11_E = {RULE_DANGLING, ODIN3_LOG_ERROR};
static const check_rule R11_W = {RULE_DANGLING, ODIN3_LOG_WARN};

/* One run of the checker over one module (or over the design's records). */
typedef struct check_ctx {
    const odin3_module *module; /* NULL for the design-level record check */
    const odin3_design *design;
    const char *where; /* the module name, or "design" */
    odin3_view view;
    uint32_t shown[RULE_LAST + 1];
    uint32_t hidden_errors[RULE_LAST + 1];
    uint32_t hidden_warnings[RULE_LAST + 1];
    uint32_t no_prov; /* live objects with prov 0 (rule 7, one warning) */
    bool failed;      /* an E rule was violated */
} check_ctx;

/* Mark arrays of a FULL check (rules 8 and 9), allocated before anything is checked. */
typedef struct check_marks {
    size_t *bit_base;   /* per wire ID: index of its bit 0 in bits */
    uint8_t *bits;      /* per bit of a live wire: matched by a primary or alias */
    uint8_t *port_node; /* per node ID: listed in the port list */
} check_marks;

/* --- reporting ----------------------------------------------------------------------------- */

ODIN3_PRINTF(3, 4)
static void violation(check_ctx *ctx, check_rule rule, const char *fmt, ...) {
    if (rule.level == ODIN3_LOG_ERROR) {
        ctx->failed = true;
    }
    if (ctx->shown[rule.num] >= SHOWN_MAX) {
        uint32_t *hidden =
            rule.level == ODIN3_LOG_ERROR ? ctx->hidden_errors : ctx->hidden_warnings;
        hidden[rule.num]++;
        return;
    }
    ctx->shown[rule.num]++;
    char detail[DETAIL_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);
    odin3_log(rule.level, "check: %s: rule %u: %s", ctx->where, (unsigned)rule.num, detail);
}

static void summarize(const check_ctx *ctx) {
    for (uint32_t num = RULE_PINS; num <= RULE_LAST; num++) {
        if (ctx->hidden_errors[num] > 0) {
            odin3_log(ODIN3_LOG_ERROR, "check: %s: rule %u: %u more not shown", ctx->where, num,
                      ctx->hidden_errors[num]);
        }
        if (ctx->hidden_warnings[num] > 0) {
            odin3_log(ODIN3_LOG_WARN, "check: %s: rule %u: %u more not shown", ctx->where, num,
                      ctx->hidden_warnings[num]);
        }
    }
    if (ctx->no_prov > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "check: %s: rule %u: %u live objects have prov 0 (no provenance; a warning in "
                  "Phase 1)",
                  ctx->where, (unsigned)RULE_PROV, ctx->no_prov);
    }
}

/* A strtab string for messages; "unnamed" for 0 or a bad ID. */
static const char *label(const check_ctx *ctx, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(ctx->design);
    return str != 0 && str < odin3_strtab_count(tab) ? odin3_strtab_get(tab, str) : "unnamed";
}

static const char *node_label(const check_ctx *ctx, odin3_node_id node) {
    return label(ctx, odin3_node_name(ctx->module, node));
}

static const char *net_label(const check_ctx *ctx, odin3_net_id net) {
    return label(ctx, odin3_net_name(ctx->module, net));
}

/* --- rules 1 and 5: nodes and their pins ---------------------------------------------------- */

/* What pin `offset` of a node must be: its port, bit and direction. */
typedef struct pin_expect {
    odin3_node_id node;
    uint32_t port;
    uint32_t bit;
    odin3_dir dir;
} pin_expect;

/* Rule 1 for one pin; false (reported) when it is not what its node's type puts there. */
static bool pin_layout(check_ctx *ctx, odin3_pin_id pin, const pin_expect *exp) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(ctx->module, pin);
    if (rec == NULL) {
        violation(ctx, R1, "node %u (%s): pin %u does not exist", exp->node.v,
                  node_label(ctx, exp->node), pin.v);
        return false;
    }
    if (rec->node.v != exp->node.v || rec->port != exp->port || rec->bit != exp->bit ||
        rec->dir != (uint8_t)exp->dir) {
        violation(ctx, R1,
                  "node %u (%s): pin %u is node %u port %u bit %u dir %u; its type puts port %u "
                  "bit %u dir %u there",
                  exp->node.v, node_label(ctx, exp->node), pin.v, rec->node.v, rec->port, rec->bit,
                  rec->dir, exp->port, exp->bit, (unsigned)exp->dir);
        return false;
    }
    return true;
}

/* Walks the pins against the type's ports (rule 1, first mismatch only); sums widths (rule 5). */
static void node_pins(check_ctx *ctx, odin3_node_id id, const odin3_node_rec *rec,
                      const odin3_celltype_def *def) {
    uint64_t total = 0;
    bool matched = true;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_port_query query = {rec->type, rec->params, port};
        uint32_t width = 0;
        if (odin3_celltype_port_width_checked(ctx->design, &query, &width) != ODIN3_OK) {
            violation(ctx, R5, "node %u (%s): port %s of '%s' has no width for its parameters",
                      id.v, node_label(ctx, id), def->ports[port].name, def->name);
            return;
        }
        for (uint32_t bit = 0; matched && bit < width && total + bit < rec->pin_count; bit++) {
            odin3_pin_id pin = {rec->first_pin.v + (uint32_t)(total + bit)};
            pin_expect exp = {id, port, bit, def->ports[port].dir};
            matched = pin_layout(ctx, pin, &exp);
        }
        total += width;
    }
    if (total != rec->pin_count) {
        violation(ctx, R5, "node %u (%s): %u pins, but its type '%s' gives %llu", id.v,
                  node_label(ctx, id), rec->pin_count, def->name, (unsigned long long)total);
    }
}

static void check_node(check_ctx *ctx, odin3_node_id id, const odin3_node_rec *rec) {
    const odin3_celltype_def *def = odin3_celltype_get(ctx->design, rec->type);
    if (def == NULL) {
        violation(ctx, R1, "node %u (%s): %u is not a cell type", id.v, node_label(ctx, id),
                  rec->type.v);
        return;
    }
    uint64_t pin_end = odin3_module_pin_end(ctx->module);
    if (rec->first_pin.v == 0 || (uint64_t)rec->first_pin.v + rec->pin_count > pin_end) {
        violation(ctx, R1, "node %u (%s): pins %u + %u are outside the pin store", id.v,
                  node_label(ctx, id), rec->first_pin.v, rec->pin_count);
        return;
    }
    if (rec->n_params != def->n_params) {
        violation(ctx, R5, "node %u (%s): %u parameter values, its type '%s' has %u", id.v,
                  node_label(ctx, id), rec->n_params, def->name, def->n_params);
        return;
    }
    if (def->verify != NULL && def->verify(rec->params) != ODIN3_OK) {
        violation(ctx, R5, "node %u (%s): '%s' rejects its parameters", id.v, node_label(ctx, id),
                  def->name);
    }
    node_pins(ctx, id, rec, def);
}

static void check_nodes(check_ctx *ctx) {
    uint32_t end = odin3_module_node_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_node_rec *rec = odin3_node_rec_cat(ctx->module, (odin3_node_id){i});
        if (!rec->dead) {
            check_node(ctx, (odin3_node_id){i}, rec);
        }
    }
}

/* --- rules 1, 2 and 11: the pin store ------------------------------------------------------- */

static void pin_dangling(check_ctx *ctx, odin3_pin_id id, const odin3_pin_rec *pin,
                         const odin3_node_rec *node) {
    const odin3_celltype_def *def = odin3_celltype_get(ctx->design, node->type);
    if (def == NULL) {
        return; /* rule 1, reported by the node sweep */
    }
    const char *port = pin->port < def->n_ports ? def->ports[pin->port].name : "?";
    if (def == &odin3_cell_port_out) {
        violation(ctx, R11_E, "pin %u of output port node %u is unconnected", id.v, pin->node.v);
    } else if (def->gran != ODIN3_GRAN_PORT) {
        violation(ctx, R11_W, "pin %u (node %u (%s) '%s', port %s bit %u) is unconnected", id.v,
                  pin->node.v, node_label(ctx, pin->node), def->name, port, pin->bit);
    }
}

static void pin_on_net(check_ctx *ctx, odin3_pin_id id, const odin3_pin_rec *pin) {
    const odin3_net_rec *net = odin3_net_rec_cat(ctx->module, pin->net);
    if (net == NULL || net->dead) {
        violation(ctx, R2, "pin %u is on net %u, which is not a live net", id.v, pin->net.v);
        return;
    }
    if (pin->slot >= net->count || net->pins[pin->slot].v != id.v) {
        violation(ctx, R2, "pin %u is on net %u (%s) but not in its pin array", id.v, pin->net.v,
                  net_label(ctx, pin->net));
    }
}

static void check_pin(check_ctx *ctx, odin3_pin_id id, const odin3_pin_rec *pin) {
    const odin3_node_rec *node = odin3_node_rec_cat(ctx->module, pin->node);
    if (node == NULL) {
        violation(ctx, R1, "pin %u belongs to node %u, which does not exist", id.v, pin->node.v);
        return;
    }
    if (id.v < node->first_pin.v || id.v - node->first_pin.v >= node->pin_count) {
        violation(ctx, R1, "pin %u is not among the pins of its node %u (%s)", id.v, pin->node.v,
                  node_label(ctx, pin->node));
        return;
    }
    if (node->dead) {
        if (odin3_net_valid(pin->net)) {
            violation(ctx, R11_E, "pin %u of dead node %u is still on net %u", id.v, pin->node.v,
                      pin->net.v);
        }
        return;
    }
    if (odin3_net_valid(pin->net)) {
        pin_on_net(ctx, id, pin);
    } else {
        pin_dangling(ctx, id, pin, node);
    }
}

static void check_pins(check_ctx *ctx) {
    uint32_t end = odin3_module_pin_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        check_pin(ctx, (odin3_pin_id){i}, odin3_pin_rec_cat(ctx->module, (odin3_pin_id){i}));
    }
}

/* --- rules 2, 3 and 4: the nets ------------------------------------------------------------- */

/* Rule 2 (net side) and rule 3 per entry; false when the array cannot be trusted for rule 4. */
static bool net_entries(check_ctx *ctx, odin3_net_id id, const odin3_net_rec *rec) {
    bool ok = true;
    for (uint32_t i = 0; i < rec->count; i++) {
        odin3_pin_id pin = rec->pins[i];
        const odin3_pin_rec *prec = odin3_pin_rec_cat(ctx->module, pin);
        if (prec == NULL || !odin3_node_live(ctx->module, prec->node)) {
            violation(ctx, R2, "net %u (%s) lists pin %u, which is not a live pin", id.v,
                      net_label(ctx, id), pin.v);
            ok = false;
        } else if (prec->net.v != id.v || prec->slot != i) {
            violation(ctx, R2, "net %u (%s) lists pin %u at %u; the pin says net %u slot %u", id.v,
                      net_label(ctx, id), pin.v, i, prec->net.v, prec->slot);
            ok = false;
        } else if ((prec->dir != ODIN3_DIR_IN) != (i < rec->driver_count)) {
            violation(ctx, R3, "net %u (%s): pin %u (dir %u) is in the wrong partition at %u of %u",
                      id.v, net_label(ctx, id), pin.v, prec->dir, i, rec->driver_count);
            ok = false;
        }
    }
    return ok;
}

/* An inout pin or an output of a TRISTATE type: may share a net with other such drivers. */
static bool bus_driver(const check_ctx *ctx, odin3_pin_id pin) {
    const odin3_pin_rec *prec = odin3_pin_rec_cat(ctx->module, pin);
    if (prec->dir == ODIN3_DIR_INOUT) {
        return true;
    }
    const odin3_node_rec *node = odin3_node_rec_cat(ctx->module, prec->node);
    const odin3_celltype_def *def = odin3_celltype_get(ctx->design, node->type);
    return def != NULL && (def->flags & ODIN3_CT_TRISTATE) != 0;
}

static void net_drivers(check_ctx *ctx, odin3_net_id id, const odin3_net_rec *rec) {
    if (rec->count == 0) {
        violation(ctx, R4_W, "net %u (%s) has no pins", id.v, net_label(ctx, id));
        return;
    }
    if (rec->driver_count == 0) {
        violation(ctx, R4_W, "net %u (%s) has sinks and no driver", id.v, net_label(ctx, id));
        return;
    }
    uint32_t bus = 0;
    for (uint32_t i = 0; i < rec->driver_count; i++) {
        bus += bus_driver(ctx, rec->pins[i]) ? 1U : 0U;
    }
    uint32_t ordinary = rec->driver_count - bus;
    if (ordinary >= 2) {
        violation(ctx, R4_E, "net %u (%s) has %u drivers that are neither inout nor tristate", id.v,
                  net_label(ctx, id), ordinary);
    } else if (ordinary == 1 && bus > 0) {
        violation(ctx, R4_W, "net %u (%s) is a bus of %u inout/tristate drivers plus one other",
                  id.v, net_label(ctx, id), bus);
    }
}

static void check_net(check_ctx *ctx, odin3_net_id id, const odin3_net_rec *rec) {
    if (rec->dead) {
        if (rec->count > 0) {
            violation(ctx, R2, "dead net %u still lists %u pins", id.v, rec->count);
        }
        return;
    }
    if (rec->driver_count > rec->count) {
        violation(ctx, R3, "net %u (%s): driver count %u exceeds its %u pins", id.v,
                  net_label(ctx, id), rec->driver_count, rec->count);
        return;
    }
    if (net_entries(ctx, id, rec)) {
        net_drivers(ctx, id, rec);
    }
}

static void check_nets(check_ctx *ctx) {
    uint32_t end = odin3_module_net_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        check_net(ctx, (odin3_net_id){i}, odin3_net_rec_cat(ctx->module, (odin3_net_id){i}));
    }
}

/* --- rules 6, 7 and 10: names, provenance, view --------------------------------------------- */

/* One name of an object against its kind's map: a live owner must be mapped, a dead one not. */
typedef struct named {
    const odin3_u64map *map;
    const char *kind;
    uint32_t id;
    uint32_t name;
    bool live;
} named;

static void check_name(check_ctx *ctx, const named *obj) {
    uint64_t owner = 0;
    bool found = odin3_u64map_get(obj->map, obj->name, &owner);
    if (obj->live && (!found || owner != obj->id)) {
        violation(ctx, R6, "%s %u: its name '%s' maps to %s %u", obj->kind, obj->id,
                  label(ctx, obj->name), obj->kind, found ? (unsigned)owner : 0U);
    } else if (!obj->live && found && owner == obj->id) {
        violation(ctx, R6, "dead %s %u is still in the name map as '%s'", obj->kind, obj->id,
                  label(ctx, obj->name));
    }
}

static void check_map_size(check_ctx *ctx, const odin3_u64map *map, const char *kind,
                           uint32_t owned) {
    size_t entries = odin3_u64map_count(map);
    if (entries != owned) {
        violation(ctx, R6, "the %s name map has %zu entries; live %ss own %u names", kind, entries,
                  kind, owned);
    }
}

static void check_prov_id(check_ctx *ctx, const char *kind, uint32_t id, odin3_prov_id prov) {
    if (prov.v == 0) {
        ctx->no_prov++;
    } else if (prov.v >= odin3_prov_end(ctx->design)) {
        violation(ctx, R7, "%s %u has prov %u, which is not a record", kind, id, prov.v);
    }
}

static const char *gran_name(odin3_granularity gran) {
    static const char *const names[] = {"word", "bit", "hard", "blackbox", "module", "port"};
    return (unsigned)gran < sizeof names / sizeof names[0] ? names[gran] : "unknown";
}

/* IR-9: RTLIL allows word, NETLIST allows bit; both allow the rest and ANYVIEW types. */
static bool view_allows(odin3_view view, const odin3_celltype_def *def) {
    if ((def->flags & ODIN3_CT_ANYVIEW) != 0) {
        return true;
    }
    switch (def->gran) {
    case ODIN3_GRAN_WORD:
        return view == ODIN3_VIEW_RTLIL;
    case ODIN3_GRAN_BIT:
        return view == ODIN3_VIEW_NETLIST;
    case ODIN3_GRAN_HARD:
    case ODIN3_GRAN_BLACKBOX:
    case ODIN3_GRAN_MODULE:
    case ODIN3_GRAN_PORT:
        return true;
    }
    return false;
}

static void node_full(check_ctx *ctx, odin3_node_id id, const odin3_node_rec *rec) {
    if (rec->name != 0) {
        named obj = {ctx->module->node_names, "node", id.v, rec->name, !rec->dead};
        check_name(ctx, &obj);
    }
    if (rec->dead) {
        return;
    }
    check_prov_id(ctx, "node", id.v, rec->prov);
    const odin3_celltype_def *def = odin3_celltype_get(ctx->design, rec->type);
    if (ctx->view != ODIN3_VIEW_NONE && def != NULL && !view_allows(ctx->view, def)) {
        violation(ctx, R10, "node %u (%s): '%s' is %s-level, not allowed in the %s view", id.v,
                  node_label(ctx, id), def->name, gran_name(def->gran),
                  ctx->view == ODIN3_VIEW_RTLIL ? "RTLIL" : "netlist");
    }
}

static void check_nodes_full(check_ctx *ctx) {
    uint32_t owned = 0;
    uint32_t end = odin3_module_node_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_node_rec *rec = odin3_node_rec_cat(ctx->module, (odin3_node_id){i});
        node_full(ctx, (odin3_node_id){i}, rec);
        owned += !rec->dead && rec->name != 0 ? 1U : 0U;
    }
    check_map_size(ctx, ctx->module->node_names, "node", owned);
}

/* Checks a live net's own name and alias names; returns how many names it owns. */
static uint32_t net_names(check_ctx *ctx, odin3_net_id id, const odin3_net_rec *rec) {
    uint32_t owned = 0;
    named obj = {ctx->module->net_names, "net", id.v, rec->name, !rec->dead};
    if (rec->name != 0) {
        check_name(ctx, &obj);
        owned += obj.live ? 1U : 0U;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (obj.live && odin3_net_alias_next(ctx->module, id, &cursor, &alias)) {
        if (!odin3_wire_valid(alias.wb.wire) && alias.name != 0) {
            obj.name = alias.name; /* an alias name counts as the net's (IR-14) */
            check_name(ctx, &obj);
            owned++;
        }
    }
    return owned;
}

static void check_nets_full(check_ctx *ctx) {
    uint32_t owned = 0;
    uint32_t end = odin3_module_net_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_net_rec *rec = odin3_net_rec_cat(ctx->module, (odin3_net_id){i});
        owned += net_names(ctx, (odin3_net_id){i}, rec);
        if (!rec->dead) {
            check_prov_id(ctx, "net", i, rec->prov);
        }
    }
    check_map_size(ctx, ctx->module->net_names, "net", owned);
}

static void check_wires_full(check_ctx *ctx) {
    uint32_t owned = 0;
    uint32_t end = odin3_module_wire_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_wire_rec *rec = odin3_wire_rec_cat(ctx->module, (odin3_wire_id){i});
        if (rec->name != 0) {
            named obj = {ctx->module->wire_names, "wire", i, rec->name, !rec->dead};
            check_name(ctx, &obj);
            owned += obj.live ? 1U : 0U;
        }
        if (!rec->dead) {
            check_prov_id(ctx, "wire", i, rec->prov);
        }
    }
    check_map_size(ctx, ctx->module->wire_names, "wire", owned);
}

/* Rule 7 on the design's records: runs exist; DERIVED records have earlier parents. */
static void check_records(check_ctx *ctx) {
    uint32_t end = odin3_prov_end(ctx->design);
    uint32_t runs = odin3_passrun_end(ctx->design);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_prov_record *rec = odin3_prov_get(ctx->design, (odin3_prov_id){i});
        if (rec->run.v == 0 || rec->run.v >= runs) {
            violation(ctx, R7, "record %u names run %u, which does not exist", i, rec->run.v);
        }
        if (rec->kind == ODIN3_PROV_DERIVED && rec->parents.count == 0) {
            violation(ctx, R7, "derived record %u has no parent", i);
        }
        for (uint32_t k = 0; k < rec->parents.count; k++) {
            uint32_t parent = rec->parents.ids[k].v;
            if (parent == 0 || parent >= i) {
                violation(ctx, R7, "record %u has parent %u, not an earlier record", i, parent);
            }
        }
    }
}

/* --- rule 8: wires, primaries and aliases ---------------------------------------------------- */

/* A (wire, bit) membership a net records: the wire must hold the net there, once. */
static void membership(check_ctx *ctx, const check_marks *marks, odin3_net_id net,
                       odin3_wirebit wb) {
    const odin3_wire_rec *wire = odin3_wire_rec_cat(ctx->module, wb.wire);
    if (wire == NULL || wire->dead || wb.bit >= wire->width) {
        violation(ctx, R8, "net %u (%s) records wire %u bit %u, which does not exist", net.v,
                  net_label(ctx, net), wb.wire.v, wb.bit);
        return;
    }
    if (wire->nets[wb.bit].v != net.v) {
        violation(ctx, R8, "net %u (%s) records wire %u (%s) bit %u, which holds net %u", net.v,
                  net_label(ctx, net), wb.wire.v, label(ctx, wire->name), wb.bit,
                  wire->nets[wb.bit].v);
        return;
    }
    uint8_t *mark = &marks->bits[marks->bit_base[wb.wire.v] + wb.bit];
    if (*mark != 0) {
        violation(ctx, R8, "net %u (%s) records wire %u bit %u twice", net.v, net_label(ctx, net),
                  wb.wire.v, wb.bit);
    }
    *mark = 1;
}

static void net_memberships(check_ctx *ctx, const check_marks *marks, odin3_net_id id) {
    odin3_wirebit primary = odin3_net_primary(ctx->module, id);
    if (odin3_wire_valid(primary.wire)) {
        membership(ctx, marks, id, primary);
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(ctx->module, id, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire)) {
            membership(ctx, marks, id, alias.wb);
        }
    }
}

static void wire_bits(check_ctx *ctx, const check_marks *marks, odin3_wire_id id,
                      const odin3_wire_rec *rec) {
    for (uint32_t k = 0; k < rec->width; k++) {
        odin3_net_id net = rec->nets[k];
        if (!odin3_net_live(ctx->module, net)) {
            violation(ctx, R8, "wire %u (%s) bit %u holds net %u, which is not a live net", id.v,
                      label(ctx, rec->name), k, net.v);
        } else if (marks->bits[marks->bit_base[id.v] + k] == 0) {
            violation(ctx, R8, "wire %u (%s) bit %u holds net %u (%s), which does not record it",
                      id.v, label(ctx, rec->name), k, net.v, net_label(ctx, net));
        }
    }
}

static void check_wires(check_ctx *ctx, const check_marks *marks) {
    uint32_t net_end = odin3_module_net_end(ctx->module);
    for (uint32_t i = 1; i < net_end; i++) {
        net_memberships(ctx, marks, (odin3_net_id){i});
    }
    uint32_t wire_end = odin3_module_wire_end(ctx->module);
    for (uint32_t i = 1; i < wire_end; i++) {
        const odin3_wire_rec *rec = odin3_wire_rec_cat(ctx->module, (odin3_wire_id){i});
        if (!rec->dead) {
            wire_bits(ctx, marks, (odin3_wire_id){i}, rec);
        }
    }
}

/* --- rule 9: ports ---------------------------------------------------------------------------- */

static const odin3_celltype_def *port_type_for(odin3_dir dir) {
    switch (dir) {
    case ODIN3_DIR_IN:
        return &odin3_cell_port_in;
    case ODIN3_DIR_OUT:
        return &odin3_cell_port_out;
    case ODIN3_DIR_INOUT:
        return &odin3_cell_port_inout;
    }
    return NULL;
}

/* Port `index` against the module's cell type: node type and width, wire name and width. */
static void port_matches(check_ctx *ctx, uint32_t index, const odin3_port_rec *port,
                         const odin3_celltype_def *node_def) {
    const odin3_port_def *pdef = &ctx->module->type_def.ports[index];
    const odin3_node_rec *node = odin3_node_rec_cat(ctx->module, port->node);
    if (node_def != port_type_for(pdef->dir) || node->pin_count != pdef->width) {
        violation(ctx, R9,
                  "port %u (%s): node %u is a %u-bit '%s'; the cell type says dir %u, "
                  "width %u",
                  index, pdef->name, port->node.v, node->pin_count, node_def->name,
                  (unsigned)pdef->dir, pdef->width);
    }
    const odin3_wire_rec *wire = odin3_wire_rec_cat(ctx->module, port->wire);
    if (wire == NULL || wire->dead || wire->port_node.v != port->node.v ||
        wire->width != pdef->width || strcmp(label(ctx, wire->name), pdef->name) != 0) {
        violation(ctx, R9, "port %u (%s): wire %u is not its live port wire of width %u", index,
                  pdef->name, port->wire.v, pdef->width);
    }
}

static void check_port(check_ctx *ctx, const check_marks *marks, uint32_t index) {
    const odin3_port_rec *port = odin3_vec_cat(&ctx->module->ports, index);
    const odin3_node_rec *node = odin3_node_rec_cat(ctx->module, port->node);
    const odin3_celltype_def *def =
        node != NULL ? odin3_celltype_get(ctx->design, node->type) : NULL;
    if (node == NULL || node->dead || def == NULL || def->gran != ODIN3_GRAN_PORT) {
        violation(ctx, R9, "port %u: node %u is not a live port node", index, port->node.v);
        return;
    }
    if (marks->port_node[port->node.v] != 0) {
        violation(ctx, R9, "port %u: node %u is already in the port list", index, port->node.v);
    }
    marks->port_node[port->node.v] = 1;
    if (index < ctx->module->type_def.n_ports) {
        port_matches(ctx, index, port, def);
    }
}

/* Every live port node is listed, and every port wire names a listed node. */
static void ports_unlisted(check_ctx *ctx, const check_marks *marks) {
    uint32_t end = odin3_module_node_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_node_rec *rec = odin3_node_rec_cat(ctx->module, (odin3_node_id){i});
        const odin3_celltype_def *def = odin3_celltype_get(ctx->design, rec->type);
        if (!rec->dead && def != NULL && def->gran == ODIN3_GRAN_PORT && marks->port_node[i] == 0) {
            violation(ctx, R9, "port node %u ('%s') is not in the port list", i, def->name);
        }
    }
    uint32_t wire_end = odin3_module_wire_end(ctx->module);
    for (uint32_t i = 1; i < wire_end; i++) {
        odin3_node_id node = odin3_wire_port_node(ctx->module, (odin3_wire_id){i});
        if (odin3_node_valid(node) && (node.v >= end || marks->port_node[node.v] == 0)) {
            violation(ctx, R9, "wire %u names node %u as its port, which is not a listed port", i,
                      node.v);
        }
    }
}

static void check_ports(check_ctx *ctx, const check_marks *marks) {
    const odin3_module *module = ctx->module;
    if (odin3_celltype_get(ctx->design, module->type) != &module->type_def) {
        violation(ctx, R9, "the module's cell type %u is not its own port definition",
                  module->type.v);
        return;
    }
    if (module->ports.len != module->type_def.n_ports) {
        violation(ctx, R9, "the port list has %zu ports; the module's cell type has %u",
                  module->ports.len, module->type_def.n_ports);
    }
    for (uint32_t i = 0; i < module->ports.len; i++) {
        check_port(ctx, marks, i);
    }
    ports_unlisted(ctx, marks);
}

/* --- driver -------------------------------------------------------------------------------- */

static void marks_free(check_marks *marks) {
    odin3_util_free(marks->bit_base);
    odin3_util_free(marks->bits);
    odin3_util_free(marks->port_node);
    *marks = (check_marks){0};
}

static odin3_status marks_init(const odin3_module *module, check_marks *marks) {
    uint32_t wire_end = odin3_module_wire_end(module);
    marks->bit_base = odin3_util_calloc(sizeof *marks->bit_base * wire_end);
    marks->port_node = odin3_util_calloc(odin3_module_node_end(module));
    if (marks->bit_base == NULL || marks->port_node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t bits = 0;
    for (uint32_t i = 1; i < wire_end; i++) {
        const odin3_wire_rec *rec = odin3_wire_rec_cat(module, (odin3_wire_id){i});
        marks->bit_base[i] = bits;
        bits += rec->dead ? 0 : rec->width;
    }
    marks->bits = odin3_util_calloc(bits > 0 ? bits : 1);
    return marks->bits != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* How to check one module: the options, and whether the design's records are included. */
typedef struct check_job {
    odin3_check_opts opts;
    bool records;
} check_job;

static void run_full(check_ctx *ctx, const check_marks *marks, const check_job *job) {
    check_nodes_full(ctx);
    check_nets_full(ctx);
    check_wires_full(ctx);
    check_prov_id(ctx, "module", ctx->module->id.v, ctx->module->prov);
    check_wires(ctx, marks);
    check_ports(ctx, marks);
    if (job->records) {
        check_records(ctx);
    }
}

static odin3_status check_one(odin3_module *module, const check_job *job) {
    check_ctx ctx = {0};
    ctx.module = module;
    ctx.design = module->design;
    ctx.where = label(&ctx, module->name);
    ctx.view = job->opts.view;
    check_marks marks = {0};
    bool full = job->opts.level == ODIN3_CHECK_FULL;
    if (full && marks_init(module, &marks) != ODIN3_OK) {
        marks_free(&marks);
        odin3_log(ODIN3_LOG_ERROR, "check: %s: out of memory", ctx.where);
        return ODIN3_ERR_NO_MEMORY;
    }
    check_nodes(&ctx);
    check_pins(&ctx);
    check_nets(&ctx);
    if (full) {
        run_full(&ctx, &marks, job);
    }
    summarize(&ctx);
    marks_free(&marks);
    return ctx.failed ? ODIN3_ERR_CHECK : ODIN3_OK;
}

static bool opts_valid(odin3_check_opts opts) {
    if ((unsigned)opts.level > ODIN3_CHECK_FULL || (unsigned)opts.view > ODIN3_VIEW_NETLIST) {
        odin3_log(ODIN3_LOG_ERROR, "check: invalid options (level %d, view %d)", (int)opts.level,
                  (int)opts.view);
        return false;
    }
    return true;
}

odin3_status odin3_check_module(odin3_module *module, odin3_check_opts opts) {
    if (module == NULL || !opts_valid(opts)) {
        odin3_log(ODIN3_LOG_ERROR, "check_module: no module or invalid options");
        return ODIN3_ERR_INVALID_ARG;
    }
    check_job job = {opts, true};
    return check_one(module, &job);
}

/* The worse of two results: NO_MEMORY, then CHECK, then OK. */
static odin3_status worse(odin3_status lhs, odin3_status rhs) {
    if (lhs == ODIN3_ERR_NO_MEMORY || rhs == ODIN3_ERR_NO_MEMORY) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return lhs != ODIN3_OK ? lhs : rhs;
}

odin3_status odin3_check_design(odin3_design *design, odin3_check_opts opts) {
    if (design == NULL || !opts_valid(opts)) {
        odin3_log(ODIN3_LOG_ERROR, "check_design: no design or invalid options");
        return ODIN3_ERR_INVALID_ARG;
    }
    check_job job = {opts, false};
    odin3_status result = ODIN3_OK;
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t i = 1; i < end; i++) {
        result = worse(result, check_one(odin3_module_get(design, (odin3_module_id){i}), &job));
    }
    if (opts.level == ODIN3_CHECK_FULL) {
        check_ctx ctx = {0};
        ctx.design = design;
        ctx.where = "design";
        check_records(&ctx);
        summarize(&ctx);
        result = worse(result, ctx.failed ? ODIN3_ERR_CHECK : ODIN3_OK);
    }
    return result;
}
