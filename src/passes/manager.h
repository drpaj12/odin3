/*
 * manager.h — the pass manager: process-global pass registry, pass runs, CLI scripts.
 */
#ifndef ODIN3_PASSES_MANAGER_H
#define ODIN3_PASSES_MANAGER_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A pass body. ctx is the pass run the manager opened for this call (named after the pass; ctx->op
 * is 0 until the pass begins an operation, IR-13); design is ctx->design; args is the text after
 * the pass name in the script (trimmed, not NUL-terminated; split it with odin3_pass_arg_next).
 * Returns ODIN3_OK or a failure status, logging why (ODIN3_ERR_INVALID_ARG for bad arguments).
 */
typedef odin3_status (*odin3_pass_fn)(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args);

/*
 * One pass. name: non-empty, printable ASCII without blanks, ';' or '#' (a script word); help: one
 * line, its usage (shown by `odin3 --help`).
 */
typedef struct odin3_pass_def {
    const char *name;
    const char *help;
    odin3_pass_fn run;
} odin3_pass_def;

/*
 * Adds a pass to the process-global registry (plugins; the built-ins of builtin.c are always
 * present, ahead of every registered pass). Copies nothing: def and its strings must outlive the
 * process. ODIN3_ERR_INVALID_ARG (logged) for a NULL def or run, a bad name or help, or a name
 * already registered (built-in or not); ODIN3_ERR_NO_MEMORY on out of memory. Not thread-safe.
 */
odin3_status odin3_pass_register(const odin3_pass_def *def);

/* The pass named name (built-ins first, then registration order), NULL when there is none. */
const odin3_pass_def *odin3_pass_find(odin3_bytes name);

/* Number of passes (built-ins included) and pass `index` in that order; NULL out of range. */
uint32_t odin3_pass_count(void);
const odin3_pass_def *odin3_pass_at(uint32_t index);

/*
 * Process-level options the CLI sets. check: run odin3_check_design FULL before and after every
 * pass (always done in Debug builds, whatever this says; `--check` in Release). top: the requested
 * top module name (`--top`), NULL for none; caller-owned, must outlive every pass run; `read_blif`
 * applies it after reading and `hierarchy` uses it when given no `--top` (DESIGN §4.0: --top wins).
 */
typedef struct odin3_pass_options {
    bool check;
    const char *top;
} odin3_pass_options;

void odin3_pass_set_options(odin3_pass_options opts);
odin3_pass_options odin3_pass_get_options(void);

/*
 * Runs the pass named name on design: opens a provenance pass run named after the pass
 * (odin3_pass_run_begin), checks the design (FULL, when checking is on; see odin3_pass_options),
 * runs the pass, checks again, and logs "pass <name>: <ms> ms" at ODIN3_LOG_INFO.
 * Returns the pass's status when it fails ("pass <name>: failed: <status>" logged, no post-check);
 * ODIN3_ERR_CHECK when the check before or after fails (the checker logs each violated rule as
 * "check: <module>: rule <n>: …", then "pass <name>: check before|after the pass failed" is
 * logged; a failing pre-check does not run the pass); ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * design or name, or an unknown pass ("unknown pass '<name>'"); ODIN3_ERR_NO_MEMORY when the run
 * cannot be opened.
 */
odin3_status odin3_pass_run(odin3_design *design, const char *name, odin3_bytes args);

/* How script errors are located: "origin:line: …" (a file) or "origin: command <k>: …" (-p). */
typedef enum odin3_script_loc { ODIN3_SCRIPT_BY_LINE, ODIN3_SCRIPT_BY_COMMAND } odin3_script_loc;

typedef struct odin3_script_src {
    const char *origin; /* the script's path, or "-p" */
    odin3_script_loc loc;
} odin3_script_src;

/*
 * Runs a pass script on design. Commands are separated by ';' or a newline; '#' starts a comment
 * that runs to the end of the line; blank commands are skipped. A command is a pass name followed
 * by its arguments (the rest of the command, trimmed). Commands are numbered from 1 in order.
 * Every pass name is looked up before anything runs, so a typo late in a script costs nothing:
 * an unknown name is logged as "<loc>: unknown pass '<name>'" and gives ODIN3_ERR_PARSE. Then the
 * commands run in order through odin3_pass_run until one fails; its status is returned and
 * "<loc>: pass '<name>' failed: <status>" is logged. ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * design or origin; ODIN3_ERR_NO_MEMORY on out of memory.
 */
odin3_status odin3_pass_run_script(odin3_design *design, odin3_bytes text, odin3_script_src src);

/*
 * Reads the script file at path and runs it (located by line, origin = path). ODIN3_ERR_IO
 * (logged as "path: cannot read …") when it cannot be read; otherwise as odin3_pass_run_script.
 */
odin3_status odin3_pass_run_script_file(odin3_design *design, const char *path);

/*
 * Splits pass arguments: skips blanks (space, tab, CR, LF) at the front of *rest, returns the next
 * word and advances *rest past it; {NULL, 0} (len 0) when no word is left.
 */
odin3_bytes odin3_pass_arg_next(odin3_bytes *rest);

/* True when word equals the NUL-terminated string text. */
bool odin3_pass_arg_is(odin3_bytes word, const char *text);

/* The built-in passes (builtin.c), in registry order; only the manager reads them. */
extern const odin3_pass_def *const odin3_builtin_passes[];
extern const uint32_t odin3_builtin_pass_count;

#endif
