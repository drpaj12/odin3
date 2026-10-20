/*
 * extract_int.h — what extract.c and extract_trans.c share (private to src/passes/fsm): the
 * context of one FSM's extraction.
 */
#ifndef ODIN3_PASSES_FSM_EXTRACT_INT_H
#define ODIN3_PASSES_FSM_EXTRACT_INT_H

#include "odin3/odin3.h"
#include "passes/fsm/ceval.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/index.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* How an extraction ended. */
typedef enum odin3_fsm_xres {
    ODIN3_FSM_X_OK,
    ODIN3_FSM_X_FAILED, /* logged; the wire stays as it is */
    ODIN3_FSM_X_CAPPED  /* the evaluation cap was reached */
} odin3_fsm_xres;

enum { ODIN3_FSM_EXCL_SHIFT = 32 };
#define ODIN3_FSM_EXCL_MASK 0xFFFFFFFFU

/* The key of the exclusion list for "bit excludes other". */
static inline uint64_t odin3_fsm_excl_key(odin3_fsm_sig bit, odin3_fsm_sig other) {
    return ((uint64_t)bit << ODIN3_FSM_EXCL_SHIFT) | other;
}
static inline odin3_fsm_sig odin3_fsm_excl_bit(uint64_t key) {
    return (odin3_fsm_sig)(key >> ODIN3_FSM_EXCL_SHIFT);
}
static inline odin3_fsm_sig odin3_fsm_excl_other(uint64_t key) {
    return (odin3_fsm_sig)(key & ODIN3_FSM_EXCL_MASK);
}

/* A state code in the sorted state set: its bits (state_bits, in the data's arena) and number. */
typedef struct odin3_fsm_state_ent {
    uint8_t *bits;
    int32_t idx;
} odin3_fsm_state_ent;

typedef struct odin3_fsm_xctx {
    odin3_fsm_mod *fm;
    const odin3_vec *excl;  /* sorted uint64_t: (bit << 32) | other, pmux selects that exclude */
    const odin3_vec *insts; /* odin3_fsm_inst: the module's instances; the last is being built */
    odin3_fsm_budget budget;
    odin3_fsm_inst *inst;
    odin3_vec dff_in; /* odin3_fsm_sig: the next-state root */
    odin3_vec states; /* odin3_fsm_state_ent, sorted by the Const comparator */
} odin3_fsm_xctx;

/* The state with code `bits` (state_bits long) in the sorted set; NULL when absent. */
odin3_fsm_state_ent *odin3_fsm_xctx_find_state(const odin3_fsm_xctx *xc, const uint8_t *bits);

/*
 * find_transitions for every state (fsm_extract.cc:144-254 and :407-418): fills inst->data's
 * transition table. NO_MEMORY; *res is X_CAPPED when the budget ran out and X_FAILED when the
 * logic holds a cell the evaluator cannot evaluate (logged).
 */
odin3_status odin3_fsm_extract_transitions(odin3_fsm_xctx *xc, odin3_fsm_xres *res);

#endif
