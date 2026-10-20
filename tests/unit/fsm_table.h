/*
 * fsm_table.h — hand-made FSM tables for the unit tests of the fsm pass (4.8): an instance with
 * given state codes and control nets, transitions written as text, and the table dumped as text
 * to compare. A state is its code written bit 0 first; a transition is "in>out ctrl_in|ctrl_out"
 * with the bits 0 1 x - in the same order. Test code.
 */
#ifndef ODIN3_TESTS_UNIT_FSM_TABLE_H
#define ODIN3_TESTS_UNIT_FSM_TABLE_H

#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/opt/kit.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TABLE_TEXT = 4096, FIRST_NET = 10 };

/* --- hand-made tables --------------------------------------------------------------------------
 */

static inline uint8_t bit_of(char ch) {
    static const char k_chars[] = "01xz-m";
    return (uint8_t)(strchr(k_chars, ch) - k_chars);
}

/* The shape of a table: its sizes and reset state. */
typedef struct shape {
    int32_t state_bits, n_in, n_out, reset;
} shape;

/* An instance with the states of `states` (n of them) and control nets FIRST_NET.. */
static inline void make(odin3_fsm_inst *inst, shape sh, const char *const *states, uint32_t count) {
    memset(inst, 0, sizeof *inst);
    odin3_vec_init(&inst->ctrl_in, sizeof(odin3_fsm_sig));
    odin3_vec_init(&inst->ctrl_out, sizeof(odin3_fsm_sig));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_init(&inst->data));
    inst->data.state_bits = sh.state_bits;
    inst->data.num_inputs = sh.n_in;
    inst->data.num_outputs = sh.n_out;
    inst->data.reset_state = sh.reset;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t code[16];
        for (int32_t k = 0; k < sh.state_bits; k++) {
            code[k] = bit_of(states[i][k]);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_add_state(&inst->data, code));
    }
    for (int32_t i = 0; i < sh.n_in; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_kit_push_u32(&inst->ctrl_in, FIRST_NET + (uint32_t)i));
    }
    for (int32_t i = 0; i < sh.n_out; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_kit_push_u32(&inst->ctrl_out, FIRST_NET + 100U + (uint32_t)i));
    }
}

/* One transition of the table. */
typedef struct tr_text {
    int32_t state_in, state_out;
    const char *ctrl_in, *ctrl_out;
} tr_text;

static inline void add(odin3_fsm_inst *inst, tr_text tt) {
    odin3_fsm_data *data = &inst->data;
    odin3_fsm_trans tr = {tt.state_in, tt.state_out,
                          odin3_fsm_data_bits(data, (size_t)data->num_inputs),
                          odin3_fsm_data_bits(data, (size_t)data->num_outputs)};
    for (int32_t i = 0; i < data->num_inputs; i++) {
        tr.ctrl_in[i] = bit_of(tt.ctrl_in[i]);
    }
    for (int32_t i = 0; i < data->num_outputs; i++) {
        tr.ctrl_out[i] = bit_of(tt.ctrl_out[i]);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_add_trans(data, &tr));
}

/* The table as text: "reset=R states=a,b trans: 0>1 01|10; ...". */
static inline const char *dump(const odin3_fsm_inst *inst) {
    static char buf[TABLE_TEXT];
    const odin3_fsm_data *data = &inst->data;
    size_t len =
        (size_t)snprintf(buf, sizeof buf, "reset=%d in=%d out=%d states=", data->reset_state,
                         data->num_inputs, data->num_outputs);
    for (size_t i = 0; i < data->states.len; i++) {
        for (int32_t k = 0; k < data->state_bits; k++) {
            buf[len++] = odin3_fsm_bit_char(odin3_fsm_data_state(data, i)[k]);
        }
        buf[len++] = i + 1 < data->states.len ? ',' : ' ';
    }
    for (size_t i = 0; i < data->trans.len; i++) {
        const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%d>%d ", tr->state_in, tr->state_out);
        for (int32_t k = 0; k < data->num_inputs; k++) {
            buf[len++] = odin3_fsm_bit_char(tr->ctrl_in[k]);
        }
        buf[len++] = '|';
        for (int32_t k = 0; k < data->num_outputs; k++) {
            buf[len++] = odin3_fsm_bit_char(tr->ctrl_out[k]);
        }
        buf[len++] = ';';
    }
    buf[len] = '\0';
    return buf;
}

static inline void done(odin3_fsm_inst *inst) {
    odin3_vec_free(&inst->ctrl_in);
    odin3_vec_free(&inst->ctrl_out);
    odin3_fsm_data_free(&inst->data);
}

#endif
