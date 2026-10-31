/*
 * test_graphlets_count.c — the graphlet counters against brute force (spec section 9 and 17):
 * table sizes (15 / 73 undirected orbits, 40 / 129 directed), a bijection check of the orbit
 * tables against an independent isomorphism test, ESU counts against subset enumeration on random
 * graphs, a catalogue check by recomputing every orbit class, and a hand-worked example per
 * directed graphlet family.
 */
#include "graph/glet.h"
#include "graph/glet_int.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { MAXN = 9, MAXK = 5, MAX_KEYS = 4096, ORBITS_D = 129, ORBITS_U = 73, GRAPHLETS_MAX = 64 };

void setUp(void) {
}
void tearDown(void) {
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525U + 1013904223U;
    return g_rng >> 8;
}

/* --- small matrices and an exhaustive isomorphism test -----------------------------------------
 */

typedef struct mat {
    unsigned size;
    uint8_t cell[MAXK][MAXK]; /* cell[i][j] = 1 when i -> j */
} mat;

static unsigned fact(unsigned count) {
    unsigned prod = 1;
    for (unsigned k = 2; k <= count; k++) {
        prod *= k;
    }
    return prod;
}

/* The idx-th permutation of 0 .. size-1 (factorial number system). */
static void nth_perm(const mat *mt, unsigned idx, unsigned *perm) {
    unsigned avail[MAXK];
    for (unsigned i = 0; i < mt->size; i++) {
        avail[i] = i;
    }
    unsigned left = mt->size;
    for (unsigned i = 0; i < mt->size; i++) {
        unsigned weight = fact(left - 1);
        unsigned pick = idx / weight;
        idx %= weight;
        perm[i] = avail[pick];
        for (unsigned k = pick; k + 1 < left; k++) {
            avail[k] = avail[k + 1];
        }
        left--;
    }
}

/* Row-major bit string of the matrix relabelled by perm. */
static uint64_t matrix_key(const mat *mt, const unsigned *perm) {
    uint64_t key = 0;
    for (unsigned i = 0; i < mt->size; i++) {
        for (unsigned j = 0; j < mt->size; j++) {
            key = (key << 1) | mt->cell[perm[i]][perm[j]];
        }
    }
    return key;
}

/* The smallest key over all relabellings; bestp receives the permutation reaching it. */
static uint64_t canonical_key(const mat *mt, unsigned *bestp) {
    unsigned perm[MAXK];
    uint64_t best = ~0ULL;
    for (unsigned k = 0; k < fact(mt->size); k++) {
        nth_perm(mt, k, perm);
        uint64_t key = matrix_key(mt, perm);
        if (key < best) {
            best = key;
            memcpy(bestp, perm, sizeof perm);
        }
    }
    return best;
}

/* Smallest image of position pos under the automorphisms of mt. */
static unsigned aut_class(const mat *mt, unsigned pos) {
    unsigned perm[MAXK];
    unsigned ident[MAXK];
    for (unsigned i = 0; i < mt->size; i++) {
        ident[i] = i;
    }
    uint64_t self = matrix_key(mt, ident);
    unsigned rep = pos;
    for (unsigned k = 0; k < fact(mt->size); k++) {
        nth_perm(mt, k, perm);
        if (matrix_key(mt, perm) == self && perm[pos] < rep) {
            rep = perm[pos];
        }
    }
    return rep;
}

/* mt relabelled so that canonical position i holds node bestp[i]. */
static mat relabel(const mat *mt, const unsigned *bestp) {
    mat out;
    memset(&out, 0, sizeof out);
    out.size = mt->size;
    for (unsigned i = 0; i < mt->size; i++) {
        for (unsigned j = 0; j < mt->size; j++) {
            out.cell[i][j] = mt->cell[bestp[i]][bestp[j]];
        }
    }
    return out;
}

/* (canonical key, class of node's canonical position) packed with the node count: comparable
 * across node sets. */
static uint64_t iso_key(const mat *mt, unsigned node) {
    unsigned bestp[MAXK] = {0};
    uint64_t best = canonical_key(mt, bestp);
    unsigned pos = 0;
    for (unsigned i = 0; i < mt->size; i++) {
        pos = bestp[i] == node ? i : pos;
    }
    mat canon = relabel(mt, bestp);
    return (best << 8) | ((uint64_t)aut_class(&canon, pos) << 4) | mt->size;
}

static uint64_t graph_key(const mat *mt) {
    unsigned bestp[MAXK] = {0};
    return (canonical_key(mt, bestp) << 3) | mt->size;
}

/* mt from a table code (tables.c layout). */
static mat mat_from_code(bool directed, const glet_orbit_info *info) {
    mat out;
    memset(&out, 0, sizeof out);
    out.size = info->nodes;
    for (unsigned j = 1; j < info->nodes; j++) {
        for (unsigned i = 0; i < j; i++) {
            unsigned pair = j * (j - 1) / 2 + i;
            unsigned bits = directed ? 2U : 1U;
            unsigned state = (info->code >> (bits * pair)) & ((1U << bits) - 1U);
            out.cell[i][j] = (uint8_t)(state & 1U);
            out.cell[j][i] = (uint8_t)(directed ? (state >> 1) & 1U : state & 1U);
        }
    }
    return out;
}

static bool is_connected(const mat *mt) {
    unsigned seen = 1;
    for (bool grew = true; grew;) {
        grew = false;
        for (unsigned i = 0; i < mt->size; i++) {
            for (unsigned j = 0; j < mt->size; j++) {
                bool edge = mt->cell[i][j] != 0 || mt->cell[j][i] != 0;
                bool fresh = ((seen >> i) & 1U) != 0 && ((seen >> j) & 1U) == 0;
                seen |= edge && fresh ? 1U << j : 0U;
                grew = grew || (edge && fresh);
            }
        }
    }
    return seen == (1U << mt->size) - 1U;
}

/* --- bijection between brute-force keys and table orbit ids -------------------------------------
 */

typedef struct key_reg {
    uint64_t key[MAX_KEYS];
    uint8_t orbit[MAX_KEYS];
    unsigned count;
} key_reg;

typedef struct orbit_obs {
    uint64_t key;
    unsigned orbit;
} orbit_obs;

static void reg_check(key_reg *reg, const orbit_obs *obs) {
    for (unsigned i = 0; i < reg->count; i++) {
        if (reg->key[i] == obs->key) {
            TEST_ASSERT_EQUAL_UINT(reg->orbit[i], obs->orbit);
            return;
        }
        TEST_ASSERT_NOT_EQUAL(reg->orbit[i], obs->orbit);
    }
    TEST_ASSERT_TRUE(reg->count < MAX_KEYS);
    reg->key[reg->count] = obs->key;
    reg->orbit[reg->count++] = (uint8_t)obs->orbit;
}

/* --- brute force -------------------------------------------------------------------------------
 */

typedef struct brute {
    bool directed;
    unsigned nodes;
    uint8_t arc[MAXN][MAXN]; /* arc[u][v]: u -> v */
    int64_t *counts;
    unsigned norb;
    key_reg reg;
} brute;

static unsigned code_bits(bool directed, const mat *mt, unsigned ii, unsigned jj) {
    if (directed) {
        return (unsigned)mt->cell[ii][jj] | ((unsigned)mt->cell[jj][ii] << 1);
    }
    return mt->cell[ii][jj];
}

static unsigned mat_code(bool directed, const mat *mt) {
    unsigned code = 0;
    for (unsigned j = 1; j < mt->size; j++) {
        for (unsigned i = 0; i < j; i++) {
            code |= code_bits(directed, mt, i, j) << ((directed ? 2U : 1U) * (j * (j - 1) / 2 + i));
        }
    }
    return code;
}

/* Counts one (oriented) matrix whose rows are the nodes set[0 ..]. */
static void brute_one(brute *bf, const unsigned *set, const mat *mt) {
    unsigned code = mat_code(bf->directed, mt);
    const uint8_t *row = (bf->directed ? g_glet_tab.d_orb[mt->size] : g_glet_tab.u_orb[mt->size]) +
                         (size_t)code * mt->size;
    for (unsigned pos = 0; pos < mt->size; pos++) {
        TEST_ASSERT_NOT_EQUAL(0xFF, row[pos]);
        orbit_obs obs = {iso_key(mt, pos), row[pos]};
        reg_check(&bf->reg, &obs);
        bf->counts[(size_t)set[pos] * bf->norb + row[pos]]++;
    }
}

typedef struct bidir_pairs {
    unsigned pair[10][2];
    unsigned count;
} bidir_pairs;

static bidir_pairs find_bidir(const mat *mt) {
    bidir_pairs found = {{{0}}, 0};
    for (unsigned i = 0; i < mt->size; i++) {
        for (unsigned j = i + 1; j < mt->size; j++) {
            if (mt->cell[i][j] != 0 && mt->cell[j][i] != 0) {
                found.pair[found.count][0] = i;
                found.pair[found.count++][1] = j;
            }
        }
    }
    return found;
}

/* The matrix with one direction of every anti-parallel pair removed, chosen by mask. */
static mat orient(const mat *mt, const bidir_pairs *bp, unsigned mask) {
    mat res = *mt;
    for (unsigned k = 0; k < bp->count; k++) {
        unsigned lo = bp->pair[k][0];
        unsigned hi = bp->pair[k][1];
        bool keep_forward = ((mask >> k) & 1U) != 0;
        res.cell[keep_forward ? hi : lo][keep_forward ? lo : hi] = 0;
    }
    return res;
}

/* Every orientation that keeps one direction of each anti-parallel pair. */
static void brute_resolve(brute *bf, const unsigned *set, const mat *mt) {
    bidir_pairs bp = find_bidir(mt);
    for (unsigned mask = 0; mask < (1U << bp.count); mask++) {
        mat res = orient(mt, &bp, mask);
        brute_one(bf, set, &res);
    }
}

static void brute_set(brute *bf, const unsigned *set, unsigned size) {
    mat mt;
    memset(&mt, 0, sizeof mt);
    mt.size = size;
    for (unsigned i = 0; i < size; i++) {
        for (unsigned j = 0; j < size; j++) {
            mt.cell[i][j] = bf->arc[set[i]][set[j]];
        }
    }
    if (bf->directed) {
        brute_resolve(bf, set, &mt);
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        for (unsigned j = 0; j < size; j++) {
            mt.cell[i][j] = (uint8_t)(mt.cell[i][j] | bf->arc[set[j]][set[i]]);
        }
    }
    brute_one(bf, set, &mt);
}

/* The induced matrix of the set (arcs in either direction count for connectivity). */
static bool brute_connected(const brute *bf, const unsigned *set, unsigned size) {
    mat mt;
    memset(&mt, 0, sizeof mt);
    mt.size = size;
    for (unsigned i = 0; i < size; i++) {
        for (unsigned j = 0; j < size; j++) {
            mt.cell[i][j] = bf->arc[set[i]][set[j]];
        }
    }
    return is_connected(&mt);
}

static void brute_all(brute *bf, unsigned maxk) {
    for (unsigned size = 2; size <= maxk; size++) {
        for (unsigned mask = 0; mask < (1U << bf->nodes); mask++) {
            unsigned set[MAXN];
            unsigned count = 0;
            for (unsigned node = 0; node < bf->nodes; node++) {
                set[count] = node;
                count += ((mask >> node) & 1U) != 0 ? 1U : 0U;
            }
            if (count == size && brute_connected(bf, set, size)) {
                brute_set(bf, set, size);
            }
        }
    }
}

typedef struct cmp_case {
    bool directed;
    unsigned size;
    unsigned maxk;
    unsigned trials;
} cmp_case;

static void random_graph(brute *bf, const cmp_case *cc, unsigned trial) {
    memset(bf->arc, 0, sizeof bf->arc);
    bf->nodes = 5 + trial % 5;
    bf->directed = cc->directed;
    unsigned pct = 15 + (trial * 7) % 40;
    for (unsigned from = 0; from < bf->nodes; from++) {
        for (unsigned to = 0; to < bf->nodes; to++) {
            bf->arc[from][to] = (uint8_t)(from != to && rnd() % 100 < pct);
        }
    }
}

static void compare_one(const cmp_case *cc, unsigned trial) {
    brute *bf = calloc(1, sizeof *bf);
    TEST_ASSERT_NOT_NULL(bf);
    random_graph(bf, cc, trial);
    bf->norb = glet_orbit_count(cc->directed, cc->size);
    bf->counts = calloc((size_t)MAXN * bf->norb, sizeof(int64_t));
    int64_t *got = calloc((size_t)MAXN * bf->norb, sizeof(int64_t));
    glet_edge arcs[MAXN * MAXN];
    size_t count = 0;
    for (unsigned from = 0; from < bf->nodes; from++) {
        for (unsigned to = 0; to < bf->nodes; to++) {
            if (bf->arc[from][to] != 0) {
                arcs[count++] = (glet_edge){from, to};
            }
        }
    }
    glet_graph graph;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&graph, bf->nodes, arcs, count));
    uint64_t work = 0;
    glet_spec spec = {cc->directed, cc->size, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&graph, &spec, got, &work));
    brute_all(bf, cc->maxk);
    TEST_ASSERT_EQUAL_INT64_ARRAY(bf->counts, got, (size_t)bf->nodes * bf->norb);
    glet_graph_free(&graph);
    free(got);
    free(bf->counts);
    free(bf);
}

static void run_compare(const cmp_case *cc) {
    for (unsigned trial = 0; trial < cc->trials; trial++) {
        compare_one(cc, trial);
    }
}

/* --- table and catalogue checks ----------------------------------------------------------------
 */

static glet_orbit_info orbit_info(bool directed, unsigned orbit) {
    glet_orbit_info info;
    TEST_ASSERT_TRUE(glet_orbit_describe(directed, orbit, &info));
    TEST_ASSERT_TRUE(info.mult >= 1);
    TEST_ASSERT_TRUE(info.mult <= info.nodes);
    return info;
}

static unsigned graphlets_in(bool directed, unsigned n_orbits) {
    unsigned top = 0;
    for (unsigned orbit = 0; orbit < n_orbits; orbit++) {
        unsigned graphlet = orbit_info(directed, orbit).graphlet;
        top = graphlet + 1 > top ? graphlet + 1 : top;
    }
    return top;
}

static void test_table_sizes(void) {
    glet_tables_init();
    TEST_ASSERT_EQUAL_UINT(15, glet_orbit_count(false, 4));
    TEST_ASSERT_EQUAL_UINT(ORBITS_U, glet_orbit_count(false, 5));
    TEST_ASSERT_EQUAL_UINT(ORBITS_D, glet_orbit_count(true, 4));
    TEST_ASSERT_EQUAL_UINT(30, graphlets_in(false, ORBITS_U));
    TEST_ASSERT_EQUAL_UINT(40, graphlets_in(true, ORBITS_D));
}

static void count_per_size(bool directed, unsigned n_orbits, unsigned *per) {
    for (unsigned orbit = 0; orbit < n_orbits; orbit++) {
        glet_orbit_info info;
        (void)glet_orbit_describe(directed, orbit, &info);
        per[info.nodes]++;
    }
}

/* Orbits per node count: undirected 1 + 2 + 11 (+ 58 for five nodes), directed 2 + 11 + 116. */
static void test_orbits_per_size(void) {
    unsigned per_u[6] = {0};
    unsigned per_d[6] = {0};
    count_per_size(false, ORBITS_U, per_u);
    count_per_size(true, ORBITS_D, per_d);
    TEST_ASSERT_EQUAL_UINT(1, per_u[2]);
    TEST_ASSERT_EQUAL_UINT(3, per_u[3]);
    TEST_ASSERT_EQUAL_UINT(11, per_u[4]);
    TEST_ASSERT_EQUAL_UINT(58, per_u[5]);
    TEST_ASSERT_EQUAL_UINT(2, per_d[2]);
    TEST_ASSERT_EQUAL_UINT(11, per_d[3]);
    TEST_ASSERT_EQUAL_UINT(116, per_d[4]);
}

static void test_undirected4_vs_bruteforce(void) {
    run_compare(&(cmp_case){false, 4, 4, 150});
}
static void test_undirected5_vs_bruteforce(void) {
    run_compare(&(cmp_case){false, 5, 5, 20});
}
static void test_directed_vs_bruteforce(void) {
    run_compare(&(cmp_case){true, 4, 4, 600});
}

/* The orbit class and size of every catalogue entry, recomputed by exhaustive permutation. */
static void catalogue_orbit(bool directed, unsigned orbit, uint64_t *seen, unsigned *n_seen) {
    glet_orbit_info info;
    TEST_ASSERT_TRUE(glet_orbit_describe(directed, orbit, &info));
    mat mt = mat_from_code(directed, &info);
    unsigned cls = aut_class(&mt, info.pos);
    unsigned mult = 0;
    for (unsigned pos = 0; pos < info.nodes; pos++) {
        mult += aut_class(&mt, pos) == cls ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT(mult, info.mult);
    uint64_t key = graph_key(&mt);
    bool known = false;
    for (unsigned i = 0; i < *n_seen; i++) {
        known = known || seen[i] == key;
    }
    if (!known) {
        seen[(*n_seen)++] = key;
    }
}

static unsigned catalogue_graphlets(bool directed, unsigned n_orbits, uint64_t *seen) {
    glet_tables_init();
    unsigned n_seen = 0;
    for (unsigned orbit = 0; orbit < n_orbits; orbit++) {
        catalogue_orbit(directed, orbit, seen, &n_seen);
    }
    return n_seen;
}

static void test_catalogue_orbits_directed(void) {
    uint64_t seen[GRAPHLETS_MAX];
    TEST_ASSERT_EQUAL_UINT(40, catalogue_graphlets(true, ORBITS_D, seen));
}
static void test_catalogue_orbits_undirected(void) {
    uint64_t seen[GRAPHLETS_MAX];
    TEST_ASSERT_EQUAL_UINT(30, catalogue_graphlets(false, ORBITS_U, seen));
}

/* The matrix of a directed code of the given size. */
static mat directed_mat(unsigned size, unsigned code) {
    glet_orbit_info info = {0, size, 0, 0, code, 0};
    return mat_from_code(true, &info);
}

static bool is_oriented(const mat *mt) {
    return find_bidir(mt).count == 0;
}

/* Every connected oriented graph of this size matches exactly one catalogue graphlet. */
static unsigned match_all(unsigned size, const uint64_t *cat, unsigned n_cat) {
    unsigned found = 0;
    for (unsigned code = 0; code < (1U << (size * (size - 1))); code++) {
        mat mt = directed_mat(size, code);
        if (!is_oriented(&mt) || !is_connected(&mt)) {
            continue;
        }
        uint64_t key = graph_key(&mt);
        unsigned hits = 0;
        for (unsigned i = 0; i < n_cat; i++) {
            hits += cat[i] == key ? 1U : 0U;
        }
        TEST_ASSERT_EQUAL_UINT(1, hits);
        found++;
    }
    return found;
}

static void test_catalogue_complete(void) {
    uint64_t cat[GRAPHLETS_MAX];
    unsigned n_cat = catalogue_graphlets(true, ORBITS_D, cat);
    unsigned found = 0;
    for (unsigned size = 2; size <= 4; size++) {
        found += match_all(size, cat, n_cat);
    }
    TEST_ASSERT_TRUE(found > n_cat);
}

/* --- hand-worked directed examples -------------------------------------------------------------
 */

typedef struct fam {
    unsigned nodes;
    glet_edge arcs[6];
    size_t n_arcs;
} fam;

typedef struct expect {
    unsigned nodes;
    unsigned edges;
    unsigned hit; /* positions covered by the nonzero orbits of that node count */
} expect;

/* Exactly one graphlet of the expected node and arc count is found once, in `hit` positions. */
static void expect_single(const fam *fm, const expect *ex) {
    glet_graph graph;
    int64_t got[4 * ORBITS_D];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&graph, fm->nodes, fm->arcs, fm->n_arcs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&graph, &(glet_spec){true, 4, 0}, got, NULL));
    unsigned hit = 0;
    for (unsigned orbit = 0; orbit < ORBITS_D; orbit++) {
        glet_orbit_info info;
        (void)glet_orbit_describe(true, orbit, &info);
        int64_t total = 0;
        for (unsigned node = 0; node < fm->nodes; node++) {
            total += got[(size_t)node * ORBITS_D + orbit];
        }
        if (info.nodes == ex->nodes && total != 0) {
            TEST_ASSERT_EQUAL_UINT(ex->edges, info.edges);
            TEST_ASSERT_EQUAL_INT64((int64_t)info.mult, total);
            hit += info.mult;
        }
    }
    TEST_ASSERT_EQUAL_UINT(ex->hit, hit);
    glet_graph_free(&graph);
}

static void test_directed_families_small(void) {
    fam edge = {2, {{0, 1}}, 1}; /* single edge: both ends in distinct orbits */
    expect_single(&edge, &(expect){2, 1, 2});
    fam ostar = {3, {{0, 1}, {0, 2}}, 2};
    expect_single(&ostar, &(expect){3, 2, 3});
    fam istar = {3, {{1, 0}, {2, 0}}, 2};
    expect_single(&istar, &(expect){3, 2, 3});
    fam chain = {3, {{0, 1}, {1, 2}}, 2};
    expect_single(&chain, &(expect){3, 2, 3});
    fam cyc = {3, {{0, 1}, {1, 2}, {2, 0}}, 3}; /* cyclic triangle: one orbit of 3 */
    expect_single(&cyc, &(expect){3, 3, 3});
    fam tri = {3, {{0, 1}, {1, 2}, {0, 2}}, 3}; /* transitive triangle */
    expect_single(&tri, &(expect){3, 3, 3});
}

static void test_directed_families_four(void) {
    fam path4 = {4, {{0, 1}, {1, 2}, {2, 3}}, 3};
    expect_single(&path4, &(expect){4, 3, 4});
    fam star4 = {4, {{0, 1}, {0, 2}, {0, 3}}, 3};
    expect_single(&star4, &(expect){4, 3, 4});
    fam cyc4 = {4, {{0, 1}, {1, 2}, {2, 3}, {3, 0}}, 4};
    expect_single(&cyc4, &(expect){4, 4, 4});
}

/* An anti-parallel pair counts once per direction: two 2-node occurrences. */
static void test_antiparallel_pair(void) {
    glet_edge arcs[2] = {{0, 1}, {1, 0}};
    glet_graph graph;
    int64_t got[2 * ORBITS_D];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&graph, 2, arcs, 2));
    TEST_ASSERT_EQUAL_UINT64(1, graph.m);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&graph, &(glet_spec){true, 4, 0}, got, NULL));
    int64_t sum0 = 0;
    for (unsigned orbit = 0; orbit < ORBITS_D; orbit++) {
        sum0 += got[orbit];
    }
    TEST_ASSERT_EQUAL_INT64(2, sum0); /* node 0 sits in one out-end and one in-end occurrence */
    glet_graph_free(&graph);
    glet_edge loop[1] = {{0, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&graph, 1, loop, 1));
    TEST_ASSERT_EQUAL_UINT64(1, graph.self_loops);
    glet_graph_free(&graph);
}

static void test_work_bound(void) {
    glet_edge arcs[4] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}};
    glet_graph graph;
    int64_t got[5 * ORBITS_U];
    memset(got, 0, sizeof got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&graph, 5, arcs, 4));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          glet_count(&graph, &(glet_spec){false, 5, 3}, got, NULL));
    glet_graph_free(&graph);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_sizes);
    RUN_TEST(test_orbits_per_size);
    RUN_TEST(test_undirected4_vs_bruteforce);
    RUN_TEST(test_undirected5_vs_bruteforce);
    RUN_TEST(test_directed_vs_bruteforce);
    RUN_TEST(test_directed_families_small);
    RUN_TEST(test_directed_families_four);
    RUN_TEST(test_antiparallel_pair);
    RUN_TEST(test_work_bound);
    RUN_TEST(test_catalogue_orbits_directed);
    RUN_TEST(test_catalogue_orbits_undirected);
    RUN_TEST(test_catalogue_complete);
    return UNITY_END();
}
