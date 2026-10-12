/*
 * elab.h — the elaboration entry read_verilog calls after a successful parse (2C §4.2 step 8;
 * 2D spec §2.1 EL-2, plan point (a)). Implemented in elab1.c.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ELAB_H
#define ODIN3_FRONTENDS_VERILOG_ELAB_H

#include "frontends/elab/elab.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

/*
 * Elaborates the parsed store parsed (sealed, of ctx's run) into design, EL-2 steps 1–7: (1) the
 * options: *opts (NULL: the defaults; read_verilog passes --nolatches, --nomem2reg and
 * --auto-blackbox, and top 0) with the top filled from --top, else the project record
 * (odin3_elab1_options); (2) odin3_elab_stage1, and in Debug or under --check
 * odin3_ast_check(FULL) of the elaborated store (a violation is ODIN3_ERR_CHECK, a 2D bug);
 * (3) odin3_design_drop_ast(design, run, PARSED); (4) odin3_elab_stage2; (5)–(7) from Task 17
 * (the top, the summary line); (6) the elaborated store is handed to the design, which keeps it
 * under --keep-ast (ELABORATED or ALL) and otherwise destroys it at once.
 *
 * Ownership (review I10): read_verilog hands parsed to the design (odin3_design_set_ast) before
 * the call. This function never destroys parsed itself: step (3) asks the design to drop it, which
 * destroys it unless the keep level is ALL, so it is gone before stage 2; read_verilog's own
 * drop afterwards is a no-op. A store the design does not hold is left to its caller.
 *
 * Returns ODIN3_OK, or ODIN3_ERR_PARSE after the located diagnostics (until Task 17, stage 2's
 * stub always: "elaboration is not available yet (2D)"), ODIN3_ERR_NO_MEMORY, ODIN3_ERR_CHECK, or
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL ctx, design or parsed.
 */
odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed,
                            const odin3_elab_opts *opts);

#endif
