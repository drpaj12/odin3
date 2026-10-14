/*
 * pg_init.h — what pg_init.c (the initial values of EL-40: the interpreter's store, the walk over
 * the `initial` blocks and variable initialisers, the results) and pg_readmem.c (`$readmemh/b`)
 * share. Private to the two files and to their tests.
 *
 * The state is one 4-state byte (an odin3_bit) per bit of every variable and memory word an
 * `initial` wrote, in one pool; a bit never written is not in it and reads x. A statement's writes
 * are journalled so a statement that turns out not to be constant leaves its targets as they were.
 */
#ifndef ODIN3_FRONTENDS_ELAB_PG_INIT_H
#define ODIN3_FRONTENDS_ELAB_PG_INIT_H

#include "frontends/elab/ceval.h"
#include "frontends/elab/elab_internal.h"

#include "ast/ast.h"
#include "ast/symtab.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* A variable an initial wrote: its symbol, width, where its bits begin in the pool and the place
 * of the first statement that wrote it (the place of a warning about its result). */
typedef struct odin3_init_var {
    odin3_sym_id sym;
    uint32_t width;
    uint64_t off;
    odin3_loc loc;
} odin3_init_var;

/* One journalled write: bytes [off, off + n) of the pool had the saved bytes (from saved). */
typedef struct odin3_init_undo {
    uint64_t off, saved;
    uint32_t n;
} odin3_init_undo;

typedef struct odin3_init {
    odin3_elab2 *e2;
    odin3_ceval *ev;
    odin3_vec bits;     /* uint8_t: the pool */
    odin3_u64map *vars; /* symbol -> index + 1 into vlist */
    odin3_vec vlist;    /* odin3_init_var, in the order first written */
    odin3_vec moff;     /* uint64_t per memory table entry: pool offset + 1 (0: untouched) */
    odin3_vec mloc;     /* odin3_loc per memory table entry: its first writer */
    odin3_vec journal;  /* odin3_init_undo of the statement being run */
    odin3_vec saved;    /* uint8_t: the bytes the journal restores */
    odin3_vec blocks;   /* init_frame: the `begin ... end` blocks being walked (no recursion) */
    uint64_t total;     /* bits allocated in the pool, against the init-bits cap */
    odin3_ast_id stmt;  /* the statement being run (the place of its errors) */
    odin3_ceval_store store;
} odin3_init;

/*
 * `read_verilog --vtr-compat` (pg_init_vtr.c): the statement of an `initial`, found not constant,
 * as the continuous assignment Parmys reads it as. *handled is true when the statement was
 * decided (assigned with the ruling's warning, or kept ignored with a warning that says why), false
 * when the option is off or the statement is not a plain assignment to a variable: the caller
 * then ignores it as before. Status as odin3_elab2_assign (a counted ERR_PARSE after a located
 * error from the lvalue).
 */
odin3_status odin3_elab2_initial_assign(odin3_elab2 *e2, odin3_ast_id stmt, bool *handled);

/* A word of a memory: the table index and the position (row-major, from 0). */
typedef struct odin3_init_wordref {
    uint32_t mem, pos;
} odin3_init_wordref;

/* The word's bytes in the pool (allocated all x on first use, against the init-bits cap: the
 * located error `initial values need more than N bits` and NULL with *st ODIN3_ERR_PARSE). *st is
 * ODIN3_OK or NO_MEMORY otherwise. */
uint8_t *odin3_init_word(odin3_init *run, const odin3_init_wordref *ref, odin3_status *st);

/* Journals the n bytes at `bytes` (a pointer into the pool) before they are overwritten. */
odin3_status odin3_init_journal(odin3_init *run, const uint8_t *bytes, uint32_t count);

#endif
