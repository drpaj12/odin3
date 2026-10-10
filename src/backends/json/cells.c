/* cells.c — the "cells" section of the Yosys JSON writer: cell framing and generic cells. */
#include "backends/json/jw.h"
#include "ir/celltype.h"

#include <string.h>

enum { CELL_DEPTH_OFFSET = 1, MEMBER_DEPTH_OFFSET = 2, NAME_BUF = 40 };

static const char *dir_name(odin3_dir dir) {
    switch (dir) {
    case ODIN3_DIR_IN:
        return "input";
    case ODIN3_DIR_OUT:
        return "output";
    case ODIN3_DIR_INOUT:
    default:
        return "inout";
    }
}

void jw_dir(jw *out, jw_cell *cell, const char *name, odin3_dir dir) {
    jw_key(out, &cell->dirs, name);
    jw_string(out, odin3_bytes_cstr(dir_name(dir)));
}

/* Writes `,\n<indent>"key": {` at the cell's member depth and starts the members list. */
static void open_member(jw *out, const jw_cell *cell, const char *key, jw_list *members) {
    jw_raw(out, ",\n");
    jw_indent(out, cell->cells->depth + CELL_DEPTH_OFFSET);
    jw_string(out, odin3_bytes_cstr(key));
    jw_raw(out, ": ");
    members->depth = cell->cells->depth + MEMBER_DEPTH_OFFSET;
    jw_open(out, members);
}

/* The unique key of a cell: the node's name or $c<ID>, then suffix. */
static const char *cell_key(jw *out, const jw_cell_ref *ref) {
    char buf[NAME_BUF];
    uint32_t name = odin3_node_name(out->module, ref->node);
    jw_keyspec spec = {JW_KEY_CELL, ref->suffix[0] != '\0', odin3_strtab_get(out->strtab, name),
                       ref->suffix};
    if (name == 0) {
        (void)snprintf(buf, sizeof buf, "%u%s", (unsigned)ref->node.v, ref->suffix);
        spec.generated = true;
        spec.head = "$c";
        spec.tail = buf;
    }
    return jw_make_key(out, &spec);
}

void jw_cell_begin(jw *out, jw_list *cells, const jw_cell_ref *ref, jw_cell *cell) {
    const char *key = cell_key(out, ref);
    cell->cells = cells;
    jw_item(out, cells);
    jw_string(out, odin3_bytes_cstr(key));
    jw_raw(out, ": {\n");
    jw_indent(out, cells->depth + CELL_DEPTH_OFFSET);
    jw_fmt(out, "\"hide_name\": %d,\n", key[0] == '$' ? 1 : 0);
    jw_indent(out, cells->depth + CELL_DEPTH_OFFSET);
    jw_raw(out, "\"type\": ");
    jw_string(out, odin3_bytes_cstr(ref->type));
    jw_raw(out, ",\n");
    jw_indent(out, cells->depth + CELL_DEPTH_OFFSET);
    jw_raw(out, "\"parameters\": ");
    cell->params.depth = cells->depth + MEMBER_DEPTH_OFFSET;
    jw_open(out, &cell->params);
}

void jw_cell_attrs(jw *out, jw_cell *cell, odin3_node_id node) {
    jw_list attrs;
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    odin3_wattr_seen_clear(&out->seen);
    jw_user_attrs(out, &cell->params, obj, ODIN3_WATTR_PARAMETER);
    jw_close(out, &cell->params);
    open_member(out, cell, "attributes", &attrs);
    odin3_wattr_seen_clear(&out->seen);
    jw_user_attrs(out, &attrs, obj, ODIN3_WATTR_ATTRIBUTE);
    jw_src(out, &attrs, odin3_node_prov(out->module, node));
    jw_close(out, &attrs);
    open_member(out, cell, "port_directions", &cell->dirs);
}

void jw_cell_conns(jw *out, jw_cell *cell) {
    jw_close(out, &cell->dirs);
    open_member(out, cell, "connections", &cell->conns);
}

void jw_cell_end(jw *out, const jw_cell *cell) {
    jw_close(out, &cell->conns);
    jw_raw(out, "\n");
    jw_indent(out, cell->cells->depth);
    jw_raw(out, "}");
}

/* Bit-level storage cells keep INIT on the Q net (init attribute), not as a parameter. */
static bool param_is_net_init(const odin3_celltype_def *def, uint32_t index) {
    return strncmp(def->name, "$_", 2) == 0 && strcmp(def->params[index].name, "INIT") == 0;
}

static void write_params(jw *out, jw_list *params, odin3_node_id node,
                         const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_value *val = odin3_node_param(out->module, node, i);
        if (val != NULL && !param_is_net_init(def, i)) {
            jw_param(out, params, def->params[i].name, val);
        }
    }
}

static void write_generic(jw *out, jw_list *cells, odin3_node_id node,
                          const odin3_celltype_def *def) {
    jw_cell_ref ref = {node, "", def->name};
    jw_cell cell;
    jw_cell_begin(out, cells, &ref, &cell);
    write_params(out, &cell.params, node, def);
    jw_cell_attrs(out, &cell, node);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        jw_dir(out, &cell, def->ports[i].name, def->ports[i].dir);
    }
    jw_cell_conns(out, &cell);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        jw_key(out, &cell.conns, def->ports[i].name);
        jw_pins(out, odin3_node_port(out->module, node, i));
    }
    jw_cell_end(out, &cell);
}

/* A constant driver ($_CONST*_, a zero-input $sop): its nets are written as "0"/"1"/"x"/"z". */
static bool is_constant_cell(const jw *out, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(out->module, node);
    if (pins.count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(out->module, pin);
        if (odin3_pin_reads(out->module, pin) || !odin3_net_valid(net) ||
            odin3_net_const_value(out->module, net) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

void jw_cells(jw *out, jw_list *list) {
    const uint32_t end = odin3_module_node_end(out->module);
    for (uint32_t i = 1; i < end && out->status == ODIN3_OK; i++) {
        odin3_node_id node = {i};
        const odin3_celltype_def *def =
            odin3_celltype_get(out->design, odin3_node_type(out->module, node));
        if (!odin3_node_live(out->module, node) || def == NULL || def->gran == ODIN3_GRAN_PORT ||
            is_constant_cell(out, node)) {
            continue;
        }
        if (strcmp(def->name, "$sop") == 0) {
            jw_sop_cell(out, list, node);
        } else {
            write_generic(out, list, node, def);
        }
    }
}
