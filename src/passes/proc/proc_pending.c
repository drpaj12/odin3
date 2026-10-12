/*
 * proc_pending.c — the phases of proc that have not landed yet (plan point (h), ruling I4), so the
 * pass can be registered while main stays green. Each pending phase reports "proc: '<phase>' is
 * not available yet (2E)" once per run, with ODIN3_ERR_PARSE. An analysis phase is pending when
 * step A reaches it, before any IR change; the lowering phases are checked at the end of step A
 * (odin3_proc_pending_first), before step B deletes anything, so no stub ever runs after a node
 * is deleted. Tasks 4-9 each remove their entries in the commit that lands the phase, and Task 9
 * deletes this file.
 */
#include "passes/proc/proc_internal.h"

#include "odin3/odin3.h"
#include "passes/proc/proc_test.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The pending phases, in schedule order, with the names their line gives. */
typedef struct pending_phase {
    odin3_proc_phase phase;
    const char *name;
} pending_phase;

static const pending_phase k_pending[] = {
    {ODIN3_PROC_PH_CLEAN, "clean"},   {ODIN3_PROC_PH_RMDEAD, "rmdead"},
    {ODIN3_PROC_PH_PRUNE, "prune"},   {ODIN3_PROC_PH_ARST, "arst"},
    {ODIN3_PROC_PH_CLOCKS, "clocks"}, {ODIN3_PROC_PH_MUX, "mux"},
    {ODIN3_PROC_PH_LATCH, "latch"},   {ODIN3_PROC_PH_FF, "ff"},
};

enum { N_PENDING = sizeof k_pending / sizeof k_pending[0] };

static const pending_phase *find_pending(odin3_proc_phase phase) {
    for (size_t i = 0; i < N_PENDING; i++) {
        if (k_pending[i].phase == phase) {
            return &k_pending[i];
        }
    }
    return NULL;
}

odin3_proc_phase odin3_proc_pending_first(void) {
    return N_PENDING > 0 ? k_pending[0].phase : ODIN3_PROC_N_PHASES;
}

bool odin3_proc_pending(odin3_proc_phase phase) {
    return find_pending(phase) != NULL;
}

odin3_status odin3_proc_pending_run(odin3_proc_mctx *mctx, odin3_proc_phase phase) {
    const pending_phase *entry = find_pending(phase);
    if (entry == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: phase %d is not pending", (int)phase);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t bit = 1U << (uint32_t)phase;
    if (mctx->reported != NULL && (*mctx->reported & bit) != 0) {
        return ODIN3_ERR_PARSE;
    }
    if (mctx->reported != NULL) {
        *mctx->reported |= bit;
    }
    mctx->errors++;
    if (mctx->stats != NULL) {
        mctx->stats->errors++;
    }
    odin3_log(ODIN3_LOG_ERROR, "proc: '%s' is not available yet (2E)", entry->name);
    return ODIN3_ERR_PARSE;
}
