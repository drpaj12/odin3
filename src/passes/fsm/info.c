/*
 * info.c — fsm_info (spec I-1; Yosys 0.55 FsmData::log_info, fsmdata.h:133-170): the FSM tables
 * as log text. One message per line; every line starts "fsm: " so a harness can pick them out.
 */
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LINE_MAX_BITS = 512, LABEL_MAX = 160, TEXT_SLACK = 16 };

void odin3_fsm_sig_label(const odin3_fsm_mod *fm, odin3_fsm_sig sig, char *buf, size_t size) {
    if (odin3_fsm_sig_is_const(sig)) {
        (void)snprintf(buf, size, "1'%c", odin3_fsm_bit_char(odin3_fsm_sig_bit(sig)));
        return;
    }
    odin3_wirebit prim = odin3_net_primary(fm->module, (odin3_net_id){sig});
    if (prim.wire.v == 0) {
        (void)snprintf(buf, size, "$n%u", sig);
        return;
    }
    const char *name = odin3_fsm_str(fm, odin3_wire_name(fm->module, prim.wire));
    if (odin3_wire_width(fm->module, prim.wire) > 1) {
        (void)snprintf(buf, size, "%s [%u]", name, prim.bit);
    } else {
        (void)snprintf(buf, size, "%s", name);
    }
}

/* "N'bits" with the MSB first, like log_signal(Const). */
static void bits_text(const uint8_t *bits, int32_t count, char *buf, size_t size) {
    int32_t shown = count < LINE_MAX_BITS - TEXT_SLACK ? count : LINE_MAX_BITS - TEXT_SLACK;
    int len = snprintf(buf, size, "%d'", count);
    for (int32_t i = shown - 1; i >= 0 && len > 0 && (size_t)len + 1 < size; i--) {
        buf[len++] = odin3_fsm_bit_char(bits[i]);
    }
    buf[len > 0 ? len : 0] = '\0';
}

static void log_signals(const odin3_fsm_mod *fm, const odin3_vec *sigs, const char *title) {
    odin3_log(ODIN3_LOG_INFO, "fsm:   %s:", title);
    const odin3_fsm_sig *bits = sigs->data;
    for (size_t i = 0; i < sigs->len; i++) {
        char label[LABEL_MAX];
        odin3_fsm_sig_label(fm, bits[i], label, sizeof label);
        odin3_log(ODIN3_LOG_INFO, "fsm:   %3zu: %s", i, label);
    }
}

static void log_states(const odin3_fsm_data *data) {
    odin3_log(ODIN3_LOG_INFO, "fsm:   state encoding:");
    for (size_t i = 0; i < data->states.len; i++) {
        char text[LINE_MAX_BITS];
        bits_text(odin3_fsm_data_state(data, i), data->state_bits, text, sizeof text);
        odin3_log(ODIN3_LOG_INFO, "fsm:   %3zu: %10s%s", i, text,
                  (int32_t)i == data->reset_state ? "  <RESET STATE>" : "");
    }
}

static void log_transitions(const odin3_fsm_data *data) {
    odin3_log(ODIN3_LOG_INFO, "fsm:   transition table (state_in, ctrl_in, state_out, ctrl_out):");
    for (size_t i = 0; i < data->trans.len; i++) {
        const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        char in_text[LINE_MAX_BITS];
        char out_text[LINE_MAX_BITS];
        bits_text(tr->ctrl_in, data->num_inputs, in_text, sizeof in_text);
        bits_text(tr->ctrl_out, data->num_outputs, out_text, sizeof out_text);
        odin3_log(ODIN3_LOG_INFO, "fsm:   %5zu: %5d %s   -> %5d %s", i, tr->state_in, in_text,
                  tr->state_out, out_text);
    }
}

void odin3_fsm_info(const odin3_fsm_mod *fm, const odin3_fsm_inst *inst) {
    char label[LABEL_MAX];
    odin3_fsm_wire_label(fm, inst->wire, label, sizeof label);
    const odin3_fsm_data *data = &inst->data;
    odin3_log(ODIN3_LOG_INFO, "fsm: information on FSM '%s':", label);
    odin3_log(ODIN3_LOG_INFO, "fsm:   number of input signals:  %3d", data->num_inputs);
    odin3_log(ODIN3_LOG_INFO, "fsm:   number of output signals: %3d", data->num_outputs);
    odin3_log(ODIN3_LOG_INFO, "fsm:   number of state bits:     %3d", data->state_bits);
    log_signals(fm, &inst->ctrl_in, "input signals");
    log_signals(fm, &inst->ctrl_out, "output signals");
    log_states(data);
    log_transitions(data);
}
