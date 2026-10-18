/*
 * bench_lower.c — `lower` on a synthetic design of mixed word cells (3B spec 14.5, plan Task 9
 * step 6). Release only. Usage: `bench_lower [--cells N]` (default 100000; the PHASE3 number is
 * `--cells 1000000`, run inside tools/heavy-run).
 *
 * One module of N cells cycling through $and $add $mux-free types (bit, shift, compare and
 * arithmetic cells of 4 bits, every port a module port, as the unit fixtures build them), lowered
 * once. Prints: cells, nodes_before, nodes_after, seconds, nodes_per_s (bit nodes created per
 * second), rss_build_kib (peak before `lower`) and rss_peak_kib (peak after).
 */
#include "../unit/lower_fixture.h"
#include "bench_guard.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/* Unity (linked for the fixture helper) references these. */
void setUp(void);
void tearDown(void);
void setUp(void) {
}
void tearDown(void) {
}

enum { DEFAULT_CELLS = 100000, WIDTH = 4, NS_PER_S = 1000000000, KIB = 1 };

static const char *const k_binary[] = {"$and", "$or", "$xor", "$add", "$sub",
                                       "$lt",  "$eq", "$shl", "$shr", "$ge"};

static long peak_rss_kib(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss * KIB : -1;
}

static double seconds_since(const struct timespec *start) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / (double)NS_PER_S;
}

int main(int argc, char **argv) {
    int refused = bench_refuse_unoptimized(argv[0]);
    if (refused != 0) {
        return refused;
    }
    uint32_t cells = DEFAULT_CELLS;
    if (argc == 3 && strcmp(argv[1], "--cells") == 0) {
        cells = (uint32_t)strtoul(argv[2], NULL, 10);
    } else if (argc != 1) {
        (void)fprintf(stderr, "usage: bench_lower [--cells N]\n");
        return 2;
    }
    odin3_log_set_sink(NULL, NULL);
    lf fix;
    lf_begin(&fix);
    odin3_module *mod = lf_module(&fix, "m");
    lf_binary spec = {false, false, WIDTH, WIDTH, WIDTH};
    odin3_value params[5];
    lf_binary_params(&spec, params);
    for (uint32_t i = 0; i < cells; i++) {
        char prefix[16];
        (void)snprintf(prefix, sizeof prefix, "c%u_", (unsigned)i);
        lf_cell_spec cell = {
            k_binary[i % (sizeof k_binary / sizeof k_binary[0])], params, 5, "dut", {0}};
        (void)lf_cell_prefixed(&fix, mod, &cell, prefix);
    }
    uint32_t before = lf_live_nodes(mod);
    long rss_build = peak_rss_kib();
    struct timespec start;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    odin3_status st = odin3_pass_run(fix.design, "lower", odin3_bytes_cstr(""));
    double secs = seconds_since(&start);
    if (st != ODIN3_OK) {
        (void)fprintf(stderr, "bench_lower: lower failed: %s\n", odin3_status_string(st));
        return 1;
    }
    uint32_t after = lf_live_nodes(mod);
    uint64_t made = (uint64_t)after - before + cells; /* the word cells are gone */
    (void)printf("cells=%" PRIu32 " nodes_before=%" PRIu32 " nodes_after=%" PRIu32
                 " seconds=%.3f nodes_per_s=%.0f rss_build_kib=%ld rss_peak_kib=%ld\n",
                 cells, before, after, secs, secs > 0 ? (double)made / secs : 0.0, rss_build,
                 peak_rss_kib());
    lf_end(&fix);
    return 0;
}
