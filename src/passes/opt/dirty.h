/*
 * dirty.h — the opt session and opt_expr's dirty set (2F spec §19, OP-12; plan Task 13).
 *
 * An opt session spans one `opt` invocation on a design. It keeps, per module, a mark per node:
 * the nodes whose input nets were connected, merged or re-driven since opt_expr's last call, and
 * their readers (the kit marks them as it rewrites). After the invocation's first opt_expr call
 * (which sees every node), opt_expr visits only the marked nodes, which is exact because every
 * opt_expr rule reads only a node's inputs, their drivers and the invert map (itself made of
 * drivers), never fanout. The session also remembers, per module, whether anything changed in the
 * current iteration (the cap's warning names those modules).
 *
 * One session at a time (the opt passes are single-threaded); sub-passes run outside a session
 * (standalone `opt_expr`, 2E's step F) see no marks and sweep everything.
 */
#ifndef ODIN3_PASSES_OPT_DIRTY_H
#define ODIN3_PASSES_OPT_DIRTY_H

#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"

#include <stdbool.h>

/*
 * Starts the session of design: every module is "all dirty" (opt_expr's first call sweeps it).
 * With incremental false the marks are not kept at all (opt_expr always sweeps: --opt-full-rescan)
 * but the per-module change flags still are. ODIN3_ERR_NO_MEMORY with no session started;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL design or a session already open.
 */
odin3_status odin3_opt_session_begin(odin3_design *design, bool incremental);

/* Ends the session and frees its state; a no-op when none is open. */
void odin3_opt_session_end(void);

/* True when the session keeps marks (incremental) and has a state for this module. */
bool odin3_opt_session_incremental(void);

/* Clears every module's "changed in this iteration" flag. */
void odin3_opt_session_new_iteration(void);

/* True when a kit rewrite touched the module since the last new_iteration. */
bool odin3_opt_session_module_changed(odin3_module_id module);

#endif
