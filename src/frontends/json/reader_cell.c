/*
 * reader_cell.c — cells (M-5 to M-8, M-11): type, parameters, connections, the node, its
 * attributes and the second-driver check.
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

static const char *const CELL_KEYS[] = {
    "hide_name", "type", "parameters", "attributes", "port_directions", "connections", "model"};

/* One cell being built. */
typedef struct jr_cell {
    const odin3_json_node *node;
    const odin3_json_node *params;
    const char *key;
    const char *type_name;
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    jr_where where;
    bool attr_params; /* parameters of a module or black box are attributes parameter:<NAME> */
} jr_cell;

static int find_port(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The member `member` of a cell for an error. */
static jr_where member_where(const jr_cell *cell, const char *path) {
    return (jr_where){cell->where.section, cell->where.key, path};
}

/* --- the type ------------------------------------------------------------------------------ */

static odin3_status cell_begin(jr *rd, const odin3_json_node *node, jr_cell *cell) {
    *cell = (jr_cell){
        .node = node, .key = odin3_json_key(node), .where = {"cells", odin3_json_key(node), NULL}};
    odin3_status audited = jr_audit(rd, node, CELL_KEYS, sizeof CELL_KEYS / sizeof CELL_KEYS[0]);
    if (audited != ODIN3_OK) {
        return audited;
    }
    const odin3_json_node *type = jr_member(node, "type");
    cell->type_name = jr_text(type, NULL);
    cell->params = jr_member(node, "parameters");
    if (cell->params != NULL && jr_kind(cell->params) != ODIN3_JSON_OBJECT) {
        jr_where here = member_where(cell, "parameters");
        return jr_error(rd, cell->params, &here, "parameters is not an object");
    }
    if (!jr_lookup_type(rd, cell->type_name, &cell->type)) {
        return jr_error(rd, type, &cell->where, "internal error: type '%s' is unknown",
                        cell->type_name);
    }
    cell->def = odin3_celltype_get(rd->design, cell->type);
    cell->attr_params = cell->def->n_params == 0 && (cell->def->gran == ODIN3_GRAN_MODULE ||
                                                     cell->def->gran == ODIN3_GRAN_BLACKBOX);
    return ODIN3_OK;
}

/* --- parameters (M-6) ---------------------------------------------------------------------- */

/* True for Yosys's positional parameter name of an unresolved cell: `$` and digits only. */
static bool positional_name(const char *name) {
    if (name[0] != '$' || name[1] == '\0') {
        return false;
    }
    for (const char *at = name + 1; *at != '\0'; at++) {
        if (*at < '0' || *at > '9') {
            return false;
        }
    }
    return true;
}

/* True when member is a positional parameter (`$1`, `$2`, ...) of a cell whose type has no
 * parameter of that name and is a hard cell, black box or module: Yosys writes `#(2)` of a cell
 * it could not resolve that way. It is kept as the attribute parameter:<NAME>, which the writer
 * writes back as the parameter (R-5). */
static bool positional_param(const jr_cell *cell, const odin3_json_node *member) {
    const char *name = odin3_json_key(member);
    odin3_granularity gran = cell->def->gran;
    bool opaque =
        gran == ODIN3_GRAN_HARD || gran == ODIN3_GRAN_BLACKBOX || gran == ODIN3_GRAN_MODULE;
    return opaque && positional_name(name) && jr_find_param(cell->def, name) < 0;
}

/* One parameter of the cell, typed by the registry. */
static odin3_status typed_param(jr *rd, const jr_cell *cell, const odin3_json_node *member) {
    char path[JR_NAME_BUF];
    jr_where here = member_where(cell, "parameters");
    odin3_status joined =
        jr_join(rd, &(jr_join_args){member, &here, "parameters.", odin3_json_key(member)}, path,
                sizeof path);
    if (joined != ODIN3_OK) {
        return joined;
    }
    here = member_where(cell, path);
    int at = jr_find_param(cell->def, odin3_json_key(member));
    if (at < 0 && positional_param(cell, member)) {
        return ODIN3_OK; /* an attribute, set once the node exists (param_attrs) */
    }
    if (at < 0) {
        return jr_error(rd, member, &here, "'%s' has no parameter '%s'", cell->type_name,
                        odin3_json_key(member));
    }
    odin3_value *vals = rd->cur.params.data;
    return jr_param_value(rd, cell->def->params[at].kind, member, &here, &vals[at]);
}

/* The defaults of the type, then the cell's parameters over them. */
static odin3_status cell_params(jr *rd, const jr_cell *cell) {
    odin3_vec_clear(&rd->cur.params);
    for (uint32_t i = 0; i < cell->def->n_params; i++) {
        odin3_value *val = odin3_vec_push(&rd->cur.params);
        if (val == NULL) {
            return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        *val = cell->def->params[i].dflt;
    }
    if (cell->attr_params) {
        return ODIN3_OK; /* set as attributes once the node exists */
    }
    if (strcmp(cell->def->name, "$sop") == 0 && cell->params != NULL &&
        jr_member(cell->params, "TABLE") != NULL) {
        jr_where here = member_where(cell, "parameters");
        return jr_sop_params(rd, cell->params, &here);
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *member = jr_first(cell->params); st == ODIN3_OK && member != NULL;
         member = odin3_json_next(member)) {
        st = typed_param(rd, cell, member);
    }
    return st;
}

/* The attributes parameter:<NAME> of an instance of a module or black box. */
static odin3_status param_attrs(jr *rd, const jr_cell *cell, odin3_node_id node) {
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *member = jr_first(cell->params); st == ODIN3_OK && member != NULL;
         member = odin3_json_next(member)) {
        if (!cell->attr_params && !positional_param(cell, member)) {
            continue;
        }
        char key[JR_NAME_BUF];
        char path[JR_NAME_BUF];
        const char *name = odin3_json_key(member);
        jr_where here = member_where(cell, "parameters");
        st = jr_join(rd, &(jr_join_args){member, &here, "parameter:", name}, key, sizeof key);
        if (st == ODIN3_OK) {
            st =
                jr_join(rd, &(jr_join_args){member, &here, "parameters.", name}, path, sizeof path);
        }
        here = member_where(cell, path);
        odin3_value val = {0};
        if (st == ODIN3_OK) {
            st = jr_attr_value(rd, member, &here, &val);
        }
        if (st == ODIN3_OK) {
            st = jr_set_attr_named(rd, rd->cur.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key,
                                   &val);
        }
    }
    return st;
}

/* M-11: INIT of a bit flip-flop or latch from the init attribute of the netname of its Q bit. */
static void apply_init(jr *rd, const jr_cell *cell) {
    int init = jr_find_param(cell->def, "INIT");
    const odin3_json_node *qconn = jr_member(jr_member(cell->node, "connections"), "Q");
    const odin3_json_node *bit = jr_first(qconn);
    uint64_t value = 0;
    if (init < 0 || cell->def->gran != ODIN3_GRAN_BIT || bit == NULL ||
        jr_kind(bit) != ODIN3_JSON_INT ||
        !odin3_u64map_get(rd->cur.qinit, (uint64_t)jr_int(bit), &value)) {
        return;
    }
    odin3_value *vals = rd->cur.params.data;
    if (vals[init].kind == ODIN3_VAL_INT) {
        vals[init] = odin3_value_int((int64_t)value);
    }
}

/* --- connections (M-7) --------------------------------------------------------------------- */

/* Widths of the ports for the cell's parameters, and the zeroed pin nets of the cell. */
static odin3_status size_ports(jr *rd, const jr_cell *cell) {
    odin3_vec_clear(&rd->cur.conn_nets);
    odin3_vec_clear(&rd->cur.conn_ports);
    uint64_t total = 0;
    for (uint32_t i = 0; i < cell->def->n_ports; i++) {
        uint32_t width = 0;
        odin3_port_query query = {cell->type, rd->cur.params.data, i};
        odin3_status st = odin3_celltype_port_width_checked(rd->design, &query, &width);
        total += width;
        if (st != ODIN3_OK || width > ODIN3_READER_MAX_WIDTH ||
            total > ODIN3_READER_MAX_TOTAL_WIDTH) {
            return st == ODIN3_ERR_NO_MEMORY
                       ? jr_fail(rd, st)
                       : jr_error(
                             rd, cell->node, &cell->where,
                             "port '%s' of '%s' has no valid width for these parameters (or the "
                             "cell is wider than the reader's limit)",
                             cell->def->ports[i].name, cell->type_name);
        }
        odin3_netvec *port = odin3_vec_push(&rd->cur.conn_ports);
        if (port == NULL) {
            return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        port->count = width;
    }
    return odin3_vec_reserve(&rd->cur.conn_nets, total + 1) == ODIN3_OK
               ? ODIN3_OK
               : jr_fail(rd, ODIN3_ERR_NO_MEMORY);
}

/* Gives each port its slice of the zeroed net array. */
static void slice_ports(jr *rd) {
    uint64_t total = 0;
    for (size_t i = 0; i < rd->cur.conn_ports.len; i++) {
        total += ((const odin3_netvec *)odin3_vec_cat(&rd->cur.conn_ports, i))->count;
    }
    for (uint64_t i = 0; i < total; i++) {
        (void)odin3_vec_push(&rd->cur.conn_nets); /* reserved: cannot fail */
    }
    odin3_net_id *at = rd->cur.conn_nets.data;
    for (size_t i = 0; i < rd->cur.conn_ports.len; i++) {
        odin3_netvec *port = odin3_vec_at(&rd->cur.conn_ports, i);
        port->nets = at;
        at += port->count;
    }
}

/* A constant on an output pin: allowed when it is x or z (the pin is open) or when the type's
 * own constant is that value (a $lut of a constant table: Odin III's writer writes its net as the
 * constant); the pin stays open then. */
static odin3_status output_const(jr *rd, const jr_cell *cell, const jr_bit *bit,
                                 const jr_where *where) {
    odin3_const own = cell->def->const_value != NULL ? cell->def->const_value(rd->cur.params.data)
                                                     : ODIN3_CONST_NONE;
    if (bit->kind == ODIN3_BIT_X || bit->kind == ODIN3_BIT_Z || (uint32_t)own == bit->kind + 1U) {
        return ODIN3_OK;
    }
    return jr_error(rd, cell->node, where, "a constant on the output port of a '%s'",
                    cell->type_name);
}

/* One connection: the bits of port `port` into its net slice. */
static odin3_status connect_port(jr *rd, const jr_cell *cell, const odin3_json_node *conn,
                                 uint32_t port) {
    odin3_netvec *vec = odin3_vec_at(&rd->cur.conn_ports, port);
    char path[JR_NAME_BUF];
    (void)snprintf(path, sizeof path, "connections.%s", odin3_json_key(conn));
    jr_where where = member_where(cell, path);
    uint32_t count = 0;
    odin3_status st = jr_bits_count(rd, conn, &where, true, &count);
    if (st == ODIN3_OK && count != vec->count) {
        return jr_error(rd, conn, &where,
                        "%u bits, but the port '%s' of '%s' is %u bits wide for these parameters",
                        (unsigned)count, odin3_json_key(conn), cell->type_name,
                        (unsigned)vec->count);
    }
    odin3_net_id *slot = (odin3_net_id *)vec->nets;
    uint32_t k = 0;
    for (const odin3_json_node *elem = jr_first(conn); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem), k++) {
        jr_bit bit = {0, 0};
        st = jr_bit_parse(rd, elem, &where, k, &bit);
        bool is_out = cell->def->ports[port].dir == ODIN3_DIR_OUT;
        if (st == ODIN3_OK && is_out && bit.kind != JR_BIT_NET) {
            st = output_const(rd, cell, &bit, &where);
        } else if (st == ODIN3_OK) {
            slot[k] = jr_net_of(rd, &bit);
        }
    }
    return st;
}

static const char *dir_text(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "input";
    }
    return dir == ODIN3_DIR_OUT ? "output" : "inout";
}

/* E-6: a direction of port_directions must be the registry's. */
static odin3_status check_directions(const jr *rd, const jr_cell *cell) {
    const odin3_json_node *dirs = jr_member(cell->node, "port_directions");
    if (dirs == NULL) {
        return ODIN3_OK;
    }
    jr_where here = member_where(cell, "port_directions");
    if (jr_kind(dirs) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, dirs, &here, "port_directions is not an object");
    }
    for (const odin3_json_node *dir = jr_first(dirs); dir != NULL; dir = odin3_json_next(dir)) {
        int port = find_port(cell->def, odin3_json_key(dir));
        const char *text = jr_kind(dir) == ODIN3_JSON_STRING ? jr_text(dir, NULL) : "";
        char path[JR_NAME_BUF];
        (void)snprintf(path, sizeof path, "port_directions.%s", odin3_json_key(dir));
        jr_where there = member_where(cell, path);
        if (port < 0) {
            return jr_error(rd, dir, &there, "'%s' has no port '%s'", cell->type_name,
                            odin3_json_key(dir));
        }
        const char *want = dir_text(cell->def->ports[port].dir);
        if (strcmp(text, want) != 0) {
            return jr_error(rd, dir, &there, "direction '%s', but the port '%s' of '%s' is an %s",
                            text, odin3_json_key(dir), cell->type_name, want);
        }
    }
    return ODIN3_OK;
}

static odin3_status connect_all(jr *rd, const jr_cell *cell) {
    const odin3_json_node *conns = jr_member(cell->node, "connections");
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(conns); st == ODIN3_OK && conn != NULL;
         conn = odin3_json_next(conn)) {
        int port = find_port(cell->def, odin3_json_key(conn));
        if (port < 0) {
            char path[JR_NAME_BUF];
            (void)snprintf(path, sizeof path, "connections.%s", odin3_json_key(conn));
            jr_where here = member_where(cell, path);
            return jr_error(rd, conn, &here, "'%s' has no port '%s'", cell->type_name,
                            odin3_json_key(conn));
        }
        st = connect_port(rd, cell, conn, (uint32_t)port);
    }
    return st;
}

/* --- the node ------------------------------------------------------------------------------ */

/* E-8: the node just made must not give a net a second ordinary driver (check rule 4). */
static odin3_status check_drivers(jr *rd, const jr_cell *cell, odin3_node_id node) {
    odin3_module *module = rd->cur.module;
    odin3_pinslice pins = odin3_node_pins(module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(module, pin);
        if (!odin3_net_valid(net) || !odin3_pin_drives(module, pin) || jr_is_bus_driver(rd, pin)) {
            continue;
        }
        uint32_t count = jr_ordinary_drivers(rd, net);
        if (count >= 2) {
            char path[JR_NAME_BUF];
            (void)snprintf(path, sizeof path, "connections.%s[%u]",
                           cell->def->ports[odin3_pin_port(module, pin)].name,
                           (unsigned)odin3_pin_bit(module, pin));
            jr_where here = member_where(cell, path);
            odin3_status st =
                jr_error(rd, cell->node, &here, "net is driven by %u cell outputs (one allowed)",
                         (unsigned)count);
            (void)odin3_node_delete(module, node);
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status create_node(jr *rd, const jr_cell *cell, odin3_node_id *node) {
    odin3_node_spec spec = {.type = cell->type,
                            .params = cell->def->n_params > 0 ? rd->cur.params.data : NULL,
                            .n_params = cell->def->n_params};
    odin3_prov_id imported = {0};
    jr_named named = {0};
    odin3_status st = jr_name_key(rd, cell->node, JR_NAME_NODE, &named);
    spec.name = named.name;
    st = st == ODIN3_OK ? jr_prov(rd, cell->node, &imported) : st;
    if (st == ODIN3_OK) {
        st = jr_restore_src(
            rd, &(jr_restore_in){jr_member(cell->node, "attributes"), imported, true, &named},
            &spec.prov);
    }
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(rd->cur.module, &spec, rd->cur.conn_ports.data, node);
        if (st == ODIN3_ERR_INVALID_ARG) {
            return jr_error(rd, cell->node, &cell->where,
                            "the cell of type '%s' is refused (see the message before this one)",
                            cell->type_name);
        }
        st = jr_fail(rd, st);
    }
    return st;
}

/* The type after a `$lut` of no inputs became a `$sop`. */
static odin3_status retype_lut0(jr *rd, jr_cell *cell) {
    odin3_celltype_id sop = {0};
    bool converted = false;
    if (strcmp(cell->def->name, "$lut") != 0 || cell->attr_params) {
        return ODIN3_OK;
    }
    if (!jr_lookup_type(rd, "$sop", &sop)) {
        return jr_error(rd, cell->node, &cell->where, "internal error: no $sop cell type");
    }
    const jr_lut_sop defs = {cell->def, odin3_celltype_get(rd->design, sop)};
    odin3_status st = jr_lut0_params(rd, &defs, &converted);
    if (st == ODIN3_OK && converted) {
        cell->type = sop;
        cell->def = defs.sop;
    }
    return st;
}

static odin3_status build_cell(jr *rd, const odin3_json_node *node) {
    jr_cell cell;
    odin3_status st = cell_begin(rd, node, &cell);
    odin3_node_id made = {0};
    if (st == ODIN3_OK) {
        st = cell_params(rd, &cell);
    }
    if (st == ODIN3_OK) {
        st = retype_lut0(rd, &cell);
    }
    if (st == ODIN3_OK) {
        apply_init(rd, &cell);
        st = size_ports(rd, &cell);
    }
    if (st == ODIN3_OK) {
        slice_ports(rd);
        st = check_directions(rd, &cell);
    }
    if (st == ODIN3_OK) {
        st = connect_all(rd, &cell);
    }
    if (st == ODIN3_OK) {
        st = create_node(rd, &cell, &made);
    }
    if (st == ODIN3_OK) {
        st = param_attrs(rd, &cell, made);
    }
    if (st == ODIN3_OK) {
        st = jr_set_attrs(rd, rd->cur.module, (odin3_objref){ODIN3_OBJ_NODE, made.v},
                          jr_member(node, "attributes"), &cell.where);
    }
    if (st == ODIN3_OK) {
        st = jr_apply_ext(rd, rd->cur.module, (odin3_objref){ODIN3_OBJ_NODE, made.v});
    }
    return st == ODIN3_OK ? check_drivers(rd, &cell, made) : st;
}

odin3_status jr_read_cells(jr *rd, const odin3_json_node *obj) {
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *cell = jr_first(obj); st == ODIN3_OK && cell != NULL;
         cell = odin3_json_next(cell)) {
        const odin3_json_node *type = jr_member(cell, "type");
        if (strcmp(jr_text(type, NULL), "$scopeinfo") != 0) {
            st = build_cell(rd, cell);
        }
    }
    return st;
}
