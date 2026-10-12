/*
 * elab_pending.c — the constructs whose elaboration has not landed yet (plan point (l), ruling
 * C3), so main stays green after every 2D PR: one table per stage of the AST kinds and features
 * still pending, the forced entries of the test hook, and the once-per-kind located error.
 *
 * Each task that lands a kind deletes its line in the same commit; the comments name the task
 * point (l) stages it in. Task 16 asserts the stage-1 table is empty, Task 24 both.
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
    [ODIN3_ELAB_STAGE1] =
        {
            /* Task 12: gates (ordered connections and arrays are forms of Task 11's kinds,
             * reported through odin3_elab1_pend_form until Task 12). */
            [ODIN3_AST_GATE_DECL] = true,
            /* Task 14: generate and scope names. */
            [ODIN3_AST_GENERATE] = true,
            [ODIN3_AST_GEN_FOR] = true,
            [ODIN3_AST_GEN_IF] = true,
            [ODIN3_AST_GEN_CASE] = true,
            [ODIN3_AST_GEN_BLOCK] = true,
            [ODIN3_AST_GENVAR_DECL] = true,
            [ODIN3_AST_HIER_NAME] = true,
            /* Task 15: non-ANSI ports, attributes, subroutines, processes and every statement
             * kind (a port declared in the body: odin3_elab1_pend_form). */
            [ODIN3_AST_PORT_REF] = true,
            [ODIN3_AST_ATTR] = true,
            [ODIN3_AST_FUNCTION_DECL] = true,
            [ODIN3_AST_TASK_DECL] = true,
            [ODIN3_AST_ALWAYS] = true,
            [ODIN3_AST_INITIAL] = true,
            [ODIN3_AST_SEQ_BLOCK] = true,
            [ODIN3_AST_BLOCKING_ASSIGN] = true,
            [ODIN3_AST_NONBLOCKING_ASSIGN] = true,
            [ODIN3_AST_IF] = true,
            [ODIN3_AST_CASE] = true,
            [ODIN3_AST_CASE_ITEM] = true,
            [ODIN3_AST_FOR] = true,
            [ODIN3_AST_WHILE] = true,
            [ODIN3_AST_REPEAT] = true,
            [ODIN3_AST_TIMING_STMT] = true,
            [ODIN3_AST_TASK_CALL] = true,
            [ODIN3_AST_NULL_STMT] = true,
            [ODIN3_AST_EVENT_CONTROL] = true,
            [ODIN3_AST_EVENT_EXPR] = true,
            /* Task 16: the REJECT and IGNORE kinds. */
            [ODIN3_AST_DIRECTIVE] = true,
            [ODIN3_AST_EVENT_DECL] = true,
            [ODIN3_AST_STRENGTH] = true,
            [ODIN3_AST_DELAY] = true,
            [ODIN3_AST_SPECIFY_BLOCK] = true,
            [ODIN3_AST_PAR_BLOCK] = true,
            [ODIN3_AST_PROC_CONT_ASSIGN] = true,
            [ODIN3_AST_FOREVER] = true,
            [ODIN3_AST_WAIT] = true,
            [ODIN3_AST_DISABLE] = true,
            [ODIN3_AST_EVENT_TRIGGER] = true,
            [ODIN3_AST_REAL] = true,
            [ODIN3_AST_MINTYPMAX] = true,
        },
    [ODIN3_ELAB_STAGE2] =
        {
            /* Task 17: the store, scopes, declarations and ports. */
            [ODIN3_AST_UNIT] = true,
            [ODIN3_AST_MODULE] = true,
            [ODIN3_AST_LIST] = true,
            [ODIN3_AST_ATTR] = true,
            [ODIN3_AST_PORT_DECL] = true,
            [ODIN3_AST_DECLARATOR] = true,
            [ODIN3_AST_RANGE] = true,
            [ODIN3_AST_NET_DECL] = true,
            [ODIN3_AST_VAR_DECL] = true,
            [ODIN3_AST_PARAM_DECL] = true,
            [ODIN3_AST_GEN_BLOCK] = true,
            [ODIN3_AST_SEQ_BLOCK] = true,
            [ODIN3_AST_FUNCTION_DECL] = true,
            [ODIN3_AST_TASK_DECL] = true,
            /* Task 18: continuous assignments and expressions. */
            [ODIN3_AST_CONT_ASSIGN] = true,
            [ODIN3_AST_NET_ASSIGN] = true,
            [ODIN3_AST_NUMBER] = true,
            [ODIN3_AST_STRING] = true,
            [ODIN3_AST_IDENT] = true,
            [ODIN3_AST_HIER_NAME] = true,
            [ODIN3_AST_SELECT] = true,
            [ODIN3_AST_CONCAT] = true,
            [ODIN3_AST_REPLICATE] = true,
            [ODIN3_AST_UNARY] = true,
            [ODIN3_AST_BINARY] = true,
            [ODIN3_AST_TERNARY] = true,
            [ODIN3_AST_CALL] = true,
            /* Task 19: instances, gates, drivers. */
            [ODIN3_AST_INSTANTIATION] = true,
            [ODIN3_AST_INSTANCE] = true,
            [ODIN3_AST_CONNECTION] = true,
            [ODIN3_AST_GATE_DECL] = true,
            /* Task 21: always blocks and assignments. */
            [ODIN3_AST_ALWAYS] = true,
            [ODIN3_AST_TIMING_STMT] = true,
            [ODIN3_AST_EVENT_CONTROL] = true,
            [ODIN3_AST_EVENT_EXPR] = true,
            [ODIN3_AST_BLOCKING_ASSIGN] = true,
            [ODIN3_AST_NONBLOCKING_ASSIGN] = true,
            [ODIN3_AST_NULL_STMT] = true,
            /* Task 22: if and case. */
            [ODIN3_AST_IF] = true,
            [ODIN3_AST_CASE] = true,
            [ODIN3_AST_CASE_ITEM] = true,
            /* Task 23: loops, task calls, calls in processes, the two read features. */
            [ODIN3_AST_FOR] = true,
            [ODIN3_AST_REPEAT] = true,
            [ODIN3_AST_WHILE] = true,
            [ODIN3_AST_TASK_CALL] = true,
            [ODIN3_PEND_COMB_CALL] = true,
            [ODIN3_PEND_REGS_VAR_READ] = true,
            /* Task 24: initial blocks and memory writes. */
            [ODIN3_AST_INITIAL] = true,
            [ODIN3_PEND_MEM_WRITE] = true,
        },
};

/* The forms of landed kinds still pending (elab.h, odin3_elab_pending_form). */
static const bool k_forms[ODIN3_ELAB_STAGES + 1][ODIN3_AST_KIND_COUNT] = {
    [ODIN3_ELAB_STAGE1] =
        {
            /* Task 12: ordered connections, instance arrays, a defparam on a cell type. */
            [ODIN3_AST_CONNECTION] = true,
            [ODIN3_AST_INSTANCE] = true,
            [ODIN3_AST_DEFPARAM] = true,
            /* Task 15: a port declared in the body. */
            [ODIN3_AST_PORT_DECL] = true,
            /* Task 16: specparam, time/real variables, tri0/tri1/trireg, hierarchical calls. */
            [ODIN3_AST_PARAM_DECL] = true,
            [ODIN3_AST_VAR_DECL] = true,
            [ODIN3_AST_NET_DECL] = true,
            [ODIN3_AST_CALL] = true,
        },
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
    "continuous-context call",
    "variable index into a register array",
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
