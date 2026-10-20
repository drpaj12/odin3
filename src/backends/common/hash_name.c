/*
 * hash_name.c — hashed object names (spec 5.4f, H-2, H-3). See hash_name.h.
 */
#include "backends/common/hash_name.h"

#include "util/alloc.h"
#include "util/log.h"
#include "util/sha256.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { BITS_PER_CHAR = 5, BYTE_BITS = 8, CHAR_MASK = 0x1F, DIGEST_BITS = 256 };

static const char BASE32[] = "abcdefghijklmnopqrstuvwxyz234567";

/* The 5 bits at bit offset `bit` of digest (big-endian); bits past the 256th read 0. */
static uint32_t digest_group(const uint8_t digest[ODIN3_SHA256_DIGEST_BYTES], uint32_t bit) {
    uint32_t value = 0;
    for (uint32_t k = 0; k < BITS_PER_CHAR; k++) {
        uint32_t pos = bit + k;
        uint32_t one = 0;
        if (pos < DIGEST_BITS) {
            one = ((uint32_t)digest[pos / BYTE_BITS] >> (BYTE_BITS - 1U - pos % BYTE_BITS)) & 1U;
        }
        value = (value << 1) | one;
    }
    return value & CHAR_MASK;
}

void odin3_base32_encode(const uint8_t digest[ODIN3_SHA256_DIGEST_BYTES], uint32_t chars,
                         char *out) {
    for (uint32_t i = 0; i < chars; i++) {
        out[i] = BASE32[digest_group(digest, i * BITS_PER_CHAR)];
    }
    out[chars] = '\0';
}

void odin3_hash_name_make(odin3_bytes full, uint32_t chars, odin3_hash_name *out) {
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_sha256 ctx;
    if (chars < 1) {
        chars = 1;
    }
    if (chars > ODIN3_HASH_NAME_MAX_CHARS) {
        chars = ODIN3_HASH_NAME_MAX_CHARS;
    }
    odin3_sha256_init(&ctx);
    odin3_sha256_update(&ctx, full.ptr, full.len);
    odin3_sha256_final(&ctx, digest);
    out->text[0] = 'h';
    odin3_base32_encode(digest, chars, out->text + 1);
}

/* --- collision resolution -----------------------------------------------------------------------
 */

/* One name at its current width, for sorting into runs of equal text. */
typedef struct hn_row {
    char text[ODIN3_HASH_NAME_BUF];
    odin3_bytes full;
    uint32_t idx;
} hn_row;

/* The working set of one resolve call. */
typedef struct hn_work {
    const odin3_bytes *fulls;
    uint32_t n;
    odin3_hash_name *out;
    hn_row *rows;
    uint8_t *widths;       /* current width of each name (at most 52) */
    uint8_t *marks;        /* names to extend this round */
    odin3_bytes *reserved; /* sorted copy */
    uint32_t nreserved;
} hn_work;

static int bytes_cmp(odin3_bytes lhs, odin3_bytes rhs) {
    size_t common = lhs.len < rhs.len ? lhs.len : rhs.len;
    int cmp = common == 0 ? 0 : memcmp(lhs.ptr, rhs.ptr, common);
    if (cmp != 0 || lhs.len == rhs.len) {
        return cmp;
    }
    return lhs.len < rhs.len ? -1 : 1;
}

static int row_cmp(const void *lhs, const void *rhs) {
    const hn_row *row_l = (const hn_row *)lhs;
    const hn_row *row_r = (const hn_row *)rhs;
    int cmp = strcmp(row_l->text, row_r->text);
    return cmp != 0 ? cmp : bytes_cmp(row_l->full, row_r->full);
}

static int reserved_cmp(const void *lhs, const void *rhs) {
    return bytes_cmp(*(const odin3_bytes *)lhs, *(const odin3_bytes *)rhs);
}

static void work_free(hn_work *work) {
    odin3_util_free(work->rows);
    odin3_util_free(work->widths);
    odin3_util_free(work->marks);
    odin3_util_free(work->reserved);
}

static odin3_status work_init(hn_work *work, const odin3_bytes *reserved, uint32_t start) {
    size_t count = work->n;
    work->rows = (hn_row *)odin3_util_malloc(count * sizeof(hn_row));
    work->widths = (uint8_t *)odin3_util_malloc(count);
    work->marks = (uint8_t *)odin3_util_malloc(count);
    if (work->nreserved > 0) {
        work->reserved = (odin3_bytes *)odin3_util_malloc(work->nreserved * sizeof(odin3_bytes));
    }
    if (work->rows == NULL || work->widths == NULL || work->marks == NULL ||
        (work->nreserved > 0 && work->reserved == NULL)) {
        work_free(work);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (work->nreserved > 0) {
        memcpy(work->reserved, reserved, work->nreserved * sizeof(odin3_bytes));
        qsort(work->reserved, work->nreserved, sizeof(odin3_bytes), reserved_cmp);
    }
    memset(work->widths, (int)start, count);
    memset(work->marks, 0, count);
    return ODIN3_OK;
}

static bool is_reserved(const hn_work *work, const odin3_hash_name *name) {
    odin3_bytes key = odin3_bytes_cstr(name->text);
    return work->nreserved > 0 && bsearch(&key, work->reserved, work->nreserved,
                                          sizeof(odin3_bytes), reserved_cmp) != NULL;
}

/* Marks every name of the run rows[first..last] when the run holds two distinct full names. */
static void mark_run(hn_work *work, uint32_t first, uint32_t last) {
    if (bytes_cmp(work->rows[first].full, work->rows[last].full) == 0) {
        return; /* equal full names are one name */
    }
    for (uint32_t i = first; i <= last; i++) {
        work->marks[work->rows[i].idx] = 1;
    }
}

/* Marks the names whose text collides with another name's, and those equal to a reserved name. */
static void mark_collisions(hn_work *work) {
    for (uint32_t i = 0; i < work->n; i++) {
        work->rows[i].idx = i;
        work->rows[i].full = work->fulls[i];
        (void)memcpy(work->rows[i].text, work->out[i].text, ODIN3_HASH_NAME_BUF);
        if (is_reserved(work, &work->out[i])) {
            work->marks[i] = 1;
        }
    }
    qsort(work->rows, work->n, sizeof(hn_row), row_cmp);
    uint32_t first = 0;
    for (uint32_t i = 1; i <= work->n; i++) {
        if (i == work->n || strcmp(work->rows[i].text, work->rows[first].text) != 0) {
            mark_run(work, first, i - 1);
            first = i;
        }
    }
}

/* Widens every marked name by one step; false when a marked name is already at the cap. */
static bool widen_marked(hn_work *work, bool *changed) {
    *changed = false;
    for (uint32_t i = 0; i < work->n; i++) {
        if (work->marks[i] == 0) {
            continue;
        }
        work->marks[i] = 0;
        if (work->widths[i] >= ODIN3_HASH_NAME_MAX_CHARS) {
            odin3_log(ODIN3_LOG_ERROR,
                      "hash names: '%s' still collides at %d characters; no hash name",
                      work->out[i].text, ODIN3_HASH_NAME_MAX_CHARS);
            return false;
        }
        uint32_t next = work->widths[i] + ODIN3_HASH_NAME_STEP;
        work->widths[i] =
            (uint8_t)(next > ODIN3_HASH_NAME_MAX_CHARS ? ODIN3_HASH_NAME_MAX_CHARS : next);
        odin3_hash_name_make(work->fulls[i], work->widths[i], &work->out[i]);
        *changed = true;
    }
    return true;
}

odin3_status odin3_hash_names_resolve_at(const odin3_hash_name_set *set, uint32_t start,
                                         odin3_hash_name *out) {
    if (set->n > ODIN3_HASH_NAME_MAX_ENTRIES || set->nreserved > ODIN3_HASH_NAME_MAX_ENTRIES ||
        start < 1 || start > ODIN3_HASH_NAME_MAX_CHARS) {
        odin3_log(ODIN3_LOG_ERROR,
                  "hash names: %u names, %u reserved (limit %d each) or start width %u invalid",
                  set->n, set->nreserved, ODIN3_HASH_NAME_MAX_ENTRIES, start);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (set->n == 0) {
        return ODIN3_OK;
    }
    hn_work work = {.fulls = set->fulls, .n = set->n, .out = out, .nreserved = set->nreserved};
    odin3_status st = work_init(&work, set->reserved, start);
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t i = 0; i < set->n; i++) {
        odin3_hash_name_make(set->fulls[i], work.widths[i], &out[i]);
    }
    bool changed = true;
    while (changed) {
        mark_collisions(&work);
        if (!widen_marked(&work, &changed)) {
            st = ODIN3_ERR_INVALID_ARG;
            break;
        }
    }
    work_free(&work);
    return st;
}

odin3_status odin3_hash_names_resolve(const odin3_bytes *fulls, uint32_t n,
                                      const odin3_bytes *reserved, uint32_t nreserved,
                                      odin3_hash_name *out) {
    const odin3_hash_name_set set = {fulls, n, reserved, nreserved};
    return odin3_hash_names_resolve_at(&set, ODIN3_HASH_NAME_WIDTH, out);
}
