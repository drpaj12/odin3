/*
 * mul.c — `multiply` to `$mul` (M-1 to M-3).
 */
#include "passes/unmap/mul.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"
#include "passes/unmap/operand.h"

#include <stdbool.h>
#include <stdint.h>

enum { UM_MUL_PARAMS = 5 };

/* The nets of bits 0 .. span.index - 1 of port span.port (span.index is a count here). */
static odin3_status load_bits(const um_ctx *uc, odin3_node_id node, um_pos span, odin3_vec *out) {
    odin3_vec_init(out, sizeof(odin3_net_id));
    odin3_status st = odin3_vec_reserve(out, span.index);
    for (uint32_t i = 0; st == ODIN3_OK && i < span.index; i++) {
        odin3_net_id *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = um_port_net(uc->module, node, (um_pos){span.port, i});
    }
    return st;
}

/* M-1: 1 + the highest index of port `port` whose class is not pad or open; 1 when none. */
static uint32_t used_width(const um_ctx *uc, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(uc->module, node, port);
    for (uint32_t i = pins.count; i > 0; i--) {
        um_cls cls = um_net_class(uc->module, um_port_net(uc->module, node, (um_pos){port, i - 1}));
        if (cls != UM_PAD && cls != UM_NONE) {
            return i;
        }
    }
    return 1;
}

/* M-1: 1 + the highest `out` index somebody reads; operands' widths added when none is read. */
static uint32_t result_width(const um_ctx *uc, odin3_node_id node, uint32_t operands) {
    odin3_pinslice pins = odin3_node_port(uc->module, node, UM_MUL_OUT);
    for (uint32_t i = pins.count; i > 0; i--) {
        if (um_observed(uc->module, um_port_net(uc->module, node, (um_pos){UM_MUL_OUT, i - 1}))) {
            return i;
        }
    }
    return operands;
}

static odin3_status convert_mul(um_ctx *uc, odin3_node_id node, int64_t cell_width) {
    uint32_t ua = used_width(uc, node, UM_MUL_A);
    uint32_t ub = used_width(uc, node, UM_MUL_B);
    uint32_t yw = result_width(uc, node, ua + ub);
    odin3_vec bits_a;
    odin3_vec bits_b;
    odin3_vec bits_y;
    odin3_status st = load_bits(uc, node, (um_pos){UM_MUL_A, ua}, &bits_a);
    st = st == ODIN3_OK ? load_bits(uc, node, (um_pos){UM_MUL_B, ub}, &bits_b) : st;
    st = st == ODIN3_OK ? load_bits(uc, node, (um_pos){UM_MUL_OUT, yw}, &bits_y) : st;
    if (st == ODIN3_OK) {
        /* M-2: signed when the operands are equally wide, no wider than the result shows, and at
         * least one has a repeated top net */
        um_vec va = {bits_a.data, ua};
        um_vec vb = {bits_b.data, ub};
        uint32_t wa = um_top_run(uc->module, &va);
        uint32_t wb = um_top_run(uc->module, &vb);
        bool is_signed = ua == ub && yw <= ua && (wa < ua || wb < ub);
        va.len = is_signed ? wa : ua;
        vb.len = is_signed ? wb : ub;
        odin3_value params[UM_MUL_PARAMS] = {odin3_value_int(is_signed), odin3_value_int(is_signed),
                                             odin3_value_int(va.len), odin3_value_int(vb.len),
                                             odin3_value_int(yw)};
        odin3_netvec ports[3] = {{va.nets, va.len}, {vb.nets, vb.len}, {bits_y.data, yw}};
        um_group group;
        um_group_init(&group);
        st = um_group_add(&group, node);
        um_new req = {uc->types.mul, params, UM_MUL_PARAMS, ports, node, "mul", 0, cell_width};
        odin3_node_id out = {0};
        st = st == ODIN3_OK ? um_replace(uc, &group, &req, &out) : st;
        um_group_free(&group);
    }
    odin3_vec_free(&bits_a);
    odin3_vec_free(&bits_b);
    odin3_vec_free(&bits_y);
    return st;
}

odin3_status um_mul_phase(um_ctx *uc) {
    uint32_t end = odin3_module_node_end(uc->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(uc->module, node) ||
            odin3_node_type(uc->module, node).v != uc->types.multiply.v) {
            continue;
        }
        const odin3_value *width = odin3_node_param(uc->module, node, 0);
        uc->stats.multiply++;
        if (width == NULL || width->kind != ODIN3_VAL_INT || width->i < 1) {
            st = um_decline(uc, &node, 1, (um_why){"mul", "width"});
            continue;
        }
        st = convert_mul(uc, node, width->i);
        uc->stats.mul += st == ODIN3_OK ? 1 : 0;
    }
    return st;
}
