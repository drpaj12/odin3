/*
 * share.c — opt_share (2F spec OP-11, §11; plan Task 14): cells of one class that share an operand
 * and each drive a whole data port of one $mux or $pmux are replaced by one cell on the shared
 * operand, fed by a new mux over the other operands.
 *
 * Soundness (OP-13) outranks parity with Yosys 0.55, so the rewrite is narrower than Yosys's
 * (spec §17 #44 to #46):
 * - a candidate drives a whole chunk of a data port (its whole Y, bit for bit, and nothing else
 *   reads those nets; the chunk may be part of a wider port, PHASE3 #52, and the cells of a group
 *   start at the same bit); Yosys also merges cells whose Y is wider than the chunk (spec 17 #44);
 * - the cells of a group have the same parameters, except the width of the muxed operand, which
 *   may differ only for the types whose extension composes (extends in k_kinds) and equal signed
 *   flags: the narrower operands are extended by that flag, as the cell would have done;
 * - an $add and a $sub mix only when the shared operand is not the subtracted one (a $sub's port
 *   B is; Yosys keys the shared operand on that too), the $sub operands are negated at a width
 *   of at least Y_WIDTH, so the sum modulo 2^Y_WIDTH is the same, and with equal signed flags;
 * - the shared operand may be on different ports of the cells only for the commutative types
 *   (extends the 2F spec: PHASE3 #52; `d + 1` and `1 - d` share the 1), with the signed flags
 *   equal role by role;
 * - the group's cell is a new node of the type, ports and name of the member on the first mux
 *   port (Yosys's ports[0], PHASE3 #52), and the members' Y nets are merged into its Y net.
 * Each rewrite builds the $neg cells, the new mux and the new cell before its first destructive
 * step, kit_connect; any later failure is a half-done rewrite (odin3_kit_half_done).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/log.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    PORT_A = 0,
    PORT_B = 1,
    PORT_Y = 2,
    PRM_A_SIGNED = 0,
    PRM_B_SIGNED = 1,
    PRM_A_WIDTH = 2,
    PRM_B_WIDTH = 3,
    PRM_Y_WIDTH = 4,
    N_BIN_PARAMS = 5,
    N_BIN_PORTS = 3,
    MUX_PORT_A = 0,
    MUX_PORT_B = 1,
    MUX_PORT_S = 2,
    MUX_PORT_Y = 3,
    MUX_PORTS = 4,
    PRM_MUX_WIDTH = 0,
    PRM_PMUX_S_WIDTH = 1,
    PRM_PMUX_N = 2,
    NEG_PORT_A = 0,
    NEG_PORT_Y = 1,
    NEG_PORTS = 2,
    NEG_PARAMS = 3, /* A_SIGNED, A_WIDTH, Y_WIDTH */
    NEG_PRM_A_WIDTH = 1,
    NEG_PRM_Y_WIDTH = 2,
    MIN_GROUP = 2
};

/* The cell types that can be merged. Types with one cls merge together ($add with $sub). extends:
 * a narrower muxed operand may be extended by the signed flag to the group's widest. */
typedef struct op_kind {
    const char *name;
    uint8_t cls;
    bool extends;
    bool commutes; /* f(A, B) = f(B, A) with the widths and flags swapped too */
    bool negates;  /* $sub: its port B is subtracted */
} op_kind;

static const op_kind k_kinds[] = {
    {"$add", 0, true, true, false},       {"$sub", 0, true, false, true},
    {"$mul", 1, true, true, false},       {"$div", 2, false, false, false},
    {"$mod", 3, false, false, false},     {"$and", 4, true, true, false},
    {"$or", 5, true, true, false},        {"$xor", 6, true, true, false},
    {"$xnor", 7, true, true, false},      {"$shl", 8, false, false, false},
    {"$shr", 9, false, false, false},     {"$sshr", 10, false, false, false},
    {"$shiftx", 11, false, false, false}, {"$eq", 12, true, true, false},
    {"$ne", 13, true, true, false},       {"$lt", 14, true, false, false},
    {"$le", 15, true, false, false},      {"$gt", 16, true, false, false},
    {"$ge", 17, true, false, false},      {"$logic_and", 18, true, true, false},
    {"$logic_or", 19, true, true, false},
};
#define N_KINDS (sizeof k_kinds / sizeof k_kinds[0])

typedef struct cand {
    uint32_t slot; /* 0 .. slices-1: B slice; slices: A */
    uint32_t op;
    uint32_t kind;
    uint32_t sport; /* the port the cell holds the shared operand on (set when it joins) */
    uint32_t off;   /* the bit of the mux's data port where the cell's Y starts */
    bool open;
} cand;

typedef struct sh_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    odin3_celltype_id kinds[N_KINDS];
    odin3_celltype_id mux, pmux, neg;
    odin3_node_id mux_node;
    odin3_node_id lead; /* the member on the first mux port: Yosys keeps its type and ports */
    uint32_t width, slices;
    odin3_vec cands;  /* cand */
    odin3_vec dn;     /* the mux's data nets: B slices, then A, width each */
    odin3_vec sn;     /* the mux's selects */
    odin3_vec group;  /* indices into cands, in port order */
    odin3_vec byid;   /* the group's cells (odin3_node_id), lowest ID first */
    odin3_vec shared; /* the shared operand's nets */
    odin3_vec opnd;   /* the muxed operands, wn nets per member in port order */
    odin3_vec mb;     /* the new mux's B nets */
    odin3_vec ms;     /* the new mux's S nets */
    odin3_vec ma;     /* the new mux's A nets */
    odin3_vec yn;     /* the new cell's Y nets */
    odin3_vec tmp;
    odin3_vec made; /* nodes this rewrite made (odin3_node_id), in creation order */
    /* the current group */
    uint32_t lead_kind, lead_port, wn, flag;
    bool mixed, has_a;
    uint32_t merged;
} sh_state;

static void vec_init_all(sh_state *ss) {
    odin3_vec_init(&ss->cands, sizeof(cand));
    odin3_vec *nets[] = {&ss->dn, &ss->sn, &ss->shared, &ss->opnd, &ss->mb,
                         &ss->ms, &ss->ma, &ss->yn,     &ss->tmp};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        odin3_vec_init(nets[i], sizeof(odin3_net_id));
    }
    odin3_vec_init(&ss->group, sizeof(uint32_t));
    odin3_vec_init(&ss->byid, sizeof(odin3_node_id));
    odin3_vec_init(&ss->made, sizeof(odin3_node_id));
}

static void state_init(sh_state *ss, odin3_opt_ctx *oc) {
    memset(ss, 0, sizeof *ss);
    ss->oc = oc;
    ss->m = oc->module;
    for (size_t i = 0; i < N_KINDS; i++) {
        ss->kinds[i] = odin3_kit_type_named(oc->design, k_kinds[i].name);
    }
    ss->mux = odin3_kit_type_named(oc->design, "$mux");
    ss->pmux = odin3_kit_type_named(oc->design, "$pmux");
    ss->neg = odin3_kit_type_named(oc->design, "$neg");
    vec_init_all(ss);
}

static void state_free(sh_state *ss) {
    odin3_vec *all[] = {&ss->cands, &ss->dn, &ss->sn, &ss->group, &ss->byid, &ss->shared, &ss->opnd,
                        &ss->mb,    &ss->ms, &ss->ma, &ss->yn,    &ss->tmp,  &ss->made};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
}

/* --- reading the IR --------------------------------------------------------------------------- */

static uint32_t kind_of(const sh_state *ss, odin3_celltype_id type) {
    for (uint32_t i = 0; i < N_KINDS; i++) {
        if (ss->kinds[i].v != 0 && ss->kinds[i].v == type.v) {
            return i;
        }
    }
    return (uint32_t)N_KINDS;
}

static uint32_t port_width(const sh_state *ss, odin3_node_id node, uint32_t port) {
    return odin3_node_port(ss->m, node, port).count;
}

static int64_t param_int(const sh_state *ss, odin3_node_id node, uint32_t index) {
    return odin3_node_param(ss->m, node, index)->i;
}

/* One port of a node. */
typedef struct port_ref {
    odin3_node_id node;
    uint32_t port;
} port_ref;

static odin3_net_id port_net(const sh_state *ss, port_ref at, size_t bit) {
    odin3_pinslice ps = odin3_node_port(ss->m, at.node, at.port);
    return odin3_pin_net(ss->m, (odin3_pin_id){ps.first.v + (uint32_t)bit});
}

/* Appends the nets of one port of node to vec; none (a open pin) is appended as it is. */
static odin3_status push_port(const sh_state *ss, odin3_vec *vec, odin3_node_id node,
                              uint32_t port) {
    const uint32_t count = port_width(ss, node, port);
    odin3_status st = odin3_vec_reserve(vec, vec->len + count);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < count; bit++) {
        st = odin3_kit_push_net(vec, port_net(ss, (port_ref){node, port}, bit));
    }
    return st;
}

static const odin3_net_id *nets_at(const odin3_vec *vec, size_t index) {
    return (const odin3_net_id *)vec->data + index;
}

static bool wire_bit_keep(const sh_state *ss, odin3_wirebit wb) {
    odin3_objref obj = {ODIN3_OBJ_WIRE, wb.wire.v};
    return odin3_wire_valid(wb.wire) && odin3_kit_keep(ss->oc, obj);
}

/* True when a wire of the net has `keep` (its value is then observable by name). */
static bool net_has_keep_wire(const sh_state *ss, odin3_net_id net) {
    if (wire_bit_keep(ss, odin3_net_primary(ss->m, net))) {
        return true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(ss->m, net, &cursor, &alias)) {
        if (wire_bit_keep(ss, alias.wb)) {
            return true;
        }
    }
    return false;
}

static bool inputs_connected(const sh_state *ss, odin3_node_id op) {
    for (uint32_t port = PORT_A; port <= PORT_B; port++) {
        odin3_pinslice ps = odin3_node_port(ss->m, op, port);
        for (uint32_t bit = 0; bit < ps.count; bit++) {
            if (!odin3_net_valid(odin3_pin_net(ss->m, (odin3_pin_id){ps.first.v + bit}))) {
                return false;
            }
        }
    }
    return true;
}

/* The whole of the cell's Y is nets[0 .. Y_WIDTH), nothing else reads them, and none is kept. */
static bool y_is_port(const sh_state *ss, odin3_node_id op, const odin3_net_id *nets,
                      uint32_t avail) {
    const uint32_t ylen = port_width(ss, op, PORT_Y);
    if (ylen > avail) {
        return false;
    }
    for (uint32_t bit = 0; bit < ylen; bit++) {
        odin3_net_id net = nets[bit];
        if (!odin3_net_valid(net) || net.v != port_net(ss, (port_ref){op, PORT_Y}, bit).v ||
            odin3_net_driver_count(ss->m, net) != 1 || odin3_kit_readers(ss->oc, net) != 1 ||
            net_has_keep_wire(ss, net)) {
            return false;
        }
    }
    return true;
}

/* Does the data port `slot` hold a candidate that starts at bit `off`? Fills *out when it does:
 * a cell whose whole Y is bits off .. off + Y_WIDTH of the port (PHASE3 #52, Yosys's chunks). */
static bool cand_at(const sh_state *ss, uint32_t slot, uint32_t off, cand *out) {
    const odin3_net_id *nets = nets_at(&ss->dn, (size_t)slot * ss->width + off);
    if (!odin3_net_valid(nets[0])) {
        return false;
    }
    odin3_pin_id drv = odin3_net_driver(ss->m, nets[0]);
    if (!odin3_pin_valid(drv)) {
        return false;
    }
    odin3_node_id op = odin3_pin_node(ss->m, drv);
    const uint32_t kind = kind_of(ss, odin3_node_type(ss->m, op));
    odin3_objref obj = {ODIN3_OBJ_NODE, op.v};
    if (kind == N_KINDS || !y_is_port(ss, op, nets, ss->width - off) || !inputs_connected(ss, op) ||
        odin3_kit_keep(ss->oc, obj)) {
        return false;
    }
    if (slot < ss->slices) {
        const odin3_net_id sel = *nets_at(&ss->sn, slot);
        const odin3_const cv = odin3_net_const_value(ss->m, sel);
        if (!odin3_net_valid(sel) || cv == ODIN3_CONST_X || cv == ODIN3_CONST_Z) {
            return false;
        }
    }
    *out = (cand){slot, op.v, kind, PORT_A, off, true};
    return true;
}

/* Reads the mux's ports into dn and sn. */
static odin3_status load_mux(sh_state *ss, odin3_node_id node, bool is_pmux) {
    ss->mux_node = node;
    ss->width = (uint32_t)param_int(ss, node, PRM_MUX_WIDTH);
    ss->slices = is_pmux ? (uint32_t)param_int(ss, node, PRM_PMUX_S_WIDTH) : 1U;
    odin3_vec_clear(&ss->dn);
    odin3_vec_clear(&ss->sn);
    odin3_status st = push_port(ss, &ss->dn, node, MUX_PORT_B);
    st = st == ODIN3_OK ? push_port(ss, &ss->dn, node, MUX_PORT_A) : st;
    return st == ODIN3_OK ? push_port(ss, &ss->sn, node, MUX_PORT_S) : st;
}

static odin3_status push_cand(sh_state *ss, const cand *cnd) {
    cand *dst = odin3_vec_push(&ss->cands);
    if (dst == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *dst = *cnd;
    return ODIN3_OK;
}

/* The candidates of every data port, in port order and, within a port, by offset. */
static odin3_status gather(sh_state *ss) {
    odin3_vec_clear(&ss->cands);
    odin3_status st = ODIN3_OK;
    for (uint32_t slot = 0; st == ODIN3_OK && slot <= ss->slices; slot++) {
        for (uint32_t off = 0; st == ODIN3_OK && off < ss->width;) {
            cand cnd;
            if (cand_at(ss, slot, off, &cnd)) {
                st = push_cand(ss, &cnd);
                off += port_width(ss, (odin3_node_id){cnd.op}, PORT_Y);
            } else {
                off++;
            }
        }
    }
    return st;
}

/* --- choosing a group ------------------------------------------------------------------------- */

static uint32_t other_port(uint32_t port) {
    return port == PORT_A ? PORT_B : PORT_A;
}

/* The nets of one port of lhs and of one port of rhs are the same, bit for bit. */
static bool ports_equal(const sh_state *ss, port_ref lhs, port_ref rhs) {
    const uint32_t count = port_width(ss, lhs.node, lhs.port);
    if (count != port_width(ss, rhs.node, rhs.port)) {
        return false;
    }
    for (uint32_t bit = 0; bit < count; bit++) {
        if (port_net(ss, lhs, bit).v != port_net(ss, rhs, bit).v) {
            return false;
        }
    }
    return true;
}

static int64_t signed_of(const sh_state *ss, odin3_node_id node, uint32_t port) {
    return param_int(ss, node, port == PORT_A ? PRM_A_SIGNED : PRM_B_SIGNED);
}

/* The parameters agree role by role (the width of the muxed operand apart): the shared operand
 * (port sport of the seed, port cport of the cell), the muxed one, and Y_WIDTH. With cport == sport
 * this is "all parameters equal". */
static bool params_match(const sh_state *ss, port_ref seed, odin3_node_id cell, uint32_t cport) {
    return signed_of(ss, seed.node, seed.port) == signed_of(ss, cell, cport) &&
           signed_of(ss, seed.node, other_port(seed.port)) ==
               signed_of(ss, cell, other_port(cport)) &&
           param_int(ss, seed.node, PRM_Y_WIDTH) == param_int(ss, cell, PRM_Y_WIDTH);
}

/* Conservative (spec 17 #45): a mixed-sign cell is unsigned (#57), but a group still needs equal
 * flags. */
static bool signs_equal(const sh_state *ss, odin3_node_id op) {
    return param_int(ss, op, PRM_A_SIGNED) == param_int(ss, op, PRM_B_SIGNED);
}

/* Is the operand on `port` of the cell subtracted ($sub's port B)? Yosys keys the shared operand
 * on this (opt_share.cc decode_port_sign), so an $add and a $sub share only a port that is not
 * subtracted in either. */
static bool subtracted(const cand *cnd, uint32_t port) {
    return k_kinds[cnd->kind].negates && port == PORT_B;
}

/* Can cell cnd, holding the shared operand on port cport, join the group of seed, which holds it on
 * port sport? cport != sport is the commutative swap (spec 17 #52): the new cell keeps the lead's
 * ports, so the member's other operand goes to the lead's muxed port. */
static bool joins_on(const sh_state *ss, const cand *seed, uint32_t sport, const cand *cnd,
                     uint32_t cport) {
    const port_ref sref = {{seed->op}, sport};
    const port_ref cref = {{cnd->op}, cport};
    if (!ports_equal(ss, sref, cref) || !params_match(ss, sref, cref.node, cport) ||
        subtracted(seed, sport) != subtracted(cnd, cport)) {
        return false;
    }
    if (cport != sport && !k_kinds[seed->kind].commutes && !k_kinds[cnd->kind].commutes) {
        return false;
    }
    if (seed->kind != cnd->kind && (!signs_equal(ss, sref.node) || ss->neg.v == 0)) {
        return false;
    }
    return port_width(ss, sref.node, other_port(sport)) ==
               port_width(ss, cref.node, other_port(cport)) ||
           (k_kinds[seed->kind].extends && signs_equal(ss, sref.node));
}

/* Can c join the group of seed, sharing the operand held on port sport of the seed? On a yes
 * *at (when not NULL) is the port the cell holds it on: sport when it can, else the other. */
static bool joins(const sh_state *ss, const cand *seed, const cand *cnd, uint32_t sport,
                  uint32_t *at) {
    if (k_kinds[seed->kind].cls != k_kinds[cnd->kind].cls || seed->off != cnd->off) {
        return false;
    }
    for (uint32_t pick = 0; pick < 2U; pick++) {
        const uint32_t port = pick == 0 ? sport : other_port(sport);
        if (joins_on(ss, seed, sport, cnd, port)) {
            if (at != NULL) {
                *at = port;
            }
            return true;
        }
    }
    return false;
}

static uint32_t count_joiners(const sh_state *ss, size_t seed, uint32_t sport) {
    const cand *all = ss->cands.data;
    uint32_t hits = 0;
    for (size_t i = seed + 1U; i < ss->cands.len; i++) {
        hits += all[i].open && joins(ss, &all[seed], &all[i], sport, NULL) ? 1U : 0U;
    }
    return hits;
}

/* The port to share on: port A when another open candidate shares it, else port B (Yosys's
 * find_shared_operand tries A first). */
static bool choose_port(const sh_state *ss, size_t seed, uint32_t *port) {
    if (count_joiners(ss, seed, PORT_A) != 0) {
        *port = PORT_A;
        return true;
    }
    *port = PORT_B;
    return count_joiners(ss, seed, PORT_B) != 0;
}

/* group = the seed and the open candidates that join it, in port order; each member's sport is
 * the port it holds the shared operand on. */
static odin3_status build_group(sh_state *ss, size_t seed, uint32_t sport) {
    cand *all = ss->cands.data;
    odin3_vec_clear(&ss->group);
    for (size_t i = seed; i < ss->cands.len; i++) {
        uint32_t at = sport;
        if (i == seed || (all[i].open && joins(ss, &all[seed], &all[i], sport, &at))) {
            uint32_t *slot = odin3_vec_push(&ss->group);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = (uint32_t)i;
            all[i].sport = at;
        }
    }
    return ODIN3_OK;
}

static int cmp_node(const void *lhs, const void *rhs) {
    const uint32_t lhs_id = ((const odin3_node_id *)lhs)->v;
    const uint32_t rhs_id = ((const odin3_node_id *)rhs)->v;
    if (lhs_id == rhs_id) {
        return 0;
    }
    return lhs_id < rhs_id ? -1 : 1;
}

/* byid = the group's cells, lowest ID first (for the record); the lead (the first member in port
 * order, as Yosys's ports[0]) and the widths follow. */
static odin3_status group_facts(sh_state *ss) {
    const cand *all = ss->cands.data;
    const uint32_t *idx = ss->group.data;
    odin3_vec_clear(&ss->byid);
    uint32_t wmax = 0;
    for (size_t i = 0; i < ss->group.len; i++) {
        odin3_node_id *dst = odin3_vec_push(&ss->byid);
        if (dst == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *dst = (odin3_node_id){all[idx[i]].op};
        const uint32_t wid = port_width(ss, *dst, other_port(all[idx[i]].sport));
        wmax = wid > wmax ? wid : wmax;
    }
    qsort(ss->byid.data, ss->byid.len, sizeof(odin3_node_id), cmp_node);
    const odin3_node_id lead = {all[idx[0]].op};
    ss->lead = lead;
    ss->lead_kind = kind_of(ss, odin3_node_type(ss->m, lead));
    ss->mixed = false;
    for (size_t i = 0; i < ss->group.len; i++) {
        ss->mixed = ss->mixed || all[idx[i]].kind != ss->lead_kind;
        ss->lead_port = all[idx[i]].op == lead.v ? all[idx[i]].sport : ss->lead_port;
    }
    const uint32_t ywidth = (uint32_t)param_int(ss, lead, PRM_Y_WIDTH);
    ss->wn = ss->mixed && ywidth > wmax ? ywidth : wmax;
    ss->flag = (uint32_t)param_int(ss, lead, PRM_A_SIGNED);
    ss->has_a = all[idx[ss->group.len - 1U]].slot == ss->slices;
    return ODIN3_OK;
}

/* --- building --------------------------------------------------------------------------------- */

/* Creates a cell and records it in `made` in one step: room is reserved first, so the push cannot
 * fail once the cell exists and the rollback always sees it. */
static odin3_status new_cell(sh_state *ss, const odin3_kit_cell *cell, odin3_node_id *out) {
    odin3_status st = odin3_vec_reserve(&ss->made, ss->made.len + 1U);
    st = st == ODIN3_OK ? odin3_kit_new_cell(ss->oc, cell, out) : st;
    if (st == ODIN3_OK) {
        *(odin3_node_id *)odin3_vec_push(&ss->made) = *out;
    }
    return st;
}

/* Deletes a node this rewrite made and the output nets it leaves without pins; the first failure
 * is returned (a delete can fail on allocation) and the rest is still tried. */
static odin3_status undo_node(sh_state *ss, odin3_node_id node, uint32_t out_port) {
    if (!odin3_node_live(ss->m, node)) {
        return ODIN3_OK;
    }
    odin3_vec_clear(&ss->tmp);
    odin3_status first = push_port(ss, &ss->tmp, node, out_port);
    odin3_status st = odin3_node_delete(ss->m, node);
    first = first == ODIN3_OK ? st : first;
    for (size_t i = 0; i < ss->tmp.len; i++) {
        odin3_net_id net = *nets_at(&ss->tmp, i);
        if (odin3_net_valid(net) && odin3_net_live(ss->m, net) &&
            odin3_net_pins(ss->m, net).count == 0) {
            st = odin3_net_delete(ss->m, net);
            first = first == ODIN3_OK ? st : first;
        }
    }
    return first;
}

/* Undoes the made nodes, newest first; the output port of each is its last one. Returns the first
 * failure. */
static odin3_status undo_made(sh_state *ss) {
    odin3_status first = ODIN3_OK;
    while (ss->made.len > 0) {
        const odin3_node_id node =
            *(const odin3_node_id *)odin3_vec_cat(&ss->made, ss->made.len - 1U);
        const odin3_celltype_def *def =
            odin3_celltype_get(ss->oc->design, odin3_node_type(ss->m, node));
        odin3_status st = undo_node(ss, node, def->n_ports - 1U);
        first = first == ODIN3_OK ? st : first;
        odin3_vec_pop(&ss->made);
    }
    return first;
}

/* An undone failure stays plain; a rollback that itself failed leaves dead cells: half done. */
static odin3_status rolled_back(sh_state *ss, odin3_status st) {
    return undo_made(ss) == ODIN3_OK ? st : odin3_kit_half_done(ss->oc, st);
}

/* The canonical net of constant bit `bit`. */
static odin3_status const_net(sh_state *ss, odin3_bit bit, odin3_net_id *out) {
    return odin3_opt_const_net(ss->oc, bit, out);
}

/* tmp-free: appends the operand of member `node` (port mport), extended to wn nets, to opnd. */
static odin3_status push_operand(sh_state *ss, odin3_node_id node, uint32_t mport) {
    const uint32_t have = port_width(ss, node, mport);
    odin3_status st = push_port(ss, &ss->opnd, node, mport);
    odin3_net_id fill = port_net(ss, (port_ref){node, mport}, have - 1U);
    if (st == ODIN3_OK && have < ss->wn && ss->flag == 0) {
        st = const_net(ss, ODIN3_BIT_0, &fill);
    }
    for (uint32_t bit = have; st == ODIN3_OK && bit < ss->wn; bit++) {
        st = odin3_kit_push_net(&ss->opnd, fill);
    }
    return st;
}

/* A $neg of the last wn nets of opnd (a member whose type differs from the lead's); they are
 * replaced by its Y nets. */
static odin3_status negate_last(sh_state *ss, odin3_prov_id prov) {
    const size_t first = ss->opnd.len - ss->wn;
    odin3_value params[NEG_PARAMS] = {odin3_value_int((int64_t)ss->flag),
                                      odin3_value_int((int64_t)ss->wn),
                                      odin3_value_int((int64_t)ss->wn)};
    const odin3_netvec ports[NEG_PORTS] = {{nets_at(&ss->opnd, first), ss->wn}, {NULL, 0}};
    odin3_kit_cell cell = {ss->neg, params, NEG_PARAMS, ports, prov};
    odin3_node_id node = {0};
    odin3_status st = new_cell(ss, &cell, &node);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vec_clear(&ss->tmp);
    st = push_port(ss, &ss->tmp, node, NEG_PORT_Y);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < ss->wn; bit++) {
        odin3_net_id *slot = odin3_vec_at(&ss->opnd, first + bit);
        *slot = *nets_at(&ss->tmp, bit);
    }
    return st;
}

/* opnd = the muxed operand of every member in port order (extended, negated where needed). */
static odin3_status build_operands(sh_state *ss, odin3_prov_id prov) {
    const cand *all = ss->cands.data;
    const uint32_t *idx = ss->group.data;
    odin3_vec_clear(&ss->opnd);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ss->group.len; i++) {
        const cand *cnd = &all[idx[i]];
        st = push_operand(ss, (odin3_node_id){cnd->op}, other_port(cnd->sport));
        st = st == ODIN3_OK && cnd->kind != ss->lead_kind ? negate_last(ss, prov) : st;
    }
    return st;
}

static odin3_status append_span(sh_state *ss, odin3_vec *dst, size_t member) {
    for (uint32_t bit = 0; bit < ss->wn; bit++) {
        odin3_status st = odin3_kit_push_net(dst, *nets_at(&ss->opnd, member * ss->wn + bit));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The new mux's A, B and S nets from opnd and the old mux's selects (no A in the group: x). */
static odin3_status mux_inputs(sh_state *ss) {
    const cand *all = ss->cands.data;
    const uint32_t *idx = ss->group.data;
    odin3_vec_clear(&ss->ma);
    odin3_vec_clear(&ss->mb);
    odin3_vec_clear(&ss->ms);
    odin3_net_id unknown = {0};
    odin3_status st = ss->has_a ? ODIN3_OK : const_net(ss, ODIN3_BIT_X, &unknown);
    for (uint32_t k = 0; st == ODIN3_OK && !ss->has_a && k < ss->wn; k++) {
        st = odin3_kit_push_net(&ss->ma, unknown);
    }
    for (size_t i = 0; st == ODIN3_OK && i < ss->group.len; i++) {
        const uint32_t slot = all[idx[i]].slot;
        if (slot == ss->slices) {
            st = append_span(ss, &ss->ma, i);
        } else {
            st = append_span(ss, &ss->mb, i);
            st = st == ODIN3_OK ? odin3_kit_push_net(&ss->ms, *nets_at(&ss->sn, slot)) : st;
        }
    }
    return st;
}

/* The new mux: a $mux for one select, else a $pmux. */
static odin3_status make_mux(sh_state *ss, odin3_prov_id prov, odin3_node_id *out) {
    const bool single = ss->ms.len == 1;
    odin3_value params[PRM_PMUX_N] = {odin3_value_int((int64_t)ss->wn),
                                      odin3_value_int((int64_t)ss->ms.len)};
    const odin3_netvec ports[MUX_PORTS] = {{ss->ma.data, (uint32_t)ss->ma.len},
                                           {ss->mb.data, (uint32_t)ss->mb.len},
                                           {ss->ms.data, (uint32_t)ss->ms.len},
                                           {NULL, 0}};
    odin3_kit_cell cell = {single ? ss->mux : ss->pmux, params, single ? 1U : PRM_PMUX_N, ports,
                           prov};
    return new_cell(ss, &cell, out);
}

/* The new cell: the lead's type and parameters, the shared operand, the new mux's Y. */
static odin3_status make_op(sh_state *ss, odin3_node_id lead, odin3_node_id mux_new,
                            odin3_prov_id prov, odin3_node_id *out) {
    odin3_value params[N_BIN_PARAMS];
    for (uint32_t i = 0; i < N_BIN_PARAMS; i++) {
        params[i] = *odin3_node_param(ss->m, lead, i);
    }
    const uint32_t mport = other_port(ss->lead_port);
    params[mport == PORT_A ? PRM_A_WIDTH : PRM_B_WIDTH] = odin3_value_int((int64_t)ss->wn);
    odin3_vec_clear(&ss->shared);
    odin3_vec_clear(&ss->tmp);
    odin3_status st = push_port(ss, &ss->shared, lead, ss->lead_port);
    st = st == ODIN3_OK ? push_port(ss, &ss->tmp, mux_new, MUX_PORT_Y) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_netvec ports[N_BIN_PORTS] = {{NULL, 0}, {NULL, 0}, {NULL, 0}};
    ports[ss->lead_port] = (odin3_netvec){ss->shared.data, (uint32_t)ss->shared.len};
    ports[mport] = (odin3_netvec){ss->tmp.data, (uint32_t)ss->tmp.len};
    odin3_kit_cell cell = {odin3_node_type(ss->m, lead), params, N_BIN_PARAMS, ports, prov};
    st = new_cell(ss, &cell, out);
    return st == ODIN3_OK ? odin3_kit_copy_attrs(ss->m, lead, *out) : st;
}

/* The record of the rewrite: the group's cells in ID order, then the old mux. */
static odin3_status group_prov(sh_state *ss, odin3_prov_id *prov) {
    odin3_node_id *tail = odin3_vec_push(&ss->byid);
    if (tail == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *tail = ss->mux_node;
    odin3_status st = odin3_kit_prov(ss->oc, ss->byid.data, (uint32_t)ss->byid.len, prov);
    odin3_vec_pop(&ss->byid);
    return st;
}

/* --- the destructive part --------------------------------------------------------------------- */

static uint32_t members_gone(const sh_state *ss) {
    const odin3_node_id *ids = ss->byid.data;
    uint32_t gone = 0;
    for (size_t i = 0; i < ss->byid.len; i++) {
        gone += odin3_node_live(ss->m, ids[i]) ? 0U : 1U;
    }
    return gone;
}

/* Deletes the members, merging each one's Y nets into the new cell's; the new cell takes the
 * lead's name. */
static odin3_status commit(sh_state *ss, odin3_node_id op) {
    const odin3_node_id *ids = ss->byid.data;
    const uint32_t name = odin3_node_name(ss->m, ss->lead);
    odin3_vec_clear(&ss->yn);
    odin3_status st = push_port(ss, &ss->yn, op, PORT_Y);
    for (size_t i = 0; st == ODIN3_OK && i < ss->byid.len; i++) {
        const odin3_kit_conn conn = {ids[i], PORT_Y, ss->yn.data, (uint32_t)ss->yn.len};
        st = odin3_kit_connect(ss->oc, &conn);
    }
    if (st == ODIN3_OK && name != 0) {
        st = odin3_node_rename(ss->m, op, name);
    }
    if (st != ODIN3_OK && members_gone(ss) == 0) {
        return rolled_back(ss, st);
    }
    return st == ODIN3_OK ? st : odin3_kit_half_done(ss->oc, st);
}

/* One rewrite of the current group. */
static odin3_status apply_group(sh_state *ss) {
    odin3_kit_begin_rewrite(ss->oc);
    odin3_vec_clear(&ss->made);
    odin3_prov_id prov = {0};
    odin3_status st = group_facts(ss);
    st = st == ODIN3_OK ? group_prov(ss, &prov) : st;
    st = st == ODIN3_OK ? build_operands(ss, prov) : st;
    st = st == ODIN3_OK ? mux_inputs(ss) : st;
    odin3_node_id mux_new = {0};
    odin3_node_id op = {0};
    st = st == ODIN3_OK ? make_mux(ss, prov, &mux_new) : st;
    st = st == ODIN3_OK ? make_op(ss, ss->lead, mux_new, prov, &op)
                        : st;
    if (st != ODIN3_OK) {
        return rolled_back(ss, st);
    }
    st = commit(ss, op);
    /* a half-done rewrite counts the members it already deleted */
    ss->merged += st == ODIN3_OK ? (uint32_t)ss->byid.len : members_gone(ss);
    return st;
}

/* --- one mux, the module, the pass ------------------------------------------------------------ */

static odin3_status mux_visit(sh_state *ss, odin3_node_id node, bool is_pmux) {
    odin3_status st = load_mux(ss, node, is_pmux);
    st = st == ODIN3_OK ? gather(ss) : st;
    for (size_t seed = 0; st == ODIN3_OK && seed + 1U < ss->cands.len; seed++) {
        uint32_t port = PORT_A;
        cand *all = ss->cands.data;
        if (!all[seed].open) {
            continue;
        }
        all[seed].open = false;
        if (!choose_port(ss, seed, &port)) {
            continue;
        }
        st = build_group(ss, seed, port);
        for (size_t i = 0; st == ODIN3_OK && i < ss->group.len; i++) {
            all[((const uint32_t *)ss->group.data)[i]].open = false;
        }
        st = st == ODIN3_OK ? apply_group(ss) : st;
    }
    return st;
}

static odin3_status share_module(odin3_opt_ctx *oc, uint32_t *total) {
    sh_state ss;
    state_init(&ss, oc);
    odin3_status st = ODIN3_OK;
    const uint32_t end = odin3_module_node_end(oc->module);
    for (uint32_t id = 1; ss.mux.v != 0 && ss.pmux.v != 0 && st == ODIN3_OK && id < end; id++) {
        const odin3_node_id node = {id};
        if (!odin3_node_live(ss.m, node)) {
            continue;
        }
        const odin3_celltype_id type = odin3_node_type(ss.m, node);
        if (type.v == ss.mux.v || type.v == ss.pmux.v) {
            st = mux_visit(&ss, node, type.v == ss.pmux.v);
        }
    }
    *total += ss.merged;
    state_free(&ss);
    return st;
}

odin3_status odin3_opt_share_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_share: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t total = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = share_module(&oc, &total);
        odin3_opt_ctx_free(&oc);
    }
    counts->share_merged += total;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "opt_share: merged %" PRIu32 " cells", total);
    }
    return st;
}
