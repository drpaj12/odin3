/*
 * register_merge.c — the `memory_merge` pass definition (4.2 spec G-1): no arguments, no
 * processes in the design (the manager refuses a design that still has `$proc` nodes); the entry
 * point is pm_mem_merge in memory_merge.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/partial_map/memory_merge.h"
#include "util/log.h"

#include <stddef.h>

static odin3_status memory_merge_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)user;
    if (odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "memory_merge: takes no arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    return pm_mem_merge(ctx, design, NULL);
}

const odin3_pass_def odin3_pass_memory_merge = {
    "memory_merge", "memory_merge", memory_merge_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};
