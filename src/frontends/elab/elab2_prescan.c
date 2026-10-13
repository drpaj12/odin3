/*
 * elab2_prescan.c — step (2) of EL-28: the pre-scan of the module's symbols (driver rules per
 * net kind, input ports, the always-assigned bit set and the wired-net check on `always` targets).
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab2_drive.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
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

/* --- the symbols of the module ---------------------------------------------------------------- */

/* fn on the net and variable symbols of scope that have a wire in the module being emitted. */
static odin3_status scope_symbols(odin3_elab2 *e2, odin3_scope_id scope, odin3_elab2_symbol_fn fn) {
    odin3_status st = ODIN3_OK;
    for (odin3_sym_id sym = odin3_scope_first_symbol(e2->st, scope); st == ODIN3_OK && sym.v != 0;
         sym = odin3_scope_next_symbol(e2->st, sym)) {
        const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
        bool variable = rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR;
        bool ours = rec->ir_wire.v != 0 && rec->ir_module.v == e2->mod_id.v;
        st = variable && ours ? fn(e2, rec) : ODIN3_OK;
    }
    return st;
}

/* The child scopes of scope onto the worklist. */
static odin3_status push_scopes(const odin3_elab2 *e2, odin3_vec *todo, odin3_scope_id scope) {
    for (odin3_scope_id kid = odin3_scope_first_child(e2->st, scope); kid.v != 0;
         kid = odin3_scope_next_sibling(e2->st, kid)) {
        odin3_scope_id *slot = odin3_vec_push(todo);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = kid;
    }
    return ODIN3_OK;
}

/* Calls fn on every net and variable symbol of the module being emitted that has a wire, in the
 * module's scope and its generate and named-block scopes (a worklist, no recursion). */
odin3_status odin3_elab2_each_symbol(odin3_elab2 *e2, odin3_ast_id module_node,
                                     odin3_elab2_symbol_fn fn) {
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(odin3_scope_id));
    odin3_scope_id *slot = odin3_vec_push(&todo);
    odin3_status st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (slot != NULL) {
        *slot = odin3_elab_scope_of(&e2->bind, module_node);
    }
    while (st == ODIN3_OK && todo.len > 0) {
        odin3_scope_id scope = *(const odin3_scope_id *)odin3_vec_cat(&todo, todo.len - 1);
        odin3_vec_pop(&todo);
        st = scope_symbols(e2, scope, fn);
        st = st != ODIN3_OK ? st : push_scopes(e2, &todo, scope);
    }
    odin3_vec_free(&todo);
    return st;
}

/* --- step (2): the pre-scan ------------------------------------------------------------------ */

/* The input ports are driven from outside; the module's own inout pins drive their nets. */
static odin3_status prescan_ports(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                  odin3_ast_id module_node) {
    odin3_ast_span ports = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, 1));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ports.n; i++) {
        uint32_t dir = odin3_ast_sub(e2->elab, ports.ids[i]);
        odin3_ast_id declarator = odin3_ast_child(e2->elab, ports.ids[i], 1);
        const odin3_symbol *rec =
            odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, declarator));
        if (rec == NULL || rec->ir_wire.v == 0 || dir == ODIN3_AST_PORT_DECL_OUTPUT) {
            continue;
        }
        odin3_drv entry = {
            dir == ODIN3_AST_PORT_DECL_INOUT ? ODIN3_DRV_INOUT_PIN : ODIN3_DRV_PORT_IN, declarator};
        uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
        for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
            st = odin3_elab2_drv_set(dr, odin3_wire_net(e2->mod, rec->ir_wire, bit), entry);
        }
    }
    return st;
}

/* A wired wire: a record, and each of its nets knows its bit. */
static odin3_status register_wired(const odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                   const odin3_symbol *rec, bool is_or) {
    odin3_elab2_wired_rec *slot = odin3_vec_push(&dr->wired);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&slot->ops, sizeof(odin3_elab2_wired_op));
    slot->wire = rec->ir_wire;
    slot->is_or = is_or;
    slot->declarator = rec->declarator;
    slot->scope = rec->scope.v;
    uint32_t index = (uint32_t)dr->wired.len;
    uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_elab2_net_info *info =
            odin3_elab2_net_slot(&dr->info, odin3_wire_net(e2->mod, rec->ir_wire, bit));
        if (info == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *info = (odin3_elab2_net_info){index, bit};
    }
    return ODIN3_OK;
}

/* `supply0`/`supply1`: the wire's nets become the shared constant net; any driver is A4's. */
static odin3_status register_supply(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                    const odin3_symbol *rec, odin3_bit bit) {
    odin3_net_id shared = {0};
    odin3_status st = odin3_elab2_const_net(e2, bit, &shared);
    uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        odin3_net_id net = odin3_elab2_resolve(e2, odin3_wire_net(e2->mod, rec->ir_wire, k));
        st = net.v != shared.v ? odin3_elab2_merge(e2, (odin3_net_pair){shared, net}) : ODIN3_OK;
        st = st != ODIN3_OK ? st
                            : odin3_elab2_claim_put(dr, (odin3_wirebit){rec->ir_wire, k},
                                                    (odin3_drv){ODIN3_DRV_CONST, rec->declarator});
    }
    return st;
}

/* The net kinds with driver rules (EL-26) of one symbol. */
static odin3_status prescan_symbol(odin3_elab2 *e2, const odin3_symbol *rec) {
    struct odin3_elab2_drive *dr = e2->drive;
    if (rec->kind != ODIN3_SYM_NET) {
        return ODIN3_OK;
    }
    const odin3_elab2_wired_kind *wired = odin3_elab2_wired_kind_of(rec->sub);
    if (wired != NULL) {
        return register_wired(e2, dr, rec, wired->is_or);
    }
    switch (rec->sub) {
    case ODIN3_AST_NET_DECL_UWIRE:
        return odin3_u64map_put(dr->uwires, (odin3_kv){rec->ir_wire.v, 1});
    case ODIN3_AST_NET_DECL_SUPPLY0:
        return register_supply(e2, dr, rec, ODIN3_BIT_0);
    case ODIN3_AST_NET_DECL_SUPPLY1:
        return register_supply(e2, dr, rec, ODIN3_BIT_1);
    default:
        return ODIN3_OK;
    }
}

/* --- the always-assigned bit set ------------------------------------------------------------- */

/* The flags of the object sym names: one per bit of its wire (a memory: one in all), made on first
 * use; NULL on out of memory. */
static uint8_t *always_flags(odin3_elab2 *e2, odin3_sym_id sym, uint32_t *width) {
    struct odin3_elab2_drive *dr = e2->drive;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    *width =
        rec->ir_wire.v != 0 && rec->n_unpacked == 0 ? odin3_wire_width(e2->mod, rec->ir_wire) : 1U;
    uint64_t at = 0;
    if (!odin3_u64map_get(dr->always, sym.v, &at)) {
        at = dr->abits.len;
        for (uint32_t i = 0; i < *width; i++) {
            if (odin3_vec_push(&dr->abits) == NULL) {
                return NULL;
            }
        }
        if (odin3_u64map_put(dr->always, (odin3_kv){sym.v, at}) != ODIN3_OK) {
            return NULL;
        }
    }
    return (uint8_t *)dr->abits.data + at;
}

bool odin3_elab2_always_assigned(const odin3_elab2 *e2, odin3_sym_id sym, uint32_t bit) {
    uint64_t at = 0;
    if (e2->drive == NULL || !odin3_u64map_get(e2->drive->always, sym.v, &at)) {
        return false;
    }
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    uint32_t width =
        rec->ir_wire.v != 0 && rec->n_unpacked == 0 ? odin3_wire_width(e2->mod, rec->ir_wire) : 1U;
    return bit < width && ((const uint8_t *)e2->drive->abits.data)[at + bit] != 0;
}

odin3_status odin3_elab2_word_assigned(odin3_elab2 *e2, odin3_wire_id wire) {
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (odin3_u64map_get(dr->words, wire.v, NULL)) {
        return ODIN3_OK;
    }
    uint32_t width = odin3_wire_width(e2->mod, wire);
    size_t at = dr->wbits.len;
    for (uint32_t i = 0; i < width; i++) {
        uint8_t *flag = odin3_vec_push(&dr->wbits);
        if (flag == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *flag = 1;
    }
    return odin3_u64map_put(dr->words, (odin3_kv){wire.v, at});
}

bool odin3_elab2_always_assigned_word(const odin3_elab2 *e2, odin3_wire_id wire, uint32_t bit) {
    uint64_t at = 0;
    if (e2->drive == NULL || !odin3_u64map_get(e2->drive->words, wire.v, &at)) {
        return false;
    }
    return bit < odin3_wire_width(e2->mod, wire) &&
           ((const uint8_t *)e2->drive->wbits.data)[at + bit] != 0;
}

/* The bits of a select whose index is a NUMBER, from..to inclusive (positions), else the whole
 * object: reports whether the select's bits are exact. */
static bool select_span(odin3_elab2 *e2, odin3_ast_id sel, uint32_t width, uint32_t span[2]) {
    odin3_ast_id object = odin3_ast_child(e2->elab, sel, 0);
    uint32_t sub = odin3_ast_sub(e2->elab, sel);
    odin3_ast_id first = odin3_ast_child(e2->elab, sel, 1);
    odin3_ast_id second = odin3_ast_child(e2->elab, sel, 2);
    bool numbers =
        odin3_ast_kind_of(e2->elab, first) == ODIN3_AST_NUMBER &&
        (sub == ODIN3_AST_SELECT_BIT ||
         (sub == ODIN3_AST_SELECT_PART && odin3_ast_kind_of(e2->elab, second) == ODIN3_AST_NUMBER));
    odin3_elab2_selobj obj;
    int64_t idx[2] = {0, 0};
    bool known[2] = {false, false};
    if (!numbers) {
        return false;
    }
    odin3_elab2_sel_object(e2, object, width, &obj);
    if (odin3_elab2_number_i64(e2, first, &idx[0], &known[0]) != ODIN3_OK || !known[0] ||
        (sub == ODIN3_AST_SELECT_PART &&
         (odin3_elab2_number_i64(e2, second, &idx[1], &known[1]) != ODIN3_OK || !known[1]))) {
        return false;
    }
    idx[1] = sub == ODIN3_AST_SELECT_BIT ? idx[0] : idx[1];
    int64_t pos[2] = {odin3_elab2_sel_pos(&obj, idx[0]), odin3_elab2_sel_pos(&obj, idx[1])};
    if (pos[0] < 0 || pos[1] < 0) {
        return false;
    }
    span[0] = (uint32_t)(pos[0] < pos[1] ? pos[0] : pos[1]);
    span[1] = (uint32_t)(pos[0] < pos[1] ? pos[1] : pos[0]);
    return true;
}

/* An object assigned in an always block: bits span[0]..span[1] (or all of them), and the wired
 * check. */
static odin3_status mark_object(odin3_elab2 *e2, odin3_ast_id name, const uint32_t *span) {
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, name);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    if (rec == NULL || (rec->kind != ODIN3_SYM_NET && rec->kind != ODIN3_SYM_VAR)) {
        return ODIN3_OK;
    }
    const odin3_elab2_wired_kind *wired = odin3_elab2_wired_kind_of(rec->sub);
    if (rec->kind == ODIN3_SYM_NET && wired != NULL) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, name),
                          "'%s' is a wired net (%s) and cannot be assigned in an always block",
                          odin3_strtab_get(odin3_design_strtab(e2->design), rec->name),
                          wired->name);
        return ODIN3_OK;
    }
    uint32_t width = 0;
    uint8_t *flags = always_flags(e2, sym, &width);
    if (flags == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t from = span != NULL && span[1] < width ? span[0] : 0;
    uint32_t to = span != NULL && span[1] < width ? span[1] : width - 1U;
    memset(flags + from, 1, (size_t)to - from + 1U);
    return ODIN3_OK;
}

/* One lvalue node of an always assignment: a concatenation is its parts, a select its object. */
static odin3_status mark_lvalue_node(odin3_elab2 *e2, odin3_vec *work, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e2->elab, node)) {
    case ODIN3_AST_CONCAT: {
        odin3_ast_span parts = odin3_ast_children(e2->elab, node);
        for (uint32_t i = 0; i < parts.n; i++) {
            odin3_ast_id *slot = odin3_vec_push(work);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = parts.ids[i];
        }
        return ODIN3_OK;
    }
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return mark_object(e2, node, NULL);
    case ODIN3_AST_SELECT: {
        odin3_ast_id object = odin3_ast_child(e2->elab, node, 0);
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, object);
        if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
            return ODIN3_OK; /* the process generator reports a select of an expression */
        }
        const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, object));
        uint32_t span[2] = {0, 0};
        bool exact = rec != NULL && rec->ir_wire.v != 0 && rec->n_unpacked == 0 &&
                     select_span(e2, node, odin3_wire_width(e2->mod, rec->ir_wire), span);
        return mark_object(e2, object, exact ? span : NULL);
    }
    default:
        return ODIN3_OK;
    }
}

/* The lvalue of one assignment inside an always block. */
static odin3_status mark_lvalue(odin3_elab2 *e2, odin3_ast_id lhs) {
    odin3_vec work;
    odin3_vec_init(&work, sizeof(odin3_ast_id));
    odin3_ast_id *slot = odin3_vec_push(&work);
    odin3_status st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (slot != NULL) {
        *slot = lhs;
    }
    while (st == ODIN3_OK && work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&work, work.len - 1);
        odin3_vec_pop(&work);
        st = mark_lvalue_node(e2, &work, node);
    }
    odin3_vec_free(&work);
    return st;
}

/* Every blocking and nonblocking assignment below one ALWAYS (functions and tasks are inlined
 * where they are called, EL-38). */
static odin3_status scan_one_always(odin3_elab2 *e2, odin3_ast_id always) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, always);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev == ODIN3_AST_ENTER &&
            (kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN)) {
            st = mark_lvalue(e2, odin3_ast_child(e2->elab, node, 0));
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* The ALWAYS blocks of the module and its generate blocks. */
static odin3_status scan_always(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, module_node);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && !e2->stopped && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev != ODIN3_AST_ENTER || kind == ODIN3_AST_MODULE || kind == ODIN3_AST_GEN_BLOCK) {
            continue;
        }
        st = kind == ODIN3_AST_ALWAYS ? scan_one_always(e2, node) : ODIN3_OK;
        odin3_ast_cursor_skip(&cur);
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

odin3_status odin3_elab2_prescan(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_prescan: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = prescan_ports(e2, dr, module_node);
    st = st != ODIN3_OK ? st : odin3_elab2_each_symbol(e2, module_node, prescan_symbol);
    st = st != ODIN3_OK ? st : scan_always(e2, module_node);
    return st != ODIN3_OK ? st : e2->fatal;
}
