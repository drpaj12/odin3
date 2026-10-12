/*
 * elab2_stub.c — stage 2's stand-in until Task 17 (plan point (l)): it reaches no IR and fails
 * with 2C's unlocated text, so a clean stage 1 still ends the read with ODIN3_ERR_PARSE. Task 17
 * deletes it.
 */
#include "frontends/elab/elab.h"

#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/log.h"

odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts) {
    (void)ctx;
    (void)elab;
    (void)opts;
    odin3_log(ODIN3_LOG_ERROR, "elaboration is not available yet (2D)");
    return ODIN3_ERR_PARSE;
}
