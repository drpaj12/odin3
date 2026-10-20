/*
 * fsmdata.h — the FSM table of the fsm pass (4.8 spec section 4, F-n): Yosys 0.55's FsmData
 * (passes/fsm/fsmdata.h) as a C struct that lives inside the pass; no `$fsm` cell exists in the
 * IR (PHASE4 decision 14, question 1).
 *
 * A code is `state_bits` values of odin3_fsm_bit, bit 0 first (Yosys RTLIL::Const). The six values
 * are Yosys's State order, which is also the order of the Const comparator (rtlil.cc:316-326):
 * 0 < 1 < x < z < - < m. A transition holds `num_inputs` control-input bits and `num_outputs`
 * control-output bits (the pattern of ctrl_in has `-` for a bit that did not matter).
 *
 * Ownership. All code and transition bit arrays live in the data's arena and are never freed one
 * by one; an optimisation stage builds new arrays and drops the old ones until the arena goes with
 * odin3_fsm_data_free. Failures. ODIN3_ERR_NO_MEMORY with the data valid (what was added before
 * the failure stays); nothing logs.
 */
#ifndef ODIN3_PASSES_FSM_FSMDATA_H
#define ODIN3_PASSES_FSM_FSMDATA_H

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Yosys RTLIL::State: the order is the Const comparator's. */
typedef enum odin3_fsm_bit {
    ODIN3_FSM_BIT_0,
    ODIN3_FSM_BIT_1,
    ODIN3_FSM_BIT_X,
    ODIN3_FSM_BIT_Z,
    ODIN3_FSM_BIT_DC,  /* Sa, printed '-' */
    ODIN3_FSM_BIT_MARK /* Sm, printed 'm' */
} odin3_fsm_bit;

/* The character Yosys prints for a bit: 0 1 x z - m. */
char odin3_fsm_bit_char(uint8_t bit);

/* True for a 0 or a 1. */
static inline bool odin3_fsm_bit_defined(uint8_t bit) {
    return bit <= ODIN3_FSM_BIT_1;
}

/*
 * Yosys RTLIL::Const::operator<: a shorter constant sorts first; of equal length the first
 * differing bit from bit 0 up decides, by the order of odin3_fsm_bit. Returns <0, 0 or >0. This
 * order numbers the states (std::map<Const,int>), so it fixes every one-hot bit: value 2 sorts
 * before value 1 for 2-bit codes (bit 0 decides first).
 */
int odin3_fsm_const_cmp(const uint8_t *lhs, uint32_t lhs_len, const uint8_t *rhs, uint32_t rhs_len);

/* One transition (state_in, ctrl_in, state_out, ctrl_out); the arrays are in the data's arena. */
typedef struct odin3_fsm_trans {
    int32_t state_in, state_out;
    uint8_t *ctrl_in, *ctrl_out;
} odin3_fsm_trans;

typedef struct odin3_fsm_data {
    odin3_arena *arena;
    int32_t num_inputs, num_outputs, state_bits, reset_state; /* reset_state -1: none */
    odin3_vec states;                                         /* uint8_t *, state_bits each */
    odin3_vec trans;                                          /* odin3_fsm_trans */
} odin3_fsm_data;

/* An empty table (reset_state -1); NO_MEMORY with *data zeroed. */
odin3_status odin3_fsm_data_init(odin3_fsm_data *data);

/* Frees everything; safe on a zeroed or already freed data. */
void odin3_fsm_data_free(odin3_fsm_data *data);

/* A zeroed array of n bits in the arena; NULL on out of memory. */
uint8_t *odin3_fsm_data_bits(odin3_fsm_data *data, size_t n);

/* Appends a state code of data->state_bits bits (copied). */
odin3_status odin3_fsm_data_add_state(odin3_fsm_data *data, const uint8_t *code);

/* Appends a transition; its arrays are used as given (they must be in the data's arena). */
odin3_status odin3_fsm_data_add_trans(odin3_fsm_data *data, const odin3_fsm_trans *trans);

/* The code of state i (i < number of states) and the transition i. */
static inline uint8_t *odin3_fsm_data_state(const odin3_fsm_data *data, size_t idx) {
    return ((uint8_t **)data->states.data)[idx];
}
static inline odin3_fsm_trans *odin3_fsm_data_trans(const odin3_fsm_data *data, size_t idx) {
    return &((odin3_fsm_trans *)data->trans.data)[idx];
}

/* A deep copy of src in *dst (init'ed here; *dst is zeroed on failure). */
odin3_status odin3_fsm_data_copy(const odin3_fsm_data *src, odin3_fsm_data *dst);

#endif
