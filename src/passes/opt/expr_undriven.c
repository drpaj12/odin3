/*
 * expr_undriven.c — replace_undriven (2F spec §4.6; Yosys 0.55 opt_expr.cc `replace_undriven`, run
 * by `opt_expr -undriven`). Every undriven net becomes the constant its `init` gives (0 or 1) or
 * x; the init bits used are cleared. A net that no public wire names is replaced only when
 * something reads it (a `$`-named wire's rule).
 */
#include "passes/opt/expr.h"

#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the wires of a net say: some public wire names it, and some `keep` wire does. */
typedef struct wire_facts {
    bool is_public, is_keep;
} wire_facts;

static void note_wire(const odin3_expr_ctx *ec, odin3_wire_id wire, wire_facts *facts) {
    if (!odin3_wire_valid(wire)) {
        return;
    }
    const odin3_strtab *tab = odin3_design_strtab(ec->oc->design);
    uint32_t name = odin3_wire_name(ec->module, wire);
    const char *text = name != 0 ? odin3_strtab_get(tab, name) : NULL;
    facts->is_public = facts->is_public || (text != NULL && text[0] != '$');
    facts->is_keep =
        facts->is_keep || odin3_kit_keep(ec->oc, (odin3_objref){ODIN3_OBJ_WIRE, wire.v});
}

static wire_facts facts_of(const odin3_expr_ctx *ec, odin3_net_id net) {
    wire_facts facts = {false, false};
    note_wire(ec, odin3_net_primary(ec->module, net).wire, &facts);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(ec->module, net, &cursor, &alias)) {
        note_wire(ec, alias.wb.wire, &facts);
    }
    return facts;
}

/* Replaces one undriven net by its init value or x; the used init bits go. */
static odin3_status replace_net(odin3_expr_ctx *ec, odin3_net_id net) {
    odin3_bit init = odin3_ffinit_get(ec->module, net);
    odin3_status st = ODIN3_OK;
    if (init != ODIN3_BIT_X) {
        st = odin3_ffinit_remove(ec->module, net, NULL);
    }
    odin3_net_id value = {0};
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, init, &value) : st;
    st = st == ODIN3_OK ? odin3_net_merge(ec->module, (odin3_net_pair){value, net}) : st;
    if (st == ODIN3_OK) {
        ec->oc->changed = true;
        ec->oc->counts->expr.rule[ODIN3_OPT_RULE_UNDRIVEN]++;
        ec->oc->counts->expr.rewritten++;
    }
    return st;
}

odin3_status odin3_expr_replace_undriven(odin3_expr_ctx *ec) {
    uint32_t end = odin3_module_net_end(ec->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_net_id net = {id};
        if (!odin3_net_live(ec->module, net) || odin3_net_driver_count(ec->module, net) != 0) {
            continue;
        }
        wire_facts facts = facts_of(ec, net);
        bool used = facts.is_keep || odin3_kit_readers(ec->oc, net) > 0;
        if (facts.is_public || used) {
            st = replace_net(ec, net);
        }
    }
    return st;
}
