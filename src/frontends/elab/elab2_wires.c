/*
 * elab2_wires.c — stage 2's IR objects of a declaration (2D Task 17; EL-23, EL-24, EL-45, IR-4):
 * the module and its ports, one wire per net or variable declarator named by its scope path,
 * the SOURCE records, the attributes of 2A §7 and the shared constant nets. The context, the
 * diagnostics and the module loop are elab2.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "frontends/elab/netlist_locs.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The separators of a scope path: wire names join with '.', hierarchy paths with '/'. */
enum { E2_NAME_SEP = '.', E2_HIER_SEP = '/' };

static const char k_implicit[] = "implicit"; /* stage 1's marker ATTR on an implicit net */

/* --- scope paths and provenance (EL-45) ------------------------------------------------------ */

/* A scope path to append to e2->text: the generate and named-block scopes from the module down,
 * each `name` or `name[index]`, joined by sep, with sep after each component (trailing) or before
 * it. */
typedef struct path_req {
    odin3_scope_id scope;
    char sep;
    bool trailing;
} path_req;

/* One scope component. */
static odin3_status append_scope(odin3_elab2 *e2, const odin3_scope *rec, const path_req *req) {
    const char *name = odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
    const char sep[2] = {req->sep, '\0'};
    odin3_status st =
        req->trailing ? ODIN3_OK : odin3_strbuf_append(&e2->text, odin3_bytes_cstr(sep));
    st = st != ODIN3_OK ? st : odin3_strbuf_append(&e2->text, odin3_bytes_cstr(name));
    if (st == ODIN3_OK && rec->has_index) {
        st = odin3_strbuf_appendf(&e2->text, "[%d]", (int)rec->index);
    }
    return st == ODIN3_OK && req->trailing ? odin3_strbuf_append(&e2->text, odin3_bytes_cstr(sep))
                                           : st;
}

static odin3_status path_append(odin3_elab2 *e2, const path_req *req) {
    odin3_vec_clear(&e2->chain);
    for (odin3_scope_id at = req->scope; at.v != 0;) {
        const odin3_scope *rec = odin3_scope_get(e2->st, at);
        if (rec == NULL || rec->kind == ODIN3_SCOPE_MODULE || rec->kind == ODIN3_SCOPE_LIBRARY ||
            rec->kind == ODIN3_SCOPE_DESIGN) {
            break;
        }
        odin3_scope_id *slot = odin3_vec_push(&e2->chain);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = at;
        at = rec->parent;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = e2->chain.len; st == ODIN3_OK && i > 0; i--) {
        const odin3_scope *rec =
            odin3_scope_get(e2->st, *(const odin3_scope_id *)odin3_vec_cat(&e2->chain, i - 1));
        st = rec->name != 0 ? append_scope(e2, rec, req) : ODIN3_OK;
    }
    return st;
}

/* The source location of node's {loc, end}. */
static void node_srcloc(const odin3_elab2 *e2, const odin3_srcman *sm, odin3_ast_id node,
                        odin3_srcloc *out) {
    odin3_srcman_srcloc(
        sm, (odin3_range){odin3_ast_loc(e2->elab, node), odin3_ast_end(e2->elab, node)}, out);
}

/* The record of req for a module named spec_name (the module itself has no IR module yet). */
static odin3_status prov_for(odin3_elab2 *e2, uint32_t spec_name, const odin3_elab2_prov_req *req,
                             const odin3_elab2_nl_extra *nl, odin3_prov_id *out) {
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(e2->design, &sm);
    odin3_srcloc locs[2];
    odin3_prov_origin origin = {locs, 0, req->node.v, 0};
    if (st == ODIN3_OK && req->node.v != 0) {
        node_srcloc(e2, sm, req->node, &locs[origin.n_locs++]);
    }
    if (st == ODIN3_OK && req->call.v != 0) {
        node_srcloc(e2, sm, req->call, &locs[origin.n_locs++]);
    }
    odin3_strbuf_clear(&e2->text);
    const char *spec = odin3_strtab_get(odin3_design_strtab(e2->design), spec_name);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(&e2->text, odin3_bytes_cstr(spec));
    path_req path = {(odin3_scope_id){req->scope}, E2_HIER_SEP, false};
    st = st != ODIN3_OK ? st : path_append(e2, &path);
    st = st != ODIN3_OK ? st
                        : odin3_design_intern(
                              e2->design, (odin3_bytes){e2->text.data, e2->text.len}, &origin.hier);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!e2->opts.netlist) {
        return odin3_prov_source(e2->ctx, &origin, out);
    }
    /* -netlist: the reader's own record is a netlist line (L-7), then the restored locations */
    odin3_prov_id imported = {0};
    st = odin3_prov_imported(e2->ctx, &origin, &imported);
    return st != ODIN3_OK ? st : odin3_netlist_prov(e2, req, nl, imported, out);
}

odin3_status odin3_elab2_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                              odin3_prov_id *out) {
    return odin3_elab2_prov_nl(e2, req, NULL, out);
}

odin3_status odin3_elab2_prov_nl(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                                 const odin3_elab2_nl_extra *nl, odin3_prov_id *out) {
    if (e2->mod == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_prov: no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_elab2_prov_req inlined = *req;
    if (inlined.call.v == 0) { /* inside a call being inlined: the second loc of EL-45 */
        inlined.call = e2->call;
    }
    return prov_for(e2, e2->mod_name, &inlined, nl, out);
}

/* --- attributes (2A §7) ---------------------------------------------------------------------- */

/* The value of one ATTR: none → INT 1, a NUMBER → BITS, a STRING → STRING. */
static odin3_status attr_value(odin3_elab2 *e2, odin3_ast_id attr, odin3_value *out) {
    odin3_ast_id value = odin3_ast_child(e2->elab, attr, 0);
    switch (odin3_ast_kind_of(e2->elab, value)) {
    case ODIN3_AST_NUMBER: {
        odin3_ast_literal lit;
        odin3_status st = odin3_ast_number_value(e2->elab, value, e2->scratch, &lit);
        *out = lit.value;
        return st;
    }
    case ODIN3_AST_STRING:
        *out = (odin3_value){.kind = ODIN3_VAL_STRING, .str = odin3_ast_name(e2->elab, value)};
        return ODIN3_OK;
    default:
        *out = odin3_value_int(1);
        return ODIN3_OK;
    }
}

/* The attributes of node onto obj, except the ATTR named skip (0: none). */
static odin3_status attrs_apply(odin3_elab2 *e2, odin3_ast_id node, odin3_objref obj,
                                uint32_t skip) {
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        uint32_t key = odin3_ast_name(e2->elab, attrs.ids[i]);
        if (key == skip) {
            continue;
        }
        if (odin3_netlist_src_consumed(e2, attrs.ids[i])) { /* restored as a record (D-3a) */
            continue;
        }
        odin3_value value;
        odin3_status st = attr_value(e2, attrs.ids[i], &value);
        st = st != ODIN3_OK ? st : odin3_attr_set(e2->mod, obj, key, &value);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_elab2_attrs(odin3_elab2 *e2, odin3_ast_id node, odin3_objref obj) {
    if (e2->mod == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_attrs: no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    return attrs_apply(e2, node, obj, 0);
}

/* --- constants (IR-4) ------------------------------------------------------------------------ */

/* The record of the constant cell about to be made: the module's, except under -netlist, where a
 * constant that is the whole rhs of `assign w = 1'bX; // LOC` is that line's cell (a constant met
 * inside an expression is the module's). */
static odin3_status const_prov(odin3_elab2 *e2, odin3_prov_id *prov) {
    *prov = odin3_module_prov(e2->mod);
    if (!e2->asg_const || e2->asg_loc == NULL) {
        return ODIN3_OK;
    }
    const odin3_elab2_prov_req req = {{0}, {0}, e2->scope.v};
    return odin3_netlist_prov(e2, &req, NULL, *prov, prov);
}

odin3_status odin3_elab2_const_net(odin3_elab2 *e2, odin3_bit bit, odin3_net_id *out) {
    if (e2->mod == NULL || (unsigned)bit >= (unsigned)ODIN3_E2_CONSTS) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_const_net: no module or no such constant");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (e2->const_net[bit].v == 0) {
        odin3_prov_id prov = {0};
        odin3_net_id net = {0};
        odin3_status st = const_prov(e2, &prov);
        st = st != ODIN3_OK ? st : odin3_net_create(e2->mod, 0, prov, &net);
        odin3_node_spec spec = {.type = e2->types[(unsigned)ODIN3_E2T_CONST0 + (unsigned)bit],
                                .prov = prov};
        odin3_netvec port = {&net, 1};
        st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, &port, NULL);
        if (st != ODIN3_OK) {
            if (net.v != 0) {
                (void)odin3_net_delete(e2->mod, net);
            }
            return st;
        }
        e2->const_net[bit] = net;
        odin3_elab2_counts made = {1, 1, 1, 0};
        st = odin3_elab2_account(e2, &made);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    *out = e2->const_net[bit];
    return ODIN3_OK;
}

/* --- wires (EL-23) --------------------------------------------------------------------------- */

/* The width of a symbol's packed range; 1 without one. */
static uint32_t symbol_width(const odin3_symbol *rec) {
    if (!rec->has_packed) {
        return 1;
    }
    return (uint32_t)odin3_elab2_dim_size(&rec->packed);
}

/* The IR name of rec: its scope path ('.'-joined) and its own name, interned. */
odin3_status odin3_elab2_scoped_name(odin3_elab2 *e2, const odin3_symbol *rec, uint32_t *name) {
    odin3_strbuf_clear(&e2->text);
    path_req path = {rec->scope, E2_NAME_SEP, true};
    odin3_status st = path_append(e2, &path);
    const char *own = odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(&e2->text, odin3_bytes_cstr(own));
    return st != ODIN3_OK
               ? st
               : odin3_design_intern(e2->design, (odin3_bytes){e2->text.data, e2->text.len}, name);
}

/* The marker ATTR of an implicit net is stage 1's, not the user's: it is not copied. */
static uint32_t implicit_key(const odin3_elab2 *e2, const odin3_symbol *rec) {
    uint32_t key = 0;
    if ((rec->flags & ODIN3_SYM_F_IMPLICIT) == 0 ||
        !odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(k_implicit), &key)) {
        return 0;
    }
    return key;
}

/* Under -netlist, a module-level wire named `\\hier/short@file:line ` takes SHORT (L-5, D-3c), made
 * unique (IR-14); nl_name carries the decoded name for its record. */
static odin3_status netlist_wire_name(odin3_elab2 *e2, const odin3_symbol *rec,
                                      odin3_wire_spec *spec, odin3_netlist_name *nl_name,
                                      odin3_loc at) {
    if (!e2->opts.netlist || spec->name != rec->name) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_netlist_name_decode(e2, rec->name, at, nl_name);
    return st != ODIN3_OK || !nl_name->matched
               ? st
               : odin3_netlist_unique_wire(e2, nl_name->short_str, &spec->name);
}

/* The declaration's attributes, then the declarator's, onto a wire just made. */
typedef struct wire_decl {
    odin3_ast_id decl, declarator;
} wire_decl;

static odin3_status wire_attrs(odin3_elab2 *e2, const odin3_symbol *rec, const wire_decl *wd,
                               odin3_wire_id wire) {
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    odin3_status st = attrs_apply(e2, wd->decl, obj, implicit_key(e2, rec));
    return st != ODIN3_OK ? st : attrs_apply(e2, wd->declarator, obj, 0);
}

/* The wire of one net or variable declarator (a memory makes none: EL-27, Task 20). */
static odin3_status wire_one(odin3_elab2 *e2, const wire_decl *wd) {
    odin3_ast_id decl = wd->decl;
    odin3_ast_id declarator = wd->declarator;
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, declarator);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    if (rec == NULL || rec->ir_wire.v != 0 || rec->n_unpacked > 0) {
        return ODIN3_OK;
    }
    odin3_loc loc = odin3_ast_loc(e2->elab, declarator);
    uint32_t width = symbol_width(rec);
    odin3_wire_spec spec = {.msb = rec->has_packed ? rec->packed.msb : 0,
                            .lsb = rec->has_packed ? rec->packed.lsb : 0,
                            .is_signed = (rec->flags & ODIN3_SYM_F_SIGNED) != 0};
    odin3_status st = odin3_elab2_reserve_nets(e2, width, loc);
    st = st != ODIN3_OK ? st : odin3_elab2_scoped_name(e2, rec, &spec.name);
    if (st == ODIN3_OK && odin3_module_find_wire(e2->mod, spec.name).v != 0) {
        odin3_elab2_error(e2, loc, "'%s' is already declared",
                          odin3_strtab_get(odin3_design_strtab(e2->design), spec.name));
        return ODIN3_OK;
    }
    odin3_netlist_name nl_name = {0};
    st = st != ODIN3_OK ? st : netlist_wire_name(e2, rec, &spec, &nl_name, loc);
    odin3_elab2_prov_req req = {declarator, {0}, rec->scope.v};
    odin3_elab2_nl_extra nl = {decl, &nl_name};
    st = st != ODIN3_OK ? st : odin3_elab2_prov_nl(e2, &req, &nl, &spec.prov);
    odin3_wire_id wire = {0};
    st = st != ODIN3_OK ? st : odin3_wire_create(e2->mod, &spec, NULL, &wire);
    st = st != ODIN3_OK ? st : wire_attrs(e2, rec, wd, wire);
    st = st != ODIN3_OK ? st : odin3_symbol_set_ir(e2->st, sym, e2->mod_id, wire);
    odin3_elab2_counts made = {0, 0, width, 1};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* Every declarator of a NET_DECL or VAR_DECL. */
static odin3_status wires_of(odin3_elab2 *e2, odin3_ast_id decl) {
    odin3_ast_span kids = odin3_ast_children(e2->elab, decl);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < kids.n && !e2->stopped; i++) {
        if (kids.ids[i].v != 0 &&
            odin3_ast_kind_of(e2->elab, kids.ids[i]) == ODIN3_AST_DECLARATOR) {
            st = wire_one(e2, &(wire_decl){decl, kids.ids[i]});
        }
    }
    return st;
}

/* -netlist, before the wires: the raw spelling of every module-level declarator is noted, so a
 * decoded name never takes one (the raw name always wins, as in the BLIF reader). */
static odin3_status raw_names_of(odin3_elab2 *e2, odin3_ast_id decl) {
    odin3_ast_span kids = odin3_ast_children(e2->elab, decl);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < kids.n; i++) {
        if (kids.ids[i].v == 0 ||
            odin3_ast_kind_of(e2->elab, kids.ids[i]) != ODIN3_AST_DECLARATOR) {
            continue;
        }
        const odin3_symbol *rec =
            odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, kids.ids[i]));
        uint32_t name = 0;
        st = rec == NULL ? ODIN3_OK : odin3_elab2_scoped_name(e2, rec, &name);
        st = st == ODIN3_OK && rec != NULL && name == rec->name ? odin3_netlist_note_raw(e2, name)
                                                                : st;
    }
    return st;
}

/* True for a kind of the E class (an expression): no declaration lives below it. */
static bool is_expression(odin3_ast_kind kind) {
    return odin3_ast_class_has(ODIN3_AST_CLS_E, odin3_ast_kind_info_of((uint32_t)kind));
}

/* One NET_DECL or VAR_DECL met by the module walk. */
typedef odin3_status (*decl_fn)(odin3_elab2 *e2, odin3_ast_id decl);

/* Whether the walk goes below node; a declaration is handed to on_decl here (status in *st). */
static bool wires_descend(odin3_elab2 *e2, odin3_ast_id node, decl_fn on_decl, odin3_status *st) {
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
    switch (kind) {
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
        *st = odin3_elab2_pending(e2, node) ? ODIN3_OK : on_decl(e2, node);
        return false;
    case ODIN3_AST_GEN_BLOCK:
    case ODIN3_AST_SEQ_BLOCK:
        return !odin3_elab2_pending(e2, node);
    case ODIN3_AST_PORT_DECL:
    case ODIN3_AST_PARAM_DECL:
    case ODIN3_AST_FUNCTION_DECL: /* inlined where called (EL-38): no wires of their own */
    case ODIN3_AST_TASK_DECL:
    case ODIN3_AST_CONT_ASSIGN:
    case ODIN3_AST_INSTANTIATION:
    case ODIN3_AST_GATE_DECL:
    case ODIN3_AST_ATTR:
        return false;
    default:
        return !is_expression(kind);
    }
}

/* Hands every NET_DECL and VAR_DECL of the module (and its selected generate blocks) to on_decl. */
static odin3_status walk_decls(odin3_elab2 *e2, odin3_ast_id module_node, decl_fn on_decl) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, module_node);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && !e2->stopped && odin3_ast_cursor_next(&cur, &ev, &node)) {
        if (ev == ODIN3_AST_ENTER && !wires_descend(e2, node, on_decl, &st)) {
            odin3_ast_cursor_skip(&cur);
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

odin3_status odin3_elab2_wires(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_status st =
        e2->mod_node.v == module_node.v ? ODIN3_OK : odin3_elab2_enter(e2, module_node);
    st = st == ODIN3_OK && e2->opts.netlist ? walk_decls(e2, module_node, raw_names_of) : st;
    st = st == ODIN3_OK ? walk_decls(e2, module_node, wires_of) : st;
    if (st == ODIN3_OK && e2->stopped) {
        return ODIN3_ERR_PARSE;
    }
    return st != ODIN3_OK ? st : e2->fatal;
}

/* --- modules and ports (EL-24) --------------------------------------------------------------- */

static odin3_dir port_dir(uint32_t sub) {
    switch (sub) {
    case ODIN3_AST_PORT_DECL_OUTPUT:
        return ODIN3_DIR_OUT;
    case ODIN3_AST_PORT_DECL_INOUT:
        return ODIN3_DIR_INOUT;
    default:
        return ODIN3_DIR_IN;
    }
}

/* The IR's answer to a port: the located failure of a module whose ports cannot be built. */
static odin3_status port_failed(odin3_elab2 *e2, odin3_ast_id decl, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    odin3_elab2_error(e2, odin3_ast_loc(e2->elab, decl), "cannot create port '%s' of module '%s'",
                      odin3_ast_name_str(e2->elab, odin3_ast_child(e2->elab, decl, 1)),
                      odin3_ast_name_str(e2->elab, e2->mod_node));
    return ODIN3_ERR_PARSE;
}

/* Hands the spec to the IR, unless the refuse_port test hook has this one refused. */
static odin3_status add_port(odin3_elab2 *e2, const odin3_port_spec *spec, odin3_node_id *node) {
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    e2->ports_made++;
    if (hooks->refuse_port != 0 && hooks->refuse_port == e2->ports_made) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_module_add_port(e2->mod, spec, node);
}

/* The attributes of a port on its wire and node: the PORT_DECL's, then its DECLARATOR's (a
 * non-ANSI port carries those of both declarations), read the same way for every port. */
static odin3_status port_attrs(odin3_elab2 *e2, odin3_ast_id decl, odin3_node_id node) {
    uint32_t index = odin3_module_port_count(e2->mod) - 1U;
    odin3_objref objs[2] = {{ODIN3_OBJ_WIRE, odin3_module_port_wire(e2->mod, index).v},
                            {ODIN3_OBJ_NODE, node.v}};
    odin3_ast_id sources[2] = {decl, odin3_ast_child(e2->elab, decl, 1)};
    odin3_status st = ODIN3_OK;
    for (unsigned obj = 0; st == ODIN3_OK && obj < 2; obj++) {
        for (unsigned src = 0; st == ODIN3_OK && src < 2; src++) {
            st = attrs_apply(e2, sources[src], objs[obj], 0);
        }
    }
    return st;
}

/* One PORT_DECL (one DECLARATOR, EL-4) as an IR port, and its symbol's ir_wire. */
static odin3_status port_one(odin3_elab2 *e2, odin3_ast_id decl) {
    odin3_ast_id declarator = odin3_ast_child(e2->elab, decl, 1);
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, declarator);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    if (rec == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_ports: a port without a symbol (a 2D bug)");
        return ODIN3_ERR_CHECK;
    }
    uint32_t width = symbol_width(rec);
    odin3_port_spec spec = {.name = rec->name,
                            .dir = port_dir(odin3_ast_sub(e2->elab, decl)),
                            .width = rec->has_packed ? 0 : 1,
                            .scalar = !rec->has_packed,
                            .has_range = rec->has_packed,
                            .msb = rec->packed.msb,
                            .lsb = rec->packed.lsb,
                            .is_signed = (rec->flags & ODIN3_SYM_F_SIGNED) != 0};
    odin3_elab2_prov_req req = {decl, {0}, 0};
    odin3_elab2_nl_extra nl = {declarator, NULL};
    odin3_status st = odin3_elab2_reserve_nets(e2, width, odin3_ast_loc(e2->elab, declarator));
    st = st != ODIN3_OK ? st : odin3_elab2_prov_nl(e2, &req, &nl, &spec.prov);
    odin3_node_id node = {0};
    odin3_status made = st != ODIN3_OK ? st : add_port(e2, &spec, &node);
    if (st == ODIN3_OK && made != ODIN3_OK) {
        return port_failed(e2, decl, made);
    }
    st = st != ODIN3_OK ? st : port_attrs(e2, decl, node);
    odin3_wire_id wire = odin3_module_port_wire(e2->mod, odin3_module_port_count(e2->mod) - 1U);
    st = st != ODIN3_OK ? st : odin3_symbol_set_ir(e2->st, sym, e2->mod_id, wire);
    odin3_elab2_counts counts = {1, width, width, 1};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &counts);
}

/* Records the made module: the MODULE node maps to it, and the run lists it. */
static odin3_status register_module(odin3_elab2 *e2, odin3_ast_id node, odin3_module_id id) {
    odin3_module_id *slot = odin3_vec_push(&e2->built);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    return odin3_u64map_put(e2->modules, (odin3_kv){node.v, id.v});
}

/* The IR module of MODULE node (named like it, EL-9) with the node's record and attributes. */
static odin3_status create_module(odin3_elab2 *e2, odin3_ast_id node) {
    uint32_t name = odin3_ast_name(e2->elab, node);
    odin3_celltype_id taken;
    if (odin3_celltype_find(e2->design, name, &taken)) {
        odin3_elab2_error(
            e2, odin3_ast_loc(e2->elab, node),
            "module '%s' has the name of a cell type of the design; rename the module",
            odin3_ast_name_str(e2->elab, node));
        return ODIN3_ERR_PARSE;
    }
    odin3_prov_id prov = {0};
    odin3_elab2_prov_req req = {node, {0}, 0};
    odin3_status st = prov_for(e2, name, &req, NULL, &prov);
    odin3_module_id id = {0};
    st = st != ODIN3_OK ? st : odin3_module_create(e2->design, name, prov, &id);
    st = st != ODIN3_OK ? st : register_module(e2, node, id);
    st = st != ODIN3_OK ? st : odin3_elab2_enter(e2, node);
    odin3_objref obj = {ODIN3_OBJ_MODULE, id.v};
    return st != ODIN3_OK ? st : attrs_apply(e2, node, obj, 0);
}

/* Every port of MODULE node, in port order (the ports LIST is its second slot). */
static odin3_status ports_of(odin3_elab2 *e2, odin3_ast_id node) {
    odin3_ast_id list = odin3_ast_child(e2->elab, node, 1);
    odin3_ast_span ports = odin3_ast_children(e2->elab, list);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ports.n; i++) {
        st = port_one(e2, ports.ids[i]);
    }
    return st;
}

odin3_status odin3_elab2_ports(odin3_elab2 *e2, odin3_ast_id module_node) {
    uint64_t made = 0;
    if (odin3_ast_kind_of(e2->elab, module_node) != ODIN3_AST_MODULE ||
        odin3_u64map_get(e2->modules, module_node.v, &made)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_ports: not a MODULE node, or its ports exist");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_elab2_is_blackbox(e2, module_node)) {
        return odin3_elab2_blackbox(e2, module_node); /* a cell type (EL-11), never an IR module */
    }
    odin3_status st = odin3_elab2_budget(e2);
    st = st != ODIN3_OK ? st : create_module(e2, module_node);
    return st != ODIN3_OK ? st : ports_of(e2, module_node);
}
