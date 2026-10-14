/*
 * flatten_test.h — test hooks of `flatten` (3A plan point (g)): lowered caps. Never part of the
 * ABI. The limits live in graph.c.
 */
#ifndef ODIN3_PASSES_FLATTEN_FLATTEN_TEST_H
#define ODIN3_PASSES_FLATTEN_FLATTEN_TEST_H

#include <stdint.h>

/* The caps of flatten.h that tests lower; 0 keeps a default. A value above its default is
 * clamped to the default. */
typedef struct odin3_flatten_limits {
    uint64_t max_nodes, max_pins, max_nets, max_wires;
    uint32_t max_roots;
} odin3_flatten_limits;

/* Sets the caps (NULL restores the defaults). */
void odin3_flatten_test_set_limits(const odin3_flatten_limits *limits);

/* The caps in force. */
odin3_flatten_limits odin3_flatten_limits_now(void);

#endif
