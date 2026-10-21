/*
 * builtin.h — the built-in pass table (private to src/passes: never part of the public ABI).
 */
#ifndef ODIN3_PASSES_BUILTIN_H
#define ODIN3_PASSES_BUILTIN_H

#include "passes/manager.h"
#include "util/str.h"

#include <stdint.h>

/* The built-in passes (builtin.c), in registry order; only the manager reads them. */
extern const odin3_pass_def *const odin3_builtin_passes[];
extern const uint32_t odin3_builtin_pass_count;

/* Built-in passes defined outside builtin.c: write_formats.c (write_json, write_verilog,
 * write_dot, write_rtlil), sim_pass.c, project_pass.c, proc/proc_pass.c (proc, 2E) and
 * read_verilog.c. */
extern const odin3_pass_def odin3_pass_write_json;
/* read_json_pass.c (5.3): read_json. */
extern const odin3_pass_def odin3_pass_read_json;
extern const odin3_pass_def odin3_pass_write_verilog;
extern const odin3_pass_def odin3_pass_write_dot;
extern const odin3_pass_def odin3_pass_write_rtlil;
extern const odin3_pass_def odin3_pass_sim;
/* arch_pass.c (Phase 4.1): read_arch and write_techlib. */
extern const odin3_pass_def odin3_pass_read_arch;
extern const odin3_pass_def odin3_pass_write_techlib;
extern const odin3_pass_def odin3_pass_read_project;
extern const odin3_pass_def odin3_pass_dump_project;
/* dump_locs.c (5.4): dump_locs. */
extern const odin3_pass_def odin3_pass_dump_locs;
/* src/passes/levelize/levelize.c (Phase 6, 6.4): levelize. */
extern const odin3_pass_def odin3_pass_levelize;
extern const odin3_pass_def odin3_pass_preprocess;
extern const odin3_pass_def odin3_pass_proc;
/* src/passes/opt/register.c (2F Task 4): the eight opt passes. */
extern const odin3_pass_def odin3_pass_opt;
extern const odin3_pass_def odin3_pass_opt_expr;
extern const odin3_pass_def odin3_pass_opt_merge;
extern const odin3_pass_def odin3_pass_opt_muxtree;
extern const odin3_pass_def odin3_pass_opt_reduce;
extern const odin3_pass_def odin3_pass_opt_share;
extern const odin3_pass_def odin3_pass_opt_dff;
extern const odin3_pass_def odin3_pass_opt_clean;
/* src/passes/abc/run.c (3D Task 14): the abc pass. */
extern const odin3_pass_def odin3_pass_abc;
extern const odin3_pass_def odin3_pass_wreduce;
/* src/passes/fsm/register.c (4.8): fsm_detect, and fsm. */
extern const odin3_pass_def odin3_pass_fsm_detect;
extern const odin3_pass_def odin3_pass_fsm;
/* src/passes/flatten/register.c (3A Task 2). */
extern const odin3_pass_def odin3_pass_flatten;
/* src/passes/lower/register.c (3B Task 1). */
extern const odin3_pass_def odin3_pass_lower;
/* src/passes/partial_map/register.c (3C Task 2; 4.2): `partial_map` and its alias `hardmap`. */
extern const odin3_pass_def odin3_pass_partial_map;
extern const odin3_pass_def odin3_pass_hardmap;
/* src/passes/partial_map/register_merge.c (4.7): `memory_merge`. */
extern const odin3_pass_def odin3_pass_memory_merge;
extern const odin3_pass_def odin3_pass_lpmmap;
/* src/frontends/verilog/read_verilog.c (2C §4). */
extern const odin3_pass_def odin3_read_verilog_pass;

/* Writes text to path atomically (the destination keeps its old contents on failure); IO
 * (logged) when it cannot be written. */
odin3_status odin3_pass_write_text(const char *path, const odin3_strbuf *text);

/*
 * The body of the `compact` pass (IR-6): every module renumbered densely (odin3_module_compact),
 * the held ASTs' symbol tables following the wire renumbering (EL-2). Used by the `compact` pass;
 * `abc` (3D) ends with it. `opt` never compacts (IR-6 unchanged, PHASE2 #19). Atomic per module
 * only: NO_MEMORY leaves the modules compacted so far compacted. Invalidates every module-local ID
 * held outside the IR.
 */
odin3_status odin3_design_compact(odin3_pass_ctx *ctx, odin3_design *design);

#endif
