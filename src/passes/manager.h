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
 * the pass name in the script (trimmed, not NUL-terminated; split it with odin3_pass_arg_next);
 * user is the definition's user pointer (odin3_pass_def.user). Returns ODIN3_OK or a failure
 * status, logging why (ODIN3_ERR_INVALID_ARG for bad arguments).
 */
typedef odin3_status (*odin3_pass_fn)(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user);

/*
 * One pass. name: non-empty, printable ASCII without blanks, ';' or '#' (a script word); help: one
 * line, its usage (shown by `odin3 --help`); run: the body; user: handed to run on every call
 * (NULL for the built-ins; a plugin pass's own definition for the ABI's plugin passes).
 */
typedef struct odin3_pass_def {
    const char *name;
    const char *help;
    odin3_pass_fn run;
    void *user;
} odin3_pass_def;

/*
 * Adds a pass to the process-global registry (plugins; the built-ins of builtin.c are always
 * present, ahead of every registered pass). Copies nothing: def and its strings must outlive the
 * process. ODIN3_ERR_INVALID_ARG (logged) for a NULL def or run, a bad name or help, or a name
 * already registered (built-in or not); ODIN3_ERR_NO_MEMORY on out of memory. Not thread-safe.
 */
odin3_status odin3_pass_register_def(const odin3_pass_def *def);

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
 * (odin3_pass_run_begin), checks the design (FULL, when checking is on; see odin3_pass_options;
 * this check reports errors only, so the warnings of the check after a pass show once per pass;
 * blind spots: a warning that first appears in this check, e.g. from IR changed outside a pass,
 * is counted in odin3_log_count but never delivered, and while it runs the process-wide log level
 * is lowered to ODIN3_LOG_ERROR, under any sink, then restored),
 * runs the pass, checks again (also after a failed pass: it must leave valid IR), and logs
 * "pass <name>: <ms> ms" at ODIN3_LOG_INFO (checks included). args must hold closed double quotes
 * (see odin3_pass_arg_next). Returns the pass's status when it fails ("pass <name>: failed:
 * <status>" logged; a failing check after it is logged too but the pass's status wins);
 * ODIN3_ERR_CHECK when the check before or after fails (the checker logs each violated rule as
 * "check: <module>: rule <n>: …", then "pass <name>: check before|after the pass failed" is
 * logged; a failing pre-check does not run the pass); ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * design or name, args {NULL, len > 0}, an unknown pass ("unknown pass '<name>'") or an
 * unterminated quote in args; ODIN3_ERR_NO_MEMORY (logged) when the run cannot be opened.
 */
odin3_status odin3_pass_run(odin3_design *design, const char *name, odin3_bytes args);

/* odin3_script_loc and odin3_script_src (where a script's errors are located) are public: see
 * odin3.h. */

/*
 * Runs a pass script on design. Commands are separated by ';' or a newline; '#' starts a comment
 * that runs to the end of the line; ';' and '#' inside double quotes are literal (a quote must
 * close on its line, else "<loc>: unterminated quote", ODIN3_ERR_PARSE). Blank commands are
 * skipped and not numbered: the others are commands 1, 2, … in order. A command is a pass name
 * followed by its arguments (the rest of the command, trimmed; split by odin3_pass_arg_next).
 * Every pass name is looked up before anything runs, so a typo late in a script costs nothing:
 * an unknown name is logged as "<loc>: unknown pass '<name>'" and gives ODIN3_ERR_PARSE. Then the
 * commands run in order through odin3_pass_run until one fails; its status is returned and
 * "<loc>: pass '<name>' failed: <status>" is logged. ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * design or origin, or text {NULL, len > 0}; ODIN3_ERR_NO_MEMORY on out of memory.
 */
odin3_status odin3_pass_run_script(odin3_design *design, odin3_bytes text, odin3_script_src src);

/* As odin3_pass_run_script without a design: splits the script and looks up every pass name
 * (same errors and statuses), running nothing. The CLI resolves every script before running any. */
odin3_status odin3_pass_resolve_script(odin3_bytes text, odin3_script_src src);

/*
 * Reads the script file at path and runs it (located by line, origin = path). ODIN3_ERR_IO
 * (logged as "path: cannot read …") when it cannot be read; otherwise as odin3_pass_run_script.
 */
odin3_status odin3_pass_run_script_file(odin3_design *design, const char *path);

/* Reads the script file at path and resolves it (odin3_pass_resolve_script), running nothing. */
odin3_status odin3_pass_resolve_script_file(const char *path);

/*
 * Splits pass arguments: skips blanks (space, tab, CR, LF) at the front of *rest, returns the next
 * word and advances *rest past it. A word is either a run of characters other than blanks and '"',
 * or a double-quoted string ("a b": blanks, ';' and '#' kept; no escapes) returned without its
 * quotes, which may be empty ("" gives a non-NULL ptr with len 0). An unclosed quote runs to the
 * end. No word left: ptr NULL (and len 0).
 */
odin3_bytes odin3_pass_arg_next(odin3_bytes *rest);

/* True when word equals the NUL-terminated string text. */
bool odin3_pass_arg_is(odin3_bytes word, const char *text);

#endif
