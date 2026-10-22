/*
 * register.c — the `unmap` pass definition (spec S-1, S-4): the option `--no-regroup` and the entry
 * point um_run (unmap.c). No processes in the design (the manager refuses them).
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/unmap/unmap.h"
#include "util/log.h"

#include <stdbool.h>

static odin3_status unmap_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)user;
    um_opts opts = {.regroup = true};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "--no-regroup")) {
            opts.regroup = false;
        } else {
            odin3_log(ODIN3_LOG_ERROR, "unmap: unknown argument '%.*s' (known: --no-regroup)",
                      (int)word.len, (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return um_run(ctx, design, &opts, NULL);
}

const odin3_pass_def odin3_pass_unmap = {"unmap", "unmap [--no-regroup]",     unmap_pass_run, NULL,
                                         false,   ODIN3_PASS_PROCESSES_REFUSE};
