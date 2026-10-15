/*
 * splice_int.h — the state the splice's source files share (3D plan Task 17): splice.c (phase A,
 * the driver), stitch.c (phases B and C) and clean.c (phase D). Not part of any interface.
 */
#ifndef ODIN3_PASSES_ABC_SPLICE_INT_H
#define ODIN3_PASSES_ABC_SPLICE_INT_H

#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/abc/run.h"
#include "passes/abc/splice.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* An object the splice made, so a failure can remove it again. */
enum { SP_NODE = 0, SP_NET = 1 };
typedef struct sp_created {
    uint8_t kind;
    uint32_t id;
} sp_created;

/* What phase B does to a CO c whose new driver is object d (§6.5's table). */
enum {
    SP_TAKE = 1, /* d is a fresh net: merge(keep = c, drop = d's net) */
    SP_FOLLOW,   /* d is carried by another net: merge(keep = that net, drop = c) */
    SP_BUFFER,   /* a buffer $lut drives c (special nets, and a net equal to itself) */
    SP_CONST     /* merge(keep = the canonical constant net, drop = c) */
};
typedef struct sp_step {
    uint8_t act;
    uint32_t buffer; /* SP_BUFFER: the node */
    uint32_t keep;   /* SP_FOLLOW, SP_CONST: the surviving net */
    uint32_t take;   /* SP_TAKE: the fresh net that merges into c */
} sp_step;

typedef struct splice {
    odin3_abc_run *run;
    odin3_module *mod;
    const odin3_abc_region *region;
    odin3_abc_result *res;
    odin3_abc_splice_stats stats;
    odin3_vec created;  /* sp_created, in creation order */
    odin3_vec objnet;   /* uint32_t per result object: the net it drives */
    odin3_vec objprov;  /* uint32_t per result object: the record of the new object */
    odin3_vec objnode;  /* uint32_t per result object: the LUT node that implements it, or 0 */
    odin3_vec anchors;  /* odin3_abc_anchor */
    odin3_vec carried;  /* uint32_t per result object: the net that carries it so far */
    odin3_vec ffnode;   /* uint32_t per result register: the new flip-flop */
    odin3_vec oldq;     /* uint32_t per result register: its representative's old Q net */
    odin3_vec repname;  /* uint32_t per result register: its representative's node name */
    odin3_vec steps;    /* sp_step per real CO */
    odin3_vec old_nets; /* uint32_t: the output nets of the old cells */
    odin3_net_id canon[2];
} splice;

/* The log of creations. NO_MEMORY. */
odin3_status sp_note(splice *sp, uint8_t kind, uint32_t id);

/* Removes what phase A made, in reverse order (best effort: a failed delete is skipped). */
void sp_rollback(splice *sp);

/* Phase A: records, nets, LUTs, flip-flops, the buffers and the plan of phase B. */
odin3_status sp_phase_a(splice *sp);

/* Phase B: re-stitch every CO. */
odin3_status sp_phase_b(splice *sp);

/* Phase C: delete the old cells, move the register names and Q nets. */
odin3_status sp_phase_c(splice *sp);

/* Phase D: remove the new cells nothing reads, then the pinless nets and wires. */
void sp_phase_d(splice *sp);

/* The canonical constant net of value (0 or 1) for this splice: found or made (and logged for
 * the rollback). */
odin3_status sp_canon(splice *sp, int value, odin3_net_id *out);

/* True when the net has a pin on a port node (a port net). */
bool sp_on_port(const odin3_module *mod, odin3_net_id net);

/* True when the net feeds the clock pin of a flip-flop or latch. */
bool sp_is_clock(const odin3_module *mod, odin3_net_id net);

/* u32 at index of a vec of uint32_t. */
uint32_t sp_u32(const odin3_vec *vec, size_t idx);

/* Pushes a uint32_t; NO_MEMORY. */
odin3_status sp_push(odin3_vec *vec, uint32_t value);

#endif
