/*
 * alloc.h — allocation wrapper with a test hook that makes allocations fail.
 */
#ifndef ODIN3_UTIL_ALLOC_H
#define ODIN3_UTIL_ALLOC_H

#include <stddef.h>

/* Like malloc/realloc/free; NULL on failure. A failed realloc leaves the old block valid. */
void *odin3_util_malloc(size_t bytes);
void *odin3_util_realloc(void *ptr, size_t bytes);
void odin3_util_free(void *ptr);

/* Zero-filled block of `bytes` bytes (one size, so no swappable parameters); NULL on failure. */
void *odin3_util_calloc(size_t bytes);

/*
 * Test hook (hidden, not part of the ABI). n < 0 disables failure injection; n == 0 makes the
 * next allocation fail; n == k lets k allocations succeed and fails the one after. After the
 * injected failure the hook disables itself.
 */
void odin3_util_set_alloc_fail_after(long n);

/*
 * The persistent form (what real out-of-memory does): n allocations succeed, then every later one
 * fails until odin3_util_set_alloc_fail_after(-1).
 */
void odin3_util_set_alloc_fail_from(long n);

/* A burst: `after` allocations succeed, the next `len` fail, then the hook disables itself. */
typedef struct odin3_alloc_window {
    long after, len;
} odin3_alloc_window;
void odin3_util_set_alloc_fail_window(odin3_alloc_window window);

#endif
