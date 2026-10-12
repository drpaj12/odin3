/*
 * elab.h — the elaboration entry read_verilog calls after a successful parse (2C §4.2 step 8,
 * plan point (l)). 2D owns it: until 2D lands, elab_stub.c implements it as a stub, and 2D
 * replaces that file with its own implementation of this exact declaration (2D plan point (a)).
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ELAB_H
#define ODIN3_FRONTENDS_VERILOG_ELAB_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

/* 2D's elaboration options (2D spec §2.2; defined in src/frontends/elab/elab.h by 2D). */
typedef struct odin3_elab_opts odin3_elab_opts;

/*
 * Elaborates the parsed store parsed (sealed, held by design: read_verilog hands it over with
 * odin3_design_set_ast before the call) into the design. opts are the read's elaboration options
 * (NULL: the defaults). Never destroys parsed: read_verilog drops it afterwards
 * (odin3_design_drop_ast, which keeps it under --keep-ast=all, AST-16).
 *
 * The 2C stub logs "elaboration is not available yet (2D)" and returns ODIN3_ERR_INVALID_ARG,
 * so no script silently passes before 2D.
 */
odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed,
                            const odin3_elab_opts *opts);

#endif
