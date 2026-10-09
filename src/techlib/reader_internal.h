/*
 * reader_internal.h — state and helpers shared by the .o3lib reader sources (reader.c: lines,
 * statements, registration; reader_cell.c: the statements of a cell). Never included elsewhere.
 */
#ifndef ODIN3_TECHLIB_READER_INTERNAL_H
#define ODIN3_TECHLIB_READER_INTERNAL_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "techlib/expr.h"
#include "techlib/reader.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The status of every malformed-library error, in this one place. */
#define ODIN3_RD_PARSE_ERROR ODIN3_ERR_PARSE

enum { ODIN3_RD_MSG_MAX = 512 };

/* A byte range of the current line; col is the 1-based column of ptr[0]. */
typedef struct odin3_span {
    const char *ptr;
    size_t len;
    uint32_t col;
} odin3_span;

/* A source location; col 0 prints "file:line:". */
typedef struct odin3_rd_loc {
    uint32_t line;
    uint32_t col;
} odin3_rd_loc;

/* A port being declared: its definition (names point into the strtab) and reader bookkeeping. */
typedef struct odin3_rd_port {
    odin3_port_def def;
    uint32_t name;        /* strtab ID */
    uint32_t line;        /* line of its declaration */
    uint32_t driver_line; /* line of the fn/seq/memory driving it; 0 while undriven */
    odin3_techlib_port mods;
} odin3_rd_port;

typedef struct odin3_rd_param {
    uint32_t name; /* strtab ID */
    int64_t dflt;
} odin3_rd_param;

/* The cell between `cell` and `end`. */
typedef struct odin3_rd_cell {
    uint32_t name; /* strtab ID */
    uint32_t line;
    odin3_techlib_kind kind;
    uint32_t area, delay; /* strtab IDs, 0 when absent */
    odin3_vec ports;      /* odin3_rd_port */
    odin3_vec params;     /* odin3_rd_param */
    odin3_vec fns;        /* odin3_techlib_fn */
    odin3_vec seqs;       /* odin3_techlib_seq */
    odin3_vec mports;     /* odin3_techlib_memport */
    bool has_memory;
    uint64_t total_width;        /* port widths so far, with the default parameters */
    odin3_techlib_memory memory; /* words, width, line; the arrays are filled at `end` */
} odin3_rd_cell;

/* A finished cell waiting for registration (everything in the design arena). */
typedef struct odin3_rd_pending {
    uint32_t name; /* strtab ID */
    uint32_t line;
    const odin3_celltype_def *def;
    const odin3_techlib_cell *lib;
} odin3_rd_pending;

typedef struct odin3_reader {
    odin3_design *design;
    odin3_strtab *strtab; /* the design's */
    odin3_arena *arena;   /* the design's cell-type arena: ASTs, definitions, library data */
    const char *file;     /* source name, in the arena */
    const char *line_start;
    uint32_t line;
    uint32_t library; /* strtab ID; 0 before the library statement */
    uint32_t library_line;
    bool in_cell;
    odin3_rd_cell cell;
    odin3_vec pending; /* odin3_rd_pending, file order */
    odin3_vec idents;  /* scratch: const odin3_expr * */
    odin3_vec ids;     /* scratch: uint32_t */
    odin3_strbuf text; /* scratch: an expression padded to its line column */
} odin3_reader;

/* Logs "file:line[:col]: message" and returns ODIN3_RD_PARSE_ERROR. */
odin3_status odin3_rd_err(const odin3_reader *rd, odin3_rd_loc loc, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);
odin3_rd_loc odin3_rd_at(const odin3_reader *rd, uint32_t col);

/* Space, tab, CR, VT or FF (newlines end lines before statements are split). */
bool odin3_rd_space(char ch);
void odin3_rd_advance(odin3_span *sp, size_t count);
void odin3_rd_skip_ws(odin3_span *sp);
odin3_span odin3_rd_trim(odin3_span sp);
/* Next whitespace-delimited word; false (word empty) at the end. */
bool odin3_rd_word(odin3_span *sp, odin3_span *word);
bool odin3_rd_is(odin3_span word, const char *keyword);
/* Next identifier ([A-Za-z_][A-Za-z0-9_$]*) after optional whitespace; false if none starts. */
bool odin3_rd_ident(odin3_span *sp, odin3_span *ident);
/* Consumes lit after optional whitespace; false (nothing consumed but whitespace) if absent. */
bool odin3_rd_lit(odin3_span *sp, const char *lit);
/* Error at the first word of rest, if any. */
odin3_status odin3_rd_end(const odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_intern(odin3_reader *rd, odin3_span sp, uint32_t *id);
/* Parses text (one expression of the current line) into the design arena; located errors. */
odin3_status odin3_rd_expr(odin3_reader *rd, odin3_span text, const odin3_expr **out);

/* reader_cell.c */
void odin3_rd_cell_init(odin3_rd_cell *cell);
void odin3_rd_cell_free(odin3_rd_cell *cell);
const char *odin3_rd_cell_name(const odin3_reader *rd);
odin3_status odin3_rd_st_cell(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_param(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_in(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_out(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_inout(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_fn(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_seq(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_memory(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_mem_width(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_mem_write(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_mem_read(odin3_reader *rd, odin3_span rest);
odin3_status odin3_rd_st_cell_end(odin3_reader *rd, odin3_span rest);

#endif
