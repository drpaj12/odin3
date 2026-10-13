/*
 * elab1_unit.c — a `read_verilog --lib` unit against a techlib cell of the same name (2D-7b, spec
 * 3.1a). EL-6 resolves a library-unit MODULE (item 3) before a registered cell type (item 5), so
 * a unit named like a hard cell would silently replace every instance of the cell, the 1-bit
 * `adder u(...)` of a design included. The rule instead looks at the instance's `#()`:
 *
 *  - every value fits a parameter of the cell (ordered values no more than the cell has; named
 *    values by name): the cell wins, with one info line per name;
 *  - otherwise (`adder #(8)`, `adder #(.WIDTH(8))`: the cell has no such parameter) the unit
 *    wins, with a warning at the instantiation.
 *
 * Neither choice is silent. A design that wants the unit everywhere reads no techlib with the
 * cell; one that wants the cell everywhere reads no --lib file defining it.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ir/celltype.h"
#include "odin3/odin3.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { E1U_SHIFT = 32 };

/* The cell type's parameter named text (by name), else false. */
static bool cell_has_param(const odin3_celltype_def *type, const char *text) {
    for (uint32_t i = 0; i < type->n_params; i++) {
        if (strcmp(type->params[i].name, text) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether the `#()` of INSTANTIATION item fits the cell type's parameters. A mixed list is left
 * to the parameter code, which reports it whichever target wins. */
static bool params_fit(const odin3_elab1 *e1, odin3_ast_id item, const odin3_celltype_def *type) {
    const odin3_ast *ast = e1->parsed;
    odin3_ast_span entries = odin3_ast_children(ast, odin3_ast_child(ast, item, 0));
    for (uint32_t i = 0; i < entries.n; i++) {
        uint32_t name = odin3_ast_name(ast, entries.ids[i]);
        if (name == 0 ? i >= type->n_params : !cell_has_param(type, odin3_elab1_str(e1, name))) {
            return false;
        }
    }
    return true;
}

/* Marks name noted in the high half of the key space; true the first time. */
static bool first_cell_note(odin3_elab1 *e1, uint32_t name) {
    uint64_t key = (uint64_t)name | (uint64_t)1 << E1U_SHIFT;
    if (odin3_u64map_get(e1->noted, key, NULL)) {
        return false;
    }
    odin3_status st = odin3_u64map_put(e1->noted, (odin3_kv){key, 1});
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
        return false;
    }
    return true;
}

void odin3_elab1_prefer_unit(odin3_elab1 *e1, odin3_ast_id item, odin3_elab_res *res) {
    if (res->kind != ODIN3_RES_MODULE || !odin3_elab1_def_shadows_cell(e1, res->def)) {
        return;
    }
    uint32_t name = odin3_elab1_def(e1, res->def)->name;
    odin3_celltype_id type_id;
    if (!odin3_celltype_find(e1->design, name, &type_id)) {
        return;
    }
    const odin3_celltype_def *type = odin3_celltype_get(e1->design, type_id);
    if (!params_fit(e1, item, type)) {
        odin3_elab1_warn_once(e1, item,
                              "instance of '%s' uses the library unit '%s' (read with --lib), not "
                              "the hard cell of that name: its parameters do not fit the cell",
                              type->name, type->name);
        return;
    }
    res->kind = ODIN3_RES_CELLTYPE;
    res->type = type_id;
    if (first_cell_note(e1, name)) {
        odin3_elab1_info(e1, odin3_ast_loc(e1->parsed, item),
                         "instance of '%s' uses the hard cell '%s': its parameters fit the cell, "
                         "so the library unit of that name (read with --lib) is not used",
                         type->name, type->name);
    }
}
