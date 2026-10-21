/*
 * raise_int.h — what the files of the raise pass share (Phase 7 spec 3.4, plan Task 8). Internal.
 * The pass reuses the group, the template and the option parsing of cone_check.
 */
#ifndef ODIN3_PASSES_RAISE_RAISE_INT_H
#define ODIN3_PASSES_RAISE_RAISE_INT_H

#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/cone_check/cone_check_int.h"

#include <stdbool.h>

/*
 * Checks one module without changing it (same refusals as a run): *found says whether it has a
 * group KEY=V at all (when not, nothing else happens). A module with the group is refused,
 * unchanged, unless it holds the cone_check proof for exactly this group, binding and template
 * (spec 3.4 R-2); the reasons are logged.
 */
odin3_status raise_check_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                                bool *found);

/* The same run, changing the module; call raise_check_module on every module first. */
odin3_status raise_run_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                              bool *found);

#endif
