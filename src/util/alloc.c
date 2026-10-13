/*
 * alloc.c — allocation wrapper with a test hook that makes allocations fail.
 */
#include "util/alloc.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static long fail_after = -1;
static long burst = 1; /* allocations that fail once failing starts (LONG_MAX: all later ones) */

/* True when this allocation is the one the test hook wants to fail. */
static bool should_fail(void) {
    if (fail_after < 0) {
        return false;
    }
    if (fail_after == 0) {
        burst--;
        fail_after = burst > 0 ? 0 : -1;
        return true;
    }
    fail_after--;
    return false;
}

void odin3_util_set_alloc_fail_after(long n) {
    fail_after = n;
    burst = 1;
}

void odin3_util_set_alloc_fail_window(odin3_alloc_window window) {
    fail_after = window.after;
    burst = window.len;
}

void odin3_util_set_alloc_fail_from(long n) {
    odin3_util_set_alloc_fail_window((odin3_alloc_window){n, LONG_MAX});
}

void *odin3_util_malloc(size_t bytes) {
    return should_fail() ? NULL : malloc(bytes);
}

void *odin3_util_calloc(size_t bytes) {
    return should_fail() ? NULL : calloc(1, bytes);
}

void *odin3_util_realloc(void *ptr, size_t bytes) {
    return should_fail() ? NULL : realloc(ptr, bytes);
}

void odin3_util_free(void *ptr) {
    free(ptr);
}
