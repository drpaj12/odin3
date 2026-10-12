/*
 * passrun_fixture.h — the run-information test hook of the test-only odin3_procfix library
 * (3D Task 8, fix round 1; passrun_fixture_lib.c). Python reaches it through its own cdef.
 */
#ifndef ODIN3_TESTS_UNIT_PASSRUN_FIXTURE_H
#define ODIN3_TESTS_UNIT_PASSRUN_FIXTURE_H

#include "odin3/odin3.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Formats count key=value words with odin3_passrun_info_append, pairs[2i] the key and pairs[2i + 1]
 * the value (both NUL-terminated), into out (cap bytes, NUL-terminated). Returns the first
 * failure's status; ODIN3_ERR_INVALID_ARG for NULL pairs or out or a cap of 0, and
 * ODIN3_ERR_NO_MEMORY also when the words do not fit in cap bytes (out is then unchanged).
 */
odin3_status odin3_test_passrun_info(const char *const *pairs, uint32_t count, char *out,
                                     size_t cap);

#endif
