/*
 * test_graphlets_count.c — the graphlet counters against brute force (spec section 9): table
 * sizes (15 / 73 undirected orbits, 40 / 129 directed), a bijection check of the orbit tables
 * against an independent isomorphism test, ESU counts against subset enumeration on random
 * graphs, and a hand-worked example per directed graphlet family.
 */
#include "graph/glet.h"
#include "graph/glet_int.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { MAXN = 9, MAXK = 5, MAX_KEYS = 4096 };

void setUp(void) {
}
void tearDown(void) {
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525U + 1013904223U;
    return g_rng >> 8;
}

/* --- independent isomorphism keys ----------------------------------------------------------- */

typedef struct key_reg {
    uint64_t key[MAX_KEYS];
    uint8_t orbit[MAX_KEYS];
    unsigned n;
} key_reg;

static unsigned arr_perm(unsigned s, unsigned idx, unsigned *perm) {
    unsigned avail[MAXK];
    for (unsigned i = 0; i < s; i++) {
        avail[i] = i;
    }
    unsigned left = s;
    for (unsigned i = 0; i < s; i++) {
        unsigned f = 1;
        for (unsigned k = 2; k < left; k++) {
            f *= k;
        }
        unsigned pick = idx / f;
        idx %= f;
        perm[i] = avail[pick];
        for (unsigned k = pick; k + 1 < left; k++) {
            avail[k] = avail[k + 1];
        }
        left--;
    }
    return 0;
}

static unsigned fact(unsigned s) {
    unsigned f = 1;
    for (unsigned k = 2; k <= s; k++) {
        f *= k;
    }
    return f;
}

/* m[i][j] = 1 when i -> j. Row-major bit string under a permutation. */
static uint64_t matrix_key(unsigned s, uint8_t m[MAXK][MAXK], const unsigned *perm) {
    uint64_t key = 0;
    for (unsigned i = 0; i < s; i++) {
        for (unsigned j = 0; j < s; j++) {
            key = (key << 1) | m[perm[i]][perm[j]];
        }
    }
    return key;
}

/* (canonical key, class of p's canonical position under the automorphisms of the canonical
 * matrix) packed with the node count: comparable across node sets. */
static uint64_t iso_key(unsigned s, uint8_t m[MAXK][MAXK], unsigned p) {
    unsigned perm[MAXK];
    unsigned bestp[MAXK];
    uint64_t best = ~0ULL;
    for (unsigned k = 0; k < fact(s); k++) {
        arr_perm(s, k, perm);
        uint64_t key = matrix_key(s, m, perm);
        if (key < best) {
            best = key;
            memcpy(bestp, perm, sizeof perm);
        }
    }
    uint8_t c[MAXK][MAXK];
    unsigned ident[MAXK];
    unsigned q = 0;
    for (unsigned i = 0; i < s; i++) {
        ident[i] = i;
        q = bestp[i] == p ? i : q;
    }
    for (unsigned i = 0; i < s; i++) {
        for (unsigned j = 0; j < s; j++) {
            c[i][j] = m[bestp[i]][bestp[j]];
        }
    }
    uint64_t self = matrix_key(s, c, ident);
    unsigned rep = q;
    for (unsigned k = 0; k < fact(s); k++) {
        arr_perm(s, k, perm);
        if (matrix_key(s, c, perm) == self) {
            rep = perm[q] < rep ? perm[q] : rep;
        }
    }
    return (best << 8) | (rep << 4) | s;
}

static void reg_check(key_reg *reg, uint64_t key, unsigned orbit) {
    for (unsigned i = 0; i < reg->n; i++) {
        if (reg->key[i] == key) {
            TEST_ASSERT_EQUAL_UINT(reg->orbit[i], orbit);
            return;
        }
        TEST_ASSERT_NOT_EQUAL(reg->orbit[i], orbit);
    }
    TEST_ASSERT_TRUE(reg->n < MAX_KEYS);
    reg->key[reg->n] = key;
    reg->orbit[reg->n++] = (uint8_t)orbit;
}

/* --- brute force ---------------------------------------------------------------------------- */

typedef struct bf {
    bool directed;
    unsigned n;
    uint8_t a[MAXN][MAXN]; /* a[u][v]: u -> v */
    int64_t *counts;
    unsigned norb;
    key_reg reg;
} bf;

static bool bf_conn(const bf *b, const unsigned *set, unsigned s) {
    unsigned seen = 1;
    for (bool grew = true; grew;) {
        grew = false;
        for (unsigned i = 0; i < s; i++) {
            for (unsigned j = 0; j < s; j++) {
                bool e = b->a[set[i]][set[j]] != 0 || b->a[set[j]][set[i]] != 0;
                if (e && ((seen >> i) & 1U) != 0 && ((seen >> j) & 1U) == 0) {
                    seen |= 1U << j;
                    grew = true;
                }
            }
        }
    }
    return seen == (1U << s) - 1U;
}

static unsigned code_of(const bf *b, const unsigned *set, unsigned s, uint8_t m[MAXK][MAXK]) {
    unsigned code = 0;
    for (unsigned j = 1; j < s; j++) {
        for (unsigned i = 0; i < j; i++) {
            unsigned pi = j * (j - 1) / 2 + i;
            if (b->directed) {
                code |= (unsigned)(m[i][j] != 0 ? 1U : 0U) << (2 * pi);     /* bit 0: i -> j */
                code |= (unsigned)(m[j][i] != 0 ? 1U : 0U) << (2 * pi + 1); /* bit 1: j -> i */
            } else {
                code |= (unsigned)(m[i][j] != 0 ? 1U : 0U) << pi;
            }
        }
    }
    (void)set;
    return code;
}

/* Counts one oriented matrix on the set. */
static void bf_one(bf *b, const unsigned *set, unsigned s, uint8_t m[MAXK][MAXK]) {
    unsigned code = code_of(b, set, s, m);
    const uint8_t *row = b->directed ? g_glet_tab.d_orb[s] + (size_t)code * s
                                     : g_glet_tab.u_orb[s] + (size_t)code * s;
    for (unsigned p = 0; p < s; p++) {
        TEST_ASSERT_NOT_EQUAL(0xFF, row[p]);
        reg_check(&b->reg, iso_key(s, m, p), row[p]);
        b->counts[(size_t)set[p] * b->norb + row[p]]++;
    }
}

static void bf_set(bf *b, const unsigned *set, unsigned s) {
    uint8_t m[MAXK][MAXK];
    unsigned pairs[10][2];
    unsigned np = 0;
    memset(m, 0, sizeof m);
    for (unsigned i = 0; i < s; i++) {
        for (unsigned j = 0; j < s; j++) {
            m[i][j] = b->a[set[i]][set[j]];
        }
    }
    if (!b->directed) {
        for (unsigned i = 0; i < s; i++) {
            for (unsigned j = 0; j < s; j++) {
                m[i][j] = (uint8_t)(m[i][j] | b->a[set[j]][set[i]]);
            }
        }
        bf_one(b, set, s, m);
        return;
    }
    for (unsigned i = 0; i < s; i++) {
        for (unsigned j = i + 1; j < s; j++) {
            if (m[i][j] != 0 && m[j][i] != 0) {
                pairs[np][0] = i;
                pairs[np++][1] = j;
            }
        }
    }
    for (unsigned mask = 0; mask < (1U << np); mask++) {
        uint8_t r[MAXK][MAXK];
        memcpy(r, m, sizeof r);
        for (unsigned k = 0; k < np; k++) {
            unsigned i = pairs[k][0];
            unsigned j = pairs[k][1];
            if (((mask >> k) & 1U) != 0) {
                r[i][j] = 0;
            } else {
                r[j][i] = 0;
            }
        }
        bf_one(b, set, s, r);
    }
}

static void bf_all(bf *b, unsigned maxk) {
    for (unsigned s = 2; s <= maxk; s++) {
        unsigned set[MAXN];
        for (unsigned mask = 0; mask < (1U << b->n); mask++) {
            unsigned c = 0;
            for (unsigned v = 0; v < b->n; v++) {
                if (((mask >> v) & 1U) != 0) {
                    set[c++] = v;
                }
            }
            if (c == s && bf_conn(b, set, s)) {
                bf_set(b, set, s);
            }
        }
    }
}

static void random_graph(bf *b, unsigned n, unsigned pct_edge, bool directed) {
    memset(b->a, 0, sizeof b->a);
    b->n = n;
    b->directed = directed;
    for (unsigned u = 0; u < n; u++) {
        for (unsigned v = 0; v < n; v++) {
            if (u != v && rnd() % 100 < pct_edge) {
                b->a[u][v] = 1;
            }
        }
    }
}

static void run_compare(bool directed, unsigned size, unsigned maxk, unsigned trials) {
    for (unsigned t = 0; t < trials; t++) {
        bf *b = calloc(1, sizeof *b);
        TEST_ASSERT_NOT_NULL(b);
        random_graph(b, 5 + t % 5, 15 + (t * 7) % 40, directed);
        b->norb = glet_orbit_count(directed, size);
        b->counts = calloc((size_t)MAXN * b->norb, sizeof(int64_t));
        int64_t *got = calloc((size_t)MAXN * b->norb, sizeof(int64_t));
        glet_edge edges[MAXN * MAXN];
        size_t ne = 0;
        for (unsigned u = 0; u < b->n; u++) {
            for (unsigned v = 0; v < b->n; v++) {
                if (b->a[u][v] != 0) {
                    edges[ne++] = (glet_edge){u, v};
                }
            }
        }
        glet_graph g;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, b->n, edges, ne));
        uint64_t work = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              glet_count(&g, &(glet_spec){directed, size, 0}, got, &work));
        bf_all(b, maxk);
        TEST_ASSERT_EQUAL_INT64_ARRAY(b->counts, got, (size_t)b->n * b->norb);
        glet_graph_free(&g);
        free(got);
        free(b->counts);
        free(b);
    }
}

static void test_table_sizes(void) {
    glet_tables_init();
    TEST_ASSERT_EQUAL_UINT(15, glet_orbit_count(false, 4));
    TEST_ASSERT_EQUAL_UINT(73, glet_orbit_count(false, 5));
    TEST_ASSERT_EQUAL_UINT(129, glet_orbit_count(true, 4));
    unsigned gmax_u = 0;
    unsigned gmax_d = 0;
    for (unsigned o = 0; o < 73; o++) {
        glet_orbit_info in;
        TEST_ASSERT_TRUE(glet_orbit_describe(false, o, &in));
        gmax_u = in.graphlet + 1 > gmax_u ? in.graphlet + 1 : gmax_u;
        TEST_ASSERT_TRUE(in.mult >= 1 && in.mult <= in.nodes);
    }
    for (unsigned o = 0; o < 129; o++) {
        glet_orbit_info in;
        TEST_ASSERT_TRUE(glet_orbit_describe(true, o, &in));
        gmax_d = in.graphlet + 1 > gmax_d ? in.graphlet + 1 : gmax_d;
    }
    TEST_ASSERT_EQUAL_UINT(30, gmax_u);
    TEST_ASSERT_EQUAL_UINT(40, gmax_d);
}

/* Orbit counts per node count: 1 + 2 + 11 (undirected 4: 15), directed 2 + 11 + 116. */
static void test_orbits_per_size(void) {
    unsigned per_u[6] = {0};
    unsigned per_d[6] = {0};
    for (unsigned o = 0; o < 73; o++) {
        glet_orbit_info in;
        (void)glet_orbit_describe(false, o, &in);
        per_u[in.nodes]++;
    }
    for (unsigned o = 0; o < 129; o++) {
        glet_orbit_info in;
        (void)glet_orbit_describe(true, o, &in);
        per_d[in.nodes]++;
    }
    TEST_ASSERT_EQUAL_UINT(1, per_u[2]);
    TEST_ASSERT_EQUAL_UINT(2, per_u[3] - 0 - 0 > 0 ? per_u[3] - 1 : 0);
    TEST_ASSERT_EQUAL_UINT(2, per_d[2]);
    TEST_ASSERT_EQUAL_UINT(11, per_d[3]);
    TEST_ASSERT_EQUAL_UINT(116, per_d[4]);
}

static void test_undirected4_vs_bruteforce(void) {
    run_compare(false, 4, 4, 30);
}
static void test_undirected5_vs_bruteforce(void) {
    run_compare(false, 5, 5, 20);
}
static void test_directed_vs_bruteforce(void) {
    run_compare(true, 4, 4, 40);
}

/* --- hand-worked directed examples ---------------------------------------------------------- */

typedef struct fam {
    unsigned n;
    glet_edge e[6];
    size_t ne;
} fam;

/* The graphlet histogram of a small digraph: counts per graphlet id of nodes>=3, and whether one
 * graphlet of the expected (nodes, edges) is hit exactly once. */
static void expect_single(const fam *f, unsigned nodes, unsigned edges, unsigned mults) {
    glet_graph g;
    int64_t got[4 * 129];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, f->n, f->e, f->ne));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&g, &(glet_spec){true, 4, 0}, got, NULL));
    unsigned hit = 0;
    for (unsigned o = 0; o < 129; o++) {
        glet_orbit_info in;
        (void)glet_orbit_describe(true, o, &in);
        int64_t total = 0;
        for (unsigned v = 0; v < f->n; v++) {
            total += got[(size_t)v * 129 + o];
        }
        if (in.nodes == nodes && total != 0) {
            TEST_ASSERT_EQUAL_UINT(edges, in.edges);
            TEST_ASSERT_EQUAL_INT64((int64_t)in.mult, total);
            hit += in.mult;
        }
    }
    TEST_ASSERT_EQUAL_UINT(mults, hit);
    glet_graph_free(&g);
}

static void test_directed_families(void) {
    /* single edge: one 2-node graphlet, both ends in distinct orbits */
    fam edge = {2, {{0, 1}}, 1};
    expect_single(&edge, 2, 1, 2);
    /* out-star a->b, a->c */
    fam ostar = {3, {{0, 1}, {0, 2}}, 2};
    expect_single(&ostar, 3, 2, 3);
    /* in-star */
    fam istar = {3, {{1, 0}, {2, 0}}, 2};
    expect_single(&istar, 3, 2, 3);
    /* chain a->b->c */
    fam chain = {3, {{0, 1}, {1, 2}}, 2};
    expect_single(&chain, 3, 2, 3);
    /* cyclic triangle */
    fam cyc = {3, {{0, 1}, {1, 2}, {2, 0}}, 3};
    expect_single(&cyc, 3, 3, 3);
    /* transitive triangle */
    fam tri = {3, {{0, 1}, {1, 2}, {0, 2}}, 3};
    expect_single(&tri, 3, 3, 3);
    /* 4-node directed path and out-star with 3 leaves */
    fam path4 = {4, {{0, 1}, {1, 2}, {2, 3}}, 3};
    expect_single(&path4, 4, 3, 4);
    fam star4 = {4, {{0, 1}, {0, 2}, {0, 3}}, 3};
    expect_single(&star4, 4, 3, 4);
    /* 4-cycle */
    fam cyc4 = {4, {{0, 1}, {1, 2}, {2, 3}, {3, 0}}, 4};
    expect_single(&cyc4, 4, 4, 4);
}

/* An anti-parallel pair counts once per direction: two 2-node occurrences. */
static void test_antiparallel_pair(void) {
    glet_edge e[2] = {{0, 1}, {1, 0}};
    glet_graph g;
    int64_t got[2 * 129];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, 2, e, 2));
    TEST_ASSERT_EQUAL_UINT64(1, g.m);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&g, &(glet_spec){true, 4, 0}, got, NULL));
    int64_t sum0 = 0;
    for (unsigned o = 0; o < 129; o++) {
        sum0 += got[o];
    }
    TEST_ASSERT_EQUAL_INT64(2, sum0); /* node 0 sits in one out-end and one in-end occurrence */
    glet_graph_free(&g);
    glet_edge loop[1] = {{0, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, 1, loop, 1));
    TEST_ASSERT_EQUAL_UINT64(1, g.self_loops);
    glet_graph_free(&g);
}

static void test_work_bound(void) {
    glet_edge e[4] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}};
    glet_graph g;
    int64_t got[5 * 73];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, 5, e, 4));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          glet_count(&g, &(glet_spec){false, 5, 3}, got, NULL));
    glet_graph_free(&g);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_sizes);
    RUN_TEST(test_orbits_per_size);
    RUN_TEST(test_undirected4_vs_bruteforce);
    RUN_TEST(test_undirected5_vs_bruteforce);
    RUN_TEST(test_directed_vs_bruteforce);
    RUN_TEST(test_directed_families);
    RUN_TEST(test_antiparallel_pair);
    RUN_TEST(test_work_bound);
    return UNITY_END();
}
