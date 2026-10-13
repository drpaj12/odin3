/*
 * bench_guard.h — the build guard every timing benchmark shares (2D Task 27 review I1).
 *
 * A Debug or sanitizer binary prints inflated times and RSS that would be mistaken for a missed
 * target, so a benchmark that reports §15 numbers (bench_elab; 2E's bench_proc) calls
 * bench_refuse_unoptimized(argv[0]) first and returns its result when it is non-zero. CMake
 * defines ODIN3_BENCH_RELEASE for the benchmark when the build is neither Debug nor sanitized.
 */
#ifndef ODIN3_BENCH_GUARD_H
#define ODIN3_BENCH_GUARD_H

#include <stdio.h>

/* 0 in an optimized build; else prints the refusal on stderr and returns the exit code 2. */
static inline int bench_refuse_unoptimized(const char *tool) {
#ifdef ODIN3_BENCH_RELEASE
    (void)tool;
    return 0;
#else
    (void)fprintf(stderr,
                  "%s: needs a Release build (this one is Debug or sanitized: its times "
                  "and RSS are not the targets' numbers)\n",
                  tool);
    return 2;
#endif
}

#endif
