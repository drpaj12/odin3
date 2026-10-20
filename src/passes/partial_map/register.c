/*
 * register.c — the `partial_map` pass definition and its `hardmap` alias (3C spec H-1..H-3): the
 * argument `--min-add N` (4.2 C-4), no processes in the design (the manager refuses a design that
 * still has `$proc` nodes), the entry point is odin3_partial_map_run_opts in hardmap.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/partial_map/hardmap.h"
#include "util/log.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { ARG_CAP = 32, DECIMAL = 10 };
#define OPT_MIN_ADD "--min-add"

/* `--min-add N` (4.2 C-4): N a whole number of at least 1. */
static odin3_status parse_min_add(odin3_bytes value, uint32_t *out) {
    char text[ARG_CAP];
    if (value.ptr == NULL || value.len == 0 || value.len >= sizeof text) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: --min-add needs a whole number of at least 1");
        return ODIN3_ERR_INVALID_ARG;
    }
    memcpy(text, value.ptr, value.len);
    text[value.len] = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, DECIMAL);
    if (errno != 0 || *end != '\0' || text[0] == '-' || parsed < 1 || parsed > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: --min-add '%s': need a whole number of at least 1",
                  text);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (uint32_t)parsed;
    return ODIN3_OK;
}

static odin3_status partial_map_pass_run(odin3_pass_ctx *ctx, odin3_design *design,
                                         odin3_bytes args, void *user) {
    (void)user;
    pm_options opts = {0};
    for (odin3_bytes arg = odin3_pass_arg_next(&args); arg.ptr != NULL;
         arg = odin3_pass_arg_next(&args)) {
        if (arg.len != strlen(OPT_MIN_ADD) || memcmp(arg.ptr, OPT_MIN_ADD, arg.len) != 0) {
            odin3_log(ODIN3_LOG_ERROR, "partial_map: unknown argument '%.*s' (known: --min-add N)",
                      (int)arg.len, (const char *)arg.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_status st = parse_min_add(odin3_pass_arg_next(&args), &opts.min_add);
        if (st != ODIN3_OK) {
            return st;
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
