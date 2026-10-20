/*
 * register.c — the `partial_map` pass definition and its `hardmap` alias (3C spec H-1..H-3): no
 * arguments but `--allow-unknown-blocks` (B-6) and `--mem-width arch` (R-4), no processes in the
 * design (the manager refuses a design that still has `$proc` nodes), the entry point is
 * odin3_partial_map_run_opts in hardmap.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/partial_map/hardmap.h"
#include "util/log.h"

#include <stddef.h>

/* `--mem-width arch` (R-4, spec S-5): the only option. `--mem-width 1` is the default, accepted. */
static odin3_status parse_mem_width(odin3_bytes *args, odin3_partial_map_opts *opts) {
    odin3_bytes value = odin3_pass_arg_next(args);
    if (value.ptr != NULL && odin3_pass_arg_is(value, "arch")) {
        opts->mem_width_arch = true;
        return ODIN3_OK;
    }
    if (value.ptr != NULL && odin3_pass_arg_is(value, "1")) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR, "partial_map: --mem-width expects arch or 1");
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status partial_map_pass_run(odin3_pass_ctx *ctx, odin3_design *design,
                                         odin3_bytes args, void *user) {
    (void)user;
    odin3_partial_map_opts opts = {false, false};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "--allow-unknown-blocks")) {
            opts.allow_unknown_blocks = true;
        } else if (odin3_pass_arg_is(word, "--mem-width")) {
            odin3_status st = parse_mem_width(&args, &opts);
            if (st != ODIN3_OK) {
                return st;
            }
        } else {
            odin3_log(ODIN3_LOG_ERROR,
                      "partial_map: takes no arguments except --allow-unknown-blocks and "
                      "--mem-width (got '%.*s')",
                      (int)word.len, (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return odin3_partial_map_run_opts(ctx, design, &opts, NULL);
}

const odin3_pass_def odin3_pass_partial_map = {
    "partial_map", "partial_map", partial_map_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};

/* S-1: the old name, kept for this phase; the same entry, so it logs the new `partial_map:`
 * prefix too. */
const odin3_pass_def odin3_pass_hardmap = {
    "hardmap", "hardmap (alias of partial_map)", partial_map_pass_run, NULL,
    false,     ODIN3_PASS_PROCESSES_REFUSE};
