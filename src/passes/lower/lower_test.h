/*
 * lower_test.h — test hooks of `lower` (3B plan Task 1): lowered caps. Never part of the ABI. The
 * limits live in lower.c.
 */
#ifndef ODIN3_PASSES_LOWER_LOWER_TEST_H
#define ODIN3_PASSES_LOWER_LOWER_TEST_H

#include <stdint.h>

/* The caps of lower.h that tests lower; 0 keeps a default. A value above its default is clamped
 * to the default. */
typedef struct odin3_lower_limits {
    uint64_t max_cell_nodes, max_module_nodes, max_mem_bits;
} odin3_lower_limits;

/* Sets the caps (NULL restores the defaults). */
void odin3_lower_test_set_limits(const odin3_lower_limits *limits);

/* The caps in force. */
odin3_lower_limits odin3_lower_limits_now(void);

#endif
