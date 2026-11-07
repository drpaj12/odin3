/*
 * graphlets.c — the public C ABI entry for graph-generic graphlet counting (include/odin3/odin3.h,
 * spec docs/specs/2026-10-30-graphlets-design.md section 16): a thin wrapper over src/graph.
 */
#include "api/api.h"
#include "graph/glet.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdint.h>
#include <string.h>

/* Builds the simple graph of a public graph description. */
static odin3_status build_graph(const odin3_graphlet_graph *graph, glet_graph *gr) {
    if (graph->n_edges > SIZE_MAX / sizeof(glet_edge) - 1) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: too many edges (%llu)",
                  (unsigned long long)graph->n_edges);
        return ODIN3_ERR_INVALID_ARG;
    }
    glet_edge *edges = odin3_util_calloc(((size_t)graph->n_edges + 1) * sizeof *edges);
    if (edges == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint64_t i = 0; i < graph->n_edges; i++) {
        edges[i] = (glet_edge){graph->from[i], graph->to[i]};
    }
    odin3_status st = glet_graph_build(gr, graph->n, edges, (size_t)graph->n_edges);
    odin3_util_free(edges);
    return st;
}

ODIN3_EXPORT odin3_status odin3_graphlets_count(const odin3_graphlet_graph *graph,
                                                const odin3_graphlet_opts *opts, int64_t *out,
                                                uint64_t out_len) {
    if (graph == NULL || opts == NULL || out == NULL ||
        (graph->n_edges > 0 && (graph->from == NULL || graph->to == NULL))) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: graph, opts and out are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool dir = opts->directed != 0;
    uint32_t norb = glet_orbit_count(dir, dir ? GLET_MAX_DIR_SIZE : opts->size);
    if (norb == 0 || out_len < (uint64_t)graph->n * norb) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: size must be 4 or 5 and out must hold n * orbits");
        return ODIN3_ERR_INVALID_ARG;
    }
    glet_graph gr;
    odin3_status st = build_graph(graph, &gr);
    if (st != ODIN3_OK) {
        return st;
    }
    memset(out, 0, (size_t)graph->n * norb * sizeof *out);
    st = glet_count(&gr, &(glet_spec){dir, opts->size, opts->max_work, NULL}, out, NULL);
    glet_graph_free(&gr);
    return st;
}

ODIN3_EXPORT odin3_status odin3_graphlet_nodes(const odin3_graphlet_opts *opts, uint32_t graphlet,
                                               uint32_t *nodes) {
    if (opts == NULL || nodes == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: opts and nodes are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool dir = opts->directed != 0;
    *nodes = glet_graphlet_nodes(dir, dir ? GLET_MAX_DIR_SIZE : opts->size, graphlet);
    if (*nodes == 0) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: graphlet %u is not a graphlet of this mode",
                  (unsigned)graphlet);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static bool instance_args_ok(const odin3_graphlet_graph *graph, const odin3_graphlet_opts *opts,
                             const odin3_graphlet_instance_buf *buf) {
    return graph != NULL && opts != NULL && buf != NULL && (buf->max == 0 || buf->out != NULL) &&
           (graph->n_edges == 0 || (graph->from != NULL && graph->to != NULL));
}

ODIN3_EXPORT odin3_status odin3_graphlets_instances(const odin3_graphlet_graph *graph,
                                                    const odin3_graphlet_opts *opts,
                                                    uint32_t graphlet,
                                                    odin3_graphlet_instance_buf *buf) {
    if (!instance_args_ok(graph, opts, buf)) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: graph, opts and buf (with out) are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t nodes = 0;
    odin3_status st = odin3_graphlet_nodes(opts, graphlet, &nodes);
    if (st == ODIN3_OK && buf->max > buf->out_len / nodes) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: out must hold max * %u ids", (unsigned)nodes);
        st = ODIN3_ERR_INVALID_ARG;
    }
    glet_graph gr;
    st = st == ODIN3_OK ? build_graph(graph, &gr) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    glet_inst inst = {graphlet, buf->max, buf->out, 0, 0, 0};
    glet_spec spec = {opts->directed != 0, opts->size, opts->max_work, &inst};
    st = glet_count(&gr, &spec, NULL, NULL);
    buf->total = inst.total;
    glet_graph_free(&gr);
    return st;
}

static bool rows_ok(const odin3_graphlet_rows *rows) {
    return rows != NULL && rows->n <= UINT32_MAX && (rows->n == 0 || rows->counts != NULL);
}

ODIN3_EXPORT odin3_status odin3_graphlets_similarity(const odin3_graphlet_rows *rows_a,
                                                     const odin3_graphlet_rows *rows_b,
                                                     uint32_t n_orbits,
                                                     odin3_graphlet_similarity *out) {
    bool known =
        n_orbits == GLET_ORBITS_U4 || n_orbits == GLET_ORBITS_U5 || n_orbits == GLET_ORBITS_D4;
    if (!known || !rows_ok(rows_a) || !rows_ok(rows_b) || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: rows, out and n_orbits (15, 73 or 129) are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    glet_sig sa;
    glet_sig sb;
    glet_rows ra = {rows_a->counts, (uint32_t)rows_a->n, n_orbits};
    glet_rows rb = {rows_b->counts, (uint32_t)rows_b->n, n_orbits};
    odin3_status st = glet_sig_make(&sa, &ra, NULL);
    if (st != ODIN3_OK) {
        return st;
    }
    st = glet_sig_make(&sb, &rb, NULL);
    if (st == ODIN3_OK) {
        out->gdd_agreement = glet_gdd_agreement(&sa, &sb, NULL);
        out->cosine_log = glet_cosine_log(&sa, &sb);
        glet_sig_free(&sb);
    }
    glet_sig_free(&sa);
    return st;
}
