/* writer.c — Yosys-schema JSON netlist writer: file, modules, ports and netnames. */
#include "backends/json/writer.h"

#include "backends/json/jw.h"
#include "ir/celltype.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <errno.h>
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

static void write_wire_netname(jw *out, jw_list *names, odin3_wire_id wire) {
    uint32_t name = odin3_wire_name(out->module, wire);
    jw_list fields = {false, FIELD_DEPTH};
    netname_bits bits = {NULL, wire, odin3_wire_width(out->module, wire)};
    const char *text = odin3_strtab_get(out->strtab, name);
    jw_record_key(out, text != NULL ? text : "");
    jw_item(out, names);
    jw_str_id(out, name);
    jw_raw(out, ": ");
    jw_open(out, &fields);
    jw_key(out, &fields, "hide_name");
    jw_raw(out, name_hidden(out, name) ? "1" : "0");
    jw_key(out, &fields, "bits");
    write_wire_bits(out, wire);
    write_wire_shape(out, &fields, wire);
    write_attrs(out, &fields, &bits, (odin3_objref){ODIN3_OBJ_WIRE, wire.v});
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
    jw_keyspec spec = {JW_KEY_NETNAME, entry->clash, odin3_strtab_get(out->strtab, entry->name),
                       entry->clash ? tail : ""};
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
    jw_raw(out, key[0] == '$' ? "1" : "0");
    jw_key(out, &fields, "bits");
    jw_raw(out, "[ ");
    jw_bit(out, jw_net_code(out, entry->net));
    jw_raw(out, " ]");
    write_attrs(out, &fields, &bits, (odin3_objref){ODIN3_OBJ_NET, entry->net.v});
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
    jw_user_attrs(out, &attrs, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(out->module).v},
                  ODIN3_WATTR_ATTRIBUTE);
    jw_close(out, &attrs);
    write_ports(out, entry);
    jw_key(out, entry, "cells");
    jw_open(out, &cells);
    jw_cells(out, &cells);
    jw_close(out, &cells);
    write_netnames(out, entry);
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
    jw_item(out, modules);
    jw_str_id(out, odin3_module_name(mod));
    jw_raw(out, ": ");
    jw_open(out, &entry);
    write_module_body(out, &entry);
    jw_close(out, &entry);
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

/* A port's width; a width parameter or function is evaluated at the type's defaults. */
static uint32_t blackbox_port_width(jw *out, odin3_celltype_id type, uint32_t port) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    if (def->ports[port].width_param == NULL && def->ports[port].width_fn == NULL &&
        def->ports[port].width_expr == NULL) {
        return def->ports[port].width;
    }
    odin3_value *params = odin3_util_calloc(sizeof(odin3_value) * (def->n_params + 1));
    if (params == NULL) {
        out->status = out->status != ODIN3_OK ? out->status : ODIN3_ERR_NO_MEMORY;
        return 0;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
    }
    uint32_t width = odin3_celltype_port_width(out->design, type, params, port);
    odin3_util_free(params);
    return width;
}

static void write_blackbox_ports(jw *out, jw_list *entry, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
    jw_list ports = {false, ENTRY_DEPTH};
    uint32_t bit = 2;
    jw_key(out, entry, "ports");
    jw_open(out, &ports);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        jw_list fields = {false, FIELD_DEPTH};
        uint32_t width = blackbox_port_width(out, type, i);
        jw_key(out, &ports, def->ports[i].name);
        jw_open(out, &fields);
        jw_key(out, &fields, "direction");
        jw_string(out, odin3_bytes_cstr(dir_text(def->ports[i].dir)));
        jw_key(out, &fields, "bits");
        jw_raw(out, "[");
        for (uint32_t j = 0; j < width; j++) {
            jw_raw(out, j > 0 ? ", " : " ");
            jw_bit(out, bit++);
        }
        jw_raw(out, width > 0 ? " ]" : "]");
        jw_close(out, &fields);
    }
    jw_close(out, &ports);
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

/* Declared models once each, in declaration order; `$` names are Yosys internal cells. */
static void write_blackboxes(jw *out, jw_list *modules) {
    const uint32_t count = odin3_design_declared_model_count(out->design);
    for (uint32_t i = 0; i < count && out->status == ODIN3_OK; i++) {
        odin3_celltype_id type = odin3_design_declared_model(out->design, i);
        const odin3_celltype_def *def = odin3_celltype_get(out->design, type);
        bool first = true;
        for (uint32_t j = 0; j < i; j++) {
            first = first && odin3_design_declared_model(out->design, j).v != type.v;
        }
        if (first && def != NULL && def->name[0] != '$' &&
            (def->gran == ODIN3_GRAN_BLACKBOX || def->gran == ODIN3_GRAN_HARD)) {
            write_blackbox(out, modules, type);
        }
    }
}

static odin3_status write_design(jw *out) {
    jw_list top = {false, 1};
    jw_list modules = {false, MODULE_DEPTH};
    odin3_status status = ODIN3_OK;
    jw_open(out, &top);
    jw_key(out, &top, "creator");
    jw_raw(out, "\"Odin III\"");
    jw_key(out, &top, "modules");
    jw_open(out, &modules);
    const uint32_t end = odin3_design_module_end(out->design);
    for (uint32_t i = 1; i < end && status == ODIN3_OK; i++) {
        odin3_module *mod = odin3_module_get(out->design, (odin3_module_id){i});
        status = mod != NULL ? write_module(out, &modules, mod) : ODIN3_OK;
    }
    if (status == ODIN3_OK) {
        write_blackboxes(out, &modules);
    }
    jw_close(out, &modules);
    jw_close(out, &top);
    jw_raw(out, "\n");
    return status != ODIN3_OK ? status : out->status;
}

/* Writes a private temporary beside the destination, then renames it over the destination: a
 * failure leaves the destination as it was and removes only the temporary. */
static odin3_status save_design(jw *state, const char *path) {
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
    return odin3_atomic_file_close(&file, status);
}

odin3_status odin3_json_write(const odin3_design *design, const char *path) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_json_write: no design or no path");
        return ODIN3_ERR_INVALID_ARG;
    }
    /* odin3_module_get takes a mutable design; the writer never changes it. */
    jw state = {.design = (odin3_design *)design,
                .strtab = odin3_design_strtab(design),
                .status = ODIN3_OK};
    odin3_strbuf_init(&state.key);
    odin3_wattr_seen_init(&state.seen);
    odin3_status status = save_design(&state, path);
    odin3_wattr_seen_free(&state.seen);
    odin3_strbuf_free(&state.key);
    return status;
}
