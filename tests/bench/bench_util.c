/* bench_util.c — times and sizes the util containers at 2,000,000 elements each. */
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

enum { BENCH_COUNT = 2000000 };

typedef struct bench_item {
    uint64_t words[4];
} bench_item;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long max_rss_kib(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return usage.ru_maxrss;
}

static int bench_strtab(void) {
    odin3_strtab *tab = odin3_strtab_create();
    if (tab == NULL) {
        return 1;
    }
    char name[16];
    size_t name_bytes = 0;
    double start = now_seconds();
    for (uint32_t i = 0; i < BENCH_COUNT; i++) {
        int len = snprintf(name, sizeof name, "n%07u", (unsigned)i);
        uint32_t id = 0;
        if (len < 0 ||
            odin3_strtab_intern(tab, (odin3_bytes){name, (size_t)len}, &id) != ODIN3_OK) {
            odin3_strtab_destroy(tab);
            return 1;
        }
        name_bytes += (size_t)len + 1;
    }
    double elapsed = now_seconds() - start;
    printf("strtab intern  : %9.3f s  %zu strings  %zu name bytes (incl. NUL)  maxrss-so-far %ld "
           "KiB\n",
           elapsed, odin3_strtab_count(tab), name_bytes, max_rss_kib());
    odin3_strtab_destroy(tab);
    return 0;
}

static int bench_u64map(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    if (map == NULL) {
        return 1;
    }
    double start = now_seconds();
    for (uint64_t i = 0; i < BENCH_COUNT; i++) {
        if (odin3_u64map_put(map, (odin3_kv){i, i}) != ODIN3_OK) {
            odin3_u64map_destroy(map);
            return 1;
        }
    }
    double elapsed = now_seconds() - start;
    printf("u64map put     : %9.3f s  %zu entries  %zu entry bytes (live)  maxrss-so-far %ld KiB\n",
           elapsed, odin3_u64map_count(map), odin3_u64map_count(map) * sizeof(odin3_kv),
           max_rss_kib());
    odin3_u64map_destroy(map);
    return 0;
}

static int bench_pagevec(void) {
    odin3_pagevec *vec = odin3_pagevec_create(sizeof(bench_item));
    if (vec == NULL) {
        return 1;
    }
    double start = now_seconds();
    for (uint64_t i = 0; i < BENCH_COUNT; i++) {
        bench_item *item = odin3_pagevec_push(vec, NULL);
        if (item == NULL) {
            odin3_pagevec_destroy(vec);
            return 1;
        }
        item->words[0] = i;
    }
    double elapsed = now_seconds() - start;
    printf("pagevec push   : %9.3f s  %zu elements  %zu bytes reserved  maxrss-so-far %ld KiB\n",
           elapsed, odin3_pagevec_len(vec), odin3_pagevec_bytes_reserved(vec), max_rss_kib());
    odin3_pagevec_destroy(vec);
    return 0;
}

int main(void) {
    if (bench_strtab() != 0 || bench_u64map() != 0 || bench_pagevec() != 0) {
        fprintf(stderr, "bench_util: out of memory\n");
        return 1;
    }
    printf("max RSS (process, cumulative): %ld KiB\n", max_rss_kib());
    return 0;
}
