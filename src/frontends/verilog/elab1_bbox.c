/*
 * elab1_bbox.c — black boxes in stage 1 (2D spec §3.6, EL-11; EL-4 v4.8; plan Task 12 and its
 * fix round, ruling 4): the black-box table (`(* blackbox *)` specializations and implicit black
 * boxes, with their ports), the body-ignored info line, the implicit black boxes' port unions and
 * widths (--auto-blackbox), and the ports-only black-box MODULEs written into the elaborated
 * store, from which stage 2 registers black-box cell types (IR-7b).
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { E1_ITEMS = 2, E1_KEY_SHIFT = 32 };

static const uint32_t E1_NONE = UINT32_MAX;
static const uint64_t E1_SPEC_KEY = (uint64_t)1 << 63;
static const char k_blackbox[] = "blackbox";

static const char *str_of(const odin3_elab1 *e1, uint32_t id) {
    return odin3_elab1_str(e1, id);
}

/* The connections of an INSTANCE (after its array slot). */
static odin3_ast_span inst_conns(const odin3_elab1 *e1, odin3_ast_id instance) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, instance);
    uint32_t from = odin3_ast_slot_count(ODIN3_AST_INSTANCE);
    return kids.n > from ? (odin3_ast_span){kids.ids + from, kids.n - from}
                         : (odin3_ast_span){NULL, 0};
}

/* --- the table ------------------------------------------------------------------------------ */

const odin3_elab1_bbox *odin3_elab1_bbox_at(const odin3_elab1 *e1, uint32_t idx) {
    return idx < e1->bboxes.len ? odin3_vec_cat(&e1->bboxes, idx) : NULL;
}

const odin3_elab1_bbport *odin3_elab1_bbport_at(const odin3_elab1 *e1, uint32_t idx) {
    return idx < e1->bbports.len ? odin3_vec_cat(&e1->bbports, idx) : NULL;
}

uint32_t odin3_elab1_implicit_box(const odin3_elab1 *e1, uint32_t name) {
    uint64_t at = 0;
    return odin3_u64map_get(e1->bbox_names, name, &at) ? (uint32_t)at : 0;
}

uint32_t odin3_elab1_bbox_port(const odin3_elab1 *e1, uint32_t box, uint32_t name) {
    uint64_t at = 0;
    return odin3_u64map_get(e1->bbport_ix, (uint64_t)box << E1_KEY_SHIFT | name, &at) ? (uint32_t)at
                                                                                      : 0;
}

/* A new black box keyed key (an implicit name, or E1_SPEC_KEY | spec) in *idx. */
static odin3_status bbox_new(odin3_elab1 *e1, uint64_t key, const odin3_elab1_bbox *proto,
                             uint32_t *idx) {
    odin3_elab1_bbox *slot = odin3_vec_push(&e1->bboxes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *proto;
    *idx = (uint32_t)e1->bboxes.len - 1;
    odin3_status st = odin3_u64map_put(e1->bbox_names, (odin3_kv){key, e1->bboxes.len});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&e1->bboxes);
    }
    return st;
}

/* A port appended to black box box (its chain kept in order). */
static odin3_status bbport_add(odin3_elab1 *e1, uint32_t box, const odin3_elab1_port *port,
                               odin3_ast_id at) {
    odin3_elab1_bbport *slot = odin3_vec_push(&e1->bbports);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_elab1_bbport){*port, at, 0};
    uint32_t self = (uint32_t)e1->bbports.len;
    odin3_kv entry = {(uint64_t)box << E1_KEY_SHIFT | port->name, self};
    odin3_status st = port->name != 0 ? odin3_u64map_put(e1->bbport_ix, entry) : ODIN3_OK;
    if (st != ODIN3_OK) {
        odin3_vec_pop(&e1->bbports);
        return st;
    }
    odin3_elab1_bbox *rec = odin3_vec_at(&e1->bboxes, box);
    if (rec->last_port != 0) {
        ((odin3_elab1_bbport *)odin3_vec_at(&e1->bbports, rec->last_port - 1))->next = self;
    } else {
        rec->first_port = self;
    }
    rec->last_port = self;
    rec->n_ports++;
    return ODIN3_OK;
}

/* --- the ports-only black-box MODULE (EL-4 v4.8) ------------------------------------------ */

/* The writer of one black-box MODULE: its scratch arena and its status (the first failure). */
typedef struct e1b_writer {
    odin3_elab1 *e1;
    odin3_arena *arena;
    odin3_status st;
} e1b_writer;

static odin3_ast_spec bw_spec(const e1b_writer *bw, uint32_t kind, odin3_ast_id from) {
    return (odin3_ast_spec){.kind = (uint8_t)kind,
                            .loc = odin3_ast_loc(bw->e1->parsed, from).v,
                            .end = odin3_ast_end(bw->e1->parsed, from).v};
}

static void bw_push(e1b_writer *bw, odin3_ast_id id) {
    if (bw->st == ODIN3_OK) {
        bw->st = odin3_ast_push(bw->e1->elab, id);
    }
}

/* A node whose children are the pending span from mark (the elaborated-node cap first). */
static odin3_ast_id bw_make(e1b_writer *bw, const odin3_ast_spec *spec, uint32_t mark,
                            odin3_ast_id from) {
    odin3_ast_id id = {0};
    if (bw->st == ODIN3_OK && !odin3_elab1_node_room(bw->e1, from)) {
        bw->st = ODIN3_ERR_PARSE;
    }
    if (bw->st == ODIN3_OK) {
        bw->st = odin3_ast_make_marked(bw->e1->elab, spec, mark, &id);
    }
    return id;
}

static odin3_ast_id bw_leaf(e1b_writer *bw, const odin3_cval *val, int64_t num, odin3_ast_id from) {
    odin3_ast_id id = {0};
    odin3_elab1_leaf leaf = {val, ODIN3_AST_NUMBER_BIN, num, from};
    if (bw->st == ODIN3_OK) {
        bw->st = odin3_elab1_make_leaf(bw->e1, bw->arena, &leaf, &id);
    }
    return id;
}

/* PORT_DECL{IN_HEADER}[dir] of a `wire` with one DECLARATOR and its NUMBER range (none when the
 * port is scalar), at from. */
static odin3_ast_id bw_port(e1b_writer *bw, const odin3_elab1_port *port, odin3_ast_id from) {
    uint32_t mark = odin3_ast_mark(bw->e1->elab);
    odin3_ast_id range = {0};
    if (!port->scalar) {
        uint32_t bounds = odin3_ast_mark(bw->e1->elab);
        bw_push(bw, bw_leaf(bw, NULL, port->msb, from));
        bw_push(bw, bw_leaf(bw, NULL, port->lsb, from));
        odin3_ast_spec spec = bw_spec(bw, ODIN3_AST_RANGE, from);
        range = bw_make(bw, &spec, bounds, from);
    }
    bw_push(bw, range);
    uint32_t decl_mark = odin3_ast_mark(bw->e1->elab);
    bw_push(bw, (odin3_ast_id){0});
    odin3_ast_spec decl = bw_spec(bw, ODIN3_AST_DECLARATOR, from);
    decl.name = port->name;
    bw_push(bw, bw_make(bw, &decl, decl_mark, from));
    odin3_ast_spec spec = bw_spec(bw, ODIN3_AST_PORT_DECL, from);
    spec.sub = port->dir;
    spec.flags = (uint16_t)((uint32_t)ODIN3_AST_F_PORT_DECL_IN_HEADER |
                            (1U + (uint32_t)ODIN3_AST_NET_DECL_WIRE)
                                << (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT |
                            (port->is_signed ? (uint32_t)ODIN3_AST_F_PORT_DECL_SIGNED : 0U));
    return bw_make(bw, &spec, mark, from);
}

/* The ports LIST of black box box, at from. */
static odin3_ast_id bw_ports(e1b_writer *bw, const odin3_elab1_bbox *box, odin3_ast_id from) {
    uint32_t mark = odin3_ast_mark(bw->e1->elab);
    for (uint32_t at = box->first_port; at != 0 && bw->st == ODIN3_OK;) {
        const odin3_elab1_bbport *port = odin3_elab1_bbport_at(bw->e1, at - 1);
        bw_push(bw, bw_port(bw, &port->port, port->port.decl.v != 0 ? port->port.decl : port->at));
        at = port->next;
    }
    odin3_ast_spec spec = bw_spec(bw, ODIN3_AST_LIST, from);
    return bw_make(bw, &spec, mark, from);
}

/* The params LIST of a `(* blackbox *)` specialization: PARAM_DECL[localparam]{IN_HEADER} with one
 * DECLARATOR per `parameter`, its final value (point (bb)); absent without parameters. */
static odin3_ast_id bw_params(e1b_writer *bw, const odin3_elab1_spec *spec, odin3_ast_id from) {
    odin3_elab1 *e1 = bw->e1;
    if (spec->n_values == 0) {
        return (odin3_ast_id){0};
    }
    const odin3_elab_def *def = odin3_elab1_def(e1, spec->def);
    uint32_t mark = odin3_ast_mark(e1->elab);
    for (uint32_t k = 0; k < spec->n_values && bw->st == ODIN3_OK; k++) {
        odin3_ast_id decl = *(const odin3_ast_id *)odin3_vec_cat(&e1->params, def->first_param + k);
        uint32_t group = odin3_ast_mark(e1->elab);
        bw_push(bw, (odin3_ast_id){0});
        uint32_t value = odin3_ast_mark(e1->elab);
        odin3_elab1_leaf leaf = {&spec->values[k], spec->bases[k], 0, decl};
        odin3_ast_id node = {0};
        bw->st = bw->st != ODIN3_OK ? bw->st : odin3_elab1_make_leaf(e1, bw->arena, &leaf, &node);
        bw_push(bw, node);
        odin3_ast_spec dspec = bw_spec(bw, ODIN3_AST_DECLARATOR, decl);
        dspec.name = odin3_ast_name(e1->parsed, decl);
        bw_push(bw, bw_make(bw, &dspec, value, decl));
        odin3_ast_spec pspec = bw_spec(bw, ODIN3_AST_PARAM_DECL, decl);
        pspec.sub = ODIN3_AST_PARAM_DECL_LOCALPARAM;
        pspec.flags = ODIN3_AST_F_PARAM_DECL_IN_HEADER;
        bw_push(bw, bw_make(bw, &pspec, group, decl));
    }
    odin3_ast_spec list = bw_spec(bw, ODIN3_AST_LIST, from);
    return bw_make(bw, &list, mark, from);
}

/* A STRING node holding the name base, at from (a carrier's `blackbox` value). */
static odin3_ast_id bw_string(e1b_writer *bw, uint32_t base, odin3_ast_id from) {
    odin3_ast_spec spec = bw_spec(bw, ODIN3_AST_STRING, from);
    spec.name = base;
    return bw_make(bw, &spec, odin3_ast_mark(bw->e1->elab), from);
}

/* The `blackbox` ATTR (synthesized: the source's stays a pending kind): without a value for a
 * definition's own MODULE, with the definition's name as a STRING for a carrier (base != 0). */
static odin3_ast_id bw_attr(e1b_writer *bw, uint32_t base, odin3_ast_id from) {
    odin3_ast_spec spec = bw_spec(bw, ODIN3_AST_ATTR, from);
    odin3_ast_id value = base != 0 ? bw_string(bw, base, from) : (odin3_ast_id){0};
    odin3_ast_id attr = {0};
    if (bw->st == ODIN3_OK && !odin3_elab1_node_room(bw->e1, from)) {
        bw->st = ODIN3_ERR_PARSE;
    }
    bw->st = bw->st != ODIN3_OK
                 ? bw->st
                 : odin3_ast_intern(bw->e1->elab, odin3_bytes_cstr(k_blackbox), &spec.name);
    bw->st = bw->st != ODIN3_OK ? bw->st : odin3_ast_make(bw->e1->elab, &spec, &value, 1, &attr);
    return attr;
}

/* Black box idx as a ports-only MODULE on the store's pending stack (EL-4 v4.8), at from. */
static odin3_status bw_module(odin3_elab1 *e1, uint32_t idx, odin3_ast_id from) {
    e1b_writer bw = {e1, odin3_arena_create(0), ODIN3_OK};
    if (bw.arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_elab1_bbox *box = odin3_elab1_bbox_at(e1, idx);
    const odin3_elab1_spec *spec = box->implicit ? NULL : odin3_elab1_spec_at(e1, box->spec);
    uint32_t mark = odin3_ast_mark(e1->elab);
    bw_push(&bw, spec != NULL ? bw_params(&bw, spec, from) : (odin3_ast_id){0});
    bw_push(&bw, bw_ports(&bw, box, from));
    odin3_ast_spec mspec = bw_spec(&bw, ODIN3_AST_MODULE, from);
    mspec.name = box->name;
    odin3_ast_id module = bw_make(&bw, &mspec, mark, from);
    odin3_ast_id attr = bw_attr(&bw, box->base, from);
    bw.st = bw.st != ODIN3_OK ? bw.st : odin3_ast_attach(e1->elab, module, &attr, 1);
    bw_push(&bw, module);
    odin3_arena_destroy(bw.arena);
    return bw.st;
}

/* Whether the store is still being built (no error so far). */
static bool building(const odin3_elab1 *e1) {
    return e1->elab != NULL && e1->errors == 0 && e1->fatal == ODIN3_OK && !e1->stopped;
}

/* --- `(* blackbox *)` specializations ------------------------------------------------------ */

/* Whether a `(* blackbox *)` definition has a body to ignore: an item other than a port or
 * parameter declaration. */
static bool has_body(const odin3_elab1 *e1, odin3_ast_id module) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, module);
    for (uint32_t i = E1_ITEMS; i < kids.n; i++) {
        odin3_ast_kind kind = odin3_ast_kind_of(e1->parsed, kids.ids[i]);
        if (kind != ODIN3_AST_PORT_DECL && kind != ODIN3_AST_PARAM_DECL) {
            return true;
        }
    }
    return false;
}

/* The info line once per definition when its body is not empty (EL-11). */
static void note_body(odin3_elab1 *e1, uint32_t def) {
    odin3_elab1_defx *ext = odin3_vec_at(&e1->defx, def);
    if (ext->body_noted) {
        return;
    }
    ext->body_noted = true;
    const odin3_elab_def *rec = odin3_elab1_def(e1, def);
    if (has_body(e1, rec->node)) {
        odin3_elab1_info(e1, odin3_ast_loc(e1->parsed, rec->node),
                         "body of black box '%s' is ignored", str_of(e1, rec->name));
    }
}

odin3_status odin3_elab1_blackbox(odin3_elab1 *e1, uint32_t spec) {
    const odin3_elab1_spec *rec = odin3_elab1_spec_at(e1, spec);
    const odin3_elab_def *def = odin3_elab1_def(e1, rec->def);
    note_body(e1, rec->def);
    uint32_t count = def->n_positions;
    odin3_elab1_port *ports =
        odin3_arena_alloc(e1->arena, (size_t)(count > 0 ? count : 1) * sizeof *ports);
    odin3_status st =
        ports != NULL ? odin3_elab1_eval_ports(e1, spec, false, ports) : ODIN3_ERR_NO_MEMORY;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab1_bbox proto = {.name = rec->name, .spec = spec, .at = def->node};
    uint32_t box = 0;
    st = bbox_new(e1, E1_SPEC_KEY | spec, &proto, &box);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        st = ports[k].name != 0 ? bbport_add(e1, box, &ports[k], def->node) : ODIN3_OK;
    }
    return st == ODIN3_OK && building(e1) ? bw_module(e1, box, def->node) : st;
}

/* --- carriers: the other parameter sets of a `(* blackbox *)` definition (v4.19) ----------------
 */

/* Specialization spec's carrier MODULE: its ports under the set's values (the connection check's
 * own, quiet), in the black-box table and written on the store's pending stack. */
static odin3_status write_carrier(odin3_elab1 *e1, uint32_t spec) {
    const odin3_elab1_spec *rec = odin3_elab1_spec_at(e1, spec);
    odin3_ast_id node = odin3_elab1_def(e1, rec->def)->node;
    uint32_t count = 0;
    const odin3_elab1_port *ports = odin3_elab1_spec_ports(e1, spec, &count);
    if (ports == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_elab1_bbox proto = {.name = rec->carrier, .base = rec->name, .spec = spec, .at = node};
    uint32_t box = 0;
    odin3_status st = bbox_new(e1, E1_SPEC_KEY | spec, &proto, &box);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        st = ports[k].name != 0 ? bbport_add(e1, box, &ports[k], node) : ODIN3_OK;
    }
    return st == ODIN3_OK ? bw_module(e1, box, node) : st;
}

odin3_status odin3_elab1_write_carriers(odin3_elab1 *e1) {
    odin3_status st = ODIN3_OK;
    for (uint32_t spec = 0; st == ODIN3_OK && building(e1) && spec < e1->specs.len; spec++) {
        const odin3_elab1_spec *rec = odin3_elab1_spec_at(e1, spec);
        bool skip =
            rec->carrier == 0 || odin3_elab1_pending(e1, odin3_elab1_def(e1, rec->def)->node);
        st = skip ? ODIN3_OK : write_carrier(e1, spec);
    }
    return st;
}

/* --- implicit black boxes (--auto-blackbox) ------------------------------------------------- */

/* The connections of every INSTANCE of item add their names to black box box (first use). */
static odin3_status union_of(odin3_elab1 *e1, uint32_t box, odin3_ast_id item) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, item);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); st == ODIN3_OK && i < kids.n;
         i++) {
        odin3_ast_span conns = inst_conns(e1, kids.ids[i]);
        for (uint32_t j = 0; st == ODIN3_OK && j < conns.n; j++) {
            uint32_t name = odin3_ast_name(e1->parsed, conns.ids[j]);
            odin3_elab1_port port = {.name = name, .dir = ODIN3_DIR_INOUT};
            bool fresh = name != 0 && odin3_elab1_bbox_port(e1, box, name) == 0;
            st = fresh ? bbport_add(e1, box, &port, conns.ids[j]) : ODIN3_OK;
        }
    }
    return st;
}

/* The implicit black box named name, made on first use with the union of the names every
 * instantiation of it in the sources connects (EL-11; source order; black-box bodies excluded). */
static odin3_status implicit_get(odin3_elab1 *e1, const odin3_elab_inst *inst, uint32_t *box) {
    uint32_t name = odin3_ast_name(e1->parsed, inst->instantiation);
    uint32_t at = odin3_elab1_implicit_box(e1, name);
    if (at != 0) {
        *box = at - 1;
        return ODIN3_OK;
    }
    odin3_elab1_bbox proto = {
        .name = name, .spec = E1_NONE, .implicit = true, .at = inst->instantiation};
    odin3_status st = bbox_new(e1, name, &proto, box);
    for (uint32_t def = 0; st == ODIN3_OK && def < odin3_elab1_def_count(e1); def++) {
        const odin3_elab1_defx *ext = odin3_vec_cat(&e1->defx, def);
        bool body = odin3_elab1_def(e1, def)->kind == ODIN3_DEF_MODULE;
        for (uint32_t i = 0; st == ODIN3_OK && body && i < ext->n_inst; i++) {
            odin3_ast_id item =
                *(const odin3_ast_id *)odin3_vec_cat(&e1->insts, ext->first_inst + i);
            st = odin3_ast_name(e1->parsed, item) == name ? union_of(e1, *box, item) : ODIN3_OK;
        }
    }
    return st;
}

/* One implicit instance being checked: whether any connection failed. */
typedef struct e1b_auto {
    odin3_elab1 *e1;
    const odin3_elab_inst *inst;
    bool bad;
} e1b_auto;

/* One named connection's width against its port's (fixed by the first connection with an
 * expression: from an array of n elements, a width that n divides is n slices of width / n, as
 * for a declared port (EL-11); a later connection may be one element's width or all of them). */
static odin3_status auto_port(e1b_auto *ax, uint32_t box, odin3_ast_id conn) {
    odin3_elab1 *e1 = ax->e1;
    uint32_t name = odin3_ast_name(e1->parsed, conn);
    uint32_t at = odin3_elab1_bbox_port(e1, box, name);
    odin3_ast_id expr = odin3_ast_child(e1->parsed, conn, 0);
    uint32_t width = 0;
    odin3_status st = expr.v != 0 ? odin3_elab1_expr_width(e1, expr, &width) : ODIN3_OK;
    if (st != ODIN3_OK || expr.v == 0 || at == 0) {
        return st;
    }
    odin3_elab1_bbport *rec = odin3_vec_at(&e1->bbports, at - 1);
    uint32_t want = rec->port.width;
    if (want == 0) {
        if (e1->array.n > 1 && width % e1->array.n == 0) {
            width /= e1->array.n;
        }
        rec->port.width = width;
        rec->port.msb = (int64_t)width - 1;
        rec->port.scalar = width == 1;
        return ODIN3_OK;
    }
    if (width == want || width == (uint64_t)e1->array.n * want) {
        return ODIN3_OK;
    }
    odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, conn),
                      "implicit black box '%s': port '%s' is %" PRIu32
                      " bits at its first instance; this connection is %" PRIu32 " bits",
                      odin3_ast_name_str(e1->parsed, ax->inst->instantiation), str_of(e1, name),
                      want, width);
    ax->bad = true;
    return ODIN3_OK;
}

/* An implicit black box's instance needs named connections. */
static odin3_status auto_named(const e1b_auto *ax) {
    odin3_elab1 *e1 = ax->e1;
    odin3_ast_span conns = inst_conns(e1, ax->inst->instance);
    for (uint32_t i = 0; i < conns.n; i++) {
        if (odin3_ast_name(e1->parsed, conns.ids[i]) == 0) {
            odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, ax->inst->instance),
                              "implicit black box '%s' needs named connections",
                              odin3_ast_name_str(e1->parsed, ax->inst->instantiation));
            return ODIN3_ERR_PARSE;
        }
    }
    return ODIN3_OK;
}

/* Each port once: "port 'p' of instance 'u' is connected twice", then the widths. */
static odin3_status auto_ports(e1b_auto *ax, uint32_t box) {
    odin3_elab1 *e1 = ax->e1;
    odin3_ast_span conns = inst_conns(e1, ax->inst->instance);
    uint64_t self = (uint64_t)ax->inst->instance.v << E1_KEY_SHIFT;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < conns.n; i++) {
        uint32_t port = odin3_ast_name(e1->parsed, conns.ids[i]);
        if (odin3_u64map_get(e1->conn_seen, self | port, NULL)) {
            odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, conns.ids[i]),
                              "port '%s' of instance '%s' is connected twice", str_of(e1, port),
                              odin3_ast_name_str(e1->parsed, ax->inst->instance));
            ax->bad = true;
            continue;
        }
        st = odin3_u64map_put(e1->conn_seen, (odin3_kv){self | port, 1});
        st = st != ODIN3_OK ? st : auto_port(ax, box, conns.ids[i]);
    }
    for (uint32_t i = 0; i < conns.n; i++) {
        (void)odin3_u64map_remove(e1->conn_seen, self | odin3_ast_name(e1->parsed, conns.ids[i]));
    }
    return st;
}

odin3_status odin3_elab1_auto_blackbox(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    e1b_auto ax = {e1, inst, false};
    uint32_t box = 0;
    odin3_status st = auto_named(&ax);
    st = st != ODIN3_OK ? st : odin3_elab1_fold_array(e1, inst->instance, &e1->array);
    st = st != ODIN3_OK ? st : implicit_get(e1, inst, &box);
    st = st != ODIN3_OK ? st : auto_ports(&ax, box);
    return st == ODIN3_OK && ax.bad ? ODIN3_ERR_PARSE : st;
}

odin3_status odin3_elab1_write_implicit(odin3_elab1 *e1) {
    odin3_status st = building(e1) ? odin3_elab1_infer_directions(e1) : ODIN3_OK;
    for (uint32_t idx = 0; st == ODIN3_OK && building(e1) && idx < e1->bboxes.len; idx++) {
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(e1, idx);
        if (!box->implicit) {
            continue;
        }
        for (uint32_t at = box->first_port; at != 0;) {
            odin3_elab1_bbport *port = odin3_vec_at(&e1->bbports, at - 1);
            if (port->port.width == 0) { /* no connection gave one (EL-11): 1 bit */
                port->port = (odin3_elab1_port){
                    .name = port->port.name, .dir = port->port.dir, .width = 1, .scalar = true};
            }
            at = port->next;
        }
        st = bw_module(e1, idx, box->at);
    }
    return st;
}
