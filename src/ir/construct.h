/*
 * construct.h — the construct table (IR-12a; docs/V1.2.0.md 2, 4.3): the AST constructs that
 * SOURCE records point at, held by the design so a record's chain (statement, block, module, ...)
 * survives the AST store. Design-global, append-only, additive: no field of a record changes.
 * Rows are parents first. The elaborator fills the table at the end of stage 2
 * (odin3_construct_snapshot, construct_snapshot.c); a design that was not read from source holds an
 * empty table unless a test adds rows.
 */
#ifndef ODIN3_IR_CONSTRUCT_H
#define ODIN3_IR_CONSTRUCT_H

#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The part of a row: the node itself, or the synthesized then and else arm of an IF (2.2). */
enum {
    ODIN3_CONSTRUCT_PART_NODE = 0,
    ODIN3_CONSTRUCT_PART_THEN = 1,
    ODIN3_CONSTRUCT_PART_ELSE = 2
};

/*
 * One construct. Row numbers are 1-based; 0 means none. parent is a row number smaller than the
 * row's own. call is the row of the call-site row that a call-instance copy of a function body
 * hangs under (0 for an ordinary row). next is private to the table (the chain of rows with one
 * (run, ast)); callers leave it 0.
 */
typedef struct odin3_construct_row {
    uint32_t run;
    uint32_t ast;
    uint32_t call;
    uint32_t parent;
    uint32_t file; /* strtab ID, 0 unknown */
    uint32_t name; /* strtab ID, 0 none */
    uint32_t line, col, end_line, end_col;
    uint32_t next;
    uint8_t part;
    uint8_t level; /* odin3_level */
    uint8_t kind;  /* odin3_ast_kind, or an ODIN3_CONSTRUCT_* value */
} odin3_construct_row;

/*
 * True when row may hang under parent (NULL: no parent): the parent's level is the row's or
 * coarser, or the row is a branch under a statement (an arm or case item under its IF or CASE) or a
 * call-instance copy (call != 0) under its call site. The one rule of odin3_construct_add and
 * check rule 12.
 */
bool odin3_construct_parent_fits(const odin3_construct_row *parent, const odin3_construct_row *row);

/* What `--prov-ast` kept for a run (4.2). OFF: no snapshot ran. */
typedef enum odin3_construct_mode {
    ODIN3_CONSTRUCT_MODE_OFF,
    ODIN3_CONSTRUCT_MODE_REFS,
    ODIN3_CONSTRUCT_MODE_CONSTRUCTS,
    ODIN3_CONSTRUCT_MODE_FULL
} odin3_construct_mode;

/* The mode of a read that gave no `--prov-ast` (the gate of V1.2.0 5.3 decides it). */
#define ODIN3_CONSTRUCT_MODE_DEFAULT ODIN3_CONSTRUCT_MODE_REFS

/* The kind map (2.1). Every kind of odin3_ast_kind_name has exactly one entry. */
typedef enum odin3_construct_kind_class {
    ODIN3_KCLASS_UNKNOWN,     /* not a kind (0, past the synthesized rows) */
    ODIN3_KCLASS_TRANSPARENT, /* no row: children attach to the nearest ancestor with one */
    ODIN3_KCLASS_LEVELLED     /* a row, of odin3_construct_kind_level(kind) */
} odin3_construct_kind_class;

odin3_construct_kind_class odin3_construct_kind_class_of(uint32_t kind);

/* The level of a kind's row: NONE for a transparent or unknown kind. */
odin3_level odin3_construct_kind_level(uint32_t kind);

/* The table: opaque; the design owns one (design.c creates, copies and frees it). */
typedef struct odin3_construct_table odin3_construct_table;

/* A new empty table, NULL on out of memory. */
odin3_construct_table *odin3_construct_create(void);

/* Frees the table; NULL is a no-op. */
void odin3_construct_destroy(odin3_construct_table *table);

/* Makes dst (empty) a copy of src. NO_MEMORY on out of memory (dst partly filled). */
odin3_status odin3_construct_copy(odin3_construct_table *dst, const odin3_construct_table *src);

/*
 * Appends row (its next is ignored) and sets *index to its row number. INVALID_ARG (logged) for a
 * run or ast of 0, a kind the map has no row for or a level that does not match it, a part that
 * does not match the kind (part 1 only for IF_THEN, 2 only for IF_ELSE), a parent or call that is
 * not an earlier row, a parent of a finer level than the row (odin3_construct_parent_fits), or a
 * key (run, ast, part, call) that exists; NO_MEMORY (also when the table is full: 2^32 - 2 rows).
 * Nothing changes on failure. That the parent of an arm row is its IF row is left to check rule 12.
 */
odin3_status odin3_construct_add(odin3_design *design, const odin3_construct_row *row,
                                 uint32_t *index);

/* The key of a row: the node of a run, the part of it and the call instance (0 for none). */
typedef struct odin3_construct_key {
    odin3_passrun_id run;
    uint32_t ast;
    uint32_t part;
    uint32_t call;
} odin3_construct_key;

/* The row with this key, 0 when there is none. (V1.2.0 4.3 lists find(design, run, ast, part,
 * call); the key is one argument so that no two adjacent arguments can be swapped.) */
uint32_t odin3_construct_find(const odin3_design *design, const odin3_construct_key *key);

/* The row numbered index, NULL for 0 or past the last. Valid until the next add. */
const odin3_construct_row *odin3_construct_get(const odin3_design *design, uint32_t index);

/* One past the last row number (1 when the table is empty). */
uint32_t odin3_construct_end(const odin3_design *design);

/* Bytes the table holds (rows and index reserved). 0 for a NULL design. */
size_t odin3_construct_bytes_reserved(const odin3_design *design);

/*
 * The one cache a reader keeps per design for data it derives from records, rows and operation
 * classes (prov_export's anchor sets). Returns the cache made by create, or makes it. The cache is
 * valid for one stamp: the end of the record store, of the construct table and the count of
 * declared classes and overrides. Those only grow (records are never deleted or changed, rows and
 * classes are appended), so an unchanged stamp means every derived value is still right; a new
 * stamp frees the old cache with destroy and makes a new one. odin3_design_clone does not copy
 * it. Readers fill it through a const design, like the visit marks of odin3_prov_sources; two
 * threads must not read one design at once. NULL when create fails (out of memory) or on NULL.
 */
void *odin3_construct_derived(const odin3_design *design, void *(*create)(void),
                              void (*destroy)(void *));

/* True when some row of run has node ast with part 0 (a call-instance copy counts). */
bool odin3_construct_has_node(const odin3_design *design, odin3_passrun_id run, uint32_t ast);

/*
 * The row of a SOURCE record: key (run, ast, 0, 0); for a call-site record (two or more
 * locations) the call-instance row whose call row has exactly the span of the second location.
 * 0 for a record that is not SOURCE, has ast 0, or matches no row (a span-only leaf).
 */
uint32_t odin3_construct_of_record(const odin3_design *design, odin3_prov_id record);

/* Which runs had a snapshot, and what it kept (V1.2.0 4.2). A run marked twice keeps the first
 * mode. INVALID_ARG for run 0 or mode OFF; NO_MEMORY. */
odin3_status odin3_construct_mark_run(odin3_design *design, odin3_passrun_id run,
                                      odin3_construct_mode mode);

/* The mode of run's snapshot, OFF when none ran. */
odin3_construct_mode odin3_construct_run_mode(const odin3_design *design, odin3_passrun_id run);

/* Records whose call site matched no row (V1.2.0 4.3): span-only leaves, "recorded misses".
 * Records must arrive in increasing order. INVALID_ARG otherwise; NO_MEMORY. */
odin3_status odin3_construct_note_miss(odin3_design *design, odin3_prov_id record);
bool odin3_construct_is_miss(const odin3_design *design, odin3_prov_id record);

/*
 * Makes node ast of run resolve to row, the row of its nearest construct ancestor: a node that has
 * no row of its own (an expression under `--prov-ast constructs`, a transparent kind) is found by
 * odin3_construct_find and _of_record as that row. INVALID_ARG (logged) for a run or ast of 0, a
 * row that does not exist or whose node has other rows already (only a node's first row can be
 * aliased), or a node that already has a row or an alias; NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_construct_alias(odin3_design *design, odin3_passrun_id run, uint32_t ast,
                                   uint32_t row);

/*
 * The mode the next snapshot of this design takes when its caller gives none (read_verilog sets it
 * from `--prov-ast` for the length of one read; the slang reader reaches stage 2 through
 * odin3_ast_elaborate, which has no room for an option, so the mode travels here). A design starts
 * with ODIN3_CONSTRUCT_MODE_DEFAULT; a clone starts again from it. The setter ignores a NULL
 * design.
 */
void odin3_construct_set_read_mode(odin3_design *design, odin3_construct_mode mode);
odin3_construct_mode odin3_construct_read_mode(const odin3_design *design);

/*
 * The snapshot (V1.2.0 4.3 item 4): from the ELABORATED store ast (its run is the run of the SOURCE
 * records), builds the rows of `mode` by a root-down walk with true child edges, marks the run with
 * mode and notes the call-site misses.
 *   REFS         the nodes a SOURCE record of the run points at, and all their ancestors;
 *   CONSTRUCTS   the same without expression rows (an expression resolves to its nearest construct
 *                ancestor through odin3_construct_alias);
 *   FULL         every node that has a kind row;
 *   OFF          nothing: no rows, no mark, no cost.
 * The arms of an IF are synthesized rows (2.2). A SOURCE record with two locations (the call-site
 * form) gets a call-instance copy of the rows between its node and the function or task declaration
 * under the CALL or TASK_CALL row of exactly the second location; a call site that matches no row
 * or more than one is a recorded miss. Kinds that are transparent (2.1) have no row. Failures:
 * INVALID_ARG (logged) for a NULL design or ast, a store that is not ELABORATED or whose run is not
 * a run of the design, a run already marked with another mode; NO_MEMORY. A failure leaves the rows
 * made so far (the read fails and the design is destroyed).
 */
odin3_status odin3_construct_snapshot(odin3_design *design, const odin3_ast *ast,
                                      odin3_construct_mode mode);

#endif
