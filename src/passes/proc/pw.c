/*
 * pw.c — the working form of a process: decode from a $proc node (2E spec §4, PR-6), free, and the
 * bit key that orders bits as Yosys's sort_and_unify does (§4, "Bit order").
 */
#include "passes/proc/pw.h"

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_internal.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { KEY_WIRE_SHIFT = 32 };

/* The working form's constant of each odin3_const (NONE has none). */
static const odin3_pw_bit k_const_bits[] = {0, ODIN3_PW_S0, ODIN3_PW_S1, ODIN3_PW_SX, ODIN3_PW_SZ};

uint64_t odin3_proc_bit_key(const odin3_module *module, odin3_pw_bit bit) {
    if ((bit & ODIN3_PW_CONST) != 0) {
        return bit & ~ODIN3_PW_CONST;
    }
    odin3_wirebit wb = odin3_net_primary(module, (odin3_net_id){bit});
    if (odin3_wire_valid(wb.wire)) {
        return (uint64_t)wb.wire.v << KEY_WIRE_SHIFT | wb.bit;
    }
    return (uint64_t)UINT32_MAX << KEY_WIRE_SHIFT | bit;
}

/* An empty working form: every vector initialised, nothing allocated. */
static void pw_init(odin3_pw *pw) {
    memset(pw, 0, sizeof *pw);
    odin3_vec_init(&pw->bits, sizeof(odin3_pw_bit));
    odin3_vec_init(&pw->cases, sizeof(odin3_pw_case));
    odin3_vec_init(&pw->switches, sizeof(odin3_pw_switch));
    odin3_vec_init(&pw->syncs, sizeof(odin3_pw_sync));
    odin3_vec_init(&pw->promoted, sizeof(odin3_pw_action));
    odin3_vec_init(&pw->in_nets, sizeof(odin3_net_id));
    odin3_vec_init(&pw->attrs, sizeof(odin3_pw_attr));
}

void odin3_pw_free(odin3_pw *pw) {
    if (pw == NULL) {
        return;
    }
    for (size_t i = 0; i < pw->cases.len; i++) {
        odin3_pw_case *cs = odin3_vec_at(&pw->cases, i);
        odin3_vec_free(&cs->compares);
        odin3_vec_free(&cs->actions);
        odin3_vec_free(&cs->switches);
    }
    for (size_t i = 0; i < pw->switches.len; i++) {
        odin3_vec_free(&((odin3_pw_switch *)odin3_vec_at(&pw->switches, i))->cases);
    }
    for (size_t i = 0; i < pw->syncs.len; i++) {
        odin3_vec_free(&((odin3_pw_sync *)odin3_vec_at(&pw->syncs, i))->actions);
    }
    odin3_vec_free(&pw->bits);
    odin3_vec_free(&pw->cases);
    odin3_vec_free(&pw->switches);
    odin3_vec_free(&pw->syncs);
    odin3_vec_free(&pw->promoted);
    odin3_vec_free(&pw->in_nets);
    odin3_vec_free(&pw->attrs);
}

/* --- decode -------------------------------------------------------------------------------- */

typedef struct decoder {
    const odin3_module *module;
    odin3_node_id node;
    odin3_proc_body body;
    odin3_pinslice in, outs; /* the I and O pins */
    odin3_pw *pw;
} decoder;

/* The node's verified body and pins; INVALID_ARG (logged) unless it is a live, valid $proc. */
static odin3_status dec_open(decoder *dec) {
    const odin3_module *module = dec->module;
    odin3_node_id node = dec->node;
    const odin3_celltype_def *def =
        odin3_node_live(module, node)
            ? odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))
            : NULL;
    if (def == NULL || (def->flags & ODIN3_CT_PROCESS) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: decode: node %u is not a live process", node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_proc_widths widths = {(uint32_t)odin3_node_param(module, node, ODIN3_PROC_P_I_WIDTH)->i,
                                (uint32_t)odin3_node_param(module, node, ODIN3_PROC_P_O_WIDTH)->i};
    odin3_proc_why why = {""};
    if (!odin3_proc_body_view(odin3_node_param(module, node, ODIN3_PROC_P_BODY), &dec->body) ||
        odin3_proc_body_verify(&dec->body, widths, &why) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "proc: decode: node %u: BODY: %s", node.v,
                  why.text[0] != '\0' ? why.text : "not a process body");
        return ODIN3_ERR_INVALID_ARG;
    }
    dec->in = odin3_node_port(module, node, ODIN3_PROC_PORT_I);
    dec->outs = odin3_node_port(module, node, ODIN3_PROC_PORT_O);
    return ODIN3_OK;
}

/* The bit of a position: its pin's net (a constant net as its constant), or SA for ANY. */
static odin3_status dec_bit(const decoder *dec, odin3_pbit pbit, odin3_pw_bit *out) {
    uint32_t tag = pbit & ~ODIN3_PBIT_INDEX_MASK;
    uint32_t idx = pbit & ODIN3_PBIT_INDEX_MASK;
    if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT) {
        *out = ODIN3_PW_SA; /* verify allows only ANY besides IN and OUT */
        return ODIN3_OK;
    }
    odin3_pinslice port = tag == ODIN3_PBIT_IN ? dec->in : dec->outs;
    odin3_net_id net = odin3_pin_net(dec->module, (odin3_pin_id){port.first.v + idx});
    if (!odin3_net_valid(net)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: decode: node %u: position %s|%u is on an unconnected pin",
                  dec->node.v, tag == ODIN3_PBIT_IN ? "IN" : "OUT", idx);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_const value = odin3_net_const_value(dec->module, net);
    *out = value != ODIN3_CONST_NONE ? k_const_bits[value] : net.v;
    return ODIN3_OK;
}

/* Every position, in order, so a sig's first is its span's first. */
static odin3_status dec_bits(decoder *dec) {
    odin3_vec *bits = &dec->pw->bits;
    if (odin3_vec_reserve(bits, dec->body.n_pbits) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < dec->body.n_pbits; i++) {
        odin3_pw_bit bit = 0;
        odin3_status st = dec_bit(dec, dec->body.pbits[i], &bit);
        if (st != ODIN3_OK) {
            return st;
        }
        *(odin3_pw_bit *)odin3_vec_push(bits) = bit; /* reserved */
    }
    return ODIN3_OK;
}

/* The nets of the I pins, then of the O pins, in pin order (every one connected: dec_bits). */
static odin3_status dec_in_nets(decoder *dec) {
    odin3_vec *nets = &dec->pw->in_nets;
    if (odin3_vec_reserve(nets, (size_t)dec->in.count + dec->outs.count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_pinslice ports[] = {dec->in, dec->outs};
    for (size_t port = 0; port < sizeof ports / sizeof ports[0]; port++) {
        for (uint32_t i = 0; i < ports[port].count; i++) {
            *(odin3_net_id *)odin3_vec_push(nets) =
                odin3_pin_net(dec->module, (odin3_pin_id){ports[port].first.v + i});
        }
    }
    return ODIN3_OK;
}

/* Appends n actions of the body, from first, to vec. */
static odin3_status dec_actions(const decoder *dec, odin3_pspan range, odin3_vec *vec) {
    if (odin3_vec_reserve(vec, range.len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = range.first; i < range.first + range.len; i++) {
        const odin3_proc_action *act = &dec->body.actions[i];
        odin3_pw_action *to = odin3_vec_push(vec);
        to->lhs = (odin3_pw_sig){act->lhs.first, act->lhs.len};
        to->rhs = (odin3_pw_sig){act->rhs.first, act->rhs.len};
        to->prov = act->prov;
    }
    return ODIN3_OK;
}

/* Appends the indices first .. first + count - 1 to vec. */
static odin3_status dec_indices(odin3_vec *vec, uint32_t first, uint32_t count) {
    if (odin3_vec_reserve(vec, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; i++) {
        *(uint32_t *)odin3_vec_push(vec) = first + i;
    }
    return ODIN3_OK;
}

/* Case idx: its compares, actions and switch indices. */
static odin3_status dec_case(decoder *dec, uint32_t idx) {
    const odin3_proc_case *from = &dec->body.cases[idx];
    odin3_pw_case *cs = odin3_vec_push(&dec->pw->cases); /* reserved */
    odin3_vec_init(&cs->compares, sizeof(odin3_pw_sig));
    odin3_vec_init(&cs->actions, sizeof(odin3_pw_action));
    odin3_vec_init(&cs->switches, sizeof(uint32_t));
    cs->prov = from->prov;
    if (odin3_vec_reserve(&cs->compares, from->n_compares) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t j = 0; j < from->n_compares; j++) {
        odin3_pspan cmp = dec->body.compares[from->first_compare + j];
        *(odin3_pw_sig *)odin3_vec_push(&cs->compares) = (odin3_pw_sig){cmp.first, cmp.len};
    }
    odin3_status st =
        dec_actions(dec, (odin3_pspan){from->first_action, from->n_actions}, &cs->actions);
    return st == ODIN3_OK ? dec_indices(&cs->switches, from->first_switch, from->n_switches) : st;
}

static odin3_status dec_switch(decoder *dec, uint32_t idx) {
    const odin3_proc_switch *from = &dec->body.switches[idx];
    odin3_pw_switch *sw = odin3_vec_push(&dec->pw->switches); /* reserved */
    odin3_vec_init(&sw->cases, sizeof(uint32_t));
    sw->signal = (odin3_pw_sig){from->signal.first, from->signal.len};
    sw->flags = from->flags;
    sw->prov = from->prov;
    return dec_indices(&sw->cases, from->first_case, from->n_cases);
}

static odin3_status dec_sync(decoder *dec, uint32_t idx) {
    const odin3_proc_sync *from = &dec->body.syncs[idx];
    odin3_pw_sync *sy = odin3_vec_push(&dec->pw->syncs); /* reserved */
    odin3_vec_init(&sy->actions, sizeof(odin3_pw_action));
    sy->type = from->type;
    odin3_status st = ODIN3_OK;
    if (from->type != ODIN3_SYNC_ALWAYS) {
        st = dec_bit(dec, from->signal, &sy->signal);
    }
    return st == ODIN3_OK
               ? dec_actions(dec, (odin3_pspan){from->first_action, from->n_actions}, &sy->actions)
               : st;
}

/* The cases, switches and syncs, each array reserved first so the records do not move. */
static odin3_status dec_tree(decoder *dec) {
    odin3_pw *pw = dec->pw;
    if (odin3_vec_reserve(&pw->cases, dec->body.n_cases) != ODIN3_OK ||
        odin3_vec_reserve(&pw->switches, dec->body.n_switches) != ODIN3_OK ||
        odin3_vec_reserve(&pw->syncs, dec->body.n_syncs) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < dec->body.n_cases; i++) {
        st = dec_case(dec, i);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < dec->body.n_switches; i++) {
        st = dec_switch(dec, i);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < dec->body.n_syncs; i++) {
        st = dec_sync(dec, i);
    }
    return st;
}

/* attr_foreach visitor: appends (key, value) to the working form's attrs. */
static odin3_status dec_attr(void *user, uint32_t key_str, const odin3_value *value) {
    odin3_pw_attr *attr = odin3_vec_push(user);
    if (attr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    attr->key = key_str;
    attr->value = value;
    return ODIN3_OK;
}

odin3_status odin3_pw_decode(const odin3_proc_mctx *mctx, odin3_node_id node, odin3_pw *out) {
    pw_init(out);
    decoder dec = {mctx->module, node, {0}, {{0}, 0}, {{0}, 0}, out};
    odin3_status st = dec_open(&dec);
    if (st == ODIN3_OK) {
        st = dec_bits(&dec);
    }
    if (st == ODIN3_OK) {
        st = dec_in_nets(&dec);
    }
    if (st == ODIN3_OK) {
        st = dec_tree(&dec);
    }
    if (st == ODIN3_OK) {
        st = odin3_attr_foreach(mctx->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, dec_attr,
                                &out->attrs);
    }
    if (st != ODIN3_OK) {
        odin3_pw_free(out);
        return st;
    }
    out->node = node;
    out->prov = odin3_node_prov(mctx->module, node);
    return ODIN3_OK;
}
