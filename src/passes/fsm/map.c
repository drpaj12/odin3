/*
 * map.c — fsm_map (spec M-1 to M-6, P-1 to P-3; Yosys 0.55 passes/fsm/fsm_map.cc:157-321): one
 * extracted FSM becomes a state register, the state decoders, the next-state logic and the
 * control-output logic. All new nodes and nets are built first (a failure there deletes them
 * again); only then do the old drivers of the control outputs let go, the new cells take their
 * nets and the state wire changes. The old register, its mux tree and compare cells are left for
 * the next opt to remove, or to keep when something else reads them.
 */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/map_int.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LABEL_MAX = 160, NAME_MAX_TEXT = 256, BINARY_PARAMS = 5 };

/* --- building blocks ----------------------------------------------------------------------- */

odin3_status odin3_fsm_map_net(odin3_fsm_mapper *mp, odin3_net_id *out) {
    odin3_status st = odin3_net_create(mp->fm->module, 0, mp->prov, out);
    if (st == ODIN3_OK) {
        odin3_net_id net = *out;
        st = odin3_kit_push_u32(&mp->nets, net.v);
    }
    return st;
}

odin3_status odin3_fsm_map_sig_net(odin3_fsm_mapper *mp, odin3_fsm_sig sig, odin3_net_id *out) {
    if (!odin3_fsm_sig_is_const(sig)) {
        out->v = sig;
        return ODIN3_OK;
    }
    odin3_bit want = ODIN3_BIT_X;
    switch (odin3_fsm_sig_bit(sig)) {
    case ODIN3_FSM_BIT_0:
        want = ODIN3_BIT_0;
        break;
    case ODIN3_FSM_BIT_1:
        want = ODIN3_BIT_1;
        break;
    case ODIN3_FSM_BIT_Z:
        want = ODIN3_BIT_Z;
        break;
    default:
        break;
    }
    return odin3_opt_const_net(&mp->oc, want, out);
}

odin3_status odin3_fsm_map_cell(odin3_fsm_mapper *mp, const odin3_fsm_cell_req *req,
                                odin3_node_id *out) {
    odin3_kit_cell cell = {odin3_kit_type_named(mp->fm->design, req->type), req->params,
                           req->n_params, req->ports, mp->prov};
    odin3_status st = odin3_kit_new_cell(&mp->oc, &cell, out);
    if (st == ODIN3_OK) {
        st = odin3_kit_push_u32(&mp->nodes, out->v);
        mp->n_cells++;
    }
    return st;
}

/* Deletes what the build made (before any destructive step). */
static void undo_build(odin3_fsm_mapper *mp) {
    const uint32_t *nodes = mp->nodes.data;
    for (size_t i = mp->nodes.len; i-- > 0;) {
        (void)odin3_node_delete(mp->fm->module, (odin3_node_id){nodes[i]});
    }
    const uint32_t *nets = mp->nets.data;
    for (size_t i = 0; i < mp->nets.len; i++) {
        odin3_net_id net = {nets[i]};
        if (odin3_net_live(mp->fm->module, net) && odin3_net_pins(mp->fm->module, net).count == 0) {
            (void)odin3_net_delete(mp->fm->module, net);
        }
    }
}

/* --- the state register and decoders (M-1, M-2) ------------------------------------------------
 */

typedef struct reg_nets {
    odin3_vec q, d; /* odin3_net_id, one per state bit */
    bool one_hot;
} reg_nets;

/* The code of state i with a don't care read as x. */
static odin3_fsm_sig code_sig(const odin3_fsm_data *data, size_t state, int32_t bit) {
    uint8_t value = odin3_fsm_data_state(data, state)[bit];
    return odin3_fsm_sig_of_bit(odin3_fsm_bit_defined(value) || value == ODIN3_FSM_BIT_Z
                                    ? value
                                    : (uint8_t)ODIN3_FSM_BIT_X);
}

static odin3_status push_fresh(odin3_fsm_mapper *mp, odin3_vec *vec, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        odin3_net_id net = {0};
        odin3_status st = odin3_fsm_map_net(mp, &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(vec, net) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The next-state nets: constants for a single state, else fresh nets. */
static odin3_status make_d_nets(odin3_fsm_mapper *mp, reg_nets *rn) {
    const odin3_fsm_data *data = &mp->inst->data;
    if (mp->n_states != 1) {
        return push_fresh(mp, &rn->d, data->state_bits);
    }
    for (int32_t bit = 0; bit < data->state_bits; bit++) {
        odin3_net_id net = {0};
        odin3_status st = odin3_fsm_map_sig_net(mp, code_sig(data, 0, bit), &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(&rn->d, net) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The reset value: the reset state's code, every bit that is not 1 taken as 0 (:177-181). */
static void reset_value(const odin3_fsm_data *data, uint8_t *bits) {
    for (int32_t i = 0; i < data->state_bits; i++) {
        bool one = data->reset_state >= 0 &&
                   odin3_fsm_data_state(data, (size_t)data->reset_state)[i] == ODIN3_FSM_BIT_1;
        bits[i] = one ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
}

/* The $adff of a register with an asynchronous reset (:167-188). */
static odin3_status make_adff(odin3_fsm_mapper *mp, const reg_nets *rn, odin3_net_id clk,
                              odin3_node_id *node) {
    const odin3_fsm_inst *inst = mp->inst;
    const odin3_fsm_data *data = &inst->data;
    odin3_net_id arst = {0};
    uint8_t *rst_bits = odin3_util_malloc((size_t)data->state_bits + 1);
    odin3_status st =
        rst_bits != NULL ? odin3_fsm_map_sig_net(mp, inst->arst, &arst) : ODIN3_ERR_NO_MEMORY;
    if (st == ODIN3_OK) {
        reset_value(data, rst_bits);
        odin3_value params[4] = {odin3_value_int(data->state_bits),
                                 odin3_value_int(inst->clk_pol),
                                 odin3_value_int(inst->arst_pol),
                                 {ODIN3_VAL_BITS, 0, rst_bits, (uint32_t)data->state_bits, 0, 0}};
        odin3_netvec ports[4] = {{&clk, 1},
                                 {&arst, 1},
                                 {rn->d.data, (uint32_t)rn->d.len},
                                 {rn->q.data, (uint32_t)rn->q.len}};
        odin3_fsm_cell_req req = {"$adff", params, ODIN3_FSM_COUNT_OF(params), ports};
        st = odin3_fsm_map_cell(mp, &req, node);
    }
    odin3_util_free(rst_bits);
    return st;
}

/* The $dff, or $adff for a register with an asynchronous reset (:167-188). */
static odin3_status make_register(odin3_fsm_mapper *mp, const reg_nets *rn, odin3_node_id *node) {
    const odin3_fsm_inst *inst = mp->inst;
    odin3_net_id clk = {0};
    odin3_status st = odin3_fsm_map_sig_net(mp, inst->clk, &clk);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_fsm_sig_is_const(inst->arst)) {
        return make_adff(mp, rn, clk, node);
    }
    odin3_value params[2] = {odin3_value_int(inst->data.state_bits),
                             odin3_value_int(inst->clk_pol)};
    odin3_netvec ports[3] = {
        {&clk, 1}, {rn->d.data, (uint32_t)rn->d.len}, {rn->q.data, (uint32_t)rn->q.len}};
    odin3_fsm_cell_req req = {"$dff", params, ODIN3_FSM_COUNT_OF(params), ports};
    return odin3_fsm_map_cell(mp, &req, node);
}

/* State i is decoded by a Q bit when its code is a lone 1 among don't cares (:196-205). */
static bool lone_one(const odin3_fsm_data *data, size_t state, int32_t *bit) {
    int32_t defined = 0;
    for (int32_t i = 0; i < data->state_bits; i++) {
        uint8_t value = odin3_fsm_data_state(data, state)[i];
        if (odin3_fsm_bit_defined(value)) {
            defined++;
            *bit = i;
            if (value != ODIN3_FSM_BIT_1) {
                return false;
            }
        }
    }
    return defined == 1;
}

/* The $eq of the defined bits of state i against its code (:206-222). */
static odin3_status decode_eq(odin3_fsm_mapper *mp, const reg_nets *rn, size_t state,
                              odin3_net_id *out) {
    const odin3_fsm_data *data = &mp->inst->data;
    odin3_vec lhs;
    odin3_vec rhs;
    odin3_vec_init(&lhs, sizeof(odin3_net_id));
    odin3_vec_init(&rhs, sizeof(odin3_net_id));
    odin3_status st = ODIN3_OK;
    const odin3_net_id *qbits = rn->q.data;
    for (int32_t i = 0; st == ODIN3_OK && i < data->state_bits; i++) {
        uint8_t value = odin3_fsm_data_state(data, state)[i];
        if (!odin3_fsm_bit_defined(value)) {
            continue;
        }
        odin3_net_id net = {0};
        st = odin3_kit_push_net(&lhs, qbits[i]);
        st = st == ODIN3_OK ? odin3_fsm_map_sig_net(mp, odin3_fsm_sig_of_bit(value), &net) : st;
        st = st == ODIN3_OK ? odin3_kit_push_net(&rhs, net) : st;
    }
    st = st == ODIN3_OK ? odin3_fsm_map_net(mp, out) : st;
    if (st == ODIN3_OK) {
        odin3_value params[BINARY_PARAMS] = {odin3_value_int(0), odin3_value_int(0),
                                             odin3_value_int((int64_t)lhs.len),
                                             odin3_value_int((int64_t)rhs.len), odin3_value_int(1)};
        odin3_netvec ports[3] = {
            {lhs.data, (uint32_t)lhs.len}, {rhs.data, (uint32_t)rhs.len}, {out, 1}};
        odin3_fsm_cell_req req = {"$eq", params, ODIN3_FSM_COUNT_OF(params), ports};
        odin3_node_id node = {0};
        st = odin3_fsm_map_cell(mp, &req, &node);
    }
    odin3_vec_free(&lhs);
    odin3_vec_free(&rhs);
    return st;
}

/* Fills state_sig and rn->one_hot (:190-228). */
static odin3_status make_decoders(odin3_fsm_mapper *mp, reg_nets *rn) {
    const odin3_fsm_data *data = &mp->inst->data;
    const odin3_net_id *qbits = rn->q.data;
    rn->one_hot = true;
    for (size_t i = 0; i < mp->n_states; i++) {
        int32_t bit = 0;
        if (lone_one(data, i, &bit)) {
            mp->state_sig[i] = qbits[bit];
            continue;
        }
        rn->one_hot = false;
        odin3_status st = decode_eq(mp, rn, i, &mp->state_sig[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- next state (M-3) and control outputs (M-4) ---------------------------------------------- */

/* The bit of a one-hot code that holds the 1. */
static int32_t one_hot_bit(const odin3_fsm_data *data, size_t state) {
    int32_t bit = -1;
    for (int32_t i = 0; i < data->state_bits; i++) {
        if (odin3_fsm_data_state(data, state)[i] == ODIN3_FSM_BIT_1) {
            bit = i;
        }
    }
    return bit;
}

/* The three input vectors of the next-state $pmux. */
typedef struct pmux_ins {
    odin3_vec a, b, s;
} pmux_ins;

/* Appends the code of `state` (a don't care read as x) to dst as constant nets. */
static odin3_status push_code(odin3_fsm_mapper *mp, size_t state, odin3_vec *dst) {
    const odin3_fsm_data *data = &mp->inst->data;
    for (int32_t bit = 0; bit < data->state_bits; bit++) {
        odin3_net_id net = {0};
        odin3_status st = odin3_fsm_map_sig_net(mp, code_sig(data, state, bit), &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(dst, net) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A = the reset code (x without one), B = the other codes, S = their next-state nets. */
static odin3_status pmux_inputs(odin3_fsm_mapper *mp, const odin3_net_id *nso, pmux_ins *ins) {
    const odin3_fsm_data *data = &mp->inst->data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < mp->n_states; i++) {
        bool is_reset = (int32_t)i == data->reset_state;
        st = push_code(mp, i, is_reset ? &ins->a : &ins->b);
        st = st == ODIN3_OK && !is_reset ? odin3_kit_push_net(&ins->s, nso[i]) : st;
    }
    for (int32_t bit = 0; st == ODIN3_OK && ins->a.len == 0 && bit < data->state_bits; bit++) {
        odin3_net_id net = {0};
        st = odin3_fsm_map_sig_net(mp, odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_X), &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(&ins->a, net) : st;
    }
    return st;
}

/* The $pmux of the non-reset codes selected by the per-state next-state nets (:273-295). */
static odin3_status make_pmux(odin3_fsm_mapper *mp, const reg_nets *rn, const odin3_net_id *nso) {
    pmux_ins ins;
    odin3_vec_init(&ins.a, sizeof(odin3_net_id));
    odin3_vec_init(&ins.b, sizeof(odin3_net_id));
    odin3_vec_init(&ins.s, sizeof(odin3_net_id));
    odin3_status st = pmux_inputs(mp, nso, &ins);
    if (st == ODIN3_OK) {
        odin3_value params[2] = {odin3_value_int(mp->inst->data.state_bits),
                                 odin3_value_int((int64_t)ins.s.len)};
        odin3_netvec ports[4] = {{ins.a.data, (uint32_t)ins.a.len},
                                 {ins.b.data, (uint32_t)ins.b.len},
                                 {ins.s.data, (uint32_t)ins.s.len},
                                 {rn->d.data, (uint32_t)rn->d.len}};
        odin3_fsm_cell_req req = {"$pmux", params, ODIN3_FSM_COUNT_OF(params), ports};
        odin3_node_id node = {0};
        st = odin3_fsm_map_cell(mp, &req, &node);
    }
    odin3_vec_free(&ins.a);
    odin3_vec_free(&ins.b);
    odin3_vec_free(&ins.s);
    return st;
}

/* The per-state next-state logic, wired into the register (:230-296). */
static odin3_status make_next_state(odin3_fsm_mapper *mp, const reg_nets *rn) {
    const odin3_fsm_data *data = &mp->inst->data;
    if (mp->n_states == 1) {
        return ODIN3_OK; /* D is the constant code */
    }
    odin3_net_id *nso = odin3_util_calloc((mp->n_states + 1) * sizeof *nso);
    if (nso == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_net_id *dbits = rn->d.data;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < mp->n_states; i++) {
        if (rn->one_hot) {
            nso[i] = dbits[one_hot_bit(data, i)];
        } else {
            st = odin3_fsm_map_net(mp, &nso[i]);
        }
        odin3_fsm_target target = {ODIN3_FSM_TARGET_NEXT_STATE, i, nso[i], 0};
        st = st == ODIN3_OK ? odin3_fsm_map_target(mp, &target) : st;
    }
    st = st == ODIN3_OK && !rn->one_hot ? make_pmux(mp, rn, nso) : st;
    odin3_util_free(nso);
    return st;
}

/* One logic cone per control output that is a net (a constant output needs none). */
static odin3_status make_outputs(odin3_fsm_mapper *mp) {
    const odin3_fsm_inst *inst = mp->inst;
    const odin3_fsm_sig *out = inst->ctrl_out.data;
    for (uint32_t i = 0; i < inst->ctrl_out.len; i++) {
        if (odin3_fsm_sig_is_const(out[i])) {
            continue;
        }
        odin3_fsm_target target = {ODIN3_FSM_TARGET_CTRL_OUT, i, {0}, out[i]};
        odin3_status st = odin3_fsm_map_target(mp, &target);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the destructive step (M-4, X-5) ------------------------------------------------------------
 */

/* Lets go of the old drivers of net and connects the new output pin to it. */
static odin3_status take_over(odin3_fsm_mapper *mp, odin3_pin_id pin, odin3_net_id net) {
    odin3_module *module = mp->fm->module;
    uint32_t drivers = odin3_net_driver_count(module, net);
    odin3_pinlist pins = odin3_net_pins(module, net);
    uint32_t count = drivers < pins.count ? drivers : pins.count;
    odin3_pin_id *old = odin3_util_malloc(((size_t)count + 1) * sizeof *old);
    if (old == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(old, pins.pins, count * sizeof *old);
    for (uint32_t i = 0; i < count; i++) {
        (void)odin3_pin_disconnect(module, old[i]); /* never fails */
    }
    odin3_util_free(old);
    return odin3_pin_connect(module, pin, net);
}

/* Every new output cone takes its control-output net. */
static odin3_status take_over_all(odin3_fsm_mapper *mp) {
    const uint32_t *pending = mp->pending.data;
    for (size_t i = 0; i + 1 < mp->pending.len; i += 2) {
        odin3_status st = take_over(mp, (odin3_pin_id){pending[i]}, (odin3_net_id){pending[i + 1]});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the state wire and the attributes (M-1, P-3) ------------------------------------------------
 */

/* The text of a string ID of the design. */
static const char *text_of(const odin3_fsm_mapper *mp, uint32_t str) {
    return odin3_fsm_str(mp->fm, str);
}

static odin3_status set_int(odin3_fsm_mapper *mp, odin3_objref obj, const char *key, int64_t num) {
    uint32_t key_str = 0;
    odin3_status st = odin3_design_intern(mp->fm->design, odin3_bytes_cstr(key), &key_str);
    odin3_value val = odin3_value_int(num);
    return st == ODIN3_OK ? odin3_attr_set(mp->fm->module, obj, key_str, &val) : st;
}

static odin3_status set_str(odin3_fsm_mapper *mp, odin3_objref obj, const char *key,
                            const char *text) {
    uint32_t key_str = 0;
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0};
    odin3_status st = odin3_design_intern(mp->fm->design, odin3_bytes_cstr(key), &key_str);
    st =
        st == ODIN3_OK ? odin3_design_intern(mp->fm->design, odin3_bytes_cstr(text), &val.str) : st;
    return st == ODIN3_OK ? odin3_attr_set(mp->fm->module, obj, key_str, &val) : st;
}

/* The odin3_fsm_* attributes of P-3 on an object. */
static odin3_status set_fsm_attrs(odin3_fsm_mapper *mp, odin3_objref obj) {
    const odin3_fsm_inst *inst = mp->inst;
    char name[NAME_MAX_TEXT];
    (void)snprintf(name, sizeof name, "%s",
                   text_of(mp, odin3_wire_name(mp->fm->module, inst->wire)));
    odin3_status st = set_int(mp, obj, "odin3_fsm_states", (int64_t)mp->n_states);
    st = st == ODIN3_OK ? set_str(mp, obj, "odin3_fsm_encoding", inst->final_encoding) : st;
    st = st == ODIN3_OK ? set_int(mp, obj, "odin3_fsm_orig_bits", (int64_t)inst->dff_out.len) : st;
    return st == ODIN3_OK ? set_str(mp, obj, "odin3_fsm_orig_name", name) : st;
}

/* An attribute to carry over: key and a pointer into the module's attribute store. */
typedef struct carried {
    uint32_t key;
    const odin3_value *value;
} carried;

static odin3_status collect_attr(void *ctx, uint32_t key, const odin3_value *value) {
    odin3_vec *vec = ctx;
    carried *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->key = key;
    slot->value = value;
    return ODIN3_OK;
}

/* The wire's nets in bit order. */
static odin3_status wire_nets(const odin3_module *module, odin3_wire_id wire, odin3_vec *out) {
    for (uint32_t bit = 0; bit < odin3_wire_width(module, wire); bit++) {
        odin3_status st = odin3_kit_push_net(out, odin3_wire_net(module, wire, bit));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The old wire becomes $fsm$oldstate<name> (attributes but fsm_encoding kept), over its nets. */
static odin3_status rename_old_wire(odin3_fsm_mapper *mp, odin3_wire_id old, uint32_t old_name) {
    odin3_module *module = mp->fm->module;
    odin3_vec nets;
    odin3_vec attrs;
    odin3_vec_init(&nets, sizeof(odin3_net_id));
    odin3_vec_init(&attrs, sizeof(carried));
    odin3_wire_spec spec = {0, odin3_wire_msb(module, old), odin3_wire_lsb(module, old),
                            odin3_wire_signed(module, old), odin3_wire_prov(module, old)};
    char text[NAME_MAX_TEXT];
    (void)snprintf(text, sizeof text, "$fsm$oldstate\\%s", text_of(mp, old_name));
    odin3_status st = odin3_design_intern(mp->fm->design, odin3_bytes_cstr(text), &spec.name);
    st = st == ODIN3_OK ? wire_nets(module, old, &nets) : st;
    st = st == ODIN3_OK ? odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_WIRE, old.v},
                                             collect_attr, &attrs)
                        : st;
    st = st == ODIN3_OK ? odin3_wire_delete(module, old) : st;
    odin3_wire_id fresh = {0};
    st = st == ODIN3_OK ? odin3_wire_create(module, &spec, nets.data, &fresh) : st;
    const carried *list = attrs.data;
    for (size_t i = 0; st == ODIN3_OK && i < attrs.len; i++) {
        if (list[i].key != mp->fm->key_encoding) {
            st = odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_WIRE, fresh.v}, list[i].key,
                                list[i].value);
        }
    }
    odin3_vec_free(&nets);
    odin3_vec_free(&attrs);
    return st;
}

/* A name for the new wire when the old one stays (a port): name_1, name_2, ... */
static odin3_status free_name(odin3_fsm_mapper *mp, uint32_t base, uint32_t *out) {
    char text[NAME_MAX_TEXT];
    for (uint32_t k = 1; k < UINT16_MAX; k++) {
        (void)snprintf(text, sizeof text, "%s_%u", text_of(mp, base), k);
        odin3_status st = odin3_design_intern(mp->fm->design, odin3_bytes_cstr(text), out);
        if (st != ODIN3_OK || !odin3_wire_valid(odin3_module_find_wire(mp->fm->module, *out))) {
            return st;
        }
    }
    return ODIN3_ERR_INVALID_ARG;
}

/* The new state wire, named like the old one, over the new Q nets, with its attributes. */
static odin3_status make_state_wire(odin3_fsm_mapper *mp, const reg_nets *rn) {
    odin3_module *module = mp->fm->module;
    odin3_wire_id old = mp->inst->wire;
    uint32_t name = odin3_wire_name(module, old);
    bool is_port = odin3_wire_port_node(module, old).v != 0;
    odin3_status st = ODIN3_OK;
    if (name != 0 && is_port) {
        st = free_name(mp, name, &name);
    } else if (name != 0) {
        st = rename_old_wire(mp, old, name);
    }
    odin3_wire_spec spec = {name, (int32_t)rn->q.len - 1, 0, false, mp->prov};
    odin3_wire_id wire = {0};
    st = st == ODIN3_OK ? odin3_wire_create(module, &spec, rn->q.data, &wire) : st;
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    st = st == ODIN3_OK ? set_fsm_attrs(mp, obj) : st;
    return st == ODIN3_OK && rn->one_hot ? set_int(mp, obj, "onehot", 1) : st;
}

/* --- one FSM -------------------------------------------------------------------------------------
 */

/* The DERIVED record of P-1/P-2: the old register, the tree cells, the old output drivers. */
static odin3_status make_prov(odin3_fsm_mapper *mp) {
    const odin3_fsm_inst *inst = mp->inst;
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_node_id));
    odin3_status st = odin3_kit_push_u32(&parents, inst->dff_node);
    const uint32_t *lists[2] = {inst->mux_cells.data, inst->out_drivers.data};
    const size_t sizes[2] = {inst->mux_cells.len, inst->out_drivers.len};
    for (size_t which = 0; st == ODIN3_OK && which < 2; which++) {
        for (size_t i = 0; st == ODIN3_OK && i < sizes[which]; i++) {
            const uint32_t *have = parents.data;
            bool seen = false;
            for (size_t k = 0; k < parents.len && !seen; k++) {
                seen = have[k] == lists[which][i];
            }
            st = seen ? ODIN3_OK : odin3_kit_push_u32(&parents, lists[which][i]);
        }
    }
    odin3_kit_begin_rewrite(&mp->oc);
    st = st == ODIN3_OK ? odin3_kit_prov(&mp->oc, parents.data, (uint32_t)parents.len,
                                         &odin3_kit_class_coarsen, &mp->prov)
                        : st;
    odin3_vec_free(&parents);
    return st;
}

/* Builds every new object; on failure the build is undone by the caller. */
static odin3_status build(odin3_fsm_mapper *mp, reg_nets *rn, odin3_node_id *dff) {
    const odin3_fsm_data *data = &mp->inst->data;
    odin3_status st = push_fresh(mp, &rn->q, data->state_bits);
    st = st == ODIN3_OK ? make_d_nets(mp, rn) : st;
    st = st == ODIN3_OK ? make_register(mp, rn, dff) : st;
    st = st == ODIN3_OK ? make_decoders(mp, rn) : st;
    st = st == ODIN3_OK ? make_next_state(mp, rn) : st;
    return st == ODIN3_OK ? make_outputs(mp) : st;
}

static void mapper_free(odin3_fsm_mapper *mp, reg_nets *rn) {
    odin3_vec_free(&mp->nodes);
    odin3_vec_free(&mp->nets);
    odin3_vec_free(&mp->pending);
    odin3_vec_free(&rn->q);
    odin3_vec_free(&rn->d);
    odin3_util_free(mp->state_sig);
    odin3_opt_ctx_free(&mp->oc);
}

odin3_status odin3_fsm_map(const odin3_fsm_mod *fm, odin3_pass_ctx *pass,
                           const odin3_fsm_inst *inst) {
    odin3_fsm_mapper mp;
    reg_nets rn;
    memset(&mp, 0, sizeof mp);
    memset(&rn, 0, sizeof rn);
    odin3_vec_init(&rn.q, sizeof(odin3_net_id));
    odin3_vec_init(&rn.d, sizeof(odin3_net_id));
    odin3_vec_init(&mp.nodes, sizeof(uint32_t));
    odin3_vec_init(&mp.nets, sizeof(uint32_t));
    odin3_vec_init(&mp.pending, sizeof(uint32_t));
    mp.fm = fm;
    mp.inst = inst;
    mp.n_states = (uint32_t)inst->data.states.len;
    mp.state_sig = odin3_util_calloc(((size_t)mp.n_states + 1) * sizeof *mp.state_sig);
    odin3_status st = mp.state_sig != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? odin3_opt_ctx_init(&mp.oc, pass, fm->module, &mp.opts) : st;
    if (st != ODIN3_OK) {
        mapper_free(&mp, &rn);
        return st;
    }
    odin3_node_id dff = {0};
    st = make_prov(&mp);
    st = st == ODIN3_OK ? build(&mp, &rn, &dff) : st;
    if (st != ODIN3_OK) {
        undo_build(&mp);
        mapper_free(&mp, &rn);
        return st;
    }
    st = take_over_all(&mp);
    st = st == ODIN3_OK ? make_state_wire(&mp, &rn) : st;
    st = st == ODIN3_OK ? set_fsm_attrs(&mp, (odin3_objref){ODIN3_OBJ_NODE, dff.v}) : st;
    st = odin3_kit_half_done(&mp.oc, st);
    if (st == ODIN3_OK) {
        char label[LABEL_MAX];
        odin3_fsm_wire_label(fm, inst->wire, label, sizeof label);
        odin3_log(ODIN3_LOG_INFO, "fsm: mapped '%s' to %u cells", label, mp.n_cells);
    }
    mapper_free(&mp, &rn);
    return st;
}
