/*
 * tables.c — the orbit tables of the graphlets pass (spec section 5). For graphs of s nodes a
 * "code" holds the adjacency among positions 0..s-1: pair (i, j), i < j, has index
 * j*(j-1)/2 + i; undirected uses one bit per pair, directed two (bit 0: i -> j, bit 1: j -> i;
 * the state 3 = both is resolved by the counter, never stored here). The canonical form of a code
 * is the smallest code over all permutations of the positions; the orbit of a position is its
 * class under the automorphisms. Graphlets are numbered by (node count, edge count, canonical
 * code), orbits by graphlet and then the first position of the class in the canonical labelling.
 * The undirected orbit ids are then renamed to Przulj's numbering (k_przulj below).
 */
#include "graph/glet.h"
#include "graph/glet_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_PERMS = 120,
    MAX_CANON = 64,
    MAX_PAIRS = 10,
    NONE_ORBIT = 0xFF,
    MAX_CODES_U = 1024,
    MAX_CODES_D = 4096
};

typedef struct perm_set {
    unsigned count;
    uint8_t perm[MAX_PERMS][GLET_MAX_SIZE];
} perm_set;

typedef struct tab_ctx {
    bool directed;
    unsigned bits; /* bits per pair */
    unsigned size;
    unsigned pairs;
    perm_set perms;
} tab_ctx;

glet_tables g_glet_tab;
static bool g_built;

static unsigned pair_index(unsigned ii, unsigned jj) {
    return jj * (jj - 1) / 2 + ii;
}

static void perms_gen(perm_set *set, unsigned size) {
    uint8_t cur[GLET_MAX_SIZE] = {0};
    for (unsigned ii = 0; ii < size; ii++) {
        cur[ii] = (uint8_t)ii;
    }
    set->count = 0;
    for (;;) {
        memcpy(set->perm[set->count++], cur, size);
        int ii = (int)size - 2;
        while (ii >= 0 && cur[ii] > cur[ii + 1]) {
            ii--;
        }
        if (ii < 0) {
            return;
        }
        int jj = (int)size - 1;
        while (cur[jj] < cur[ii]) {
            jj--;
        }
        uint8_t tmp = cur[ii];
        cur[ii] = cur[jj];
        cur[jj] = tmp;
        for (int xa = ii + 1, xb = (int)size - 1; xa < xb; xa++, xb--) {
            tmp = cur[xa];
            cur[xa] = cur[xb];
            cur[xb] = tmp;
        }
    }
}

static unsigned permute_code(const tab_ctx *tc, unsigned code, const uint8_t *perm) {
    unsigned mask = (1U << tc->bits) - 1U;
    unsigned out = 0;
    for (unsigned jj = 1; jj < tc->size; jj++) {
        for (unsigned ii = 0; ii < jj; ii++) {
            unsigned st = (code >> (tc->bits * pair_index(ii, jj))) & mask;
            unsigned xa = perm[ii];
            unsigned xb = perm[jj];
            if (xa > xb) {
                unsigned tmp = xa;
                xa = xb;
                xb = tmp;
                st = tc->bits == 2 ? ((st & 1U) << 1) | (st >> 1) : st;
            }
            out |= st << (tc->bits * pair_index(xa, xb));
        }
    }
    return out;
}

/* True when the code has no state 3 (directed) . */
static bool code_oriented(const tab_ctx *tc, unsigned code) {
    for (unsigned ps = 0; tc->bits == 2 && ps < tc->pairs; ps++) {
        if (((code >> (2 * ps)) & 3U) == 3U) {
            return false;
        }
    }
    return true;
}

static bool code_connected(const tab_ctx *tc, unsigned code) {
    unsigned mask = (1U << tc->bits) - 1U;
    unsigned seen = 1U;
    for (bool grew = true; grew;) {
        grew = false;
        for (unsigned jj = 1; jj < tc->size; jj++) {
            for (unsigned ii = 0; ii < jj; ii++) {
                bool edge = ((code >> (tc->bits * pair_index(ii, jj))) & mask) != 0;
                bool si = ((seen >> ii) & 1U) != 0;
                bool sj = ((seen >> jj) & 1U) != 0;
                if (edge && si != sj) {
                    seen |= (1U << ii) | (1U << jj);
                    grew = true;
                }
            }
        }
    }
    return seen == (1U << tc->size) - 1U;
}

static unsigned code_edges(const tab_ctx *tc, unsigned code) {
    unsigned nn = 0;
    for (unsigned xb = 0; xb < tc->bits * tc->pairs; xb++) {
        nn += (code >> xb) & 1U;
    }
    return nn;
}

/* One graphlet of one size: its canonical code and the orbit of each canonical position. */
typedef struct canon_entry {
    unsigned size;
    unsigned code;
    unsigned edges;
    uint8_t local[GLET_MAX_SIZE]; /* local orbit index of each position */
    unsigned n_local;
    unsigned mult[GLET_MAX_SIZE];
    unsigned first_graphlet;
    unsigned first_orbit;
} canon_entry;

static int canon_cmp(const void *lhs, const void *rhs) {
    const canon_entry *xa = lhs;
    const canon_entry *xb = rhs;
    if (xa->edges != xb->edges) {
        return xa->edges < xb->edges ? -1 : 1;
    }
    if (xa->code != xb->code) {
        return xa->code < xb->code ? -1 : 1;
    }
    return 0;
}

static void canon_orbits(const tab_ctx *tc, canon_entry *ce) {
    uint8_t rep[GLET_MAX_SIZE];
    for (unsigned ps = 0; ps < tc->size; ps++) {
        rep[ps] = (uint8_t)ps;
    }
    for (unsigned k = 0; k < tc->perms.count; k++) {
        const uint8_t *perm = tc->perms.perm[k];
        if (permute_code(tc, ce->code, perm) != ce->code) {
            continue;
        }
        for (unsigned ps = 0; ps < tc->size; ps++) {
            rep[ps] = perm[ps] < rep[ps] ? perm[ps] : rep[ps];
        }
    }
    ce->n_local = 0;
    for (unsigned ps = 0; ps < tc->size; ps++) {
        if (rep[ps] == ps) {
            ce->local[ps] = (uint8_t)ce->n_local;
            ce->mult[ce->n_local++] = 0;
        }
    }
    for (unsigned ps = 0; ps < tc->size; ps++) {
        ce->local[ps] = ce->local[rep[ps]];
        ce->mult[ce->local[ps]]++;
    }
}

/* Finds the canonical code of a code and the permutation reaching it. */
static unsigned canon_of(const tab_ctx *tc, unsigned code, const uint8_t **perm) {
    unsigned best = ~0U;
    for (unsigned k = 0; k < tc->perms.count; k++) {
        unsigned val = permute_code(tc, code, tc->perms.perm[k]);
        if (val < best) {
            best = val;
            *perm = tc->perms.perm[k];
        }
    }
    return best;
}

static unsigned max_code(const tab_ctx *tc) {
    return 1U << (tc->bits * tc->pairs);
}

typedef struct size_build {
    canon_entry ents[MAX_CANON];
    unsigned n_ents;
} size_build;

static void collect_canon(const tab_ctx *tc, size_build *sb) {
    sb->n_ents = 0;
    const uint8_t *perm = NULL;
    for (unsigned code = 0; code < max_code(tc); code++) {
        if (!code_oriented(tc, code) || !code_connected(tc, code) ||
            canon_of(tc, code, &perm) != code) {
            continue;
        }
        canon_entry *ce = &sb->ents[sb->n_ents++];
        ce->size = tc->size;
        ce->code = code;
        ce->edges = code_edges(tc, code);
    }
    qsort(sb->ents, sb->n_ents, sizeof sb->ents[0], canon_cmp);
}

/* Renames mine -> Przulj for the undirected orbit ids; identity otherwise. */
static unsigned rename_orbit(bool directed, unsigned orbit) {
    return directed ? orbit : k_przulj[orbit];
}

static unsigned first_pos(const canon_entry *ce, unsigned local) {
    for (unsigned ps = 0; ps < ce->size; ps++) {
        if (ce->local[ps] == local) {
            return ps;
        }
    }
    return 0;
}

typedef struct next_ids {
    unsigned graphlet;
    unsigned orbit;
} next_ids;

static void fill_size(tab_ctx *tc, size_build *sb, next_ids *next) {
    unsigned n_g = next->graphlet;
    for (unsigned gr = 0; gr < sb->n_ents; gr++) {
        canon_orbits(tc, &sb->ents[gr]);
        sb->ents[gr].first_graphlet = next->graphlet + gr;
        sb->ents[gr].first_orbit = next->orbit;
        for (unsigned lv = 0; lv < sb->ents[gr].n_local; lv++) {
            glet_orbit_info *info = tc->directed
                                        ? &g_glet_tab.info_d[next->orbit + lv]
                                        : &g_glet_tab.info_u[rename_orbit(false, next->orbit + lv)];
            info->graphlet = next->graphlet + gr;
            info->nodes = tc->size;
            info->edges = sb->ents[gr].edges;
            info->mult = sb->ents[gr].mult[lv];
            info->code = sb->ents[gr].code;
            info->pos = first_pos(&sb->ents[gr], lv);
        }
        next->orbit += sb->ents[gr].n_local;
    }
    next->graphlet = n_g + sb->n_ents;
}

static void fill_codes(const tab_ctx *tc, const size_build *sb, uint8_t *table) {
    const uint8_t *perm = NULL;
    for (unsigned code = 0; code < max_code(tc); code++) {
        uint8_t *row = table + (size_t)code * tc->size;
        memset(row, NONE_ORBIT, tc->size);
        if (!code_oriented(tc, code) || !code_connected(tc, code)) {
            continue;
        }
        unsigned canon = canon_of(tc, code, &perm);
        for (unsigned gr = 0; gr < sb->n_ents; gr++) {
            if (sb->ents[gr].code != canon) {
                continue;
            }
            for (unsigned ii = 0; ii < tc->size; ii++) {
                unsigned loc = sb->ents[gr].local[perm[ii]];
                row[ii] = (uint8_t)rename_orbit(tc->directed, sb->ents[gr].first_orbit + loc);
            }
        }
    }
}

static void build_mode(bool directed, unsigned max_size, uint8_t *tables[]) {
    next_ids next = {0, 0};
    for (unsigned sz = 2; sz <= max_size; sz++) {
        tab_ctx tc;
        memset(&tc, 0, sizeof tc);
        tc.directed = directed;
        tc.bits = directed ? 2U : 1U;
        tc.size = sz;
        tc.pairs = sz * (sz - 1) / 2;
        perms_gen(&tc.perms, sz);
        size_build *sb = calloc(1, sizeof *sb);
        if (sb == NULL) {
            return;
        }
        collect_canon(&tc, sb);
        fill_size(&tc, sb, &next);
        fill_codes(&tc, sb, tables[sz]);
        free(sb);
    }
}

void glet_tables_init(void) {
    if (g_built) {
        return;
    }
    uint8_t *ut[GLET_MAX_SIZE + 1];
    uint8_t *dt[GLET_MAX_DIR_SIZE + 1];
    for (unsigned sz = 2; sz <= GLET_MAX_SIZE; sz++) {
        ut[sz] = g_glet_tab.u_orb[sz];
    }
    for (unsigned sz = 2; sz <= GLET_MAX_DIR_SIZE; sz++) {
        dt[sz] = g_glet_tab.d_orb[sz];
    }
    build_mode(false, GLET_MAX_SIZE, ut);
    build_mode(true, GLET_MAX_DIR_SIZE, dt);
    g_built = true;
}

uint32_t glet_orbit_count(bool directed, unsigned size) {
    if (directed) {
        return size >= 2 && size <= GLET_MAX_DIR_SIZE ? GLET_ORBITS_D4 : 0;
    }
    if (size == GLET_MAX_SIZE) {
        return GLET_ORBITS_U5;
    }
    return size == GLET_MAX_SIZE - 1 ? GLET_ORBITS_U4 : 0;
}

uint32_t glet_graphlet_count(bool directed, unsigned size) {
    if (directed) {
        return size >= 2 && size <= GLET_MAX_DIR_SIZE ? GLET_GRAPHLETS_D4 : 0;
    }
    if (size == GLET_MAX_SIZE) {
        return GLET_GRAPHLETS_U5;
    }
    return size == GLET_MAX_SIZE - 1 ? GLET_GRAPHLETS_U4 : 0;
}

bool glet_orbit_describe(bool directed, uint32_t orbit, glet_orbit_info *out) {
    glet_tables_init();
    if (orbit >= (directed ? (uint32_t)GLET_ORBITS_D4 : (uint32_t)GLET_ORBITS_U5)) {
        return false;
    }
    *out = directed ? g_glet_tab.info_d[orbit] : g_glet_tab.info_u[orbit];
    return true;
}
