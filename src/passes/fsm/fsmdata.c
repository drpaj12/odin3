/*
 * fsmdata.c — the FSM table (fsmdata.h): the Const comparator, the arena-backed tables.
 */
#include "passes/fsm/fsmdata.h"

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { ARENA_CHUNK = 16384 };

char odin3_fsm_bit_char(uint8_t bit) {
    static const char k_chars[] = {'0', '1', 'x', 'z', '-', 'm'};
    char out = '?';
    if (bit <= ODIN3_FSM_BIT_MARK) {
        out = k_chars[bit];
    }
    return out;
}

int odin3_fsm_const_cmp(const uint8_t *lhs, uint32_t lhs_len, const uint8_t *rhs,
                        uint32_t rhs_len) {
    if (lhs_len != rhs_len) {
        return lhs_len < rhs_len ? -1 : 1;
    }
    for (uint32_t i = 0; i < lhs_len; i++) {
        if (lhs[i] != rhs[i]) {
            return lhs[i] < rhs[i] ? -1 : 1;
        }
    }
    return 0;
}

odin3_status odin3_fsm_data_init(odin3_fsm_data *data) {
    memset(data, 0, sizeof *data);
    data->reset_state = -1;
    data->arena = odin3_arena_create(ARENA_CHUNK);
    if (data->arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&data->states, sizeof(uint8_t *));
    odin3_vec_init(&data->trans, sizeof(odin3_fsm_trans));
    return ODIN3_OK;
}

void odin3_fsm_data_free(odin3_fsm_data *data) {
    if (data == NULL) {
        return;
    }
    odin3_vec_free(&data->states);
    odin3_vec_free(&data->trans);
    odin3_arena_destroy(data->arena);
    data->arena = NULL;
}

uint8_t *odin3_fsm_data_bits(odin3_fsm_data *data, size_t n) {
    return odin3_arena_alloc(data->arena, n);
}

odin3_status odin3_fsm_data_add_state(odin3_fsm_data *data, const uint8_t *code) {
    uint8_t *copy = odin3_fsm_data_bits(data, (size_t)data->state_bits);
    uint8_t **slot = copy != NULL ? (uint8_t **)odin3_vec_push(&data->states) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, code, (size_t)data->state_bits);
    *slot = copy;
    return ODIN3_OK;
}

odin3_status odin3_fsm_data_add_trans(odin3_fsm_data *data, const odin3_fsm_trans *trans) {
    odin3_fsm_trans *slot = odin3_vec_push(&data->trans);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *trans;
    return ODIN3_OK;
}

/* Copies one transition's arrays into dst's arena and appends it. */
static odin3_status copy_trans(odin3_fsm_data *dst, const odin3_fsm_trans *src) {
    odin3_fsm_trans tr = *src;
    tr.ctrl_in = odin3_fsm_data_bits(dst, (size_t)dst->num_inputs);
    tr.ctrl_out = odin3_fsm_data_bits(dst, (size_t)dst->num_outputs);
    if (tr.ctrl_in == NULL || tr.ctrl_out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(tr.ctrl_in, src->ctrl_in, (size_t)dst->num_inputs);
    memcpy(tr.ctrl_out, src->ctrl_out, (size_t)dst->num_outputs);
    return odin3_fsm_data_add_trans(dst, &tr);
}

odin3_status odin3_fsm_data_copy(const odin3_fsm_data *src, odin3_fsm_data *dst) {
    odin3_status st = odin3_fsm_data_init(dst);
    if (st != ODIN3_OK) {
        return st;
    }
    dst->num_inputs = src->num_inputs;
    dst->num_outputs = src->num_outputs;
    dst->state_bits = src->state_bits;
    dst->reset_state = src->reset_state;
    for (size_t i = 0; st == ODIN3_OK && i < src->states.len; i++) {
        st = odin3_fsm_data_add_state(dst, odin3_fsm_data_state(src, i));
    }
    for (size_t i = 0; st == ODIN3_OK && i < src->trans.len; i++) {
        st = copy_trans(dst, odin3_fsm_data_trans(src, i));
    }
    if (st != ODIN3_OK) {
        odin3_fsm_data_free(dst);
        memset(dst, 0, sizeof *dst);
    }
    return st;
}
