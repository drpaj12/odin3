/*
 * register.c — the `lpmmap` pass definition (5.1 spec W-5): `lpmmap [--family NAME]`, no processes
 * in the design (the manager refuses a design that still has `$proc` nodes); the entry point is
 * odin3_lpmmap_run in lpmmap.c.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/lpmmap/lpmmap.h"
#include "passes/manager.h"
#include "util/log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { FAMILY_MAX = 128 };

/* Parses the arguments into opts; family is the storage of the family name. */
static odin3_status parse_args(odin3_bytes args, odin3_lpmmap_opts *opts, char *family) {
    odin3_bytes tok = odin3_pass_arg_next(&args);
    while (tok.ptr != NULL) {
        if (tok.len == strlen("--family") && memcmp(tok.ptr, "--family", tok.len) == 0) {
            odin3_bytes name = odin3_pass_arg_next(&args);
            if (name.ptr == NULL || name.len == 0) {
                odin3_log(ODIN3_LOG_ERROR, "lpmmap: --family needs a name");
                return ODIN3_ERR_INVALID_ARG;
            }
            if (name.len >= FAMILY_MAX) {
                odin3_log(ODIN3_LOG_ERROR, "lpmmap: the family name is too long");
                return ODIN3_ERR_INVALID_ARG;
            }
            memcpy(family, name.ptr, name.len);
            family[name.len] = '\0';
            opts->family = family;
        } else {
            odin3_log(ODIN3_LOG_ERROR, "lpmmap: unknown option '%.*s'", (int)tok.len,
                      (const char *)tok.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        tok = odin3_pass_arg_next(&args);
    }
    return ODIN3_OK;
}

static odin3_status lpmmap_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)user;
    char family[FAMILY_MAX];
    odin3_lpmmap_opts opts = {NULL};
    odin3_status st = parse_args(args, &opts, family);
    return st == ODIN3_OK ? odin3_lpmmap_run(ctx, design, &opts, NULL) : st;
}

const odin3_pass_def odin3_pass_lpmmap = {"lpmmap", "lpmmap", lpmmap_pass_run,
                                          NULL,     false,    ODIN3_PASS_PROCESSES_REFUSE};
