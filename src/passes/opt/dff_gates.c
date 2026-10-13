/*
 * dff_gates.c — the helper cells of opt_dff's rules (2F spec OP-9 §9.3): word gates built on fresh
 * nets with the FF's provenance record.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/dff.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MAX_GATE_PORTS = 4, MAX_GATE_PARAMS = 8 };

/* --- gates (the rules' helper cells) ---------------------------------------------------------- */

/* A DERIVED record of the FF's node for the helper cells (one per visit). */
static odin3_status state_prov(odin3_dff_state *ds, odin3_prov_id *out) {
    odin3_status st = ODIN3_OK;
    if (!ds->prov_ready) {
        st = odin3_kit_prov(ds->oc, &ds->cell, 1, &ds->prov);
        ds->prov_ready = st == ODIN3_OK;
    }
    *out = ds->prov;
    return st;
}

/* The widths of a word gate: its inputs' (A_WIDTH, B_WIDTH, WIDTH) and its output's (Y_WIDTH). */
typedef struct gate_shape {
    uint32_t in, out;
} gate_shape;

/* The value of parameter `name` of a word gate (the *_SIGNED flags 0). */
static odin3_value gate_param(const char *name, gate_shape shape) {
    if (strcmp(name, "Y_WIDTH") == 0) {
        return odin3_value_int((int64_t)shape.out);
    }
    return odin3_value_int(strstr(name, "WIDTH") != NULL ? (int64_t)shape.in : 0);
}

/*
 * A word gate type_name ($not, $and, $or, $mux, $ne, $reduce_*) over ins (inputs in port order),
 * shape.in bits wide (a $mux's S is one bit), on fresh output nets, which are appended to outs
 * (cleared first).
 */
static odin3_status new_gate(odin3_dff_state *ds, const char *type_name, const odin3_netvec *ins,
                             gate_shape shape, odin3_vec *outs) {
    odin3_celltype_id type = odin3_kit_type_named(ds->oc->design, type_name);
    const odin3_celltype_def *def = odin3_celltype_get(ds->oc->design, type);
    if (def == NULL || def->n_ports > MAX_GATE_PORTS || def->n_params > MAX_GATE_PARAMS) {
        odin3_log(ODIN3_LOG_ERROR, "opt_dff: cell type '%s' is not registered", type_name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_value params[MAX_GATE_PARAMS];
    odin3_netvec ports[MAX_GATE_PORTS];
    for (uint32_t k = 0; k < def->n_params; k++) {
        params[k] = gate_param(def->params[k].name, shape);
    }
    uint32_t input = 0;
    for (uint32_t k = 0; k < def->n_ports; k++) {
        ports[k] = def->ports[k].dir == ODIN3_DIR_IN ? ins[input++] : (odin3_netvec){NULL, 0};
    }
    odin3_kit_cell cell = {type, params, def->n_params, ports, {0}};
    odin3_status st = state_prov(ds, &cell.prov);
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_kit_new_cell(ds->oc, &cell, &node) : st;
    odin3_vec_clear(outs);
    odin3_pinslice out = {0};
    out = st == ODIN3_OK ? odin3_node_port(ds->oc->module, node, def->n_ports - 1U) : out;
    for (uint32_t k = 0; st == ODIN3_OK && k < out.count; k++) {
        st = odin3_kit_push_net(outs,
                                odin3_pin_net(ds->oc->module, (odin3_pin_id){out.first.v + k}));
    }
    return st;
}

static odin3_netvec as_netvec(const odin3_vec *vec) {
    return (odin3_netvec){vec->data, (uint32_t)vec->len};
}

/* dst = ~src (a word $not over the vector). */
odin3_status odin3_dff_gate_not(odin3_dff_state *ds, const odin3_vec *src, odin3_vec *dst) {
    const odin3_netvec ins[1] = {as_netvec(src)};
    return new_gate(ds, "$not", ins, (gate_shape){(uint32_t)src->len, (uint32_t)src->len}, dst);
}

/* dst = lhs op rhs over two vectors of one width ("$and" or "$or"). */
odin3_status odin3_dff_gate_bin(odin3_dff_state *ds, const char *op, const odin3_vec *lhs,
                                const odin3_vec *rhs, odin3_vec *dst) {
    const odin3_netvec ins[2] = {as_netvec(lhs), as_netvec(rhs)};
    return new_gate(ds, op, ins, (gate_shape){(uint32_t)lhs->len, (uint32_t)lhs->len}, dst);
}

/* dst = sel ? rhs : lhs. */
odin3_status odin3_dff_gate_mux(odin3_dff_state *ds, const odin3_vec *lhs, const odin3_vec *rhs,
                                odin3_net_id sel, odin3_vec *dst) {
    const odin3_netvec ins[3] = {as_netvec(lhs), as_netvec(rhs), {&sel, 1}};
    return new_gate(ds, "$mux", ins, (gate_shape){(uint32_t)lhs->len, (uint32_t)lhs->len}, dst);
}

/* ~net for one net (the one-bit $not of Yosys's Not on a SigBit). */
odin3_status odin3_dff_not_net(odin3_dff_state *ds, odin3_net_id net, odin3_net_id *out) {
    odin3_vec one;
    odin3_vec res;
    odin3_vec_init(&one, sizeof(odin3_net_id));
    odin3_vec_init(&res, sizeof(odin3_net_id));
    odin3_status st = odin3_kit_push_net(&one, net);
    st = st == ODIN3_OK ? odin3_dff_gate_not(ds, &one, &res) : st;
    *out = st == ODIN3_OK ? odin3_ff_net_at(&res, 0) : (odin3_net_id){0};
    odin3_vec_free(&one);
    odin3_vec_free(&res);
    return st;
}

/* lhs & rhs for two single nets. */
odin3_status odin3_dff_and_nets(odin3_dff_state *ds, odin3_net_id lhs, odin3_net_id rhs,
                                odin3_net_id *out) {
    odin3_vec va;
    odin3_vec vb;
    odin3_vec res;
    odin3_vec_init(&va, sizeof(odin3_net_id));
    odin3_vec_init(&vb, sizeof(odin3_net_id));
    odin3_vec_init(&res, sizeof(odin3_net_id));
    odin3_status st = odin3_kit_push_net(&va, lhs);
    st = st == ODIN3_OK ? odin3_kit_push_net(&vb, rhs) : st;
    st = st == ODIN3_OK ? odin3_dff_gate_bin(ds, "$and", &va, &vb, &res) : st;
    *out = st == ODIN3_OK ? odin3_ff_net_at(&res, 0) : (odin3_net_id){0};
    odin3_vec_free(&va);
    odin3_vec_free(&vb);
    odin3_vec_free(&res);
    return st;
}

/* A one-bit result of a gate over nets: *out = its output net. */
static odin3_status one_bit(odin3_dff_state *ds, const char *type, const odin3_netvec *ins,
                            uint32_t width, odin3_net_id *out) {
    odin3_vec res;
    odin3_vec_init(&res, sizeof(odin3_net_id));
    odin3_status st = new_gate(ds, type, ins, (gate_shape){width, 1}, &res);
    *out = st == ODIN3_OK ? odin3_ff_net_at(&res, 0) : (odin3_net_id){0};
    odin3_vec_free(&res);
    return st;
}

/* *out = the one-bit $reduce_or / $reduce_and (op) of the nets of vec. */
odin3_status odin3_dff_gate_reduce(odin3_dff_state *ds, const char *op, const odin3_vec *vec,
                                   odin3_net_id *out) {
    const odin3_netvec ins[1] = {as_netvec(vec)};
    return one_bit(ds, op, ins, (uint32_t)vec->len, out);
}

/* *out = the one-bit $ne of two equally wide vectors. */
odin3_status odin3_dff_gate_ne(odin3_dff_state *ds, const odin3_vec *lhs, const odin3_vec *rhs,
                               odin3_net_id *out) {
    const odin3_netvec ins[2] = {as_netvec(lhs), as_netvec(rhs)};
    return one_bit(ds, "$ne", ins, (uint32_t)lhs->len, out);
}
