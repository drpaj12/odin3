/*
 * json_parse.h — iterative JSON parser with limits (spec 5.3, E-1 to E-3).
 *
 * RFC 8259 JSON with: integers in the int64 range only (a fraction or exponent is an error),
 * \uXXXX decoded to UTF-8 (a lone surrogate and \u0000 are errors), a leading byte-order mark
 * skipped, no raw control byte in a string, no trailing comma, no duplicate key in one object.
 * Strings keep invalid UTF-8 as is. The parser and the free never recurse: depth costs heap, not
 * stack.
 */
#ifndef ODIN3_FRONTENDS_JSON_JSON_PARSE_H
#define ODIN3_FRONTENDS_JSON_JSON_PARSE_H

#include "frontends/json/limits.h"
#include "odin3/odin3.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum odin3_json_kind {
    ODIN3_JSON_NULL = 0,
    ODIN3_JSON_FALSE,
    ODIN3_JSON_TRUE,
    ODIN3_JSON_INT,
    ODIN3_JSON_STRING,
    ODIN3_JSON_ARRAY,
    ODIN3_JSON_OBJECT
} odin3_json_kind;

/* A value. Fields are private; use the accessors. */
typedef struct odin3_json_node odin3_json_node;

enum { ODIN3_JSON_ERR_MESSAGE = 160 };

/* A position: line and column are 1-based; the column counts bytes. */
typedef struct odin3_json_loc {
    uint32_t line, col;
} odin3_json_loc;

/* Set on failure. line and column are 1-based; the column counts bytes. */
typedef struct odin3_json_error {
    uint32_t line, col;
    char message[ODIN3_JSON_ERR_MESSAGE];
} odin3_json_error;

typedef struct odin3_json_doc {
    odin3_arena *arena;          /* owns every node and string */
    const odin3_json_node *root; /* NULL after a failed parse */
    odin3_json_error error;      /* valid when the parse failed */
} odin3_json_doc;

/*
 * Parses len bytes at text into doc (any prior content of doc is overwritten, not freed). lim
 * NULL means the defaults of limits.h (or what odin3_json_test_set_limits set). text is not
 * kept: strings are copied into doc->arena.
 *
 * Returns ODIN3_OK; ODIN3_ERR_PARSE for malformed input or a limit exceeded, with doc->error
 * naming the line and column of the offending value, key or byte (end of file for a truncated
 * file); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a NULL doc or a NULL text with len > 0. On
 * any failure doc->root is NULL and the doc must still be passed to odin3_json_doc_free. Nothing
 * is logged. The tree lives until odin3_json_doc_free.
 */
odin3_status odin3_json_parse(const uint8_t *text, size_t len, const odin3_json_limits *lim,
                              odin3_json_doc *doc);

/* Frees the arena and empties the doc. Safe on a zeroed doc and twice. */
void odin3_json_doc_free(odin3_json_doc *doc);

/* Test hook (not ABI): the limits used when lim is NULL. NULL restores the defaults. */
void odin3_json_test_set_limits(const odin3_json_limits *lim);

odin3_json_kind odin3_json_kind_of(const odin3_json_node *node);

/*
 * The value of an INT node, 0 for any other kind. A STRING node's bytes (NUL-terminated, owned by
 * the doc) and its length in *len (len may be NULL); NULL for any other kind.
 */
int64_t odin3_json_int(const odin3_json_node *node);
const char *odin3_json_string(const odin3_json_node *node, size_t *len);

/*
 * Children in file order: the first child of an array or object (NULL if empty or another kind),
 * the next sibling (NULL at the end), the number of children, and the member key of a node that
 * is an object member (NULL for array elements and the root). odin3_json_member is a linear
 * search of an object for key (NULL if absent or not an object).
 */
const odin3_json_node *odin3_json_first(const odin3_json_node *node);
const odin3_json_node *odin3_json_next(const odin3_json_node *node);
size_t odin3_json_count(const odin3_json_node *node);
const char *odin3_json_key(const odin3_json_node *node);
const odin3_json_node *odin3_json_member(const odin3_json_node *obj, const char *key);

/* Position of the first byte of the value; of the key for a member. */
odin3_json_loc odin3_json_pos(const odin3_json_node *node);
odin3_json_loc odin3_json_key_pos(const odin3_json_node *node);

#endif
