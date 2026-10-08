/*
 * str.h — string interning (strtab) and a growable string builder (strbuf).
 */
#ifndef ODIN3_UTIL_STR_H
#define ODIN3_UTIL_STR_H

#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct odin3_strtab odin3_strtab; /* bytes <-> uint32_t ID, on idindex + an arena */

/* Table with "" pre-interned as ID 0; NULL on out of memory. */
odin3_strtab *odin3_strtab_create(void);

/* Frees the table and every interned string; NULL is allowed. */
void odin3_strtab_destroy(odin3_strtab *tab);

/*
 * Stores the ID of str, interning a copy if new. str may hold any byte except NUL (a NUL inside
 * str.len is ODIN3_ERR_INVALID_ARG); {NULL, 0} is the empty string. ODIN3_ERR_NO_MEMORY on out of
 * memory or ID exhaustion, leaving the table unchanged.
 */
odin3_status odin3_strtab_intern(odin3_strtab *tab, odin3_bytes str, uint32_t *id);

/* True and *id (may be NULL) when str is already interned; never inserts. */
bool odin3_strtab_find(const odin3_strtab *tab, odin3_bytes str, uint32_t *id);

/* NUL-terminated string for id; never moves. NULL when id is out of range. */
const char *odin3_strtab_get(const odin3_strtab *tab, uint32_t id);

/* Length of the string for id (0 when out of range). */
size_t odin3_strtab_len(const odin3_strtab *tab, uint32_t id);

/* Number of interned strings, including the empty string. */
size_t odin3_strtab_count(const odin3_strtab *tab);

/*
 * data is NUL-terminated after every successful append that adds bytes; it stays NULL on a fresh
 * buffer after an empty append. Appending a buffer's own data to itself (append, or appendf with
 * %s of buf.data) is unsupported: growth may free the source.
 */
typedef struct odin3_strbuf {
    char *data;
    size_t len, cap;
} odin3_strbuf;

void odin3_strbuf_init(odin3_strbuf *buf);
void odin3_strbuf_free(odin3_strbuf *buf);

/* Appends str (may be {NULL, 0}: no-op). On failure the buffer is unchanged. */
odin3_status odin3_strbuf_append(odin3_strbuf *buf, odin3_bytes str);

/*
 * Appends printf-style output. ODIN3_ERR_INVALID_ARG on a formatting error, NO_MEMORY on out
 * of memory; the buffer is unchanged on failure.
 */
odin3_status odin3_strbuf_appendf(odin3_strbuf *buf, const char *fmt, ...) ODIN3_PRINTF(2, 3);

/* len = 0, keeps capacity; data stays NUL-terminated. */
void odin3_strbuf_clear(odin3_strbuf *buf);

#endif
