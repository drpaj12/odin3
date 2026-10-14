/*
 * run.h — one invocation of the abc pass (3D spec §7.1, §7.6, AB-19, AB-22; plan Tasks 14 and 16):
 * the options, the checked script, the engine and the exchange, the run counter, the log, and the
 * module, domain and region being worked on. The tasks after 16 (the splice, provenance) read
 * and extend this struct.
 */
#ifndef ODIN3_PASSES_ABC_RUN_H
#define ODIN3_PASSES_ABC_RUN_H

#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/abc/abc.h"
#include "passes/abc/region.h"
#include "passes/abc/script.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { ODIN3_ABC_DIR_BYTES = 1024, ODIN3_ABC_LAST_LINE_BYTES = 256 };

/* What region.c knows of a cell type, computed once per type and cached in the run. */
enum {
    ODIN3_ABC_TK_OTHER = 0, /* nothing ABC handles: a cut cell */
    ODIN3_ABC_TK_SOP,       /* $sop */
    ODIN3_ABC_TK_LUT,       /* $lut */
    ODIN3_ABC_TK_GATE,      /* a parameterless bit type with a simulate hook (§5.1) */
    ODIN3_ABC_TK_DFF_P,     /* $_DFF_P_ */
    ODIN3_ABC_TK_DFF_N,     /* $_DFF_N_ */
    ODIN3_ABC_TK_FF         /* $_FF_ */
};

typedef struct odin3_abc_gate {
    uint32_t type;     /* cell-type ID */
    uint8_t kind;      /* an ODIN3_ABC_TK_* value */
    uint32_t n_inputs; /* GATE: input bits */
    uint64_t table;    /* GATE: 2^n_inputs bits */
} odin3_abc_gate;

struct odin3_abc_run {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_abc_opts opts;
    odin3_abc_script_choice choice;
    odin3_abc_script script; /* the checked script (odin3_abc_script_free) */
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_route_kind route; /* where the BLIF exchange runs: linked engine or child */
    const odin3_abc_engine *engine;
    uint32_t exchange;             /* an odin3_abc_exchange */
    uint32_t run_no;               /* r: counts from 0 across the invocation */
    FILE *log;                     /* --log, or NULL */
    FILE *runlog;                  /* abc<r>.log while a run is open, or NULL */
    bool streaming;                /* the text callback handles lines as they arrive (child) */
    odin3_arena *arena;            /* per-invocation scratch */
    odin3_vec gates;               /* odin3_abc_gate */
    odin3_u64map *gate_index;      /* cell-type ID -> index into gates */
    char dir[ODIN3_ABC_DIR_BYTES]; /* the exchange directory (absolute) */
    bool dir_is_temp;              /* made by mkdtemp: the pass removes its files */
    bool dir_made;                 /* the directory exists (made or given) */
    odin3_module *module;          /* the current module, domain and region */
    odin3_module_id module_id;
    const odin3_abc_domain *domain;
    odin3_abc_region *region;
    odin3_strbuf lines; /* ABC output lines of the call in progress, '\n'-ended */
    bool lines_overflow;
    uint32_t cmd_class;                        /* the odin3_abc_class of the command in progress */
    char last_line[ODIN3_ABC_LAST_LINE_BYTES]; /* the last non-empty ABC output line */
};

/*
 * One invocation. opts is copied (its strings must outlive the run: they live in the pass's
 * arena). The run owns its arena, vecs and map; the log file and the script are the caller's
 * (odin3_abc_run_destroy closes the log and frees the script). ODIN3_ERR_INVALID_ARG (logged) for
 * a NULL argument; NO_MEMORY.
 */
odin3_status odin3_abc_run_create(odin3_pass_ctx *ctx, const odin3_abc_opts *opts,
                                  odin3_abc_run **out);

/*
 * Start-up (spec §7.1's order, before any IR change or engine call): parses args, then chooses the
 * exchange and the script and checks it, finds the engine, decides the route, checks the views,
 * opens --log and creates a given --via-blif directory, and logs the first INFO line. *out is a
 * run ready for the module loop (run->module and the rest unset); odin3_abc_pass_run is this, the
 * loop, compaction and odin3_abc_run_destroy. Errors as odin3_abc_pass_run's.
 */
odin3_status odin3_abc_run_start(odin3_pass_ctx *ctx, odin3_bytes args, odin3_abc_run **out);

/* Frees everything the run holds, closes its log, and frees *run. NULL is a no-op. */
void odin3_abc_run_destroy(odin3_abc_run *run);

/*
 * The first word cell in module creation order, then node-ID order: `abc: module '<m>' is not in
 * the netlist view: <type> '<cell>'; run lower first` (logged once; ODIN3_ERR_CHECK, nothing
 * changed). OK when every module is in the netlist view.
 */
odin3_status odin3_abc_check_views(const odin3_design *design);

/*
 * The names of the files of run r in run->dir: dir/abc<r>.in.blif (kind 0), .cmd (1), .log (2),
 * .out.blif (3). ODIN3_ERR_INVALID_ARG for a longer path than cap.
 */
enum odin3_abc_file_kind {
    ODIN3_ABC_FILE_IN = 0,
    ODIN3_ABC_FILE_CMD,
    ODIN3_ABC_FILE_LOG,
    ODIN3_ABC_FILE_OUT
};
odin3_status odin3_abc_run_path(const odin3_abc_run *run, uint32_t file_kind, char *path,
                                size_t cap);

/*
 * Makes the exchange directory when it does not exist yet: run->dir when --via-blif named one
 * (created if missing, a single level), else a fresh `odin3-abc-XXXXXX` under $TMPDIR (else /tmp)
 * that the pass removes afterwards. Refused (INVALID_ARG after logging `abc: --via-blif directory
 * '<dir>' contains a control character, a quote, ';' or '#'; ABC cannot read it`) when the
 * absolute path holds a byte below 0x20, 0x7F, '"', '\'', ';' or '#' (I12); `abc: <dir>: cannot
 * create the directory: <strerror>` is IO. Idempotent.
 */
odin3_status odin3_abc_run_ensure_dir(odin3_abc_run *run);

/* path against the working directory, lexically normalised, in arena; NULL on out of memory. */
const char *odin3_abc_resolve_cwd(odin3_arena *arena, const char *path);

#endif
