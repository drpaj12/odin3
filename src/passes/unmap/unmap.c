/*
 * unmap.c — the module loop of the `unmap` pass (spec S-1, S-3): the phases per module in order.
 */
#include "passes/unmap/unmap.h"

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/unmap/chain.h"
#include "passes/unmap/mul.h"
#include "passes/unmap/mul_group.h"
#include "passes/unmap/ram.h"
#include "passes/unmap/ram_depth.h"

#include "util/log.h"

#include <inttypes.h>
#include <string.h>

odin3_status um_module(um_ctx *uc) {
    odin3_status st = ODIN3_OK;
    if (uc->types.adder.v != 0) {
        st = um_adder_phase(uc);
    }
    if (st == ODIN3_OK && uc->types.multiply.v != 0) {
        st = um_mul_phase(uc);
    }
    if (st == ODIN3_OK && uc->opts->regroup && uc->types.multiply.v != 0) {
        st = um_mul_group_phase(uc);
    }
    if (st == ODIN3_OK && (uc->types.sp_ram.v != 0 || uc->types.dp_ram.v != 0)) {
        st = um_ram_phase(uc);
    }
    if (st == ODIN3_OK && uc->opts->regroup && uc->types.mem.v != 0) {
        st = um_ram_depth_phase(uc);
    }
    return st;
}

/* P-4: one INFO line per run, when at least one hard cell was seen. */
static void log_summary(const um_stats *stats) {
    if (stats->adder_cells + stats->multiply + stats->ram_cells == 0) {
        return;
    }
    odin3_log(ODIN3_LOG_INFO,
              "unmap: %" PRIu64 " adder cells to %" PRIu64 " $add + %" PRIu64 " $sub, %" PRIu64
              " multiply to %" PRIu64 " $mul (%" PRIu64 " regrouped), %" PRIu64
              " RAM cells to %" PRIu64 " $mem, %" PRIu64 " groups left (see unmap_note)",
              stats->adder_cells, stats->add, stats->sub, stats->multiply, stats->mul,
              stats->regrouped_mul, stats->ram_cells, stats->mem, stats->declined);
}

odin3_status um_run(odin3_pass_ctx *ctx, odin3_design *design, const um_opts *opts,
                    um_stats *stats) {
    um_ctx uc;
    memset(&uc, 0, sizeof uc);
    uc.pass = ctx;
    uc.design = design;
    uc.opts = opts;
    uint32_t end = odin3_design_module_end(design);
    odin3_status st = um_types_find(design, &uc.types);
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        uc.module = odin3_module_get(design, (odin3_module_id){mid});
        memset(uc.konst, 0, sizeof uc.konst);
        st = um_module(&uc);
    }
    if (st == ODIN3_OK) {
        log_summary(&uc.stats);
    }
    if (stats != NULL) {
        *stats = uc.stats;
    }
    return st;
}
