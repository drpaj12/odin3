/*
 * submod.c — moving one attribute group of a module into a new module (Phase 7 spec 3.1, S-1 to
 * S-5). IR nodes cannot change module, so each group node is created in the new module on the
 * mapped nets and deleted from the old one. Boundary nets (S-2) become scalar ports; nets whose
 * pins all lie in the group move with it.
 */
#include "passes/submod/submod_int.h"

#include "ast/srcman.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/builtin.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The class of the submod operation (IR-12a). */
static const odin3_opclass submod_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                           false, 0};

enum { SM_INT_TEXT = 32 };

/* The state of one extraction. Per-ID vectors are indexed by the old module's IDs. */
typedef struct sm_run {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *src;
    odin3_module *dst;
    const odin3_submod_group *group;
    uint32_t key_str;
    odin3_prov_id prov;
    odin3_vec nodes;     /* uint32_t: group node IDs, ascending */
    odin3_vec inside;    /* uint8_t per node ID: 1 for a group node */
    odin3_vec state;     /* uint8_t per net ID: SM_* */
    odin3_vec map;       /* uint32_t per net ID: the net in the new module */
    odin3_vec ports;     /* odin3_submod_port, nets unset until the end */
    odin3_vec port_nets; /* odin3_net_id: the old nets of every port bit, in port order */
    odin3_node_id instance;
    odin3_vec vals;            /* odin3_value scratch */
    odin3_vec nets;            /* odin3_net_id scratch */
    odin3_vec netvecs;         /* odin3_netvec scratch */
    odin3_submod_busset buses; /* -bus: the NAME[BIT] table, empty without it */
    bool check_only;           /* the check phase: no warnings */
} sm_run;

static void run_init(sm_run *run) {
    odin3_vec_init(&run->nodes, sizeof(uint32_t));
    odin3_vec_init(&run->inside, sizeof(uint8_t));
    odin3_vec_init(&run->state, sizeof(uint8_t));
    odin3_vec_init(&run->map, sizeof(uint32_t));
    odin3_vec_init(&run->ports, sizeof(odin3_submod_port));
    odin3_vec_init(&run->port_nets, sizeof(odin3_net_id));
    run->instance = (odin3_node_id){0};
    odin3_vec_init(&run->vals, sizeof(odin3_value));
    odin3_vec_init(&run->nets, sizeof(odin3_net_id));
    odin3_vec_init(&run->netvecs, sizeof(odin3_netvec));
    odin3_submod_busset_init(&run->buses);
    run->check_only = false;
}

static void run_free(sm_run *run) {
    odin3_vec_free(&run->nodes);
    odin3_vec_free(&run->inside);
    odin3_vec_free(&run->state);
    odin3_vec_free(&run->map);
    odin3_vec_free(&run->ports);
    odin3_vec_free(&run->port_nets);
    odin3_vec_free(&run->vals);
    odin3_vec_free(&run->nets);
    odin3_vec_free(&run->netvecs);
    odin3_submod_busset_free(&run->buses);
}

/* Appends count zero bytes or words so that index count - 1 is valid. */
static odin3_status fill_zero(odin3_vec *vec, size_t count) {
    for (size_t i = 0; i < count; i++) {
        void *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memset(slot, 0, vec->elem_size);
    }
    return ODIN3_OK;
}

/* --- errors -------------------------------------------------------------------------------- */

static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *found = user;
    if (found->v == 0) {
        *found = leaf;
    }
}

/* Logs an error located at the source of prov when it has one; always ERR_INVALID_ARG. */
odin3_status odin3_submod_error(odin3_design *design, odin3_prov_id prov, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_prov_id leaf = {0};
    if (prov.v != 0) {
        (void)odin3_prov_sources(design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(design, ODIN3_LOG_ERROR, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%" PRIu32 ":%" PRIu32 ": error: %s",
                  odin3_strtab_get(odin3_design_strtab(design), rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg);
    }
    return ODIN3_ERR_INVALID_ARG;
}

/* The run's design for odin3_submod_error. */
#define sm_error(run, prov, ...) odin3_submod_error((run)->design, (prov), __VA_ARGS__)

static const char *module_text(const sm_run *run) {
    return odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->src));
}

/* The name of net for a message: its own, else `$n<ID>` (in buf). */
static const char *net_label(const sm_run *run, odin3_net_id net, char *buf, size_t size) {
    uint32_t name = odin3_net_name(run->src, net);
    if (name != 0) {
        return odin3_strtab_get(odin3_design_strtab(run->design), name);
    }
    (void)snprintf(buf, size, "$n%" PRIu32, net.v);
    return buf;
}

/* --- the group (S-1) ------------------------------------------------------------------------ */

/* True when node carries the key with the group's value (a STRING of that text or an INT of that
 * decimal text). */
static bool node_in_group(const sm_run *run, odin3_node_id node) {
    const odin3_value *val =
        odin3_attr_get(run->src, (odin3_objref){ODIN3_OBJ_NODE, node.v}, run->key_str);
    if (val == NULL) {
        return false;
    }
    if (val->kind == ODIN3_VAL_STRING) {
        return strcmp(odin3_strtab_get(odin3_design_strtab(run->design), val->str),
                      run->group->value) == 0;
    }
    char text[SM_INT_TEXT];
    if (val->kind != ODIN3_VAL_INT) {
        return false;
    }
    (void)snprintf(text, sizeof text, "%" PRId64, val->i);
    return strcmp(text, run->group->value) == 0;
}

static odin3_status collect_group(sm_run *run) {
    odin3_status st = fill_zero(&run->inside, odin3_module_node_end(run->src));
    uint32_t key = 0;
    bool known = odin3_strtab_find(odin3_design_strtab(run->design),
                                   odin3_bytes_cstr(run->group->key), &key);
    run->key_str = key;
    for (uint32_t nid = 1; known && st == ODIN3_OK && nid < odin3_module_node_end(run->src);
         nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(run->src, node) || !node_in_group(run, node)) {
            continue;
        }
        uint32_t *slot = odin3_vec_push(&run->nodes);
        st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (slot != NULL) {
            *slot = nid;
            *(uint8_t *)odin3_vec_at(&run->inside, nid) = 1;
        }
    }
    return st;
}

/* The refusals that need no net analysis (S-5): empty group, port node, module name clash. */
static odin3_status check_group(const sm_run *run) {
    if (run->nodes.len == 0) {
        return sm_error(run, (odin3_prov_id){0},
                        "submod: group '%s' of module '%s' is empty (no live node has "
                        "attribute '%s' = '%s')",
                        run->group->value, module_text(run), run->group->key, run->group->value);
    }
    const uint32_t *ids = run->nodes.data;
    for (size_t i = 0; i < run->nodes.len; i++) {
        odin3_node_id node = {ids[i]};
        odin3_celltype_id type = odin3_node_type(run->src, node);
        if (odin3_celltype_get(run->design, type)->gran == ODIN3_GRAN_PORT) {
            return sm_error(run, odin3_node_prov(run->src, node),
                            "submod: group '%s' holds a port node of module '%s'; ports cannot "
                            "move",
                            run->group->value, module_text(run));
        }
    }
    uint32_t name = 0;
    odin3_celltype_id clash;
    if (odin3_design_intern(run->design, odin3_bytes_cstr(run->group->name), &name) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (odin3_celltype_find(run->design, name, &clash)) {
        return sm_error(run, odin3_node_prov(run->src, (odin3_node_id){ids[0]}),
                        "submod: module name '%s' is already in use (a module or cell type)",
                        run->group->name);
    }
    return ODIN3_OK;
}

/* --- boundary nets (S-2) -------------------------------------------------------------------- */

/* What a net's pins say: those inside and outside the group, drivers on each side, inout. */
typedef struct sm_pins {
    uint32_t inside, outside, in_drivers, out_drivers;
    bool inout;
} sm_pins;

static sm_pins count_pins(const sm_run *run, odin3_net_id net) {
    sm_pins sum = {0, 0, 0, 0, false};
    odin3_pinlist list = odin3_net_pins(run->src, net);
    const uint8_t *inside = run->inside.data;
    for (uint32_t i = 0; i < list.count; i++) {
        odin3_pin_id pin = list.pins[i];
        bool in_group = inside[odin3_pin_node(run->src, pin).v] != 0;
        bool drives = odin3_pin_drives(run->src, pin);
        sum.inside += in_group ? 1U : 0U;
        sum.outside += in_group ? 0U : 1U;
        sum.in_drivers += in_group && drives ? 1U : 0U;
        sum.out_drivers += !in_group && drives ? 1U : 0U;
        sum.inout = sum.inout || odin3_pin_dir(run->src, pin) == ODIN3_DIR_INOUT;
    }
    return sum;
}

static odin3_status classify_net(sm_run *run, odin3_net_id net) {
    sm_pins sum = count_pins(run, net);
    uint8_t *state = odin3_vec_at(&run->state, net.v);
    if (sum.outside == 0) {
        *state = SM_INTERNAL;
        return ODIN3_OK;
    }
    char buf[SM_INT_TEXT];
    odin3_pin_id first = odin3_net_pins(run->src, net).pins[0];
    odin3_prov_id prov = odin3_pin_prov(run->src, first);
    if (sum.inout) {
        return sm_error(run, prov,
                        "submod: net '%s' of module '%s' crosses the boundary of group '%s' "
                        "through an inout pin; the port direction is not defined",
                        net_label(run, net, buf, sizeof buf), module_text(run), run->group->value);
    }
    if (sum.in_drivers + sum.out_drivers > 1) {
        return sm_error(run, prov,
                        "submod: net '%s' of module '%s' crosses the boundary of group '%s' "
                        "and has %" PRIu32 " drivers; the port direction is not defined",
                        net_label(run, net, buf, sizeof buf), module_text(run), run->group->value,
                        sum.in_drivers + sum.out_drivers);
    }
    *state = sum.in_drivers == 1 ? SM_OUT : SM_IN;
    return ODIN3_OK;
}

/* Classifies every net a group pin touches, once each; no recursion, one pass over the pins. */
static odin3_status classify_nets(sm_run *run) {
    odin3_status st = fill_zero(&run->state, odin3_module_net_end(run->src));
    st = st == ODIN3_OK ? fill_zero(&run->map, odin3_module_net_end(run->src)) : st;
    const uint32_t *ids = run->nodes.data;
    for (size_t i = 0; st == ODIN3_OK && i < run->nodes.len; i++) {
        odin3_pinslice pins = odin3_node_pins(run->src, (odin3_node_id){ids[i]});
        for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
            odin3_net_id net = odin3_pin_net(run->src, (odin3_pin_id){pins.first.v + k});
            if (odin3_net_valid(net) && *(uint8_t *)odin3_vec_at(&run->state, net.v) == SM_NONE) {
                st = classify_net(run, net);
            }
        }
    }
    return st;
}

/* --- the new module ------------------------------------------------------------------------ */

/* The first provenance of a group node, or a record without source made in this run. */
static odin3_status first_parent(sm_run *run, odin3_prov_id *out) {
    const uint32_t *ids = run->nodes.data;
    for (size_t i = 0; i < run->nodes.len; i++) {
        *out = odin3_node_prov(run->src, (odin3_node_id){ids[i]});
        if (out->v != 0) {
            return ODIN3_OK;
        }
    }
    return odin3_prov_imported(run->pass, &(odin3_prov_origin){NULL, 0, 0, 0}, out);
}

/* The DERIVED record of this operation: the new module and the instance always get one (S-4). */
static odin3_status make_prov(sm_run *run) {
    odin3_prov_id first = {0};
    run->prov = (odin3_prov_id){0};
    odin3_prov_begin_op_class(run->pass, &submod_class);
    odin3_status st = first_parent(run, &first);
    return st == ODIN3_OK ? odin3_prov_derive(run->pass, (odin3_prov_list){&first, 1}, &run->prov)
                          : st;
}

static bool real_name(const char *text) {
    return text[0] != '\0' && text[0] != '$';
}

/* Interns text as a port wire name of the new module, adding `_<n>` until no wire has it. */
static odin3_status unique_wire_name(sm_run *run, odin3_strbuf *text, uint32_t *out) {
    size_t base = text->len;
    odin3_status st = ODIN3_OK;
    for (uint32_t tries = 1; st == ODIN3_OK; tries++) {
        st = odin3_design_intern(run->design, odin3_bytes_cstr(text->data), out);
        if (st != ODIN3_OK || !odin3_wire_valid(odin3_module_find_wire(run->dst, *out))) {
            break;
        }
        text->len = base;
        text->data[base] = '\0';
        st = odin3_strbuf_appendf(text, "_%" PRIu32, tries);
    }
    return st;
}

/* The text of the port for net `net` as port number `port_no`: a real net name, else the name of
 * its primary wire (`w` or `w[index]`), else `p<port_no>`; made unique among the new module's
 * wires. */
static odin3_status port_name(sm_run *run, odin3_net_id net, uint32_t port_no, uint32_t *out) {
    const odin3_strtab *tab = odin3_design_strtab(run->design);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_wirebit wb = odin3_net_primary(run->src, net);
    const char *own = odin3_net_name(run->src, net) != 0
                          ? odin3_strtab_get(tab, odin3_net_name(run->src, net))
                          : "";
    const char *wire = odin3_wire_valid(wb.wire) && odin3_wire_name(run->src, wb.wire) != 0
                           ? odin3_strtab_get(tab, odin3_wire_name(run->src, wb.wire))
                           : "";
    odin3_status st = ODIN3_OK;
    if (real_name(own)) {
        st = odin3_strbuf_appendf(&text, "%s", own);
    } else if (real_name(wire) && odin3_wire_width(run->src, wb.wire) == 1) {
        st = odin3_strbuf_appendf(&text, "%s", wire);
    } else if (real_name(wire)) {
        st = odin3_strbuf_appendf(&text, "%s[%" PRId32 "]", wire,
                                  odin3_wire_index(run->src, wb.wire, wb.bit));
    } else {
        st = odin3_strbuf_appendf(&text, "p%" PRIu32, port_no);
    }
    st = st == ODIN3_OK ? unique_wire_name(run, &text, out) : st;
    odin3_strbuf_free(&text);
    return st;
}

/* Records the port just added (spec) and the old nets it carries, LSB first (spec->width). */
static odin3_status record_port(sm_run *run, const odin3_port_spec *spec,
                                const odin3_net_id *nets) {
    odin3_submod_port *port = odin3_vec_push(&run->ports);
    if (port == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *port = (odin3_submod_port){spec->name, spec->dir, spec->width, NULL};
    for (uint32_t bit = 0; bit < spec->width; bit++) {
        odin3_net_id *slot = odin3_vec_push(&run->port_nets);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = nets[bit];
    }
    return ODIN3_OK;
}

/* The key and value of an attribute copy: copies every attribute of one object but the group key.
 */
typedef struct sm_attr_job {
    const sm_run *run;
    odin3_module *to;
    odin3_objref dst;
} sm_attr_job;

static odin3_status copy_attr(void *user, uint32_t key, const odin3_value *value) {
    const sm_attr_job *job = user;
    if (key == job->run->key_str) {
        return ODIN3_OK;
    }
    return odin3_attr_set(job->to, job->dst, key, value);
}

/* One object of the old module and its copy in the new one. */
typedef struct sm_objpair {
    odin3_objref from, to;
} sm_objpair;

static odin3_status copy_attrs(const sm_run *run, sm_objpair pair) {
    sm_attr_job job = {run, run->dst, pair.to};
    return odin3_attr_foreach(run->src, pair.from, copy_attr, &job);
}

/* Maps boundary net `old` to the inner net of pin `bit` of the port node just added. */
static void map_port_bit(sm_run *run, odin3_node_id port, uint32_t bit, odin3_net_id old) {
    odin3_pin_id pin = {odin3_node_pins(run->dst, port).first.v + bit};
    *(uint32_t *)odin3_vec_at(&run->map, old.v) = odin3_pin_net(run->dst, pin).v;
}

/* The wire of the last port added gets the attributes of the old wire `from` (when it has one). */
static odin3_status copy_port_wire_attrs(const sm_run *run, odin3_wire_id from) {
    if (!odin3_wire_valid(from)) {
        return ODIN3_OK;
    }
    odin3_wire_id to = odin3_module_port_wire(run->dst, odin3_module_port_count(run->dst) - 1);
    return copy_attrs(run, (sm_objpair){{ODIN3_OBJ_WIRE, from.v}, {ODIN3_OBJ_WIRE, to.v}});
}

/* Adds the scalar port for boundary net `net` (number port_no) and maps the net to its inner net.
 */
static odin3_status add_port(sm_run *run, odin3_net_id net, uint32_t port_no) {
    odin3_port_spec spec = {0, ODIN3_DIR_IN, 1, true, run->prov, false, 0, 0, false};
    spec.dir = *(uint8_t *)odin3_vec_at(&run->state, net.v) == SM_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT;
    odin3_status st = port_name(run, net, port_no, &spec.name);
    odin3_node_id port = {0};
    st = st == ODIN3_OK ? odin3_module_add_port(run->dst, &spec, &port) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    map_port_bit(run, port, 0, net);
    st = record_port(run, &spec, &net);
    return st == ODIN3_OK ? copy_port_wire_attrs(run, odin3_net_primary(run->src, net).wire) : st;
}

/* The bus (index + 1) of an ok `-bus` set that net belongs to, else 0 (spec B-5). */
static uint32_t bus_of(const sm_run *run, odin3_net_id net) {
    const uint32_t *of_net = run->buses.of_net.data;
    return net.v < run->buses.of_net.len ? of_net[net.v] : 0U;
}

/* True when `wire` has two or more bits, a real name, and every bit is a boundary net of one
 * direction that has the bit as its primary: it can be one bus port. */
static bool full_bus(sm_run *run, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(run->src, wire);
    uint32_t name = odin3_wire_name(run->src, wire);
    if (width < 2 || name == 0 ||
        !real_name(odin3_strtab_get(odin3_design_strtab(run->design), name))) {
        return false;
    }
    uint8_t dir = 0;
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id net = odin3_wire_net(run->src, wire, bit);
        odin3_wirebit prim =
            odin3_net_valid(net) ? odin3_net_primary(run->src, net) : (odin3_wirebit){{0}, 0};
        uint8_t state =
            odin3_net_valid(net) ? *(uint8_t *)odin3_vec_at(&run->state, net.v) : (uint8_t)SM_NONE;
        if ((state != SM_IN && state != SM_OUT) || (bit > 0 && state != dir) ||
            prim.wire.v != wire.v || prim.bit != bit || bus_of(run, net) != 0) {
            return false;
        }
        dir = state;
    }
    return true;
}

/* Adds one bus port for `wire` (see full_bus), named like it, and maps every bit's net. */
static odin3_status add_bus_port(sm_run *run, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(run->src, wire);
    odin3_net_id first = odin3_wire_net(run->src, wire, 0);
    odin3_port_spec spec = {0,
                            *(uint8_t *)odin3_vec_at(&run->state, first.v) == SM_IN ? ODIN3_DIR_IN
                                                                                    : ODIN3_DIR_OUT,
                            width,
                            false,
                            run->prov,
                            true,
                            odin3_wire_msb(run->src, wire),
                            odin3_wire_lsb(run->src, wire),
                            odin3_wire_signed(run->src, wire)};
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_strbuf_appendf(
        &text, "%s",
        odin3_strtab_get(odin3_design_strtab(run->design), odin3_wire_name(run->src, wire)));
    st = st == ODIN3_OK ? unique_wire_name(run, &text, &spec.name) : st;
    odin3_strbuf_free(&text);
    odin3_node_id port = {0};
    st = st == ODIN3_OK ? odin3_module_add_port(run->dst, &spec, &port) : st;
    odin3_vec_clear(&run->nets);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        odin3_net_id *slot = odin3_vec_push(&run->nets);
        st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (slot != NULL) {
            *slot = odin3_wire_net(run->src, wire, bit);
            map_port_bit(run, port, bit, *slot);
        }
    }
    st = st == ODIN3_OK ? record_port(run, &spec, run->nets.data) : st;
    return st == ODIN3_OK ? copy_port_wire_attrs(run, wire) : st;
}

/* Adds the vector port of bus `index` of the `-bus` table (spec B-1, B-2): width W, range [W-1:0],
 * named NAME (made unique), bit i on the net tagged NAME[i]; no wire attributes (B-6). */
static odin3_status add_attr_bus_port(sm_run *run, uint32_t index) {
    const odin3_submod_bus *bus = (const odin3_submod_bus *)run->buses.buses.data + index;
    const odin3_net_id *nets = (const odin3_net_id *)run->buses.nets.data + bus->first;
    odin3_port_spec spec = {0,
                            bus->dir == SM_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT,
                            bus->width,
                            false,
                            run->prov,
                            true,
                            (int32_t)bus->width - 1,
                            0,
                            false};
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_strbuf_appendf(
        &text, "%s", odin3_strtab_get(odin3_design_strtab(run->design), bus->name));
    st = st == ODIN3_OK ? unique_wire_name(run, &text, &spec.name) : st;
    odin3_strbuf_free(&text);
    odin3_node_id port = {0};
    st = st == ODIN3_OK ? odin3_module_add_port(run->dst, &spec, &port) : st;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < bus->width; bit++) {
        map_port_bit(run, port, bit, nets[bit]);
    }
    return st == ODIN3_OK ? record_port(run, &spec, nets) : st;
}

/* The port for boundary net `net`: its `-bus` vector when it is in one, a bus when its primary
 * wire is one in full, else a scalar. */
static odin3_status add_boundary_port(sm_run *run, odin3_net_id net, uint32_t port_no) {
    if (bus_of(run, net) != 0) {
        return add_attr_bus_port(run, bus_of(run, net) - 1U);
    }
    odin3_wire_id wire = odin3_net_primary(run->src, net).wire;
    if (odin3_wire_valid(wire) && full_bus(run, wire)) {
        return add_bus_port(run, wire);
    }
    return add_port(run, net, port_no);
}

/* An internal net of the group in the new module; a name a port wire has is replaced by
 * `$submod$<ID>` so that no net shadows a port. */
static odin3_status add_internal_net(sm_run *run, odin3_net_id net) {
    uint32_t name = odin3_net_name(run->src, net);
    odin3_status st = ODIN3_OK;
    if (name != 0 && odin3_wire_valid(odin3_module_find_wire(run->dst, name))) {
        char text[SM_INT_TEXT];
        (void)snprintf(text, sizeof text, "$submod$%" PRIu32, net.v);
        st = odin3_design_intern(run->design, odin3_bytes_cstr(text), &name);
    }
    odin3_net_id fresh = {0};
    st = st == ODIN3_OK ? odin3_net_create(run->dst, name, odin3_net_prov(run->src, net), &fresh)
                        : st;
    if (st != ODIN3_OK) {
        return st;
    }
    *(uint32_t *)odin3_vec_at(&run->map, net.v) = fresh.v;
    return copy_attrs(run, (sm_objpair){{ODIN3_OBJ_NET, net.v}, {ODIN3_OBJ_NET, fresh.v}});
}

/* New module, its ports (boundary nets in ID order, bus wires whole) and the nets that move with
 * the group. */
static odin3_status build_module(sm_run *run, odin3_module_id *id) {
    uint32_t name = 0;
    odin3_status st = make_prov(run);
    st = st == ODIN3_OK
             ? odin3_design_intern(run->design, odin3_bytes_cstr(run->group->name), &name)
             : st;
    st = st == ODIN3_OK ? odin3_module_create(run->design, name, run->prov, id) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    run->dst = odin3_module_get(run->design, *id);
    for (uint32_t nid = 1; st == ODIN3_OK && nid < odin3_module_net_end(run->src); nid++) {
        uint8_t state = *(uint8_t *)odin3_vec_at(&run->state, nid);
        bool mapped = *(uint32_t *)odin3_vec_at(&run->map, nid) != 0;
        if ((state == SM_IN || state == SM_OUT) && !mapped) {
            st = add_boundary_port(run, (odin3_net_id){nid}, (uint32_t)run->ports.len);
        }
    }
    for (uint32_t nid = 1; st == ODIN3_OK && nid < odin3_module_net_end(run->src); nid++) {
        if (*(uint8_t *)odin3_vec_at(&run->state, nid) == SM_INTERNAL) {
            st = add_internal_net(run, (odin3_net_id){nid});
        }
    }
    return st;
}

/* --- moving the nodes ----------------------------------------------------------------------- */

/* The mapped nets of every port of node in run->nets and, as slices, run->netvecs. */
static odin3_status gather_ports(sm_run *run, odin3_node_id node, const odin3_celltype_def *def) {
    odin3_vec_clear(&run->nets);
    odin3_vec_clear(&run->netvecs);
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice slice = odin3_node_port(run->src, node, port);
        for (uint32_t bit = 0; bit < slice.count; bit++) {
            odin3_net_id *net = odin3_vec_push(&run->nets);
            if (net == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            odin3_net_id old = odin3_pin_net(run->src, (odin3_pin_id){slice.first.v + bit});
            *net = (odin3_net_id){0};
            if (odin3_net_valid(old)) {
                net->v = *(uint32_t *)odin3_vec_at(&run->map, old.v);
            }
        }
    }
    const odin3_net_id *nets = run->nets.data;
    uint32_t at = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_netvec *vec = odin3_vec_push(&run->netvecs);
        if (vec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *vec = (odin3_netvec){nets + at, odin3_node_port(run->src, node, port).count};
        at += vec->count;
    }
    return ODIN3_OK;
}

static odin3_status move_node(sm_run *run, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(run->src, node);
    const odin3_celltype_def *def = odin3_celltype_get(run->design, type);
    odin3_vec_clear(&run->vals);
    for (uint32_t i = 0; i < def->n_params; i++) {
        odin3_value *val = odin3_vec_push(&run->vals);
        if (val == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *val = *odin3_node_param(run->src, node, i);
    }
    odin3_node_spec spec = {type, odin3_node_name(run->src, node), odin3_node_prov(run->src, node),
                            run->vals.data, def->n_params};
    odin3_node_id copy = {0};
    odin3_status st = gather_ports(run, node, def);
    st = st == ODIN3_OK ? odin3_node_create_connected(run->dst, &spec, run->netvecs.data, &copy)
                        : st;
    st = st == ODIN3_OK
             ? copy_attrs(run, (sm_objpair){{ODIN3_OBJ_NODE, node.v}, {ODIN3_OBJ_NODE, copy.v}})
             : st;
    return st == ODIN3_OK ? odin3_node_delete(run->src, node) : st;
}

/* Deletes the old copies of the nets that moved, when nothing else (a wire, an alias) holds them.
 */
static void drop_moved_nets(sm_run *run) {
    for (uint32_t nid = 1; nid < odin3_module_net_end(run->src); nid++) {
        odin3_net_id net = {nid};
        if (*(uint8_t *)odin3_vec_at(&run->state, nid) != SM_INTERNAL ||
            odin3_net_pins(run->src, net).count != 0 ||
            odin3_wire_valid(odin3_net_primary(run->src, net).wire) ||
            odin3_net_alias_count(run->src, net) != 0) {
            continue;
        }
        (void)odin3_net_delete(run->src, net);
    }
}

/* The instance of the new module, on the boundary nets (S-3), named `<name>_i` (made unique). */
static odin3_status make_instance(sm_run *run, odin3_celltype_id type) {
    odin3_vec_clear(&run->netvecs);
    const odin3_submod_port *ports = run->ports.data;
    const odin3_net_id *nets = run->port_nets.data;
    for (size_t i = 0, at = 0; i < run->ports.len; at += ports[i].width, i++) {
        odin3_netvec *vec = odin3_vec_push(&run->netvecs);
        if (vec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *vec = (odin3_netvec){nets + at, ports[i].width};
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_node_spec spec = {type, 0, run->prov, NULL, 0};
    odin3_status st = odin3_strbuf_appendf(&text, "%s_i", run->group->name);
    size_t base = text.len;
    for (uint32_t tries = 1; st == ODIN3_OK; tries++) {
        st = odin3_design_intern(run->design, odin3_bytes_cstr(text.data), &spec.name);
        if (st != ODIN3_OK || !odin3_node_valid(odin3_module_find_node(run->src, spec.name))) {
            break;
        }
        text.len = base;
        text.data[base] = '\0';
        st = odin3_strbuf_appendf(&text, "_%" PRIu32, tries);
    }
    st = st == ODIN3_OK
             ? odin3_node_create_connected(run->src, &spec, run->netvecs.data, &run->instance)
             : st;
    odin3_strbuf_free(&text);
    return st;
}

static odin3_status move_all(sm_run *run) {
    const uint32_t *ids = run->nodes.data;
    for (size_t i = 0; i < run->nodes.len; i++) {
        odin3_status st = move_node(run, (odin3_node_id){ids[i]});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    drop_moved_nets(run);
    return make_instance(run, odin3_module_celltype(run->dst));
}

/* Everything that can refuse a group (S-1, S-2, S-5), without changing the IR. */
static odin3_status analyze(sm_run *run) {
    odin3_status st = run->src != NULL ? collect_group(run) : ODIN3_ERR_INVALID_ARG;
    st = st == ODIN3_OK ? check_group(run) : st;
    st = st == ODIN3_OK ? classify_nets(run) : st;
    run->buses.quiet = run->check_only;
    if (st == ODIN3_OK && run->group->bus_key != NULL) {
        st = odin3_submod_busset_build(run->src, &run->state, run->group->bus_key,
                                       run->group->value, &run->buses);
    }
    return st;
}

odin3_status odin3_submod_check_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                      const odin3_submod_group *group) {
    sm_run run;
    run_init(&run);
    run.pass = ctx;
    run.design = ctx->design;
    run.src = odin3_module_get(ctx->design, module_id);
    run.group = group;
    run.check_only = true;
    odin3_status st = analyze(&run);
    run_free(&run);
    return st;
}

/* Hands the ports and their nets to out (the run's vectors move; the run frees empty ones). */
static void give_result(sm_run *run, odin3_module_id id, odin3_submod_result *out) {
    odin3_submod_result_free(out);
    out->module = id;
    out->instance = run->instance;
    out->port_store = run->ports;
    out->net_store = run->port_nets;
    odin3_vec_init(&run->ports, sizeof(odin3_submod_port));
    odin3_vec_init(&run->port_nets, sizeof(odin3_net_id));
    odin3_submod_port *ports = out->port_store.data;
    const odin3_net_id *nets = out->net_store.data;
    for (size_t i = 0, at = 0; i < out->port_store.len; at += ports[i].width, i++) {
        ports[i].nets = nets + at;
    }
    out->ports = ports;
    out->n_ports = (uint32_t)out->port_store.len;
}

void odin3_submod_result_free(odin3_submod_result *result) {
    if (result->port_store.elem_size == 0) {
        odin3_vec_init(&result->port_store, sizeof(odin3_submod_port));
        odin3_vec_init(&result->net_store, sizeof(odin3_net_id));
    }
    odin3_vec_free(&result->port_store);
    odin3_vec_free(&result->net_store);
    result->ports = NULL;
    result->n_ports = 0;
}

odin3_status odin3_submod_extract_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                        const odin3_submod_group *group, odin3_submod_result *out) {
    sm_run run;
    run_init(&run);
    run.pass = ctx;
    run.design = ctx->design;
    run.src = odin3_module_get(ctx->design, module_id);
    run.group = group;
    odin3_module_id id = {0};
    odin3_status st = analyze(&run);
    st = st == ODIN3_OK ? build_module(&run, &id) : st;
    st = st == ODIN3_OK ? move_all(&run) : st;
    if (st == ODIN3_OK && out != NULL) {
        give_result(&run, id, out);
    }
    run_free(&run);
    return st;
}

odin3_status odin3_submod_extract_ctx(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                      const char *key, const char *value,
                                      odin3_submod_result *out) {
    odin3_submod_group group = {key, value, value, NULL};
    return odin3_submod_extract_named(ctx, module_id, &group, out);
}

odin3_status odin3_submod_extract(odin3_design *design, odin3_module_id module_id, const char *key,
                                  const char *value, odin3_submod_result *out) {
    uint32_t name = 0;
    odin3_pass_ctx ctx;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr("submod"), &name);
    st = st == ODIN3_OK ? odin3_pass_run_begin(design, name, &ctx) : st;
    return st == ODIN3_OK ? odin3_submod_extract_ctx(&ctx, module_id, key, value, out) : st;
}
