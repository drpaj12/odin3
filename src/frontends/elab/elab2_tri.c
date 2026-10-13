/*
 * elab2_tri.c — the `$tribuf` of D3 (2D Task 19): tri-state drivers of a top-level output or
 * inout port.
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

/* --- tri-state (D3) -------------------------------------------------------------------------- */

/* The unary cell op over a one-bit or wider signal, giving a one-bit net in *out. */
static odin3_status tri_unary(odin3_elab2 *e2, odin3_elab2_type type, odin3_ast_id node,
                              const odin3_sig *operand, odin3_net_id *out) {
    odin3_elab2_cell_req req = {.type = type,
                                .node = node,
                                .a = operand,
                                .y_width = type == ODIN3_E2T_NOT ? operand->width : 1};
    odin3_sig result;
    odin3_status st = odin3_elab2_cell(e2, &req, &result);
    return st != ODIN3_OK ? st : odin3_elab2_to_nets(e2, &result, out);
}

/* EN from the condition: a bit as is or through the cell the width and the inversion ask for. */
static odin3_status tri_enable(odin3_elab2 *e2, const odin3_elab2_tri_req *req,
                               const odin3_sig *cond, odin3_net_id *out) {
    if (cond->width == 1 && !req->invert) {
        return odin3_elab2_to_nets(e2, cond, out);
    }
    odin3_elab2_type type = req->invert ? ODIN3_E2T_LOGIC_NOT : ODIN3_E2T_REDUCE_BOOL;
    if (cond->width == 1) {
        type = ODIN3_E2T_NOT; /* only reached inverted */
    }
    return tri_unary(e2, type, req->node, cond, out);
}

/* The data and condition signals: generated from the nodes, or the ones given. */
static odin3_status tri_operands(odin3_elab2 *e2, const odin3_elab2_tri_req *req, odin3_sig *data,
                                 odin3_sig *cond) {
    uint32_t width = (uint32_t)req->lv->nets.len;
    odin3_status st = ODIN3_OK;
    if (req->data_node.v != 0) {
        odin3_elab2_expr_req dreq = {req->data_node, {width, true, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &dreq, data);
    } else {
        *data = *req->data;
    }
    st = st != ODIN3_OK ? st : odin3_elab2_extend(e2, data, width, data->is_signed, data);
    if (st == ODIN3_OK && req->cond_node.v != 0) {
        odin3_elab2_expr_req creq = {req->cond_node, {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &creq, cond);
    } else if (st == ODIN3_OK) {
        *cond = *req->cond;
    }
    return st;
}

/* The tristate cell: A (data), EN, Y (the lvalue's nets, fresh ones for dropped bits). */
static odin3_status tri_cell(odin3_elab2 *e2, const odin3_elab2_tri_req *req, const odin3_sig *data,
                             odin3_net_id enable) {
    uint32_t width = data->width;
    uint64_t pins = 2ULL * width + 1U;
    odin3_loc at = odin3_ast_loc(e2->elab, req->node);
    odin3_status cap = odin3_elab2_pins_cap(e2, at, pins);
    if (cap != ODIN3_OK) {
        return cap;
    }
    odin3_net_id *data_nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *data_nets);
    odin3_net_id *out_nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *out_nets);
    if (data_nets == NULL || out_nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_elab2_to_nets(e2, data, data_nets);
    uint32_t fresh = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        out_nets[i] = ((const odin3_net_id *)req->lv->nets.data)[i];
        if (out_nets[i].v == 0) {
            st = odin3_elab2_reserve_nets(e2, 1, at);
            st = st != ODIN3_OK
                     ? st
                     : odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &out_nets[i]);
            fresh++;
        }
    }
    odin3_elab2_prov_req preq = {req->node, {0}, e2->scope.v};
    odin3_prov_id prov = {0};
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &preq, &prov);
    odin3_value param = odin3_value_int((int64_t)width);
    odin3_node_spec spec = {
        .type = e2->types[ODIN3_E2T_TRIBUF], .prov = prov, .params = &param, .n_params = 1};
    odin3_netvec ports[3] = {{data_nets, width}, {&enable, 1}, {out_nets, width}};
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, ports, &node);
    st = st != ODIN3_OK ? st
                        : odin3_elab2_attrs(e2, req->node, (odin3_objref){ODIN3_OBJ_NODE, node.v});
    odin3_elab2_counts made = {1, (uint32_t)pins, fresh, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* The data inverted through a $not (notif), in place. */
static odin3_status tri_invert_data(odin3_elab2 *e2, const odin3_elab2_tri_req *req,
                                    odin3_sig *data) {
    odin3_elab2_cell_req nreq = {
        .type = ODIN3_E2T_NOT, .node = req->node, .a = data, .y_width = data->width};
    return odin3_elab2_cell(e2, &nreq, data);
}

odin3_status odin3_elab2_tribuf(odin3_elab2 *e2, const odin3_elab2_tri_req *req) {
    if (req == NULL || req->lv == NULL || e2->mod == NULL ||
        (req->data_node.v == 0 && req->data == NULL) ||
        (req->cond_node.v == 0 && req->cond == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_tribuf: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool granted = false;
    odin3_elab2_lvclaim claim = {req->lv, ODIN3_DRV_TRIBUF, req->claim, true, NULL, NULL};
    odin3_status st = odin3_elab2_lvalue_claim(e2, &claim, &granted);
    if (st != ODIN3_OK || !granted) {
        return st != ODIN3_OK ? st : ODIN3_ERR_PARSE;
    }
    odin3_sig data;
    odin3_sig cond;
    odin3_net_id enable = {0};
    st = tri_operands(e2, req, &data, &cond);
    st = st == ODIN3_OK && req->invert_data ? tri_invert_data(e2, req, &data) : st;
    st = st != ODIN3_OK ? st : tri_enable(e2, req, &cond, &enable);
    return st != ODIN3_OK ? st : tri_cell(e2, req, &data, enable);
}
