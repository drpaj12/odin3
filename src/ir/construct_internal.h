/*
 * construct_internal.h — the construct table's private layout, shared by construct.c and the test
 * hook in ir_test.c. Nothing else includes it.
 */
#ifndef ODIN3_IR_CONSTRUCT_INTERNAL_H
#define ODIN3_IR_CONSTRUCT_INTERNAL_H

#include "ir/construct.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdint.h>

/* A run the snapshot covered. 8 bytes. */
typedef struct run_row {
    uint32_t run;
    uint32_t mode;
} run_row;

struct odin3_construct_table {
    odin3_vec rows;      /* odin3_construct_row; row number n is index n - 1 */
    odin3_u64map *heads; /* (run << 32 | ast) -> row number of the newest row of that node */
    odin3_vec runs;      /* run_row, in the order marked */
    odin3_vec misses;    /* uint32_t record IDs, increasing */
};

#endif
