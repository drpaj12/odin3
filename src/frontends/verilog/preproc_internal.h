/*
 * preproc_internal.h — the whole odin3_pp state and the hooks shared by the preprocessor files.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_PREPROC_INTERNAL_H
#define ODIN3_FRONTENDS_VERILOG_PREPROC_INTERNAL_H

#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* --- records ------------------------------------------------------------------------------- */

/* A run (PP-2): len bytes that never move (file cache or arena) spelled at loc onward; loc 0
 * for inserted bytes. Frames are run lists and never copy text. */
typedef struct odin3_pp_textrun {
    const uint8_t *bytes;
    uint32_t len;
    odin3_loc loc;
} odin3_pp_textrun;

/* A position in a frame: run index relative to the frame's first run, byte offset in it. */
typedef struct odin3_pp_cursor {
    uint32_t run;
    uint32_t offset;
} odin3_pp_cursor;

typedef enum odin3_pp_frame_kind {
    ODIN3_PP_FRAME_FILE = 1, /* a file: one run over the cached bytes */
    ODIN3_PP_FRAME_EXPANSION /* a macro use: body and argument runs */
} odin3_pp_frame_kind;

/* One input frame; its runs are pp->runs[first_run, first_run + nruns). */
typedef struct odin3_pp_frame {
    uint32_t kind;
    uint32_t first_run;
    uint32_t nruns;
    odin3_pp_cursor cur;
    odin3_srcbuf_id buffer; /* FILE: its FILE buffer; EXPANSION: its EXPANSION buffer */
    uint32_t macro;         /* EXPANSION: index of the macro in pp->macros */
    uint32_t resolved;      /* FILE: resolved path (strtab) */
    uint32_t library;       /* FILE: library (strtab) */
    uint32_t cond_base;     /* FILE: index of its first entry in pp->conds */
    odin3_loc use_loc;      /* EXPANSION: loc of the use's backtick */
} odin3_pp_frame;

/* One open conditional (PP-3/PP-4). */
typedef struct odin3_pp_cond {
    odin3_loc loc;  /* the opening `ifdef/`ifndef's backtick */
    bool taken;     /* the current branch is emitted */
    bool seen_true; /* a branch was taken, or the frame was pushed while skipping */
    bool seen_else;
} odin3_pp_cond;

/* One formal of a macro: name (strtab) and its default text, copied into the arena, spelled at
 * [def, def + len) in the defining buffer. */
typedef struct odin3_pp_formal {
    uint32_t name;
    bool has_default;
    odin3_loc def;
    uint32_t len;
    const uint8_t *text;
} odin3_pp_formal;

/* One macro definition (spec §7.3 row `define). */
typedef struct odin3_pp_macro {
    uint32_t name;            /* strtab ID */
    odin3_loc at;             /* the `define's backtick */
    odin3_loc def;            /* the body's first byte (empty: just after the name or ')') */
    uint32_t len;             /* body bytes as spelled, continuations included */
    const uint8_t *body;      /* the spelled body, copied into the arena */
    odin3_pp_formal *formals; /* arena array of nformals */
    uint32_t nformals;
    bool has_formals; /* '(' followed the name (`define F() has formals and nformals 0) */
} odin3_pp_macro;

/* One cached file: bytes owned by the cache (odin3_util_*), keyed by resolved path. Entry 0 is
 * the <command line> text (resolved 0, bytes in the arena). */
typedef struct odin3_pp_cached {
    uint32_t resolved;
    uint32_t len;
    uint8_t *bytes;
} odin3_pp_cached;

/* Per buffer created in the current run (index = buffer ID - pp->first_buffer). */
typedef struct odin3_pp_bufrec {
    /* PP-7: 0 for a FILE; 1 + that of its def's buffer for a MACRO_ARG of an actual, or of its
     * parent's for an EXPANSION and for a MACRO_ARG of a default (from_default) */
    uint32_t chain;
    uint32_t reach;    /* spec §7.8: non-zero iff some emitted byte's walk passes the buffer */
    bool from_default; /* a MACRO_ARG substituting a formal's default: macro text, whose
                          ancestry is the EXPANSION it is substituted into (PP-7) */
} odin3_pp_bufrec;

/* A buffer to create through odin3_pp_add_buffer: a FILE (file filled) or another kind. */
typedef struct odin3_pp_bufspec {
    uint32_t kind; /* odin3_srcbuf_kind */
    odin3_srcfile_spec file;
    odin3_expansion_spec expansion;
    bool from_default; /* a MACRO_ARG substituting a formal's default */
} odin3_pp_bufspec;

/* The macro arena's chunk size: ODIN3_PP_MAX_MACRO_BYTES leaves one chunk of headroom. */
enum { ODIN3_PP_ARENA_CHUNK = 1 << 16 };

struct odin3_pp {
    odin3_design *design;
    odin3_srcman *sm;
    odin3_strtab *strtab;
    odin3_arena *arena; /* macro bodies, formals, defaults, include dirs */
    odin3_pp_limits limits;
    const char **incdirs; /* arena copies */
    uint32_t nincdirs;

    /* macro table (lives for the preprocessor) */
    odin3_vec macros; /* odin3_pp_macro; a redefinition appends (ODIN3_PP_MAX_DEFINITIONS) */
    odin3_u64map *macro_by_name; /* name strtab ID -> index in macros */

    /* file cache (lives for the preprocessor) */
    odin3_vec cache;               /* odin3_pp_cached; entry 0 = <command line> */
    odin3_u64map *cache_by_path;   /* resolved strtab ID -> cache index */
    odin3_u64map *cache_by_buffer; /* FILE buffer ID -> cache index (every FILE buffer made) */
    uint64_t cache_bytes;          /* bytes held, for ODIN3_PP_MAX_FILE_CACHE_BYTES */
    odin3_vec files_read;          /* uint32_t resolved IDs, first-read order */
    odin3_srcbuf_id cmdline_buf;   /* the <command line> FILE buffer */

    /* the current run */
    odin3_strbuf out;             /* the stream text */
    odin3_srcbuf_id stream;       /* the stream's FILE buffer */
    odin3_srcbuf_id first_buffer; /* the first buffer the run made (the stream's) */
    odin3_vec bufs;               /* odin3_pp_bufrec per buffer made in the run */
    odin3_vec frames;             /* odin3_pp_frame, the input stack */
    odin3_vec runs;               /* odin3_pp_textrun of every live frame */
    odin3_vec conds;              /* odin3_pp_cond of every live FILE frame, see cond_base */
    odin3_vec incstack;   /* uint32_t resolved paths: the stream's file, then each `include */
    odin3_vec args;       /* odin3_pp_textrun: runs of the actuals being collected (PP-5) */
    odin3_vec arg_starts; /* uint32_t: index in args where each actual starts */
    odin3_vec scratch;    /* odin3_pp_textrun: a frame's runs being built */
    uint32_t nsegments;
    odin3_loc last_loc;   /* loc of the last output byte (0 when inserted or none) */
    uint32_t nbuffers;    /* buffers made in the run */
    uint32_t nincludes;   /* FILE buffers made in the run (the stream's included) */
    uint32_t nexpansions; /* live EXPANSION frames */
    uint64_t loc_bytes;   /* location space reserved by the run (sum of len + 1) */
    odin3_loc xoff_loc;   /* the open translate_off comment (PP-10), 0 none */
    bool skipping;        /* inside a false conditional branch (PP-4) */
    bool gap;             /* a directive, skipped region or include end was consumed: PP-3 space */
};

/* --- shared helpers (preproc.c) ------------------------------------------------------------ */

/* The PP-9 error naming op ("syntax: '<op>' is a SystemVerilog macro operator; ..."). */
odin3_status odin3_pp_sv_operator(odin3_pp *pp, odin3_loc at, const char *op);

/* Logs "loc: error: msg" through odin3_diag (loc 0: "odin3: error: msg") and returns
 * ODIN3_ERR_PARSE. */
odin3_status odin3_pp_error(odin3_pp *pp, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* The located cap error "syntax: <what> exceeds <cap> (<value>)"; ODIN3_ERR_PARSE. */
typedef struct odin3_pp_cap {
    const char *what; /* e.g. "macro text" */
    const char *name; /* e.g. "ODIN3_PP_MAX_MACRO_TEXT" */
    uint32_t value;
} odin3_pp_cap;
odin3_status odin3_pp_cap_error(odin3_pp *pp, odin3_loc loc, odin3_pp_cap cap);

/* The top frame (NULL when the stack is empty) and its current run (NULL at the frame's end). */
odin3_pp_frame *odin3_pp_top(odin3_pp *pp);
const odin3_pp_textrun *odin3_pp_cur_run(const odin3_pp *pp);

/* The byte ahead bytes after the top frame's cursor (across its runs), -1 past the frame's end. */
int odin3_pp_peek(const odin3_pp *pp, uint32_t ahead);

/* loc + n, keeping 0 (an inserted run) at 0. */
static inline odin3_loc odin3_pp_loc_add(odin3_loc loc, uint32_t n) {
    return loc.v == 0 ? loc : (odin3_loc){loc.v + n};
}

/* The loc of the top frame's current byte (0 in an inserted run); at the frame's end, one past
 * the last byte of its last run, or 0 when that run is inserted (or the frame has no runs). It
 * locates errors only: an EXPANSION's parent_end must be the loc of the last consumed byte + 1
 * (AST-2), which this is not at a run boundary. */
odin3_loc odin3_pp_loc_here(const odin3_pp *pp);

/* Moves the top frame's cursor n bytes forward (stopping at the frame's end). */
void odin3_pp_advance(odin3_pp *pp, uint32_t n);

/* Emits the next n bytes of the top frame (or skips them while skipping or in translate_off)
 * and advances past them. */
odin3_status odin3_pp_emit_next(odin3_pp *pp, uint32_t n);

/* Appends len bytes spelled at loc onward to the stream (loc 0: inserted, each byte its own
 * segment), applying the segment rule (spec §7.2), the PP-3 protective space and the
 * ODIN3_PP_MAX_STREAM_BYTES cap (located at at). */
typedef struct odin3_pp_text {
    const uint8_t *bytes;
    uint32_t len;
    odin3_loc loc;
} odin3_pp_text;
odin3_status odin3_pp_emit(odin3_pp *pp, odin3_pp_text text);

/* Counts a blank for the protective space and actual trimming: space, tab, CR, LF, VT, FF. */
bool odin3_pp_is_blank(int ch);
bool odin3_pp_is_ident_start(int ch);
bool odin3_pp_is_ident_char(int ch);

/* Reads an IDENT at the top frame's cursor within its current run (empty when none) and
 * advances past it. */
odin3_bytes odin3_pp_read_ident(odin3_pp *pp);

/* The IDENT starting ahead bytes after the top frame's cursor, within its run (empty when
 * none). */
odin3_bytes odin3_pp_ident_ahead(const odin3_pp *pp, uint32_t ahead);

/* Skips spaces and tabs at the top frame's cursor. */
void odin3_pp_skip_spaces(odin3_pp *pp);

/* Lexical lengths at the top frame's cursor, shared by the scanner and argument collection:
 * a string literal (to the closing quote, or up to an unescaped newline); an escaped
 * identifier (the backslash and every byte to the next blank); a // comment (to the newline,
 * which it excludes) or a block comment (through its close, or to the frame's end). */
uint32_t odin3_pp_string_length(const odin3_pp *pp);
uint32_t odin3_pp_escaped_length(const odin3_pp *pp);
uint32_t odin3_pp_comment_length(const odin3_pp *pp);

/* The plain bytes at the cursor up to the next byte the scanner treats specially (a backtick,
 * '"', '/', '\\'; with brackets also ( ) [ ] { } and ','), within the current run, at least
 * one. */
uint32_t odin3_pp_plain_length(const odin3_pp *pp, bool brackets);

/*
 * Creates a source-manager buffer for the run after checking ODIN3_PP_MAX_BUFFERS_PER_STREAM,
 * ODIN3_PP_MAX_INCLUDES_PER_STREAM (a FILE) and ODIN3_PP_MAX_LOC_BYTES_PER_STREAM (located at
 * at); records its chain length (odin3_pp_bufrec) and stores the ID in *out.
 */
odin3_status odin3_pp_add_buffer(odin3_pp *pp, const odin3_pp_bufspec *spec, odin3_loc at,
                                 odin3_srcbuf_id *out);

/* The chain length of the buffer holding loc (0 for a FILE or a buffer of an earlier run). */
uint32_t odin3_pp_chain_of(const odin3_pp *pp, odin3_loc loc);

/* The record of the run's buffer holding loc, NULL for a buffer of an earlier run or none. */
const odin3_pp_bufrec *odin3_pp_bufrec_of(const odin3_pp *pp, odin3_loc loc);

/* The cached bytes of FILE buffer buf; false when it is not a FILE buffer this pp made. */
bool odin3_pp_file_bytes(const odin3_pp *pp, odin3_srcbuf_id buf, odin3_bytes *out);

/* Loads resolved (strtab) through the cache (ODIN3_PP_MAX_FILE_CACHE_BYTES checked first;
 * missing, unreadable and too-big files located at at as below) and stores its cache index. */
typedef struct odin3_pp_load {
    uint32_t given;    /* the name as written, for messages */
    uint32_t resolved; /* the absolute path */
    odin3_loc at;      /* the loc that named it, 0 for unlocated */
} odin3_pp_load;
odin3_status odin3_pp_cache_load(odin3_pp *pp, const odin3_pp_load *load, uint32_t *index);

/* Registers a FILE buffer for cache entry index (lines included) and pushes its file frame. */
typedef struct odin3_pp_open {
    uint32_t cache;   /* cache index */
    uint32_t given;   /* name (strtab) */
    uint32_t library; /* library (strtab) */
    odin3_loc parent; /* the `include's backtick, 0 for the stream's file */
    odin3_loc at;     /* locates the cap errors: parent, or the stream source's at */
} odin3_pp_open;
odin3_status odin3_pp_open_file(odin3_pp *pp, const odin3_pp_open *open, odin3_srcbuf_id *out);

/* Pushes a frame whose runs are the n runs given (copied as records into pp->runs; the bytes are
 * not copied), checking ODIN3_PP_MAX_LIVE_RUNS (located at at). */
odin3_status odin3_pp_push_frame(odin3_pp *pp, const odin3_pp_frame *frame,
                                 const odin3_pp_textrun *runs, odin3_loc at);

/* Pops the top frame and its runs. */
void odin3_pp_pop_frame(odin3_pp *pp);

/* --- macro table and definition scanner (macro.c) ------------------------------------------ */

/* Index + 1 of the macro named name, 0 when undefined. */
uint32_t odin3_pp_macro_find(const odin3_pp *pp, odin3_bytes name);

/* True for a compiler directive name (spec §7.3: never a macro name). */
bool odin3_pp_is_directive_name(odin3_bytes name);

/* A text a definition is scanned from: bytes spelled at base onward, starting at pos. */
typedef struct odin3_pp_deftext {
    const uint8_t *bytes;
    uint32_t len;
    odin3_loc base;     /* loc of bytes[0] */
    uint32_t pos;       /* in: just after `define; out: the end of the definition */
    odin3_loc at;       /* the `define's backtick */
    bool continuations; /* \-newline continues (off on the command line) */
    bool comments;      /* // ends the text, / * * / is kept (off on the command line) */
} odin3_pp_deftext;

/* Scans one definition (spec §7.3) and enters it in the table; pos ends at the newline that
 * ends its logical line (or the text's end). */
odin3_status odin3_pp_define_scan(odin3_pp *pp, odin3_pp_deftext *text);

/* The `define and `undef directives at the top FILE frame's cursor (just after the name);
 * at = the backtick. */
odin3_status odin3_pp_define_directive(odin3_pp *pp, odin3_loc at);
odin3_status odin3_pp_undef_directive(odin3_pp *pp, odin3_loc at);

/* --- hooks: conditionals and translate_off (cond.c) ---------------------------------------- */

typedef enum odin3_pp_cond_kind {
    ODIN3_PP_COND_IFDEF = 1,
    ODIN3_PP_COND_IFNDEF,
    ODIN3_PP_COND_ELSIF,
    ODIN3_PP_COND_ELSE,
    ODIN3_PP_COND_ENDIF
} odin3_pp_cond_kind;

/* The compiler directives the scanner knows (preproc.c's table). */
typedef enum odin3_pp_dir_kind {
    ODIN3_PP_DIR_NONE,
    ODIN3_PP_DIR_DEFINE,
    ODIN3_PP_DIR_UNDEF,
    ODIN3_PP_DIR_COND,
    ODIN3_PP_DIR_INCLUDE,
    ODIN3_PP_DIR_LINE,
    ODIN3_PP_DIR_PASS,
    ODIN3_PP_DIR_SV
} odin3_pp_dir_kind;

typedef struct odin3_pp_dir {
    const char *name;
    odin3_pp_dir_kind kind;
    odin3_pp_cond_kind cond; /* ODIN3_PP_DIR_COND only */
} odin3_pp_dir;

/* The directive named name, NULL for a macro name. */
const odin3_pp_dir *odin3_pp_dir_lookup(odin3_bytes name);

/* A conditional directive whose backtick is at at; the cursor is just after its name. Called
 * also while skipping. Maintains pp->conds and pp->skipping. */
odin3_status odin3_pp_cond_directive(odin3_pp *pp, odin3_pp_cond_kind kind, odin3_loc at);

/* A comment about to be scanned (outside skipped regions): bytes and len when it lies in one
 * run (NULL otherwise), loc of its first byte. Sets *drop when the comment leaves the stream
 * (translate_off/on, PP-10) and maintains pp->xoff_loc. */
typedef struct odin3_pp_comment {
    const uint8_t *bytes;
    uint32_t len;
    odin3_loc loc;
} odin3_pp_comment;
odin3_status odin3_pp_cond_comment(odin3_pp *pp, const odin3_pp_comment *comment, bool *drop);

/* The top FILE frame has ended: its conditionals must be closed (and, for the stream's file,
 * translate_off). */
odin3_status odin3_pp_cond_file_end(odin3_pp *pp);

/* --- hooks: include (include.c) ------------------------------------------------------------ */

/* `include with its backtick at at; the cursor is just after the name. */
odin3_status odin3_pp_include_directive(odin3_pp *pp, odin3_loc at);

/* An included file's frame is about to be popped (the stream's own file never calls this). */
odin3_status odin3_pp_include_end(odin3_pp *pp);

/* --- hooks: macro uses (expand.c) ---------------------------------------------------------- */

/* A macro use `NAME (not a directive) with its backtick at at; the cursor is just after the
 * name; macro = odin3_pp_macro_find of the name (0 undefined). */
typedef struct odin3_pp_use {
    odin3_bytes name;
    uint32_t macro;
    odin3_loc at;
} odin3_pp_use;
odin3_status odin3_pp_expand_use(odin3_pp *pp, const odin3_pp_use *use);

/* --- hooks: checking (pp_check.c) ---------------------------------------------------------- */

/* Called for every emitted text with a non-zero loc (reachability counting, spec §7.8). */
void odin3_pp_check_note(odin3_pp *pp, odin3_loc loc);

#endif
