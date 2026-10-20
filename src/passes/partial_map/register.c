/*
 * register.c — the `partial_map` pass definition and its `hardmap` alias (3C spec H-1..H-3): the
 * arguments `--allow-unknown-blocks` (B-6), `--min-add N` (4.2 C-4) and `--mem-width arch` (R-4),
 * no processes in the design (the manager refuses a design that still has `$proc` nodes), the
 * entry point is
 * odin3_partial_map_run_opts in hardmap.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/partial_map/hardmap.h"
#include "util/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { ARG_CAP = 32, DECIMAL = 10 };
#define OPT_MIN_ADD "--min-add"

/* `--min-add N` (4.2 C-4): N a plain decimal whole number of at least 1 (no sign, no blanks).
 * Given twice, the last value wins. */
static odin3_status parse_min_add(odin3_bytes value, uint32_t *out) {
    char text[ARG_CAP];
    if (value.ptr == NULL || value.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: --min-add needs a whole number of at least 1");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool fits = value.len < sizeof text;
    size_t len = fits ? value.len : sizeof text - 1;
    memcpy(text, value.ptr, len);
    text[len] = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, DECIMAL);
    bool digits = isdigit((unsigned char)text[0]) != 0;
    if (!digits || !fits || errno != 0 || *end != '\0' || parsed < 1 || parsed > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "partial_map: --min-add '%s': need a whole number of at least 1",
                  text);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (uint32_t)parsed;
    return ODIN3_OK;
}

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
    odin3_partial_map_opts opts = {.allow_unknown_blocks = false};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        odin3_status st = ODIN3_OK;
        if (odin3_pass_arg_is(word, "--allow-unknown-blocks")) {
            opts.allow_unknown_blocks = true;
        } else if (odin3_pass_arg_is(word, OPT_MIN_ADD)) {
            st = parse_min_add(odin3_pass_arg_next(&args), &opts.min_add);
        } else if (odin3_pass_arg_is(word, "--mem-width")) {
            st = parse_mem_width(&args, &opts);
        } else {
            odin3_log(ODIN3_LOG_ERROR,
                      "partial_map: unknown argument '%.*s' (known: --allow-unknown-blocks, "
                      "--min-add N, --mem-width arch)",
                      (int)word.len, (const char *)word.ptr);
            st = ODIN3_ERR_INVALID_ARG;
        }
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
