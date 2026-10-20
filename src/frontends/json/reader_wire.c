/* reader_wire.c — netnames (M-2, M-4, M-11) and memories (M-13) of one module. */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

static const char *const NETNAME_KEYS[] = {"hide_name", "bits",   "offset",
                                           "upto",      "signed", "attributes"};
static const char *const MEMORY_KEYS[] = {"hide_name", "width", "size", "start_offset",
                                          "attributes"};

/* --- the init attribute (M-11) -------------------------------------------------------------- */

/* True when the netname has a string `init` and one of its bits is a latch Q bit: the init then
 * went to that latch's INIT parameter and is not kept as an attribute. */
static bool init_consumed(const jr *rd, const odin3_json_node *name) {
    const odin3_json_node *attrs = jr_member(name, "attributes");
    const odin3_json_node *init = attrs != NULL ? jr_member(attrs, "init") : NULL;
    if (init == NULL || jr_kind(init) != ODIN3_JSON_STRING) {
        return false;
    }
    uint64_t seen = 0;
    for (const odin3_json_node *bit = jr_first(jr_member(name, "bits")); bit != NULL;
         bit = odin3_json_next(bit)) {
        if (jr_kind(bit) == ODIN3_JSON_INT &&
            odin3_u64map_get(rd->cur.qbits, (uint64_t)jr_int(bit), &seen)) {
            return true;
        }
    }
    return false;
}

/* The attributes of a netname on obj; `init` only when no latch took it. */
static odin3_status name_attrs(jr *rd, odin3_objref obj, const odin3_json_node *name,
                               const jr_where *where) {
    const odin3_json_node *attrs = jr_member(name, "attributes");
    if (attrs == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(attrs) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, attrs, &(jr_where){where->section, where->key, "attributes"},
                        "attributes is not an object");
    }
    bool consumed = init_consumed(rd, name);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *member = jr_first(attrs); st == ODIN3_OK && member != NULL;
         member = odin3_json_next(member)) {
        if (!(consumed && strcmp(odin3_json_key(member), "init") == 0)) {
            st = jr_set_attr_member(rd, rd->cur.module, obj, member, where);
        }
    }
    return st;
}

/* --- one netname ---------------------------------------------------------------------------- */

/* Fills rd->cur.conn_nets with the nets of the netname's bits. */
static odin3_status collect_nets(jr *rd, const odin3_json_node *bits, const jr_where *where) {
    odin3_vec_clear(&rd->cur.conn_nets);
    uint32_t k = 0;
    for (const odin3_json_node *elem = jr_first(bits); elem != NULL;
         elem = odin3_json_next(elem), k++) {
        jr_bit bit = {0, 0};
        odin3_status st = jr_bit_parse(rd, elem, where, k, &bit);
        odin3_net_id *slot = st == ODIN3_OK ? odin3_vec_push(&rd->cur.conn_nets) : NULL;
        if (st == ODIN3_OK && slot == NULL) {
            return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        *slot = jr_net_of(rd, &bit);
    }
    return ODIN3_OK;
}

/* E-7: a netname that has the name of a port must have its width and its nets. */
static odin3_status check_port_netname(const jr *rd, odin3_wire_id wire,
                                       const odin3_json_node *name, const jr_where *where) {
    const odin3_module *module = rd->cur.module;
    const uint32_t width = (uint32_t)rd->cur.conn_nets.len;
    if (odin3_wire_width(module, wire) != width) {
        return jr_error(rd, name, where, "netname '%s' has %u bits but the port '%s' has %u",
                        odin3_json_key(name), (unsigned)width, odin3_json_key(name),
                        (unsigned)odin3_wire_width(module, wire));
    }
    for (uint32_t k = 0; k < width; k++) {
        const odin3_net_id *net = odin3_vec_cat(&rd->cur.conn_nets, k);
        if (net->v != odin3_wire_net(module, wire, k).v) {
            return jr_error(rd, name, where, "bit %u of netname '%s' differs from the port '%s'",
                            (unsigned)k, odin3_json_key(name), odin3_json_key(name));
        }
    }
    return ODIN3_OK;
}

static odin3_status make_wire(jr *rd, const odin3_json_node *name, uint32_t width,
                              odin3_wire_id *wire) {
    odin3_wire_spec spec = {0};
    odin3_status st = jr_shape(rd, name, "netnames", (jr_shape_out){width, &spec});
    if (st == ODIN3_OK) {
        st = jr_prov(rd, name, &spec.prov);
    }
    if (st == ODIN3_OK) {
        st = jr_intern(rd, odin3_json_key(name), &spec.name);
    }
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_wire_create(rd->cur.module, &spec, rd->cur.conn_nets.data, wire));
    }
    return st;
}

static odin3_status read_netname(jr *rd, const odin3_json_node *name) {
    jr_where where = {"netnames", odin3_json_key(name), NULL};
    if (jr_kind(name) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, name, &where, "the netname is not an object");
    }
    jr_audit(rd, name, NETNAME_KEYS, sizeof NETNAME_KEYS / sizeof NETNAME_KEYS[0]);
    const odin3_json_node *bits = jr_member(name, "bits");
    uint32_t width = 0;
    odin3_status st =
        jr_bits_count(rd, bits, &(jr_where){"netnames", where.key, "bits"}, false, &width);
    if (st == ODIN3_OK) {
        st = collect_nets(rd, bits, &(jr_where){"netnames", where.key, "bits"});
    }
    uint32_t str = 0;
    if (st == ODIN3_OK) {
        st = jr_intern(rd, where.key, &str);
    }
    odin3_wire_id wire = odin3_module_find_wire(rd->cur.module, str);
    if (st == ODIN3_OK && odin3_wire_valid(wire)) {
        st = check_port_netname(rd, wire, name, &where);
    } else if (st == ODIN3_OK) {
        st = make_wire(rd, name, width, &wire);
    }
    return st == ODIN3_OK ? name_attrs(rd, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, name, &where)
                          : st;
}

odin3_status jr_read_netnames(jr *rd, const odin3_json_node *obj) {
    if (obj == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(obj) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, obj, &(jr_where){"netnames", NULL, NULL}, "netnames is not an object");
    }
    if (jr_count(obj) > JR_MAX_OBJECTS) {
        return jr_error(rd, obj, &(jr_where){"netnames", NULL, NULL}, "more than %u netnames",
                        (unsigned)JR_MAX_OBJECTS);
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *name = jr_first(obj); st == ODIN3_OK && name != NULL;
         name = odin3_json_next(name)) {
        st = read_netname(rd, name);
    }
    return st;
}

/* --- memories (M-13) ------------------------------------------------------------------------ */

static odin3_status memory_attr(jr *rd, const char *name, const char *field,
                                const odin3_value *val) {
    char key[JR_NAME_BUF * 2];
    (void)snprintf(key, sizeof key, "memory:%s:%s", name, field);
    odin3_module *module = rd->cur.module;
    return jr_set_attr_named(rd, module, (odin3_objref){ODIN3_OBJ_MODULE, rd->cur.mod->module.v},
                             key, val);
}

/* The numbers of a memory, each non-negative. */
static odin3_status memory_number(jr *rd, const odin3_json_node *mem, const char *field) {
    const odin3_json_node *num = jr_member(mem, field);
    jr_where where = {"memories", odin3_json_key(mem), field};
    if (num == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(num) != ODIN3_JSON_INT || jr_int(num) < 0) {
        return jr_error(rd, num, &where, "%s is not a non-negative integer", field);
    }
    odin3_value val = odin3_value_int(jr_int(num));
    return memory_attr(rd, odin3_json_key(mem), field, &val);
}

static odin3_status memory_attrs(jr *rd, const odin3_json_node *mem) {
    const odin3_json_node *attrs = jr_member(mem, "attributes");
    jr_where where = {"memories", odin3_json_key(mem), "attributes"};
    if (attrs == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(attrs) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, attrs, &where, "attributes is not an object");
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *attr = jr_first(attrs); st == ODIN3_OK && attr != NULL;
         attr = odin3_json_next(attr)) {
        char field[JR_NAME_BUF];
        odin3_value val;
        (void)snprintf(field, sizeof field, "attribute:%s", odin3_json_key(attr));
        st = jr_attr_value(rd, attr, &where, &val);
        if (st == ODIN3_OK) {
            st = memory_attr(rd, odin3_json_key(mem), field, &val);
        }
    }
    return st;
}

static odin3_status read_memory(jr *rd, const odin3_json_node *mem) {
    jr_where where = {"memories", odin3_json_key(mem), NULL};
    if (jr_kind(mem) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, mem, &where, "the memory is not an object");
    }
    jr_audit(rd, mem, MEMORY_KEYS, sizeof MEMORY_KEYS / sizeof MEMORY_KEYS[0]);
    static const char *const numbers[] = {"width", "size", "start_offset"};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sizeof numbers / sizeof numbers[0]; i++) {
        st = memory_number(rd, mem, numbers[i]);
    }
    return st == ODIN3_OK ? memory_attrs(rd, mem) : st;
}

odin3_status jr_read_memories(jr *rd, const odin3_json_node *obj) {
    if (obj == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(obj) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, obj, &(jr_where){"memories", NULL, NULL}, "memories is not an object");
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *mem = jr_first(obj); st == ODIN3_OK && mem != NULL;
         mem = odin3_json_next(mem)) {
        st = read_memory(rd, mem);
    }
    return st;
}
