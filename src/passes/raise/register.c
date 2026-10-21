/*
 * register.c — the `raise` pass definition (Phase 7 spec 3.4): option parsing (the options of
 * cone_check without -timeout) and the loop over the modules; a group that no module has is an
 * error.
 */
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/raise/raise_int.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char k_usage[] =
    "raise -attr KEY=V -cell TYPE [-param P=V ...] (trusts the cone_check result in "
    "the module attributes: run cone_check in this session first)";

static odin3_status raise_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)user;
    cc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.arena = odin3_arena_create(0);
    opts.who = "raise";
    opts.usage = k_usage;
    odin3_vec_init(&opts.params, sizeof(cc_param));
    odin3_status st = opts.arena != NULL ? cc_parse_args(args, &opts) : ODIN3_ERR_NO_MEMORY;
    bool any = false;
    uint32_t end = odin3_design_module_end(design);
    /* all-or-nothing: every module is checked before the first one changes */
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        bool found = false;
        st = raise_check_module(ctx, odin3_module_get(design, (odin3_module_id){mid}), &opts,
                                &found);
    }
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        bool found = false;
        st = raise_run_module(ctx, odin3_module_get(design, (odin3_module_id){mid}), &opts, &found);
        any = any || found;
    }
    if (st == ODIN3_OK && !any) {
        odin3_log(ODIN3_LOG_ERROR,
                  "raise: group '%s=%s' is empty: no live node of any module has it", opts.key,
                  opts.value);
        st = ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec_free(&opts.params);
    odin3_arena_destroy(opts.arena);
    return st;
}

const odin3_pass_def odin3_pass_raise = {"raise", k_usage, raise_pass_run,
                                         NULL,    false,   ODIN3_PASS_PROCESSES_REFUSE};
