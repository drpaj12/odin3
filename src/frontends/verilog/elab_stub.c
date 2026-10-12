/*
 * elab_stub.c — the stand-in for 2D's elaboration entry (elab.h, plan point (l)); 2D deletes this
 * file when it adds its own odin3_elab_run.
 */
#include "frontends/verilog/elab.h"

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/log.h"

odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed,
                            const odin3_elab_opts *opts) {
    (void)ctx;
    (void)design;
    (void)parsed;
    (void)opts;
    odin3_log(ODIN3_LOG_ERROR, "elaboration is not available yet (2D)");
    return ODIN3_ERR_INVALID_ARG;
}
