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
 * write_dot, write_rtlil), sim_pass.c and project_pass.c. */
extern const odin3_pass_def odin3_pass_write_json;
extern const odin3_pass_def odin3_pass_write_verilog;
extern const odin3_pass_def odin3_pass_write_dot;
extern const odin3_pass_def odin3_pass_write_rtlil;
extern const odin3_pass_def odin3_pass_sim;
extern const odin3_pass_def odin3_pass_read_project;
extern const odin3_pass_def odin3_pass_dump_project;
extern const odin3_pass_def odin3_pass_preprocess;

/* Writes text to path atomically (the destination keeps its old contents on failure); IO
 * (logged) when it cannot be written. */
odin3_status odin3_pass_write_text(const char *path, const odin3_strbuf *text);

#endif
