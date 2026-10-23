/*
 * odin3_slang.h — the SystemVerilog adapter's one exported C function (DESIGN.md §4.2, §12.1).
 *
 * adapters/slang builds libodin3_slang.so from C++20 sources, links the vendored slang
 * (third_party/slang), walks slang's elaborated AST (post-parameter, post-generate) and emits
 * the Odin III elaborated AST (ODIN3_AST_FORM_ELABORATED) through the odin3_ast_* builders.
 * The host registers the function with odin3_sv_reader_register (odin3.h, ABI v6); the core's
 * read_verilog pass drives it for .sv/.svh files. The core never links this library.
 *
 * The signature is odin3_sv_read_fn's exactly; see its paragraph in odin3.h for the contract.
 */
#ifndef ODIN3_SLANG_H
#define ODIN3_SLANG_H

#include "odin3/odin3.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads paths[0 .. npaths) as one SystemVerilog compilation, elaborates it with slang and writes
 * the Odin III elaborated AST into the store of pass run `run`, then runs stage 2 on it
 * (odin3_ast_elaborate). top is the top module name or NULL/"" for automatic selection (exactly
 * one uninstantiated module must then exist). opts are the read's elaboration options (nullable).
 *
 * Ownership: every IR object and string the call makes lives in design; the store is handed to
 * the design and never destroyed by the adapter. Failures: a source or elaboration error is a
 * located ODIN3_ERR_PARSE (the design is then fit only for destruction, as after a failed read);
 * ODIN3_ERR_INVALID_ARG for a NULL design/paths as odin3.h says; ODIN3_ERR_NO_MEMORY. Temporary
 * state (slang's compilation, the adapter's maps) is freed before returning, on every path.
 */
odin3_status odin3_read_slang(odin3_design *design, uint32_t run, const char *const *paths,
                              uint32_t npaths, const char *top, const odin3_elab_options *opts);

#ifdef __cplusplus
}
#endif

#endif
