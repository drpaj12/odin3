/*
 * gv_stg.c — write_graph stg (spec section 3): one graph per FSM the fsm pass extracts. States are
 * nodes, transitions edges; the table is the fsm pass's own (detect, extract, opt; no recode).
 */
#include "ir/module.h"
#include "ir/proc.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/graphviews/gv.h"

#include <stdio.h>
#include <string.h>

enum { LABEL_MAX = 160, EXPR_MAX = 1024 };

typedef struct stg_ctx {
    gv_run *run;
    const odin3_fsm_mod *fm;
    const odin3_fsm_inst *inst;
    uint32_t wire;
    odin3_strbuf *out;
} stg_ctx;

/* The code or pattern of count bits, MSB first, in the characters of Yosys (0 1 x z - m). */
static void bits_msb(const uint8_t *bits, int32_t count, char *buf, size_t size) {
    size_t at = 0;
    for (int32_t i = count - 1; i >= 0 && at + 1 < size; i--) {
        buf[at++] = odin3_fsm_bit_char(bits[i]);
    }
    buf[at] = '\0';
}

static odin3_status put_sig_list(stg_ctx *cx, const char *key, const odin3_vec *sigs) {
    odin3_status st = odin3_strbuf_appendf(cx->out, "\"%s\": [", key);
    const odin3_fsm_sig *bits = sigs->data;
    bool first = true;
    for (size_t i = 0; st == ODIN3_OK && i < sigs->len; i++) {
        char label[LABEL_MAX];
        odin3_fsm_sig_label(cx->fm, bits[i], label, sizeof label);
        st = gv_sep(cx->out, &first);
        st = st == ODIN3_OK
                 ? odin3_strbuf_appendf(cx->out, "{\"index\": %zu, \"net\": %u, \"label\": ", i,
                                        odin3_fsm_sig_is_const(bits[i]) ? 0U : bits[i])
                 : st;
        st = st == ODIN3_OK ? gv_put_str(cx->out, label) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("}")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("], ")) : st;
}

static odin3_status put_select_nodes(stg_ctx *cx) {
    odin3_status st = odin3_strbuf_append(cx->out, odin3_bytes_cstr("\"select_nodes\": ["));
    const uint32_t *ids = cx->inst->mux_cells.data;
    for (size_t i = 0; st == ODIN3_OK && i < cx->inst->mux_cells.len; i++) {
        odin3_node_id node = {ids[i]};
        st = odin3_strbuf_appendf(cx->out, "%s{\"node\": %u, ", i == 0 ? "" : ", ", ids[i]);
        st = st == ODIN3_OK ? gv_put_prov(cx->run, cx->out, odin3_node_prov(cx->fm->module, node))
                            : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("}")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("], ")) : st;
}

static odin3_status put_graph_head(stg_ctx *cx) {
    const odin3_fsm_inst *inst = cx->inst;
    const odin3_module *mod = cx->fm->module;
    const odin3_fsm_data *data = &inst->data;
    char label[LABEL_MAX];
    odin3_fsm_wire_label(cx->fm, inst->wire, label, sizeof label);
    odin3_status st = odin3_strbuf_appendf(
        cx->out, "{\"id\": \"fsm:%u\", \"kind\": \"stg\", \"label\": ", cx->wire);
    st = st == ODIN3_OK ? gv_put_str(cx->out, label) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(
                   cx->out,
                   ", \"state_wire\": {\"wire\": %u, \"width\": %u}, \"state_node\": %u, "
                   "\"encoding\": ",
                   cx->wire, odin3_wire_width(mod, inst->wire), inst->dff_node)
             : st;
    st = st == ODIN3_OK ? gv_put_str(cx->out, inst->encoding) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(cx->out, ", \"reset_state\": %d, \"state_bits\": %d, ",
                                    data->reset_state, data->state_bits)
             : st;
    st = st == ODIN3_OK ? put_sig_list(cx, "inputs", &inst->ctrl_in) : st;
    st = st == ODIN3_OK ? put_sig_list(cx, "outputs", &inst->ctrl_out) : st;
    st = st == ODIN3_OK ? put_select_nodes(cx) : st;
    return st == ODIN3_OK ? gv_put_prov(cx->run, cx->out,
                                        odin3_node_prov(mod, (odin3_node_id){inst->dff_node}))
                          : st;
}

static odin3_status put_states(stg_ctx *cx) {
    const odin3_fsm_data *data = &cx->inst->data;
    odin3_status st = odin3_strbuf_append(cx->out, odin3_bytes_cstr(", \"nodes\": ["));
    for (size_t i = 0; st == ODIN3_OK && i < data->states.len; i++) {
        char code[LABEL_MAX];
        bits_msb(odin3_fsm_data_state(data, i), data->state_bits, code, sizeof code);
        st =
            odin3_strbuf_appendf(cx->out,
                                 "%s{\"id\": \"fsm:%u/s%zu\", \"kind\": \"state\", \"index\": %zu, "
                                 "\"code\": \"%s\", \"label\": \"%s\", \"reset\": %s, "
                                 "\"prov\": 0, \"src\": []}",
                                 i == 0 ? "" : ", ", cx->wire, i, i, code, code,
                                 (int32_t)i == data->reset_state ? "true" : "false");
    }
    return st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("]")) : st;
}

/* "a & !b" over the control inputs a transition tests; "1" when none. */
static void cond_expr(const stg_ctx *cx, const uint8_t *pat, char *buf, size_t size) {
    const odin3_fsm_sig *sigs = cx->inst->ctrl_in.data;
    size_t at = 0;
    buf[0] = '\0';
    for (int32_t i = 0; i < cx->inst->data.num_inputs; i++) {
        if (!odin3_fsm_bit_defined(pat[i])) {
            continue;
        }
        char label[LABEL_MAX];
        odin3_fsm_sig_label(cx->fm, sigs[i], label, sizeof label);
        int len = snprintf(buf + at, size - at, "%s%s%s", at == 0 ? "" : " & ",
                           pat[i] == ODIN3_FSM_BIT_0 ? "!" : "", label);
        if (len < 0 || (size_t)len >= size - at) {
            break;
        }
        at += (size_t)len;
    }
    if (at == 0) {
        (void)snprintf(buf, size, "1");
    }
}

static odin3_status put_transition(stg_ctx *cx, size_t idx) {
    const odin3_fsm_data *data = &cx->inst->data;
    const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, idx);
    char in_pat[LABEL_MAX];
    char out_pat[LABEL_MAX];
    char expr[EXPR_MAX];
    bits_msb(tr->ctrl_in, data->num_inputs, in_pat, sizeof in_pat);
    bits_msb(tr->ctrl_out, data->num_outputs, out_pat, sizeof out_pat);
    cond_expr(cx, tr->ctrl_in, expr, sizeof expr);
    odin3_status st = odin3_strbuf_appendf(
        cx->out,
        "%s{\"id\": \"e%zu\", \"from\": \"fsm:%u/s%d\", \"to\": \"fsm:%u/s%d\", \"kind\": "
        "\"transition\", \"trans\": %zu, \"cond\": \"%s\", \"out\": \"%s\", \"label\": ",
        idx == 0 ? "" : ", ", idx, cx->wire, tr->state_in, cx->wire, tr->state_out, idx, in_pat,
        out_pat);
    st = st == ODIN3_OK ? gv_put_str(cx->out, expr) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr(", \"cond_expr\": ")) : st;
    st = st == ODIN3_OK ? gv_put_str(cx->out, expr) : st;
    return st == ODIN3_OK
               ? odin3_strbuf_append(cx->out, odin3_bytes_cstr(", \"prov\": 0, \"src\": []}"))
               : st;
}

static odin3_status put_transitions(stg_ctx *cx) {
    odin3_status st = odin3_strbuf_append(cx->out, odin3_bytes_cstr(", \"edges\": ["));
    for (size_t i = 0; st == ODIN3_OK && i < cx->inst->data.trans.len; i++) {
        st = put_transition(cx, i);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(cx->out, odin3_bytes_cstr("]}")) : st;
}

/* The module ID of fm: the ID whose module is the context's. */
static uint32_t module_id_of(const gv_run *run, const odin3_fsm_mod *fm) {
    for (uint32_t id = 1; id < run->n_mods; id++) {
        if (odin3_module_get(run->design, (odin3_module_id){id}) == fm->module) {
            return id;
        }
    }
    return 0;
}

static odin3_status visit_fsm(void *user, const odin3_fsm_mod *fm, const odin3_fsm_inst *inst) {
    gv_run *run = user;
    uint32_t mid = module_id_of(run, fm);
    odin3_strbuf *out = &run->mods[mid];
    stg_ctx cx = {run, fm, inst, inst->wire.v, out};
    if (out->len > 0) {
        odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(",\n      "));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status st = put_graph_head(&cx);
    st = st == ODIN3_OK ? put_states(&cx) : st;
    return st == ODIN3_OK ? put_transitions(&cx) : st;
}

odin3_status gv_write_stg(gv_run *run) {
    odin3_status st = odin3_design_refuse_processes(run->design, "write_graph stg");
    return st != ODIN3_OK ? st : odin3_fsm_visit_design(run->design, visit_fsm, run);
}
