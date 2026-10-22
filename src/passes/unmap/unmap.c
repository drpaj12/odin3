/*
 * unmap.c — the module loop of the `unmap` pass (spec S-1, S-3): the phases per module in order.
 */
#include "passes/unmap/unmap.h"

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"

#include <string.h>

odin3_status um_module(um_ctx *uc) {
    (void)uc;
    return ODIN3_OK;
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
    if (stats != NULL) {
        *stats = uc.stats;
    }
    return st;
}
