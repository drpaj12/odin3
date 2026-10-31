/*
 * glet_count — graphlet orbit counts of a graph file (spec
 * docs/specs/2026-10-30-graphlets-design.md). Usage: glet_count [--directed] SIZE IN OUT. IN: first
 * line "N M", then M lines "u v" (0-based; directed mode reads them as u -> v, undirected mode
 * symmetrises). OUT: one line per node, the orbit counts separated by spaces (the format ORCA
 * writes). The oracle gate and the timing table use it next to the ORCA binary.
 */
#include "graph/glet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr, "usage: glet_count [--directed] SIZE IN OUT\n");
    return 2;
}

static int load(const char *path, glet_graph *g) {
    FILE *fp = fopen(path, "r");
    unsigned n = 0;
    unsigned long m = 0;
    if (fp == NULL || fscanf(fp, "%u %lu", &n, &m) != 2) {
        return 1;
    }
    glet_edge *edges = malloc((m + 1) * sizeof *edges);
    for (unsigned long i = 0; edges != NULL && i < m; i++) {
        if (fscanf(fp, "%u %u", &edges[i].from, &edges[i].to) != 2) {
            free(edges);
            edges = NULL;
        }
    }
    (void)fclose(fp);
    int bad = edges == NULL || glet_graph_build(g, n, edges, m) != ODIN3_OK;
    free(edges);
    return bad;
}

int main(int argc, char **argv) {
    bool directed = argc > 1 && strcmp(argv[1], "--directed") == 0;
    int base = directed ? 2 : 1;
    if (argc != base + 3) {
        return usage();
    }
    unsigned size = (unsigned)atoi(argv[base]);
    glet_graph g;
    if (load(argv[base + 1], &g) != 0) {
        fprintf(stderr, "glet_count: cannot read %s\n", argv[base + 1]);
        return 1;
    }
    uint32_t norb = glet_orbit_count(directed, size);
    int64_t *counts = calloc(((size_t)g.n + 1) * (norb + 1), sizeof *counts);
    if (norb == 0 || counts == NULL ||
        glet_count(&g, &(glet_spec){directed, size, 0}, counts, NULL) != ODIN3_OK) {
        return 1;
    }
    FILE *out = fopen(argv[base + 2], "w");
    if (out == NULL) {
        return 1;
    }
    for (uint32_t v = 0; v < g.n; v++) {
        for (uint32_t o = 0; o < norb; o++) {
            fprintf(out, "%s%lld", o != 0 ? " " : "", (long long)counts[(size_t)v * norb + o]);
        }
        fputc('\n', out);
    }
    (void)fclose(out);
    free(counts);
    glet_graph_free(&g);
    return 0;
}
