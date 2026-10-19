/*
 * elab2_inst.c — instances and gates in stage 2 (2D Task 19; EL-10, EL-11, EL-29, Q1-Q3): the node
 * of a module, a black box or a registered cell type with its ports resized to the port (I4,
 * Yosys's hierarchy rules), the parameters of a cell type, the attributes `parameter:<NAME>`, and
 * the primitives lowered as Yosys's simplify does. The black-box cell types are elab2_bbox.c;
 * drivers (the claim of every output) are elab2_claim.c.
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
#include <limits.h>
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
    bool icell; /* a `$` cell type read under -icells (5.2): widths are exact, kinds are checked */
} inst_run;

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
    if (run->tg.def == NULL) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, run->instantiation),
                          "module '%s' has no cell type (a 2D bug)", run->module_name);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
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
    const char *want = odin3_elab2_str_of(run->e2, name);
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
odin3_status odin3_elab2_param_value(odin3_elab2 *e2, const odin3_param_def *pdef,
                                     odin3_ast_id node, odin3_value *out) {
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

/* The kind a parameter wants, as the error of a value of another kind says it. */
static const char *kind_text(odin3_value_kind kind) {
    if (kind == ODIN3_VAL_INT) {
        return "an integer";
    }
    if (kind == ODIN3_VAL_COVER) {
        return "a quoted list of cover rows (blank separated; inputs 0, 1 or -, then output 0 or "
               "1)";
    }
    return kind == ODIN3_VAL_STRING ? "a string" : "a bit vector";
}

/* The error of a parameter value that does not fit the kind (spec V-2 wording under -icells). */
static void param_kind_error(const inst_run *run, const odin3_param_def *pdef, odin3_loc at) {
    if (!run->icell) {
        odin3_elab2_error(run->e2, at, "value of parameter '%s' of '%s' is not an integer",
                          pdef->name, run->module_name);
        return;
    }
    odin3_elab2_error(run->e2, at, "value of parameter '%s' of cell type '%s' is not %s",
                      pdef->name, run->module_name, kind_text(pdef->kind));
}

/* The longest string a BITS or COVER parameter is read from (the lengths are uint32_t). */
enum { ICELL_STRING_MAX = 1U << 24 };

/* Spec V-5, under -icells only: a string literal given to a BITS parameter is the bits of its
 * bytes, first character most significant (Verilog's rule), and "" is the zero-length value the
 * writer uses for an empty BITS value. ODIN3_ERR_PARSE: too long. */
static odin3_status string_to_bits(odin3_elab2 *e2, const char *text, odin3_value *out) {
    size_t bytes = strlen(text);
    const size_t per_byte = CHAR_BIT;
    size_t count = bytes * per_byte;
    if (bytes > ICELL_STRING_MAX) {
        return ODIN3_ERR_PARSE;
    }
    uint8_t *bits = odin3_arena_alloc(e2->scratch, count + 1U);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        unsigned byte = (unsigned char)text[bytes - 1U - i / per_byte];
        bits[i] = ((byte >> (i % per_byte)) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    *out = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bits, .len = (uint32_t)count};
    return ODIN3_OK;
}

/* The inputs per row of an empty cover: the WIDTH parameter of the type, 0 when it has none. */
static uint32_t cover_default_inputs(const inst_run *run) {
    int64_t at = param_index(run->tg.def, "WIDTH");
    bool usable = at >= 0 && run->params != NULL && run->params[at].kind == ODIN3_VAL_INT &&
                  run->params[at].i >= 0 && run->params[at].i < ICELL_STRING_MAX;
    return usable ? (uint32_t)run->params[at].i : 0U;
}

/* True when text[0 .. inputs] is a cover row: `inputs` characters of 0, 1 or -, then 0 or 1. */
static bool cover_row_ok(const char *text, uint32_t inputs) {
    for (uint32_t k = 0; k < inputs; k++) {
        if (text[k] != '0' && text[k] != '1' && text[k] != '-') {
            return false;
        }
    }
    return text[inputs] == '0' || text[inputs] == '1';
}

/* The writer's COVER spelling (writer.h): the rows, each `inputs` characters of 0, 1 or - and one
 * output character 0 or 1, separated by blanks; the inputs per row are those of the first row
 * (an empty text has no rows). ODIN3_ERR_PARSE for text that is not that. */
static odin3_status string_to_cover(odin3_elab2 *e2, const char *text, uint32_t dflt_inputs,
                                    odin3_value *out) {
    size_t len = strlen(text);
    uint32_t inputs = dflt_inputs;
    if (len > 0) {
        size_t first = strcspn(text, " ");
        inputs = (uint32_t)(first > 0 ? first - 1U : 0U);
    }
    size_t row_len = (size_t)inputs + 1U;
    if (len > ICELL_STRING_MAX) {
        return ODIN3_ERR_PARSE;
    }
    uint8_t *rows = odin3_arena_alloc(e2->scratch, len + 1U);
    if (rows == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = 0;
    for (size_t at = 0; at < len; at += row_len + 1U) {
        bool whole = at + row_len <= len && cover_row_ok(text + at, inputs);
        if (!whole || (at + row_len < len && text[at + row_len] != ' ')) {
            return ODIN3_ERR_PARSE;
        }
        memcpy(rows + used, text + at, row_len);
        used += row_len;
    }
    *out = (odin3_value){
        .kind = ODIN3_VAL_COVER, .bits = rows, .len = (uint32_t)used, .cover_inputs = inputs};
    return ODIN3_OK;
}

/* Under -icells, a string literal given to a BITS or COVER parameter. ODIN3_ERR_INVALID_ARG: not
 * this case (nothing done; the ordinary value reading follows). */
static odin3_status icell_string_value(const inst_run *run, const odin3_param_def *pdef,
                                       odin3_ast_id node, odin3_value *out) {
    odin3_elab2 *e2 = run->e2;
    bool wanted = pdef->kind == ODIN3_VAL_BITS || pdef->kind == ODIN3_VAL_COVER;
    if (!run->icell || !wanted || odin3_ast_kind_of(e2->elab, node) != ODIN3_AST_STRING) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *text = odin3_elab2_str_of(e2, odin3_ast_name(e2->elab, node));
    return pdef->kind == ODIN3_VAL_BITS ? string_to_bits(e2, text, out)
                                        : string_to_cover(e2, text, cover_default_inputs(run), out);
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
        odin3_ast_id node = odin3_ast_child(e2->elab, list.ids[i], 0);
        odin3_status st = icell_string_value(run, &def->params[at], node, &value);
        st = st == ODIN3_ERR_INVALID_ARG
                 ? odin3_elab2_param_value(e2, &def->params[at], node, &value)
                 : st;
        if (st == ODIN3_ERR_PARSE) {
            param_kind_error(run, &def->params[at], odin3_ast_loc(e2->elab, list.ids[i]));
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
    if (def->n_params == 0 || run->params != NULL) {
        return ODIN3_OK; /* nothing to work out, or already done */
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
            if (st == ODIN3_ERR_NO_MEMORY) {
                return st;
            }
            odin3_elab2_error(e2, odin3_ast_loc(e2->elab, run->instance),
                              "instance '%s' of '%s': port '%s' has no width for its parameter "
                              "values (see the port_width line above)",
                              run->name, run->module_name, def->ports[i].name);
            return ODIN3_ERR_PARSE;
        }
        run->ports[i].width = width;
        run->pins += width;
    }
    return odin3_elab2_pins_cap(e2, odin3_ast_loc(e2->elab, run->instance), run->pins);
}

/* Spec V-3: under -icells a connection has exactly the width of its port (the writer sizes every
 * one); an unconnected port is allowed. */
static odin3_status inst_strict_widths(inst_run *run) {
    const odin3_celltype_def *def = run->tg.def;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; run->icell && i < def->n_ports; i++) {
        const port_conn *pc = &run->ports[i];
        uint32_t got = port_seen_width(pc);
        if (got != 0 && got != pc->width) {
            odin3_elab2_error(run->e2, odin3_ast_loc(run->e2->elab, pc->conn),
                              "port '%s' of '%s' instance '%s' is %" PRIu32
                              " bits; the connection is %" PRIu32 " bits",
                              def->ports[i].name, run->module_name, run->name, pc->width, got);
            st = ODIN3_ERR_PARSE;
        }
    }
    return st;
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
    if (st == ODIN3_ERR_INVALID_ARG && run->icell) {
        /* the type's verify hook refused the parameter set (spec V-6): located, and counted */
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, run->instance),
                          "instance '%s' of '%s': the parameter values are not valid for the cell "
                          "type (see the message above)",
                          run->name, run->module_name);
        return ODIN3_ERR_PARSE;
    }
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
    run->icell = e2->opts.icells && !run->tg.is_module && run->module_name[0] == '$';
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
    st = st != ODIN3_OK ? st : inst_strict_widths(run);
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
            odin3_elab2_count_parse(e2, odin3_ast_loc(e2->elab, kids.ids[i]));
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
            odin3_elab2_count_parse(e2, odin3_ast_loc(e2->elab, kids.ids[i]));
            result = ODIN3_ERR_PARSE;
        } else if (st != ODIN3_OK) {
            return st;
        }
    }
    return result;
}
