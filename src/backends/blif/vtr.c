/* vtr.c — the VTR dialect's preflight (spec 3E VB-11) and resolved options (VB-4). */
#include "backends/blif/vtr.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "util/log.h"

#include <stdint.h>

/* The prefix of every preflight refusal and of the dialect's own lines (spec 3E §11). */
#define VTR_PREFIX "write_blif: --vtr: "

uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts) {
    return opts->lut_k == 0 ? ODIN3_BLIF_VTR_DEFAULT_K : opts->lut_k;
}

/* The options' own checks: a direct caller may pass what odin3_blif_write_with refuses. */
static odin3_status check_opts(const odin3_blif_write_opts *opts) {
    uint32_t k = odin3_blif_vtr_k(opts);
    if (opts->dialect != ODIN3_BLIF_DIALECT_VTR) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: opts are not the VTR dialect");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (k < ODIN3_BLIF_VTR_MIN_K || k > ODIN3_BLIF_VTR_MAX_K) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: lut_k %u is outside %u..%u",
                  (unsigned)k, ODIN3_BLIF_VTR_MIN_K, ODIN3_BLIF_VTR_MAX_K);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (opts->name_style != ODIN3_BLIF_NAMES_SHORT &&
        opts->name_style != ODIN3_BLIF_NAMES_PROVENANCE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: name_style %d is unknown",
                  (int)opts->name_style);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report) {
    if (design == NULL || opts == NULL || report == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: NULL design, opts or report");
        return ODIN3_ERR_INVALID_ARG;
    }
    *report = (odin3_blif_vtr_report){0};
    odin3_status st = check_opts(opts);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_module_id top = odin3_design_top(design);
    if (!odin3_module_valid(top)) { /* E13, checked first and alone (plan 3E point (f)) */
        odin3_log(ODIN3_LOG_ERROR,
                  VTR_PREFIX "no top module (read_blif sets one; or run hierarchy)");
        report->errors = 1;
        return ODIN3_ERR_INVALID_ARG;
    }
    /* module IDs run from 1 to odin3_design_module_end - 1; all but the top are skipped */
    report->modules_skipped = odin3_design_module_end(design) - 2;
    return ODIN3_OK;
}
