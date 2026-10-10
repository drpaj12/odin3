/*
 * srcman.h — the source manager (AST-1..3): buffers, line maps, 32-bit locations with macro and
 * include chains, segment maps of preprocessed streams, printing and located diagnostics.
 */
#ifndef ODIN3_AST_SRCMAN_H
#define ODIN3_AST_SRCMAN_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A location (AST-2): one offset into the design's virtual source space, 0 = unknown. Every
 * buffer occupies [start, start + len] (one-past-the-end is a loc of its own buffer); buffers
 * are allocated in creation order from 1. A buffer ID is its creation index, 0 = none.
 */
typedef struct odin3_loc {
    uint32_t v;
} odin3_loc;

typedef struct odin3_srcbuf_id {
    uint32_t v;
} odin3_srcbuf_id;

/* A node's range: two independent locs that may lie in different buffers; end 0 = unknown. */
typedef struct odin3_range {
    odin3_loc loc;
    odin3_loc end;
} odin3_range;

/* Source limits (spec §3.5); exceeding one is a located ODIN3_ERR_PARSE. */
#define ODIN3_SRC_MAX_FILE_BYTES (UINT32_C(1) << 28)
#define ODIN3_SRC_MAX_BUFFERS (UINT32_C(1) << 24)

typedef enum odin3_srcbuf_kind {
    ODIN3_SRCBUF_FILE = 1,  /* a file's bytes, or the <command line> buffer */
    ODIN3_SRCBUF_EXPANSION, /* a macro body as spelled in its definition */
    ODIN3_SRCBUF_MACRO_ARG, /* one contiguous spelled run of a substituted argument */
    ODIN3_SRCBUF_SCRATCH    /* pasted or stringified text that exists nowhere */
} odin3_srcbuf_kind;

/*
 * A FILE buffer (spec §3.1): name = path as given, resolved = absolute path, library (strtab IDs;
 * resolved and library 0 for <command line>); parent = loc of the `include that opened it, 0 for
 * a project file; len = bytes. reserved must be zero.
 */
typedef struct odin3_srcfile_spec {
    uint32_t name;
    uint32_t resolved;
    uint32_t library;
    uint32_t parent;
    uint32_t len;
    uint32_t reserved[2];
} odin3_srcfile_spec;

/*
 * An EXPANSION, MACRO_ARG or SCRATCH buffer (spec §3.1 table): kind; name = macro name (strtab;
 * 0 for SCRATCH); parent = the use site (EXPANSION), the formal inside the body (MACRO_ARG) or
 * the operator inside the body (SCRATCH); parent_end = one past it (0 = unknown); def = loc of
 * the first byte as spelled (0 for SCRATCH; offset k <-> def + k, within def's buffer);
 * len = bytes. reserved must be zero.
 */
typedef struct odin3_expansion_spec {
    uint32_t kind;
    uint32_t name;
    uint32_t parent;
    uint32_t parent_end;
    uint32_t def;
    uint32_t len;
    uint32_t reserved[2];
} odin3_expansion_spec;

/* A buffer as stored; resolved, library and lines are 0 except for a FILE. */
typedef struct odin3_srcbuf_info {
    uint32_t kind;
    uint32_t name;
    uint32_t resolved;
    uint32_t library;
    uint32_t parent;
    uint32_t parent_end;
    uint32_t def;
    uint32_t start; /* loc of offset 0 */
    uint32_t len;
    uint32_t lines; /* line count (1 + line starts added) */
} odin3_srcbuf_info;

/*
 * One segment of a stream's map (spec §3.3): output byte out_offset onward is spelled at loc
 * onward until the next segment (loc 0: inserted bytes). stream = the srcbuf ID of the stream's
 * project FILE buffer. reserved must be zero.
 */
typedef struct odin3_segment {
    uint32_t stream;
    uint32_t out_offset;
    uint32_t loc;
    uint32_t reserved[2];
} odin3_segment;

/* A decoded FILE location: buffer ID, 1-based line and column in bytes (Clang's rule). */
typedef struct odin3_srcpos {
    uint32_t buffer;
    uint32_t line;
    uint32_t col;
    uint32_t reserved[2];
} odin3_srcpos;

typedef enum odin3_srcfmt {
    ODIN3_SRCFMT_GIVEN,   /* the path as given */
    ODIN3_SRCFMT_RESOLVED /* the absolute path, else the given name */
} odin3_srcfmt;

typedef enum odin3_srcchain_kind {
    ODIN3_SRCCHAIN_ARG = 1,   /* in argument of macro 'name' at <spelling of the formal> */
    ODIN3_SRCCHAIN_EXPANSION, /* expanded from macro 'name' at <spelling in the body> */
    ODIN3_SRCCHAIN_SCRATCH,   /* pasted by macro 'name' (at = the operator's file location) */
    ODIN3_SRCCHAIN_INCLUDE    /* included from <at> (file:line); name = the included file */
} odin3_srcchain_kind;

typedef struct odin3_srcchain_entry {
    uint32_t kind;
    uint32_t name; /* strtab ID */
    uint32_t at;   /* a FILE loc */
} odin3_srcchain_entry;

typedef void (*odin3_srcchain_visit)(void *user, const odin3_srcchain_entry *entry);

typedef struct odin3_srcman odin3_srcman; /* fields: internals section below */

/* A monotone segment cursor over one stream; plain data, no cleanup. */
typedef struct odin3_srcman_cursor {
    const odin3_srcman *sm;
    uint32_t file; /* index of the stream's file record, 0 = no stream */
    uint32_t seg;  /* index of the current segment */
} odin3_srcman_cursor;

/* --- lifetime ------------------------------------------------------------------------------ */

/*
 * A new, empty source manager whose names live in strtab (borrowed; must outlive it). The
 * design creates its own through odin3_design_get_srcman. NULL on out of memory.
 */
odin3_srcman *odin3_srcman_create(odin3_strtab *strtab);

/* Frees the source manager; NULL is a no-op. */
void odin3_srcman_destroy(odin3_srcman *sm);

/* Bytes the source manager holds (its struct and the reserved capacity of every table). */
size_t odin3_srcman_bytes_reserved(const odin3_srcman *sm);

/* --- building (2B) ------------------------------------------------------------------------- */

/*
 * Adds a FILE buffer (line 1 at offset 0 is implicit) and stores its ID in *out. INVALID_ARG
 * (logged) for a NULL argument, a name that is 0 or not a strtab ID, a resolved or library that
 * is not a strtab ID, a parent that is not a loc, or non-zero reserved fields; PARSE (located at
 * parent) for a file over ODIN3_SRC_MAX_FILE_BYTES, too many buffers or an exhausted location
 * space; NO_MEMORY on out of memory. Nothing changes on failure.
 */
odin3_status odin3_srcman_add_file(odin3_srcman *sm, const odin3_srcfile_spec *spec,
                                   odin3_srcbuf_id *out);

/*
 * Adds an EXPANSION, MACRO_ARG or SCRATCH buffer and stores its ID in *out. INVALID_ARG (logged)
 * for a bad kind, name (required except SCRATCH, which takes 0), a parent that is 0 or not a loc,
 * a parent_end that is not 0 or a loc, a def that is not a loc (0 for SCRATCH) or whose buffer
 * cannot hold len bytes from it, or non-zero reserved fields; PARSE (located at parent) for too
 * many buffers or an exhausted location space; NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_srcman_add_expansion(odin3_srcman *sm, const odin3_expansion_spec *spec,
                                        odin3_srcbuf_id *out);

/*
 * Records that a line starts at offset of FILE buffer buf. Offsets are strictly increasing per
 * buffer, above 0 and at most len; INVALID_ARG (logged) otherwise; NO_MEMORY. Unchanged on
 * failure.
 */
odin3_status odin3_srcman_add_line(odin3_srcman *sm, odin3_srcbuf_id buf, uint32_t offset);

/*
 * Appends a segment to its stream's map. INVALID_ARG (logged) when stream is not a project
 * FILE buffer (parent 0), out_offset does not increase within the stream, loc is neither 0 nor
 * a loc, or reserved fields are non-zero; NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_srcman_add_segment(odin3_srcman *sm, const odin3_segment *seg);

/* --- queries ------------------------------------------------------------------------------- */

/* The loc of offset (≤ len) in buf, 0 for a bad buffer or offset. */
odin3_loc odin3_srcman_loc(const odin3_srcman *sm, odin3_srcbuf_id buf, uint32_t offset);

/* The buffer holding loc, 0 for 0 or a loc past the space allocated so far. */
odin3_srcbuf_id odin3_srcman_buffer_of(const odin3_srcman *sm, odin3_loc loc);

/* Fills *out and returns true for an existing buffer; false (out untouched) otherwise. */
bool odin3_srcman_buffer_info(const odin3_srcman *sm, odin3_srcbuf_id buf, odin3_srcbuf_info *out);

/* A cursor over the segment map of stream (a project FILE buffer ID); any value is accepted. */
void odin3_srcman_cursor_init(odin3_srcman_cursor *cur, const odin3_srcman *sm, uint32_t stream);

/*
 * The loc of output byte out_offset of the cursor's stream: seg.loc + (out_offset -
 * seg.out_offset) of the last segment at or before it, 0 in a loc-0 segment, before the first
 * segment or for no stream. Amortized O(1) moving forward; binary search moving backward.
 */
odin3_loc odin3_srcman_cursor_loc(odin3_srcman_cursor *cur, uint32_t out_offset);

/* --- decoding (spec §3.2); every walk is a loop, 0 in gives 0 out -------------------------- */

/* One spelling step: a FILE loc itself; def + (loc - start) in an EXPANSION or MACRO_ARG; 0 in
 * a SCRATCH. */
odin3_loc odin3_srcman_spelling(const odin3_srcman *sm, odin3_loc loc);

/* Clang's getExpansionLoc: follow parent until a FILE (where the outermost macro was used). */
odin3_loc odin3_srcman_expansion_loc(const odin3_srcman *sm, odin3_loc loc);

/* Clang's getFileLoc: a MACRO_ARG takes a spelling step, an EXPANSION or SCRATCH goes to its
 * parent, until a FILE (an argument token reports its own spelling, a body token the use). */
odin3_loc odin3_srcman_file_loc(const odin3_srcman *sm, odin3_loc loc);

/* An end mapped to its file: parent_end while the buffer is not a FILE (0 when unknown). */
odin3_loc odin3_srcman_expansion_end(const odin3_srcman *sm, odin3_loc end);

/* {expansion_loc(loc), expansion_end(end)} when both lie in one FILE buffer in order, else
 * {expansion_loc(loc), 0}. */
odin3_range odin3_srcman_expansion_range(const odin3_srcman *sm, odin3_range range);

/* {buffer, line, col} of loc's file location; false (pos untouched) when it has none. */
bool odin3_srcman_decode(const odin3_srcman *sm, odin3_loc loc, odin3_srcpos *pos);

/*
 * The provenance location of range (IR-12): loc = range.loc (raw), file = given name of the file
 * location's buffer, line and col of the file location, end_line and end_col of the expansion
 * range's end (0 when the range is not in one file). All 0 except loc when range.loc has no file
 * location.
 */
void odin3_srcman_srcloc(const odin3_srcman *sm, odin3_range range, odin3_srcloc *out);

/* --- printing (spec §3.4) ------------------------------------------------------------------ */

/*
 * Appends "file:line:col" of loc's file location in the given style ("<unknown>:0:0" when it
 * has none). INVALID_ARG for a NULL argument; NO_MEMORY; out unchanged on failure.
 */
odin3_status odin3_srcman_format(const odin3_srcman *sm, odin3_loc loc, odin3_strbuf *out,
                                 odin3_srcfmt style);

/*
 * Calls visit once per chain entry of loc, outward from its own buffer: macro arguments and
 * expansions (and pastes) until a FILE, then one INCLUDE per include level. Never allocates.
 * INVALID_ARG for a NULL sm or visit.
 */
odin3_status odin3_srcman_chain(const odin3_srcman *sm, odin3_loc loc, odin3_srcchain_visit visit,
                                void *user);

/*
 * Appends the chain as lines, each "\n  " + text (no trailing newline). INVALID_ARG for a NULL
 * argument; NO_MEMORY; out unchanged on failure.
 */
odin3_status odin3_srcman_format_chain(const odin3_srcman *sm, odin3_loc loc, odin3_strbuf *out);

/*
 * Logs "file:line:col: error|warning|info|debug: message" plus the chain lines as one odin3_log
 * message (the header alone on out of memory). The design's source manager is created if
 * needed; without one (out of memory) the header reads <unknown>:0:0.
 */
void odin3_diag(odin3_design *design, odin3_log_level level, odin3_loc loc, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* odin3_diag on a source manager with a va_list (the source manager's own located errors). */
void odin3_srcman_vdiag(const odin3_srcman *sm, odin3_log_level level, odin3_loc loc,
                        const char *fmt, va_list args);

/* --- internals shared by src/ast/srcman*.c (other code uses the functions above) ----------- */

/* One buffer; slot 0 of the table is reserved. resolved/library/lines live in the file record. */
typedef struct odin3_srcbuf_rec {
    uint32_t start;
    uint32_t len;
    uint32_t name;
    uint32_t parent;
    uint32_t parent_end;
    uint32_t def;
    uint32_t file; /* FILE: index of its odin3_srcfile_rec (from 1); 0 otherwise */
    uint8_t kind;
} odin3_srcbuf_rec;

/* One segment of a stream map as stored. */
typedef struct odin3_srcseg_rec {
    uint32_t out_offset;
    uint32_t loc;
} odin3_srcseg_rec;

/* Per-FILE data: names, line starts and (for a project file) the stream's segment map. */
typedef struct odin3_srcfile_rec {
    uint32_t resolved;
    uint32_t library;
    odin3_vec lines; /* uint32_t offsets of lines 2.. (line 1 at 0 is implicit) */
    odin3_vec segs;  /* odin3_srcseg_rec, out_offset increasing */
} odin3_srcfile_rec;

struct odin3_srcman {
    odin3_strtab *strtab; /* borrowed: the design's */
    odin3_vec bufs;       /* odin3_srcbuf_rec; slot 0 reserved */
    odin3_vec files;      /* odin3_srcfile_rec; slot 0 reserved */
    uint64_t next;        /* first free loc */
    uint32_t max_buffers; /* ODIN3_SRC_MAX_BUFFERS unless a test lowered it */
    uint32_t space_end;   /* last usable loc */
};

/* The record of the buffer holding loc, NULL when none. */
const odin3_srcbuf_rec *odin3_srcman_rec_of(const odin3_srcman *sm, odin3_loc loc);

/* The file record of a FILE buffer record. */
const odin3_srcfile_rec *odin3_srcman_file_of(const odin3_srcman *sm, const odin3_srcbuf_rec *rec);

#endif
