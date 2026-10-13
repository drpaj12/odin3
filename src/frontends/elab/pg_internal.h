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

enum { PG_F_STMT = 1, PG_F_BLOCK = 2 };

/* One frame of the statement stack: a lone statement, or a `begin ... end` and the next child. */
typedef struct pg_frame {
    uint8_t kind;
    odin3_ast_id node;
    uint32_t next;
} pg_frame;

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
} pg_asg;
odin3_status pg_apply(odin3_pg *pg, const pg_asg *asg);

/* A temporary net's group and its position in it. */
typedef struct pg_grp {
    uint32_t group, pos;
} pg_grp;

/* The group of a temporary net; false for a net that is not one. */
bool pg_group_of(const odin3_pg *pg, odin3_net_id net, pg_grp *out);

/* --- pg_always.c ------------------------------------------------------------------------------ */

/* The read hook of the expression walker (odin3_elab2_reads) over rv. */
odin3_status pg_read_bit(void *user, odin3_net_id net, odin3_sigbit *out);

/* Runs one statement through the frame stack. */
odin3_status pg_run(odin3_pg *pg, odin3_ast_id stmt);

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
