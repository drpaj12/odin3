/* stub */
#include "passes/builtin.h"
static odin3_status submod_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)ctx; (void)design; (void)args; (void)user;
    return ODIN3_ERR_INVALID_ARG;
}
const odin3_pass_def odin3_pass_submod = {"submod", "submod [-attr KEY] [-name N]", submod_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};
