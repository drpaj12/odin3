/*
 * proc_build.h — hand-built processes for the proc and opt unit tests (2E spec §18.1; plan Task 2,
 * ruling I5, point (s)). pb_build reads a subset of RTLIL, the same text Yosys 0.55 reads, so every
 * expectation can be re-derived with tools/proc-check/yosys-phase:
 *
 * - one or more `module \name ... end` blocks, created in text order;
 * - `wire [width N] [input K | output K] \name` (a port wire is a module port, created in text
 *   order); a wire whose name starts with $tmp makes unnamed nets with no wire (point (s));
 * - `cell $type \name` with `parameter \P <int | N'bits | "str">` and `connect \PORT <sig>`, then
 *   `end`; `cell \sub \u1` instantiates a module defined earlier in the text, by its port names;
 * - module `connect a b`: the nets of b merged into those of a, bit by bit (a constant side, on
 *   the module's shared constant net, is the one kept);
 * - `process` blocks: `assign`, `switch` (`attribute \full_case 1` and `\parallel_case 1` before it
 *   set its flags), `case` with comma-separated compares, `end`, `sync posedge|negedge|low|high
 *   <sig>`, `sync always` and `update`. Each becomes a working form, built here, then encoded
 *   with odin3_pw_encode (so its positions follow the text);
 * - `attribute \name <int | N'bits | "str">` before a module, wire, cell, process, switch, case or
 *   assign sets it on that object (on the $proc node for a process; switches, cases and assigns
 *   keep only their flags and records). `attribute \src "t.v:L.C"` instead gives the object a
 *   SOURCE record at file t.v, line L, column C (loc 0). Without one, a module, wire, net, cell
 *   or process gets the build's own SOURCE record (no location), and a case, switch or action
 *   none (0), as 2D Task 3's hand-packed bodies have.
 *
 * Sigspecs: `\w` (every bit), `\w [k]` (vector position k), `\w [hi:lo]`, `N'bits` MSB first with
 * 0 1 x z and `-` (any, compares only), and `{ ... }` (chunks MSB first). Test code: it may
 * allocate freely and reports the first error with its line through odin3_log.
 */
#ifndef ODIN3_TESTS_UNIT_PROC_BUILD_H
#define ODIN3_TESTS_UNIT_PROC_BUILD_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdint.h>

typedef struct pb_result {
    odin3_design *design;
    odin3_module *module; /* the last module of the text */
    odin3_pass_ctx ctx;   /* the build's pass run ("pb_build") */
} pb_result;

/*
 * Builds a new design from rtlil into *out (owned by the caller: pb_free, also after a failure,
 * which leaves *out empty). ODIN3_ERR_PARSE (logged "pb_build: line N: ...") for text outside the
 * subset, an unknown name or a width mismatch; the IR's status for an IR call that fails
 * (ODIN3_ERR_NO_MEMORY, ODIN3_ERR_INVALID_ARG).
 */
odin3_status pb_build(const char *rtlil, pb_result *out);

/* Destroys the design and empties *res; NULL is a no-op. */
void pb_free(pb_result *res);

/* The module named name, NULL when the design has none. */
odin3_module *pb_module(const pb_result *res, const char *name);

/* The nth live $proc node (from 0) of module `module` in node-ID order; none when absent. */
odin3_node_id pb_proc(const pb_result *res, const char *module, uint32_t nth);

/* Appends odin3_proc_print's text of node (indent 0) without its `process` line. */
odin3_status pb_print(const odin3_module *module, odin3_node_id node, odin3_strbuf *out);

/* Appends write_rtlil's text of the design (odin3_rtlil_text). */
odin3_status pb_rtlil(const pb_result *res, odin3_strbuf *out);

#endif
