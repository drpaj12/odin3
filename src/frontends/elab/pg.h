/*
 * pg.h — the process generator (2D Task 21; EL-33 emission, EL-34 syncs, EL-35 steps 1-7): it
 * follows Yosys 0.55's ProcessGenerator step for step and builds one `$proc` node (docs/IR.md
 * IR-19) per `always` block, off the IR in vectors (cases, switches, actions, compares and syncs,
 * whose signal bits are odin3_sigbit: a net, a constant 0/1/x/z or ODIN3_SB_ANY), then creates the
 * node whole in odin3_pg_emit (step 7). Tasks 22-24 add if/case, loops, calls and memory writes to
 * the same builder.
 *
 * One odin3_pg serves a whole module (elab2.c makes it on the first block); everything it keeps
 * per block is reset by the next odin3_pg_always. No recursion: statements run through an explicit
 * frame stack.
 */
#ifndef ODIN3_FRONTENDS_ELAB_PG_H
#define ODIN3_FRONTENDS_ELAB_PG_H

#include "frontends/elab/elab_internal.h"

#include "ast/ast.h"
#include "ir/ids.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_pg odin3_pg;

/*
 * Makes a generator for the module e2 is emitting (EL-28 step (6)). *out is NULL on failure.
 * ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or no module being
 * emitted. Free it with odin3_pg_destroy (NULL is a no-op); it does not own e2.
 */
odin3_status odin3_pg_create(odin3_elab2 *e2, odin3_pg **out);
void odin3_pg_destroy(odin3_pg *pg);

/*
 * Builds the `$proc` node of one `always` block of the module (EL-35): the outputs and their
 * temporaries (steps 1-2), the root defaults (3), the syncs (EL-34; step 4), the statements (5, 6)
 * and the emission (7). A block with no outputs and no temporaries creates no node, silently
 * (`always @* ;`). *out is not written. The block's own located errors (an event on a multi-bit
 * signal, a net that cannot be assigned, a construct that is pending) are counted and
 * ODIN3_ERR_PARSE, with no node made; the warnings are the net-in-process warning of Task 19's
 * carry and T3 (EL-34). ODIN3_ERR_NO_MEMORY leaves the IR valid (unused nets may remain).
 * ODIN3_ERR_INVALID_ARG (logged) for a node that is not an ALWAYS.
 */
odin3_status odin3_pg_always(odin3_pg *pg, odin3_ast_id always_node);

/*
 * Runs one statement of the block being built through the frame stack (EL-35 step 5): blocking and
 * non-blocking assignments, `begin ... end` and the null statement are built; a construct that is
 * pending (Tasks 22-24) is reported by point (l)'s line, once per kind, and is ODIN3_ERR_PARSE.
 * Statuses as odin3_pg_always. Only valid between odin3_pg_open and odin3_pg_emit (odin3_pg_always
 * does all three).
 */
odin3_status odin3_pg_stmt(odin3_pg *pg, odin3_ast_id stmt);

/*
 * One assignment to build (EL-35 step 5): stmt is the statement (provenance, messages), lhs the
 * target node (an identifier, a constant select, a word of a register array, or a concatenation of
 * those), rhs the value, as wide as the target and already generated (bit k of rhs is bit k of the
 * target, LSB first), and blocking whether `=` (the value is visible to later reads) or `<=`.
 */
typedef struct odin3_pg_assign_req {
    odin3_ast_id stmt, lhs;
    const odin3_sig *rhs;
    bool blocking;
} odin3_pg_assign_req;

/*
 * Applies one assignment to the block being built (EL-35 step 5, v4.2 chunks): the target is taken
 * apart (a bit written twice in one target keeps its first occurrence, a bit outside its net is
 * dropped); for `=`, `rv(bit)` becomes the value bit; the target bits' earlier actions in the
 * current case and the switches nested in it are removed (the slot index, EL-35 step 6); and the
 * action `lv(bit) = value` is appended to the current case as one action per run, in the target's
 * lvalue order (the leftmost part of a concatenation first), of consecutive bits of one temporary
 * group. rhs->width must equal the target's width (ODIN3_ERR_INVALID_ARG, logged, otherwise).
 * Errors of the target (not a variable, a variable index, a memory word) are counted and
 * ODIN3_ERR_PARSE. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pg_assign(odin3_pg *pg, const odin3_pg_assign_req *req);

/*
 * The undo log of the `lv` and `rv` maps: odin3_pg_save returns a mark, odin3_pg_restore(mark)
 * reverts every change made since, in O(changes) (a branch saves and restores around itself).
 * A mark from before a restore to an earlier one is invalid (not checked).
 */
uint32_t odin3_pg_save(const odin3_pg *pg);
void odin3_pg_restore(odin3_pg *pg, uint32_t mark);

/*
 * Makes `width` fresh unnamed nets (consecutive net IDs from *first) that only this process
 * drives (EL-35: case temporaries, call frames, select positions, memory write variables), as one
 * temporary group: an assignment splits its actions at a group's end. why is the statement that
 * needed them (provenance). The net-bits cap of point (aa) is checked first (fatal,
 * ODIN3_ERR_PARSE). ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG (logged) for a width of 0.
 */
odin3_status odin3_pg_temp(odin3_pg *pg, uint32_t width, odin3_ast_id why, odin3_net_id *first);

/*
 * The switch and case primitives of EL-36 (Task 22 builds if/case on them). odin3_pg_switch opens a
 * switch on `signal` in the current case (the current case does not change; a case of the switch
 * follows); odin3_pg_case opens a case of the newest switch of the current case, with `n_compares`
 * compares (each as wide as the switch's signal, ODIN3_SB_ANY allowed; none: the default case), and
 * makes it the current case, so assignments go into it and removals reach it; odin3_pg_case_end
 * makes the owner of the switch current again. stmt (a CASE, IF or CASE_ITEM node, or none) gives
 * the SOURCE record. ODIN3_ERR_INVALID_ARG (logged) for a case with no switch to join or compares
 * of the wrong width; ODIN3_ERR_NO_MEMORY.
 */
typedef struct odin3_pg_switch_req {
    odin3_ast_id stmt;
    const odin3_sig *signal;
    uint16_t flags; /* ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE */
} odin3_pg_switch_req;

typedef struct odin3_pg_case_req {
    odin3_ast_id stmt;
    const odin3_sig *compares;
    uint32_t n_compares;
} odin3_pg_case_req;

odin3_status odin3_pg_switch(odin3_pg *pg, const odin3_pg_switch_req *req);
odin3_status odin3_pg_case(odin3_pg *pg, const odin3_pg_case_req *req);
void odin3_pg_case_end(odin3_pg *pg);

/*
 * Step 7 for the block being built: renumbers the cases, switches and actions breadth first into
 * IR-19's canonical layout; every net some action assigns is an `O` pin and every other net the
 * body reads an `I` pin, each constant as an `I` pin on the module's shared constant net, both
 * lists in net-ID order; the driver table is claimed for every output (odin3_elab2_lvalue_claim,
 * kind process; a conflict is the claim's located error and the actions of the conflicting
 * outputs are dropped); then one odin3_node_create_connected makes the node, whose SOURCE record is
 * that of prov_node and which takes the `(* *)` attributes of prov_node (ODIN3_OBJ_NODE). *out
 * gets the node, or none (ID 0) when nothing was left to drive. The pins cap of point (aa) is
 * checked first (fatal, ODIN3_ERR_PARSE). ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a body
 * the IR refuses (a 2D bug, logged).
 */
odin3_status odin3_pg_emit(odin3_pg *pg, odin3_ast_id prov_node, odin3_node_id *out);

/*
 * Starts a block without running it (steps 1-4): odin3_pg_always is open, odin3_pg_stmt of the
 * body and odin3_pg_emit. Exposed for the tasks that build blocks of their own (call frames,
 * mem2reg reads outside a process). Statuses as odin3_pg_always.
 */
odin3_status odin3_pg_open(odin3_pg *pg, odin3_ast_id always_node);

#endif
