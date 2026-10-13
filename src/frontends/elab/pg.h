/*
 * pg.h — the process generator (2D Task 21; EL-33 emission, EL-34 syncs, EL-35 steps 1-7): it
 * follows Yosys 0.55's ProcessGenerator step for step and builds one `$proc` node (docs/IR.md
 * IR-19) per `always` block, off the IR in vectors (cases, switches, actions, compares and syncs,
 * whose signal bits are odin3_sigbit: a net, a constant 0/1/x/z or ODIN3_SB_ANY), then creates the
 * node whole in odin3_pg_emit (step 7). Task 22 added if/case (EL-36) and the case method for a
 * non-constant lvalue select (EL-32); Tasks 23-24 add loops, calls and memory writes to the same
 * builder.
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
 * non-blocking assignments, `begin ... end`, `if`, `case`, `casez`, `casex` and the null statement
 * are built; a construct that is pending (Tasks 23-24) is reported by point (l)'s line, once per
 * kind, and is ODIN3_ERR_PARSE. Statuses as odin3_pg_always. Only valid between odin3_pg_open and
 * odin3_pg_emit (odin3_pg_always does all three), for a statement of the body odin3_pg_open
 * scanned.
 */
odin3_status odin3_pg_stmt(odin3_pg *pg, odin3_ast_id stmt);

/*
 * odin3_pg_stmt for one IF (odin3_pg_if) or one CASE, CASEZ or CASEX (odin3_pg_case_stmt) statement
 * of the scanned body, which EL-36 builds as a switch: a constant condition or selector with
 * constant items keeps the taken branch alone (no switch); else the bits the branches assign by
 * `=` get case temporaries t1 (one group per chunk, v4.2), every case starts with `t1 = rv(bits)`
 * and runs with lv(bits) = t1 (lv and rv saved and restored around it), the cases are the items in
 * source order and the default item last, an added default case holds `t1 = rv(bits)` (x under
 * `full_case`) when the branches assign by `=` and no item is the default, and after the switch
 * rv(bits) = t1 and the current case gets `lv(bits) = t1`. The switch flags come from the
 * `full_case` and `parallel_case` attributes and metacomments, each with D1's warning. These are
 * odin3_pg_stmt with a kind check (the brief's `odin3_pg_case` is the Task 21 primitive below).
 * ODIN3_ERR_INVALID_ARG (logged) for another kind of statement or one the scan did not see.
 */
odin3_status odin3_pg_if(odin3_pg *pg, odin3_ast_id stmt);
odin3_status odin3_pg_case_stmt(odin3_pg *pg, odin3_ast_id stmt);

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
 * A target that is a select with a non-constant index is odin3_pg_select_write's. Errors of the
 * target (not a variable, a memory word, a non-constant index in a concatenation) are counted and
 * ODIN3_ERR_PARSE; a word of a register array with a non-constant index is the pending line.
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pg_assign(odin3_pg *pg, const odin3_pg_assign_req *req);

/*
 * The case method for a target that is a select with a non-constant index (EL-32 writes): `y[s] =
 * d`, `y[b +: w] <= d`. A position temporary p (as wide as the index) is assigned the index once,
 * in the root case, and a switch on p has one case per position the select can take
 * (odin3_elab2_varsel_make), each assigning the constant slice of d at that position, with no
 * default; for `=` the switch has the case temporaries of EL-36 (and so a default that holds them).
 * A position at which the slice straddles an end of the target has a case writing the bits inside
 * it. The target must be such a select alone (ODIN3_ERR_INVALID_ARG, logged, otherwise);
 * odin3_pg_assign calls this for one. rhs->width must be the select's width. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pg_select_write(odin3_pg *pg, const odin3_pg_assign_req *req);

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
 * The switch and case primitives of EL-36 (odin3_pg_if and odin3_pg_case_stmt build on them).
 * odin3_pg_switch opens a switch on `signal` in the current case (the current case does not change;
 * a case of the switch follows); odin3_pg_case opens a case of the newest switch of the current
 * case, with `n_compares` compares (each as wide as the switch's signal, ODIN3_SB_ANY allowed;
 * none: the default case), and makes it the current case, so assignments go into it and removals
 * reach it; odin3_pg_case_end makes the owner of the switch current again. stmt (a CASE, IF or
 * CASE_ITEM node, or none) gives the SOURCE record. ODIN3_ERR_INVALID_ARG (logged) for a case with
 * no switch to join or compares of the wrong width; ODIN3_ERR_NO_MEMORY.
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
 * Loops (EL-37, Task 23): odin3_pg_stmt for one FOR or REPEAT statement of the scanned body.
 * `for (i = a; c; i = s) body` is unrolled: a is evaluated (constants, and the values of variables
 * the generator holds as constants), i is bound to it for the AST constant test, c must be
 * constant ("for-loop condition is not constant (it reads 'n')"), and while it is true the body is
 * run and s evaluated; init and step must assign the same variable ("for-loop initialisation and
 * step must assign the same variable"), the body may not ("loop variable 'i' is assigned in the
 * loop body"). The variable is an output of the block only when the module reads it outside the
 * loops it is the variable of (EL-35, divergence 5); else only `rv` holds its values, the final
 * one after the loop. `repeat (n)` runs the body n times (a NUMBER after stage 1). A loop that
 * runs past ODIN3_ELAB_MAX_LOOP_ITERATIONS is `loop exceeds N iterations`, one past the run's
 * ODIN3_ELAB_MAX_TOTAL_ITERATIONS (counted on from stage 1's generate loops, point (k)) the fatal
 * `unrolled loops exceed N iterations in this design`, both located at the loop and
 * ODIN3_ERR_PARSE. ODIN3_ERR_INVALID_ARG (logged) for another kind of statement.
 */
odin3_status odin3_pg_for(odin3_pg *pg, odin3_ast_id stmt);
odin3_status odin3_pg_repeat(odin3_pg *pg, odin3_ast_id stmt);

/*
 * Inlines a user function CALL (result gets its value: the result variable's width, in the frame)
 * or a TASK_CALL (result may be NULL), EL-38: the actuals are generated in the caller's context,
 * a frame of unnamed wires holds the callee's ports, locals and result (process-local: `O` pins
 * with no sync action), the inputs are assigned to the formals by blocking assignment, the body
 * runs in this builder, and a task's output and inout formals are copied to their actuals (K9: an
 * actual that is no variable is `output argument N of task 't' must be a variable`). The
 * interpreter is tried first by the expression walker, so a constant call never gets here. The
 * errors of point (aa): `function 'f' has N inputs; M arguments given`, `recursive call of 'f'
 * cannot be inlined (its arguments are not constant)`, `function call nesting deeper than N at
 * 'f'` (fatal), `function 'f' can be called only with constant arguments (it uses 'while')`;
 * ODIN3_ERR_PARSE after one. ODIN3_ERR_INVALID_ARG (logged) for another node. Only valid inside a
 * block (between odin3_pg_open and odin3_pg_emit).
 */
odin3_status odin3_pg_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result);

/*
 * A user function CALL met in a continuous context (an assign right side, a port connection, a net
 * declaration assignment): a `$proc` of its own with one ALWAYS sync whose output is the result
 * variable's wire, generated whole (the block it needs is its own: no block may be open). result
 * gets the wire's nets. Statuses as odin3_pg_call. Outside the walker's context the arguments are
 * generated first, in the caller's.
 */
odin3_status odin3_pg_comb_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result);

/*
 * A word `m[s]` of a register array (EL-27, one dimension) with a variable index met outside a
 * process: a `$proc` of its own with one ALWAYS sync, a switch on the index with one case per
 * word the index can name (each assigning the word) and a default of x. result gets the nets.
 * ODIN3_ERR_INVALID_ARG (logged) for another select; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pg_comb_mem_read(odin3_pg *pg, odin3_ast_id select, odin3_sig *result);

/*
 * Starts a block without running it (steps 1-4): odin3_pg_always is open, odin3_pg_stmt of the
 * body and odin3_pg_emit. Exposed for the tasks that build blocks of their own (call frames,
 * mem2reg reads outside a process). Statuses as odin3_pg_always.
 */
odin3_status odin3_pg_open(odin3_pg *pg, odin3_ast_id always_node);

#endif
