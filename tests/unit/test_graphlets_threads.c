/*
 * test_graphlets_threads.c — odin3_graphlets_count from many threads at once, before any call has
 * built the shared orbit tables (PR #304 review): every thread must get the same, correct counts.
 * This binary calls nothing else first, so the first calls race to build the tables.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <pthread.h>
#include <string.h>

enum { THREADS = 8, NODES = 6, ORBITS_U5 = 73, ORBITS_D4 = 129 };

typedef struct job {
    int directed;
    odin3_status st;
    int64_t out[NODES * ORBITS_D4];
} job;

static const uint32_t k_from[NODES] = {0, 1, 2, 3, 4, 5};
static const uint32_t k_to[NODES] = {1, 2, 3, 4, 5, 0};

void setUp(void) {
}
void tearDown(void) {
}

static void *worker(void *arg) {
    job *jb = arg;
    odin3_graphlet_graph graph = {NODES, NODES, k_from, k_to};
    odin3_graphlet_opts opts = {jb->directed, 5, 0};
    uint64_t cap = (uint64_t)NODES * (jb->directed ? ORBITS_D4 : ORBITS_U5);
    jb->st = odin3_graphlets_count(&graph, &opts, jb->out, cap);
    return NULL;
}

static void test_concurrent_first_use(void) {
    static job jobs[THREADS];
    pthread_t tid[THREADS];
    for (int i = 0; i < THREADS; i++) {
        jobs[i].directed = i % 2;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid[i], NULL, worker, &jobs[i]));
    }
    for (int i = 0; i < THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tid[i], NULL));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, jobs[i].st);
    }
    /* a 6-ring: every node has degree 2 (orbit 0) in the undirected view */
    for (int i = 0; i < THREADS; i += 2) {
        TEST_ASSERT_EQUAL_INT(
            0, memcmp(jobs[i].out, jobs[0].out, sizeof(int64_t) * NODES * ORBITS_U5));
        for (size_t node = 0; node < NODES; node++) {
            TEST_ASSERT_EQUAL_INT64(2, jobs[i].out[node * ORBITS_U5]);
        }
    }
    for (int i = 1; i < THREADS; i += 2) {
        TEST_ASSERT_EQUAL_INT(
            0, memcmp(jobs[i].out, jobs[1].out, sizeof(int64_t) * NODES * ORBITS_D4));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_concurrent_first_use);
    return UNITY_END();
}
