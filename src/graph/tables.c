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

enum { MAX_PERMS = 120, MAX_PAIRS = 10, NONE_ORBIT = 0xFF, MAX_CODES_U = 1024, MAX_CODES_D = 4096 };

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

static unsigned pair_index(unsigned i, unsigned j) {
    return j * (j - 1) / 2 + i;
}

static void perms_gen(perm_set *set, unsigned size) {
    uint8_t cur[GLET_MAX_SIZE] = {0};
    for (unsigned i = 0; i < size; i++) {
        cur[i] = (uint8_t)i;
    }
    set->count = 0;
    for (;;) {
        memcpy(set->perm[set->count++], cur, size);
        int i = (int)size - 2;
        while (i >= 0 && cur[i] > cur[i + 1]) {
            i--;
        }
        if (i < 0) {
            return;
        }
        int j = (int)size - 1;
        while (cur[j] < cur[i]) {
            j--;
        }
        uint8_t tmp = cur[i];
        cur[i] = cur[j];
        cur[j] = tmp;
        for (int a = i + 1, b = (int)size - 1; a < b; a++, b--) {
            tmp = cur[a];
            cur[a] = cur[b];
            cur[b] = tmp;
        }
    }
}

static unsigned permute_code(const tab_ctx *tc, unsigned code, const uint8_t *perm) {
    unsigned mask = (1U << tc->bits) - 1U;
    unsigned out = 0;
    for (unsigned j = 1; j < tc->size; j++) {
        for (unsigned i = 0; i < j; i++) {
            unsigned st = (code >> (tc->bits * pair_index(i, j))) & mask;
            unsigned a = perm[i];
            unsigned b = perm[j];
            if (a > b) {
                unsigned tmp = a;
                a = b;
                b = tmp;
                st = tc->bits == 2 ? ((st & 1U) << 1) | (st >> 1) : st;
            }
            out |= st << (tc->bits * pair_index(a, b));
        }
    }
    return out;
}

/* True when the code has no state 3 (directed) . */
static bool code_oriented(const tab_ctx *tc, unsigned code) {
    for (unsigned p = 0; tc->bits == 2 && p < tc->pairs; p++) {
        if (((code >> (2 * p)) & 3U) == 3U) {
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
        for (unsigned j = 1; j < tc->size; j++) {
            for (unsigned i = 0; i < j; i++) {
                bool edge = ((code >> (tc->bits * pair_index(i, j))) & mask) != 0;
                bool si = ((seen >> i) & 1U) != 0;
                bool sj = ((seen >> j) & 1U) != 0;
                if (edge && si != sj) {
                    seen |= (1U << i) | (1U << j);
                    grew = true;
                }
            }
        }
    }
    return seen == (1U << tc->size) - 1U;
}

static unsigned code_edges(const tab_ctx *tc, unsigned code) {
    unsigned n = 0;
    for (unsigned b = 0; b < tc->bits * tc->pairs; b++) {
        n += (code >> b) & 1U;
    }
    return n;
}

/* One graphlet of one size: its canonical code and the orbit of each canonical position. */
typedef struct canon_entry {
    unsigned code;
    unsigned edges;
    uint8_t local[GLET_MAX_SIZE]; /* local orbit index of each position */
    unsigned n_local;
    unsigned mult[GLET_MAX_SIZE];
    unsigned first_graphlet;
    unsigned first_orbit;
} canon_entry;

static int canon_cmp(const void *lhs, const void *rhs) {
    const canon_entry *a = lhs;
    const canon_entry *b = rhs;
    if (a->edges != b->edges) {
        return a->edges < b->edges ? -1 : 1;
    }
    return a->code < b->code ? -1 : (a->code > b->code ? 1 : 0);
}

static void canon_orbits(const tab_ctx *tc, canon_entry *ce) {
    uint8_t rep[GLET_MAX_SIZE];
    for (unsigned p = 0; p < tc->size; p++) {
        rep[p] = (uint8_t)p;
    }
    for (unsigned k = 0; k < tc->perms.count; k++) {
        const uint8_t *perm = tc->perms.perm[k];
        if (permute_code(tc, ce->code, perm) != ce->code) {
            continue;
        }
        for (unsigned p = 0; p < tc->size; p++) {
            rep[p] = perm[p] < rep[p] ? perm[p] : rep[p];
        }
    }
    ce->n_local = 0;
    for (unsigned p = 0; p < tc->size; p++) {
        if (rep[p] == p) {
            ce->local[p] = (uint8_t)ce->n_local;
            ce->mult[ce->n_local++] = 0;
        }
    }
    for (unsigned p = 0; p < tc->size; p++) {
        ce->local[p] = ce->local[rep[p]];
        ce->mult[ce->local[p]]++;
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
    canon_entry ents[64];
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
        ce->code = code;
        ce->edges = code_edges(tc, code);
    }
    qsort(sb->ents, sb->n_ents, sizeof sb->ents[0], canon_cmp);
}

/* Renames mine -> Przulj for the undirected orbit ids; identity otherwise. */
static unsigned rename_orbit(bool directed, unsigned orbit) {
    return directed ? orbit : k_przulj[orbit];
}

static unsigned first_pos(const canon_entry *ce, unsigned local, unsigned size) {
    for (unsigned p = 0; p < size; p++) {
        if (ce->local[p] == local) {
            return p;
        }
    }
    return 0;
}

static void fill_size(tab_ctx *tc, size_build *sb, unsigned *graphlet, unsigned *orbit) {
    unsigned n_g = *graphlet;
    for (unsigned g = 0; g < sb->n_ents; g++) {
        canon_orbits(tc, &sb->ents[g]);
        sb->ents[g].first_graphlet = *graphlet + g;
        sb->ents[g].first_orbit = *orbit;
        for (unsigned l = 0; l < sb->ents[g].n_local; l++) {
            glet_orbit_info *info = tc->directed
                                        ? &g_glet_tab.info_d[*orbit + l]
                                        : &g_glet_tab.info_u[rename_orbit(false, *orbit + l)];
            info->graphlet = *graphlet + g;
            info->nodes = tc->size;
            info->edges = sb->ents[g].edges;
            info->mult = sb->ents[g].mult[l];
            info->code = sb->ents[g].code;
            info->pos = first_pos(&sb->ents[g], l, tc->size);
        }
        *orbit += sb->ents[g].n_local;
    }
    *graphlet = n_g + sb->n_ents;
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
        for (unsigned g = 0; g < sb->n_ents; g++) {
            if (sb->ents[g].code != canon) {
                continue;
            }
            for (unsigned i = 0; i < tc->size; i++) {
                unsigned loc = sb->ents[g].local[perm[i]];
                row[i] = (uint8_t)rename_orbit(tc->directed, sb->ents[g].first_orbit + loc);
            }
        }
    }
}

static void build_mode(bool directed, unsigned max_size, uint8_t *tables[]) {
    unsigned graphlet = 0;
    unsigned orbit = 0;
    for (unsigned s = 2; s <= max_size; s++) {
        tab_ctx tc;
        memset(&tc, 0, sizeof tc);
        tc.directed = directed;
        tc.bits = directed ? 2U : 1U;
        tc.size = s;
        tc.pairs = s * (s - 1) / 2;
        perms_gen(&tc.perms, s);
        size_build *sb = calloc(1, sizeof *sb);
        if (sb == NULL) {
            return;
        }
        collect_canon(&tc, sb);
        fill_size(&tc, sb, &graphlet, &orbit);
        fill_codes(&tc, sb, tables[s]);
        free(sb);
    }
}

void glet_tables_init(void) {
    if (g_built) {
        return;
    }
    uint8_t *ut[GLET_MAX_SIZE + 1];
    uint8_t *dt[GLET_MAX_DIR_SIZE + 1];
    for (unsigned s = 2; s <= GLET_MAX_SIZE; s++) {
        ut[s] = g_glet_tab.u_orb[s];
    }
    for (unsigned s = 2; s <= GLET_MAX_DIR_SIZE; s++) {
        dt[s] = g_glet_tab.d_orb[s];
    }
    build_mode(false, GLET_MAX_SIZE, ut);
    build_mode(true, GLET_MAX_DIR_SIZE, dt);
    g_built = true;
}

uint32_t glet_orbit_count(bool directed, unsigned size) {
    if (directed) {
        return size >= 2 && size <= GLET_MAX_DIR_SIZE ? GLET_ORBITS_D4 : 0;
    }
    return size == GLET_MAX_SIZE ? GLET_ORBITS_U5
                                 : (size == GLET_MAX_SIZE - 1 ? GLET_ORBITS_U4 : 0);
}

uint32_t glet_graphlet_count(bool directed, unsigned size) {
    if (directed) {
        return size >= 2 && size <= GLET_MAX_DIR_SIZE ? GLET_GRAPHLETS_D4 : 0;
    }
    return size == GLET_MAX_SIZE ? GLET_GRAPHLETS_U5
                                 : (size == GLET_MAX_SIZE - 1 ? GLET_GRAPHLETS_U4 : 0);
}

bool glet_orbit_describe(bool directed, uint32_t orbit, glet_orbit_info *out) {
    glet_tables_init();
    if (orbit >= (directed ? (uint32_t)GLET_ORBITS_D4 : (uint32_t)GLET_ORBITS_U5)) {
        return false;
    }
    *out = directed ? g_glet_tab.info_d[orbit] : g_glet_tab.info_u[orbit];
    return true;
}
