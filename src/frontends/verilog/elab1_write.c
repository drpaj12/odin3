/*
 * elab1_write.c — pass W (2D spec §3.4, EL-4): one specialization's MODULE written through the 2A
 * builders, bottom up, replaying pass E's decision log. Task 11 writes the header parameters as
 * PARAM_DECL[localparam]{IN_HEADER} with their final values (point (bb)'s spellings), the ANSI
 * ports one PORT_DECL per declarator, NET_DECL and VAR_DECL with NUMBER bounds, CONT_ASSIGN, the
 * body's parameters and localparams, and each INSTANTIATION named after its specialization with
 * one named CONNECTION per parameter and per port; expressions as written, but with the select
 * bounds and widths and the replication counts folded to NUMBERs (the elaborated form's resolved
 * constants). A kind not landed yet is reported (point (l)); after any error nothing is built,
 * though the walk goes on to report.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    E1_SLOT_PARAMS = 0,
    E1_SLOT_PORTS = 1,
    E1_ITEMS = 2,
    E1_INT_BITS = 32,
    E1_NET_RANGE_SLOT = 1,
    E1_NET_DELAY_SLOT = 2
};

static const uint32_t E1_NONE = UINT32_MAX;

/* The writer of one specialization. */
typedef struct e1w {
    odin3_elab1 *e1;
    odin3_elab1_env *env;
    const odin3_ast *ast; /* parsed */
    odin3_ast *out;       /* elaborated */
    const odin3_elab1_spec *spec;
    bool building;
    odin3_status st;
    uint32_t cursor; /* the next decision-log entry */
    uint32_t used;
    bool replay_failed;
    bool dropped;
} e1w;

static bool live(const e1w *wr) {
    return wr->building && wr->st == ODIN3_OK && !wr->e1->stopped;
}

static void halt(e1w *wr) {
    wr->building = false;
}

static void fail(e1w *wr, odin3_status st) {
    if (st != ODIN3_OK && wr->st == ODIN3_OK) {
        wr->st = st;
    }
    wr->building = false;
}

/* A failed step: a diagnostic was reported (PARSE: stop building) or worse (the status). */
static void stop_on(e1w *wr, odin3_status st) {
    if (st == ODIN3_ERR_PARSE) {
        halt(wr);
    } else {
        fail(wr, st);
    }
}

static odin3_ast_spec spec_of(const e1w *wr, uint32_t kind, odin3_ast_id from) {
    return (odin3_ast_spec){.kind = (uint8_t)kind,
                            .loc = odin3_ast_loc(wr->ast, from).v,
                            .end = odin3_ast_end(wr->ast, from).v};
}

static uint32_t w_mark(const e1w *wr) {
    return live(wr) ? odin3_ast_mark(wr->out) : 0;
}

static void w_push(e1w *wr, odin3_ast_id id) {
    if (live(wr)) {
        odin3_status st = odin3_ast_push(wr->out, id);
        if (st != ODIN3_OK) {
            fail(wr, st);
        }
    }
}

/* The elaborated-node cap before a make (point (aa)): fatal at the node being written. */
static bool w_room(e1w *wr, odin3_ast_id from) {
    uint32_t made = odin3_ast_node_end(wr->out).v - 1;
    if (made < wr->e1->limits->max_elab_nodes) {
        return true;
    }
    odin3_elab1_fatal(wr->e1, odin3_ast_loc(wr->ast, from),
                      "elaboration needs more than %" PRIu32 " elaborated nodes",
                      wr->e1->limits->max_elab_nodes);
    halt(wr);
    return false;
}

static odin3_ast_id w_make_marked(e1w *wr, const odin3_ast_spec *spec, uint32_t mark,
                                  odin3_ast_id from) {
    odin3_ast_id id = {0};
    if (!live(wr) || !w_room(wr, from)) {
        return id;
    }
    odin3_status st = odin3_ast_make_marked(wr->out, spec, mark, &id);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
    return id;
}

/* --- constants (point (bb)) ----------------------------------------------------------------- */

/* A NUMBER payload made from num, at from's location. */
static odin3_ast_id w_number(e1w *wr, const odin3_ast_number *num, odin3_ast_spec *spec,
                             odin3_ast_id from) {
    odin3_status st =
        odin3_ast_number_new(wr->out, odin3_ast_loc(wr->ast, from), num, &spec->payload);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    return w_make_marked(wr, spec, odin3_ast_mark(wr->out), from);
}

/* A known 32-bit value as a NUMBER: sub DEC, SIGNED, payload width 32 (the integer form). */
static odin3_ast_id w_int(e1w *wr, int64_t value, odin3_ast_id from) {
    if (!live(wr)) {
        return (odin3_ast_id){0};
    }
    odin3_cval_int num = {value, E1_INT_BITS, true};
    odin3_cval val = {0};
    odin3_ast_number payload;
    odin3_status st = odin3_cval_from_i64(wr->env->arena, &num, &val);
    st = st != ODIN3_OK ? st : odin3_cval_to_number(wr->env->arena, &val, &payload);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NUMBER, from);
    spec.sub = ODIN3_AST_NUMBER_DEC;
    spec.flags = ODIN3_AST_F_NUMBER_SIGNED;
    return w_number(wr, &payload, &spec, from);
}

/* A STRING value as a STRING node (the empty string: EMPTY, no name). */
static odin3_ast_id w_string(e1w *wr, const odin3_cval *val, odin3_ast_id from) {
    odin3_bytes bytes = {NULL, 0};
    odin3_status st = odin3_cval_string_bytes(wr->env->arena, val, &bytes);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_STRING, from);
    if (st == ODIN3_OK && bytes.len == 0) {
        spec.flags = ODIN3_AST_F_STRING_EMPTY;
    } else if (st == ODIN3_OK) {
        st = odin3_ast_intern(wr->out, bytes, &spec.name);
    }
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    return w_make_marked(wr, &spec, odin3_ast_mark(wr->out), from);
}

/* A final value (a parameter's, a CONNECTION's): a STRING, or a NUMBER whose sub is the source
 * literal's base while unsized (payload width 0) and BIN otherwise, SIGNED per value. */
static odin3_ast_id w_value(e1w *wr, const odin3_cval *val, uint8_t base, odin3_ast_id from) {
    if (!live(wr)) {
        return (odin3_ast_id){0};
    }
    if ((val->flags & ODIN3_CVAL_STRING) != 0) {
        return w_string(wr, val, from);
    }
    odin3_ast_number payload;
    odin3_status st = odin3_cval_to_number(wr->env->arena, val, &payload);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NUMBER, from);
    spec.sub = (val->flags & ODIN3_CVAL_UNSIZED) != 0 ? base : (uint8_t)ODIN3_AST_NUMBER_BIN;
    spec.flags = (val->flags & ODIN3_CVAL_SIGNED) != 0 ? ODIN3_AST_F_NUMBER_SIGNED : 0;
    return w_number(wr, &payload, &spec, from);
}

/* A declaration's RANGE with its folded bounds (pass E folded them). */
static odin3_ast_id w_range(e1w *wr, odin3_ast_id range) {
    if (range.v == 0) {
        return range;
    }
    int64_t msb = 0;
    int64_t lsb = 0;
    odin3_status st = odin3_elab1_env_range(wr->env, range, &msb, &lsb);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_int(wr, msb, odin3_ast_child(wr->ast, range, 0)));
    w_push(wr, w_int(wr, lsb, odin3_ast_child(wr->ast, range, 1)));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_RANGE, range);
    return w_make_marked(wr, &spec, mark, range);
}

/* --- expressions ---------------------------------------------------------------------------- */

static bool copyable(odin3_ast_kind kind) {
    switch (kind) {
    case ODIN3_AST_NUMBER:
    case ODIN3_AST_STRING:
    case ODIN3_AST_IDENT:
    case ODIN3_AST_SELECT:
    case ODIN3_AST_CONCAT:
    case ODIN3_AST_REPLICATE:
    case ODIN3_AST_UNARY:
    case ODIN3_AST_BINARY:
    case ODIN3_AST_TERNARY:
    case ODIN3_AST_CALL:
        return true;
    default:
        return false;
    }
}

/* Whether node is walked: a kind not landed is reported (pending, or a 2D bug); an attribute on
 * it is pending (Task 15), the walk goes on to report the rest. */
static bool w_enter(e1w *wr, odin3_ast_id node) {
    if (odin3_elab1_pending(wr->e1, node)) {
        halt(wr);
        return false;
    }
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    if (!copyable(kind)) {
        odin3_elab1_unhandled(wr->e1, node);
        halt(wr);
        return false;
    }
    if (kind == ODIN3_AST_CALL && (odin3_ast_flags(wr->ast, node) & ODIN3_AST_F_CALL_HIER) != 0) {
        odin3_elab1_pend_form(wr->e1, node); /* E21: Task 16 */
        halt(wr);
        return false;
    }
    odin3_ast_span attrs = odin3_ast_attrs(wr->ast, node);
    if (attrs.n > 0) {
        odin3_elab1_not_yet(wr->e1, attrs.ids[0]);
        halt(wr);
    }
    return true;
}

/* The constant role of child idx of node when the elaborated form needs it folded (a part
 * select's bounds, a +:/-: width, a replication count); NULL when it is copied. */
static const char *fold_role(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    if (kind == ODIN3_AST_REPLICATE) {
        return idx == 0 ? "replication count" : NULL;
    }
    if (kind != ODIN3_AST_SELECT) {
        return NULL;
    }
    switch (odin3_ast_sub(ast, node)) {
    case ODIN3_AST_SELECT_PART:
        return idx >= 1 ? "range bound" : NULL;
    case ODIN3_AST_SELECT_PART_PLUS:
    case ODIN3_AST_SELECT_PART_MINUS:
        return idx == 2 ? "range bound" : NULL;
    default:
        return NULL;
    }
}

static odin3_ast_id w_fold(e1w *wr, odin3_ast_id expr, const char *role) {
    int32_t value = 0;
    odin3_status st = odin3_elab1_fold_int(wr->env, expr, role, &value);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    if (value < 0 && strcmp(role, "replication count") == 0) {
        odin3_elab1_error(wr->e1, odin3_ast_loc(wr->ast, expr),
                          "replication count %" PRId32 " is negative", value);
        halt(wr);
        return (odin3_ast_id){0};
    }
    return w_int(wr, value, expr);
}

/* A copy of node (a leaf or an operator) whose children are the pending span from mark. */
static odin3_ast_id w_copy_node(e1w *wr, odin3_ast_id node, uint32_t mark) {
    if (!live(wr)) {
        return (odin3_ast_id){0};
    }
    odin3_ast_spec spec = spec_of(wr, odin3_ast_kind_of(wr->ast, node), node);
    spec.sub = (uint8_t)odin3_ast_sub(wr->ast, node);
    spec.flags = odin3_ast_flags(wr->ast, node);
    spec.name = odin3_ast_name(wr->ast, node);
    if (spec.kind == ODIN3_AST_NUMBER) {
        return w_number(wr, odin3_ast_number_of(wr->ast, node), &spec, node);
    }
    return w_make_marked(wr, &spec, mark, node);
}

static bool push_frame(e1w *wr, odin3_ast_id node) {
    odin3_elab1_frame *frame = odin3_vec_push(&wr->e1->frames);
    if (frame == NULL) {
        fail(wr, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    *frame = (odin3_elab1_frame){node.v, 0, w_mark(wr)};
    return true;
}

/* One step of the copier: the top frame's next child, or the node itself once they are made. */
static void copy_step(e1w *wr, size_t base, odin3_ast_id *result) {
    odin3_elab1_frame *top = odin3_vec_at(&wr->e1->frames, wr->e1->frames.len - 1);
    odin3_ast_id node = {top->node};
    odin3_ast_span kids = odin3_ast_children(wr->ast, node);
    if (top->next < kids.n) {
        uint32_t idx = top->next++;
        odin3_ast_id kid = kids.ids[idx];
        const char *role = kid.v != 0 ? fold_role(wr->ast, node, idx) : NULL;
        if (role != NULL) {
            w_push(wr, w_fold(wr, kid, role));
        } else if (kid.v == 0 || !w_enter(wr, kid)) {
            w_push(wr, (odin3_ast_id){0});
        } else {
            (void)push_frame(wr, kid);
        }
        return;
    }
    uint32_t mark = top->mark;
    odin3_vec_pop(&wr->e1->frames);
    odin3_ast_id made = w_copy_node(wr, node, mark);
    if (wr->e1->frames.len > base) {
        w_push(wr, made);
    } else {
        *result = made;
    }
}

/* An expression as written (iterative, no recursion), with its resolved constants folded. */
static odin3_ast_id w_expr(e1w *wr, odin3_ast_id root) {
    odin3_ast_id result = {0};
    size_t base = wr->e1->frames.len;
    if (root.v == 0 || !w_enter(wr, root) || !push_frame(wr, root)) {
        return result;
    }
    while (wr->e1->frames.len > base && wr->st == ODIN3_OK) {
        copy_step(wr, base, &result);
    }
    while (wr->e1->frames.len > base) {
        odin3_vec_pop(&wr->e1->frames);
    }
    return result;
}

/* --- declarations --------------------------------------------------------------------------- */

/* A DECLARATOR copied: its value expression (E?) and its unpacked RANGEs folded. */
static odin3_ast_id w_declarator(e1w *wr, odin3_ast_id decl) {
    uint32_t mark = w_mark(wr);
    odin3_ast_span kids = odin3_ast_children(wr->ast, decl);
    w_push(wr, kids.n > 0 ? w_expr(wr, kids.ids[0]) : (odin3_ast_id){0});
    for (uint32_t i = 1; i < kids.n; i++) {
        w_push(wr, w_range(wr, kids.ids[i]));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_DECLARATOR, decl);
    spec.name = odin3_ast_name(wr->ast, decl);
    return w_make_marked(wr, &spec, mark, decl);
}

/* A parameter's DECLARATOR with its final value (pass E evaluated it). */
static odin3_ast_id w_param_declarator(e1w *wr, odin3_ast_id decl) {
    uint32_t idx = odin3_elab1_env_find(wr->env, 0, odin3_ast_name(wr->ast, decl));
    const odin3_elab1_slot *slot = idx != E1_NONE ? odin3_elab1_env_slot(wr->env, idx) : NULL;
    if (slot == NULL || slot->state != ODIN3_E1_DONE) {
        halt(wr); /* reported in pass E */
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_value(wr, &slot->entry.value, slot->base, decl));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_DECLARATOR, decl);
    spec.name = odin3_ast_name(wr->ast, decl);
    return w_make_marked(wr, &spec, mark, decl);
}

/* The declarators of group after its slots, each through write. */
static void w_declarators(e1w *wr, odin3_ast_id group, odin3_ast_id (*write)(e1w *, odin3_ast_id)) {
    odin3_ast_span kids = odin3_ast_children(wr->ast, group);
    for (uint32_t i = odin3_ast_slot_count(odin3_ast_kind_of(wr->ast, group)); i < kids.n; i++) {
        w_push(wr, write(wr, kids.ids[i]));
    }
}

/* PARAM_DECL[localparam] with the final values (EL-4); RANGE, SIGNED and DT kept as declared. */
static odin3_ast_id w_param_decl(e1w *wr, odin3_ast_id group, bool in_header) {
    if (odin3_ast_sub(wr->ast, group) == ODIN3_AST_PARAM_DECL_SPECPARAM) {
        odin3_elab1_pend_form(wr->e1, group); /* P7: Task 16 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint16_t flags = odin3_ast_flags(wr->ast, group) &
                     (uint16_t)(ODIN3_AST_F_PARAM_DECL_SIGNED | ODIN3_AST_F_PARAM_DECL_DT_MASK);
    flags |= in_header ? (uint16_t)ODIN3_AST_F_PARAM_DECL_IN_HEADER : 0;
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    w_declarators(wr, group, w_param_declarator);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_PARAM_DECL, group);
    spec.sub = ODIN3_AST_PARAM_DECL_LOCALPARAM;
    spec.flags = flags;
    return w_make_marked(wr, &spec, mark, group);
}

/* One ANSI port: PORT_DECL{IN_HEADER} with one DECLARATOR; an untyped net port is `wire`
 * (EL-24: the NET field is never 0 on a net port). */
/* An ANSI port: its PORT_DECL group and one of its DECLARATORs. */
typedef struct e1w_port {
    odin3_ast_id group;
    odin3_ast_id decl;
} e1w_port;

static odin3_ast_id w_port(e1w *wr, const e1w_port *port) {
    odin3_ast_id group = port->group;
    odin3_ast_id decl = port->decl;
    uint32_t flags =
        (uint32_t)odin3_ast_flags(wr->ast, group) | (uint32_t)ODIN3_AST_F_PORT_DECL_IN_HEADER;
    uint32_t dt = (flags & (uint32_t)ODIN3_AST_F_PORT_DECL_DT_MASK) >>
                  (uint32_t)ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    uint32_t net = (flags & (uint32_t)ODIN3_AST_F_PORT_DECL_NET_MASK) >>
                   (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    if (dt == ODIN3_AST_DT_NONE && net == 0) {
        flags |= (1U + (uint32_t)ODIN3_AST_NET_DECL_WIRE)
                 << (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    w_push(wr, w_declarator(wr, decl));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_PORT_DECL, group);
    spec.sub = (uint8_t)odin3_ast_sub(wr->ast, group);
    spec.flags = (uint16_t)flags;
    return w_make_marked(wr, &spec, mark, group);
}

/* The ports LIST: one PORT_DECL per ANSI declarator; a non-ANSI PORT_REF is Task 15's. */
static odin3_ast_id w_ports(e1w *wr, odin3_ast_id list) {
    uint32_t mark = w_mark(wr);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        odin3_ast_id entry = entries.ids[i];
        if (odin3_ast_kind_of(wr->ast, entry) != ODIN3_AST_PORT_DECL ||
            odin3_elab1_pending(wr->e1, entry)) {
            odin3_elab1_not_yet(wr->e1, entry);
            halt(wr);
            continue;
        }
        odin3_ast_span decls = odin3_ast_children(wr->ast, entry);
        for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); j < decls.n; j++) {
            e1w_port port = {entry, decls.ids[j]};
            w_push(wr, w_port(wr, &port));
        }
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

/* A slot the elaborated form drops (STRENGTH, DELAY): Task 16's warnings; pending until then. */
static void w_dropped_slot(e1w *wr, odin3_ast_id slot) {
    if (slot.v != 0) {
        odin3_elab1_not_yet(wr->e1, slot);
        halt(wr);
    }
}

/* NET_DECL with NUMBER bounds, no strength, delay, `vectored` or `scalared` (EL-4). */
static odin3_ast_id w_net_decl(e1w *wr, odin3_ast_id group) {
    uint32_t sub = odin3_ast_sub(wr->ast, group);
    if (sub == ODIN3_AST_NET_DECL_TRI0 || sub == ODIN3_AST_NET_DECL_TRI1 ||
        sub == ODIN3_AST_NET_DECL_TRIREG) {
        odin3_elab1_pend_form(wr->e1, group); /* N15: Task 16 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    w_dropped_slot(wr, odin3_ast_child(wr->ast, group, 0));
    w_dropped_slot(wr, odin3_ast_child(wr->ast, group, E1_NET_DELAY_SLOT));
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, E1_NET_RANGE_SLOT)));
    w_push(wr, (odin3_ast_id){0});
    w_declarators(wr, group, w_declarator);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NET_DECL, group);
    spec.sub = (uint8_t)sub;
    spec.flags = odin3_ast_flags(wr->ast, group) & (uint16_t)ODIN3_AST_F_NET_DECL_SIGNED;
    return w_make_marked(wr, &spec, mark, group);
}

/* VAR_DECL[reg|integer] with NUMBER bounds (time, real and realtime are N17's, Task 16). */
static odin3_ast_id w_var_decl(e1w *wr, odin3_ast_id group) {
    uint32_t sub = odin3_ast_sub(wr->ast, group);
    if (sub != ODIN3_AST_VAR_DECL_REG && sub != ODIN3_AST_VAR_DECL_INTEGER) {
        odin3_elab1_pend_form(wr->e1, group);
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    w_declarators(wr, group, w_declarator);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_VAR_DECL, group);
    spec.sub = (uint8_t)sub;
    spec.flags = odin3_ast_flags(wr->ast, group) & (uint16_t)ODIN3_AST_F_VAR_DECL_SIGNED;
    return w_make_marked(wr, &spec, mark, group);
}

static odin3_ast_id w_net_assign(e1w *wr, odin3_ast_id assign) {
    uint32_t mark = w_mark(wr);
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, assign, 0)));
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, assign, 1)));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NET_ASSIGN, assign);
    return w_make_marked(wr, &spec, mark, assign);
}

/* CONT_ASSIGN without its STRENGTH and DELAY slots (EL-4). */
static odin3_ast_id w_cont_assign(e1w *wr, odin3_ast_id item) {
    w_dropped_slot(wr, odin3_ast_child(wr->ast, item, 0));
    w_dropped_slot(wr, odin3_ast_child(wr->ast, item, 1));
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    w_push(wr, (odin3_ast_id){0});
    w_declarators(wr, item, w_net_assign);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_CONT_ASSIGN, item);
    return w_make_marked(wr, &spec, mark, item);
}

/* --- the decision log (§3.4; internal risk §16 #4) ---------------------------------------- */

/* The decision pass E logged for node, in order; ODIN3_DECISION_SKIPPED and a failed replay when
 * the next entry is not node's. The test hook drop_log_entry skips the Nth entry of each log. */
static int32_t w_replay(e1w *wr, odin3_ast_id node) {
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    uint32_t end = wr->spec->first_log + wr->spec->n_log;
    if (!wr->dropped && hooks->drop_log_entry != 0 && wr->cursor < end &&
        wr->cursor - wr->spec->first_log + 1 == hooks->drop_log_entry) {
        wr->cursor++;
        wr->dropped = true;
    }
    const odin3_elab_decision *entry =
        wr->cursor < end ? odin3_vec_cat(&wr->e1->log, wr->cursor) : NULL;
    if (entry == NULL || entry->node.v != node.v) {
        wr->replay_failed = true;
        return ODIN3_DECISION_SKIPPED;
    }
    wr->cursor++;
    wr->used++;
    return entry->value;
}

static odin3_status w_replay_done(e1w *wr) {
    if (!wr->replay_failed && wr->used == wr->spec->n_log) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR,
              "stage 1: decision log of '%s' not replayed (%" PRIu32 " of %" PRIu32 " entries)",
              odin3_elab1_str(wr->e1, wr->spec->name), wr->used, wr->spec->n_log);
    wr->e1->fatal = wr->e1->fatal != ODIN3_OK ? wr->e1->fatal : ODIN3_ERR_CHECK;
    return ODIN3_ERR_CHECK;
}

/* --- instances ------------------------------------------------------------------------------ */

/* A CONNECTION to write: its name (0: ordered), its value (absent when 0), its location. */
typedef struct e1w_conn {
    uint32_t name;
    odin3_ast_id value;
    odin3_ast_id from;
} e1w_conn;

static odin3_ast_id w_connection(e1w *wr, const e1w_conn *conn) {
    uint32_t mark = w_mark(wr);
    w_push(wr, conn->value);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_CONNECTION, conn->from);
    spec.name = conn->name;
    return w_make_marked(wr, &spec, mark, conn->from);
}

/* The params LIST of a module instance: one named CONNECTION per parameter of the
 * specialization, in declaration order, with its final value (absent without parameters). */
static odin3_ast_id w_spec_params(e1w *wr, odin3_ast_id item, uint32_t child) {
    const odin3_elab1_spec *spec = odin3_elab1_spec_at(wr->e1, child);
    const odin3_elab_def *def = odin3_elab1_def(wr->e1, spec->def);
    if (spec->n_values == 0) {
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    for (uint32_t k = 0; k < spec->n_values; k++) {
        odin3_ast_id decl =
            *(const odin3_ast_id *)odin3_vec_cat(&wr->e1->params, def->first_param + k);
        e1w_conn conn = {odin3_ast_name(wr->ast, decl),
                         w_value(wr, &spec->values[k], spec->bases[k], item), item};
        w_push(wr, w_connection(wr, &conn));
    }
    odin3_ast_spec list = spec_of(wr, ODIN3_AST_LIST, item);
    return w_make_marked(wr, &list, mark, item);
}

/* §9.1's <what> of a value written as given: "parameter 'P'" for a named one (in the
 * environment's arena), "parameter value" for an ordered one. */
static const char *given_what(const e1w *wr, odin3_ast_id entry) {
    const char *name = odin3_ast_name_str(wr->ast, entry);
    size_t len = strlen(name) + sizeof "parameter ''";
    char *what = name[0] != '\0' ? odin3_arena_alloc(wr->env->arena, len) : NULL;
    int got = what != NULL ? snprintf(what, len, "parameter '%s'", name) : -1;
    return got > 0 && (size_t)got < len ? what : "parameter value";
}

/* The params LIST of an instance written as given (a cell type, a black box: EL-4): the
 * overrides as given, their values resolved (Task 12 checks them against the type). */
static odin3_ast_id w_given_params(e1w *wr, odin3_ast_id item) {
    odin3_ast_id list = odin3_ast_child(wr->ast, item, 0);
    if (list.v == 0) {
        return list;
    }
    uint32_t mark = w_mark(wr);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        odin3_ast_id expr = odin3_ast_child(wr->ast, entries.ids[i], 0);
        odin3_ast_id value = {0};
        if (expr.v != 0) {
            odin3_cval val = {0};
            uint8_t base = ODIN3_AST_NUMBER_DEC;
            odin3_elab1_eval_req req = {expr, given_what(wr, entries.ids[i]), &base};
            odin3_status st = odin3_elab1_eval(wr->env, &req, &val);
            if (st != ODIN3_OK) {
                stop_on(wr, st);
            }
            value = st == ODIN3_OK ? w_value(wr, &val, base, expr) : value;
        }
        e1w_conn conn = {odin3_ast_name(wr->ast, entries.ids[i]), value, entries.ids[i]};
        w_push(wr, w_connection(wr, &conn));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

/* The port position of definition def named name; E1_NONE when none. */
static uint32_t port_position(const e1w *wr, const odin3_elab_def *def, uint32_t name) {
    for (uint32_t k = 0; name != 0 && k < def->n_positions; k++) {
        odin3_ast_id port =
            *(const odin3_ast_id *)odin3_vec_cat(&wr->e1->ports, def->first_port + k);
        if (odin3_ast_name(wr->ast, port) == name) {
            return k;
        }
    }
    return E1_NONE;
}

/* The named connections of inst checked against def's ports (EL-10, §9.1); an ordered one is
 * Task 12's (pending). False after a diagnostic. */
static bool check_connections(e1w *wr, odin3_ast_id inst, const odin3_elab_def *def) {
    odin3_ast_span conns = odin3_ast_children(wr->ast, inst);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANCE); i < conns.n; i++) {
        uint32_t name = odin3_ast_name(wr->ast, conns.ids[i]);
        odin3_loc at = odin3_ast_loc(wr->ast, conns.ids[i]);
        if (name == 0) {
            odin3_elab1_pend_form(wr->e1, conns.ids[i]);
            return false;
        }
        if (port_position(wr, def, name) == E1_NONE) {
            odin3_elab1_error(wr->e1, at, "module '%s' has no port '%s'",
                              odin3_elab1_str(wr->e1, def->name), odin3_elab1_str(wr->e1, name));
            return false;
        }
        for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_INSTANCE); j < i; j++) {
            if (odin3_ast_name(wr->ast, conns.ids[j]) == name) {
                odin3_elab1_error(wr->e1, at, "port '%s' of instance '%s' is connected twice",
                                  odin3_elab1_str(wr->e1, name), odin3_ast_name_str(wr->ast, inst));
                return false;
            }
        }
    }
    return true;
}

/* The connection of inst named name (0 when none). */
static odin3_ast_id named_connection(const e1w *wr, odin3_ast_id inst, uint32_t name) {
    odin3_ast_span conns = odin3_ast_children(wr->ast, inst);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANCE); i < conns.n; i++) {
        if (odin3_ast_name(wr->ast, conns.ids[i]) == name) {
            return conns.ids[i];
        }
    }
    return (odin3_ast_id){0};
}

/* A module INSTANCE: one named CONNECTION per port in port order (EL-4), unconnected ones
 * without an expression. */
static odin3_ast_id w_module_instance(e1w *wr, odin3_ast_id inst, uint32_t child) {
    const odin3_elab_def *def = odin3_elab1_def(wr->e1, odin3_elab1_spec_at(wr->e1, child)->def);
    if (!check_connections(wr, inst, def)) {
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    for (uint32_t k = 0; k < def->n_positions; k++) {
        odin3_ast_id port =
            *(const odin3_ast_id *)odin3_vec_cat(&wr->e1->ports, def->first_port + k);
        uint32_t name = odin3_ast_name(wr->ast, port);
        if (name == 0) {
            continue; /* an unnamed position (non-ANSI): Tasks 12 and 15 */
        }
        odin3_ast_id conn = named_connection(wr, inst, name);
        e1w_conn out = {name, {0}, conn.v != 0 ? conn : inst};
        out.value = conn.v != 0 ? w_expr(wr, odin3_ast_child(wr->ast, conn, 0)) : out.value;
        w_push(wr, w_connection(wr, &out));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_INSTANCE, inst);
    spec.name = odin3_ast_name(wr->ast, inst);
    return w_make_marked(wr, &spec, mark, inst);
}

/* An INSTANCE written as given: its connections as written (an array is Task 12's). */
static odin3_ast_id w_given_instance(e1w *wr, odin3_ast_id inst) {
    if (odin3_ast_child(wr->ast, inst, 0).v != 0) {
        odin3_elab1_pend_form(wr->e1, inst);
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    odin3_ast_span conns = odin3_ast_children(wr->ast, inst);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANCE); i < conns.n; i++) {
        e1w_conn conn = {odin3_ast_name(wr->ast, conns.ids[i]),
                         w_expr(wr, odin3_ast_child(wr->ast, conns.ids[i], 0)), conns.ids[i]};
        w_push(wr, w_connection(wr, &conn));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_INSTANCE, inst);
    spec.name = odin3_ast_name(wr->ast, inst);
    return w_make_marked(wr, &spec, mark, inst);
}

/* The INSTANTIATIONs written for item so far: consecutive instances of one specialization share
 * one (EL-4: its name is the specialization's). */
typedef struct e1w_group {
    bool open;
    int32_t value;
    uint32_t mark;
} e1w_group;

static void close_group(e1w *wr, odin3_ast_id item, e1w_group *group) {
    if (!group->open) {
        return;
    }
    group->open = false;
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_INSTANTIATION, item);
    spec.name = group->value >= 0 ? odin3_elab1_spec_at(wr->e1, (uint32_t)group->value)->name
                                  : odin3_ast_name(wr->ast, item);
    w_push(wr, w_make_marked(wr, &spec, group->mark, item));
}

static void w_instantiation(e1w *wr, odin3_ast_id item) {
    e1w_group group = {false, 0, 0};
    odin3_ast_span kids = odin3_ast_children(wr->ast, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); i < kids.n; i++) {
        int32_t value = w_replay(wr, kids.ids[i]);
        if (value == ODIN3_DECISION_SKIPPED) {
            halt(wr); /* reported in pass E, or a failed replay */
            continue;
        }
        if (group.open && value != group.value) {
            close_group(wr, item, &group);
        }
        if (!group.open) {
            group = (e1w_group){true, value, w_mark(wr)};
            w_push(wr, value >= 0 ? w_spec_params(wr, item, (uint32_t)value)
                                  : w_given_params(wr, item));
        }
        w_push(wr, value >= 0 ? w_module_instance(wr, kids.ids[i], (uint32_t)value)
                              : w_given_instance(wr, kids.ids[i]));
    }
    close_group(wr, item, &group);
}

/* --- the module ----------------------------------------------------------------------------- */

static void w_item(e1w *wr, odin3_ast_id item) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, item);
    odin3_ast_span attrs = odin3_ast_attrs(wr->ast, item);
    if (odin3_elab1_pending(wr->e1, item)) {
        halt(wr);
        return;
    }
    if (attrs.n > 0) {
        odin3_elab1_not_yet(wr->e1, attrs.ids[0]); /* ATTR: Task 15 */
        halt(wr);
    }
    switch (kind) {
    case ODIN3_AST_PARAM_DECL:
        w_push(wr, w_param_decl(wr, item, false));
        break;
    case ODIN3_AST_NET_DECL:
        w_push(wr, w_net_decl(wr, item));
        break;
    case ODIN3_AST_VAR_DECL:
        w_push(wr, w_var_decl(wr, item));
        break;
    case ODIN3_AST_CONT_ASSIGN:
        w_push(wr, w_cont_assign(wr, item));
        break;
    case ODIN3_AST_INSTANTIATION:
        w_instantiation(wr, item);
        break;
    case ODIN3_AST_DEFPARAM:
        break; /* resolved in pass E; absent from the elaborated form */
    case ODIN3_AST_PORT_DECL:
        odin3_elab1_pend_form(wr->e1, item); /* a port declared in the body: Task 15 */
        halt(wr);
        break;
    default:
        odin3_elab1_unhandled(wr->e1, item);
        halt(wr);
        break;
    }
}

/* The header's parameters LIST (absent when the source has none). */
static odin3_ast_id w_header_params(e1w *wr, odin3_ast_id list) {
    if (list.v == 0) {
        return list;
    }
    uint32_t mark = w_mark(wr);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        if (odin3_ast_kind_of(wr->ast, entries.ids[i]) != ODIN3_AST_PARAM_DECL ||
            odin3_elab1_pending(wr->e1, entries.ids[i])) {
            odin3_elab1_not_yet(wr->e1, entries.ids[i]);
            halt(wr);
            continue;
        }
        w_push(wr, w_param_decl(wr, entries.ids[i], true));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

static odin3_ast_id w_module(e1w *wr) {
    odin3_ast_id node = odin3_elab1_def(wr->e1, wr->spec->def)->node;
    odin3_ast_span attrs = odin3_ast_attrs(wr->ast, node);
    if (attrs.n > 0) {
        odin3_elab1_not_yet(wr->e1, attrs.ids[0]); /* ATTR: Task 15 */
        halt(wr);
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_header_params(wr, odin3_ast_child(wr->ast, node, E1_SLOT_PARAMS)));
    odin3_ast_id ports = odin3_ast_child(wr->ast, node, E1_SLOT_PORTS);
    w_push(wr, ports.v != 0 ? w_ports(wr, ports) : ports);
    odin3_ast_span kids = odin3_ast_children(wr->ast, node);
    for (uint32_t i = E1_ITEMS; i < kids.n && wr->st == ODIN3_OK && !wr->e1->stopped; i++) {
        w_item(wr, kids.ids[i]);
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_MODULE, node);
    spec.name = wr->spec->name;
    return w_make_marked(wr, &spec, mark, node);
}

odin3_status odin3_elab1_write(odin3_elab1 *e1, uint32_t spec) {
    e1w wr = {.e1 = e1,
              .env = e1->env,
              .ast = e1->parsed,
              .out = e1->elab,
              .spec = odin3_elab1_spec_at(e1, spec),
              .building = e1->errors == 0 && e1->fatal == ODIN3_OK,
              .st = ODIN3_OK};
    if (wr.env == NULL || wr.out == NULL || wr.spec == NULL) {
        return ODIN3_ERR_CHECK; /* pass E did not run: a 2D bug */
    }
    wr.cursor = wr.spec->first_log;
    odin3_ast_id module = w_module(&wr);
    odin3_status st = wr.st;
    st = st != ODIN3_OK || e1->stopped ? st : w_replay_done(&wr);
    w_push(&wr, module);
    st = st != ODIN3_OK ? st : wr.st;
    odin3_elab1_env_destroy(e1->env);
    e1->env = NULL;
    return st;
}
