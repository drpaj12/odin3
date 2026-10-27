/* opclass.c — the op-class table: default class per (run, op), override per DERIVED record. */
#include "ir/opclass.h"

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Packed class: dir bits 0-2, from 3-6, to 7-10, bulk 11, tail 12-19. */
enum {
    SHIFT_FROM = 3,
    SHIFT_TO = 7,
    SHIFT_BULK = 11,
    SHIFT_TAIL = 12,
    MASK_DIR = 0x7U,
    MASK_LEVEL = 0xFU,
    MASK_TAIL = 0xFFU
};

/* Operations first..last (inclusive) of run, all with the packed class. 16 bytes. */
typedef struct range_row {
    uint32_t run;
    uint32_t first;
    uint32_t last;
    uint32_t packed;
} range_row;

/* A record's override. 8 bytes. */
typedef struct override_row {
    uint32_t record;
    uint32_t packed;
} override_row;

struct odin3_opclass_table {
    odin3_vec ranges;    /* range_row, sorted by (run, first), disjoint */
    odin3_vec overrides; /* override_row, sorted by record */
    bool failed;         /* an invalid class was declared (programming error; check rule 13) */
    bool pending;        /* a begin ran out of memory: the next derive reports it */
    uint32_t conflicts;
    uint32_t first_conflict;
};

static const char *const LEVEL_WORDS[] = {"none", "module", "instance", "block", "branch",
                                          "stmt", "expr",   "word",     "gate",  "lut",
                                          "ble",  "clb",    "site"};
static const char *const DIR_WORDS[] = {"origin", "refine", "rewrite", "coarsen", "context"};

const char *odin3_level_word(unsigned level) {
    return level < sizeof LEVEL_WORDS / sizeof LEVEL_WORDS[0] ? LEVEL_WORDS[level] : "none";
}

const char *odin3_dir_word(unsigned dir) {
    return dir < sizeof DIR_WORDS / sizeof DIR_WORDS[0] ? DIR_WORDS[dir] : "origin";
}

static bool word_index(const char *const *words, size_t count, odin3_bytes word, size_t *index) {
    for (size_t k = 0; word.ptr != NULL && k < count; k++) {
        if (strlen(words[k]) == word.len && memcmp(words[k], word.ptr, word.len) == 0) {
            *index = k;
            return true;
        }
    }
    return false;
}

bool odin3_level_from_word(odin3_bytes word, odin3_level *out) {
    size_t index = 0;
    bool found = word_index(LEVEL_WORDS, sizeof LEVEL_WORDS / sizeof LEVEL_WORDS[0], word, &index);
    *out = found ? (odin3_level)index : ODIN3_LEVEL_NONE;
    return found;
}

bool odin3_dir_from_word(odin3_bytes word, odin3_prov_dir *out) {
    size_t index = 0;
    bool found = word_index(DIR_WORDS, sizeof DIR_WORDS / sizeof DIR_WORDS[0], word, &index);
    *out = found ? (odin3_prov_dir)index : ODIN3_DIR_ORIGIN;
    return found;
}

bool odin3_opclass_valid(const odin3_opclass *cls) {
    if (cls == NULL || cls->dir == ODIN3_DIR_ORIGIN || cls->dir > ODIN3_DIR_CONTEXT) {
        return false;
    }
    if (cls->from_level == ODIN3_LEVEL_NONE || cls->from_level > ODIN3_LEVEL_SITE ||
        cls->to_level == ODIN3_LEVEL_NONE || cls->to_level > ODIN3_LEVEL_SITE) {
        return false;
    }
    /* bulk marks the abc over-budget records, which are coarsen (prov_export counts |J| only for
     * a coarsen record); a context tail belongs to a context class. */
    if (cls->bulk && cls->dir != ODIN3_DIR_COARSEN) {
        return false;
    }
    return cls->context_tail == 0 || cls->dir == ODIN3_DIR_CONTEXT;
}

bool odin3_opclass_equal(const odin3_opclass *lhs, const odin3_opclass *rhs) {
    return lhs->dir == rhs->dir && lhs->from_level == rhs->from_level &&
           lhs->to_level == rhs->to_level && lhs->bulk == rhs->bulk &&
           lhs->context_tail == rhs->context_tail;
}

static uint32_t pack(const odin3_opclass *cls) {
    return (uint32_t)cls->dir | ((uint32_t)cls->from_level << SHIFT_FROM) |
           ((uint32_t)cls->to_level << SHIFT_TO) | ((uint32_t)cls->bulk << SHIFT_BULK) |
           ((uint32_t)cls->context_tail << SHIFT_TAIL);
}

static void unpack(uint32_t packed, odin3_opclass *out) {
    out->dir = (odin3_prov_dir)(packed & MASK_DIR);
    out->from_level = (odin3_level)((packed >> SHIFT_FROM) & MASK_LEVEL);
    out->to_level = (odin3_level)((packed >> SHIFT_TO) & MASK_LEVEL);
    out->bulk = ((packed >> SHIFT_BULK) & 1U) != 0;
    out->context_tail = (uint8_t)((packed >> SHIFT_TAIL) & MASK_TAIL);
}

odin3_opclass_table *odin3_opclass_create(void) {
    odin3_opclass_table *table = odin3_util_calloc(sizeof *table);
    if (table == NULL) {
        return NULL;
    }
    odin3_vec_init(&table->ranges, sizeof(range_row));
    odin3_vec_init(&table->overrides, sizeof(override_row));
    return table;
}

void odin3_opclass_destroy(odin3_opclass_table *table) {
    if (table == NULL) {
        return;
    }
    odin3_vec_free(&table->ranges);
    odin3_vec_free(&table->overrides);
    odin3_util_free(table);
}

odin3_status odin3_opclass_copy(odin3_opclass_table *dst, const odin3_opclass_table *src) {
    if (odin3_vec_copy(&dst->ranges, &src->ranges) != ODIN3_OK ||
        odin3_vec_copy(&dst->overrides, &src->overrides) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    dst->failed = src->failed;
    dst->conflicts = src->conflicts;
    dst->first_conflict = src->first_conflict;
    return ODIN3_OK;
}

/* Index of the first row that starts after (run, op), i.e. the number of rows at or before it. */
static size_t upper_bound(const odin3_opclass_table *table, uint32_t run, uint32_t op) {
    size_t lo = 0;
    size_t hi = table->ranges.len;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        const range_row *row = odin3_vec_cat(&table->ranges, mid);
        if (row->run < run || (row->run == run && row->first <= op)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The row covering (run, op), NULL when none. */
static const range_row *range_find(const odin3_opclass_table *table, uint32_t run, uint32_t op) {
    size_t at = upper_bound(table, run, op);
    if (at == 0) {
        return NULL;
    }
    const range_row *row = odin3_vec_cat(&table->ranges, at - 1);
    return row->run == run && op <= row->last ? row : NULL;
}

/* Inserts row before index at (rows after it move up one). */
static odin3_status range_insert(odin3_opclass_table *table, size_t at, range_row row) {
    if (odin3_vec_push(&table->ranges) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    range_row *rows = table->ranges.data;
    memmove(&rows[at + 1], &rows[at], (table->ranges.len - 1 - at) * sizeof *rows);
    rows[at] = row;
    return ODIN3_OK;
}

odin3_status odin3_opclass_declare(odin3_opclass_table *table, odin3_passrun_id run, uint32_t op,
                                   const odin3_opclass *cls) {
    if (!odin3_opclass_valid(cls) || run.v == 0 || op == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_opclass_declare: invalid class, run %u or op %u", run.v,
                  op);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t packed = pack(cls);
    const range_row *have = range_find(table, run.v, op);
    if (have != NULL) {
        if (have->packed == packed) {
            return ODIN3_OK;
        }
        odin3_log(ODIN3_LOG_ERROR, "odin3_opclass_declare: run %u op %u already has a class", run.v,
                  op);
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t at = upper_bound(table, run.v, op);
    if (at > 0) {
        range_row *prev = odin3_vec_at(&table->ranges, at - 1);
        if (prev->run == run.v && prev->packed == packed && prev->last + 1 == op) {
            prev->last = op;
            return ODIN3_OK;
        }
    }
    return range_insert(table, at, (range_row){run.v, op, op, packed});
}

bool odin3_opclass_default(const odin3_opclass_table *table, odin3_passrun_id run, uint32_t op,
                           odin3_opclass *out) {
    const range_row *row = range_find(table, run.v, op);
    if (row == NULL) {
        return false;
    }
    unpack(row->packed, out);
    return true;
}

odin3_status odin3_opclass_override(odin3_opclass_table *table, odin3_prov_id record,
                                    const odin3_opclass *cls) {
    size_t len = table->overrides.len;
    const override_row *last = len > 0 ? odin3_vec_cat(&table->overrides, len - 1) : NULL;
    if (!odin3_opclass_valid(cls) || record.v == 0 || (last != NULL && last->record >= record.v)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_opclass_override: invalid class or record %u", record.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    override_row *slot = odin3_vec_push(&table->overrides);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (override_row){record.v, pack(cls)};
    return ODIN3_OK;
}

bool odin3_opclass_override_of(const odin3_opclass_table *table, odin3_prov_id record,
                               odin3_opclass *out) {
    size_t lo = 0;
    size_t hi = table->overrides.len;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        const override_row *row = odin3_vec_cat(&table->overrides, mid);
        if (row->record == record.v) {
            unpack(row->packed, out);
            return true;
        }
        if (row->record < record.v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool odin3_opclass_of(const odin3_opclass_table *table, odin3_prov_id record, odin3_passrun_id run,
                      uint32_t op, odin3_opclass *out) {
    return odin3_opclass_override_of(table, record, out) ||
           odin3_opclass_default(table, run, op, out);
}

uint32_t odin3_opclass_row_count(const odin3_opclass_table *table) {
    return (uint32_t)table->ranges.len;
}

uint32_t odin3_opclass_override_count(const odin3_opclass_table *table) {
    return (uint32_t)table->overrides.len;
}

void odin3_opclass_note_failure(odin3_opclass_table *table) {
    table->failed = true;
}

void odin3_opclass_set_pending(odin3_opclass_table *table, bool pending) {
    table->pending = pending;
}

bool odin3_opclass_take_pending(odin3_opclass_table *table) {
    bool was = table->pending;
    table->pending = false;
    return was;
}

odin3_status odin3_opclass_reserve_override(odin3_opclass_table *table) {
    return odin3_vec_reserve(&table->overrides, table->overrides.len + 1U);
}

void odin3_opclass_note_conflict(odin3_opclass_table *table, odin3_prov_id record) {
    if (table->conflicts == 0) {
        table->first_conflict = record.v;
    }
    table->conflicts++;
}

bool odin3_opclass_failed(const odin3_opclass_table *table) {
    return table->failed;
}

uint32_t odin3_opclass_conflicts(const odin3_opclass_table *table, odin3_prov_id *first) {
    if (first != NULL) {
        first->v = table->first_conflict;
    }
    return table->conflicts;
}

size_t odin3_opclass_bytes_reserved(const odin3_opclass_table *table) {
    if (table == NULL) {
        return 0;
    }
    return sizeof *table + (table->ranges.cap * table->ranges.elem_size) +
           (table->overrides.cap * table->overrides.elem_size);
}
