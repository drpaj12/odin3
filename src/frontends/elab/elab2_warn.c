/*
 * elab2_warn.c — the end-of-module warnings of stage 2 (EL-28): `never driven` and the
 * self-assignments that nothing drives.
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

/* --- the end of the module: warnings ---------------------------------------------------------- */

/* The name an lvalue or rhs node reads as in a message (a select reads as its object). */
static const char *node_label(const odin3_elab2 *e2, odin3_ast_id node) {
    while (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_SELECT) {
        node = odin3_ast_child(e2->elab, node, 0);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME
               ? odin3_ast_name_str(e2->elab, node)
               : "expression";
}

/* The self-assignments whose net is still undriven. */
static odin3_status warn_loops(odin3_elab2 *e2, struct odin3_elab2_drive *dr) {
    for (size_t i = 0; i < dr->loops.len; i++) {
        const odin3_elab2_loop_note *note = odin3_vec_cat(&dr->loops, i);
        odin3_net_id net = odin3_elab2_resolve(e2, note->net);
        if (odin3_net_driver_count(e2->mod, net) > 0) {
            continue;
        }
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, note->lhs),
                         "'%s' and '%s' are connected only to each other and are never driven",
                         node_label(e2, note->rhs), node_label(e2, note->lhs));
        odin3_status st = odin3_u64map_put(dr->covered, (odin3_kv){net.v, 1});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A wire with sinks and no driver warns, once; a port is left to rule 4 of the check. */
static odin3_status warn_undriven(odin3_elab2 *e2, const odin3_symbol *rec) {
    const struct odin3_elab2_drive *dr = e2->drive;
    const odin3_module *mod = e2->mod;
    if (rec->n_unpacked > 0 || odin3_wire_port_node(mod, rec->ir_wire).v != 0) {
        return ODIN3_OK;
    }
    uint32_t width = odin3_wire_width(mod, rec->ir_wire);
    for (uint32_t k = 0; k < width; k++) {
        odin3_net_id net = odin3_wire_net(mod, rec->ir_wire, k);
        if (odin3_net_sinks(mod, net).count > 0 && odin3_net_driver_count(mod, net) == 0 &&
            !odin3_u64map_get(dr->covered, net.v, NULL)) {
            odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, rec->declarator), "'%s' is never driven",
                             odin3_strtab_get(odin3_design_strtab(e2->design),
                                              odin3_wire_name(mod, rec->ir_wire)));
            break;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_elab2_undriven(odin3_elab2 *e2) {
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (e2->mod == NULL || dr == NULL) {
        return e2->mod == NULL ? ODIN3_ERR_INVALID_ARG : ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = warn_loops(e2, dr);
    st = st != ODIN3_OK ? st : odin3_elab2_each_symbol(e2, e2->mod_node, warn_undriven);
    return st != ODIN3_OK ? st : e2->fatal;
}
