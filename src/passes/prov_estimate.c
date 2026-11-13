/*
 * prov_estimate.c — the `prov-estimate` pass: from the cells of the design as it is now (best
 * after `proc`), the records, parent edges and bytes each provenance depth would write: the name
 * map, and the step files of `odin3-flow --prov`. Spec docs/specs/2026-11-13-prov-levels.md.
 */
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/prov_depth.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdint.h>

static odin3_status prov_estimate_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user) {
    (void)ctx;
    (void)user;
    if (odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "prov-estimate: takes no arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    odin3_log(ODIN3_LOG_INFO,
              "prov-estimate: design: %" PRIu64 " cells, %" PRIu64 " pins (estimate of the data "
              "each provenance depth writes)",
              shape.nodes, shape.pins);
    for (uint32_t d = 0; d < ODIN3_PROV_DEPTH_COUNT; d++) {
        odin3_log(ODIN3_LOG_INFO,
                  "prov-estimate: depth %u (%s): %" PRIu64 " records, %" PRIu64
                  " edges, name map %" PRIu64 " bytes, step files %" PRIu64 " bytes, total %" PRIu64
                  " bytes",
                  (unsigned)d, odin3_prov_depth_name(d), est.records[d], est.edges[d],
                  est.map_bytes[d], est.step_bytes[d], odin3_prov_estimate_total(&est, d));
    }
    int depth = odin3_prov_depth_effective(design);
    if (depth >= 0) {
        odin3_log(ODIN3_LOG_INFO, "prov-estimate: in force: depth %d (%s)", depth,
                  odin3_prov_depth_name((uint32_t)depth));
    }
    return ODIN3_OK;
}

const odin3_pass_def odin3_pass_prov_estimate = {
    "prov-estimate",
    "prov-estimate: log the records, edges and bytes each provenance depth (--prov-level 0..4) "
    "would write for the design as it is (best after proc): the name map and the odin3-flow step "
    "files; the depth in force with --prov-level or --prov-budget",
    prov_estimate_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
