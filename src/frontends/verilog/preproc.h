/*
 * preproc.h — the Verilog-2005 preprocessor (2B PP-1..12): one per read_verilog run, a narrow
 * config, whole-file streams with their segment maps in the design's source manager.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_PREPROC_H
#define ODIN3_FRONTENDS_VERILOG_PREPROC_H

#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

/* --- caps (spec §9); each is a located ODIN3_ERR_PARSE naming the cap ---------------------- */

#define ODIN3_PP_MAX_INCLUDE_DEPTH UINT32_C(64)
#define ODIN3_PP_MAX_EXPANSION_CHAIN UINT32_C(1024)
#define ODIN3_PP_MAX_EXPANSION_DEPTH UINT32_C(1024)
#define ODIN3_PP_MAX_LIVE_RUNS (UINT32_C(1) << 20)
#define ODIN3_PP_MAX_COND_DEPTH UINT32_C(1024)
#define ODIN3_PP_MAX_MACRO_ARGS UINT32_C(256)
#define ODIN3_PP_MAX_MACRO_TEXT (UINT32_C(1) << 20)
#define ODIN3_PP_MAX_MACROS (UINT32_C(1) << 20)
#define ODIN3_PP_MAX_STREAM_BYTES (UINT32_C(1) << 28)
#define ODIN3_PP_MAX_BUFFERS_PER_STREAM (UINT32_C(1) << 22)
#define ODIN3_PP_MAX_INCLUDES_PER_STREAM (UINT32_C(1) << 16)
#define ODIN3_PP_MAX_LOC_BYTES_PER_STREAM (UINT32_C(1) << 28)
#define ODIN3_PP_MAX_FILE_CACHE_BYTES (UINT32_C(1) << 30)

/* --- configuration (spec §7.1, verbatim) --------------------------------------------------- */

/* One project define: `define NAME, or `define NAME value when has_value (value verbatim). */
typedef struct odin3_pp_define {
    odin3_bytes name, value;
    bool has_value;
} odin3_pp_define;

/* The narrow config 2C/2D fill from the project record; borrowed by odin3_pp_create only. */
typedef struct odin3_pp_config {
    const odin3_pp_define *defines;
    uint32_t ndefines; /* project order */
    const char *const *incdirs;
    uint32_t nincdirs; /* resolved, in order; the include search rule itself is fixed: PP-11 */
} odin3_pp_config;

/*
 * One source file to preprocess (plan point (a)): given and resolved names and its library
 * (strtab IDs) for the stream's FILE buffer (parent 0); at = the loc of the project word or
 * instantiation that named it, which locates the root file's own errors (missing, unreadable,
 * over ODIN3_SRC_MAX_FILE_BYTES); at 0 reports them unlocated ("odin3: error: ...").
 */
typedef struct odin3_pp_source {
    uint32_t given, resolved, library;
    odin3_loc at;
} odin3_pp_source;

/* A preprocessed stream: the text (owned by the preprocessor until its next run or destroy),
 * the stream's FILE buffer and the number of segments its map received. */
typedef struct odin3_pp_stream {
    odin3_bytes text;
    odin3_srcbuf_id stream;
    uint32_t nsegments;
} odin3_pp_stream;

typedef struct odin3_pp odin3_pp; /* fields: preproc_internal.h */

/*
 * A new preprocessor for design. Writes the <command line> FILE buffer (one LF-terminated line
 * per define, `define NAME or `define NAME value, in order) and defines every macro from it with
 * continuation and comment recognition off. Copies the include directories. Returns ODIN3_OK
 * with *out; ODIN3_ERR_PARSE after one located error for a bad define ("<command line>:3:9:
 * error: syntax: ..."); ODIN3_ERR_INVALID_ARG for a NULL argument; ODIN3_ERR_NO_MEMORY. *out is
 * NULL on failure (buffers already added stay in the source manager, harmlessly).
 */
odin3_status odin3_pp_create(odin3_design *design, const odin3_pp_config *config, odin3_pp **out);

/*
 * Preprocesses one source file and everything it includes into one stream (spec §7.8): the
 * text, the segment map on the source manager and the buffers and lines. Macros persist from
 * run to run. Returns ODIN3_OK; ODIN3_ERR_PARSE after one located error; ODIN3_ERR_IO for a root
 * file that exists but cannot be read; ODIN3_ERR_NO_MEMORY (the run is abandoned, nothing
 * logged); ODIN3_ERR_CHECK when the Debug-build odin3_pp_check fails; ODIN3_ERR_INVALID_ARG for
 * a NULL argument. *out is zeroed on failure.
 */
odin3_status odin3_pp_run(odin3_pp *pp, const odin3_pp_source *src, odin3_pp_stream *out);

/* The resolved paths (strtab IDs) of every file read so far, in first-read order; the array is
 * owned by pp and valid until its next run or destroy. */
uint32_t odin3_pp_files_read(const odin3_pp *pp, const uint32_t **paths);

/* Frees the macro table, the file cache and the text; NULL is a no-op. Buffers and segments
 * live on in the source manager. */
void odin3_pp_destroy(odin3_pp *pp);

/*
 * Checks the last stream against spec §7.8 (segments, locs, bytes, loc-0 blanks, per-buffer
 * reachability); ODIN3_ERR_CHECK (logged) on a violation. Debug builds run it after every run.
 */
odin3_status odin3_pp_check(const odin3_pp *pp, odin3_srcbuf_id stream);

#endif
