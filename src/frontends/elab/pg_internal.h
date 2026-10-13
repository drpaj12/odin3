/*
 * pg_internal.h — what pg.c (the builder and assignments), pg_always.c (the block: outputs,
 * syncs, statements) and pg_emit.c (step 7) share. Private to the three files and to the white-box
 * test of the canonical layout (test_elab_stage2_proc.c).
 *
 * The builder holds one block off the IR. A signal bit is an odin3_sigbit. Every list of cases,
 * switches and actions is a linked list of 1-based indices (0: none) over a vector, so appending
 * is O(1) and the breadth-first renumbering of step 7 is one pass. Spans (odin3_pspan) index the
 * pool of signal bits; an action's two spans have one length and `dead` marks the lhs positions
 * a later assignment removed (EL-35 step 6), which step 7 drops with the right-side bit paired
 * with them.
 */
#ifndef ODIN3_FRONTENDS_ELAB_PG_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_PG_INTERNAL_H

#include "frontends/elab/pg.h"

#include "ir/proc.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

enum { PG_ROOT = 0 };            /* the root case */
#define PG_SYNC_OWNER UINT32_MAX /* an action owned by a sync, not a case */

/* One output bit (EL-35 step 1): the variable's net, its `t0` temporary and where it sits. */
typedef struct pg_out {
    odin3_net_id net, t0;
    odin3_wirebit wb;  /* the wire bit the lvalue named (claims, chunks); wire 0: none */
    odin3_ast_id node; /* the lvalue piece that first wrote it (the place of a conflict) */
    bool whole;        /* the piece names the whole wire */
    uint32_t chunk;    /* index of its chunk in the chunk list */
} pg_out;

/* A maximal run of consecutive bits of one variable in the sorted outputs (v4.2). */
typedef struct pg_chunk {
    uint32_t first, len;
} pg_chunk;

typedef struct pg_act {
    odin3_pspan lhs, rhs; /* equal lengths, into the pool */
    odin3_prov_id prov;
    uint32_t next; /* the next action of the same case or sync (1-based; 0: end) */
    uint32_t cs;   /* the owning case, or PG_SYNC_OWNER */
} pg_act;

typedef struct pg_case {
    uint32_t parent;              /* the case that owns this case's switch (the root: 0) */
    uint32_t first_act, last_act; /* 1-based, 0: none */
    uint32_t first_sw, last_sw;   /* the switches opened in this case */
    uint32_t next;                /* the next case of the same switch */
    uint32_t cmp_first, cmp_n;    /* compares[] (spans) of this case */
    odin3_prov_id prov;
} pg_case;

typedef struct pg_switch {
    odin3_pspan signal;
    uint32_t first_case, last_case; /* 1-based, 0: none */
    uint32_t next;                  /* the next switch of the same case */
    uint16_t flags;
    odin3_prov_id prov;
} pg_switch;

typedef struct pg_sync {
    uint8_t type; /* odin3_sync_type */
    odin3_sigbit signal;
    uint32_t first_act, last_act;
} pg_sync;

/* An action slot of a target bit (EL-35 step 6): where the bit is assigned. */
typedef struct pg_slot {
    uint32_t pos; /* the position in the pool (the lhs span's) */
    uint32_t act; /* the action, 0-based */
    uint32_t next;
} pg_slot;

enum { PG_UNDO_LV = 0, PG_UNDO_RV = 1 };

typedef struct pg_undo {
    uint8_t which;
    bool had;
    uint64_t key, old;
} pg_undo;

enum { PG_F_STMT = 1, PG_F_BLOCK = 2, PG_F_SWITCH = 3, PG_F_LOOP = 4 };

/* One frame of the statement stack: a lone statement, a `begin ... end` and the next child, or an
 * `if`/`case` being built (PG_F_SWITCH: next..end are the item children still to open, dflt the
 * default item's child index or 0, aux the pg_case.c state bits). */
typedef struct pg_frame {
    uint8_t kind;
    odin3_ast_id node;
    uint32_t next, end, dflt, aux;
} pg_frame;

/* A loop being unrolled (Task 23, EL-37): its node, its variable's symbol (0 for `repeat`), the
 * iterations done and (repeat) still to do. The PG_F_LOOP frame's aux is its index. */
typedef struct pg_loop {
    odin3_ast_id node;
    odin3_sym_id sym;
    uint64_t count;
    int64_t remaining;
} pg_loop;

/* A call being inlined (EL-38): the declaration, the call, the call the generator inlined before
 * it (the provenance's second loc) and where its symbols begin in pg->fsyms. */
typedef struct pg_callrec {
    odin3_ast_id decl, call, saved_call;
    uint32_t fsyms_mark;
} pg_callrec;

/* The wildcard rule of a case statement (EL-36). */
enum { PG_WILD_NONE = 0, PG_WILD_Z = 1, PG_WILD_X = 2 };

/* A switch being built (a stack parallel to the SWITCH frames, and the one of a select write): its
 * targets (the bits its branches assign by `=`, in pg->sw_nets), their case temporaries t1 (sw_t1)
 * and the values they have at its start (sw_rv), first .. first + n of each, and the case being
 * opened. */
typedef struct pg_swctx {
    odin3_ast_id node;
    uint32_t first, n;
    uint32_t width; /* the case width */
    uint32_t mark;  /* the undo mark of the open case */
    bool is_signed, full_case;
    uint8_t wild;
} pg_swctx;

/* The result of folding a statement: none, or the child to run + 1 (PG_FOLD_NONE: nothing runs). */
#define PG_FOLD_NONE UINT32_MAX

struct odin3_pg {
    odin3_elab2 *e2;
    /* the block */
    odin3_ast_id always;
    odin3_prov_id prov;
    bool edge;                  /* some sync is an edge: latches allowed, hold actions kept */
    bool track_reads;           /* a level event list: T3 records the reads */
    bool failed;                /* a located error: no node */
    odin3_ast_id control, body; /* the event control and the statement of the ALWAYS */
    /* outputs and temporaries */
    odin3_vec outs;       /* pg_out, sorted by net ID after pg_outputs */
    odin3_vec chunks;     /* pg_chunk */
    odin3_u64map *out_of; /* net -> out index + 1 */
    odin3_u64map *group;  /* a temporary net -> (group << 32 | position in the group) */
    uint32_t groups;      /* groups made so far (ids from 1) */
    /* the maps and their undo log */
    odin3_u64map *lv, *rv;
    odin3_vec undo; /* pg_undo */
    /* the builder */
    odin3_vec pool;          /* odin3_sigbit */
    odin3_vec dead;          /* uint8_t, parallel to pool: the lhs position was removed */
    odin3_vec cmps;          /* odin3_pspan */
    odin3_vec acts;          /* pg_act */
    odin3_vec cases;         /* pg_case; [0] is the root */
    odin3_vec sws;           /* pg_switch */
    odin3_vec syncs;         /* pg_sync */
    odin3_vec slots;         /* pg_slot */
    odin3_u64map *slot_head; /* net -> index + 1 of its newest slot */
    odin3_u64map *dead_nets; /* nets whose actions step 7 drops (a conflicting output and its t0) */
    uint32_t cur;            /* the current case */
    odin3_vec frames;        /* pg_frame */
    /* switches (Task 22) */
    odin3_u64map *swl;   /* if/case node -> (first in sw_pool << 32) | count: its targets */
    odin3_u64map *folds; /* if/case node -> the child to run + 1 (PG_FOLD_NONE: none) */
    odin3_vec blk;       /* uint32_t: nets assigned by `=` so far in the open switches (scan) */
    odin3_vec blk_marks; /* uint32_t: where each open switch's nets begin in blk */
    odin3_vec sw_pool;   /* uint32_t: the targets of every switch, sorted and unique */
    odin3_vec sw_ctx;    /* pg_swctx */
    odin3_vec sw_nets;   /* odin3_sigbit: targets of the switches being built */
    odin3_vec sw_t1;     /* odin3_sigbit: their case temporaries */
    odin3_vec sw_rv;     /* odin3_sigbit: their values at the switch's start */
    odin3_vec cmp_sigs;  /* odin3_sig: the compares of the items of the open cases not yet opened */
    odin3_elab2_reads hooks; /* the walker's hooks over this generator (pg_reads) */
    odin3_elab2_lvalue slv;  /* a synthetic target: a switch's exit, a select write's case */
    /* loops and calls (Task 23) */
    odin3_vec loops;       /* pg_loop, innermost last */
    odin3_vec calls;       /* pg_callrec, innermost last */
    odin3_vec fsyms;       /* uint32_t: the symbols whose ir_wire a call frame set */
    odin3_u64map *tvis;    /* TASK_DECL node -> 1: its body is scanned (this scan) */
    odin3_u64map *active;  /* FUNCTION_DECL / TASK_DECL node -> 1: being inlined */
    odin3_u64map *loopout; /* symbol -> 1: read outside the loops it is the variable of */
    bool loopout_ready;    /* loopout is built for the module */
    bool scan_frame;       /* the scan is of a call's body: no new outputs */
    bool scan_local;       /* the target being scanned names a callee's variable in an index */
    bool scanning;         /* pg_scan is under way: an index with a call in it is not generated */
    /* T3 */
    odin3_u64map *reads;  /* net -> 1: read, not supplied by an assignment of the block */
    odin3_u64map *listed; /* net -> 1: a bit of a signal the level list names */
    /* scratch of one assignment */
    odin3_elab2_lvalue lvs;
    odin3_vec work;         /* odin3_ast_id: a worklist */
    odin3_vec items;        /* uint32_t: kept bits of the assignment being applied */
    odin3_vec runs;         /* uint32_t: the first item of each run, then the end */
    odin3_vec run_l, run_r; /* odin3_sigbit: the two sides of the action being made */
    odin3_u64map *seen;
    uint32_t serial;
    /* the module: T9's record of how each bit was first assigned (kept across blocks) */
    odin3_u64map *kinds; /* net -> (1 + blocking) << 32 | the statement */
};

/* --- pg.c: the builder ------------------------------------------------------------------------ */

/* Forgets the block (the maps, the vectors, the nets) and makes the root case. */
odin3_status pg_block_reset(odin3_pg *pg);

bool pg_lv_get(const odin3_pg *pg, odin3_net_id net, odin3_net_id *out);
odin3_status pg_lv_set(odin3_pg *pg, odin3_net_id net, odin3_net_id target);
bool pg_rv_get(const odin3_pg *pg, odin3_net_id net, odin3_sigbit *out);
odin3_status pg_rv_set(odin3_pg *pg, odin3_net_id net, odin3_sigbit value);

/* Takes the lvalue lhs apart into pg->lvs (cleared first; a non-constant select fills lvs.var). */
odin3_status pg_take_target_of(odin3_pg *pg, odin3_ast_id lhs);

/* Appends the bits to the pool; *span is where they went. */
odin3_status pg_pool_add(odin3_pg *pg, const odin3_sigbit *bits, uint32_t n, odin3_pspan *span);

/* An action lhs = rhs (n bits each) at the end of case cs (or of the sync, with owner PG_SYNC_OWNER
 * and the sync index in sync); its lhs nets are indexed as slots when cs is a case. */
typedef struct pg_act_req {
    uint32_t cs;
    uint32_t sync;
    const odin3_sigbit *lhs, *rhs;
    uint32_t n;
    odin3_prov_id prov;
} pg_act_req;
odin3_status pg_act_add(odin3_pg *pg, const pg_act_req *req);

/* Removes bit `net` as a target from the current case and the switches nested in it. */
odin3_status pg_remove(odin3_pg *pg, odin3_net_id net);

/* The assignment of odin3_pg_assign over a target that is already taken apart (pg->lvs for the
 * statement walk). */
typedef struct pg_asg {
    const odin3_elab2_lvalue *lv;
    const odin3_sig *rhs;
    odin3_ast_id stmt;
    bool blocking;
    bool raw; /* a synthetic assignment (a switch's exit, a select's case): T9 was checked */
} pg_asg;
odin3_status pg_apply(odin3_pg *pg, const pg_asg *asg);

/* T9 (EL-41) for the target bits of asg: a bit first assigned with the other of `=` and `<=`. */
odin3_status pg_check_mixed(odin3_pg *pg, const pg_asg *asg);

/* A temporary net's group and its position in it. */
typedef struct pg_grp {
    uint32_t group, pos;
} pg_grp;

/* The group of a temporary net; false for a net that is not one. */
bool pg_group_of(const odin3_pg *pg, odin3_net_id net, pg_grp *out);

/* Whether target (a temporary) follows prev in one group: they share an action. */
bool pg_run_continues(const odin3_pg *pg, odin3_net_id prev, odin3_net_id target);

/* --- pg_always.c ------------------------------------------------------------------------------ */

/* The read hook of the expression walker (odin3_elab2_reads) over rv. */
odin3_status pg_read_bit(void *user, odin3_net_id net, odin3_sigbit *out);

/* Steps 2 to 4 of a block whose outputs are in pg->outs: their temporaries, chunks and syncs, and
 * the root defaults. */
odin3_status pg_start_block(odin3_pg *pg);

/* Runs one statement through the frame stack, on top of the frames already there (a call inlined
 * by an expression runs its body this way). */
odin3_status pg_run(odin3_pg *pg, odin3_ast_id stmt);

/* `lhs = rhs` / `lhs <= rhs` of an assignment statement (exec_assign). */
odin3_status pg_exec_assign(odin3_pg *pg, odin3_ast_id stmt);

/* Pushes a frame (a statement to run, a block, a switch). */
odin3_status pg_frame_push(odin3_pg *pg, const pg_frame *frame);

/* Whether out follows prev as the next bit of the same variable (they form one chunk, v4.2). */
bool pg_out_continues(const pg_out *prev, const pg_out *out);

/* --- pg_loop.c: for and repeat (EL-37) ---------------------------------------------------------
 */

/* Opens a FOR or REPEAT statement: pushes its PG_F_LOOP frame. */
odin3_status pg_loop_open(odin3_pg *pg, odin3_ast_id node);

/* One step of the PG_F_LOOP frame on top of the stack: the init, a test (pushing the body), a
 * step, or the end of the loop (popping the frame). */
odin3_status pg_loop_step(odin3_pg *pg);

/* Whether the variable of a for statement of the module (a symbol) is read outside the loops it is
 * the variable of, which makes it an output of the block (EL-35); *out. */
odin3_status pg_loopvar_is_output(odin3_pg *pg, odin3_sym_id sym, bool *out);

/* The symbol a loop's init or step assignment writes (0: its target is no plain name). */
odin3_sym_id pg_assign_symbol(const odin3_pg *pg, odin3_ast_id assign);

/* --- pg_call.c: function and task calls, register-array words (EL-38, EL-27) -------------------
 */

/* A TASK_CALL statement (K9 to K11). */
odin3_status pg_task_call(odin3_pg *pg, odin3_ast_id call);

/* The hooks of the expression walker (odin3_elab2_reads.call, .word) over the generator. */
odin3_status pg_call_hook(void *user, odin3_ast_id call, odin3_sig *out);
odin3_status pg_word_hook(void *user, const odin3_elab2_regs_rd *rd, odin3_sig *out);

/* The assignment of a register-array word with a variable index (pg->lvs.regs): a case over the
 * words. rhs is as wide as a word. */
odin3_status pg_regs_apply(odin3_pg *pg, const odin3_pg_assign_req *req);

/* Gives every symbol a call frame set back its (absent) wire; for a block that stopped inside a
 * call. */
void pg_frames_unwind(odin3_pg *pg);

/* The reads hook the generator gives the walker. */
odin3_elab2_reads pg_reads(odin3_pg *pg);

/* --- pg_scan.c: the scan of a block's statements (EL-35 step 1, EL-36 targets, folding) ------ */

/* The scan of a call's body in its frame: the targets of its switches, no new outputs. */
odin3_status pg_scan_call(odin3_pg *pg, odin3_ast_id body);

/* Collects the outputs (every bit an assignment can write) into pg->outs/out_of, each if/case's
 * blocking targets into swl/sw_pool, and the folded if/case into folds; reports the constructs
 * still pending. */
odin3_status pg_scan(odin3_pg *pg, odin3_ast_id body);

/* The qsort order of a vec of uint32_t (net IDs, positions). */
int pg_u32_compare(const void *lhs, const void *rhs);

/* --- pg_case.c: if and case (EL-36) ----------------------------------------------------------- */

/* Whether an if/case statement folds to one branch by the AST alone (a constant condition or
 * selector with constant items): *pick is the child to run + 1, PG_FOLD_NONE, or 0 (not folded). */
odin3_status pg_fold(odin3_pg *pg, odin3_ast_id node, uint32_t *pick);

/* The statement a folded if/case runs (pick as pg_fold gave it): the branch, or the folded item's
 * body; none (0) for PG_FOLD_NONE. */
odin3_ast_id pg_fold_body(const odin3_pg *pg, odin3_ast_id node, uint32_t pick);

/* Opens an IF / CASE statement: folded, it pushes the branch to run; else it makes the switch and
 * a PG_F_SWITCH frame, which pg_switch_step then drives. */
odin3_status pg_if_open(odin3_pg *pg, odin3_ast_id node);
odin3_status pg_case_open(odin3_pg *pg, odin3_ast_id node);

/* One step of the SWITCH frame on top of the stack: closes the open case, opens the next (its
 * compares generated, its entry actions made, its body pushed) or ends the switch. */
odin3_status pg_switch_step(odin3_pg *pg);

/* The switch of EL-36 as primitives, shared with the select write (pg_select.c). pg_sw_begin
 * makes the case temporaries t1 for the nets (sorted, unique; the bits the branches assign by `=`)
 * as one group per chunk and opens the switch; pg_sw_case opens a case (saving lv/rv, lv(nets) =
 * t1, the entry actions t1 = rv: x when x_entry); pg_sw_case_end closes it and restores; pg_sw_end
 * makes rv(nets) = t1 and the exit actions lv(nets) = t1 in the current case. */
typedef struct pg_sw_req {
    odin3_ast_id stmt;
    const odin3_sig *signal;
    const odin3_sigbit *nets;
    uint32_t n_nets;
    uint16_t flags;
    uint32_t width; /* the case width, sign and wildcard rule for the compares of its items */
    bool is_signed;
    uint8_t wild;
} pg_sw_req;
odin3_status pg_sw_begin(odin3_pg *pg, const pg_sw_req *req);
odin3_status pg_sw_case(odin3_pg *pg, const odin3_pg_case_req *req, bool x_entry);
void pg_sw_case_end(odin3_pg *pg);
odin3_status pg_sw_end(odin3_pg *pg);

/* --- pg_select.c: the case method for a non-constant lvalue select (EL-32) -------------------- */

/* Applies the assignment whose target pg->lvs is a non-constant select (lvs.var.node set); rhs is
 * lvs.var.width bits. */
odin3_status pg_select_apply(odin3_pg *pg, const odin3_pg_assign_req *req);

/* --- pg_emit.c -------------------------------------------------------------------------------- */

/* A body in canonical layout, still over signal bits (white-box test; step 7 maps them to pins). */
typedef struct pg_laid {
    odin3_vec sbits;      /* odin3_sigbit: the positions */
    odin3_vec spans_cmp;  /* odin3_pspan: the compares */
    odin3_vec acts;       /* odin3_proc_action */
    odin3_vec cases;      /* odin3_proc_case */
    odin3_vec sws;        /* odin3_proc_switch */
    odin3_vec syncs;      /* odin3_proc_sync */
    odin3_vec sync_sig;   /* odin3_sigbit per sync (0 for always) */
    odin3_vec case_order; /* uint32_t: the builder case at each new index */
    odin3_vec sw_order;   /* uint32_t: the builder switch at each new index */
} pg_laid;

void pg_laid_init(pg_laid *lay);
void pg_laid_free(pg_laid *lay);

/* The builder renumbered breadth first, dead positions dropped, empty actions removed. */
odin3_status pg_layout(const odin3_pg *pg, pg_laid *lay);

#endif
