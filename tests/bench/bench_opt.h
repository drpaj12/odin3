/*
 * bench_opt.h — the measuring core of bench_opt (2F plan Task 15, spec section 19), shared with the
 * gate test test_bench_opt_columns. Test and bench code, never part of the library.
 */
#ifndef ODIN3_TESTS_BENCH_BENCH_OPT_H
#define ODIN3_TESTS_BENCH_BENCH_OPT_H

#include "odin3/odin3.h"

#include <stddef.h>

/* The longest line bench_opt_design writes (a NUL included). */
enum { BENCH_OPT_LINE = 4096 };

/*
 * Runs the Phase 2 script's opt passes on a design that is already read, in the script's order
 * (tools/opt-check/phase2.script): `opt_expr; opt_clean; opt -nodffe -nosdff`, then `proc`
 * (untimed), then `opt`, then `compact` (timed apart); then `opt -full` on the result. Times each
 * sub-pass call through opt_test.h's hooks and writes one line of key=value columns (no newline)
 * named `name` to buf. ODIN3_OK, or the first failing pass's status (logged; the design is then
 * unusable); ODIN3_ERR_INVALID_ARG when buf is too small.
 */
odin3_status bench_opt_design(odin3_design *design, const char *name, char *buf, size_t cap);

/* Reads one Verilog file (read_verilog --nolatches, untimed) into a fresh design and measures it.
 */
odin3_status bench_opt_one(const char *path, char *buf, size_t cap);

#endif
