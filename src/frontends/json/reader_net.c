/*
 * reader_net.c — bits and nets of one module (M-3): the labels the file uses, the nets made for
 * them in ascending order, the constants, and the Q bits of latches (M-11).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INIT_UNKNOWN = 3 };

static const char *const CONST_TYPES[JR_CONSTS] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_",
                                                   "$_CONSTZ_"};

/* --- bits ---------------------------------------------------------------------------------- */

odin3_status jr_bit_parse(const jr *rd, const odin3_json_node *node, const jr_where *where,
                          uint32_t index, jr_bit *out) {
    odin3_json_kind kind = jr_kind(node);
    if (kind == ODIN3_JSON_INT && jr_int(node) >= 0) {
        *out = (jr_bit){JR_BIT_NET, (uint64_t)jr_int(node)};
        return ODIN3_OK;
    }
    size_t len = 0;
    const char *text = kind == ODIN3_JSON_STRING ? jr_text(node, &len) : NULL;
    if (text != NULL && len == 1 && strchr("01xz", text[0]) != NULL) {
        *out = (jr_bit){(uint32_t)(strchr("01xz", text[0]) - "01xz"), 0};
        return ODIN3_OK;
    }
    char path[JR_NAME_BUF];
    (void)snprintf(path, sizeof path, "%s[%u]", where->member != NULL ? where->member : "bits",
                   (unsigned)index);
    jr_where here = {where->section, where->key, path};
    return jr_error(rd, node, &here,
                    "a bit is a non-negative integer or one of \"0\" \"1\" \"x\" \"z\"");
}

odin3_status jr_bits_count(const jr *rd, const odin3_json_node *bits, const jr_where *where,
                           bool empty_ok, uint32_t *count) {
    if (bits == NULL || jr_kind(bits) != ODIN3_JSON_ARRAY) {
        return jr_error(rd, bits, where, "bits is not an array");
    }
    size_t have = jr_count(bits);
    if ((have == 0 && !empty_ok) || have > ODIN3_READER_MAX_WIDTH) {
        return jr_error(rd, bits, where, "%zu bits (1 to %u are allowed)", have,
                        (unsigned)ODIN3_READER_MAX_WIDTH);
    }
    *count = (uint32_t)have;
    return ODIN3_OK;
}

/* --- state ---------------------------------------------------------------------------------- */

void jr_cur_init(jr *rd) {
    odin3_vec_init(&rd->cur.labels, sizeof(uint64_t));
    odin3_vec_init(&rd->cur.conn_nets, sizeof(odin3_net_id));
    odin3_vec_init(&rd->cur.conn_ports, sizeof(odin3_netvec));
    odin3_vec_init(&rd->cur.params, sizeof(odin3_value));
}

odin3_status jr_cur_begin(jr *rd, const jr_mod *mod) {
    jr_cur *cur = &rd->cur;
    odin3_u64map_destroy(cur->nets);
    odin3_u64map_destroy(cur->qbits);
    odin3_u64map_destroy(cur->qinit);
    cur->nets = odin3_u64map_create(0);
    cur->qbits = odin3_u64map_create(0);
    cur->qinit = odin3_u64map_create(0);
    odin3_vec_clear(&cur->labels);
    memset(cur->consts, 0, sizeof cur->consts);
    memset(cur->const_used, 0, sizeof cur->const_used);
    cur->n_consts = 0;
    cur->mod = mod;
    cur->module = odin3_module_get(rd->design, mod->module);
    if (cur->nets == NULL || cur->qbits == NULL || cur->qinit == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    return ODIN3_OK;
}

void jr_cur_end(jr *rd) {
    jr_cur *cur = &rd->cur;
    odin3_u64map_destroy(cur->nets);
    odin3_u64map_destroy(cur->qbits);
    odin3_u64map_destroy(cur->qinit);
    cur->nets = cur->qbits = cur->qinit = NULL;
    odin3_vec_free(&cur->labels);
    odin3_vec_free(&cur->conn_nets);
    odin3_vec_free(&cur->conn_ports);
    odin3_vec_free(&cur->params);
}

/* --- drivers (check rule 4 at the source) ------------------------------------------------- */

/* An inout pin or an output of a TRISTATE type: may share a net with other such drivers. */
bool jr_is_bus_driver(const jr *rd, odin3_pin_id pin) {
    const odin3_module *module = rd->cur.module;
    if (odin3_pin_dir(module, pin) == ODIN3_DIR_INOUT) {
        return true;
    }
    odin3_celltype_id type = odin3_node_type(module, odin3_pin_node(module, pin));
    const odin3_celltype_def *def = odin3_celltype_get(rd->design, type);
    return def != NULL && (def->flags & ODIN3_CT_TRISTATE) != 0;
}

uint32_t jr_ordinary_drivers(const jr *rd, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(rd->cur.module, net);
    uint32_t drivers = odin3_net_driver_count(rd->cur.module, net);
    uint32_t ordinary = 0;
    for (uint32_t i = 0; i < drivers && i < pins.count; i++) {
        ordinary += jr_is_bus_driver(rd, pins.pins[i]) ? 0U : 1U;
    }
    return ordinary;
}

/* --- ports: the nets add_port made ---------------------------------------------------------- */

static bool port_is_output(const odin3_json_node *port) {
    const odin3_json_node *dir = jr_member(port, "direction");
    size_t len = 0;
    const char *text = dir != NULL ? jr_text(dir, &len) : NULL;
    return text != NULL && strcmp(text, "output") == 0;
}

/* Marks constant k as used, in order of first use. */
static void use_const(jr_cur *cur, uint32_t kind) {
    if (!cur->const_used[kind]) {
        cur->const_used[kind] = true;
        cur->const_order[cur->n_consts++] = kind;
    }
}

/* Merges the net drop into keep (a port bit that shares a label or a constant with an earlier
 * one); two drivers on the result are an error. */
static odin3_status merge_port_net(jr *rd, odin3_net_pair pair, const odin3_json_node *port,
                                   const jr_where *where) {
    odin3_status st = jr_fail(rd, odin3_net_merge(rd->cur.module, pair));
    if (st == ODIN3_OK && jr_ordinary_drivers(rd, pair.keep) >= 2) {
        return jr_error(rd, port, where, "a net is driven by more than one input port bit");
    }
    return st;
}

/* One port bit: bit k of the port wire, whose net add_port made. */
static odin3_status bind_port_bit(jr *rd, const jr_bit *bit, odin3_net_id made,
                                  const odin3_json_node *port, const jr_where *where) {
    jr_cur *cur = &rd->cur;
    uint64_t found = 0;
    if (bit->kind == JR_BIT_NET) {
        if (!odin3_u64map_get(cur->nets, bit->num, &found)) {
            return jr_fail(rd, odin3_u64map_put(cur->nets, (odin3_kv){bit->num, made.v}));
        }
        return merge_port_net(rd, (odin3_net_pair){{(uint32_t)found}, made}, port, where);
    }
    if (!port_is_output(port)) {
        return jr_error(rd, port, where, "an input or inout port bit cannot be a constant");
    }
    use_const(cur, bit->kind);
    if (!odin3_net_valid(cur->consts[bit->kind])) {
        cur->consts[bit->kind] = made;
        return ODIN3_OK;
    }
    return merge_port_net(rd, (odin3_net_pair){cur->consts[bit->kind], made}, port, where);
}

static odin3_status bind_port(jr *rd, const odin3_json_node *port, uint32_t index) {
    const odin3_module *module = rd->cur.module;
    odin3_wire_id wire = odin3_module_port_wire(module, index);
    jr_where where = {"ports", odin3_json_key(port), "bits"};
    const odin3_json_node *bits = jr_member(port, "bits");
    uint32_t k = 0;
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *elem = jr_first(bits); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem), k++) {
        jr_bit bit = {0, 0};
        st = jr_bit_parse(rd, elem, &where, k, &bit);
        if (st == ODIN3_OK) {
            st = bind_port_bit(rd, &bit, odin3_wire_net(module, wire, k), port, &where);
        }
    }
    return st;
}

static odin3_status bind_ports(jr *rd) {
    const odin3_json_node *ports = jr_member(rd->cur.mod->node, "ports");
    odin3_status st = ODIN3_OK;
    uint32_t index = 0;
    for (const odin3_json_node *port = jr_first(ports); st == ODIN3_OK && port != NULL;
         port = odin3_json_next(port), index++) {
        st = bind_port(rd, port, index);
    }
    return st;
}

/* --- the labels the netnames and cells use -------------------------------------------------- */

/* A label no net has yet: remembered once, in the order it is met. */
static odin3_status note_bit(jr *rd, const jr_bit *bit) {
    jr_cur *cur = &rd->cur;
    uint64_t seen = 0;
    if (bit->kind != JR_BIT_NET) {
        use_const(cur, bit->kind);
        return ODIN3_OK;
    }
    if (odin3_u64map_get(cur->nets, bit->num, &seen)) {
        return ODIN3_OK;
    }
    uint64_t *slot = odin3_vec_push(&cur->labels);
    if (slot == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *slot = bit->num;
    return jr_fail(rd, odin3_u64map_put(cur->nets, (odin3_kv){bit->num, 0}));
}

/* Notes every bit of a bits array; q marks them as Q bits of a latch too. */
static odin3_status note_bits(jr *rd, const odin3_json_node *bits, const jr_where *where,
                              bool is_q) {
    uint32_t count = 0;
    odin3_status st = jr_bits_count(rd, bits, where, true, &count);
    uint32_t k = 0;
    for (const odin3_json_node *elem = jr_first(bits); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem), k++) {
        jr_bit bit = {0, 0};
        st = jr_bit_parse(rd, elem, where, k, &bit);
        if (st == ODIN3_OK) {
            st = note_bit(rd, &bit);
        }
        if (st == ODIN3_OK && is_q && bit.kind == JR_BIT_NET) {
            st = jr_fail(rd, odin3_u64map_put(rd->cur.qbits, (odin3_kv){bit.num, 1}));
        }
    }
    return st;
}

/* True for a registered bit-level storage cell: it has an INIT parameter and a Q port. */
static bool is_q_cell(const odin3_celltype_def *def) {
    bool init = false;
    bool qport = false;
    for (uint32_t i = 0; i < def->n_params; i++) {
        init = init ||
               (strcmp(def->params[i].name, "INIT") == 0 && def->params[i].kind == ODIN3_VAL_INT);
    }
    for (uint32_t i = 0; i < def->n_ports; i++) {
        qport = qport || strcmp(def->ports[i].name, "Q") == 0;
    }
    return def->gran == ODIN3_GRAN_BIT && init && qport;
}

static bool cell_has_q(const jr *rd, const odin3_json_node *cell) {
    const odin3_json_node *type = jr_member(cell, "type");
    odin3_celltype_id id = {0};
    if (type == NULL || jr_kind(type) != ODIN3_JSON_STRING ||
        !jr_lookup_type(rd, jr_text(type, NULL), &id)) {
        return false;
    }
    const odin3_celltype_def *def = odin3_celltype_get(rd->design, id);
    return def != NULL && is_q_cell(def);
}

/* The connections of one cell. */
static odin3_status scan_cell(jr *rd, const odin3_json_node *cell) {
    const odin3_json_node *conns = jr_member(cell, "connections");
    if (conns == NULL || jr_kind(conns) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, cell, &(jr_where){"cells", odin3_json_key(cell), NULL},
                        "the cell has no connections object");
    }
    bool has_q = cell_has_q(rd, cell);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(conns); st == ODIN3_OK && conn != NULL;
         conn = odin3_json_next(conn)) {
        char path[JR_NAME_BUF];
        (void)snprintf(path, sizeof path, "connections.%s", odin3_json_key(conn));
        jr_where where = {"cells", odin3_json_key(cell), path};
        st = note_bits(rd, conn, &where, has_q && strcmp(odin3_json_key(conn), "Q") == 0);
    }
    return st;
}

/* The INIT value of one char of an init string: 0, 1, else 3 (unknown). */
static uint64_t init_value(char chr) {
    if (chr == '0' || chr == '1') {
        return (uint64_t)(chr - '0');
    }
    return INIT_UNKNOWN;
}

/* M-11: the init attribute of a netname gives the INIT of the latches whose Q bits it names. */
static odin3_status scan_init(jr *rd, const odin3_json_node *name) {
    const odin3_json_node *attrs = jr_member(name, "attributes");
    const odin3_json_node *init = attrs != NULL ? jr_member(attrs, "init") : NULL;
    const odin3_json_node *bits = jr_member(name, "bits");
    if (init == NULL || jr_kind(init) != ODIN3_JSON_STRING || bits == NULL) {
        return ODIN3_OK;
    }
    size_t len = 0;
    const char *text = jr_text(init, &len);
    odin3_status st = ODIN3_OK;
    size_t k = 0; /* bit k of the vector is the char len-1-k of the string */
    for (const odin3_json_node *elem = jr_first(bits); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem), k++) {
        uint64_t label = (uint64_t)jr_int(elem);
        uint64_t have = 0;
        bool is_q = jr_kind(elem) == ODIN3_JSON_INT && k < len &&
                    odin3_u64map_get(rd->cur.qbits, label, &have) &&
                    !odin3_u64map_get(rd->cur.qinit, label, &have);
        if (is_q) {
            st = jr_fail(rd, odin3_u64map_put(rd->cur.qinit,
                                              (odin3_kv){label, init_value(text[len - 1 - k])}));
        }
    }
    return st;
}

static odin3_status scan_netname(jr *rd, const odin3_json_node *name) {
    jr_where where = {"netnames", odin3_json_key(name), "bits"};
    if (jr_kind(name) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, name, &(jr_where){"netnames", where.key, NULL},
                        "the netname is not an object");
    }
    odin3_status st = note_bits(rd, jr_member(name, "bits"), &where, false);
    return st == ODIN3_OK ? scan_init(rd, name) : st;
}

odin3_status jr_scan_bits(jr *rd) {
    odin3_status st = bind_ports(rd);
    const odin3_json_node *cells = jr_member(rd->cur.mod->node, "cells");
    for (const odin3_json_node *cell = jr_first(cells); st == ODIN3_OK && cell != NULL;
         cell = odin3_json_next(cell)) {
        st = scan_cell(rd, cell);
    }
    const odin3_json_node *names = jr_member(rd->cur.mod->node, "netnames");
    for (const odin3_json_node *name = jr_first(names); st == ODIN3_OK && name != NULL;
         name = odin3_json_next(name)) {
        st = scan_netname(rd, name);
    }
    return st;
}

/* --- creating the nets ---------------------------------------------------------------------- */

static int compare_labels(const void *lhs, const void *rhs) {
    uint64_t left = *(const uint64_t *)lhs;
    uint64_t right = *(const uint64_t *)rhs;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* A net for every label no port made, in ascending label order. */
static odin3_status make_label_nets(jr *rd, odin3_prov_id prov) {
    jr_cur *cur = &rd->cur;
    if (cur->labels.len > 1) {
        qsort(cur->labels.data, cur->labels.len, sizeof(uint64_t), compare_labels);
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < cur->labels.len; i++) {
        uint64_t label = *(const uint64_t *)odin3_vec_cat(&cur->labels, i);
        odin3_net_id net = {0};
        st = jr_fail(rd, odin3_net_create(cur->module, 0, prov, &net));
        if (st == ODIN3_OK) {
            st = jr_fail(rd, odin3_u64map_put(cur->nets, (odin3_kv){label, net.v}));
        }
    }
    odin3_vec_clear(&cur->labels);
    return st;
}

/* The constant driver of net: one $_CONST?_ node of the module. */
static odin3_status make_const_node(jr *rd, uint32_t kind, odin3_prov_id prov) {
    uint32_t name = 0;
    odin3_celltype_id type = {0};
    odin3_status st = jr_intern(rd, CONST_TYPES[kind], &name);
    if (st == ODIN3_OK && !odin3_celltype_find(rd->design, name, &type)) {
        return jr_error(rd, NULL, NULL, "internal error: no %s cell type", CONST_TYPES[kind]);
    }
    odin3_net_id net = rd->cur.consts[kind];
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {.type = type, .prov = prov};
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_node_create_connected(rd->cur.module, &spec, &port, NULL));
    }
    return st;
}

/* The constants not made by a port, in the order of first use, and the driver of each. */
static odin3_status make_const_nets(jr *rd, odin3_prov_id prov) {
    jr_cur *cur = &rd->cur;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < cur->n_consts; i++) {
        uint32_t kind = cur->const_order[i];
        if (!odin3_net_valid(cur->consts[kind])) {
            st = jr_fail(rd, odin3_net_create(cur->module, 0, prov, &cur->consts[kind]));
        }
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < cur->n_consts; i++) {
        st = make_const_node(rd, cur->const_order[i], prov);
    }
    return st;
}

odin3_status jr_make_nets(jr *rd) {
    odin3_prov_id prov = odin3_module_prov(rd->cur.module);
    odin3_status st = make_label_nets(rd, prov);
    return st == ODIN3_OK ? make_const_nets(rd, prov) : st;
}

odin3_net_id jr_net_of(const jr *rd, const jr_bit *bit) {
    uint64_t net = 0;
    if (bit->kind != JR_BIT_NET) {
        return rd->cur.consts[bit->kind];
    }
    (void)odin3_u64map_get(rd->cur.nets, bit->num, &net);
    return (odin3_net_id){(uint32_t)net};
}
