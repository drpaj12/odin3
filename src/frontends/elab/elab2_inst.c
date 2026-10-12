/*
 * elab2_inst.c — instances and gates in stage 2 (2D Task 19; EL-10, EL-11, EL-29, Q1-Q3): the node
 * of a module, a black box or a registered cell type with its ports resized to the port (I4,
 * Yosys's hierarchy rules), the parameters of a cell type, the attributes `parameter:<NAME>`, the
 * black-box cell type declared once per definition, and the primitives lowered as Yosys's simplify
 * does. Drivers (the claim of every output) are elab2_drive.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- the target of an instantiation ----------------------------------------------------------- */

/* What an INSTANTIATION instantiates: the cell type, and whether it is a module of the store. */
typedef struct inst_target {
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    bool is_module; /* an IR module of the store (not a black box or a registered type) */
} inst_target;

/* One port of the instance: its connection, what was generated for it and its pins' nets. */
typedef struct port_conn {
    odin3_ast_id conn, expr;
    odin3_sig in; /* an input's expression */
    bool in_const;
    odin3_elab2_lvalue lv; /* an output's or inout's net lvalue */
    bool has_lv;
    uint32_t width;
    odin3_net_id *nets;
} port_conn;

typedef struct inst_run {
    odin3_elab2 *e2;
    odin3_ast_id instantiation, instance;
    inst_target tg;
    const char *module_name, *name;
    port_conn *ports;
    odin3_value *params;
    uint64_t pins;
} inst_run;

static const char *str_of(const odin3_elab2 *e2, uint32_t id) {
    const char *str = odin3_strtab_get(odin3_design_strtab(e2->design), id);
    return str != NULL ? str : "";
}

/* The cell type of the INSTANTIATION: a module of the store, a black box, or a registered type. */
static odin3_status inst_find_target(inst_run *run, bool *found) {
    odin3_elab2 *e2 = run->e2;
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, run->instantiation);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    uint64_t value = 0;
    *found = true;
    if (rec != NULL && rec->kind == ODIN3_SYM_MODULE) {
        if (odin3_u64map_get(e2->bboxes, rec->decl.v, &value)) {
            run->tg.type = (odin3_celltype_id){(uint32_t)value};
        } else if (odin3_u64map_get(e2->modules, rec->decl.v, &value)) {
            run->tg.type = odin3_module_celltype(
                odin3_module_get(e2->design, (odin3_module_id){(uint32_t)value}));
            run->tg.is_module = true;
        } else {
            *found = false;
        }
    } else if (!odin3_celltype_find(e2->design, odin3_ast_name(e2->elab, run->instantiation),
                                    &run->tg.type)) {
        *found = false;
    }
    if (!*found) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, run->instantiation),
                          "module '%s' has no IR object (a 2D bug)", run->module_name);
        return ODIN3_ERR_PARSE;
    }
    run->tg.def = odin3_celltype_get(e2->design, run->tg.type);
    return run->tg.def != NULL ? ODIN3_OK : ODIN3_ERR_PARSE;
}

/* --- connections ------------------------------------------------------------------------------ */

/* The port of the type a connection goes to: by name, else by its position among the connections
 * (a module's or black box's connections are one per port in port order, EL-4). */
static int64_t conn_port(const inst_run *run, odin3_ast_id conn, uint32_t position) {
    const odin3_celltype_def *def = run->tg.def;
    uint32_t name = odin3_ast_name(run->e2->elab, conn);
    if (name == 0) {
        return position < def->n_ports ? (int64_t)position : -1;
    }
    const char *want = str_of(run->e2, name);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, want) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

/* Fills ports[].conn / expr from the INSTANCE's connections. */
static void inst_map_connections(inst_run *run) {
    const odin3_elab2 *e2 = run->e2;
    odin3_ast_span conns = odin3_ast_children(e2->elab, run->instance);
    for (uint32_t i = 1; i < conns.n; i++) {
        int64_t port = conn_port(run, conns.ids[i], i - 1U);
        if (port >= 0) {
            run->ports[port].conn = conns.ids[i];
            run->ports[port].expr = odin3_ast_child(e2->elab, conns.ids[i], 0);
        }
    }
}

/* True when every bit of sig is a constant: such an input is resized without a warning. */
static bool sig_is_const(const odin3_sig *sig) {
    for (uint32_t i = 0; i < sig->width; i++) {
        if (!odin3_sb_is_const(sig->bits[i])) {
            return false;
        }
    }
    return true;
}

/* The inputs generated at their self-determined width and sign, the outputs and inouts taken
 * apart as net lvalues. */
static odin3_status inst_generate(inst_run *run) {
    odin3_elab2 *e2 = run->e2;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < run->tg.def->n_ports; i++) {
        port_conn *pc = &run->ports[i];
        if (pc->expr.v == 0) {
            continue;
        }
        if (run->tg.def->ports[i].dir == ODIN3_DIR_IN) {
            odin3_elab2_expr_req req = {pc->expr, {0, false, false}, NULL, NULL};
            st = odin3_elab2_expr(e2, &req, &pc->in);
            pc->in_const = st == ODIN3_OK && sig_is_const(&pc->in);
        } else {
            odin3_elab2_lvalue_init(&pc->lv);
            pc->has_lv = true;
            st = odin3_elab2_lvalue_make(e2, pc->expr, &pc->lv);
        }
    }
    return st;
}

/* --- parameters ------------------------------------------------------------------------------ */

/* The value of the parameter node holds, as the type's parameter wants it: an INT (a NUMBER), BITS
 * or STRING. A value that does not fit the kind is a located error. */
static odin3_status param_value(odin3_elab2 *e2, const odin3_param_def *pdef, odin3_ast_id node,
                                odin3_value *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
    if (kind == ODIN3_AST_STRING && pdef->kind == ODIN3_VAL_STRING) {
        *out = (odin3_value){.kind = ODIN3_VAL_STRING, .str = odin3_ast_name(e2->elab, node)};
        return ODIN3_OK;
    }
    if (kind != ODIN3_AST_NUMBER) {
        return ODIN3_ERR_PARSE;
    }
    if (pdef->kind == ODIN3_VAL_INT) {
        int64_t num = 0;
        bool known = false;
        odin3_status st = odin3_elab2_number_i64(e2, node, &num, &known);
        *out = odin3_value_int(num);
        return st == ODIN3_OK && !known ? ODIN3_ERR_PARSE : st;
    }
    odin3_ast_literal lit;
    odin3_status st = odin3_ast_number_value(e2->elab, node, e2->scratch, &lit);
    *out = lit.value;
    return st == ODIN3_OK && pdef->kind != ODIN3_VAL_BITS ? ODIN3_ERR_PARSE : st;
}

/* The parameter of the type named name; -1 when it has none. */
static int64_t param_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

/* The values given by the INSTANTIATION's params LIST, over the defaults and the inferred ones. */
static odin3_status inst_given_params(inst_run *run) {
    odin3_elab2 *e2 = run->e2;
    const odin3_celltype_def *def = run->tg.def;
    odin3_ast_span list =
        odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, run->instantiation, 0));
    for (uint32_t i = 0; i < list.n; i++) {
        int64_t at = param_index(def, odin3_ast_name_str(e2->elab, list.ids[i]));
        if (at < 0) {
            continue; /* a registered type that does not have it: stage 1 reported cell types */
        }
        odin3_value value;
        odin3_status st =
            param_value(e2, &def->params[at], odin3_ast_child(e2->elab, list.ids[i], 0), &value);
        if (st == ODIN3_ERR_PARSE) {
            odin3_elab2_error(e2, odin3_ast_loc(e2->elab, list.ids[i]),
                              "value of parameter '%s' of '%s' is not an integer",
                              def->params[at].name, run->module_name);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        run->params[at] = value;
    }
    return ODIN3_OK;
}

/* The width the connection of a port shows the type (0: not connected). */
static uint32_t port_seen_width(const port_conn *pc) {
    if (pc->has_lv) {
        return (uint32_t)pc->lv.nets.len;
    }
    return pc->expr.v != 0 ? pc->in.width : 0U;
}

/* The cell parameters: the type's defaults, those the connection widths imply
 * (odin3_celltype_infer_params, IR-7b), then the given ones. */
static odin3_status inst_params(inst_run *run) {
    const odin3_celltype_def *def = run->tg.def;
    if (def->n_params == 0) {
        return ODIN3_OK;
    }
    odin3_elab2 *e2 = run->e2;
    uint32_t *seen = odin3_arena_alloc(e2->scratch, (size_t)def->n_ports * sizeof *seen);
    run->params = odin3_arena_alloc(e2->scratch, (size_t)def->n_params * sizeof *run->params);
    if (seen == NULL || run->params == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const port_conn *pc = &run->ports[i];
        seen[i] = port_seen_width(pc);
    }
    odin3_status st = odin3_celltype_infer_params(e2->design, run->tg.type, seen, run->params);
    return st != ODIN3_OK ? st : inst_given_params(run);
}

/* --- widths and the resize of I4 ---------------------------------------------------------------
 */

/* The warning of a connection resized to its port (not for a constant input). */
static void inst_resize_warning(const inst_run *run, uint32_t port, uint32_t got) {
    const port_conn *pc = &run->ports[port];
    odin3_elab2_warn(run->e2, odin3_ast_loc(run->e2->elab, pc->conn),
                     "port '%s' of instance '%s' (module '%s') is %" PRIu32
                     " bits; the connection of %" PRIu32 " bits is %s",
                     run->tg.def->ports[port].name, run->name, run->module_name, pc->width, got,
                     got < pc->width ? "extended" : "truncated");
}

/* Port widths for the parameters, the pin total (point (aa)'s cap) and the nets arrays. */
static odin3_status inst_widths(inst_run *run) {
    odin3_elab2 *e2 = run->e2;
    const odin3_celltype_def *def = run->tg.def;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        odin3_port_query query = {run->tg.type, run->params, i};
        uint32_t width = 0;
        odin3_status st = odin3_celltype_port_width_checked(e2->design, &query, &width);
        if (st != ODIN3_OK) {
            return st == ODIN3_ERR_NO_MEMORY ? st : ODIN3_ERR_PARSE;
        }
        run->ports[i].width = width;
        run->pins += width;
    }
    if (run->pins > e2->limits->max_cell_pins) {
        odin3_elab2_fatal(e2, odin3_ast_loc(e2->elab, run->instance),
                          "cell of %" PRIu64 " pins exceeds the limit of %" PRIu32 " pins",
                          run->pins, e2->limits->max_cell_pins);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* --- claiming the outputs and building the pins' nets -------------------------------------------
 */

/* Every output and inout connection is claimed first, as one unit: a conflict attaches nothing. */
static odin3_status inst_claims(inst_run *run, bool *granted) {
    const odin3_celltype_def *def = run->tg.def;
    *granted = true;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        port_conn *pc = &run->ports[i];
        if (!pc->has_lv) {
            continue;
        }
        bool inout = def->ports[i].dir == ODIN3_DIR_INOUT;
        odin3_elab2_lvclaim claim = {&pc->lv,
                                     inout ? ODIN3_DRV_INOUT_PIN : ODIN3_DRV_INSTANCE,
                                     pc->conn,
                                     true,
                                     def->ports[i].name,
                                     run->name};
        bool ok = false;
        odin3_status st = odin3_elab2_lvalue_claim(run->e2, &claim, &ok);
        if (st != ODIN3_OK || !ok) {
            *granted = false;
            return st;
        }
    }
    return ODIN3_OK;
}

/* A fresh unnamed net for a pin nothing is connected to. */
static odin3_status inst_fresh_net(inst_run *run, odin3_net_id *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_status st = odin3_elab2_reserve_nets(e2, 1, odin3_ast_loc(e2->elab, run->instance));
    return st != ODIN3_OK ? st : odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), out);
}

/* Pins of an input: the connection resized to the port, or the x constant when unconnected. */
static odin3_status port_input_nets(inst_run *run, uint32_t port) {
    port_conn *pc = &run->ports[port];
    odin3_status st = ODIN3_OK;
    if (pc->expr.v == 0) {
        odin3_net_id unknown = {0};
        st = odin3_elab2_const_net(run->e2, ODIN3_BIT_X, &unknown);
        for (uint32_t k = 0; st == ODIN3_OK && k < pc->width; k++) {
            pc->nets[k] = unknown;
        }
        return st;
    }
    if (pc->in.width != pc->width && !pc->in_const) {
        inst_resize_warning(run, port, pc->in.width);
    }
    odin3_sig fit;
    st = odin3_elab2_extend(run->e2, &pc->in, pc->width, pc->in.is_signed, &fit);
    return st != ODIN3_OK ? st : odin3_elab2_to_nets(run->e2, &fit, pc->nets);
}

/* Pins of an output or inout: the lvalue's nets for the bits the port has (a fresh net for a
 * dropped bit), fresh nets for the port bits past the connection. */
static odin3_status port_output_nets(inst_run *run, uint32_t port) {
    port_conn *pc = &run->ports[port];
    uint32_t got = pc->has_lv ? (uint32_t)pc->lv.nets.len : 0U;
    if (pc->has_lv && got != pc->width) {
        inst_resize_warning(run, port, got);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pc->width; k++) {
        odin3_net_id net =
            k < got ? ((const odin3_net_id *)pc->lv.nets.data)[k] : (odin3_net_id){0};
        st = net.v == 0 ? inst_fresh_net(run, &net) : ODIN3_OK;
        pc->nets[k] = net;
    }
    return st;
}

static odin3_status inst_nets(inst_run *run) {
    const odin3_celltype_def *def = run->tg.def;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        port_conn *pc = &run->ports[i];
        pc->nets = odin3_arena_alloc(run->e2->scratch, (size_t)(pc->width + 1U) * sizeof *pc->nets);
        if (pc->nets == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st = def->ports[i].dir == ODIN3_DIR_IN ? port_input_nets(run, i) : port_output_nets(run, i);
    }
    return st;
}

/* --- the node ----------------------------------------------------------------------------------
 */

/* The value of one `parameter:<NAME>` attribute from a params LIST entry. */
static odin3_status param_attr(inst_run *run, odin3_ast_id entry, odin3_node_id node) {
    odin3_elab2 *e2 = run->e2;
    odin3_ast_id value = odin3_ast_child(e2->elab, entry, 0);
    odin3_value val;
    if (odin3_ast_kind_of(e2->elab, value) == ODIN3_AST_STRING) {
        val = (odin3_value){.kind = ODIN3_VAL_STRING, .str = odin3_ast_name(e2->elab, value)};
    } else {
        odin3_ast_literal lit;
        odin3_status st = odin3_ast_number_value(e2->elab, value, e2->scratch, &lit);
        if (st != ODIN3_OK) {
            return st;
        }
        val = lit.value;
    }
    uint32_t key = 0;
    odin3_strbuf_clear(&e2->text);
    odin3_status st =
        odin3_strbuf_appendf(&e2->text, "parameter:%s", odin3_ast_name_str(e2->elab, entry));
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(e2->design, (odin3_bytes){e2->text.data, e2->text.len}, &key);
    return st != ODIN3_OK
               ? st
               : odin3_attr_set(e2->mod, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key, &val);
}

/* The attributes of the instantiation and, for a module, `parameter:<NAME>` per parameter. */
static odin3_status inst_attrs(inst_run *run, odin3_node_id node) {
    odin3_elab2 *e2 = run->e2;
    odin3_status st =
        odin3_elab2_attrs(e2, run->instantiation, (odin3_objref){ODIN3_OBJ_NODE, node.v});
    odin3_ast_span list =
        odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, run->instantiation, 0));
    for (uint32_t i = 0; st == ODIN3_OK && run->tg.is_module && i < list.n; i++) {
        st = param_attr(run, list.ids[i], node);
    }
    return st;
}

/* The IR name of the INSTANCE: its scope path and its name (unnamed: none). */
static odin3_status inst_node_name(inst_run *run, uint32_t *name) {
    odin3_elab2 *e2 = run->e2;
    *name = 0;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, run->instance));
    return rec != NULL ? odin3_elab2_scoped_name(e2, rec, name) : ODIN3_OK;
}

static odin3_status inst_create(inst_run *run, odin3_node_id *node) {
    odin3_elab2 *e2 = run->e2;
    const odin3_celltype_def *def = run->tg.def;
    odin3_netvec *vecs = odin3_arena_alloc(e2->scratch, (size_t)(def->n_ports + 1U) * sizeof *vecs);
    if (vecs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < def->n_ports; i++) {
        vecs[i] = (odin3_netvec){run->ports[i].nets, run->ports[i].width};
    }
    odin3_elab2_prov_req preq = {run->instance, {0}, e2->scope.v};
    odin3_node_spec spec = {.type = run->tg.type, .params = run->params, .n_params = def->n_params};
    odin3_status st = inst_node_name(run, &spec.name);
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &preq, &spec.prov);
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, vecs, node);
    return st != ODIN3_OK ? st : inst_attrs(run, *node);
}

/* The net the extra bits of output port `port` take: its top bit's net when the child declares the
 * port signed, else the 0 constant. */
static odin3_status extension_net(inst_run *run, uint32_t port, odin3_net_id *fill) {
    const port_conn *pc = &run->ports[port];
    if (run->tg.def->ports[port].is_signed && pc->width > 0) {
        *fill = pc->nets[pc->width - 1U];
        return ODIN3_OK;
    }
    return odin3_elab2_const_net(run->e2, ODIN3_BIT_0, fill);
}

/* An output wider than its port: the extra bits take the port's top bit when the child declares
 * the port signed, else 0 (I4; a cell type or black box has no sign, so 0). */
static odin3_status extend_one_output(inst_run *run, uint32_t port) {
    odin3_elab2 *e2 = run->e2;
    const port_conn *pc = &run->ports[port];
    odin3_net_id fill = {0};
    odin3_status st = extension_net(run, port, &fill);
    for (uint32_t k = pc->width; st == ODIN3_OK && k < pc->lv.nets.len; k++) {
        odin3_net_id extra = odin3_elab2_resolve(e2, ((const odin3_net_id *)pc->lv.nets.data)[k]);
        odin3_net_id keep = odin3_elab2_resolve(e2, fill);
        if (extra.v != 0 && extra.v != keep.v) {
            st = odin3_elab2_merge(e2, (odin3_net_pair){keep, extra});
        }
    }
    return st;
}

static odin3_status inst_extend_outputs(inst_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t port = 0; st == ODIN3_OK && port < run->tg.def->n_ports; port++) {
        const port_conn *pc = &run->ports[port];
        if (run->tg.def->ports[port].dir == ODIN3_DIR_OUT && pc->has_lv &&
            pc->lv.nets.len > pc->width) {
            st = extend_one_output(run, port);
        }
    }
    return st;
}

/* The IR objects of the instance for the byte estimate. */
static odin3_status inst_account(inst_run *run) {
    odin3_elab2_counts made = {1, (uint32_t)run->pins, 0, 0};
    return odin3_elab2_account(run->e2, &made);
}

static void inst_free(inst_run *run) {
    for (uint32_t i = 0; run->ports != NULL && i < run->tg.def->n_ports; i++) {
        if (run->ports[i].has_lv) {
            odin3_elab2_lvalue_free(&run->ports[i].lv);
        }
    }
}

/* One INSTANCE: generate its connections, work out the cell's parameters and widths, claim its
 * outputs, then make the node. */
static odin3_status inst_one(inst_run *run) {
    odin3_elab2 *e2 = run->e2;
    run->name = odin3_ast_name_str(e2->elab, run->instance);
    bool found = false;
    odin3_status st = inst_find_target(run, &found);
    if (st != ODIN3_OK) {
        return st;
    }
    run->ports =
        odin3_arena_alloc(e2->scratch, (size_t)(run->tg.def->n_ports + 1U) * sizeof *run->ports);
    if (run->ports == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(run->ports, 0, (size_t)(run->tg.def->n_ports + 1U) * sizeof *run->ports);
    inst_map_connections(run);
    odin3_node_id node = {0};
    bool granted = false;
    st = inst_generate(run);
    st = st != ODIN3_OK ? st : inst_params(run);
    st = st != ODIN3_OK ? st : inst_widths(run);
    st = st != ODIN3_OK ? st : inst_claims(run, &granted);
    st = st == ODIN3_OK && !granted ? ODIN3_ERR_PARSE : st;
    st = st != ODIN3_OK ? st : inst_nets(run);
    st = st != ODIN3_OK ? st : inst_create(run, &node);
    st = st != ODIN3_OK ? st : inst_extend_outputs(run);
    st = st != ODIN3_OK ? st : inst_account(run);
    inst_free(run);
    return st;
}

odin3_status odin3_elab2_instance(odin3_elab2 *e2, odin3_ast_id instantiation) {
    if (e2->mod == NULL || odin3_ast_kind_of(e2->elab, instantiation) != ODIN3_AST_INSTANTIATION) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_elab2_instance: not an INSTANTIATION, or no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_span kids = odin3_ast_children(e2->elab, instantiation);
    odin3_status result = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); i < kids.n && !e2->stopped;
         i++) {
        inst_run run = {.e2 = e2,
                        .instantiation = instantiation,
                        .instance = kids.ids[i],
                        .module_name = odin3_ast_name_str(e2->elab, instantiation)};
        odin3_status st = inst_one(&run);
        if (st == ODIN3_ERR_PARSE) {
            result = ODIN3_ERR_PARSE; /* counted: go on with the next instance */
        } else if (st != ODIN3_OK) {
            return st;
        }
    }
    return result;
}

/* --- gates (Q1-Q3) ---------------------------------------------------------------------------- */

/* One gate instance being lowered: its terminals (the INSTANCE's connections after the array
 * slot) and the signals generated for its inputs. */
typedef struct gate_run {
    odin3_elab2 *e2;
    odin3_ast_id gate, instance;
    uint32_t kind;
    odin3_ast_span terms; /* the INSTANCE's children: [0] is the array slot */
    uint32_t n_terms;
    odin3_sig *ins; /* the input terminals, generated self-determined */
    uint32_t n_ins;
} gate_run;

static odin3_ast_id term_expr(const gate_run *gr, uint32_t term) {
    return odin3_ast_child(gr->e2->elab, gr->terms.ids[term + 1U], 0);
}

/* The node of terminal `term` for diagnostics: its CONNECTION. */
static odin3_ast_id term_node(const gate_run *gr, uint32_t term) {
    return gr->terms.ids[term + 1U];
}

static bool gate_is_fold(uint32_t kind) {
    return kind == ODIN3_AST_GATE_DECL_AND || kind == ODIN3_AST_GATE_DECL_NAND ||
           kind == ODIN3_AST_GATE_DECL_OR || kind == ODIN3_AST_GATE_DECL_NOR ||
           kind == ODIN3_AST_GATE_DECL_XOR || kind == ODIN3_AST_GATE_DECL_XNOR;
}

static bool gate_inverts(uint32_t kind) {
    return kind == ODIN3_AST_GATE_DECL_NAND || kind == ODIN3_AST_GATE_DECL_NOR ||
           kind == ODIN3_AST_GATE_DECL_XNOR;
}

static odin3_elab2_type gate_fold_type(uint32_t kind) {
    if (kind == ODIN3_AST_GATE_DECL_AND || kind == ODIN3_AST_GATE_DECL_NAND) {
        return ODIN3_E2T_AND;
    }
    return kind == ODIN3_AST_GATE_DECL_OR || kind == ODIN3_AST_GATE_DECL_NOR ? ODIN3_E2T_OR
                                                                             : ODIN3_E2T_XOR;
}

/* Terminals first..first+count as self-determined signals into ins[0 ..). */
static odin3_status gate_inputs(gate_run *gr, uint32_t first, uint32_t count) {
    gr->ins = odin3_arena_alloc(gr->e2->scratch, (size_t)(count + 1U) * sizeof *gr->ins);
    if (gr->ins == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    gr->n_ins = count;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        odin3_elab2_expr_req req = {term_expr(gr, first + i), {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(gr->e2, &req, &gr->ins[i]);
    }
    return st;
}

/* The terminal as an lvalue, claimed as a gate's output (nothing is made on a conflict). */
static odin3_status gate_output(gate_run *gr, uint32_t term, odin3_elab2_lvalue *lv,
                                bool *granted) {
    odin3_elab2_lvalue_init(lv);
    odin3_status st = odin3_elab2_lvalue_make(gr->e2, term_expr(gr, term), lv);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab2_lvclaim claim = {lv, ODIN3_DRV_GATE, term_node(gr, term), false, NULL, NULL};
    return odin3_elab2_lvalue_claim(gr->e2, &claim, granted);
}

/* The status of a gate output: a refused claim is a counted error. */
static odin3_status gate_status(odin3_status st, bool granted) {
    if (st == ODIN3_OK && !granted) {
        return ODIN3_ERR_PARSE;
    }
    return st;
}

/* The width of the value: the widest of the output and the inputs (the context of an assign). */
static uint32_t gate_width(const gate_run *gr, uint32_t out_width) {
    uint32_t width = out_width;
    for (uint32_t i = 0; i < gr->n_ins; i++) {
        width = gr->ins[i].width > width ? gr->ins[i].width : width;
    }
    return width;
}

/* One word cell of the lowering, to dest when the width is the output's. */
static odin3_status gate_cell(gate_run *gr, odin3_elab2_cell_req req, const odin3_net_id *dest,
                              odin3_sig *out) {
    req.node = gr->gate;
    req.dest = dest;
    return odin3_elab2_cell(gr->e2, &req, out);
}

/* and/or/xor (nand/nor/xnor then $not): a left fold over the inputs; the last cell drives the
 * output when it is as wide as the value. */
static odin3_status gate_fold(gate_run *gr, odin3_elab2_lvalue *lv, odin3_sig *value) {
    uint32_t out_width = (uint32_t)lv->nets.len;
    uint32_t width = gate_width(gr, out_width);
    const odin3_net_id *dest = !lv->dropped && width == out_width ? lv->nets.data : NULL;
    bool invert = gate_inverts(gr->kind);
    odin3_sig acc = gr->ins[0];
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < gr->n_ins; i++) {
        bool last = i == gr->n_ins - 1U && !invert;
        odin3_elab2_cell_req req = {
            .type = gate_fold_type(gr->kind), .a = &acc, .b = &gr->ins[i], .y_width = width};
        st = gate_cell(gr, req, last ? dest : NULL, &acc);
    }
    if (st == ODIN3_OK && invert) {
        odin3_elab2_cell_req req = {.type = ODIN3_E2T_NOT, .a = &acc, .y_width = width};
        st = gate_cell(gr, req, dest, &acc);
    }
    *value = acc;
    return st;
}

/* The value of the lowering connected to the lvalue (by construction when a cell drives it). */
static odin3_status gate_connect(gate_run *gr, odin3_elab2_lvalue *lv, const odin3_sig *value,
                                 uint32_t out_term) {
    odin3_sig fit;
    odin3_status st =
        odin3_elab2_extend(gr->e2, value, (uint32_t)lv->nets.len, value->is_signed, &fit);
    odin3_elab2_lvconn conn = {lv, &fit, ODIN3_DRV_GATE, term_node(gr, out_term),
                               term_expr(gr, gr->n_terms - 1U)};
    return st != ODIN3_OK ? st : odin3_elab2_lvalue_connect(gr->e2, &conn);
}

/* and, nand, or, nor, xor, xnor: terminal 0 is the output, the others the inputs. */
static odin3_status gate_logic(gate_run *gr) {
    odin3_elab2_lvalue lv;
    bool granted = false;
    odin3_status st = gate_output(gr, 0, &lv, &granted);
    st = st != ODIN3_OK || !granted ? st : gate_inputs(gr, 1, gr->n_terms - 1U);
    odin3_sig value = {NULL, 0, false};
    if (st == ODIN3_OK && granted && gr->n_ins > 0) {
        st = gate_fold(gr, &lv, &value);
        st = st != ODIN3_OK ? st : gate_connect(gr, &lv, &value, 0);
    }
    odin3_elab2_lvalue_free(&lv);
    return gate_status(st, granted);
}

/* One output of buf/not: the input (inverted for not), assigned. */
static odin3_status gate_buffer_one(gate_run *gr, uint32_t term) {
    odin3_elab2_lvalue lv;
    bool granted = false;
    odin3_status st = gate_output(gr, term, &lv, &granted);
    if (st == ODIN3_OK && granted) {
        uint32_t out_width = (uint32_t)lv.nets.len;
        odin3_sig value = gr->ins[0];
        if (gr->kind == ODIN3_AST_GATE_DECL_NOT) {
            uint32_t width = gate_width(gr, out_width);
            odin3_elab2_cell_req req = {.type = ODIN3_E2T_NOT, .a = &gr->ins[0], .y_width = width};
            st =
                gate_cell(gr, req, !lv.dropped && width == out_width ? lv.nets.data : NULL, &value);
        }
        st = st != ODIN3_OK ? st : gate_connect(gr, &lv, &value, term);
    }
    odin3_elab2_lvalue_free(&lv);
    return gate_status(st, granted);
}

/* buf and not: every terminal but the last is an output, the last the input. */
static odin3_status gate_buffer(gate_run *gr) {
    odin3_status st = gate_inputs(gr, gr->n_terms - 1U, 1);
    odin3_status result = ODIN3_OK;
    for (uint32_t term = 0; st == ODIN3_OK && term + 1U < gr->n_terms && !gr->e2->stopped; term++) {
        odin3_status one = gate_buffer_one(gr, term);
        if (one == ODIN3_ERR_PARSE) {
            result = one;
        } else {
            st = one;
        }
    }
    return st != ODIN3_OK ? st : result;
}

/* bufif0/1, notif0/1 (out, in, ctrl): `ctrl ? in : z` (`in` inverted for notif), a $tribuf. */
static odin3_status gate_tristate(gate_run *gr) {
    odin3_elab2_lvalue lv;
    odin3_elab2_lvalue_init(&lv);
    odin3_status st = odin3_elab2_lvalue_make(gr->e2, term_expr(gr, 0), &lv);
    odin3_sig sigs[2];
    for (uint32_t i = 0; st == ODIN3_OK && i < 2; i++) {
        odin3_elab2_expr_req req = {term_expr(gr, 1U + i), {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(gr->e2, &req, &sigs[i]);
    }
    bool zero = gr->kind == ODIN3_AST_GATE_DECL_BUFIF0 || gr->kind == ODIN3_AST_GATE_DECL_NOTIF0;
    bool inverted_data =
        gr->kind == ODIN3_AST_GATE_DECL_NOTIF0 || gr->kind == ODIN3_AST_GATE_DECL_NOTIF1;
    odin3_elab2_tri_req req = {.lv = &lv,
                               .data = &sigs[0],
                               .cond = &sigs[1],
                               .invert = zero,
                               .invert_data = inverted_data,
                               .node = gr->gate,
                               .claim = term_node(gr, 0)};
    st = st != ODIN3_OK ? st : odin3_elab2_tribuf(gr->e2, &req);
    odin3_elab2_lvalue_free(&lv);
    return st;
}

/* The number of terminals the kind needs at least. */
static uint32_t gate_min_terms(uint32_t kind) {
    if (kind == ODIN3_AST_GATE_DECL_BUFIF0 || kind == ODIN3_AST_GATE_DECL_BUFIF1 ||
        kind == ODIN3_AST_GATE_DECL_NOTIF0 || kind == ODIN3_AST_GATE_DECL_NOTIF1) {
        return 3;
    }
    return 2;
}

static odin3_status gate_one(gate_run *gr) {
    odin3_elab2 *e2 = gr->e2;
    gr->terms = odin3_ast_children(e2->elab, gr->instance);
    gr->n_terms = gr->terms.n > 0 ? gr->terms.n - 1U : 0U;
    if (gr->n_terms < gate_min_terms(gr->kind)) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, gr->instance),
                          "gate '%s' needs at least %" PRIu32 " terminals",
                          odin3_ast_sub_name(e2->elab, gr->gate), gate_min_terms(gr->kind));
        return ODIN3_ERR_PARSE;
    }
    if (gate_is_fold(gr->kind)) {
        return gate_logic(gr);
    }
    if (gr->kind == ODIN3_AST_GATE_DECL_BUF || gr->kind == ODIN3_AST_GATE_DECL_NOT) {
        return gate_buffer(gr);
    }
    return gate_tristate(gr);
}

odin3_status odin3_elab2_gate(odin3_elab2 *e2, odin3_ast_id gate_decl) {
    if (e2->mod == NULL || odin3_ast_kind_of(e2->elab, gate_decl) != ODIN3_AST_GATE_DECL) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_elab2_gate: not a GATE_DECL, or no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_span kids = odin3_ast_children(e2->elab, gate_decl);
    odin3_status result = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_GATE_DECL); i < kids.n && !e2->stopped; i++) {
        gate_run gr = {.e2 = e2,
                       .gate = gate_decl,
                       .instance = kids.ids[i],
                       .kind = odin3_ast_sub(e2->elab, gate_decl)};
        odin3_status st = gate_one(&gr);
        if (st == ODIN3_ERR_PARSE) {
            result = ODIN3_ERR_PARSE;
        } else if (st != ODIN3_OK) {
            return st;
        }
    }
    return result;
}

/* --- black boxes (EL-11) -----------------------------------------------------------------------
 */

/* The cell type being declared from a black-box MODULE: its ports once with constant widths (the
 * declaration IR-7b compares) and once with the width parameters found, and its parameters. */
typedef struct bbox_decl {
    odin3_port_def *fixed, *sized;
    odin3_param_def *params;
    uint32_t n_ports, n_params;
    bool *overridden; /* per parameter: some instance gives it another value than its default */
} bbox_decl;

static odin3_dir bbox_dir(uint32_t sub) {
    if (sub == ODIN3_AST_PORT_DECL_OUTPUT) {
        return ODIN3_DIR_OUT;
    }
    return sub == ODIN3_AST_PORT_DECL_INOUT ? ODIN3_DIR_INOUT : ODIN3_DIR_IN;
}

/* The ports of the MODULE in port order (all written as wire ports, EL-4), with their widths. */
static odin3_status bbox_ports(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    odin3_ast_span ports = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, 1));
    bd->n_ports = ports.n;
    bd->fixed = odin3_arena_alloc(e2->scratch, (size_t)(ports.n + 1U) * sizeof *bd->fixed);
    bd->sized = odin3_arena_alloc(e2->scratch, (size_t)(ports.n + 1U) * sizeof *bd->sized);
    if (bd->fixed == NULL || bd->sized == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < ports.n; i++) {
        odin3_ast_id declarator = odin3_ast_child(e2->elab, ports.ids[i], 1);
        const odin3_symbol *rec =
            odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, declarator));
        if (rec == NULL) {
            return ODIN3_ERR_CHECK;
        }
        int64_t span = (int64_t)rec->packed.msb - (int64_t)rec->packed.lsb;
        odin3_port_def def = {.name = str_of(e2, rec->name),
                              .dir = bbox_dir(odin3_ast_sub(e2->elab, ports.ids[i])),
                              .scalar = !rec->has_packed,
                              .width =
                                  rec->has_packed ? (uint32_t)(span < 0 ? -span : span) + 1U : 1U};
        bd->fixed[i] = def;
        bd->sized[i] = def;
    }
    return ODIN3_OK;
}

/* One declared parameter: an integer when its value is one, else bits or a string. */
static odin3_status bbox_param(odin3_elab2 *e2, odin3_ast_id declarator, odin3_param_def *out) {
    odin3_ast_id value = odin3_ast_child(e2->elab, declarator, 0);
    out->name = odin3_ast_name_str(e2->elab, declarator);
    if (odin3_ast_kind_of(e2->elab, value) == ODIN3_AST_STRING) {
        out->kind = ODIN3_VAL_STRING;
        out->dflt = (odin3_value){.kind = ODIN3_VAL_STRING, .str = odin3_ast_name(e2->elab, value)};
        return ODIN3_OK;
    }
    int64_t num = 0;
    bool known = false;
    odin3_status st = odin3_elab2_number_i64(e2, value, &num, &known);
    if (st == ODIN3_OK && known) {
        out->kind = ODIN3_VAL_INT;
        out->dflt = odin3_value_int(num);
        return ODIN3_OK;
    }
    odin3_ast_literal lit;
    st = st != ODIN3_OK ? st : odin3_ast_number_value(e2->elab, value, e2->scratch, &lit);
    out->kind = ODIN3_VAL_BITS;
    out->dflt = lit.value;
    return st;
}

/* The parameters (a PARAM_DECL's declarators, bare in a black box) in declaration order. */
static odin3_status bbox_params(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    odin3_ast_id list = odin3_ast_child(e2->elab, module_node, 0);
    odin3_ast_span decls = odin3_ast_children(e2->elab, list);
    uint32_t slots = odin3_ast_slot_count(ODIN3_AST_PARAM_DECL);
    uint32_t total = 0;
    for (uint32_t i = 0; list.v != 0 && i < decls.n; i++) {
        uint32_t kids = odin3_ast_nchild(e2->elab, decls.ids[i]);
        total += kids > slots ? kids - slots : 0U;
    }
    bd->params = odin3_arena_alloc(e2->scratch, (size_t)(total + 1U) * sizeof *bd->params);
    bd->overridden = odin3_arena_alloc(e2->scratch, (size_t)(total + 1U) * sizeof(bool));
    if (bd->params == NULL || bd->overridden == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && list.v != 0 && i < decls.n; i++) {
        odin3_ast_span kids = odin3_ast_children(e2->elab, decls.ids[i]);
        for (uint32_t k = slots; st == ODIN3_OK && k < kids.n; k++) {
            bd->overridden[bd->n_params] = false;
            st = bbox_param(e2, kids.ids[k], &bd->params[bd->n_params]);
            bd->n_params++;
        }
    }
    return st;
}

/* The parameter of the black box named name; -1 for none. */
static int64_t bbox_param_index(const bbox_decl *bd, const char *name) {
    for (uint32_t k = 0; k < bd->n_params; k++) {
        if (strcmp(bd->params[k].name, name) == 0) {
            return (int64_t)k;
        }
    }
    return -1;
}

/* Marks the parameters one INSTANTIATION gives another integer value than the default. */
static void bbox_note_overrides(odin3_elab2 *e2, odin3_ast_id node, bbox_decl *bd) {
    odin3_ast_span list = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, node, 0));
    for (uint32_t i = 0; i < list.n; i++) {
        int64_t at = bbox_param_index(bd, odin3_ast_name_str(e2->elab, list.ids[i]));
        odin3_value value;
        if (at >= 0 && bd->params[at].kind == ODIN3_VAL_INT &&
            param_value(e2, &bd->params[at], odin3_ast_child(e2->elab, list.ids[i], 0), &value) ==
                ODIN3_OK &&
            value.i != bd->params[at].dflt.i) {
            bd->overridden[at] = true;
        }
    }
}

/* Which parameters some INSTANTIATION of the definition gives a value other than the default. */
static void bbox_overrides(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    uint32_t name = odin3_ast_name(e2->elab, module_node);
    uint32_t end = odin3_ast_node_end(e2->elab).v;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_INSTANTIATION &&
            odin3_ast_name(e2->elab, node) == name) {
            bbox_note_overrides(e2, node, bd);
        }
    }
}

/* A port whose width equals the default of an overridden integer parameter takes it as its width
 * parameter (IR-7b); the first such parameter wins. */
static void bbox_width_params(bbox_decl *bd) {
    for (uint32_t i = 0; i < bd->n_ports; i++) {
        for (uint32_t k = 0; k < bd->n_params && bd->sized[i].width_param == NULL; k++) {
            if (bd->overridden[k] && bd->params[k].kind == ODIN3_VAL_INT &&
                bd->params[k].dflt.i == (int64_t)bd->sized[i].width) {
                bd->sized[i].width_param = bd->params[k].name;
            }
        }
    }
}

/* A registered type of that name: the declaration must match it (IR-7b), else the located error. */
static odin3_status bbox_match(odin3_elab2 *e2, odin3_ast_id module_node, const bbox_decl *bd,
                               odin3_celltype_id registered) {
    const odin3_celltype_def *reg = odin3_celltype_get(e2->design, registered);
    odin3_celltype_def decl = {
        .name = reg->name, .gran = ODIN3_GRAN_BLACKBOX, .ports = bd->fixed, .n_ports = bd->n_ports};
    odin3_value *params =
        odin3_arena_alloc(e2->scratch, (size_t)(reg->n_params + 1U) * sizeof *params);
    if (params == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_blackbox_match match = {registered, &decl, params};
    odin3_width_why why = {{0}};
    odin3_status st = odin3_celltype_blackbox_match(e2->design, &match, &why);
    if (st == ODIN3_ERR_INVALID_ARG) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, module_node),
                          "black box '%s' does not match the registered cell type: %s", reg->name,
                          why.text);
        return ODIN3_ERR_PARSE;
    }
    return st;
}

odin3_status odin3_elab2_blackbox(odin3_elab2 *e2, odin3_ast_id module_node) {
    bbox_decl bd = {0};
    odin3_status st = bbox_ports(e2, module_node, &bd);
    st = st != ODIN3_OK ? st : bbox_params(e2, module_node, &bd);
    if (st == ODIN3_OK) {
        bbox_overrides(e2, module_node, &bd);
    }
    odin3_celltype_id registered = {0};
    uint32_t name = odin3_ast_name(e2->elab, module_node);
    bool taken = st == ODIN3_OK && odin3_celltype_find(e2->design, name, &registered);
    if (st == ODIN3_OK && taken) {
        st = bbox_match(e2, module_node, &bd, registered);
    } else if (st == ODIN3_OK) {
        bbox_width_params(&bd);
    }
    odin3_celltype_def def = {.name = str_of(e2, name),
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = taken ? bd.fixed : bd.sized,
                              .n_ports = bd.n_ports,
                              .params = bd.params,
                              .n_params = bd.n_params};
    odin3_celltype_id id = {0};
    st = st != ODIN3_OK ? st : odin3_celltype_declare_blackbox(e2->design, &def, &id);
    st = st != ODIN3_OK ? st : odin3_u64map_put(e2->bboxes, (odin3_kv){module_node.v, id.v});
    return st == ODIN3_ERR_INVALID_ARG ? ODIN3_ERR_PARSE : st;
}
