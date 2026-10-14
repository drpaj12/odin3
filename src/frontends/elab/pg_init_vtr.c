/*
 * pg_init_vtr.c — `read_verilog --vtr-compat`: a non-constant `initial x = e;` is `assign x = e;`
 * (Peter 2026-10-13; EL-40, §9.2, §17). Yosys 0.55 reads an `initial` block as a process with no
 * sync rule, so a statement it cannot evaluate stays a combinational action: `initial y = in;`
 * leaves `connect \y \in`, which is what Parmys writes. Without the option the statement is
 * ignored (pg_init.c). The assignment applies only to a target nothing else drives: a variable
 * that an `always` block assigns, or a net a continuous assignment, an instance, a gate or an
 * input port drives, keeps the ignore, with a warning that says why (Yosys 0.55 stops with
 * `Failed to get a constant init value` for an always-assigned one; Odin III warns and goes on).
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/pg_init.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The variable a statement target names: the identifier under constant selects (NUMBER
 * indices), else 0. Memories, nets and block-local variables (no wire) are not candidates. */
static const odin3_symbol *target_symbol(const odin3_elab2 *e2, odin3_ast_id lhs,
                                         odin3_sym_id *sym) {
    odin3_ast_id node = lhs;
    while (node.v != 0 && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_SELECT) {
        odin3_ast_id first = odin3_ast_child(e2->elab, node, 1);
        odin3_ast_id second = odin3_ast_child(e2->elab, node, 2);
        bool constant = odin3_ast_kind_of(e2->elab, first) == ODIN3_AST_NUMBER &&
                        (second.v == 0 || odin3_ast_kind_of(e2->elab, second) == ODIN3_AST_NUMBER);
        node = constant ? odin3_ast_child(e2->elab, node, 0) : (odin3_ast_id){0};
    }
    if (node.v == 0 || odin3_ast_kind_of(e2->elab, node) != ODIN3_AST_IDENT) {
        return NULL;
    }
    *sym = odin3_elab_sym_of(&e2->bind, node);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, *sym);
    bool variable = rec != NULL && rec->kind == ODIN3_SYM_VAR && rec->ir_wire.v != 0 &&
                    rec->n_unpacked == 0 && rec->ir_module.v == e2->mod_id.v;
    return variable ? rec : NULL;
}

/* Why the assignment cannot take the target: the kind of the first other driver of one of the
 * lvalue's bits (ODIN3_DRV_PROCESS for an always block), ODIN3_DRV_NONE when none has one. */
static uint8_t blocking_driver(const odin3_elab2 *e2, odin3_sym_id sym,
                               const odin3_elab2_lvalue *lv) {
    uint8_t found = ODIN3_DRV_NONE;
    for (size_t i = 0; i < lv->bits.len; i++) {
        odin3_wirebit wb = ((const odin3_elab2_lvbit *)lv->bits.data)[i].wb;
        if (wb.wire.v == 0) {
            continue;
        }
        if (odin3_elab2_always_assigned(e2, sym, wb.bit) ||
            odin3_elab2_always_assigned_word(e2, wb.wire, wb.bit)) {
            return ODIN3_DRV_PROCESS;
        }
        uint8_t kind = odin3_elab2_bit_driver(e2, wb).kind;
        found = found == ODIN3_DRV_NONE ? kind : found;
    }
    return found;
}

static const char *driver_text(uint8_t kind) {
    switch (kind) {
    case ODIN3_DRV_PROCESS:
        return "also assigned in an always block";
    case ODIN3_DRV_PORT_IN:
        return "driven by an input port";
    case ODIN3_DRV_INSTANCE:
    case ODIN3_DRV_INOUT_PIN:
        return "driven by an instance";
    case ODIN3_DRV_GATE:
        return "driven by a gate";
    default:
        return "driven by a continuous assignment";
    }
}

odin3_status odin3_elab2_initial_assign(odin3_elab2 *e2, odin3_ast_id stmt, bool *handled) {
    *handled = false;
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, stmt);
    bool assign = kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN;
    odin3_ast_id lhs = assign ? odin3_ast_child(e2->elab, stmt, 0) : (odin3_ast_id){0};
    odin3_ast_id rhs = assign ? odin3_ast_child(e2->elab, stmt, 2) : (odin3_ast_id){0};
    odin3_sym_id sym = {0};
    const odin3_symbol *rec = assign && rhs.v != 0 && odin3_ast_child(e2->elab, stmt, 1).v == 0
                                  ? target_symbol(e2, lhs, &sym)
                                  : NULL;
    if (!e2->mod_vtr_compat || rec == NULL) {
        return ODIN3_OK;
    }
    odin3_elab2_lvalue lv;
    odin3_elab2_lvalue_init(&lv);
    odin3_status st = odin3_elab2_lvalue_make(e2, lhs, &lv);
    uint8_t blocked = st == ODIN3_OK ? blocking_driver(e2, sym, &lv) : ODIN3_DRV_NONE;
    odin3_elab2_lvalue_free(&lv);
    const char *name = odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
    odin3_loc at = odin3_ast_loc(e2->elab, stmt);
    *handled = st == ODIN3_OK;
    if (st == ODIN3_OK && blocked != ODIN3_DRV_NONE) {
        odin3_elab2_warn(e2, at,
                         "initial value of '%s' is not constant and is ignored: '%s' is %s "
                         "(VTR/Parmys compatibility does not apply)",
                         name, name, driver_text(blocked));
    } else if (st == ODIN3_OK) {
        st = odin3_elab2_assign_nodes(e2, lhs, rhs);
        if (st == ODIN3_OK) {
            odin3_elab2_warn(e2, at,
                             "initial value of '%s' is not constant: treated as a continuous "
                             "assign (VTR/Parmys compatibility)",
                             name);
        }
    }
    return st;
}
