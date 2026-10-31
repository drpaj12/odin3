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

enum { MAX_DIR_PAIRS = 6 };

typedef struct entry {
    uint32_t node;
    uint32_t nbr;
    uint32_t flag;
} entry;

static int entry_cmp(const void *lhs, const void *rhs) {
    const entry *xa = lhs;
    const entry *xb = rhs;
    if (xa->node != xb->node) {
        return xa->node < xb->node ? -1 : 1;
    }
    if (xa->nbr != xb->nbr) {
        return xa->nbr < xb->nbr ? -1 : 1;
    }
    return 0;
}

void glet_graph_free(glet_graph *gr) {
    odin3_util_free(gr->off);
    odin3_util_free(gr->adj);
    odin3_util_free(gr->flags);
    memset(gr, 0, sizeof *gr);
}

static void emit_entries(entry *ents, size_t total, glet_graph *gr) {
    size_t out = 0;
    for (size_t ii = 0; ii < total;) {
        entry cur = ents[ii];
        size_t jj = ii + 1;
        while (jj < total && ents[jj].node == cur.node && ents[jj].nbr == cur.nbr) {
            cur.flag |= ents[jj].flag;
            jj++;
        }
        gr->adj[out] = cur.nbr;
        gr->flags[out] = (uint8_t)cur.flag;
        gr->off[cur.node + 1]++;
        out++;
        ii = jj;
    }
    for (uint32_t vn = 0; vn < gr->n; vn++) {
        gr->off[vn + 1] += gr->off[vn];
        uint32_t deg = gr->off[vn + 1] - gr->off[vn];
        gr->max_degree = deg > gr->max_degree ? deg : gr->max_degree;
    }
    gr->m = out / 2;
}

odin3_status glet_graph_build(glet_graph *gr, uint32_t n, const glet_edge *edges, size_t count) {
    memset(gr, 0, sizeof *gr);
    gr->n = n;
    if (count > UINT32_MAX / 2) { /* off[] holds 2 * count entries in uint32 */
        odin3_log(ODIN3_LOG_ERROR, "graphlets: too many edges (%llu)", (unsigned long long)count);
        return ODIN3_ERR_INVALID_ARG;
    }
    entry *ents = odin3_util_calloc((count * 2 + 1) * sizeof *ents);
    gr->off = odin3_util_calloc(((size_t)n + 1) * sizeof *gr->off);
    if (ents == NULL || gr->off == NULL) {
        odin3_util_free(ents);
        glet_graph_free(gr);
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t total = 0;
    for (size_t ii = 0; ii < count; ii++) {
        if (edges[ii].from >= n || edges[ii].to >= n) {
            odin3_util_free(ents);
            glet_graph_free(gr);
            return ODIN3_ERR_INVALID_ARG;
        }
        if (edges[ii].from == edges[ii].to) {
            gr->self_loops++;
            continue;
        }
        ents[total++] = (entry){edges[ii].from, edges[ii].to, GLET_DIR_OUT};
        ents[total++] = (entry){edges[ii].to, edges[ii].from, GLET_DIR_IN};
    }
    qsort(ents, total, sizeof *ents, entry_cmp);
    gr->adj = odin3_util_calloc((total + 1) * sizeof *gr->adj);
    gr->flags = odin3_util_calloc(total + 1);
    if (gr->adj == NULL || gr->flags == NULL) {
        odin3_util_free(ents);
        glet_graph_free(gr);
        return ODIN3_ERR_NO_MEMORY;
    }
    uint64_t loops = gr->self_loops;
    emit_entries(ents, total, gr);
    gr->self_loops = loops;
    odin3_util_free(ents);
    return ODIN3_OK;
}

/* --- the counter ----------------------------------------------------------------------------- */

typedef struct esu {
    const glet_graph *gr;
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
    unsigned level; /* nodes chosen before the one being added */
    uint32_t root;
} esu;

static unsigned pair_ix(unsigned ii, unsigned jj) {
    return jj * (jj - 1) / 2 + ii;
}

/* Flags of the edge u -> v seen from u (0 when none). */
static unsigned edge_flags(const glet_graph *gr, uint32_t un, uint32_t vn) {
    uint32_t lo = gr->off[un];
    uint32_t hi = gr->off[un + 1];
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (gr->adj[mid] < vn) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < gr->off[un + 1] && gr->adj[lo] == vn ? gr->flags[lo] : 0;
}

/* The code of the set sub[0..pos) extended with w at position pos. */
static unsigned extend_code(const esu *es, uint32_t wn) {
    unsigned pos = es->level;
    unsigned code = es->codes[pos];
    for (unsigned ii = 0; ii < pos; ii++) {
        unsigned fl = edge_flags(es->gr, wn, es->sub[ii]); /* OUT: w -> sub[i] */
        if (fl == 0) {
            continue;
        }
        if (es->directed) {
            /* pair (i, pos): bit 0 is i -> pos, bit 1 is pos -> i */
            unsigned st =
                ((fl & GLET_DIR_IN) != 0 ? 1U : 0U) | ((fl & GLET_DIR_OUT) != 0 ? 2U : 0U);
            code |= st << (2 * pair_ix(ii, pos));
        } else {
            code |= 1U << pair_ix(ii, pos);
        }
    }
    return code;
}

static void add_orbits(esu *es, unsigned size, const uint8_t *row) {
    for (unsigned ii = 0; ii < size; ii++) {
        es->counts[(size_t)es->sub[ii] * es->norb + row[ii]]++;
    }
}

static void visit_directed(esu *es, unsigned code) {
    unsigned size = es->level + 1;
    unsigned pairs = size * (size - 1) / 2;
    unsigned both[MAX_DIR_PAIRS];
    unsigned nb = 0;
    for (unsigned ps = 0; ps < pairs; ps++) {
        if (((code >> (2 * ps)) & 3U) == 3U) {
            both[nb++] = ps;
        }
    }
    for (unsigned mask = 0; mask < (1U << nb); mask++) {
        unsigned cd = code;
        for (unsigned xb = 0; xb < nb; xb++) {
            unsigned st = ((mask >> xb) & 1U) != 0 ? 1U : 2U;
            cd = (cd & ~(3U << (2 * both[xb]))) | (st << (2 * both[xb]));
        }
        add_orbits(es, size, g_glet_tab.d_orb[size] + (size_t)cd * size);
    }
}

static void visit(esu *es, unsigned code) {
    unsigned size = es->level + 1;
    es->work++;
    if (es->directed) {
        visit_directed(es, code);
    } else {
        add_orbits(es, size, g_glet_tab.u_orb[size] + (size_t)code * size);
    }
}

static void cover_add(esu *es, uint32_t wn, int delta) {
    const glet_graph *gr = es->gr;
    es->cover[wn] = (uint8_t)(es->cover[wn] + delta);
    for (uint32_t ed = gr->off[wn]; ed < gr->off[wn + 1]; ed++) {
        es->cover[gr->adj[ed]] = (uint8_t)(es->cover[gr->adj[ed]] + delta);
    }
}

/* Pushes the exclusive neighbours of w (above the root) after the ones already in ext. */
static void ext_build(esu *es, uint32_t wn) {
    unsigned level = es->level;
    uint32_t root = es->root;
    const glet_graph *gr = es->gr;
    uint32_t len = es->elen[level];
    memcpy(es->ext[level + 1], es->ext[level], (size_t)len * sizeof(uint32_t));
    for (uint32_t ed = gr->off[wn]; ed < gr->off[wn + 1]; ed++) {
        uint32_t un = gr->adj[ed];
        if (un > root && es->cover[un] == 0) {
            es->ext[level + 1][len++] = un;
        }
    }
    es->elen[level + 1] = len;
}

static void root_init(esu *es, uint32_t root) {
    const glet_graph *gr = es->gr;
    es->sub[0] = root;
    es->root = root;
    es->codes[1] = 0;
    cover_add(es, root, 1);
    es->elen[1] = 0;
    for (uint32_t ed = gr->off[root]; ed < gr->off[root + 1]; ed++) {
        if (gr->adj[ed] > root) {
            es->ext[1][es->elen[1]++] = gr->adj[ed];
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
        uint32_t wn = es->ext[level][--es->elen[level]];
        es->level = level;
        if (level + 1 < es->size) {
            ext_build(es, wn);
        }
        es->codes[level + 1] = extend_code(es, wn);
        es->sub[level] = wn;
        cover_add(es, wn, 1);
        visit(es, es->codes[level + 1]);
        if (es->max_work != 0 && es->work > es->max_work) {
            return false;
        }
        if (level + 1 < es->size) {
            level++;
        } else {
            cover_add(es, wn, -1);
        }
    }
    cover_add(es, root, -1);
    return true;
}

static void esu_free(esu *es) {
    for (unsigned lv = 1; lv <= GLET_MAX_SIZE; lv++) {
        odin3_util_free(es->ext[lv]);
    }
    odin3_util_free(es->cover);
}

static odin3_status esu_open(esu *es, const glet_graph *gr, bool directed, unsigned size) {
    memset(es, 0, sizeof *es);
    es->gr = gr;
    es->directed = directed;
    es->size = size;
    es->cap = gr->max_degree * (uint32_t)size + 1U;
    es->cover = odin3_util_calloc((size_t)gr->n + 1);
    bool ok = es->cover != NULL;
    for (unsigned lv = 1; ok && lv <= size; lv++) {
        es->ext[lv] = odin3_util_calloc((size_t)es->cap * sizeof(uint32_t));
        ok = es->ext[lv] != NULL;
    }
    if (!ok) {
        esu_free(es);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

odin3_status glet_count(const glet_graph *gr, const glet_spec *spec, int64_t *counts,
                        uint64_t *work) {
    bool directed = spec->directed;
    unsigned size = directed ? (unsigned)GLET_MAX_DIR_SIZE : spec->size;
    uint64_t max_work = spec->max_work;
    uint32_t norb = glet_orbit_count(directed, size);
    if (norb == 0 || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: unsupported size %u", size);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = glet_tables_init();
    if (st != ODIN3_OK) {
        return st;
    }
    unsigned top = directed ? GLET_MAX_DIR_SIZE : size;
    esu es;
    st = esu_open(&es, gr, directed, top);
    if (st != ODIN3_OK) {
        return st;
    }
    es.norb = norb;
    es.counts = counts;
    es.max_work = max_work;
    bool within = true;
    for (uint32_t vn = 0; within && vn < gr->n; vn++) {
        within = run_root(&es, vn);
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
