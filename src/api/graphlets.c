/*
 * graphlets.c — the public C ABI entry for graph-generic graphlet counting (include/odin3/odin3.h,
 * spec docs/specs/2026-10-30-graphlets-design.md section 16): a thin wrapper over src/graph.
 */
#include "api/api.h"
#include "graph/glet.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"

#include <string.h>

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
    glet_edge *edges = odin3_util_calloc(((size_t)graph->n_edges + 1) * sizeof *edges);
    if (edges == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint64_t i = 0; i < graph->n_edges; i++) {
        edges[i] = (glet_edge){graph->from[i], graph->to[i]};
    }
    glet_graph gr;
    odin3_status st = glet_graph_build(&gr, graph->n, edges, (size_t)graph->n_edges);
    odin3_util_free(edges);
    if (st != ODIN3_OK) {
        return st;
    }
    memset(out, 0, (size_t)graph->n * norb * sizeof *out);
    st = glet_count(&gr, &(glet_spec){dir, opts->size, opts->max_work}, out, NULL);
    glet_graph_free(&gr);
    return st;
}
