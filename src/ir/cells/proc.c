/*
 * proc.c — the process cell $proc (2D IR change B: EL-33, spec §8.1; docs/IR.md IR-19).
 *
 * One node per `always` block. Port I (width I_WIDTH) holds every net the process reads and does
 * not drive, constants included as pins on the module's shared $_CONST*_ nets; port O (width
 * O_WIDTH) every net it drives. BODY (kind ODIN3_VAL_PROC) is the decision structure: actions,
 * cases, switches and syncs over pin positions (ir/proc.h). The empty default BODY is rejected,
 * so a default node cannot exist. No const_value, simulate or writer hooks: `proc` (2E) lowers it.
 */
#include "ir/proc.h"
#include "cells.h"
#include "util/log.h"

static const odin3_param_def k_params[] = {
    ODIN3_P_INT("I_WIDTH", 0),
    ODIN3_P_INT("O_WIDTH", 1),
    {"BODY", ODIN3_VAL_PROC, {ODIN3_VAL_PROC, 0, NULL, 0, 0, 0}},
};
static const odin3_port_def k_ports[] = {
    ODIN3_PORT_VEC("I", ODIN3_DIR_IN, "I_WIDTH"),
    ODIN3_PORT_VEC("O", ODIN3_DIR_OUT, "O_WIDTH"),
};

/*
 * I_WIDTH >= 0 and O_WIDTH >= 1 (each at most UINT32_MAX), then the body against them. Never
 * allocates (odin3_proc_body_verify does not), so out of memory is never a rejected body.
 */
static odin3_status proc_verify(const odin3_value *params) {
    static const odin3_int_range k_widths[] = {{"I_WIDTH", 0, UINT32_MAX},
                                               {"O_WIDTH", 1, UINT32_MAX}};
    if (odin3_cells_check_ints(&params[ODIN3_PROC_P_I_WIDTH], k_widths, ODIN3_NELEM(k_widths)) !=
        ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_proc_widths widths = {(uint32_t)params[ODIN3_PROC_P_I_WIDTH].i,
                                (uint32_t)params[ODIN3_PROC_P_O_WIDTH].i};
    odin3_proc_body body;
    odin3_proc_why why = {"not a process body"};
    odin3_status st = odin3_proc_body_view(&params[ODIN3_PROC_P_BODY], &body)
                          ? odin3_proc_body_verify(&body, widths, &why)
                          : ODIN3_ERR_INVALID_ARG;
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "$proc: BODY: %s", why.text);
    }
    return st;
}

const odin3_celltype_def odin3_cell_proc = {
    "$proc",  ODIN3_GRAN_WORD,       ODIN3_CT_PROCESS, k_ports, ODIN3_NELEM(k_ports),
    k_params, ODIN3_NELEM(k_params), proc_verify,      NULL,    NULL,
    NULL};
