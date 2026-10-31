/*
 * glet.h — graphlet orbit counting (spec docs/specs/2026-10-30-graphlets-design.md): the simple
 * graph, the orbit tables and the exact ESU counter. Written from the published definitions
 * (Przulj 2007; Sarajlic 2016); no third-party counter code is used.
 */
#ifndef ODIN3_PASSES_GRAPHLETS_GLET_H
#define ODIN3_PASSES_GRAPHLETS_GLET_H

#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GLET_MAX_SIZE = 5,     /* largest graphlet, undirected */
    GLET_MAX_DIR_SIZE = 4, /* largest graphlet, directed */
    GLET_ORBITS_U4 = 15,
    GLET_ORBITS_U5 = 73,
    GLET_ORBITS_D4 = 129,
    GLET_GRAPHLETS_U4 = 9,
    GLET_GRAPHLETS_U5 = 30,
    GLET_GRAPHLETS_D4 = 40,
    GLET_MAX_ORBITS = 129,
    GLET_DIR_OUT = 1, /* adjacency flag: this node -> neighbour */
    GLET_DIR_IN = 2   /* adjacency flag: neighbour -> this node */
};

/* One directed edge of the input; self-loops are dropped by glet_graph_build. */
typedef struct glet_edge {
    uint32_t from;
    uint32_t to;
} glet_edge;

/* A simple graph in CSR form: neighbours of v are adj[off[v] .. off[v+1]), sorted ascending;
 * flags holds GLET_DIR_OUT / GLET_DIR_IN per entry (both set for an anti-parallel pair). */
typedef struct glet_graph {
    uint32_t n;
    uint64_t m; /* undirected edges */
    uint32_t *off;
    uint32_t *adj;
    uint8_t *flags;
    uint32_t max_degree;
    uint64_t self_loops; /* edges dropped because from == to */
} glet_graph;

/* Builds g from n nodes and the directed edges (ids < n; parallel edges collapse). NO_MEMORY;
 * INVALID_ARG for an id >= n. */
odin3_status glet_graph_build(glet_graph *gr, uint32_t n, const glet_edge *edges, size_t count);
void glet_graph_free(glet_graph *gr);

/* Orbit count for the mode: undirected size 4 or 5, directed (size ignored, 4). 0 when invalid. */
uint32_t glet_orbit_count(bool directed, unsigned size);
uint32_t glet_graphlet_count(bool directed, unsigned size);

/* Describes an orbit: its graphlet, the node count of the graphlet and the number of positions in
 * the orbit (the multiplicity: graphlet count = orbit total / mult). false when out of range. */
typedef struct glet_orbit_info {
    uint32_t graphlet;
    uint32_t nodes;
    uint32_t edges; /* edges of the graphlet (directed: oriented edges) */
    uint32_t mult;
    uint32_t code; /* canonical adjacency code of the graphlet (tables.c layout) */
    uint32_t pos;  /* a position of the orbit in that canonical labelling */
} glet_orbit_info;
bool glet_orbit_describe(bool directed, uint32_t orbit, glet_orbit_info *out);

/* What to count: directed or undirected, the largest graphlet (undirected 4 or 5; directed ignores
 * it and counts 2 to 4 nodes) and the work bound. */
typedef struct glet_spec {
    bool directed;
    unsigned size;
    uint64_t max_work;
} glet_spec;

/* Exact node orbit counts. counts is n * glet_orbit_count(...) int64, zeroed by the caller.
 * spec->max_work bounds the connected node sets visited (0: no bound); exceeding it is
 * ODIN3_ERR_INVALID_ARG (logged) with counts partial. *work receives the sets visited. */
odin3_status glet_count(const glet_graph *gr, const glet_spec *spec, int64_t *counts,
                        uint64_t *work);

/* --- signatures (spec sections 6 and 13) ---------------------------------------------------- */

/* The design-level signature of a set of nodes: orbit totals and the graphlet degree distribution
 * (GDD): for orbit j, the pairs (degree k >= 1, number of nodes that touch the orbit k times),
 * sorted by k, stored flat in deg / num with off[j] .. off[j+1]. */
typedef struct glet_sig {
    uint32_t norb;
    uint64_t nodes; /* nodes the signature covers */
    int64_t *totals;
    uint32_t *off;
    int64_t *deg;
    uint64_t *num;
} glet_sig;

/* Rows of orbit counts: n x norb int64, row-major. */
typedef struct glet_rows {
    const int64_t *counts;
    uint32_t n;
    uint32_t norb;
} glet_rows;

/* Signature of the rows whose mask byte is nonzero (mask NULL: all rows). */
odin3_status glet_sig_make(glet_sig *sig, const glet_rows *rows, const uint8_t *mask);
void glet_sig_free(glet_sig *sig);

/* GDD agreement (Przulj 2007; arithmetic mean over orbits) of two signatures of equal norb, in
 * [0, 1]; agreement of the single orbit j in per_orbit when non-NULL (norb doubles). */
double glet_gdd_agreement(const glet_sig *lhs, const glet_sig *rhs, double *per_orbit);

/* Cosine similarity of log(1 + orbit total) vectors, in [0, 1]. Two zero vectors give 1. */
double glet_cosine_log(const glet_sig *lhs, const glet_sig *rhs);

#endif
