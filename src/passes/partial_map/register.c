/*
 * register.c — the `partial_map` pass definition and its `hardmap` alias (3C spec H-1..H-3): no
 * arguments, no processes in the design (the manager refuses a design that still has `$proc`
 * nodes), the entry point is odin3_hardmap_run in hardmap.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/partial_map/hardmap.h"
#include "util/log.h"

#include <stddef.h>

static odin3_status partial_map_pass_run(odin3_pass_ctx *ctx, odin3_design *design,
                                         odin3_bytes args, void *user) {
    (void)user;
    if (odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: takes no arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_partial_map_run(ctx, design, NULL);
}

const odin3_pass_def odin3_pass_partial_map = {
    "partial_map", "partial_map", partial_map_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};

/* S-1: the old name, kept for this phase; the same entry, so the same log prefix and bytes. */
const odin3_pass_def odin3_pass_hardmap = {
    "hardmap", "hardmap (alias of partial_map)", partial_map_pass_run, NULL,
    false,     ODIN3_PASS_PROCESSES_REFUSE};
