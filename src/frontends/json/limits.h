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
    ODIN3_JSON_MAX_STRING = 1 << 20, /* decoded string length: 1 MiB */
    ODIN3_JSON_MAX_DEPTH = 32        /* nesting depth of arrays and objects */
};

typedef struct odin3_json_limits {
    size_t max_bytes;
    size_t max_string;
    uint32_t max_depth;
} odin3_json_limits;

#endif
