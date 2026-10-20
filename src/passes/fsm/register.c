/*
 * register.c — the pass definitions of the fsm family (spec F-2): `fsm_detect` and (Task 10)
 * `fsm`, with their option parsing. Both are processes = REFUSE: proc must have run.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/fsm/fsm.h"
#include "passes/manager.h"
#include "util/log.h"

#include <stdbool.h>
#include <string.h>

static odin3_status detect_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)ctx;
    (void)user;
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (!odin3_pass_arg_is(word, "-ignore-self-reset")) {
            odin3_log(ODIN3_LOG_ERROR, "fsm_detect: unknown argument '%.*s'", (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        opts.ignore_self_reset = true;
    }
    return odin3_fsm_detect_run(design, &opts);
}

const odin3_pass_def odin3_pass_fsm_detect = {
    "fsm_detect",
    "fsm_detect [-ignore-self-reset]: mark the FSM state registers (fsm_encoding = \"auto\"); "
    "Yosys fsm_detect",
    detect_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
