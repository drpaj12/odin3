/*
 * opclass.h — the op-class table (IR-12a; docs/V1.2.0.md 3.1): the class of a provenance
 * operation, declared by the pass. A default per (run, op) and an optional override per DERIVED
 * record. Design-global, additive: no field of a record changes.
 */
#ifndef ODIN3_IR_OPCLASS_H
#define ODIN3_IR_OPCLASS_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The granularity of an object or construct, coarse to fine (V1.2.0 section 2). */
typedef enum odin3_level {
    ODIN3_LEVEL_NONE,
    ODIN3_LEVEL_MODULE,
    ODIN3_LEVEL_INSTANCE,
    ODIN3_LEVEL_BLOCK,
    ODIN3_LEVEL_BRANCH,
    ODIN3_LEVEL_STMT,
    ODIN3_LEVEL_EXPR,
    ODIN3_LEVEL_WORD,
    ODIN3_LEVEL_GATE,
    ODIN3_LEVEL_LUT,
    ODIN3_LEVEL_BLE,
    ODIN3_LEVEL_CLB,
    ODIN3_LEVEL_SITE
} odin3_level;

/* How a derived record relates to its parents. ORIGIN is SOURCE or IMPORTED: never declared. */
typedef enum odin3_prov_dir {
    ODIN3_DIR_ORIGIN,
    ODIN3_DIR_REFINE,  /* one parent, many pieces share the record */
    ODIN3_DIR_REWRITE, /* 1:1, or a regrouping that is not a blend */
    ODIN3_DIR_COARSEN, /* N to 1 */
    ODIN3_DIR_CONTEXT  /* the record adds the instance (flatten) */
} odin3_prov_dir;

/*
 * A declared class. bulk: the over-budget or unanchored-objects record of abc. context_tail: how
 * many parents at the end of the parent list are context parents; 0 unless dir is CONTEXT.
 */
typedef struct odin3_opclass {
    odin3_prov_dir dir;
    odin3_level from_level;
    odin3_level to_level;
    bool bulk;
    uint8_t context_tail;
} odin3_opclass;

/* True when dir is not ORIGIN and in range, both levels are set and in range, and a tail is 0
 * unless dir is CONTEXT. */
bool odin3_opclass_valid(const odin3_opclass *cls);

/* True when a and b are the same class in every field. */
bool odin3_opclass_equal(const odin3_opclass *a, const odin3_opclass *b);

/* The table: opaque; the design owns one (design.c creates, copies and frees it). */
typedef struct odin3_opclass_table odin3_opclass_table;

/* A new empty table, NULL on out of memory. */
odin3_opclass_table *odin3_opclass_create(void);

/* Frees the table; NULL is a no-op. */
void odin3_opclass_destroy(odin3_opclass_table *table);

/* Makes dst (empty) a copy of src. NO_MEMORY on out of memory (dst partly filled). */
odin3_status odin3_opclass_copy(odin3_opclass_table *dst, const odin3_opclass_table *src);

/*
 * Declares cls as the default class of operation op of run. Ranges of consecutive operations of
 * one run with one class share a row, so a run costs a few rows. INVALID_ARG (logged) for a class
 * odin3_opclass_valid rejects, for run or op 0, or for an op that already has another default;
 * NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_opclass_declare(odin3_opclass_table *table, odin3_passrun_id run, uint32_t op,
                                   const odin3_opclass *cls);

/* The default class of (run, op): true and *out set, false when the op has none. */
bool odin3_opclass_default(const odin3_opclass_table *table, odin3_passrun_id run, uint32_t op,
                           odin3_opclass *out);

/*
 * Records cls as the override of a DERIVED record. Overrides must arrive in record order (a new
 * record has the largest ID), which the derive path guarantees. INVALID_ARG for a bad class, for
 * record 0 or one not past the last override; NO_MEMORY.
 */
odin3_status odin3_opclass_override(odin3_opclass_table *table, odin3_prov_id record,
                                    const odin3_opclass *cls);

/* Makes room for one more override so that odin3_opclass_override cannot then run out of memory.
 * NO_MEMORY. */
odin3_status odin3_opclass_reserve_override(odin3_opclass_table *table);

/* The override of record: true and *out set, false when it has none. */
bool odin3_opclass_override_of(const odin3_opclass_table *table, odin3_prov_id record,
                               odin3_opclass *out);

/* The class of a record made in (run, op): its override, else the default; false when neither. */
bool odin3_opclass_of(const odin3_opclass_table *table, odin3_prov_id record, odin3_passrun_id run,
                      uint32_t op, odin3_opclass *out);

/* Counts, for tests and check: default rows, override rows. */
uint32_t odin3_opclass_row_count(const odin3_opclass_table *table);
uint32_t odin3_opclass_override_count(const odin3_opclass_table *table);

/*
 * Problems the table saw while the passes ran. A begin cannot fail (it returns nothing): an out of
 * memory there is held (pending) and the next derive returns NO_MEMORY, through the pass's own
 * error path. An invalid class, and derives of one record that declared two classes (conflicts;
 * the first such record is kept), are programming errors that no output depends on: check rule 13
 * reports them.
 */
void odin3_opclass_note_failure(odin3_opclass_table *table);
void odin3_opclass_set_pending(odin3_opclass_table *table, bool pending);
bool odin3_opclass_take_pending(odin3_opclass_table *table);
void odin3_opclass_note_conflict(odin3_opclass_table *table, odin3_prov_id record);
bool odin3_opclass_failed(const odin3_opclass_table *table);
uint32_t odin3_opclass_conflicts(const odin3_opclass_table *table, odin3_prov_id *first);

/* Bytes the table holds (rows reserved). 0 for NULL. */
size_t odin3_opclass_bytes_reserved(const odin3_opclass_table *table);

#endif
