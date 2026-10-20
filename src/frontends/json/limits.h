/*
 * limits.h — the parser limits of the JSON reader (spec 5.3 E-3). Each is exceeded as a located
 * ODIN3_ERR_PARSE error. Only odin3_json_test_set_limits (json_parse.h) changes them.
 */
#ifndef ODIN3_FRONTENDS_JSON_LIMITS_H
#define ODIN3_FRONTENDS_JSON_LIMITS_H

#include <stddef.h>
#include <stdint.h>

enum {
    ODIN3_JSON_MAX_BYTES = 1 << 30,  /* file size: 1 GiB */
    ODIN3_JSON_MAX_STRING = 1 << 26, /* decoded string length: 64 MiB (a Yosys $mem INIT is
                                        one bit per memory bit; a 64Kx32 ROM is 2 Mi chars) */
    ODIN3_JSON_MAX_NODES = 1 << 25,  /* values in the tree: 32 Mi, about 1.5 GiB of nodes */
    ODIN3_JSON_MAX_DEPTH = 32        /* nesting depth of arrays and objects */
};

typedef struct odin3_json_limits {
    size_t max_bytes;
    size_t max_string;
    size_t max_nodes;
    uint32_t max_depth;
} odin3_json_limits;

#endif
