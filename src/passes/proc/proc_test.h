/*
 * proc_test.h — test hooks of the proc pass (2E spec §16, §18.1; plan point (h)): lowered caps,
 * and fake phases that let the schedule, atomicity, statistics and step F be tested before the
 * real phases land. Never part of the ABI.
 */
#ifndef ODIN3_PASSES_PROC_PROC_TEST_H
#define ODIN3_PASSES_PROC_PROC_TEST_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/proc/proc.h"
#include "passes/proc/pw.h"

#include <stdbool.h>
#include <stdint.h>

/* The caps of proc.h; budget_pins lowers ODIN3_PROC_BUDGET_PINS (finding 15). */
typedef struct odin3_proc_limits {
    uint32_t max_errors, max_pattern_bits, max_arst_rounds, max_signal_walk, budget_pins;
} odin3_proc_limits;

/* Sets the caps (NULL restores the defaults; each value is clamped to [1, its default]). */
void odin3_proc_test_set_limits(const odin3_proc_limits *limits);

/* The caps in force. */
odin3_proc_limits odin3_proc_limits_now(void);

/* The phases, in schedule order: the analysis phases of step A, then the lowering phases. */
typedef enum odin3_proc_phase {
    ODIN3_PROC_PH_CLEAN,
    ODIN3_PROC_PH_RMDEAD,
    ODIN3_PROC_PH_PRUNE,
    ODIN3_PROC_PH_ARST,
    ODIN3_PROC_PH_CLOCKS,
    ODIN3_PROC_PH_MUX,
    ODIN3_PROC_PH_LATCH,
    ODIN3_PROC_PH_FF,
    ODIN3_PROC_N_PHASES
} odin3_proc_phase;

/* One phase call: the module's lowering state and the process (pw is NULL for LATCH). */
typedef struct odin3_proc_phase_call {
    odin3_proc_phase phase;
    odin3_proc_mctx *m;
    odin3_pw *pw;
} odin3_proc_phase_call;

/*
 * phase: called before each phase; when it returns true the hook replaced the phase and *st is its
 * status (a phase reports a user error with odin3_proc_error and ODIN3_ERR_PARSE). While it is
 * installed the end-of-step-A check for pending lowering phases (plan point (h)) is skipped, so it
 * must handle every lowering phase still pending. on_step_f: called at step F for each lowered
 * module (unless --noopt or --stop-after). Either may be NULL.
 */
typedef struct odin3_proc_test_hooks {
    bool (*phase)(void *user, const odin3_proc_phase_call *call, odin3_status *st);
    void (*on_step_f)(void *user, odin3_module_id module);
    void *user;
} odin3_proc_test_hooks;

/* Installs the hooks (copied; NULL clears them). */
void odin3_proc_test_set_hooks(const odin3_proc_test_hooks *hooks);

/* The sweeps of the last odin3_pw_clean call, its last (no did_something) one included. */
uint32_t odin3_proc_test_clean_sweeps(void);

/* The frame steps (one per case or switch visit) of the last odin3_pw_clean call, its walk's
 * work: O(subtree) per sweep (spec §19). */
uint64_t odin3_proc_test_clean_steps(void);

/* The driver hops of the last odin3_proc_check_signal walk (arst's included): at most
 * odin3_proc_limits_now().max_signal_walk (spec §16). */
uint32_t odin3_proc_test_signal_hops(void);

#endif
