/*
 * elab_pending.c — the constructs whose elaboration has not landed yet (plan point (l), ruling
 * C3), so main stays green after every 2D PR: one table per stage of the AST kinds and features
 * still pending, the forced entries of the test hook, and the once-per-kind located error.
 *
 * Each task that lands a kind deletes its line in the same commit; the comments name the task
 * point (l) stages it in. Task 16 emptied the stage-1 tables; Task 17 removed stage 2's store,
 * scope, declaration and port kinds; Task 18 its expression and continuous-assignment kinds; Task
 * 19 its instance, connection and gate kinds and the driver check; Task 20 the memory word read;
 * Task 21 the always block and the assignments; Task 22 `if` and `case`; Task 23 loops, task and
 * function calls (in processes and in continuous contexts) and the words of register arrays with
 * a variable index; Task 24 asserts the rest of stage 2's table is empty.
 */
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"

#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>

static const bool k_pending[ODIN3_ELAB_STAGES + 1][ODIN3_PEND_END] = {
    [ODIN3_ELAB_STAGE1] = {false}, /* Task 16 landed the last stage-1 kinds: empty */
    [ODIN3_ELAB_STAGE2] =
        {
            /* Task 17 landed the store, scopes, declarations and ports: nothing is left of it. */
            /* Task 18 landed continuous assignments and expressions (a user call inside them is
             * the continuous-context call, Task 23). */
            /* Task 20 landed memory word reads: nothing is left of it. */
            /* Task 19 landed instances, gates and the driver check: nothing is left of it. */
            /* Task 21 landed always blocks, their event controls and the assignments: nothing is
             * left of them. */
            /* Task 22 landed if and case (and the case method for a bit or part select with a
             * non-constant index in a target). */
            /* Task 23 landed loops, task enables and function calls (`while` only reaches stage 2
             * in a function that is inlined, which is refused by its own located error), and the
             * word of a register array with a variable index in a read or a target. */
            /* Task 24: initial blocks and memory writes. */
            [ODIN3_AST_INITIAL] = true,
            [ODIN3_PEND_MEM_WRITE] = true,
        },
};

/* The forms of landed kinds still pending (elab.h, odin3_elab_pending_form). */
static const bool k_forms[ODIN3_ELAB_STAGES + 1][ODIN3_AST_KIND_COUNT] = {
    [ODIN3_ELAB_STAGE1] = {false}, /* every stage-1 form landed with Task 16: empty */
};

bool odin3_elab_pending_form(uint8_t stage, uint32_t kind) {
    return (stage == ODIN3_ELAB_STAGE1 || stage == ODIN3_ELAB_STAGE2) &&
           kind < ODIN3_AST_KIND_COUNT && k_forms[stage][kind];
}

uint32_t odin3_elab_pending_form_count(uint8_t stage) {
    uint32_t count = 0;
    for (uint32_t kind = 0; kind < ODIN3_AST_KIND_COUNT; kind++) {
        count += odin3_elab_pending_form(stage, kind) ? 1U : 0U;
    }
    return count;
}

/* The features' names in point (l)'s line. */
static const char *const k_feature_names[ODIN3_PEND_END - ODIN3_AST_KIND_COUNT] = {
    "memory write",
};

static bool g_forced[ODIN3_ELAB_STAGES + 1][ODIN3_PEND_END];

static bool stage_ok(uint8_t stage) {
    return stage == ODIN3_ELAB_STAGE1 || stage == ODIN3_ELAB_STAGE2;
}

bool odin3_elab_pending(uint8_t stage, uint32_t kind_or_feature) {
    if (!stage_ok(stage) || kind_or_feature >= ODIN3_PEND_END) {
        return false;
    }
    return k_pending[stage][kind_or_feature] || g_forced[stage][kind_or_feature];
}

void odin3_elab_test_pend(uint8_t stage, uint32_t kind_or_feature, bool pending) {
    if (stage_ok(stage) && kind_or_feature < ODIN3_PEND_END) {
        g_forced[stage][kind_or_feature] = pending;
    }
}

static const char *pend_name(uint32_t kind_or_feature) {
    if (kind_or_feature >= ODIN3_AST_KIND_COUNT) {
        return k_feature_names[kind_or_feature - ODIN3_AST_KIND_COUNT];
    }
    return odin3_ast_kind_name(kind_or_feature);
}

void odin3_elab_pend_report(odin3_design *design, odin3_loc loc, uint32_t kind_or_feature,
                            bool *seen) {
    if (seen == NULL || kind_or_feature >= ODIN3_PEND_END || seen[kind_or_feature]) {
        return;
    }
    seen[kind_or_feature] = true;
    const char *name = pend_name(kind_or_feature);
    if (loc.v == 0 || design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "elaboration is not available yet (2D): '%s'", name);
        return;
    }
    odin3_diag(design, ODIN3_LOG_ERROR, loc, "elaboration is not available yet (2D): '%s'", name);
}
