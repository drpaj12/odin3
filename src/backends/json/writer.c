/* writer.c — Yosys-schema JSON netlist writer: file, modules, ports and netnames. */
#include "backends/json/writer.h"

#include "backends/json/jw.h"
#include "ir/celltype.h"
#include "ir/proc.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <string.h>

enum {
    MODULE_DEPTH = 2,
    SECTION_DEPTH = 3,
    ENTRY_DEPTH = 4,
    FIELD_DEPTH = 5,
    ATTR_DEPTH = 6,
    NAME_TAIL = 24
};

/* --- latch initial values ----------------------------------------------------------------------
 */

static int find_port(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_param(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* INIT 0/1 are defined; 2 (don't care) and 3 (unknown) are x. */
static uint8_t init_char(int64_t init) {
    if (init == 0 || init == 1) {
        return (uint8_t)('0' + init);
    }
    return 'x';
}

static void mark_node_init(jw *out, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(out->design, odin3_node_type(out->module, node));
    if (def == NULL || def->gran != ODIN3_GRAN_BIT) {
        return;
    }
    int param = find_param(def, "INIT");
    int port = find_port(def, "Q");
    const odin3_value *val =
        param < 0 ? NULL : odin3_node_param(out->module, node, (uint32_t)param);
    if (val == NULL || port < 0 || val->kind != ODIN3_VAL_INT) {
        return;
    }
    odin3_pinslice pins = odin3_node_port(out->module, node, (uint32_t)port);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(out->module, pin);
        if (odin3_net_valid(net)) {
            out->init[net.v] = init_char(val->i);
        }
    }
}

static void mark_inits(jw *out) {
    const uint32_t end = odin3_module_node_end(out->module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(out->module, node)) {
            mark_node_init(out, node);
        }
    }
}

/* --- ports -------------------------------------------------------------------------------------
 */

static const char *port_direction(const jw *out, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(out->design, odin3_node_type(out->module, node));
    odin3_dir dir = def != NULL && def->n_ports > 0 ? def->ports[0].dir : ODIN3_DIR_INOUT;
    /* a $port_in pin drives the module net, so the module sees an input */
    if (dir == ODIN3_DIR_OUT) {
        return "input";
    }
    return dir == ODIN3_DIR_IN ? "output" : "inout";
}

/* offset / upto / signed members of a netname or port, from its wire. */
static void write_wire_shape(jw *out, jw_list *fields, odin3_wire_id wire) {
    int32_t msb = odin3_wire_msb(out->module, wire);
    int32_t lsb = odin3_wire_lsb(out->module, wire);
    int32_t low = msb < lsb ? msb : lsb;
    if (low != 0) {
        jw_key(out, fields, "offset");
        jw_fmt(out, "%d", (int)low);
    }
    if (msb < lsb) {
        jw_key(out, fields, "upto");
        jw_raw(out, "1");
    }
    if (odin3_wire_signed(out->module, wire)) {
        jw_key(out, fields, "signed");
        jw_raw(out, "1");
    }
}

static void write_wire_bits(jw *out, odin3_wire_id wire) {
    const uint32_t width = odin3_wire_width(out->module, wire);
    jw_raw(out, "[ ");
    for (uint32_t i = 0; i < width; i++) {
        if (i > 0) {
            jw_raw(out, ", ");
        }
        jw_bit(out, jw_net_code(out, odin3_wire_net(out->module, wire, i)));
    }
    jw_raw(out, " ]");
}

static void write_port(jw *out, jw_list *ports, uint32_t index) {
    odin3_node_id node = odin3_module_port(out->module, index);
    odin3_wire_id wire = odin3_module_port_wire(out->module, index);
    jw_list fields = {false, FIELD_DEPTH};
    jw_item(out, ports);
    jw_str_id(out, odin3_wire_name(out->module, wire));
    jw_raw(out, ": ");
    jw_open(out, &fields);
    jw_key(out, &fields, "direction");
    jw_string(out, odin3_bytes_cstr(port_direction(out, node)));
    jw_key(out, &fields, "bits");
    jw_pins(out, odin3_node_pins(out->module, node));
    write_wire_shape(out, &fields, wire);
    jw_close(out, &fields);
}

static void write_ports(jw *out, jw_list *section) {
    jw_list ports = {false, ENTRY_DEPTH};
    jw_key(out, section, "ports");
    jw_open(out, &ports);
    const uint32_t count = odin3_module_port_count(out->module);
    for (uint32_t i = 0; i < count; i++) {
        write_port(out, &ports, i);
    }
    jw_close(out, &ports);
}

/* --- netnames ----------------------------------------------------------------------------------
 */

static bool name_hidden(const jw *out, uint32_t name) {
    const char *text = odin3_strtab_get(out->strtab, name);
    return text != NULL && text[0] == '$';
}

/* The init attribute of a netname over the given nets: x for bits with no latch driver. */
typedef struct netname_bits {
    const odin3_net_id *nets; /* NULL: the wire's own nets */
    odin3_wire_id wire;
    uint32_t count;
} netname_bits;

static odin3_net_id bit_net(const jw *out, const netname_bits *bits, uint32_t index) {
    return bits->nets != NULL ? bits->nets[index] : odin3_wire_net(out->module, bits->wire, index);
}

static void write_init(jw *out, jw_list *attrs, const netname_bits *bits) {
    bool any = false;
    for (uint32_t i = 0; i < bits->count; i++) {
        odin3_net_id net = bit_net(out, bits, i);
        any = any || (odin3_net_valid(net) && out->init[net.v] != 0);
    }
    if (!any || !jw_claim(out, ODIN3_WATTR_ATTRIBUTE, "init")) {
        return;
    }
    jw_key(out, attrs, "init");
    jw_char(out, '"');
    for (uint32_t i = bits->count; i > 0; i--) {
        odin3_net_id net = bit_net(out, bits, i - 1);
        uint8_t chr = odin3_net_valid(net) ? out->init[net.v] : 0;
        jw_char(out, chr != 0 ? chr : 'x');
    }
    jw_char(out, '"');
}

/* init (from the latches), the object's own attributes, then src (unless the object has one). */
static void write_attrs(jw *out, jw_list *entry, const netname_bits *bits, odin3_objref obj) {
    jw_list attrs = {false, ATTR_DEPTH};
    jw_key(out, entry, "attributes");
    jw_open(out, &attrs);
    odin3_wattr_seen_clear(&out->seen);
    write_init(out, &attrs, bits);
    jw_user_attrs(out, &attrs, obj, ODIN3_WATTR_ATTRIBUTE);
    jw_src(out, &attrs,
           obj.kind == ODIN3_OBJ_WIRE ? odin3_wire_prov(out->module, (odin3_wire_id){obj.id})
                                      : odin3_net_prov(out->module, (odin3_net_id){obj.id}));
    jw_close(out, &attrs);
}

/* The key of a non-port wire's netname under provenance or hash names (names.c), NULL when the
 * wire keeps its name: a port, or no HDL location. */
static const char *wire_key(jw *out, odin3_wire_id wire, const char *text) {
    if (out->names == JW_NAMES_SHORT || text == NULL ||
        odin3_node_valid(odin3_wire_port_node(out->module, wire))) {
        return NULL;
    }
    jw_keyspec spec = {JW_KEY_NETNAME,  false, text, "", odin3_wire_prov(out->module, wire),
                       ODIN3_NAME_WIRE, wire.v};
    return jw_prov_key(out, &spec);
}

static void write_wire_netname(jw *out, jw_list *names, odin3_wire_id wire) {
    uint32_t name = odin3_wire_name(out->module, wire);
    jw_list fields = {false, FIELD_DEPTH};
    netname_bits bits = {NULL, wire, odin3_wire_width(out->module, wire)};
    const char *text = odin3_strtab_get(out->strtab, name);
    const char *renamed = wire_key(out, wire, text);
    bool hidden = renamed != NULL ? out->hide : name_hidden(out, name);
    if (renamed == NULL) {
        jw_record_key(out, text != NULL ? text : "");
    }
    jw_item(out, names);
    if (renamed != NULL) {
        jw_string(out, odin3_bytes_cstr(renamed));
    } else {
        jw_str_id(out, name);
    }
    jw_raw(out, ": ");
    jw_open(out, &fields);
    jw_key(out, &fields, "hide_name");
    jw_raw(out, hidden ? "1" : "0");
    jw_key(out, &fields, "bits");
    write_wire_bits(out, wire);
    write_wire_shape(out, &fields, wire);
    write_attrs(out, &fields, &bits, (odin3_objref){ODIN3_OBJ_WIRE, wire.v});
    jw_odin3_keys(out, &fields, (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                  odin3_wire_prov(out->module, wire));
    jw_close(out, &fields);
}

/* A one-bit netname for a net: its own name, a bare alias, or a generated $n<ID>. */
typedef struct net_entry {
    odin3_net_id net;
    uint32_t name; /* 0: generated */
    bool clash;    /* the name is also a wire's: disambiguate with $net<ID> */
} net_entry;

static void write_net_netname(jw *out, jw_list *names, const net_entry *entry) {
    char tail[NAME_TAIL];
    jw_list fields = {false, FIELD_DEPTH};
    netname_bits bits = {&entry->net, (odin3_wire_id){0}, 1};
    (void)snprintf(tail, sizeof tail, "%s%u", entry->clash ? "$net" : "", (unsigned)entry->net.v);
    jw_keyspec spec = {JW_KEY_NETNAME,
                       entry->clash,
                       odin3_strtab_get(out->strtab, entry->name),
                       entry->clash ? tail : "",
                       odin3_net_prov(out->module, entry->net),
                       ODIN3_NAME_NET,
                       entry->net.v};
    if (entry->name == 0) {
        spec.generated = true;
        spec.head = "$n";
        spec.tail = tail;
    }
    const char *key = jw_make_key(out, &spec);
    jw_item(out, names);
    jw_string(out, odin3_bytes_cstr(key));
    jw_raw(out, ": ");
    jw_open(out, &fields);
    jw_key(out, &fields, "hide_name");
    jw_raw(out, out->hide ? "1" : "0");
    jw_key(out, &fields, "bits");
    jw_raw(out, "[ ");
    jw_bit(out, jw_net_code(out, entry->net));
    jw_raw(out, " ]");
    write_attrs(out, &fields, &bits, (odin3_objref){ODIN3_OBJ_NET, entry->net.v});
    jw_odin3_keys(out, &fields, (odin3_objref){ODIN3_OBJ_NET, entry->net.v},
                  odin3_net_prov(out->module, entry->net));
    jw_close(out, &fields);
}

/* Whether net's name `name` needs a netname entry of its own, and whether it clashes with a wire.
 */
typedef enum name_use { NAME_SKIP, NAME_PLAIN, NAME_CLASH } name_use;

static name_use classify(const jw *out, odin3_net_id net, uint32_t name) {
    odin3_wire_id wire = odin3_module_find_wire(out->module, name);
    if (!odin3_wire_valid(wire)) {
        return NAME_PLAIN;
    }
    return odin3_net_primary(out->module, net).wire.v == wire.v ? NAME_SKIP : NAME_CLASH;
}

static void write_named(jw *out, jw_list *names, net_entry entry) {
    name_use use = classify(out, entry.net, entry.name);
    entry.clash = use == NAME_CLASH;
    if (use != NAME_SKIP) {
        write_net_netname(out, names, &entry);
    }
}

/* Names a net is known by besides its wire: own name, bare aliases; generated if init needs one. */
static void write_one_net(jw *out, jw_list *names, odin3_net_id net) {
    uint32_t own = odin3_net_name(out->module, net);
    bool named = own != 0 || odin3_wire_valid(odin3_net_primary(out->module, net).wire);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    if (own != 0) {
        write_named(out, names, (net_entry){net, own, false});
    }
    while (odin3_net_alias_next(out->module, net, &cursor, &alias)) {
        named = true;
        if (!odin3_wire_valid(alias.wb.wire) && alias.name != 0) {
            write_named(out, names, (net_entry){net, alias.name, false});
        }
    }
    if (!named && out->init[net.v] != 0) {
        write_net_netname(out, names, &(net_entry){net, 0, false});
    }
}

static void write_netnames(jw *out, jw_list *section) {
    jw_list names = {false, ENTRY_DEPTH};
    jw_key(out, section, "netnames");
    jw_open(out, &names);
    const uint32_t wires = odin3_module_wire_end(out->module);
    for (uint32_t i = 1; i < wires; i++) {
        odin3_wire_id wire = {i};
        if (odin3_wire_live(out->module, wire)) {
            write_wire_netname(out, &names, wire);
        }
    }
    const uint32_t nets = odin3_module_net_end(out->module);
    for (uint32_t i = 1; i < nets; i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(out->module, net)) {
            write_one_net(out, &names, net);
        }
    }
    jw_close(out, &names);
}

/* --- modules and file ----------------------------------------------------------------------------
 */

static void write_module_body(jw *out, jw_list *entry) {
    jw_list cells = {false, ENTRY_DEPTH};
    jw_list attrs = {false, ENTRY_DEPTH};
    jw_key(out, entry, "attributes");
    jw_open(out, &attrs);
    odin3_wattr_seen_clear(&out->seen);
    if (odin3_module_id_of(out->module).v == odin3_design_top(out->design).v &&
        jw_claim(out, ODIN3_WATTR_ATTRIBUTE, "top")) {
        jw_param_int(out, &attrs, "top", 1); /* the reader's unique-root rule cannot see it */
    }
    jw_user_attrs(out, &attrs, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(out->module).v},
                  ODIN3_WATTR_ATTRIBUTE);
    jw_src(out, &attrs, odin3_module_prov(out->module));
    jw_close(out, &attrs);
    write_ports(out, entry);
    jw_key(out, entry, "cells");
    jw_open(out, &cells);
    jw_cells(out, &cells);
    jw_close(out, &cells);
    write_netnames(out, entry);
    jw_odin3_keys(out, entry, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(out->module).v},
                  odin3_module_prov(out->module));
}

static odin3_status write_module(jw *out, jw_list *modules, odin3_module *mod) {
    const uint32_t net_end = odin3_module_net_end(mod);
    jw_list entry = {false, SECTION_DEPTH};
    out->module = mod;
    out->init = odin3_util_calloc((size_t)net_end + 1);
    if (out->init == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    out->written = odin3_strtab_create();
    if (out->written == NULL) {
        odin3_util_free(out->init);
        out->init = NULL;
        return ODIN3_ERR_NO_MEMORY;
    }
    out->extra_bit = net_end + 1;
    mark_inits(out);
    out->module_rows = odin3_hash_rows_count(&out->rows);
    jw_item(out, modules);
    jw_str_id(out, odin3_module_name(mod));
    jw_raw(out, ": ");
    jw_open(out, &entry);
    write_module_body(out, &entry);
    jw_close(out, &entry);
    jw_names_module_end(out);
    odin3_util_free(out->init);
    out->init = NULL;
    odin3_strtab_destroy(out->written);
    out->written = NULL;
    return out->status;
}

/* --- declared black boxes ------------------------------------------------------------------- */

static const char *dir_text(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "input";
    }
    return dir == ODIN3_DIR_OUT ? "output" : "inout";
}

/* A walk over the live nodes of every module, in module then node order. */
typedef struct node_cursor {
    odin3_module *mod;
    uint32_t mod_idx;
    uint32_t node_idx;
} node_cursor;

static bool cursor_next(const jw *out, node_cursor *cur) {
    for (;;) {
        while (cur->mod != NULL && cur->node_idx + 1 < odin3_module_node_end(cur->mod)) {
            cur->node_idx++;
            if (odin3_node_live(cur->mod, (odin3_node_id){cur->node_idx})) {
                return true;
            }
        }
        cur->mod_idx++;
        if (cur->mod_idx >= odin3_design_module_end(out->design)) {
            return false;
        }
        cur->mod = odin3_module_get(out->design, (odin3_module_id){cur->mod_idx});
        cur->node_idx = 0;
    }
}

/* One live instance of a type, with where it is. */
typedef struct bb_instance {
    const odin3_module *mod;
    odin3_node_id node;
} bb_instance;

/* The text of a strtab ID, or "?" for none. */
static const char *name_text(const jw *out, uint32_t str) {
    return str != 0 ? odin3_strtab_get(out->strtab, str) : "?";
}

/* The widths of every port of type for the parameters of one instance (the type's defaults where
 * the instance has none); inst NULL: the defaults alone. */
static void instance_widths(const jw *out, odin3_celltype_id type, const bb_instance *inst,
                            odin3_value *params, uint32_t *widths) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_value *val = inst != NULL ? odin3_node_param(inst->mod, inst->node, i) : NULL;
        params[i] = val != NULL ? *val : def->params[i].dflt;
    }
    for (uint32_t i = 0; i < def->n_ports; i++) {
        widths[i] = odin3_celltype_port_width(out->design, type, params, i);
    }
}

/* Reports two instances of one type whose resolved port widths differ: the types of one module
 * cannot have both (read_json would reject the file), so nothing is written. */
static void report_width_clash(jw *out, odin3_celltype_id type, const bb_instance *first,
                               const bb_instance *other, uint32_t port) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    odin3_log(ODIN3_LOG_ERROR,
              "write_json: black box '%s' is used with different port widths (port '%s'): "
              "instance '%s' in module '%s' and instance '%s' in module '%s'; one module cannot "
              "describe both",
              def->name, def->ports[port].name,
              name_text(out, odin3_node_name(first->mod, first->node)),
              name_text(out, odin3_module_name(first->mod)),
              name_text(out, odin3_node_name(other->mod, other->node)),
              name_text(out, odin3_module_name(other->mod)));
    out->status = out->status != ODIN3_OK ? out->status : ODIN3_ERR_INVALID_ARG;
}

/* The first port whose width differs between two width lists, or n_ports when none. */
static uint32_t first_width_diff(const uint32_t *lhs, const uint32_t *rhs, uint32_t n_ports) {
    uint32_t port = 0;
    while (port < n_ports && lhs[port] == rhs[port]) {
        port++;
    }
    return port;
}

/* The port widths of the module written for type: those of its first live instance, which every
 * other instance must share (else a located error, out->status set). Without an instance: the
 * type's defaults. widths and scratch hold n_ports entries, params n_params. */
static void blackbox_widths(jw *out, odin3_celltype_id type, odin3_value *params,
                            uint32_t *widths, uint32_t *scratch) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    node_cursor cur = {NULL, 0, 0};
    bb_instance first = {NULL, {0}};
    while (out->status == ODIN3_OK && cursor_next(out, &cur)) {
        bb_instance inst = {cur.mod, {cur.node_idx}};
        if (odin3_node_type(cur.mod, inst.node).v != type.v) {
            continue;
        }
        if (first.mod == NULL) {
            first = inst;
            instance_widths(out, type, &first, params, widths);
            continue;
        }
        instance_widths(out, type, &inst, params, scratch);
        uint32_t port = first_width_diff(widths, scratch, def->n_ports);
        if (port < def->n_ports) {
            report_width_clash(out, type, &first, &inst, port);
        }
    }
    if (first.mod == NULL) {
        instance_widths(out, type, NULL, params, widths);
    }
}

static void write_blackbox_ports(jw *out, jw_list *entry, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    odin3_value *params = odin3_util_calloc(sizeof(odin3_value) * (def->n_params + 1));
    uint32_t *widths = odin3_util_calloc(sizeof(uint32_t) * 2 * (def->n_ports + 1));
    if (params == NULL || widths == NULL) {
        out->status = out->status != ODIN3_OK ? out->status : ODIN3_ERR_NO_MEMORY;
    } else {
        blackbox_widths(out, type, params, widths, widths + def->n_ports + 1);
    }
    jw_list ports = {false, ENTRY_DEPTH};
    uint32_t bit = 2;
    jw_key(out, entry, "ports");
    jw_open(out, &ports);
    for (uint32_t i = 0; out->status == ODIN3_OK && i < def->n_ports; i++) {
        jw_list fields = {false, FIELD_DEPTH};
        jw_key(out, &ports, def->ports[i].name);
        jw_open(out, &fields);
        jw_key(out, &fields, "direction");
        jw_string(out, odin3_bytes_cstr(dir_text(def->ports[i].dir)));
        jw_key(out, &fields, "bits");
        jw_raw(out, "[");
        for (uint32_t j = 0; j < widths[i]; j++) {
            jw_raw(out, j > 0 ? ", " : " ");
            jw_bit(out, bit++);
        }
        jw_raw(out, widths[i] > 0 ? " ]" : "]");
        jw_close(out, &fields);
    }
    jw_close(out, &ports);
    odin3_util_free(params);
    odin3_util_free(widths);
}

/* A declared black-box or hard model (BLIF `.blackbox`) as a Yosys blackbox module: ports with
 * fresh bits, no cells (Yosys write_json writes black boxes the same way). */
static void write_blackbox(jw *out, jw_list *modules, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    jw_list entry = {false, SECTION_DEPTH};
    jw_list attrs = {false, ENTRY_DEPTH};
    jw_list empty = {false, ENTRY_DEPTH};
    jw_key(out, modules, def->name);
    jw_open(out, &entry);
    jw_key(out, &entry, "attributes");
    jw_open(out, &attrs);
    jw_param_int(out, &attrs, "blackbox", 1);
    jw_close(out, &attrs);
    write_blackbox_ports(out, &entry, type);
    jw_key(out, &entry, "cells");
    jw_open(out, &empty);
    jw_close(out, &empty);
    jw_key(out, &entry, "netnames");
    jw_open(out, &empty);
    jw_close(out, &empty);
    jw_close(out, &entry);
}

/* Declared models once each, in declaration order (a `$` name declared here is an implicit black
 * box read back from a file; the registered Yosys internal cells are no black boxes). */
static void write_blackboxes(jw *out, jw_list *modules) {
    const uint32_t count = odin3_design_declared_model_count(out->design);
    for (uint32_t i = 0; i < count && out->status == ODIN3_OK; i++) {
        odin3_celltype_id type = odin3_design_declared_model(out->design, i);
        const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
        bool first = true;
        for (uint32_t j = 0; j < i; j++) {
            first = first && odin3_design_declared_model(out->design, j).v != type.v;
        }
        if (first && def != NULL &&
            (def->gran == ODIN3_GRAN_BLACKBOX || def->gran == ODIN3_GRAN_HARD)) {
            write_blackbox(out, modules, type);
        }
    }
}

/* True when type is a declared model (write_blackboxes wrote it). */
static bool is_declared_model(const jw *out, odin3_celltype_id type) {
    const uint32_t count = odin3_design_declared_model_count(out->design);
    for (uint32_t i = 0; i < count; i++) {
        if (odin3_design_declared_model(out->design, i).v == type.v) {
            return true;
        }
    }
    return false;
}

enum { TYPE_LIST_FIRST = 8 };

/* The black-box types written so far. */
typedef struct type_list {
    uint32_t *ids;
    uint32_t count;
    uint32_t cap;
} type_list;

/* Adds type; false when it was already there, or (with *status set) on out of memory. */
static bool type_list_add(type_list *list, odin3_celltype_id type, odin3_status *status) {
    for (uint32_t i = 0; i < list->count; i++) {
        if (list->ids[i] == type.v) {
            return false;
        }
    }
    if (list->count == list->cap) {
        uint32_t cap = list->cap > 0 ? list->cap * 2 : TYPE_LIST_FIRST;
        uint32_t *grown = odin3_util_realloc(list->ids, cap * sizeof(uint32_t));
        if (grown == NULL) {
            *status = ODIN3_ERR_NO_MEMORY;
            return false;
        }
        list->ids = grown;
        list->cap = cap;
    }
    list->ids[list->count++] = type.v;
    return true;
}

/* An implicit black box: a design-local BLACKBOX type no `.blackbox` declared and no tech library
 * supplied (an undefined BLIF .subckt, or --auto-blackbox). A tech-library black box (it has
 * library data) is not: the library defines it again on read. Registered `$` cells are never
 * BLACKBOX. An undeclared HARD type is not written either (the library supplies it). */
static bool is_implicit_blackbox(const jw *out, odin3_celltype_id type,
                                 const odin3_celltype_def *def) {
    return def != NULL && def->gran == ODIN3_GRAN_BLACKBOX && !is_declared_model(out, type) &&
           odin3_celltype_lib(out->design, type) == NULL;
}

/* True when a module of the design is named name. */
static bool module_named(const jw *out, const char *name) {
    const uint32_t end = odin3_design_module_end(out->design);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_module *mod = odin3_module_get(out->design, (odin3_module_id){i});
        if (mod != NULL && strcmp(name_text(out, odin3_module_name(mod)), name) == 0) {
            return true;
        }
    }
    return false;
}

/* An implicit black box (BLIF .subckt of a type nothing defines, e.g. Yosys' `$_DFFSR_PPP_`) is a
 * design-local type that is no declared model: written from its instances, or the file would
 * not read back without --auto-blackbox. Each type once, in first-use order. */
static odin3_status write_implicit_blackboxes(jw *out, jw_list *modules) {
    type_list done = {NULL, 0, 0};
    odin3_status status = ODIN3_OK;
    node_cursor cur = {NULL, 0, 0};
    while (status == ODIN3_OK && cursor_next(out, &cur)) {
        odin3_celltype_id type = odin3_node_type(cur.mod, (odin3_node_id){cur.node_idx});
        const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
        if (is_implicit_blackbox(out, type, def) && type_list_add(&done, type, &status)) {
            if (module_named(out, def->name)) {
                odin3_log(ODIN3_LOG_ERROR,
                          "write_json: implicit black box '%s' has the name of a module", def->name);
                status = ODIN3_ERR_INVALID_ARG;
            } else {
                write_blackbox(out, modules, type);
            }
        }
    }
    odin3_util_free(done.ids);
    return status;
}

static odin3_status write_design(jw *out) {
    jw_list top = {false, 1};
    jw_list modules = {false, MODULE_DEPTH};
    odin3_status status = ODIN3_OK;
    jw_open(out, &top);
    jw_key(out, &top, "creator");
    jw_raw(out, "\"Odin III\"");
    if (out->odin3_keys) {
        jw_key(out, &top, "odin3_version");
        jw_raw(out, "1");
    }
    jw_key(out, &top, "modules");
    jw_open(out, &modules);
    const uint32_t end = odin3_design_module_end(out->design);
    for (uint32_t i = 1; i < end && status == ODIN3_OK; i++) {
        odin3_module *mod = odin3_module_get(out->design, (odin3_module_id){i});
        status = mod != NULL ? write_module(out, &modules, mod) : ODIN3_OK;
    }
    if (status == ODIN3_OK) {
        write_blackboxes(out, &modules);
        status = write_implicit_blackboxes(out, &modules);
    }
    jw_close(out, &modules);
    jw_close(out, &top);
    jw_raw(out, "\n");
    jw_names_done(out);
    return status != ODIN3_OK ? status : out->status;
}

/* Writes a private temporary beside the destination, then renames it over the destination: a
 * failure leaves the destination as it was and removes only the temporary. With a map (HASH) the
 * map is renamed first and then the netlist, both or neither (odin3_hash_map_close). */
static odin3_status save_design(jw *state, const char *path, odin3_hash_map_file *map) {
    odin3_atomic_file file;
    odin3_status status = odin3_atomic_file_open(&file, path);
    if (status != ODIN3_OK) {
        return status;
    }
    state->fp = file.fp;
    status = write_design(state);
    if (status == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "%s: write failed: %s", path, strerror(state->write_errno));
    }
    state->fp = NULL;
    if (map == NULL) {
        return odin3_atomic_file_close(&file, status);
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    status = status == ODIN3_OK ? odin3_hash_rows_map_text(&state->rows, &text) : status;
    status = odin3_hash_map_close(map, (odin3_bytes){text.data, text.len}, &file, status);
    odin3_strbuf_free(&text);
    return status;
}

/* HASH, the first pass: writes provenance names to a counting sink (no file) and collects the
 * renamed objects with their hash names (names.c). */
static odin3_status collect_names(jw *state) {
    state->fp = NULL;
    state->sunk = 0;
    state->names = JW_NAMES_COLLECT;
    odin3_status status = write_design(state);
    state->status = ODIN3_OK;
    return status;
}

/* HASH: the name map's temporary is opened first, so a map that cannot be written refuses the
 * write (H-1); then the two passes; then the map and the netlist are renamed into place. */
static odin3_status save_hashed(jw *state, const char *path, const char *name_map) {
    odin3_strbuf map_path;
    odin3_hash_map_file map = {.open = false};
    odin3_strbuf_init(&map_path);
    odin3_status status =
        odin3_hash_map_path(&(odin3_hash_map_req){"write_json", path, name_map}, &map_path);
    status = status == ODIN3_OK ? odin3_hash_map_open(&map, map_path.data) : status;
    status = status == ODIN3_OK ? collect_names(state) : status;
    if (status == ODIN3_OK) {
        state->names = JW_NAMES_APPLY;
        state->row_cursor = 0;
        status = save_design(state, path, &map);
    } else {
        status = odin3_hash_map_close(&map, (odin3_bytes){NULL, 0}, NULL, status);
    }
    odin3_strbuf_free(&map_path);
    return status;
}

odin3_status odin3_json_write(const odin3_design *design, const char *path) {
    return odin3_json_write_with(design, path, NULL);
}

odin3_status odin3_json_write_with(const odin3_design *design, const char *path,
                                   const odin3_json_write_opts *opts) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_json_write: no design or no path");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_json_name_style style = opts != NULL ? opts->name_style : ODIN3_JSON_NAMES_SHORT;
    if (style != ODIN3_JSON_NAMES_SHORT && style != ODIN3_JSON_NAMES_PROVENANCE &&
        style != ODIN3_JSON_NAMES_HASH) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_json_write: unknown name style");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_refuse_processes(design, NULL) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG; /* 2D EL-33: before the path is opened */
    }
    /* odin3_module_get takes a mutable design; the writer never changes it. */
    jw state = {.design = (odin3_design *)design,
                .strtab = odin3_design_strtab(design),
                .status = ODIN3_OK,
                .names = style == ODIN3_JSON_NAMES_SHORT ? JW_NAMES_SHORT : JW_NAMES_PROVENANCE,
                .odin3_keys = opts != NULL && opts->odin3_keys};
    (void)odin3_strtab_find(state.strtab, odin3_bytes_cstr("odin3_level"), &state.level_key);
    odin3_strbuf_init(&state.key);
    odin3_strbuf_init(&state.src);
    odin3_strbuf_init(&state.pname);
    odin3_strbuf_init(&state.pshort);
    odin3_hash_rows_init(&state.rows);
    odin3_wattr_seen_init(&state.seen);
    state.hdl = odin3_hdl_ctx_create(design, "write_json");
    odin3_status status = state.hdl != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (status == ODIN3_OK && style == ODIN3_JSON_NAMES_HASH) {
        status = save_hashed(&state, path, opts->name_map);
    } else if (status == ODIN3_OK) {
        status = save_design(&state, path, NULL);
    }
    odin3_hdl_ctx_destroy(state.hdl);
    odin3_wattr_seen_free(&state.seen);
    odin3_hash_rows_free(&state.rows);
    odin3_strbuf_free(&state.pshort);
    odin3_strbuf_free(&state.pname);
    odin3_strbuf_free(&state.src);
    odin3_strbuf_free(&state.key);
    return status;
}
