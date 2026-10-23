/*
 * read_verilog.h — the read_verilog pass's reader (2C spec §4.2 steps 1–7, VP-19): every design
 * file of a read run through the 2B preprocessor and the parser into one parsed store, the -v/-y
 * library files it needs (read_verilog_lib.c), the duplicate_module and unnamed-instance checks,
 * then the sealed and checked store. The pass (step 8) and odin3-parse-check share it.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_READ_VERILOG_H
#define ODIN3_FRONTENDS_VERILOG_READ_VERILOG_H

#include "ast/ast.h"
#include "frontends/project/project.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * One read: the design files in order (borrowed; given, resolved and library names, and the loc
 * that named each), the pass run the store belongs to, NULL or a caller-owned vec of
 * odin3_vp_loc the lexer appends every token's location to (the location oracle, parser.h), and
 * keep_streams (`read_verilog --lib`, 2D-7b): each design file's preprocessed text is kept in the
 * store (odin3_ast_keep_stream) for later reads.
 */
typedef struct odin3_rv_opts {
    const odin3_pp_source *files;
    uint32_t nfiles;
    odin3_passrun_id run;
    odin3_vec *tokens;
    bool keep_streams; /* --lib: keep each design file's preprocessed stream in the store */
} odin3_rv_opts;

/*
 * Steps 1–7 of §4.2 on design: one parsed store for opts->run, one preprocessor configured from
 * the design's finished project record (its defines and include directories; none without a
 * record), one odin3_vl_run across every stream. Each file goes odin3_pp_run → parse; a stream
 * with errors yields no UNIT and the run goes on to the next file until the error cap (20,
 * counted across the files, preprocessor errors included) stops it. Then the record's -v/-y
 * libraries (step 3), duplicate_module over the design units (step 4) and unnamed module
 * instances (step 5), both counted against the cap. On success the store is sealed, checked
 * FULL in Debug builds or under --check, the line "read_verilog: N units (L library), M nodes,
 * K comments, B bytes, T ms" is logged at ODIN3_LOG_INFO and *out receives it (owned by the
 * caller). Failures: ODIN3_ERR_PARSE after the located diagnostics (a file that exists but
 * cannot be read included: its cause is logged); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_CHECK when the
 * store fails the check (a 2C bug, never user input); ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument or no files. *out is NULL on failure and the store is destroyed (the IR is untouched,
 * 2A §9).
 */
/* odin3_read_verilog_files with soft_mask (soft_blocks.h): the primitives built as soft logic by
 * this read; each such module, when no design unit defines it, is read once from its soft body and
 * warned once, at its first instance. vtr_compat: the read's --vtr-compat (odin3_rv.vtr_compat),
 * which also reads a soft body for an instance of such a primitive whose `#()` does not fit the
 * registered hard cell of that name (an `adder #(W)` on the 1-bit cell, #262). */
odin3_status odin3_read_verilog_files_soft(odin3_design *design, const odin3_rv_opts *opts,
                                           uint32_t soft_mask, bool vtr_compat, odin3_ast **out);

odin3_status odin3_read_verilog_files(odin3_design *design, const odin3_rv_opts *opts,
                                      odin3_ast **out);

/* --- internals (read_verilog.c and read_verilog_lib.c only) ---------------------------------- */

/* What a name is known as (odin3_rv.defined values, bits). */
enum { ODIN3_RV_DESIGN = 1, ODIN3_RV_LIBRARY = 2, ODIN3_RV_UDP = 4 };

/* An instantiated module name, the loc of its first instantiation (odin3_rv.wanted) and whether
 * any of its instances' `#()` does not fit the registered cell type of that name (the reader then
 * reads the primitive's soft body under --vtr-compat, #262). */
typedef struct odin3_rv_want {
    uint32_t name;
    odin3_loc at;
    bool overflows_cell;
} odin3_rv_want;

/*
 * One read run: the design, the store being built, the preprocessor, the parser's run context,
 * the finished project record (NULL: none) and the name sets: defined (module or UDP name →
 * ODIN3_RV_* bits), wanted (odin3_rv_want, in order of first instantiation; wanted_set maps a name
 * to the entry's index + 1), libread (resolved paths of the library files read). swept is the first
 * node ID not yet swept for names; bytes the stream bytes parsed; ndesign the design UNITs' count;
 * libfiles_read: every -v file has been read.
 */
typedef struct odin3_rv {
    odin3_design *design;
    odin3_ast *ast;
    odin3_pp *pp;
    odin3_vl_run vl;
    const odin3_project *proj;
    odin3_u64map *defined;
    odin3_vec wanted;
    odin3_u64map *wanted_set;
    odin3_u64map *libread;
    uint32_t swept;
    uint64_t bytes;
    uint32_t ndesign;
    bool libfiles_read;
    uint32_t soft_mask; /* soft_blocks.h: primitives built as soft logic here */
    bool vtr_compat;    /* the read's --vtr-compat: the soft-body fallback of a full hard cell */
    bool keep; /* odin3_rv_read keeps each preprocessed stream in rv->ast (design files of --lib) */
} odin3_rv;

/*
 * Preprocesses and parses one file into rv's store. ODIN3_OK also when the file had errors
 * (logged and counted: a preprocessor error counts as one) or the run is at its error cap (then
 * "too many errors; stopping" is logged once and the run stops); the fatal statuses (NO_MEMORY,
 * CHECK from the preprocessor's Debug check, INVALID_ARG) stop the run and are returned.
 */
odin3_status odin3_rv_read(odin3_rv *rv, const odin3_pp_source *src);

/* Parses one kept stream (odin3_ast_stream_at of a --lib store) into rv's store; the same
 * statuses as odin3_rv_read. */
odin3_status odin3_rv_parse_kept(odin3_rv *rv, const odin3_ast_stream *kept);

/*
 * Step 3 (DESIGN §4.0; read_verilog_lib.c): records the module and UDP names defined and the
 * module names instantiated by the design UNITs; while some instantiated name is undefined,
 * reads every -v file of the record at once (the first time), else looks for
 * <dir>/<name><ext> over the record's libdirs in order and its libext in order (default ".v"),
 * reading the first regular file found, and repeats (a library file may instantiate further
 * modules). Library UNITs follow every design UNIT; odin3_ast_set_library_units_from records
 * where they begin. A library module or UDP whose name a design unit or an earlier library
 * definition already has stays in the store with an info line. What stays undefined is left to
 * 2D. Before the -v/-y files, the files of every
 * earlier `read_verilog --lib` read (2D-7b: the carrier stores the design holds) are read again
 * as library UNITs, each carrying its carrier's ODIN3_UNIT_* flags (odin3_ast_mark_units); a file
 * already read is skipped. Returns OK, or a fatal status (odin3_rv_read; NO_MEMORY).
 */
odin3_status odin3_rv_resolve_libraries(odin3_rv *rv);

#endif
