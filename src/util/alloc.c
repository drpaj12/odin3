/*
 * alloc.c — allocation wrapper with a test hook that makes allocations fail.
 */
#include "util/alloc.h"

#include <stdbool.h>
#include <stdlib.h>

static long fail_after = -1;

/* True when this allocation is the one the test hook wants to fail. */
static bool should_fail(void) {
    if (fail_after < 0) {
        return false;
    }
    if (fail_after == 0) {
        fail_after = -1;
        return true;
    }
    fail_after--;
    return false;
}

void odin3_util_set_alloc_fail_after(long n) {
    fail_after = n;
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
