/*
 * srcman_test.h — hidden test hook for the source manager's limits (never part of the ABI).
 */
#ifndef ODIN3_AST_SRCMAN_TEST_H
#define ODIN3_AST_SRCMAN_TEST_H

#include "ast/srcman.h"

#include <stdint.h>

/* The limits a test may lower; the defaults are ODIN3_SRC_MAX_BUFFERS and UINT32_MAX. */
typedef struct odin3_srcman_limits {
    uint32_t max_buffers; /* buffers, slot 0 excluded */
    uint32_t space_end;   /* the last usable loc */
} odin3_srcman_limits;

/* Lowers the buffer-count limit and the location space so tests can hit them cheaply. */
void odin3_srcman_test_set_limits(odin3_srcman *sm, odin3_srcman_limits limits);

#endif
