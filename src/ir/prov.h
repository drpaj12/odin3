/*
 * prov.h — provenance lineage (IR-12, IR-13): records, pass runs, operations, hash-consing,
 * backward and forward navigation, tombstones (IR-6).
 */
#ifndef ODIN3_IR_PROV_H
#define ODIN3_IR_PROV_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Records are design-global, immutable, never deleted and hash-consed: a call that would create
 * a record equal to an existing one returns the existing ID. Record IDs start at 1 and grow in
 * creation order, so every parent has a smaller ID than its children (a DAG by construction).
 * Misuse detectable in O(1) per argument returns ODIN3_ERR_INVALID_ARG with an error logged;
 * out of memory returns ODIN3_ERR_NO_MEMORY. On any failure nothing changes.
 */

/*
 * A source location: file is a strtab ID (0 = unknown), lines and columns as the reader saw.
 * loc is the raw source-manager location of the node (any buffer, 0 = none; src/ast/srcman.h),
 * from which the macro and include chains are derived on demand; part of the record identity.
 */
typedef struct odin3_srcloc {
    uint32_t file;
    uint32_t line, col, end_line, end_col;
    uint32_t loc;
} odin3_srcloc;

typedef enum odin3_prov_kind {
    ODIN3_PROV_SOURCE,   /* from source text */
    ODIN3_PROV_IMPORTED, /* from a netlist file (structural only) */
    ODIN3_PROV_DERIVED   /* made by a pass operation from its parents */
} odin3_prov_kind;

/*
 * A pass run's context: the design, the run and the current operation (0 until the first
 * odin3_prov_begin_op). Filled by odin3_pass_run_begin; passes only call begin_op on it.
 */
typedef struct odin3_pass_ctx {
    odin3_design *design;
    odin3_passrun_id run;
    uint32_t op;
} odin3_pass_ctx;

/*
 * Starts a pass run named pass_name_str (a non-empty strtab ID): appends it to the design's
 * pass-run table and fills *ctx with op 0. The run ID is the run number: one global sequence
 * from 1, in order; names may repeat across runs. Readers and front ends are runs too.
 * INVALID_ARG for a NULL design or ctx.
 */
odin3_status odin3_pass_run_begin(odin3_design *design, uint32_t pass_name_str,
                                  odin3_pass_ctx *ctx);

/* Starts the next operation of the run (++op): one per decompose or clump (IR-12). */
void odin3_prov_begin_op(odin3_pass_ctx *ctx);

/* Where a SOURCE or IMPORTED record comes from: locations (copied), AST node, hierarchy path. */
typedef struct odin3_prov_origin {
    const odin3_srcloc *locs; /* n_locs entries; may be NULL when n_locs is 0 */
    uint32_t n_locs;
    uint32_t ast;  /* AST node ID in the elaborated (else parsed) store of the run, 0 = none */
    uint32_t hier; /* strtab ID of the hierarchical path (top/u1/gen[3]), 0 = none */
} odin3_prov_origin;

/*
 * The SOURCE (or IMPORTED) record for the origin in the context's run. Identity is every field
 * except op: the same origin in the same run gives the same ID in any operation (the stored op
 * is that of the first call). INVALID_ARG for a bad context, a file or hier that is not a strtab
 * ID, or NULL locs with n_locs > 0.
 */
odin3_status odin3_prov_source(const odin3_pass_ctx *ctx, const odin3_prov_origin *origin,
                               odin3_prov_id *out);
odin3_status odin3_prov_imported(const odin3_pass_ctx *ctx, const odin3_prov_origin *origin,
                                 odin3_prov_id *out);

typedef struct odin3_prov_list {
    const odin3_prov_id *ids;
    uint32_t count;
} odin3_prov_list;

/*
 * The DERIVED record of the context's current operation with these parents, in order (a repeated
 * parent is kept once, at its first position). Identity is every field including run and op, so
 * every piece one operation makes shares one record and distinct operations never merge.
 * INVALID_ARG for a bad context, no operation begun (op 0), no parents, or a parent that is not
 * an existing record.
 */
odin3_status odin3_prov_derive(const odin3_pass_ctx *ctx, odin3_prov_list parents,
                               odin3_prov_id *out);

/* A record as stored; arrays are owned by the design and never change. */
typedef struct odin3_prov_record {
    odin3_prov_kind kind;
    odin3_passrun_id run;
    uint32_t op;
    const odin3_srcloc *locs;
    uint32_t n_locs;
    uint32_t ast;
    uint32_t hier;
    odin3_prov_list parents; /* DERIVED only; empty otherwise */
} odin3_prov_record;

/* The record with ID id, NULL for 0 or an unknown ID. Valid as long as the design. */
const odin3_prov_record *odin3_prov_get(const odin3_design *design, odin3_prov_id id);

/* One past the last record ID. */
uint32_t odin3_prov_end(const odin3_design *design);

/* The parents of a record, in order; empty for a leaf or an unknown ID. */
odin3_prov_list odin3_prov_parents(const odin3_design *design, odin3_prov_id id);

/* The pass name (strtab ID) of a run, 0 for an unknown run; one past the last run ID. */
uint32_t odin3_passrun_name(const odin3_design *design, odin3_passrun_id run);
uint32_t odin3_passrun_end(const odin3_design *design);

/*
 * Backward navigation: calls visit once for each SOURCE or IMPORTED record reachable from id
 * (id itself included), in depth-first order following parents in order, so the first call is
 * the record that names the object (IR §6). Iterative, with a visited mark; visit is called after
 * the walk, so it may itself navigate. INVALID_ARG for an unknown id or a NULL design or visit;
 * NO_MEMORY before any call.
 */
typedef void (*odin3_prov_visit)(void *user, odin3_prov_id leaf);
odin3_status odin3_prov_sources(const odin3_design *design, odin3_prov_id id,
                                odin3_prov_visit visit, void *user);

/*
 * Forward navigation (IR §6). A snapshot built by one sweep over every node, net and wire of
 * every module, live and dead, plus each module itself and every tombstone; objects with prov 0
 * (none) or an unknown prov are skipped. It stores O(records + parent edges + objects): the
 * objects carrying each record, each record's children, and the leaf records of each (file,
 * line). Queries walk children breadth-first and never allocate. NULL on out of memory or a NULL
 * design. Later changes to the design are not reflected; the design must outlive the index.
 */
typedef struct odin3_prov_index odin3_prov_index;
odin3_prov_index *odin3_prov_index_build(odin3_design *design);

/* Bytes the index holds (its struct and arrays). */
size_t odin3_prov_index_bytes(const odin3_prov_index *ix);

/*
 * One object found by the index: its module and kind/ID, whether it is live. A tombstone hit has
 * obj.id 0, live false and tombstone set to the tombstone's ID (0 for every other hit).
 */
typedef struct odin3_prov_hit {
    odin3_module_id module;
    odin3_objref obj;
    bool live;
    uint32_t tombstone;
} odin3_prov_hit;

/*
 * A view of a query's result, owned by the index: valid until the next query on the same index
 * or its destruction. Order: breadth-first over records from the start record(s) (each record's
 * objects in sweep order), so an object's own record's objects come before its descendants'.
 */
typedef struct odin3_prov_hits {
    const odin3_prov_hit *hits;
    uint32_t count;
} odin3_prov_hits;

/* Objects with a source location of the same file and line among their leaves (col ignored;
 * file 0, unknown, matches nothing). */
odin3_prov_hits odin3_prov_index_by_loc(odin3_prov_index *ix, odin3_srcloc loc);

/* Objects whose ancestry includes rec (rec itself included); empty for an unknown record. */
odin3_prov_hits odin3_prov_index_by_record(odin3_prov_index *ix, odin3_prov_id rec);

/* Frees the index; NULL is a no-op. */
void odin3_prov_index_destroy(odin3_prov_index *ix);

/*
 * What a dead node, net or wire was, kept when compact frees its slot (IR-6): written by
 * odin3_module_compact so history keeps pointing at what existed. type is none for nets and
 * wires. A node's tombstone also keeps its parameter values and, per pin in pin order, the
 * strtab name of the net the pin was on when the node was deleted (0: unconnected, or a net
 * without a name) (PHASE1 #14); nets and wires leave both arrays empty. A stored tombstone's
 * arrays are owned by the design and never change.
 */
typedef struct odin3_tombstone {
    odin3_module_id module;
    odin3_objkind kind;
    odin3_celltype_id type;
    uint32_t name;
    odin3_prov_id prov;
    const odin3_value *params; /* n_params values; NULL when n_params is 0 */
    uint32_t n_params;
    const uint32_t *pin_nets; /* n_pins net names (strtab IDs, 0 = none); NULL when n_pins is 0 */
    uint32_t n_pins;
} odin3_tombstone;

/*
 * Appends a tombstone (IDs from 1), deep-copying its parameter values (payloads included) and pin
 * net names into the design. INVALID_ARG for a NULL design or tombstone, an unknown module, a
 * kind other than node, net or wire, a node whose type is not a cell type of the design, a net
 * or wire with a type, parameters or pin names, a NULL array with a non-zero count, a parameter
 * odin3_value_valid rejects, a pin name that is not a strtab ID, or a prov that is neither 0 nor
 * an existing record. NO_MEMORY on out of memory; the table is unchanged on any failure.
 */
odin3_status odin3_tombstone_add(odin3_design *design, const odin3_tombstone *tomb);

/* Tombstone id, NULL for 0 or an unknown ID; one past the last tombstone ID. */
const odin3_tombstone *odin3_tombstone_get(const odin3_design *design, uint32_t id);
uint32_t odin3_tombstone_end(const odin3_design *design);

#endif
