/*
 * test_graphlets_instances.c — graphlet instances (spec docs/specs/2026-11-06-odin3-graphlet-
 * instances.md section 4): on random graphs the number of instances of every graphlet type equals
 * the count derived from the orbit totals of the same graph (orbit total / orbit size), every
 * written tuple is a node set whose induced subgraph is isomorphic to the type (brute-force
 * isomorphism on the catalogue code), the hand cases, truncation by max, the argument checks of the
 * C ABI and the full-precision similarity.
 */
#include "graph/glet.h"
#include "odin3/odin3.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { MAXN = 9, MAXK = 5, MAX_INST = 20000, MAX_BOTH = 10 };

void setUp(void) {
}
void tearDown(void) {
}

static uint32_t g_rng = 987654321U;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525U + 1013904223U;
    return g_rng >> 8;
}

typedef struct mat {
    unsigned size;
    uint8_t cell[MAXK][MAXK]; /* cell[i][j] = 1 when i -> j */
} mat;

typedef struct mode {
    bool directed;
    unsigned size; /* undirected 4 or 5; directed: 4 */
    unsigned max_nodes;
} mode;

static unsigned fact(unsigned count) {
    unsigned prod = 1;
    for (unsigned k = 2; k <= count; k++) {
        prod *= k;
    }
    return prod;
}

/* The idx-th permutation of the positions of mt (factorial number system). */
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

/* Smallest row-major bit string of the matrix over all relabellings. */
static uint64_t canonical_key(const mat *mt) {
    unsigned perm[MAXK];
    uint64_t best = ~0ULL;
    for (unsigned k = 0; k < fact(mt->size); k++) {
        nth_perm(mt, k, perm);
        uint64_t key = 0;
        for (unsigned i = 0; i < mt->size; i++) {
            for (unsigned j = 0; j < mt->size; j++) {
                key = (key << 1) | mt->cell[perm[i]][perm[j]];
            }
        }
        best = key < best ? key : best;
    }
    return best;
}

/* The matrix of the catalogue code of in (tables.c layout: pair (i, j), i < j, index
 * j*(j-1)/2 + i; one bit undirected, two directed: bit 0 is i -> j, bit 1 is j -> i). */
static mat code_mat(bool directed, const glet_orbit_info *in) {
    mat mt;
    memset(&mt, 0, sizeof mt);
    mt.size = in->nodes;
    for (unsigned j = 1; j < in->nodes; j++) {
        for (unsigned i = 0; i < j; i++) {
            unsigned ix = j * (j - 1) / 2 + i;
            if (directed) {
                mt.cell[i][j] = (uint8_t)((in->code >> (2 * ix)) & 1U);
                mt.cell[j][i] = (uint8_t)((in->code >> (2 * ix + 1)) & 1U);
            } else {
                mt.cell[i][j] = mt.cell[j][i] = (uint8_t)((in->code >> ix) & 1U);
            }
        }
    }
    return mt;
}

static glet_orbit_info first_orbit_of(const mode *md, uint32_t graphlet) {
    uint32_t norb = glet_orbit_count(md->directed, md->size);
    glet_orbit_info in;
    for (uint32_t orbit = 0; orbit < norb; orbit++) {
        TEST_ASSERT_TRUE(glet_orbit_describe(md->directed, orbit, &in));
        if (in.graphlet == graphlet) {
            return in;
        }
    }
    TEST_FAIL_MESSAGE("graphlet without orbit");
    return in;
}

/* The plain graph under test. */
typedef struct graph {
    uint32_t n;
    uint64_t n_edges;
    uint32_t from[MAXN * MAXN];
    uint32_t to[MAXN * MAXN];
    uint8_t arc[MAXN][MAXN];
} graph;

static void graph_random(graph *gr, bool directed, unsigned trial, unsigned max_nodes) {
    memset(gr, 0, sizeof *gr);
    gr->n = 4 + trial % (max_nodes - 3);
    unsigned pct = 12 + (trial * 11) % 45;
    for (uint32_t from = 0; from < gr->n; from++) {
        for (uint32_t to = 0; to < gr->n; to++) {
            bool take = from != to && rnd() % 100 < pct && (directed || gr->arc[to][from] == 0);
            if (take) {
                gr->arc[from][to] = 1;
                gr->from[gr->n_edges] = from;
                gr->to[gr->n_edges++] = to;
            }
        }
    }
}

static odin3_graphlet_graph view(const graph *gr) {
    return (odin3_graphlet_graph){gr->n, gr->n_edges, gr->from, gr->to};
}

/* The induced subgraph of a node set: its matrix without the anti-parallel pairs (directed), and
 * those pairs. */
typedef struct induced {
    mat base;
    unsigned pairs[MAX_BOTH][2];
    unsigned n_both;
} induced;

static induced induce(const graph *gr, bool directed, const uint32_t *set, unsigned size) {
    induced ind;
    memset(&ind, 0, sizeof ind);
    ind.base.size = size;
    for (unsigned i = 0; i < size; i++) {
        for (unsigned j = 0; j < size; j++) {
            unsigned fwd = gr->arc[set[i]][set[j]];
            unsigned bwd = gr->arc[set[j]][set[i]];
            if (!directed) {
                ind.base.cell[i][j] = (uint8_t)(fwd | bwd);
            } else if (fwd != 0 && bwd != 0) {
                ind.pairs[ind.n_both][0] = i;
                ind.pairs[ind.n_both++][1] = j;
            } else {
                ind.base.cell[i][j] = (uint8_t)fwd;
            }
        }
    }
    return ind;
}

/* True when some resolution of the anti-parallel pairs of ind is isomorphic to key. */
static bool resolves_to(const induced *ind, uint64_t key) {
    for (unsigned mask = 0; mask < (1U << ind->n_both); mask++) {
        mat cur = ind->base;
        for (unsigned pr = 0; pr < ind->n_both; pr++) {
            unsigned lo = ind->pairs[pr][0];
            unsigned hi = ind->pairs[pr][1];
            if (((mask >> pr) & 1U) != 0) {
                cur.cell[lo][hi] = 1;
            } else {
                cur.cell[hi][lo] = 1;
            }
        }
        if (canonical_key(&cur) == key) {
            return true;
        }
    }
    return false;
}

/* The derived count of type gl: every orbit of the type gives the same orbit total / orbit size. */
static int64_t derived_count(const mode *md, uint32_t gl, const int64_t *counts, uint32_t nodes_n) {
    uint32_t norb = glet_orbit_count(md->directed, md->size);
    int64_t derived = -1;
    for (uint32_t orbit = 0; orbit < norb; orbit++) {
        glet_orbit_info in;
        TEST_ASSERT_TRUE(glet_orbit_describe(md->directed, orbit, &in));
        if (in.graphlet != gl) {
            continue;
        }
        int64_t sum = 0;
        for (uint32_t node = 0; node < nodes_n; node++) {
            sum += counts[(size_t)node * norb + orbit];
        }
        TEST_ASSERT_EQUAL_INT64(0, sum % in.mult);
        derived = derived < 0 ? sum / in.mult : derived;
        TEST_ASSERT_EQUAL_INT64(derived, sum / in.mult);
    }
    return derived;
}

/* One tuple is ascending, in range, and its induced subgraph is the type of canonical key. */
static bool tuple_ok(const graph *gr, const mode *md, const uint32_t *tuple,
                     const glet_orbit_info *type, uint64_t key) {
    for (unsigned i = 0; i < type->nodes; i++) {
        if (tuple[i] >= gr->n || (i > 0 && tuple[i - 1] >= tuple[i])) {
            return false;
        }
    }
    induced ind = induce(gr, md->directed, tuple, type->nodes);
    return resolves_to(&ind, key);
}

static void check_tuples(const graph *gr, const mode *md, const uint32_t *out, uint64_t total,
                         const glet_orbit_info *type) {
    mat tm = code_mat(md->directed, type);
    uint64_t key = canonical_key(&tm);
    uint64_t bad = 0;
    for (uint64_t k = 0; k < total; k++) {
        bad += tuple_ok(gr, md, out + k * type->nodes, type, key) ? 0U : 1U;
    }
    TEST_ASSERT_EQUAL_UINT64(0, bad);
}

static void check_type(const graph *gr, const mode *md, uint32_t gl, const int64_t *counts) {
    odin3_graphlet_graph gv = view(gr);
    odin3_graphlet_opts opts = {md->directed ? 1 : 0, md->size, 0};
    uint32_t nodes = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlet_nodes(&opts, gl, &nodes));
    glet_orbit_info first = first_orbit_of(md, gl);
    TEST_ASSERT_EQUAL_UINT32(first.nodes, nodes);
    int64_t derived = derived_count(md, gl, counts, gr->n);
    uint32_t *out = calloc((size_t)MAX_INST * MAXK, sizeof *out);
    TEST_ASSERT_NOT_NULL(out);
    odin3_graphlet_instance_buf buf = {out, (uint64_t)MAX_INST * MAXK, MAX_INST, 99};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlets_instances(&gv, &opts, gl, &buf));
    TEST_ASSERT_EQUAL_INT64(derived, (int64_t)buf.total);
    TEST_ASSERT_TRUE(buf.total <= MAX_INST);
    check_tuples(gr, md, out, buf.total, &first);
    free(out);
}

static void check_graph(const graph *gr, const mode *md) {
    odin3_graphlet_graph gv = view(gr);
    odin3_graphlet_opts opts = {md->directed ? 1 : 0, md->size, 0};
    uint32_t norb = glet_orbit_count(md->directed, md->size);
    int64_t *counts = calloc((size_t)gr->n * norb, sizeof *counts);
    TEST_ASSERT_NOT_NULL(counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_graphlets_count(&gv, &opts, counts, (uint64_t)gr->n * norb));
    for (uint32_t gl = 0; gl < glet_graphlet_count(md->directed, md->size); gl++) {
        check_type(gr, md, gl, counts);
    }
    free(counts);
}

static void run_mode(const mode *md, unsigned trials) {
    graph *gr = calloc(1, sizeof *gr);
    TEST_ASSERT_NOT_NULL(gr);
    for (unsigned trial = 0; trial < trials; trial++) {
        graph_random(gr, md->directed, trial, md->max_nodes);
        check_graph(gr, md);
    }
    free(gr);
}

static void test_random_undirected_5(void) {
    run_mode(&(mode){false, 5, MAXN}, 18);
}

static void test_random_undirected_4(void) {
    run_mode(&(mode){false, 4, MAXN}, 18);
}

static void test_random_directed(void) {
    run_mode(&(mode){true, 4, 8}, 12);
}

/* --- hand cases ---------------------------------------------------------------------------------
 */

static uint32_t graphlet_with(unsigned nodes, unsigned edges) {
    glet_orbit_info in;
    for (uint32_t orbit = 0; orbit < GLET_ORBITS_U5; orbit++) {
        TEST_ASSERT_TRUE(glet_orbit_describe(false, orbit, &in));
        if (in.nodes == nodes && in.edges == edges) {
            return in.graphlet;
        }
    }
    TEST_FAIL_MESSAGE("no such graphlet");
    return 0;
}

/* True when the 3-node tuple want is among the first count tuples of out. */
static bool has_triple(const uint32_t *out, const uint32_t *want, uint64_t count) {
    for (uint64_t k = 0; k < count; k++) {
        if (memcmp(out + k * 3, want, 3 * sizeof *want) == 0) {
            return true;
        }
    }
    return false;
}

/* A hand graph of at most 4 edges. */
typedef struct hand {
    uint32_t nodes;
    uint64_t edges;
    uint32_t from[4];
    uint32_t to[4];
} hand;

/* Total instances of graphlet gl in hd (undirected, size 5); the first 10 go to out. */
static uint64_t hand_instances(const hand *hd, uint32_t gl, uint32_t *out) {
    odin3_graphlet_graph gv = {hd->nodes, hd->edges, hd->from, hd->to};
    odin3_graphlet_opts opts = {0, 5, 0};
    odin3_graphlet_instance_buf buf = {out, 30, 10, 0};
    odin3_status st = odin3_graphlets_instances(&gv, &opts, gl, &buf);
    return st == ODIN3_OK ? buf.total : UINT64_MAX;
}

static void test_hand_path(void) {
    hand hd = {5, 4, {0, 1, 2, 3}, {1, 2, 3, 4}};
    uint32_t out[30];
    TEST_ASSERT_EQUAL_UINT64(3, hand_instances(&hd, graphlet_with(3, 2), out));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){0, 1, 2}, 3));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){1, 2, 3}, 3));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){2, 3, 4}, 3));
}

static void test_hand_triangle_with_pendant(void) {
    hand hd = {4, 4, {0, 1, 2, 2}, {1, 2, 0, 3}};
    uint32_t out[30];
    TEST_ASSERT_EQUAL_UINT64(1, hand_instances(&hd, graphlet_with(3, 3), out));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){0, 1, 2}, 1));
    TEST_ASSERT_EQUAL_UINT64(2, hand_instances(&hd, graphlet_with(3, 2), out));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){0, 2, 3}, 2));
    TEST_ASSERT_TRUE(has_triple(out, (uint32_t[]){1, 2, 3}, 2));
}

static void test_max_truncates_but_total_is_exact(void) {
    odin3_graphlet_opts opts = {0, 5, 0};
    uint32_t path = graphlet_with(3, 2);
    uint32_t from[4] = {0, 1, 2, 3};
    uint32_t to[4] = {1, 2, 3, 4};
    odin3_graphlet_graph chain = {5, 4, from, to};
    uint32_t out[3] = {99, 99, 99};
    odin3_graphlet_instance_buf one = {out, 3, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlets_instances(&chain, &opts, path, &one));
    TEST_ASSERT_EQUAL_UINT64(3, one.total);
    TEST_ASSERT_TRUE(out[0] < out[1] && out[1] < out[2]);
    odin3_graphlet_instance_buf none = {NULL, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlets_instances(&chain, &opts, path, &none));
    TEST_ASSERT_EQUAL_UINT64(3, none.total);
}

/* graphlets_instances of the single-edge graph for graphlet gl with the given mode and buffer. */
static odin3_status edge_instances(const odin3_graphlet_opts *opts, uint32_t gl,
                                   odin3_graphlet_instance_buf *buf) {
    uint32_t from[1] = {0};
    uint32_t to[1] = {1};
    odin3_graphlet_graph gr = {2, 1, from, to};
    return odin3_graphlets_instances(&gr, opts, gl, buf);
}

static void test_graphlet_ids_are_checked(void) {
    odin3_graphlet_opts u5 = {0, 5, 0};
    odin3_graphlet_opts u4 = {0, 4, 0};
    odin3_graphlet_opts dir = {1, 4, 0};
    uint32_t out[10];
    odin3_graphlet_instance_buf buf = {out, 10, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&u5, 30, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&u4, 9, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&dir, 40, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, edge_instances(&u5, 0, &buf));
    TEST_ASSERT_EQUAL_UINT64(1, buf.total);
    TEST_ASSERT_EQUAL_UINT32(0, out[0]);
    TEST_ASSERT_EQUAL_UINT32(1, out[1]);
}

static void test_buffer_is_checked(void) {
    odin3_graphlet_opts u5 = {0, 5, 0};
    uint32_t out[10];
    odin3_graphlet_instance_buf small = {out, 10, 6, 0};
    odin3_graphlet_instance_buf nobuf = {NULL, 10, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&u5, 0, &small));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&u5, 0, &nobuf));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, edge_instances(&u5, 0, NULL));
}

static void test_graphlet_nodes(void) {
    odin3_graphlet_opts u5 = {0, 5, 0};
    odin3_graphlet_opts bad = {0, 3, 0};
    uint32_t nodes = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_graphlet_nodes(&bad, 0, &nodes));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlet_nodes(&u5, 0, &nodes));
    TEST_ASSERT_EQUAL_UINT32(2, nodes);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlet_nodes(&u5, 29, &nodes));
    TEST_ASSERT_EQUAL_UINT32(5, nodes);
}

/* odin3_graphlets_similarity equals the library metrics on the same rows, at full precision. */
static void test_similarity_matches_sig(void) {
    graph *gr = calloc(1, sizeof *gr);
    TEST_ASSERT_NOT_NULL(gr);
    graph_random(gr, false, 7, MAXN);
    odin3_graphlet_graph gv = view(gr);
    odin3_graphlet_opts opts = {0, 5, 0};
    int64_t *counts = calloc((size_t)gr->n * GLET_ORBITS_U5, sizeof *counts);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_graphlets_count(&gv, &opts, counts, (uint64_t)gr->n * GLET_ORBITS_U5));
    uint32_t half = gr->n / 2;
    glet_rows ra = {counts, half, GLET_ORBITS_U5};
    glet_rows rb = {counts + (size_t)half * GLET_ORBITS_U5, gr->n - half, GLET_ORBITS_U5};
    glet_sig lhs;
    glet_sig rhs;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_sig_make(&lhs, &ra, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_sig_make(&rhs, &rb, NULL));
    odin3_graphlet_rows pa = {ra.counts, ra.n};
    odin3_graphlet_rows pb = {rb.counts, rb.n};
    odin3_graphlet_similarity got = {-1, -1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlets_similarity(&pa, &pb, GLET_ORBITS_U5, &got));
    double want_gdd = glet_gdd_agreement(&lhs, &rhs, NULL);
    double want_cos = glet_cosine_log(&lhs, &rhs);
    TEST_ASSERT_EQUAL_MEMORY(&want_gdd, &got.gdd_agreement, sizeof want_gdd); /* bit-identical */
    TEST_ASSERT_EQUAL_MEMORY(&want_cos, &got.cosine_log, sizeof want_cos);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_graphlets_similarity(&pa, &pb, 74, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_graphlets_similarity(&pa, &pb, GLET_ORBITS_U5, NULL));
    odin3_graphlet_rows none = {NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_graphlets_similarity(&none, &none, 73, &got));
    TEST_ASSERT_TRUE(got.gdd_agreement > 0.999999 && got.cosine_log > 0.999999);
    glet_sig_free(&lhs);
    glet_sig_free(&rhs);
    free(counts);
    free(gr);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hand_path);
    RUN_TEST(test_hand_triangle_with_pendant);
    RUN_TEST(test_max_truncates_but_total_is_exact);
    RUN_TEST(test_graphlet_ids_are_checked);
    RUN_TEST(test_buffer_is_checked);
    RUN_TEST(test_graphlet_nodes);
    RUN_TEST(test_similarity_matches_sig);
    RUN_TEST(test_random_undirected_5);
    RUN_TEST(test_random_undirected_4);
    RUN_TEST(test_random_directed);
    return UNITY_END();
}
