/*
 * ceval.h — the constant evaluator of the fsm pass: Yosys 0.55's ConstEval (kernel/consteval.h)
 * on the IR (4.8 spec section 4). Values live on nets (the values_map), a stack of snapshots is
 * kept as an undo journal, stop signals end an evaluation, and a `busy` set guards combinational
 * loops. The cells it evaluates are the foldable registry types (fold.h, Yosys's CellTypes::eval
 * on constants, through 2D's operators) and $mux, $pmux and $_MUX_ (consteval.h:122-171: a select
 * bit of x or 1 makes its slice a candidate, a bit that is 1 counts, no 1 adds A, and candidates
 * that differ give x). A sequential cell, a memory, a tribuf, an instance and a type outside that
 * list are not evaluable: the evaluation fails with no undefined bit, as CellTypes::eval's error
 * does.
 *
 * No recursion: the evaluation runs on an explicit frame stack, so a mux tree 100,000 deep needs
 * no machine stack. The drivers of a signal are tried in node ID order (Yosys: std::set order of
 * the cell pointers).
 *
 * Budget. Every cell evaluation counts in *evals, shared by the evaluators of one FSM; over
 * max_evals the evaluation fails with res.capped set (X-6).
 *
 * Failures. ODIN3_ERR_NO_MEMORY (the evaluator is then unusable but destroyable); misuse is not
 * detected.
 */
#ifndef ODIN3_PASSES_FSM_CEVAL_H
#define ODIN3_PASSES_FSM_CEVAL_H

#include "odin3/odin3.h"
#include "passes/fsm/index.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_fsm_ceval odin3_fsm_ceval;

/* The outcome of one evaluation. undef is the first undefined bit (Yosys's `undef`) when has_undef.
 */
typedef struct odin3_fsm_ceval_res {
    bool ok;        /* every bit of the signal is now constant */
    bool capped;    /* the evaluation budget ran out */
    bool has_undef; /* undef is set: a stop signal, a loop, or a bit nothing drives */
    odin3_fsm_sig undef;
} odin3_fsm_ceval_res;

/* The two counters of one FSM's evaluators. */
typedef struct odin3_fsm_budget {
    uint64_t used, max;
} odin3_fsm_budget;

/* An evaluator over the index's module; *budget must outlive it. NO_MEMORY with *out NULL. */
odin3_status odin3_fsm_ceval_create(const odin3_fsm_index *ix, odin3_fsm_budget *budget,
                                    odin3_fsm_ceval **out);

/* NULL is a no-op. */
void odin3_fsm_ceval_destroy(odin3_fsm_ceval *ce);

/* Adds count bits to the stop signals (constants are ignored). */
void odin3_fsm_ceval_stop(odin3_fsm_ceval *ce, const odin3_fsm_sig *sig, uint32_t count);

/* Yosys push() and pop(): pop forgets every value set since the matching push. NO_MEMORY. */
odin3_status odin3_fsm_ceval_push(odin3_fsm_ceval *ce);
void odin3_fsm_ceval_pop(odin3_fsm_ceval *ce);

/*
 * Yosys set(): bit k of sig takes bits[k] (odin3_fsm_bit 0, 1, x or z; other values are skipped).
 * A constant bit of sig is skipped. NO_MEMORY with the bits set so far kept.
 */
odin3_status odin3_fsm_ceval_set(odin3_fsm_ceval *ce, const odin3_fsm_sig *sig, const uint8_t *bits,
                                 uint32_t count);

/* sig with the value set on it, if any (values_map(sig)); constants and unset nets are as given. */
odin3_fsm_sig odin3_fsm_ceval_get(const odin3_fsm_ceval *ce, odin3_fsm_sig sig);

/*
 * Yosys eval(sig, undef): evaluates the drivers of the non-constant bits of sig[0 .. count) and
 * replaces the bits that have a value by their constants in place (the array is updated as far as
 * the evaluation got, as Yosys's SigSpec is). *res tells the outcome.
 */
odin3_status odin3_fsm_ceval_eval(odin3_fsm_ceval *ce, odin3_fsm_sig *sig, uint32_t count,
                                  odin3_fsm_ceval_res *res);

#endif
