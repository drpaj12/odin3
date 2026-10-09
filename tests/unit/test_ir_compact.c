/*
 * test_ir_compact.c — unit tests for odin3_module_compact (IR-6): dense renumbering in ID order,
 * old->new maps, tombstones (with a dead node's parameters and pin net names), names, aliases,
 * attributes, out of memory and linear cost.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    CHAIN = 1000,
    OOM_CHAIN = 8,
    OOM_LIMIT = 100000,
    OOM_MIN_FAILURES = 20, /* allocation points a compact of the OOM scenario must pass through */
    DELETE_OOM_MIN_FAILURES = 2, /* the first delete of a module: record table and name array */
    BIG_SMALL = 50000,
    BIG_LARGE = 200000,
    NAME_BUF = 32,
    LINE_NET = 1000000, /* provenance lines: nodes at i, nets and wires offset */
    LINE_WIRE = 2000000,
    PORT_LINE = 3000000,
    MODULE_LINE = 4000000,
    LINK_PINS = 3,
    LEVEL_PARAM = 0,
};

/* Time and memory of a 4x larger compact may grow by at most this factor over 4x. */
static const double LINEAR_SLACK = 2.5;
static const double TIME_FLOOR_SECONDS = 0.05;

static odin3_design *design;
static odin3_module *module;
static odin3_pass_ctx reader;
static size_t errors_logged;
static size_t warnings_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    } else if (level == ODIN3_LOG_WARN) {
        warnings_logged++;
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static uint32_t numbered_name(const char *prefix, uint32_t index) {
    char buf[NAME_BUF];
    (void)snprintf(buf, sizeof buf, "%s%u", prefix, (unsigned)index);
    return intern(buf);
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(name), &id));
    return id;
}

/* A SOURCE record at line `line` of t7.v in the reader run. */
static odin3_prov_id src_prov(uint32_t line) {
    odin3_srcloc loc = {intern("t7.v"), line, 1, line, 1};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&reader, &origin, &id));
    return id;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), src_prov(MODULE_LINE), &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    TEST_ASSERT_NOT_NULL(mod);
    return mod;
}

/* A new design (the old one destroyed) with the reader run and module `name`. */
static void fresh_design(const char *name) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("reader"), &reader));
    module = new_module(name);
}

void setUp(void) {
    errors_logged = 0;
    warnings_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = NULL;
    fresh_design("top");
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
}

/* A test type: A in, Y out, Z out (one bit each); LEVEL int, INIT bits (payload in the arena). */
static const uint8_t k_init_bits[] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X};
static const odin3_param_def k_link_params[] = {
    {"LEVEL", ODIN3_VAL_INT, {ODIN3_VAL_INT, 0, NULL, 0, 0, 0}},
    {"INIT", ODIN3_VAL_BITS, {ODIN3_VAL_BITS, 0, k_init_bits, 3, 0, 0}},
};
static const odin3_port_def k_link_ports[] = {
    {"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
    {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL},
    {"Z", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL},
};
static const odin3_celltype_def k_link = {
    "test_t7_link", ODIN3_GRAN_WORD, 0, k_link_ports, 3, k_link_params, 2, NULL, NULL};

static void kill_wire(odin3_module *mod, odin3_wire_id wire) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(mod, wire));
}

/*
 * A wire marked dead with its memberships left in place, which odin3_wire_delete never does:
 * compact must still drop such memberships (test_compact_drops_dead_wire_memberships).
 */
static void mark_wire_dead(odin3_module *mod, odin3_wire_id wire) {
    odin3_wire_rec *rec = odin3_wire_rec_at(mod, wire);
    if (rec->name != 0) {
        (void)odin3_u64map_remove(mod->wire_names, rec->name);
    }
    rec->dead = true;
}

/* --- the chain ------------------------------------------------------------------------------ */

/*
 * Port "a" -> chain[0] -> node 1 -> chain[1] -> ... -> node n -> chain[n] -> port "y". Node i
 * (named n<i>, LEVEL i) also drives its own net own[i] (o<i>); chain[i] for 0 < i < n is named
 * c<i> and, with wires, is bit 0 of wire w<i>.
 */
typedef struct chain {
    uint32_t n;
    bool wires;
    bool provs; /* one record per object (else one shared record) */
    odin3_net_id *chain;
    odin3_net_id *own;
    odin3_node_id *node;
    odin3_wire_id *wire;
} chain;

static odin3_prov_id chain_prov(const chain *ch, uint32_t line) {
    return src_prov(ch->provs ? line : 1);
}

static void chain_ports(odin3_module *mod, chain *ch) {
    odin3_port_spec in = {intern("a"), ODIN3_DIR_IN, 1, true, chain_prov(ch, PORT_LINE)};
    odin3_port_spec out = {intern("y"), ODIN3_DIR_OUT, 1, true, chain_prov(ch, PORT_LINE + 1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &in, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &out, NULL));
    ch->chain[0] = odin3_wire_net(mod, odin3_module_port_wire(mod, 0), 0);
    ch->chain[ch->n] = odin3_wire_net(mod, odin3_module_port_wire(mod, 1), 0);
}

static void chain_link(odin3_module *mod, chain *ch, uint32_t index) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, numbered_name("o", index),
                                           chain_prov(ch, LINE_NET + 2 * index), &ch->own[index]));
    if (index < ch->n) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, numbered_name("c", index),
                                                         chain_prov(ch, LINE_NET + 2 * index + 1),
                                                         &ch->chain[index]));
    }
    if (index < ch->n && ch->wires) {
        odin3_wire_spec ws = {numbered_name("w", index), 0, 0, false,
                              chain_prov(ch, LINE_WIRE + index)};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_wire_create(mod, &ws, &ch->chain[index], &ch->wire[index]));
    }
    odin3_value params[2] = {odin3_value_int(index), k_link_params[1].dflt};
    odin3_node_spec spec = {type_id("test_t7_link"), numbered_name("n", index),
                            chain_prov(ch, index), params, 2};
    odin3_netvec ports[LINK_PINS] = {
        {&ch->chain[index - 1], 1}, {&ch->chain[index], 1}, {&ch->own[index], 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_create_connected(mod, &spec, ports, &ch->node[index]));
}

static void chain_build(odin3_module *mod, chain *ch) {
    size_t slots = (size_t)ch->n + 1;
    ch->chain = odin3_util_calloc(sizeof *ch->chain * slots);
    ch->own = odin3_util_calloc(sizeof *ch->own * slots);
    ch->node = odin3_util_calloc(sizeof *ch->node * slots);
    ch->wire = odin3_util_calloc(sizeof *ch->wire * slots);
    TEST_ASSERT_TRUE(ch->chain != NULL && ch->own != NULL && ch->node != NULL && ch->wire != NULL);
    chain_ports(mod, ch);
    for (uint32_t i = 1; i <= ch->n; i++) {
        chain_link(mod, ch, i);
    }
}

static void chain_free(chain *ch) {
    odin3_util_free(ch->chain);
    odin3_util_free(ch->own);
    odin3_util_free(ch->node);
    odin3_util_free(ch->wire);
}

/*
 * Deletes every odd node (n is even, so node n survives): node i+1 is moved from chain[i] onto
 * chain[i-1], then chain[i] (and its wire) and own[i], now without pins, are deleted.
 */
static void chain_delete_odd(odin3_module *mod, const chain *ch) {
    for (uint32_t i = 1; i < ch->n; i += 2) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(mod, ch->node[i]));
        odin3_pin_id a_pin = odin3_node_port(mod, ch->node[i + 1], 0).first;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(mod, a_pin));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(mod, a_pin, ch->chain[i - 1]));
        if (ch->wires) {
            kill_wire(mod, ch->wire[i]);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(mod, ch->chain[i]));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(mod, ch->own[i]));
    }
}

/* A dead node, net and wire created first, so every survivor's ID (ports included) shifts. */
static void dead_prefix(odin3_module *mod) {
    odin3_node_spec spec = {type_id("test_t7_link"), intern("pre"), src_prov(MODULE_LINE + 1), NULL,
                            0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(mod, node));
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, intern("pre"), src_prov(MODULE_LINE + 2), &net));
    odin3_wire_spec ws = {intern("pre"), 0, 0, false, src_prov(MODULE_LINE + 3)};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &ws, &net, &wire));
    kill_wire(mod, wire);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(mod, net));
}

/* --- snapshots ------------------------------------------------------------------------------ */

/* What an object was before compact, by old ID. */
typedef struct obj_snap {
    bool live;
    uint32_t name;
    odin3_prov_id prov;
    odin3_celltype_id type;
} obj_snap;

typedef struct pin_snap {
    bool live;
    odin3_node_id node;
    odin3_net_id net;
    uint32_t port;
    uint32_t bit;
} pin_snap;

typedef struct snapshot {
    obj_snap *nodes;
    obj_snap *nets;
    obj_snap *wires;
    pin_snap *pins;
    uint32_t n_node, n_net, n_wire, n_pin;
    uint32_t dead_nodes, dead_nets, dead_wires;
    odin3_net_id *wire_bit0; /* net at bit 0 of each wire */
    odin3_node_id port_node[2];
    odin3_wire_id port_wire[2];
    uint32_t tombstones; /* tombstone end */
    uint32_t records;    /* provenance record end */
} snapshot;

static void snap_nodes(const odin3_module *mod, snapshot *snap) {
    snap->n_node = odin3_module_node_end(mod);
    snap->nodes = odin3_util_calloc(sizeof *snap->nodes * snap->n_node);
    TEST_ASSERT_NOT_NULL(snap->nodes);
    for (uint32_t i = 1; i < snap->n_node; i++) {
        odin3_node_id id = {i};
        snap->nodes[i] = (obj_snap){odin3_node_live(mod, id), odin3_node_name(mod, id),
                                    odin3_node_prov(mod, id), odin3_node_type(mod, id)};
        snap->dead_nodes += snap->nodes[i].live ? 0U : 1U;
    }
    snap->n_pin = odin3_module_pin_end(mod);
    snap->pins = odin3_util_calloc(sizeof *snap->pins * snap->n_pin);
    TEST_ASSERT_NOT_NULL(snap->pins);
    for (uint32_t i = 1; i < snap->n_pin; i++) {
        odin3_pin_id id = {i};
        snap->pins[i] =
            (pin_snap){odin3_pin_live(mod, id), odin3_pin_node(mod, id), odin3_pin_net(mod, id),
                       odin3_pin_port(mod, id), odin3_pin_bit(mod, id)};
    }
}

static void snap_nets_wires(const odin3_module *mod, snapshot *snap) {
    snap->n_net = odin3_module_net_end(mod);
    snap->nets = odin3_util_calloc(sizeof *snap->nets * snap->n_net);
    TEST_ASSERT_NOT_NULL(snap->nets);
    for (uint32_t i = 1; i < snap->n_net; i++) {
        odin3_net_id id = {i};
        snap->nets[i] = (obj_snap){
            odin3_net_live(mod, id), odin3_net_name(mod, id), odin3_net_prov(mod, id), {0}};
        snap->dead_nets += snap->nets[i].live ? 0U : 1U;
    }
    snap->n_wire = odin3_module_wire_end(mod);
    snap->wires = odin3_util_calloc(sizeof *snap->wires * snap->n_wire);
    snap->wire_bit0 = odin3_util_calloc(sizeof *snap->wire_bit0 * snap->n_wire);
    TEST_ASSERT_TRUE(snap->wires != NULL && snap->wire_bit0 != NULL);
    for (uint32_t i = 1; i < snap->n_wire; i++) {
        odin3_wire_id id = {i};
        snap->wires[i] = (obj_snap){
            odin3_wire_live(mod, id), odin3_wire_name(mod, id), odin3_wire_prov(mod, id), {0}};
        snap->wire_bit0[i] = odin3_wire_net(mod, id, 0);
        snap->dead_wires += snap->wires[i].live ? 0U : 1U;
    }
}

static void snap_take(const odin3_module *mod, snapshot *snap) {
    memset(snap, 0, sizeof *snap);
    snap_nodes(mod, snap);
    snap_nets_wires(mod, snap);
    for (uint32_t k = 0; k < 2 && k < odin3_module_port_count(mod); k++) {
        snap->port_node[k] = odin3_module_port(mod, k);
        snap->port_wire[k] = odin3_module_port_wire(mod, k);
    }
    snap->tombstones = odin3_tombstone_end(design);
    snap->records = odin3_prov_end(design);
}

static void snap_free(snapshot *snap) {
    odin3_util_free(snap->nodes);
    odin3_util_free(snap->nets);
    odin3_util_free(snap->wires);
    odin3_util_free(snap->pins);
    odin3_util_free(snap->wire_bit0);
}

/* --- verification --------------------------------------------------------------------------- */

/* map[i] is 0 for dead old IDs and 1, 2, 3, ... (dense, increasing) for live ones. */
static uint32_t verify_ids(const uint32_t *map, const obj_snap *objs, uint32_t count) {
    uint32_t next = 0;
    TEST_ASSERT_EQUAL_UINT32(0, map[0]);
    for (uint32_t i = 1; i < count; i++) {
        if (objs[i].live) {
            TEST_ASSERT_EQUAL_UINT32(++next, map[i]);
        } else {
            TEST_ASSERT_EQUAL_UINT32(0, map[i]);
        }
    }
    return next;
}

static void verify_nodes(const odin3_module *mod, const snapshot *snap,
                         const odin3_compact_map *map) {
    TEST_ASSERT_EQUAL_UINT32(snap->n_node, map->n_node);
    uint32_t live = verify_ids(map->node, snap->nodes, snap->n_node);
    TEST_ASSERT_EQUAL_UINT32(live + 1, odin3_module_node_end(mod));
    for (uint32_t i = 1; i < snap->n_node; i++) {
        const obj_snap *old = &snap->nodes[i];
        odin3_node_id now = {map->node[i]};
        if (old->live) {
            TEST_ASSERT_TRUE(odin3_node_live(mod, now));
            TEST_ASSERT_EQUAL_UINT32(old->name, odin3_node_name(mod, now));
            TEST_ASSERT_EQUAL_UINT32(old->prov.v, odin3_node_prov(mod, now).v);
            TEST_ASSERT_EQUAL_UINT32(old->type.v, odin3_node_type(mod, now).v);
        }
        if (old->name != 0) { /* a dead node's name is gone; a live one finds the new ID */
            TEST_ASSERT_EQUAL_UINT32(now.v, odin3_module_find_node(mod, old->name).v);
        }
    }
}

static void verify_pins(const odin3_module *mod, const snapshot *snap,
                        const odin3_compact_map *map) {
    TEST_ASSERT_EQUAL_UINT32(snap->n_pin, map->n_pin);
    uint32_t next = 0;
    for (uint32_t i = 1; i < snap->n_pin; i++) {
        const pin_snap *old = &snap->pins[i];
        if (!old->live) {
            TEST_ASSERT_EQUAL_UINT32(0, map->pin[i]);
            continue;
        }
        odin3_pin_id now = {map->pin[i]};
        TEST_ASSERT_EQUAL_UINT32(++next, now.v);
        TEST_ASSERT_EQUAL_UINT32(map->node[old->node.v], odin3_pin_node(mod, now).v);
        TEST_ASSERT_EQUAL_UINT32(map->net[old->net.v], odin3_pin_net(mod, now).v);
        TEST_ASSERT_EQUAL_UINT32(old->port, odin3_pin_port(mod, now));
        TEST_ASSERT_EQUAL_UINT32(old->bit, odin3_pin_bit(mod, now));
    }
    TEST_ASSERT_EQUAL_UINT32(next + 1, odin3_module_pin_end(mod));
}

static void verify_nets_wires(const odin3_module *mod, const snapshot *snap,
                              const odin3_compact_map *map) {
    TEST_ASSERT_EQUAL_UINT32(snap->n_net, map->n_net);
    TEST_ASSERT_EQUAL_UINT32(verify_ids(map->net, snap->nets, snap->n_net) + 1,
                             odin3_module_net_end(mod));
    for (uint32_t i = 1; i < snap->n_net; i++) {
        const obj_snap *old = &snap->nets[i];
        if (old->live) {
            odin3_net_id now = {map->net[i]};
            TEST_ASSERT_EQUAL_UINT32(old->name, odin3_net_name(mod, now));
            TEST_ASSERT_EQUAL_UINT32(old->prov.v, odin3_net_prov(mod, now).v);
        }
        if (old->name != 0) {
            TEST_ASSERT_EQUAL_UINT32(map->net[i], odin3_module_find_net(mod, old->name).v);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(snap->n_wire, map->n_wire);
    TEST_ASSERT_EQUAL_UINT32(verify_ids(map->wire, snap->wires, snap->n_wire) + 1,
                             odin3_module_wire_end(mod));
    for (uint32_t i = 1; i < snap->n_wire; i++) {
        const obj_snap *old = &snap->wires[i];
        odin3_wire_id now = {map->wire[i]};
        if (old->live) {
            TEST_ASSERT_EQUAL_UINT32(old->name, odin3_wire_name(mod, now));
            TEST_ASSERT_EQUAL_UINT32(old->prov.v, odin3_wire_prov(mod, now).v);
            TEST_ASSERT_EQUAL_UINT32(map->net[snap->wire_bit0[i].v], odin3_wire_net(mod, now, 0).v);
        }
        if (old->name != 0) {
            TEST_ASSERT_EQUAL_UINT32(now.v, odin3_module_find_wire(mod, old->name).v);
        }
    }
}

/* The objects of one kind, by old ID. */
typedef struct obj_list {
    odin3_objkind kind;
    const obj_snap *objs;
    uint32_t count;
} obj_list;

/* A net or wire tombstone: parameters and pin names are a node's only. */
static void verify_no_arrays(const odin3_tombstone *ts) {
    TEST_ASSERT_TRUE(ts->params == NULL && ts->n_params == 0);
    TEST_ASSERT_TRUE(ts->pin_nets == NULL && ts->n_pins == 0);
}

/* One tombstone per dead node, then net, then wire, each in old ID order. */
static uint32_t verify_tombs_of(obj_list list, uint32_t tomb) {
    for (uint32_t i = 1; i < list.count; i++) {
        const obj_snap *obj = &list.objs[i];
        if (obj->live) {
            continue;
        }
        const odin3_tombstone *ts = odin3_tombstone_get(design, tomb++);
        TEST_ASSERT_NOT_NULL(ts);
        TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(module).v, ts->module.v);
        TEST_ASSERT_EQUAL_INT(list.kind, ts->kind);
        TEST_ASSERT_EQUAL_UINT32(obj->type.v, ts->type.v);
        TEST_ASSERT_EQUAL_UINT32(obj->name, ts->name);
        TEST_ASSERT_EQUAL_UINT32(obj->prov.v, ts->prov.v);
        if (list.kind != ODIN3_OBJ_NODE) {
            verify_no_arrays(ts);
        }
    }
    return tomb;
}

/* Per chain node index i (odd: deleted), the names of the nets its A, Y, Z pins were on. */
static uint32_t *chain_pin_names(const odin3_module *mod, const chain *ch) {
    uint32_t *names = odin3_util_calloc(sizeof *names * LINK_PINS * ((size_t)ch->n + 1));
    TEST_ASSERT_NOT_NULL(names);
    for (uint32_t i = 1; i < ch->n; i += 2) {
        uint32_t *at = &names[(size_t)LINK_PINS * i];
        at[0] = odin3_net_name(mod, ch->chain[i - 1]);
        at[1] = odin3_net_name(mod, ch->chain[i]);
        at[2] = odin3_net_name(mod, ch->own[i]);
    }
    return names;
}

/*
 * Every dead chain node's tombstone keeps LEVEL i and INIT's default and the names of the nets
 * its pins were on, in pin order; the never-connected "pre" node has three unconnected pins.
 */
static void verify_chain_tombs(const snapshot *snap, const uint32_t *names, uint32_t n) {
    uint32_t seen = 0;
    for (uint32_t id = snap->tombstones; id < odin3_tombstone_end(design); id++) {
        const odin3_tombstone *ts = odin3_tombstone_get(design, id);
        if (ts->kind != ODIN3_OBJ_NODE) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32(LINK_PINS, ts->n_pins);
        if (ts->name == intern("pre")) {
            uint32_t none[LINK_PINS] = {0};
            TEST_ASSERT_EQUAL_UINT32_ARRAY(none, ts->pin_nets, LINK_PINS);
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32(2, ts->n_params);
        uint32_t i = (uint32_t)ts->params[LEVEL_PARAM].i;
        TEST_ASSERT_EQUAL_UINT32(numbered_name("n", i), ts->name);
        TEST_ASSERT_TRUE(odin3_value_equal(&k_link_params[1].dflt, &ts->params[1]));
        TEST_ASSERT_EQUAL_UINT32_ARRAY(&names[(size_t)LINK_PINS * i], ts->pin_nets, LINK_PINS);
        seen++;
    }
    TEST_ASSERT_EQUAL_UINT32(n / 2, seen);
}

static void verify_tombstones(const snapshot *snap) {
    uint32_t tomb = snap->tombstones;
    tomb = verify_tombs_of((obj_list){ODIN3_OBJ_NODE, snap->nodes, snap->n_node}, tomb);
    tomb = verify_tombs_of((obj_list){ODIN3_OBJ_NET, snap->nets, snap->n_net}, tomb);
    tomb = verify_tombs_of((obj_list){ODIN3_OBJ_WIRE, snap->wires, snap->n_wire}, tomb);
    TEST_ASSERT_EQUAL_UINT32(tomb, odin3_tombstone_end(design));
    TEST_ASSERT_EQUAL_UINT32(snap->tombstones + snap->dead_nodes + snap->dead_nets +
                                 snap->dead_wires,
                             odin3_tombstone_end(design));
}

static void verify_check_clean(odin3_module *mod) {
    size_t errors = errors_logged;
    size_t warnings = warnings_logged;
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
    TEST_ASSERT_EQUAL_size_t(errors, errors_logged);
    TEST_ASSERT_EQUAL_size_t(warnings, warnings_logged);
}

/* Check FULL passes with no error (unconnected test pins and pinless nets only warn). */
static void verify_check_no_errors(odin3_module *mod) {
    size_t errors = errors_logged;
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
    TEST_ASSERT_EQUAL_size_t(errors, errors_logged);
}

/* --- Review Focus 4: the 1000-node chain ----------------------------------------------------- */

static void test_compact_chain(void) {
    chain ch = {CHAIN, true, true, NULL, NULL, NULL, NULL};
    dead_prefix(module);
    chain_build(module, &ch);
    chain_delete_odd(module, &ch);
    verify_check_clean(module);
    snapshot snap;
    snap_take(module, &snap);
    TEST_ASSERT_EQUAL_UINT32(CHAIN / 2 + 1, snap.dead_nodes);
    TEST_ASSERT_EQUAL_UINT32(CHAIN + 1, snap.dead_nets);
    TEST_ASSERT_EQUAL_UINT32(CHAIN / 2 + 1, snap.dead_wires);
    TEST_ASSERT_EQUAL_UINT32(2, snap.port_node[0].v); /* moves to 1 */
    uint32_t *names = chain_pin_names(module, &ch);

    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));

    verify_nodes(module, &snap, &map);
    verify_pins(module, &snap, &map);
    verify_nets_wires(module, &snap, &map);
    verify_tombstones(&snap);
    verify_chain_tombs(&snap, names, CHAIN);
    odin3_util_free(names);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_port(module, 0).v);
    for (uint32_t k = 0; k < 2; k++) {
        TEST_ASSERT_EQUAL_UINT32(map.node[snap.port_node[k].v], odin3_module_port(module, k).v);
        TEST_ASSERT_EQUAL_UINT32(map.wire[snap.port_wire[k].v],
                                 odin3_module_port_wire(module, k).v);
    }
    TEST_ASSERT_EQUAL_UINT32(snap.records, odin3_prov_end(design)); /* no record added */
    /* survivors: node 2j keeps LEVEL 2j (parameters copied) and reads chain[2j-2]'s new ID */
    odin3_node_id last = {map.node[ch.node[CHAIN].v]};
    TEST_ASSERT_EQUAL_INT64(CHAIN, odin3_node_param(module, last, LEVEL_PARAM)->i);
    const odin3_value *init = odin3_node_param(module, last, 1);
    TEST_ASSERT_TRUE(odin3_value_equal(&k_link_params[1].dflt, init));
    TEST_ASSERT_EQUAL_UINT32(CHAIN / 2, odin3_celltype_instances(design, type_id("test_t7_link")));
    verify_check_clean(module);
    odin3_compact_map_free(&map);
    TEST_ASSERT_NULL(map.node);
    snap_free(&snap);
    chain_free(&ch);
}

/* --- names, aliases and attributes ----------------------------------------------------------- */

static odin3_net_id make_net(const char *name) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, intern(name), src_prov(1), &id));
    return id;
}

static odin3_wire_id make_wire(const char *name, const odin3_net_id *nets, uint32_t width) {
    odin3_wire_spec spec = {intern(name), (int32_t)width - 1, 0, false, src_prov(2)};
    odin3_wire_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, nets, &id));
    return id;
}

/* --- tombstones of dead nodes: parameters and pin net names (PHASE1 #14) ------------------- */

static const uint8_t k_victim_init[] = {ODIN3_BIT_Z, ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X};

/* A link node named `name` with LEVEL level and a 4-bit INIT, its pins on nets (or unconnected). */
static odin3_node_id make_link(const char *name, int64_t level, const odin3_net_id *nets) {
    odin3_value params[2] = {odin3_value_int(level), {ODIN3_VAL_BITS, 0, k_victim_init, 4, 0, 0}};
    odin3_node_spec spec = {type_id("test_t7_link"), intern(name), src_prov(4), params, 2};
    odin3_node_id id = {0};
    if (nets == NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &id));
        return id;
    }
    odin3_netvec ports[LINK_PINS] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &id));
    return id;
}

/* Three nets for a link's pins: "<prefix>a", an unnamed one, "<prefix>c". */
static void make_link_nets(const char *prefix, odin3_net_id nets[LINK_PINS]) {
    char name[NAME_BUF];
    (void)snprintf(name, sizeof name, "%sa", prefix);
    nets[0] = make_net(name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, src_prov(5), &nets[1]));
    (void)snprintf(name, sizeof name, "%sc", prefix);
    nets[2] = make_net(name);
}

/* Tombstone id is node `name`'s, with LEVEL level, the 4-bit INIT and these pin net names. */
static void verify_link_tomb(uint32_t id, const char *name, int64_t level, const uint32_t *pins) {
    const odin3_tombstone *ts = odin3_tombstone_get(design, id);
    TEST_ASSERT_NOT_NULL(ts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OBJ_NODE, ts->kind);
    TEST_ASSERT_EQUAL_UINT32(intern(name), ts->name);
    odin3_value want[2] = {odin3_value_int(level), {ODIN3_VAL_BITS, 0, k_victim_init, 4, 0, 0}};
    TEST_ASSERT_EQUAL_UINT32(2, ts->n_params);
    TEST_ASSERT_TRUE(odin3_value_equal(&want[0], &ts->params[0]));
    TEST_ASSERT_TRUE(odin3_value_equal(&want[1], &ts->params[1]));
    TEST_ASSERT_EQUAL_UINT32(LINK_PINS, ts->n_pins);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(pins, ts->pin_nets, LINK_PINS);
}

/* The pin-name records of dead nodes are gone after compact. */
static void verify_side_table_dropped(const odin3_module *mod) {
    TEST_ASSERT_TRUE(mod->dead_pins == NULL || odin3_u64map_count(mod->dead_pins) == 0);
    TEST_ASSERT_EQUAL_size_t(0, mod->dead_pin_names.len);
}

/* A second round on the compacted module (the unnamed net is now net 2) works the same. */
static void compact_again(uint32_t tomb) {
    odin3_net_id after[LINK_PINS] = {{2},
                                     odin3_module_find_net(module, intern("ta")),
                                     odin3_module_find_net(module, intern("tc"))};
    odin3_node_id again = make_link("again", 7, after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, again));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    verify_link_tomb(tomb, "again", 7, (uint32_t[LINK_PINS]){0, intern("ta"), intern("tc")});
    verify_side_table_dropped(module);
}

static void test_compact_tombstone_params_pin_nets(void) {
    odin3_net_id nets[LINK_PINS];
    make_link_nets("t", nets);
    odin3_node_id victim = make_link("victim", 42, nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, victim));
    odin3_net_id gone = make_net("gone");
    kill_wire(module, make_wire("gw", &gone, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, gone));
    uint32_t tombs = odin3_tombstone_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    TEST_ASSERT_EQUAL_UINT32(tombs + 3, odin3_tombstone_end(design));
    uint32_t want[LINK_PINS] = {intern("ta"), 0, intern("tc")};
    verify_link_tomb(tombs, "victim", 42, want);
    const odin3_tombstone *net = odin3_tombstone_get(design, tombs + 1);
    const odin3_tombstone *wire = odin3_tombstone_get(design, tombs + 2);
    TEST_ASSERT_TRUE(net->kind == ODIN3_OBJ_NET && net->name == intern("gone"));
    TEST_ASSERT_TRUE(wire->kind == ODIN3_OBJ_WIRE && wire->name == intern("gw"));
    verify_no_arrays(net);
    verify_no_arrays(wire);
    verify_side_table_dropped(module);
    verify_check_no_errors(module);
    compact_again(tombs + 3);
}

/* A replaced node's tombstone names the nets its pins were on before the new node took them. */
static void test_compact_replaced_node_pin_nets(void) {
    odin3_net_id nets[LINK_PINS];
    make_link_nets("r", nets);
    odin3_node_id old = make_link("old", 3, nets);
    odin3_node_id now = make_link("new", 4, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_replace(module, (odin3_node_pair){old, now}));
    TEST_ASSERT_EQUAL_UINT32(nets[0].v,
                             odin3_pin_net(module, odin3_node_port(module, now, 0).first).v);
    uint32_t tombs = odin3_tombstone_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    TEST_ASSERT_EQUAL_UINT32(tombs + 1, odin3_tombstone_end(design));
    verify_link_tomb(tombs, "old", 3, (uint32_t[LINK_PINS]){intern("ra"), 0, intern("rc")});
    verify_side_table_dropped(module);
    verify_check_no_errors(module);
}

static void set_int_attr(odin3_objref obj, const char *key, int64_t num) {
    odin3_value value = odin3_value_int(num);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern(key), &value));
}

static int64_t int_attr(odin3_objref obj, const char *key) {
    const odin3_value *value = odin3_attr_get(module, obj, intern(key));
    TEST_ASSERT_NOT_NULL(value);
    return value->i;
}

/* The IDs of the objects the alias test keeps (old IDs before compact, new ones after). */
typedef struct alias_scene {
    odin3_node_id node_k;
    odin3_net_id p, s;
    odin3_wire_id bus, v;
} alias_scene;

/*
 * Dead objects first (so every survivor's ID shifts), then: nets p, q, r, s; wire "bus" [1:0]
 * over {q, r}; merge q into p (p: name q, bus[0]); bus[1] rebound to p (p's primary); bus[0]
 * rebound to s (p's alias record for it is left unused); wire "v" over {p} (p's alias v[0]); r,
 * without pins or memberships, deleted. Attributes on a node (overwritten), p, bus, the module,
 * dead q and a dead node.
 */
static alias_scene alias_build(void) {
    alias_scene sc = {0};
    odin3_node_spec spec = {type_id("test_t7_link"), intern("dead"), src_prov(3), NULL, 0};
    odin3_node_id dead = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &dead));
    set_int_attr((odin3_objref){ODIN3_OBJ_NODE, dead.v}, "keep", 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, dead));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, make_net("x")));
    spec.name = intern("k");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &sc.node_k));
    set_int_attr((odin3_objref){ODIN3_OBJ_NODE, sc.node_k.v}, "keep", 2);
    set_int_attr((odin3_objref){ODIN3_OBJ_NODE, sc.node_k.v}, "keep", 3); /* overwritten */
    sc.p = make_net("p");
    odin3_net_id net_q = make_net("q");
    odin3_net_id net_r = make_net("r");
    sc.s = make_net("s");
    odin3_net_id qr[2] = {net_q, net_r};
    sc.bus = make_wire("bus", qr, 2);
    set_int_attr((odin3_objref){ODIN3_OBJ_NET, net_q.v}, "keep", 4);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){sc.p, net_q}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){sc.bus, 1}, sc.p));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){sc.bus, 0}, sc.s));
    sc.v = make_wire("v", &sc.p, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, net_r));
    set_int_attr((odin3_objref){ODIN3_OBJ_NET, sc.p.v}, "keep", 5);
    set_int_attr((odin3_objref){ODIN3_OBJ_WIRE, sc.bus.v}, "keep", 6);
    set_int_attr((odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(module).v}, "keep", 7);
    return sc;
}

static void verify_alias_scene(const alias_scene *sc) {
    TEST_ASSERT_EQUAL_UINT32(sc->p.v, odin3_module_find_net(module, intern("p")).v);
    TEST_ASSERT_EQUAL_UINT32(sc->p.v, odin3_module_find_net(module, intern("q")).v); /* merged */
    TEST_ASSERT_EQUAL_UINT32(sc->s.v, odin3_module_find_net(module, intern("s")).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_net(module, intern("x")).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_net(module, intern("r")).v);
    TEST_ASSERT_EQUAL_UINT32(sc->bus.v, odin3_module_find_wire(module, intern("bus")).v);
    TEST_ASSERT_EQUAL_UINT32(sc->v.v, odin3_module_find_wire(module, intern("v")).v);
    odin3_wirebit primary = odin3_net_primary(module, sc->p);
    TEST_ASSERT_EQUAL_UINT32(sc->bus.v, primary.wire.v);
    TEST_ASSERT_EQUAL_UINT32(1, primary.bit);
    TEST_ASSERT_EQUAL_UINT32(sc->bus.v, odin3_net_primary(module, sc->s).wire.v);
    TEST_ASSERT_EQUAL_UINT32(sc->s.v, odin3_wire_net(module, sc->bus, 0).v);
    TEST_ASSERT_EQUAL_UINT32(sc->p.v, odin3_wire_net(module, sc->bus, 1).v);
    TEST_ASSERT_EQUAL_UINT32(sc->p.v, odin3_wire_net(module, sc->v, 0).v);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    TEST_ASSERT_TRUE(odin3_net_alias_next(module, sc->p, &cursor, &alias));
    TEST_ASSERT_EQUAL_UINT32(intern("q"), alias.name);
    TEST_ASSERT_EQUAL_UINT32(0, alias.wb.wire.v);
    TEST_ASSERT_TRUE(odin3_net_alias_next(module, sc->p, &cursor, &alias));
    TEST_ASSERT_EQUAL_UINT32(sc->v.v, alias.wb.wire.v);
    TEST_ASSERT_EQUAL_UINT32(0, alias.wb.bit);
    TEST_ASSERT_FALSE(odin3_net_alias_next(module, sc->p, &cursor, &alias));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(module, sc->s));
    TEST_ASSERT_EQUAL_INT64(3, int_attr((odin3_objref){ODIN3_OBJ_NODE, sc->node_k.v}, "keep"));
    TEST_ASSERT_EQUAL_INT64(5, int_attr((odin3_objref){ODIN3_OBJ_NET, sc->p.v}, "keep"));
    TEST_ASSERT_EQUAL_INT64(6, int_attr((odin3_objref){ODIN3_OBJ_WIRE, sc->bus.v}, "keep"));
    TEST_ASSERT_EQUAL_INT64(
        7, int_attr((odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(module).v}, "keep"));
}

static void test_compact_names_aliases_attrs(void) {
    alias_scene old = alias_build();
    verify_alias_scene(&old);
    TEST_ASSERT_EQUAL_size_t(4, module->aliases.len); /* dummy, name q, unused bus[0], v[0] */
    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    alias_scene now = {{map.node[old.node_k.v]},
                       {map.net[old.p.v]},
                       {map.net[old.s.v]},
                       {map.wire[old.bus.v]},
                       {map.wire[old.v.v]}};
    TEST_ASSERT_EQUAL_UINT32(1, now.node_k.v); /* every survivor moved down */
    TEST_ASSERT_EQUAL_UINT32(1, now.p.v);
    TEST_ASSERT_EQUAL_UINT32(2, now.s.v);
    TEST_ASSERT_EQUAL_UINT32(old.bus.v, now.bus.v); /* no wire died */
    verify_alias_scene(&now);
    TEST_ASSERT_EQUAL_size_t(3, module->aliases.len); /* the unused record is dropped */
    TEST_ASSERT_EQUAL_size_t(5, module->attrs.len);   /* dummy + four live attributes */
    TEST_ASSERT_EQUAL_size_t(4, odin3_u64map_count(module->attr_heads));
    verify_check_no_errors(module);
    odin3_compact_map_free(&map);
}

/* A dead wire whose memberships were left in place: compact drops the primary and the alias. */
static void test_compact_drops_dead_wire_memberships(void) {
    odin3_net_id net = make_net("m");
    odin3_wire_id first = make_wire("w1", &net, 1);  /* m's primary */
    odin3_wire_id second = make_wire("w2", &net, 1); /* m's alias */
    odin3_wire_id kept = make_wire("w3", &net, 1);   /* m's alias, survives */
    mark_wire_dead(module, first);
    mark_wire_dead(module, second);
    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    odin3_net_id now = {map.net[net.v]};
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_primary(module, now).wire.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_primary(module, now).bit);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_alias_count(module, now));
    uint32_t cursor = 0;
    odin3_net_alias alias;
    TEST_ASSERT_TRUE(odin3_net_alias_next(module, now, &cursor, &alias));
    TEST_ASSERT_EQUAL_UINT32(map.wire[kept.v], alias.wb.wire.v);
    TEST_ASSERT_EQUAL_UINT32(1, map.wire[kept.v]);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_wire_end(module));
    verify_check_no_errors(module);
    odin3_compact_map_free(&map);
}

/* --- small cases ------------------------------------------------------------------------------ */

static void test_compact_empty_module(void) {
    uint32_t tombs = odin3_tombstone_end(design);
    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    TEST_ASSERT_EQUAL_UINT32(1, map.n_node);
    TEST_ASSERT_EQUAL_UINT32(1, map.n_net);
    TEST_ASSERT_EQUAL_UINT32(1, map.n_wire);
    TEST_ASSERT_EQUAL_UINT32(1, map.n_pin);
    TEST_ASSERT_EQUAL_UINT32(0, map.node[0]);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_pin_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_net_end(module));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_wire_end(module));
    TEST_ASSERT_EQUAL_UINT32(tombs, odin3_tombstone_end(design));
    verify_check_clean(module);
    odin3_compact_map_free(&map);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    odin3_compact_map_free(NULL);
}

static void test_compact_null_module(void) {
    odin3_compact_map map = {NULL, NULL, NULL, NULL, 1, 1, 1, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_module_compact(NULL, &map));
    TEST_ASSERT_EQUAL_UINT32(0, map.n_node);
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
}

/* --- fingerprint: every observable fact of a module ----------------------------------------- */

static uint64_t fp_value(uint64_t hash, const odin3_value *value) {
    if (value == NULL) {
        return odin3_hash_combine(hash, UINT64_MAX);
    }
    hash = odin3_hash_combine(hash, (uint64_t)value->kind);
    hash = odin3_hash_combine(hash, (uint64_t)value->i);
    hash = odin3_hash_combine(hash, value->len);
    for (uint32_t k = 0; value->bits != NULL && k < value->len; k++) {
        hash = odin3_hash_combine(hash, value->bits[k]);
    }
    return hash;
}

static uint64_t fp_attr(const odin3_module *mod, odin3_objref obj, uint64_t hash) {
    uint32_t key = 0;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("keep"), &key)) {
        hash = fp_value(hash, odin3_attr_get(mod, obj, key));
    }
    return hash;
}

static uint64_t fp_nodes(const odin3_module *mod, uint64_t hash) {
    for (uint32_t i = 1; i < odin3_module_node_end(mod); i++) {
        odin3_node_id id = {i};
        odin3_pinslice pins = odin3_node_pins(mod, id);
        hash = odin3_hash_combine(hash, odin3_node_live(mod, id));
        hash = odin3_hash_combine(hash, odin3_node_type(mod, id).v);
        hash = odin3_hash_combine(hash, odin3_node_name(mod, id));
        hash = odin3_hash_combine(hash, odin3_node_prov(mod, id).v);
        hash = odin3_hash_combine(hash, odin3_module_find_node(mod, odin3_node_name(mod, id)).v);
        hash = odin3_hash_combine(hash, ((uint64_t)pins.first.v << 32) | pins.count);
        for (uint32_t k = 0; k < 2; k++) {
            hash = fp_value(hash, odin3_node_param(mod, id, k));
        }
        hash = fp_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, i}, hash);
    }
    for (uint32_t i = 1; i < odin3_module_pin_end(mod); i++) {
        odin3_pin_id id = {i};
        hash = odin3_hash_combine(hash, odin3_pin_node(mod, id).v);
        hash = odin3_hash_combine(hash, odin3_pin_net(mod, id).v);
        hash = odin3_hash_combine(hash, ((uint64_t)odin3_pin_port(mod, id) << 32) |
                                            odin3_pin_bit(mod, id));
    }
    return hash;
}

static uint64_t fp_net(const odin3_module *mod, odin3_net_id id, uint64_t hash) {
    odin3_pinlist pins = odin3_net_pins(mod, id);
    odin3_wirebit primary = odin3_net_primary(mod, id);
    hash = odin3_hash_combine(hash, odin3_net_live(mod, id));
    hash = odin3_hash_combine(hash, odin3_net_name(mod, id));
    hash = odin3_hash_combine(hash, odin3_net_prov(mod, id).v);
    hash = odin3_hash_combine(hash, odin3_module_find_net(mod, odin3_net_name(mod, id)).v);
    hash = odin3_hash_combine(hash, odin3_net_driver_count(mod, id));
    for (uint32_t k = 0; k < pins.count; k++) {
        hash = odin3_hash_combine(hash, pins.pins[k].v);
    }
    hash = odin3_hash_combine(hash, ((uint64_t)primary.wire.v << 32) | primary.bit);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(mod, id, &cursor, &alias)) {
        hash = odin3_hash_combine(hash, ((uint64_t)alias.wb.wire.v << 32) | alias.wb.bit);
        hash = odin3_hash_combine(hash, odin3_module_find_net(mod, alias.name).v);
    }
    return fp_attr(mod, (odin3_objref){ODIN3_OBJ_NET, id.v}, hash);
}

static uint64_t fp_wire(const odin3_module *mod, odin3_wire_id id, uint64_t hash) {
    hash = odin3_hash_combine(hash, odin3_wire_live(mod, id));
    hash = odin3_hash_combine(hash, odin3_wire_name(mod, id));
    hash = odin3_hash_combine(hash, odin3_wire_prov(mod, id).v);
    hash = odin3_hash_combine(hash, odin3_module_find_wire(mod, odin3_wire_name(mod, id)).v);
    hash = odin3_hash_combine(hash, (uint64_t)(int64_t)odin3_wire_msb(mod, id));
    hash = odin3_hash_combine(hash, (uint64_t)(int64_t)odin3_wire_lsb(mod, id));
    hash = odin3_hash_combine(hash, odin3_wire_port_node(mod, id).v);
    for (uint32_t k = 0; k < odin3_wire_width(mod, id); k++) {
        hash = odin3_hash_combine(hash, odin3_wire_net(mod, id, k).v);
    }
    return fp_attr(mod, (odin3_objref){ODIN3_OBJ_WIRE, id.v}, hash);
}

static uint64_t fingerprint(const odin3_module *mod) {
    uint64_t hash = fp_nodes(mod, ODIN3_HASH_SEED);
    for (uint32_t i = 1; i < odin3_module_net_end(mod); i++) {
        hash = fp_net(mod, (odin3_net_id){i}, hash);
    }
    for (uint32_t i = 1; i < odin3_module_wire_end(mod); i++) {
        hash = fp_wire(mod, (odin3_wire_id){i}, hash);
    }
    for (uint32_t k = 0; k < odin3_module_port_count(mod); k++) {
        hash = odin3_hash_combine(hash, odin3_module_port(mod, k).v);
        hash = odin3_hash_combine(hash, odin3_module_port_wire(mod, k).v);
    }
    hash = fp_attr(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, hash);
    hash = odin3_hash_combine(hash, odin3_tombstone_end(design));
    return odin3_hash_combine(hash, odin3_prov_end(design));
}

/* --- out of memory -----------------------------------------------------------------------------
 */

/*
 * A fresh design and module with dead nodes, nets and wires, merged names, aliases and
 * attributes (a fresh design, so its provenance arena is one chunk that exhaust_prov_arena fills).
 */
static odin3_module *oom_module(void) {
    fresh_design("oom");
    chain ch = {OOM_CHAIN, true, true, NULL, NULL, NULL, NULL};
    dead_prefix(module);
    chain_build(module, &ch);
    chain_delete_odd(module, &ch);
    chain_free(&ch);
    (void)alias_build();
    return module;
}

/*
 * Uses up the provenance arena's only chunk (a fresh design's), so the next tombstone copy needs
 * a new one.
 */
static void exhaust_prov_arena(void) {
    odin3_arena *arena = design->prov->arena;
    size_t reserved = odin3_arena_bytes_reserved(arena);
    while (odin3_arena_bytes_used(arena) < reserved) {
        TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, 1));
        TEST_ASSERT_EQUAL_size_t(reserved, odin3_arena_bytes_reserved(arena)); /* no new chunk */
    }
}

/* What a module was before a failing compact. */
typedef struct oom_before {
    uint64_t fingerprint;
    const odin3_pagevec *nodes;
    const odin3_arena *arena;
} oom_before;

/* A failed compact: NO_MEMORY, map zeroed, the containers and every observable fact unchanged. */
static void oom_verify_failed(odin3_module *mod, odin3_status st, const odin3_compact_map *map,
                              oom_before before) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_NULL(map->node);
    TEST_ASSERT_EQUAL_UINT32(0, map->n_node);
    TEST_ASSERT_TRUE(before.nodes == mod->nodes && before.arena == mod->arena); /* not swapped */
    TEST_ASSERT_TRUE(before.fingerprint == fingerprint(mod));
    verify_check_no_errors(mod);
}

/* One try: a fresh design and module, allocation `fail_at` fails. */
static odin3_status oom_try(long fail_at) {
    odin3_module *mod = oom_module();
    oom_before before = {fingerprint(mod), mod->nodes, mod->arena};
    exhaust_prov_arena();
    size_t prov_bytes = odin3_arena_bytes_reserved(design->prov->arena);
    odin3_compact_map map;
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_module_compact(mod, &map);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        oom_verify_failed(mod, st, &map, before);
        return st;
    }
    TEST_ASSERT_TRUE(before.fingerprint != fingerprint(mod)); /* it did compact */
    /* the dead nodes' tombstone arrays took a new arena chunk: the sweep failed that too */
    TEST_ASSERT_TRUE(odin3_arena_bytes_reserved(design->prov->arena) > prov_bytes);
    verify_side_table_dropped(mod);
    verify_check_no_errors(mod);
    odin3_compact_map_free(&map);
    return st;
}

static void test_compact_oom_leaves_ir_unchanged(void) {
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = oom_try(fail_at);
        failures += st != ODIN3_OK ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(OOM_MIN_FAILURES, failures);
}

/*
 * Deleting (or replacing) a node records its pins' net names; on out of memory the IR and the
 * records are unchanged. Each try uses a fresh module, so the record table starts empty.
 */
static odin3_status delete_try(long fail_at, bool replace) {
    char name[NAME_BUF];
    (void)snprintf(name, sizeof name, "%s%ld", replace ? "rep" : "del", fail_at);
    module = new_module(name);
    odin3_net_id nets[LINK_PINS];
    make_link_nets("d", nets);
    odin3_node_id victim = make_link("victim", 1, nets);
    odin3_node_id spare = make_link("spare", 2, NULL);
    uint64_t before = fingerprint(module);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = replace ? odin3_node_replace(module, (odin3_node_pair){victim, spare})
                              : odin3_node_delete(module, victim);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_TRUE(before == fingerprint(module));
        TEST_ASSERT_TRUE(odin3_node_live(module, victim));
        verify_side_table_dropped(module); /* nothing recorded */
        return st;
    }
    uint32_t tombs = odin3_tombstone_end(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    verify_link_tomb(tombs, "victim", 1, (uint32_t[LINK_PINS]){intern("da"), 0, intern("dc")});
    verify_check_no_errors(module);
    return st;
}

static void delete_oom_sweep(bool replace) {
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = delete_try(fail_at, replace);
        failures += st != ODIN3_OK ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(DELETE_OOM_MIN_FAILURES, failures);
}

static void test_node_delete_oom_records_nothing(void) {
    delete_oom_sweep(false);
}

static void test_node_replace_oom_records_nothing(void) {
    delete_oom_sweep(true);
}

/* --- linear cost -------------------------------------------------------------------------------
 */

typedef struct compact_cost {
    double seconds;
    size_t bytes;
} compact_cost;

static size_t module_bytes(const odin3_module *mod) {
    return odin3_pagevec_bytes_reserved(mod->nodes) + odin3_pagevec_bytes_reserved(mod->pins) +
           odin3_pagevec_bytes_reserved(mod->nets) + odin3_pagevec_bytes_reserved(mod->wires) +
           odin3_pinpool_bytes_reserved(&mod->pinpool) + odin3_arena_bytes_reserved(mod->arena);
}

static compact_cost big_compact(uint32_t count) {
    module = new_module(count == BIG_SMALL ? "big_small" : "big_large");
    chain ch = {count, false, false, NULL, NULL, NULL, NULL};
    chain_build(module, &ch);
    chain_delete_odd(module, &ch);
    size_t prov_bytes = odin3_arena_bytes_reserved(design->prov->arena);
    clock_t start = clock();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, NULL));
    compact_cost cost = {(double)(clock() - start) / CLOCKS_PER_SEC,
                         module_bytes(module) +
                             (odin3_arena_bytes_reserved(design->prov->arena) - prov_bytes)};
    TEST_ASSERT_EQUAL_UINT32(count / 2 + 2 + 1, odin3_module_node_end(module)); /* + 2 ports */
    odin3_node_id last = odin3_module_find_node(module, numbered_name("n", count));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_node_end(module) - 1, last.v);
    chain_free(&ch);
    return cost;
}

static void test_compact_linear(void) {
    compact_cost small = big_compact(BIG_SMALL);
    compact_cost large = big_compact(BIG_LARGE);
    double ratio = (double)BIG_LARGE / BIG_SMALL;
    printf("compact: %u nodes %.3f s %zu B; %u nodes %.3f s %zu B\n", BIG_SMALL, small.seconds,
           small.bytes, BIG_LARGE, large.seconds, large.bytes);
    TEST_ASSERT_TRUE(large.seconds <= LINEAR_SLACK * ratio * small.seconds + TIME_FLOOR_SECONDS);
    TEST_ASSERT_TRUE((double)large.bytes <= LINEAR_SLACK * ratio * (double)small.bytes);
}

int main(void) {
    if (odin3_celltype_register_global(&k_link) != ODIN3_OK) {
        return EXIT_FAILURE;
    }
    UNITY_BEGIN();
    RUN_TEST(test_compact_chain);
    RUN_TEST(test_compact_names_aliases_attrs);
    RUN_TEST(test_compact_drops_dead_wire_memberships);
    RUN_TEST(test_compact_empty_module);
    RUN_TEST(test_compact_null_module);
    RUN_TEST(test_compact_tombstone_params_pin_nets);
    RUN_TEST(test_compact_replaced_node_pin_nets);
    RUN_TEST(test_compact_oom_leaves_ir_unchanged);
    RUN_TEST(test_node_delete_oom_records_nothing);
    RUN_TEST(test_node_replace_oom_records_nothing);
    RUN_TEST(test_compact_linear);
    return UNITY_END();
}
