/*
 * sc_int.h — private declarations of the scoap pass (spec docs/specs/2026-10-31-scoap-design.md):
 * the per-module model (cells as functions of nets), the function tables and the fixed-point
 * solver.
 */
#ifndef ODIN3_PASSES_SCOAP_SC_INT_H
#define ODIN3_PASSES_SCOAP_SC_INT_H

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SCOAP_INF: unreachable. SC_MAX_EXACT: widest function treated exactly (spec section 3). */
enum { SCOAP_INF = 1000000000, SC_MAX_EXACT = 8, SC_MAX_TABLE = 1 << SC_MAX_EXACT };

/* A cube over at most SC_MAX_EXACT inputs: bit j of care set = input j specified, val = its value.
 */
typedef struct sc_cube {
    uint8_t care;
    uint8_t val;
} sc_cube;

typedef enum sc_kind {
    SC_FUNC,   /* combinational function cell, exact (<= SC_MAX_EXACT inputs) */
    SC_WIDE,   /* function cell with more inputs: bounds */
    SC_FF,     /* storage cell handled by the sequential rules */
    SC_SCAN,   /* storage cell in scan view: Q is a pseudo input, the inputs pseudo outputs */
    SC_OPAQUE, /* black box, hard block, instance, anything without a function */
    SC_NKINDS
} sc_kind;

/* One cell of the model. Input nets live in the model's pool at in_off (n_in of them, 0 = no net).
 */
typedef struct sc_cell {
    uint32_t node;
    uint32_t out; /* output net, 0 none */
    uint32_t in_off;
    uint32_t n_in;
    uint32_t prime_off[2]; /* into the prime pool, per value */
    uint32_t prime_n[2];
    uint32_t sens_off;  /* into the sens table: n_in {offset, count} pairs */
    uint8_t kind;       /* sc_kind */
    int8_t cover_val;   /* SC_WIDE $sop: the value its rows give (0, 1), -1 none */
    uint32_t cover_off; /* SC_WIDE $sop: byte offset into the cover pool of its rows */
    uint32_t cover_len;
} sc_cell;

typedef struct sc_stats {
    uint64_t cells[SC_NKINDS];
    uint32_t sweeps;
    bool converged;
    uint64_t unreachable;
} sc_stats;

typedef struct sc_model {
    odin3_module *mod;
    bool seq;
    uint32_t n_nets; /* net IDs are 1 .. n_nets - 1 */
    int32_t *cc[2], *sc[2], *co, *so;
    uint8_t *live;
    uint8_t *fixed; /* constant nets: values never change */
    sc_cell *cells;
    size_t n_cells, cap_cells;
    odin3_vec in_pool;    /* uint32_t */
    odin3_vec prime_pool; /* sc_cube */
    odin3_vec sens_ref;   /* uint32_t pairs: offset, count */
    odin3_vec sens_pool;  /* uint8_t minterms */
    odin3_vec cover_pool; /* uint8_t */
    sc_stats stats;
    odin3_vec opaque_types; /* uint32_t celltype IDs, first seen */
} sc_model;

/* --- sc_fn.c: function tables ----------------------------------------------------------------- */

/* Truth table of an n-input function: bit i of table[] is f(i). n <= SC_MAX_EXACT. */
typedef struct sc_table {
    uint32_t n;
    uint8_t f[SC_MAX_TABLE];
} sc_table;

/* Appends the prime implicants of {f = v} and the sensitising minterms of every input to cell. */
odin3_status sc_fn_compile(sc_model *md, sc_cell *cell, const sc_table *tab);

/* --- sc_model.c: building ----------------------------------------------------------------------
 */
odin3_status sc_model_build(sc_model *md, odin3_module *mod, bool seq);
void sc_model_free(sc_model *md);

/* --- sc_solve.c: the fixed point ---------------------------------------------------------------
 */
void sc_solve(sc_model *md);

#endif
