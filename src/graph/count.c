/*
 * count.c — the simple graph and the exact orbit counter (spec sections 2 and 5). ESU (Wernicke
 * 2006) enumerates every connected node set of 2..size nodes of the undirected graph exactly once;
 * the adjacency among the chosen nodes is kept as a code and looked up in the orbit tables. The
 * directed mode resolves every anti-parallel pair to one direction (both ways, one occurrence
 * each), which is Sarajlic 2016's rule for such pairs. No recursion: the stack is explicit.
 */
#include "graph/glet.h"
#include "graph/glet_int.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
    uint32_t node;
    uint32_t nbr;
    uint32_t flag;
} entry;

static int entry_cmp(const void *lhs, const void *rhs) {
    const entry *a = lhs;
    const entry *b = rhs;
    if (a->node != b->node) {
        return a->node < b->node ? -1 : 1;
    }
    return a->nbr < b->nbr ? -1 : (a->nbr > b->nbr ? 1 : 0);
}

void glet_graph_free(glet_graph *g) {
    odin3_util_free(g->off);
    odin3_util_free(g->adj);
    odin3_util_free(g->flags);
    memset(g, 0, sizeof *g);
}

static void emit_entries(entry *ents, size_t total, glet_graph *g) {
    size_t out = 0;
    for (size_t i = 0; i < total;) {
        entry cur = ents[i];
        size_t j = i + 1;
        while (j < total && ents[j].node == cur.node && ents[j].nbr == cur.nbr) {
            cur.flag |= ents[j].flag;
            j++;
        }
        g->adj[out] = cur.nbr;
        g->flags[out] = (uint8_t)cur.flag;
        g->off[cur.node + 1]++;
        out++;
        i = j;
    }
    for (uint32_t v = 0; v < g->n; v++) {
        g->off[v + 1] += g->off[v];
        uint32_t deg = g->off[v + 1] - g->off[v];
        g->max_degree = deg > g->max_degree ? deg : g->max_degree;
    }
    g->m = out / 2;
}

odin3_status glet_graph_build(glet_graph *g, uint32_t n, const glet_edge *edges, size_t count) {
    memset(g, 0, sizeof *g);
    g->n = n;
    entry *ents = odin3_util_calloc((count * 2 + 1) * sizeof *ents);
    g->off = odin3_util_calloc(((size_t)n + 1) * sizeof *g->off);
    if (ents == NULL || g->off == NULL) {
        odin3_util_free(ents);
        glet_graph_free(g);
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (edges[i].from >= n || edges[i].to >= n) {
            odin3_util_free(ents);
            glet_graph_free(g);
            return ODIN3_ERR_INVALID_ARG;
        }
        if (edges[i].from == edges[i].to) {
            g->self_loops++;
            continue;
        }
        ents[total++] = (entry){edges[i].from, edges[i].to, GLET_DIR_OUT};
        ents[total++] = (entry){edges[i].to, edges[i].from, GLET_DIR_IN};
    }
    qsort(ents, total, sizeof *ents, entry_cmp);
    g->adj = odin3_util_calloc((total + 1) * sizeof *g->adj);
    g->flags = odin3_util_calloc(total + 1);
    if (g->adj == NULL || g->flags == NULL) {
        odin3_util_free(ents);
        glet_graph_free(g);
        return ODIN3_ERR_NO_MEMORY;
    }
    uint64_t loops = g->self_loops;
    emit_entries(ents, total, g);
    g->self_loops = loops;
    odin3_util_free(ents);
    return ODIN3_OK;
}

/* --- the counter ----------------------------------------------------------------------------- */

typedef struct esu {
    const glet_graph *g;
    bool directed;
    unsigned size;
    uint32_t cap;
    uint32_t *ext[GLET_MAX_SIZE + 1];
    uint32_t elen[GLET_MAX_SIZE + 1];
    uint32_t sub[GLET_MAX_SIZE];
    unsigned codes[GLET_MAX_SIZE + 1]; /* code after adding the first s nodes */
    uint8_t *cover;
    uint32_t norb;
    int64_t *counts;
    uint64_t work;
    uint64_t max_work;
} esu;

static unsigned pair_ix(unsigned i, unsigned j) {
    return j * (j - 1) / 2 + i;
}

/* Flags of the edge u -> v seen from u (0 when none). */
static unsigned edge_flags(const glet_graph *g, uint32_t u, uint32_t v) {
    uint32_t lo = g->off[u];
    uint32_t hi = g->off[u + 1];
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g->adj[mid] < v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < g->off[u + 1] && g->adj[lo] == v ? g->flags[lo] : 0;
}

/* The code of the set sub[0..pos) extended with w at position pos. */
static unsigned extend_code(const esu *es, unsigned pos, uint32_t w) {
    unsigned code = es->codes[pos];
    for (unsigned i = 0; i < pos; i++) {
        unsigned fl = edge_flags(es->g, w, es->sub[i]); /* OUT: w -> sub[i] */
        if (fl == 0) {
            continue;
        }
        if (es->directed) {
            /* pair (i, pos): bit 0 is i -> pos, bit 1 is pos -> i */
            unsigned st =
                ((fl & GLET_DIR_IN) != 0 ? 1U : 0U) | ((fl & GLET_DIR_OUT) != 0 ? 2U : 0U);
            code |= st << (2 * pair_ix(i, pos));
        } else {
            code |= 1U << pair_ix(i, pos);
        }
    }
    return code;
}

static void add_orbits(esu *es, unsigned size, const uint8_t *row) {
    for (unsigned i = 0; i < size; i++) {
        es->counts[(size_t)es->sub[i] * es->norb + row[i]]++;
    }
}

static void visit_directed(esu *es, unsigned size, unsigned code) {
    unsigned pairs = size * (size - 1) / 2;
    unsigned both[6];
    unsigned nb = 0;
    for (unsigned p = 0; p < pairs; p++) {
        if (((code >> (2 * p)) & 3U) == 3U) {
            both[nb++] = p;
        }
    }
    for (unsigned mask = 0; mask < (1U << nb); mask++) {
        unsigned c = code;
        for (unsigned b = 0; b < nb; b++) {
            unsigned st = ((mask >> b) & 1U) != 0 ? 1U : 2U;
            c = (c & ~(3U << (2 * both[b]))) | (st << (2 * both[b]));
        }
        add_orbits(es, size, g_glet_tab.d_orb[size] + (size_t)c * size);
    }
}

static void visit(esu *es, unsigned size, unsigned code) {
    es->work++;
    if (es->directed) {
        visit_directed(es, size, code);
    } else {
        add_orbits(es, size, g_glet_tab.u_orb[size] + (size_t)code * size);
    }
}

static void cover_add(esu *es, uint32_t w, int delta) {
    const glet_graph *g = es->g;
    es->cover[w] = (uint8_t)(es->cover[w] + delta);
    for (uint32_t e = g->off[w]; e < g->off[w + 1]; e++) {
        es->cover[g->adj[e]] = (uint8_t)(es->cover[g->adj[e]] + delta);
    }
}

/* Pushes the exclusive neighbours of w (above the root) after the ones already in ext. */
static void ext_build(esu *es, unsigned level, uint32_t root, uint32_t w) {
    const glet_graph *g = es->g;
    uint32_t len = es->elen[level];
    memcpy(es->ext[level + 1], es->ext[level], (size_t)len * sizeof(uint32_t));
    for (uint32_t e = g->off[w]; e < g->off[w + 1]; e++) {
        uint32_t u = g->adj[e];
        if (u > root && es->cover[u] == 0) {
            es->ext[level + 1][len++] = u;
        }
    }
    es->elen[level + 1] = len;
}

static void root_init(esu *es, uint32_t root) {
    const glet_graph *g = es->g;
    es->sub[0] = root;
    es->codes[1] = 0;
    cover_add(es, root, 1);
    es->elen[1] = 0;
    for (uint32_t e = g->off[root]; e < g->off[root + 1]; e++) {
        if (g->adj[e] > root) {
            es->ext[1][es->elen[1]++] = g->adj[e];
        }
    }
}

/* Runs ESU from one root. Returns false when the work bound was exceeded. */
static bool run_root(esu *es, uint32_t root) {
    root_init(es, root);
    unsigned level = 1;
    while (level >= 1) {
        if (es->elen[level] == 0) {
            level--;
            if (level >= 1) {
                cover_add(es, es->sub[level], -1);
            }
            continue;
        }
        uint32_t w = es->ext[level][--es->elen[level]];
        if (level + 1 < es->size) {
            ext_build(es, level, root, w);
        }
        es->codes[level + 1] = extend_code(es, level, w);
        es->sub[level] = w;
        cover_add(es, w, 1);
        visit(es, level + 1, es->codes[level + 1]);
        if (es->max_work != 0 && es->work > es->max_work) {
            return false;
        }
        if (level + 1 < es->size) {
            level++;
        } else {
            cover_add(es, w, -1);
        }
    }
    cover_add(es, root, -1);
    return true;
}

static void esu_free(esu *es) {
    for (unsigned l = 1; l <= GLET_MAX_SIZE; l++) {
        odin3_util_free(es->ext[l]);
    }
    odin3_util_free(es->cover);
}

static odin3_status esu_open(esu *es, const glet_graph *g, bool directed, unsigned size) {
    memset(es, 0, sizeof *es);
    es->g = g;
    es->directed = directed;
    es->size = size;
    es->cap = g->max_degree * (uint32_t)size + 1U;
    es->cover = odin3_util_calloc((size_t)g->n + 1);
    bool ok = es->cover != NULL;
    for (unsigned l = 1; ok && l <= size; l++) {
        es->ext[l] = odin3_util_calloc((size_t)es->cap * sizeof(uint32_t));
        ok = es->ext[l] != NULL;
    }
    if (!ok) {
        esu_free(es);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

odin3_status glet_count(const glet_graph *g, const glet_spec *spec, int64_t *counts,
                        uint64_t *work) {
    bool directed = spec->directed;
    unsigned size = directed ? (unsigned)GLET_MAX_DIR_SIZE : spec->size;
    uint64_t max_work = spec->max_work;
    uint32_t norb = glet_orbit_count(directed, size);
    if (norb == 0 || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: unsupported size %u", size);
        return ODIN3_ERR_INVALID_ARG;
    }
    glet_tables_init();
    unsigned top = directed ? GLET_MAX_DIR_SIZE : size;
    esu es;
    odin3_status st = esu_open(&es, g, directed, top);
    if (st != ODIN3_OK) {
        return st;
    }
    es.norb = norb;
    es.counts = counts;
    es.max_work = max_work;
    bool within = true;
    for (uint32_t v = 0; within && v < g->n; v++) {
        within = run_root(&es, v);
    }
    if (work != NULL) {
        *work = es.work;
    }
    esu_free(&es);
    if (!within) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: more than %llu connected node sets; exclude global nets "
                  "(--exclude-global), narrow the input, or raise --max-work",
                  (unsigned long long)max_work);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}
