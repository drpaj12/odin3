/* wire.c — wires, their net vectors and the net alias table (IR-2, IR-14, IR-15). */
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* Cursor value for an alias iteration that has ended. */
static const uint32_t ALIAS_END = UINT32_MAX;

/* --- alias table --------------------------------------------------------------------------- */

static odin3_alias_rec *alias_at(odin3_module *module, uint32_t idx) {
    return odin3_vec_at(&module->aliases, idx);
}

odin3_status odin3_alias_reserve(odin3_module *module, uint32_t count) {
    if (count == 0) {
        return ODIN3_OK;
    }
    size_t len = module->aliases.len;
    size_t need = (len == 0 ? 1 : len) + count; /* slot 0 is the reserved dummy */
    if (need >= ALIAS_END) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_vec_reserve(&module->aliases, need);
    if (st == ODIN3_OK && len == 0) {
        (void)odin3_vec_push(&module->aliases); /* cannot fail: reserved */
    }
    return st;
}

/* Prepends a reserved alias record to net's chain. */
static void alias_push(odin3_module *module, odin3_net_id net, odin3_net_alias alias) {
    uint32_t idx = (uint32_t)module->aliases.len;
    odin3_alias_rec *rec = odin3_vec_push(&module->aliases);
    assert(rec != NULL); /* reserved by the caller */
    odin3_net_rec *nrec = odin3_net_rec_at(module, net);
    rec->net = net;
    rec->wire = alias.wb.wire;
    rec->bit = alias.wb.bit;
    rec->name = alias.name;
    rec->next = nrec->alias_head;
    nrec->alias_head = idx;
}

/* Removes the (wire, bit) alias from the net's chain and returns its record (it must be there). */
static uint32_t alias_unlink(odin3_module *module, odin3_net_rec *net, odin3_wirebit wb) {
    uint32_t prev = 0;
    uint32_t idx = net->alias_head;
    while (idx != 0) {
        odin3_alias_rec *rec = alias_at(module, idx);
        if (rec->wire.v == wb.wire.v && rec->bit == wb.bit) {
            if (prev == 0) {
                net->alias_head = rec->next;
            } else {
                alias_at(module, prev)->next = rec->next;
            }
            rec->next = 0;
            return idx;
        }
        prev = idx;
        idx = rec->next;
    }
    assert(0 && "a wire entry without its membership (check rule 8)");
    return 0;
}

/* Points what the alias names (a wire entry or a name-map entry) at net. */
static void alias_repoint(odin3_module *module, const odin3_alias_rec *alias, odin3_net_id net) {
    if (odin3_wire_valid(alias->wire)) {
        odin3_wire_rec_at(module, alias->wire)->nets[alias->bit] = net;
        return;
    }
    odin3_status st = odin3_u64map_put(module->net_names, (odin3_kv){alias->name, net.v});
    assert(st == ODIN3_OK); /* the key is present: an overwrite never allocates */
    (void)st;
}

void odin3_alias_absorb(odin3_module *module, odin3_net_id keep, odin3_net_id drop) {
    odin3_net_rec *krec = odin3_net_rec_at(module, keep);
    odin3_net_rec *drec = odin3_net_rec_at(module, drop);
    uint32_t tail = 0;
    for (uint32_t idx = drec->alias_head; idx != 0; idx = alias_at(module, idx)->next) {
        odin3_alias_rec *rec = alias_at(module, idx);
        rec->net = keep;
        alias_repoint(module, rec, keep);
        tail = idx;
    }
    if (tail != 0) { /* drop's chain goes in front of keep's */
        alias_at(module, tail)->next = krec->alias_head;
        krec->alias_head = drec->alias_head;
        drec->alias_head = 0;
    }
    if (odin3_wire_valid(drec->wire)) {
        odin3_alias_rec primary = {keep, drec->wire, drec->wire_bit, 0, 0};
        alias_repoint(module, &primary, keep);
        alias_push(module, keep, (odin3_net_alias){{drec->wire, drec->wire_bit}, 0});
        drec->wire = (odin3_wire_id){0};
        drec->wire_bit = 0;
    }
    if (drec->name != 0) {
        odin3_alias_rec name = {keep, {0}, 0, drec->name, 0};
        alias_repoint(module, &name, keep);
        alias_push(module, keep, (odin3_net_alias){{{0}, 0}, drec->name});
    }
}

/* --- wire creation ------------------------------------------------------------------------- */

static uint64_t range_width(const odin3_wire_spec *spec) {
    int64_t diff = (int64_t)spec->msb - (int64_t)spec->lsb;
    return (uint64_t)(diff < 0 ? -diff : diff) + 1;
}

static bool given_live(const odin3_module *module, const odin3_wire_plan *plan, const char *what) {
    for (uint32_t k = 0; k < plan->width; k++) {
        if (!odin3_net_live(module, plan->given[k])) {
            odin3_log(ODIN3_LOG_ERROR, "%s: net %u is not a live net", what, plan->given[k].v);
            return false;
        }
    }
    return true;
}

/* Checks the spec and the given nets; logs and returns false on the first problem. */
static bool plan_valid(odin3_module *module, odin3_wire_plan *plan, const char *what) {
    const odin3_wire_spec *spec = plan->spec;
    if (spec == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: no wire spec", what);
        return false;
    }
    uint64_t width = range_width(spec);
    if (width > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "%s: range [%d:%d] is wider than %u bits", what, spec->msb,
                  spec->lsb, UINT32_MAX);
        return false;
    }
    plan->width = (uint32_t)width;
    plan->id.v = odin3_module_wire_end(module);
    odin3_name_change name = {module->wire_names, what, plan->id.v, 0, spec->name};
    return odin3_names_available(module, &name) &&
           (plan->given == NULL || given_live(module, plan, what));
}

odin3_status odin3_wire_prepare(odin3_module *module, odin3_wire_plan *plan, const char *what) {
    if (!plan_valid(module, plan, what)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_module_reserve(module->wires, 1);
    if (st == ODIN3_OK) {
        st = plan->given == NULL ? odin3_module_reserve(module->nets, plan->width)
                                 : odin3_alias_reserve(module, plan->width);
    }
    if (st == ODIN3_OK) {
        plan->nets = odin3_arena_alloc(module->arena, sizeof(odin3_net_id) * (size_t)plan->width);
        st = plan->nets != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

/* (wire, bit) becomes the net's primary when it has none, else an alias (reserved). */
static void bind_bit(odin3_module *module, odin3_net_id net, odin3_wirebit wb) {
    odin3_net_rec *rec = odin3_net_rec_at(module, net);
    if (!odin3_wire_valid(rec->wire)) {
        rec->wire = wb.wire;
        rec->wire_bit = wb.bit;
        return;
    }
    alias_push(module, net, (odin3_net_alias){wb, 0});
}

/* A new net (reserved) whose primary is (wire, bit). */
static odin3_net_id new_member_net(odin3_module *module, odin3_prov_id prov, odin3_wirebit wb) {
    odin3_net_id id = {odin3_module_net_end(module)};
    odin3_net_rec *rec = odin3_pagevec_push(module->nets, NULL);
    assert(rec != NULL); /* reserved by odin3_wire_prepare */
    rec->prov = prov;
    rec->wire = wb.wire;
    rec->wire_bit = wb.bit;
    return id;
}

void odin3_wire_commit(odin3_module *module, const odin3_wire_plan *plan) {
    const odin3_wire_spec *spec = plan->spec;
    odin3_wire_rec *rec = odin3_pagevec_push(module->wires, NULL);
    assert(rec != NULL && odin3_module_wire_end(module) == plan->id.v + 1);
    rec->nets = plan->nets;
    rec->name = spec->name;
    rec->prov = spec->prov;
    rec->msb = spec->msb;
    rec->lsb = spec->lsb;
    rec->width = plan->width;
    rec->is_signed = spec->is_signed;
    for (uint32_t k = 0; k < plan->width; k++) {
        odin3_wirebit wb = {plan->id, k};
        if (plan->given != NULL) {
            bind_bit(module, plan->given[k], wb);
            rec->nets[k] = plan->given[k];
        } else {
            rec->nets[k] = new_member_net(module, spec->prov, wb);
        }
    }
}

odin3_status odin3_wire_create(odin3_module *module, const odin3_wire_spec *spec,
                               const odin3_net_id *nets, odin3_wire_id *out) {
    odin3_wire_plan plan = {spec, nets, NULL, 0, {0}};
    odin3_status st = odin3_wire_prepare(module, &plan, "wire_create");
    if (st == ODIN3_OK) {
        odin3_name_change name = {module->wire_names, "wire_create", plan.id.v, 0, spec->name};
        st = odin3_names_change(module, &name); /* last fallible step */
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_wire_commit(module, &plan);
    if (out != NULL) {
        *out = plan.id;
    }
    return ODIN3_OK;
}

/* --- aliases ------------------------------------------------------------------------------- */

/* The wire record for add_alias, or NULL (logged) when wb is not a bit of a non-port wire. */
static odin3_wire_rec *alias_target(odin3_module *module, odin3_wirebit wb) {
    odin3_wire_rec *wire = odin3_wire_live_rec(module, wb.wire, "wire_add_alias");
    if (wire == NULL) {
        return NULL;
    }
    if (wb.bit >= wire->width) {
        odin3_log(ODIN3_LOG_ERROR, "wire_add_alias: wire %u has no bit %u", wb.wire.v, wb.bit);
        return NULL;
    }
    if (odin3_node_valid(wire->port_node)) {
        odin3_log(ODIN3_LOG_ERROR, "wire_add_alias: wire %u is a port; its bits follow the port",
                  wb.wire.v);
        return NULL;
    }
    return wire;
}

odin3_status odin3_wire_add_alias(odin3_module *module, odin3_wirebit wb, odin3_net_id net) {
    odin3_wire_rec *wire = alias_target(module, wb);
    odin3_net_rec *nrec = wire != NULL ? odin3_net_live_rec(module, net, "wire_add_alias") : NULL;
    if (nrec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id old = wire->nets[wb.bit];
    if (old.v == net.v) {
        return ODIN3_OK;
    }
    odin3_net_rec *orec = odin3_net_rec_at(module, old);
    if (orec->wire.v == wb.wire.v && orec->wire_bit == wb.bit) { /* old's primary moves */
        odin3_status st = odin3_alias_reserve(module, 1);
        if (st != ODIN3_OK) {
            return st;
        }
        orec->wire = (odin3_wire_id){0};
        orec->wire_bit = 0;
        alias_push(module, net, (odin3_net_alias){wb, 0});
    } else { /* old's alias record moves to net */
        uint32_t idx = alias_unlink(module, orec, wb);
        odin3_alias_rec *rec = alias_at(module, idx);
        rec->net = net;
        rec->next = nrec->alias_head;
        nrec->alias_head = idx;
    }
    wire->nets[wb.bit] = net;
    return ODIN3_OK;
}

odin3_wirebit odin3_net_primary(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL ? (odin3_wirebit){rec->wire, rec->wire_bit} : (odin3_wirebit){{0}, 0};
}

bool odin3_net_alias_next(const odin3_module *module, odin3_net_id net, uint32_t *cursor,
                          odin3_net_alias *alias) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    if (rec == NULL || *cursor == ALIAS_END) {
        return false;
    }
    uint32_t idx = *cursor == 0 ? rec->alias_head : *cursor;
    if (idx == 0) {
        *cursor = ALIAS_END;
        return false;
    }
    const odin3_alias_rec *arec = odin3_vec_cat(&module->aliases, idx);
    alias->wb = (odin3_wirebit){arec->wire, arec->bit};
    alias->name = arec->name;
    *cursor = arec->next != 0 ? arec->next : ALIAS_END;
    return true;
}

uint32_t odin3_net_alias_count(const odin3_module *module, odin3_net_id net) {
    uint32_t count = 0;
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        count++;
    }
    return count;
}

/* --- wire accessors ------------------------------------------------------------------------ */

bool odin3_wire_live(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL && !rec->dead;
}

uint32_t odin3_wire_name(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->name : 0;
}

odin3_prov_id odin3_wire_prov(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->prov : (odin3_prov_id){0};
}

int32_t odin3_wire_msb(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->msb : 0;
}

int32_t odin3_wire_lsb(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->lsb : 0;
}

bool odin3_wire_signed(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL && rec->is_signed;
}

uint32_t odin3_wire_width(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->width : 0;
}

odin3_node_id odin3_wire_port_node(const odin3_module *module, odin3_wire_id wire) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL ? rec->port_node : (odin3_node_id){0};
}

odin3_net_id odin3_wire_net(const odin3_module *module, odin3_wire_id wire, uint32_t bit) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    return rec != NULL && bit < rec->width ? rec->nets[bit] : (odin3_net_id){0};
}

int32_t odin3_wire_index(const odin3_module *module, odin3_wire_id wire, uint32_t bit) {
    const odin3_wire_rec *rec = odin3_wire_rec_cat(module, wire);
    if (rec == NULL || bit >= rec->width) {
        return 0;
    }
    int64_t lsb = rec->lsb;
    return (int32_t)(rec->msb >= rec->lsb ? lsb + bit : lsb - bit);
}
