/*
 * fsm.h — the fsm pass (4.8; docs/specs/2026-10-20-4.8-fsm-design.md): Yosys 0.55's fsm driver and
 * its fsm_detect, fsm_extract, fsm_opt, fsm_recode, fsm_info and fsm_map stages, ported to the IR
 * with the FSM table inside the pass (no `$fsm` cell; PHASE4 decision 12).
 *
 * The stages run over the whole design in Yosys's order (fsm.cc:138-160): detect, extract, opt,
 * (opt_clean in Yosys: here the unused-output query of unused.c), opt, recode, info, map. Each
 * stage logs at INFO as spec section 16 says. ODIN3_ERR_NO_MEMORY from a stage before map leaves
 * the IR as it was; map builds every new object before its first destructive step.
 */
#ifndef ODIN3_PASSES_FSM_FSM_H
#define ODIN3_PASSES_FSM_FSM_H

#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* The evaluation cap per FSM (X-6, question 4); odin3_fsm_opts.max_evals 0 means this. */
enum { ODIN3_FSM_MAX_EVALS = 1000000 };

typedef struct odin3_fsm_opts {
    bool nodetect;          /* -nodetect: only user-marked registers */
    bool norecode;          /* -norecode */
    bool nomap;             /* -nomap: stages up to and including info, the IR unchanged */
    bool ignore_self_reset; /* fsm_detect -ignore-self-reset */
    const char *encoding;   /* -encoding TYPE for `auto` FSMs, NULL for none */
    uint64_t max_evals;     /* 0: ODIN3_FSM_MAX_EVALS */
} odin3_fsm_opts;

#endif
