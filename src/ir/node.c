/* node.c — node create (plain and connected), replace, delete, rename; node/pin accessors. */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/u64map.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* A node about to be created: its validated type and its parameters, copied into the module. */
typedef struct node_plan {
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    odin3_value *params; /* def->n_params values in the module arena (NULL when none) */
    uint32_t pin_count;
} node_plan;

static bool params_match(const odin3_celltype_def *def, const odin3_node_spec *spec) {
    if (spec->params == NULL) {
        return true;
    }
    if (spec->n_params != def->n_params) {
        odin3_log(ODIN3_LOG_ERROR, "node_create: '%s' takes %u parameters, got %u", def->name,
                  def->n_params, spec->n_params);
        return false;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (spec->params[i].kind != def->params[i].kind) {
            odin3_log(ODIN3_LOG_ERROR, "node_create: '%s' parameter %s has the wrong kind",
                      def->name, def->params[i].name);
            return false;
        }
    }
    return true;
}

/* Copies the spec's parameter values (or the defaults) into the module arena. */
static odin3_status copy_params(odin3_module *module, const odin3_node_spec *spec,
                                node_plan *plan) {
    const odin3_celltype_def *def = plan->def;
    plan->params = NULL;
    if (def->n_params == 0) {
        return ODIN3_OK;
    }
    odin3_value *vals = odin3_arena_alloc(module->arena, sizeof *vals * def->n_params);
    if (vals == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_value *src = spec->params != NULL ? &spec->params[i] : &def->params[i].dflt;
        odin3_status st = odin3_value_copy(module->arena, src, &vals[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    plan->params = vals;
    return ODIN3_OK;
}

static odin3_status count_pins(const odin3_module *module, node_plan *plan) {
    uint64_t total = 0;
    for (uint32_t port = 0; port < plan->def->n_ports; port++) {
        odin3_port_query query = {plan->type, plan->params, port};
        uint32_t width = 0;
        odin3_status st = odin3_celltype_port_width_checked(module->design, &query, &width);
        if (st != ODIN3_OK) {
            return st; /* logged */
        }
        total += width;
    }
    if (total > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY; /* more pins than IDs */
    }
    plan->pin_count = (uint32_t)total;
    return ODIN3_OK;
}

/* Validates the spec and prepares the node; nothing observable changes. */
static odin3_status plan_node(odin3_module *module, const odin3_node_spec *spec, node_plan *plan) {
    if (spec == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "node_create: no spec");
        return ODIN3_ERR_INVALID_ARG;
    }
    plan->type = spec->type;
    plan->def = odin3_celltype_get(module->design, spec->type);
    if (plan->def == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "node_create: %u is not a cell type", spec->type.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_name_change name = {module->node_names, "node_create", 0, 0, spec->name};
    if (!params_match(plan->def, spec) || !odin3_names_available(module, &name)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = copy_params(module, spec, plan);
    if (st != ODIN3_OK) {
        return st;
    }
    if (plan->def->verify != NULL && plan->def->verify(plan->params) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "node_create: '%s' rejects these parameters", plan->def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    return count_pins(module, plan);
}

/* Takes the reserved pin slots: port order, then bit order. */
static void make_pins(odin3_module *module, const node_plan *plan, odin3_node_id node) {
    for (uint32_t port = 0; port < plan->def->n_ports; port++) {
        uint32_t width = odin3_celltype_port_width(module->design, plan->type, plan->params, port);
        for (uint32_t bit = 0; bit < width; bit++) {
            odin3_pin_rec *pin = odin3_pagevec_push(module->pins, NULL);
            assert(pin != NULL); /* reserved by node_create */
            pin->node = node;
            pin->port = port;
            pin->bit = bit;
            pin->dir = (uint8_t)plan->def->ports[port].dir;
        }
    }
}

/* Reserves the node and its pins, then takes the name: afterwards node_commit cannot fail. */
static odin3_status node_reserve(odin3_module *module, const odin3_node_spec *spec,
                                 const node_plan *plan) {
    odin3_status st = odin3_module_reserve(module->nodes, 1);
    if (st == ODIN3_OK) {
        st = odin3_module_reserve(module->pins, plan->pin_count);
    }
    if (st == ODIN3_OK) {
        odin3_name_change name = {module->node_names, "node_create", odin3_module_node_end(module),
                                  0, spec->name};
        st = odin3_names_change(module, &name); /* last fallible step */
    }
    return st;
}

static odin3_node_id node_commit(odin3_module *module, const odin3_node_spec *spec,
                                 const node_plan *plan) {
    odin3_node_id id = {odin3_module_node_end(module)};
    odin3_node_rec *rec = odin3_pagevec_push(module->nodes, NULL);
    assert(rec != NULL); /* reserved by node_reserve */
    rec->type = plan->type;
    rec->name = spec->name;
    rec->prov = spec->prov;
    rec->first_pin.v = odin3_module_pin_end(module);
    rec->pin_count = plan->pin_count;
    rec->n_params = plan->def->n_params;
    rec->params = plan->params;
    make_pins(module, plan, id);
    odin3_celltype_instances_inc(module->design, plan->type);
    return id;
}

/* Undoes node_commit of the newest node, whose pins are unconnected. */
static void node_unmake(odin3_module *module, odin3_node_id node) {
    odin3_node_rec *rec = odin3_node_rec_at(module, node);
    assert(node.v + 1 == odin3_module_node_end(module));
    assert(rec->first_pin.v + rec->pin_count == odin3_module_pin_end(module));
    if (rec->name != 0) {
        (void)odin3_u64map_remove(module->node_names, rec->name);
    }
    odin3_celltype_instances_dec(module->design, rec->type);
    odin3_pagevec_truncate(module->pins, rec->first_pin.v);
    odin3_pagevec_truncate(module->nodes, node.v);
}

odin3_status odin3_node_create(odin3_module *module, const odin3_node_spec *spec,
                               odin3_node_id *out) {
    node_plan plan = {0};
    odin3_status st = plan_node(module, spec, &plan);
    if (st == ODIN3_OK) {
        st = node_reserve(module, spec, &plan);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id id = node_commit(module, spec, &plan);
    if (out != NULL) {
        *out = id;
    }
    return ODIN3_OK;
}

/* --- create-connected ---------------------------------------------------------------------- */

static bool netvec_ok(const odin3_module *module, const odin3_netvec *vec, uint32_t width,
                      const char *port) {
    if (vec->count != width || (width > 0 && vec->nets == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "node_create_connected: port %s has width %u, got %u nets", port,
                  width, vec->nets != NULL ? vec->count : 0);
        return false;
    }
    for (uint32_t k = 0; k < width; k++) {
        if (odin3_net_valid(vec->nets[k]) && !odin3_net_live(module, vec->nets[k])) {
            odin3_log(ODIN3_LOG_ERROR, "node_create_connected: net %u is not a live net",
                      vec->nets[k].v);
            return false;
        }
    }
    return true;
}

static bool ports_ok(const odin3_module *module, const node_plan *plan, const odin3_netvec *ports) {
    const odin3_celltype_def *def = plan->def;
    if (ports == NULL && def->n_ports > 0) {
        odin3_log(ODIN3_LOG_ERROR, "node_create_connected: no port nets for '%s'", def->name);
        return false;
    }
    for (uint32_t port = 0; port < def->n_ports; port++) {
        uint32_t width = odin3_celltype_port_width(module->design, plan->type, plan->params, port);
        if (!netvec_ok(module, &ports[port], width, def->ports[port].name)) {
            return false;
        }
    }
    return true;
}

/* Connects the new node's pins; on failure disconnects the ones done (never allocates). */
static odin3_status connect_ports(odin3_module *module, odin3_node_id node,
                                  const odin3_netvec *ports) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    uint32_t n_ports = odin3_celltype_get(module->design, rec->type)->n_ports;
    uint32_t first = rec->first_pin.v;
    uint32_t pin = first;
    for (uint32_t port = 0; port < n_ports; port++) {
        for (uint32_t k = 0; k < ports[port].count; k++, pin++) {
            odin3_net_id net = ports[port].nets[k];
            odin3_status st = odin3_net_valid(net)
                                  ? odin3_pin_connect(module, (odin3_pin_id){pin}, net)
                                  : ODIN3_OK;
            if (st != ODIN3_OK) {
                for (uint32_t done = pin; done > first; done--) { /* newest first */
                    (void)odin3_pin_disconnect(module, (odin3_pin_id){done - 1});
                }
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_node_create_connected(odin3_module *module, const odin3_node_spec *spec,
                                         const odin3_netvec *ports, odin3_node_id *out) {
    node_plan plan = {0};
    odin3_status st = plan_node(module, spec, &plan);
    if (st == ODIN3_OK && !ports_ok(module, &plan, ports)) {
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK) {
        st = node_reserve(module, spec, &plan);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id id = node_commit(module, spec, &plan);
    st = connect_ports(module, id, ports);
    if (st != ODIN3_OK) {
        node_unmake(module, id);
        return st;
    }
    if (out != NULL) {
        *out = id;
    }
    return ODIN3_OK;
}

/* --- replace ------------------------------------------------------------------------------- */

/* Same port count and, pin by pin, the same port, bit and direction; new pins unconnected. */
static const char *replace_error(const odin3_module *module, const odin3_node_rec *old_rec,
                                 const odin3_node_rec *new_rec) {
    const odin3_celltype_def *old_def = odin3_celltype_get(module->design, old_rec->type);
    const odin3_celltype_def *new_def = odin3_celltype_get(module->design, new_rec->type);
    if (old_def->gran == ODIN3_GRAN_PORT || new_def->gran == ODIN3_GRAN_PORT) {
        return "port nodes cannot be replaced";
    }
    if (old_def->n_ports != new_def->n_ports || old_rec->pin_count != new_rec->pin_count) {
        return "the port signatures differ";
    }
    for (uint32_t i = 0; i < old_rec->pin_count; i++) {
        const odin3_pin_rec *op =
            odin3_pin_rec_cat(module, (odin3_pin_id){old_rec->first_pin.v + i});
        const odin3_pin_rec *np =
            odin3_pin_rec_cat(module, (odin3_pin_id){new_rec->first_pin.v + i});
        if (op->port != np->port || op->bit != np->bit || op->dir != np->dir) {
            return "the port signatures differ";
        }
        if (odin3_net_valid(np->net)) {
            return "a pin of the new node is connected";
        }
    }
    return NULL;
}

odin3_status odin3_node_replace(odin3_module *module, odin3_node_pair pair) {
    if (pair.old_node.v == pair.new_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "node_replace: node %u cannot replace itself", pair.old_node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_node_rec *old_rec = odin3_node_live_rec(module, pair.old_node, "node_replace");
    const odin3_node_rec *new_rec =
        old_rec != NULL ? odin3_node_live_rec(module, pair.new_node, "node_replace") : NULL;
    if (new_rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *err = replace_error(module, old_rec, new_rec);
    if (err != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "node_replace: %u by %u: %s", pair.old_node.v, pair.new_node.v,
                  err);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < old_rec->pin_count; i++) {
        odin3_pin_rec *from = odin3_pin_rec_at(module, (odin3_pin_id){old_rec->first_pin.v + i});
        if (odin3_net_valid(from->net)) {
            odin3_net_handover(module, from, (odin3_pin_id){new_rec->first_pin.v + i});
        }
    }
    return odin3_node_delete(module, pair.old_node);
}

odin3_status odin3_node_delete(odin3_module *module, odin3_node_id node) {
    odin3_node_rec *rec = odin3_node_live_rec(module, node, "node_delete");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < rec->pin_count; i++) {
        odin3_pin_rec *pin = odin3_pin_rec_at(module, (odin3_pin_id){rec->first_pin.v + i});
        if (odin3_net_valid(pin->net)) {
            odin3_net_detach(module, pin);
        }
    }
    if (rec->name != 0) {
        (void)odin3_u64map_remove(module->node_names, rec->name);
    }
    rec->dead = true;
    odin3_celltype_instances_dec(module->design, rec->type);
    return ODIN3_OK;
}

odin3_status odin3_node_rename(odin3_module *module, odin3_node_id node, uint32_t name_str) {
    odin3_node_rec *rec = odin3_node_live_rec(module, node, "node_rename");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_name_change name = {module->node_names, "node_rename", node.v, rec->name, name_str};
    odin3_status st = odin3_names_change(module, &name);
    if (st == ODIN3_OK) {
        rec->name = name_str;
    }
    return st;
}

/* --- node accessors ------------------------------------------------------------------------ */

bool odin3_node_live(const odin3_module *module, odin3_node_id node) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    return rec != NULL && !rec->dead;
}

odin3_celltype_id odin3_node_type(const odin3_module *module, odin3_node_id node) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    return rec != NULL ? rec->type : (odin3_celltype_id){0};
}

uint32_t odin3_node_name(const odin3_module *module, odin3_node_id node) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    return rec != NULL ? rec->name : 0;
}

odin3_prov_id odin3_node_prov(const odin3_module *module, odin3_node_id node) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    return rec != NULL ? rec->prov : (odin3_prov_id){0};
}

odin3_pinslice odin3_node_pins(const odin3_module *module, odin3_node_id node) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    if (rec == NULL || rec->pin_count == 0) {
        return (odin3_pinslice){{0}, 0};
    }
    return (odin3_pinslice){rec->first_pin, rec->pin_count};
}

/* Offset of the node's first pin whose port is >= port (pins are sorted by port). */
static uint32_t port_lower_bound(const odin3_module *module, const odin3_node_rec *rec,
                                 uint32_t port) {
    uint32_t low = 0;
    uint32_t high = rec->pin_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const odin3_pin_rec *pin =
            odin3_pin_rec_cat(module, (odin3_pin_id){rec->first_pin.v + mid});
        if (pin->port < port) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

odin3_pinslice odin3_node_port(const odin3_module *module, odin3_node_id node, uint32_t port) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    if (rec == NULL || port == UINT32_MAX) {
        return (odin3_pinslice){{0}, 0};
    }
    uint32_t begin = port_lower_bound(module, rec, port);
    uint32_t end = port_lower_bound(module, rec, port + 1);
    if (begin == end) {
        return (odin3_pinslice){{0}, 0};
    }
    return (odin3_pinslice){{rec->first_pin.v + begin}, end - begin};
}

const odin3_value *odin3_node_param(const odin3_module *module, odin3_node_id node,
                                    uint32_t index) {
    const odin3_node_rec *rec = odin3_node_rec_cat(module, node);
    if (rec == NULL || index >= rec->n_params) {
        return NULL;
    }
    return &rec->params[index];
}

/* --- pin accessors ------------------------------------------------------------------------- */

bool odin3_pin_live(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL && odin3_node_live(module, rec->node);
}

odin3_node_id odin3_pin_node(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL ? rec->node : (odin3_node_id){0};
}

uint32_t odin3_pin_port(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL ? rec->port : 0;
}

uint32_t odin3_pin_bit(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL ? rec->bit : 0;
}

odin3_net_id odin3_pin_net(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL ? rec->net : (odin3_net_id){0};
}

odin3_dir odin3_pin_dir(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL ? (odin3_dir)rec->dir : ODIN3_DIR_IN;
}

odin3_prov_id odin3_pin_prov(const odin3_module *module, odin3_pin_id pin) {
    return odin3_node_prov(module, odin3_pin_node(module, pin));
}

bool odin3_pin_drives(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL && rec->dir != ODIN3_DIR_IN;
}

bool odin3_pin_reads(const odin3_module *module, odin3_pin_id pin) {
    const odin3_pin_rec *rec = odin3_pin_rec_cat(module, pin);
    return rec != NULL && rec->dir != ODIN3_DIR_OUT;
}
