/*
 * register.c — the `tristate` pass definition: the option `--strict` and the entry point ts_run
 * (tristate.c).
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/tristate/tristate.h"
#include "util/log.h"

#include <stdbool.h>

static odin3_status tristate_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user) {
    (void)user;
    ts_opts opts = {.strict = false};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "--strict")) {
            opts.strict = true;
        } else {
            odin3_log(ODIN3_LOG_ERROR, "tristate: unknown argument '%.*s' (known: --strict)",
                      (int)word.len, (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ts_run(ctx, design, &opts);
}

const odin3_pass_def odin3_pass_tristate = {
    "tristate", "tristate [--strict]", tristate_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};
