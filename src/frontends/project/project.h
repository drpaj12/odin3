/*
 * project.h — the project record (PRJ-1..4): the C mirror of the corpus's normalized record,
 * owned by the design, its builders, located errors and the JSON dump.
 */
#ifndef ODIN3_FRONTENDS_PROJECT_PROJECT_H
#define ODIN3_FRONTENDS_PROJECT_PROJECT_H

#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- enumerations (stored as uint8_t in the record) ---------------------------------------- */

typedef enum odin3_lang {
    ODIN3_LANG_VERILOG = 1,
    ODIN3_LANG_SYSTEMVERILOG,
    ODIN3_LANG_VHDL,
    ODIN3_LANG_BLIF,
    ODIN3_LANG_VQM,
    ODIN3_LANG_EDIF
} odin3_lang;

typedef enum odin3_proj_format {
    ODIN3_PROJ_FORMAT_O3PROJ = 1,
    ODIN3_PROJ_FORMAT_FLIST,
    ODIN3_PROJ_FORMAT_QUARTUS,
    ODIN3_PROJ_FORMAT_ODIN2
} odin3_proj_format;

typedef enum odin3_proj_arch_kind {
    ODIN3_PROJ_ARCH_O3LIB = 1,
    ODIN3_PROJ_ARCH_VPR_XML,
    ODIN3_PROJ_ARCH_DEVICE
} odin3_proj_arch_kind;

typedef enum odin3_proj_rule_kind {
    ODIN3_PROJ_RULE_MAP = 1,
    ODIN3_PROJ_RULE_SPLIT,
    ODIN3_PROJ_RULE_LIMIT,
    ODIN3_PROJ_RULE_PATTERNS
} odin3_proj_rule_kind;

typedef enum odin3_proj_action {
    ODIN3_PROJ_ACTION_TO = 1,
    ODIN3_PROJ_ACTION_SOFT,
    ODIN3_PROJ_ACTION_KEEP
} odin3_proj_action;

/* split depth: none, min, max, or a count (depth = strtab ID of the digits as written). */
typedef enum odin3_proj_depth {
    ODIN3_PROJ_DEPTH_NONE,
    ODIN3_PROJ_DEPTH_MIN,
    ODIN3_PROJ_DEPTH_MAX,
    ODIN3_PROJ_DEPTH_N
} odin3_proj_depth;

typedef enum odin3_proj_override_kind {
    ODIN3_PROJ_OVERRIDE_HIDE = 1,
    ODIN3_PROJ_OVERRIDE_CELL
} odin3_proj_override_kind;

typedef enum odin3_param_type { ODIN3_PARAM_NUMBER = 1, ODIN3_PARAM_STRING } odin3_param_type;

/* map thresholds given (odin3_proj_rule.u.map.has bits). */
enum { ODIN3_PROJ_HAS_MIN_WIDTH = 1, ODIN3_PROJ_HAS_MAX_WIDTH = 2, ODIN3_PROJ_HAS_MIN_DEPTH = 4 };

/* --- caps (§9) ----------------------------------------------------------------------------- */

#define ODIN3_PROJ_MAX_FLIST_DEPTH UINT32_C(64)
#define ODIN3_PROJ_MAX_QIP_DEPTH UINT32_C(64)
#define ODIN3_PROJ_MAX_WORDS (UINT32_C(1) << 24)

/* The caps a project enforces; tests lower them through project_test.h. */
typedef struct odin3_proj_limits {
    uint32_t flist_depth; /* nested -f/-F lists */
    uint32_t qip_depth;   /* nested .qip files */
    uint32_t words;       /* words per file list */
} odin3_proj_limits;

/* --- the record (PRJ-3) -------------------------------------------------------------------- */

/* A path: the text as written, the absolute normalized path (strtab IDs) and the naming word. */
typedef struct odin3_proj_path {
    uint32_t given, resolved;
    odin3_loc at;
} odin3_proj_path;
typedef struct odin3_proj_file {
    odin3_proj_path path;
    uint8_t lang;
    uint32_t library;
} odin3_proj_file;
typedef struct odin3_proj_define {
    uint32_t name, value;
    bool has_value;
    odin3_loc at;
} odin3_proj_define; /* `X=` has_value with value "" (ID 0) */
typedef struct odin3_proj_param {
    uint32_t scope, name, value;
    uint8_t type;
    odin3_loc at;
} odin3_proj_param; /* type: ODIN3_PARAM_NUMBER | _STRING */
typedef struct odin3_proj_arch {
    uint8_t kind;
    odin3_proj_path path;
    uint32_t family, device;
    bool has_family, has_device;
} odin3_proj_arch;               /* kind: O3LIB | VPR_XML (path) | DEVICE (family, device) */
typedef struct odin3_proj_rule { /* stored only; Phase 4 interprets */
    uint8_t kind;                /* MAP | SPLIT | LIMIT | PATTERNS */
    odin3_loc at;
    union {
        struct {
            uint32_t scope, subject;
            uint8_t action;         /* TO | SOFT | KEEP */
            uint32_t cells, ncells; /* span in proj->rule_cells */
            uint8_t has;            /* ODIN3_PROJ_HAS_* bit per threshold given */
            uint32_t min_width, max_width, min_depth;
        } map;
        struct {
            uint32_t subject;
            bool width;
            uint8_t depth_kind; /* odin3_proj_depth */
            uint32_t depth;     /* DEPTH_N: strtab ID of the digits as written */
        } split;
        struct {
            uint32_t cell, count;
        } limit;
        struct {
            odin3_proj_path path;
        } patterns;
    } u;
} odin3_proj_rule;
typedef struct odin3_proj_inventory {
    uint32_t cell, count;
    odin3_loc at;
} odin3_proj_inventory;
typedef struct odin3_proj_override {
    uint8_t kind; /* HIDE (name = the cell) | CELL (name from from, params) */
    uint32_t name, from;
    uint32_t params, nparams; /* span in proj->override_params */
    odin3_loc at;
} odin3_proj_override;
typedef struct odin3_proj_kv {
    uint32_t key, value;
} odin3_proj_kv; /* one override parameter */

typedef struct odin3_project {
    odin3_design *design;
    odin3_proj_path entry;
    uint8_t format;    /* O3PROJ | FLIST | QUARTUS | ODIN2 */
    uint32_t revision; /* --revision (may be "") */
    bool has_revision;
    odin3_vec files, libfiles;  /* odin3_proj_file; libfiles: library 0 */
    odin3_vec libdirs, incdirs; /* odin3_proj_path */
    odin3_vec libext;           /* uint32_t; empty = ".v" at lookup */
    odin3_vec defines, params, arch, rules, rule_cells, inventory, overrides, override_params;
    uint32_t top; /* "mod" or "lib.mod" as given; 0 = auto */
    odin3_loc top_at;
    odin3_proj_path flow; /* given 0 = none ("" is never a word) */
    struct {
        bool present;
        uint32_t path, format; /* path: normalized absolute; format: lower-cased */
        bool has_format;
    } output;                      /* Odin II */
    odin3_vec ignored, duplicates; /* uint32_t strtab IDs, sorted at finish */
    bool finished;
    /* internals (project.c) */
    odin3_proj_path top_file; /* a file-level top's file (set_top_file); given 0 = none */
    odin3_arena *arena;       /* quoted words' unescaped text */
    odin3_u64map *file_keys;  /* resolved << 32 | library of every files entry */
    odin3_u64map *lib_keys;   /* resolved of every libfiles entry */
    odin3_proj_limits limits;
} odin3_project;

/* --- lifetime (the design calls these; others use odin3_design_get_project) ---------------- */

/* A new, empty record of design (borrowed). NULL on out of memory. */
odin3_project *odin3_project_create(odin3_design *design);

/* Frees the record; NULL is a no-op. */
void odin3_project_destroy(odin3_project *proj);

/* --- errors (§8) --------------------------------------------------------------------------- */

typedef enum odin3_proj_err_kind {
    ODIN3_PROJ_ERR_MISSING_FILE = 1,
    ODIN3_PROJ_ERR_UNKNOWN_KEY,
    ODIN3_PROJ_ERR_UNKNOWN_OPTION,
    ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE,
    ODIN3_PROJ_ERR_UNSUPPORTED_INPUT_TYPE,
    ODIN3_PROJ_ERR_SYNTAX,
    ODIN3_PROJ_ERR_UNDEFINED_VARIABLE,
    ODIN3_PROJ_ERR_F_CYCLE,
    ODIN3_PROJ_ERR_QIP_CYCLE,
    ODIN3_PROJ_ERR_AMBIGUOUS_REVISION,
    ODIN3_PROJ_ERR_UNKNOWN_REVISION,
    ODIN3_PROJ_ERR_INCLUDE_NOT_FOUND,
    ODIN3_PROJ_ERR_INCLUDE_CYCLE
} odin3_proj_err_kind;

/* The DESIGN §4.0 name of kind ("missing_file", …); "?" for an unknown value. */
const char *odin3_proj_err_kind_name(odin3_proj_err_kind kind);

typedef enum odin3_proj_scope {
    ODIN3_PROJ_LOCATED = 1, /* "file:line:col: error: …" at `at`, with the include chain */
    ODIN3_PROJ_FILE,        /* "<file>: error: …", the file named in the design's style */
    ODIN3_PROJ_UNLOCATED    /* "odin3: error: …" */
} odin3_proj_scope;

typedef struct odin3_proj_err {
    odin3_proj_err_kind kind;
    odin3_proj_scope scope;
    odin3_loc at;                /* LOCATED */
    const odin3_proj_path *file; /* FILE */
    const char *detail;          /* NULL: the names alone */
    const odin3_bytes *names;    /* nnames entries, each printed 'quoted' (point (g)) */
    uint32_t nnames;
} odin3_proj_err;

/*
 * Emits the error once — "<kind>: <detail>: 'n1', 'n2'" (": <names>" only when there are names;
 * the detail alone when there are none; the names alone when detail is NULL) — through
 * odin3_diag (LOCATED), or odin3_log with "<file>: error: " (FILE; given or resolved path per
 * odin3_design_diag_paths) or "odin3: error: " (UNLOCATED). Allocates nothing (the text is cut at
 * ODIN3_LOG_BUF like any log message). Returns ODIN3_ERR_PARSE.
 */
odin3_status odin3_project_fail(odin3_project *proj, const odin3_proj_err *err);

/* --- project files (PRJ-5) ----------------------------------------------------------------- */

/* A project file read whole and registered as a FILE buffer. */
typedef struct odin3_proj_text {
    uint8_t *bytes; /* owned */
    uint32_t len;
    odin3_srcbuf_id buffer;
    odin3_vec lines; /* uint32_t offset of each line's first byte; a final empty line is none */
} odin3_proj_text;

/*
 * Reads file->resolved whole (ODIN3_SRC_MAX_FILE_BYTES) and registers it as a FILE buffer
 * (name = file->given, resolved, library 0, parent) with every line start; lines are split at
 * '\n' only. IO (logged "cannot read '<file>': <reason>" at parent, unlocated when parent is 0,
 * the file named in the design's diagnostic-path style) when it cannot be read; PARSE (logged,
 * `syntax` "file '<file>' is N bytes, over the limit of M") over the size cap, or from the source
 * manager;
 * NO_MEMORY. *out is empty on failure; free it with odin3_proj_text_free in every case.
 */
odin3_status odin3_project_load(odin3_project *proj, const odin3_proj_path *file, odin3_loc parent,
                                odin3_proj_text *out);

/* Line index (0-based) of text without its '\n' and the '\r' before it; *offset = its first
 * byte's offset in the file. A lone '\r' is an ordinary byte. */
odin3_bytes odin3_proj_text_line(const odin3_proj_text *text, uint32_t index, uint32_t *offset);

void odin3_proj_text_free(odin3_proj_text *text);

/* --- paths (PRJ-6) ------------------------------------------------------------------------- */

typedef enum odin3_proj_want {
    ODIN3_PROJ_WANT_FILE = 1, /* a regular file (a symlink to one counts) */
    ODIN3_PROJ_WANT_DIR,      /* a directory */
    ODIN3_PROJ_WANT_ANY       /* either */
} odin3_proj_want;

typedef struct odin3_proj_path_req {
    odin3_bytes base;  /* the directory relative paths resolve against */
    odin3_bytes given; /* the path as written */
    odin3_loc at;      /* the naming word */
    odin3_proj_want want;
} odin3_proj_path_req;

/*
 * Resolves req->given against req->base lexically (odin3_path_resolve), checks that it exists
 * with the wanted kind and interns both forms into *out. Otherwise `missing_file` at req->at,
 * names [the resolved path] (PARSE). NO_MEMORY.
 */
odin3_status odin3_project_path(odin3_project *proj, const odin3_proj_path_req *req,
                                odin3_proj_path *out);

/* --- builders (each logs its own errors through odin3_project_fail) ------------------------ */
/* A word holding a NUL byte is rejected before anything is interned: `syntax` "NUL byte in
 * word", names [word], located at the builder's at (unlocated through odin3_project_intern for
 * builders without one). No strtab string holds NUL (PRJ-4); the oracle would keep the byte. */

typedef enum odin3_proj_dir_list { ODIN3_PROJ_LIBDIRS = 1, ODIN3_PROJ_INCDIRS } odin3_proj_dir_list;

/* A span of strtab IDs (map cells). */
typedef struct odin3_proj_ids {
    const uint32_t *ids;
    uint32_t n;
} odin3_proj_ids;

/* A span of override parameters. */
typedef struct odin3_proj_kvs {
    const odin3_proj_kv *kvs;
    uint32_t n;
} odin3_proj_kvs;

/* Interns bytes in the design's strtab: a NUL byte is an unlocated `syntax` (PARSE, logged);
 * NO_MEMORY. */
odin3_status odin3_project_intern(odin3_project *proj, odin3_bytes bytes, uint32_t *id);

/* Appends file to files, or, when the same resolved path is already listed in the same library,
 * adds that path to duplicates instead (PRJ-8). NO_MEMORY. */
odin3_status odin3_project_add_file(odin3_project *proj, const odin3_proj_file *file);

/* The same for libfiles, keyed by the resolved path alone. */
odin3_status odin3_project_add_libfile(odin3_project *proj, const odin3_proj_file *file);

/* Appends a directory to libdirs or incdirs. NO_MEMORY. */
odin3_status odin3_project_add_dir(odin3_project *proj, odin3_proj_dir_list list,
                                   const odin3_proj_path *path);

/* Appends a library extension after checking it names a language (§4.1: "x" + ext has a known
 * suffix), else `unknown_file_type` at at, names [ext]. */
odin3_status odin3_project_add_libext(odin3_project *proj, odin3_bytes ext, odin3_loc at);

/* Appends a define from `NAME[=value]` (value = everything after the first '='; `X=` has an empty
 * value, `X` none). A name that is not an IDENT is `syntax` at at, names [word]. */
odin3_status odin3_project_add_define(odin3_project *proj, odin3_bytes word, odin3_loc at);

/*
 * Sets the top (§4.4): name must be IDENT or IDENT.IDENT (`syntax` "bad top name", names
 * [name]); a second top is `syntax` "top given twice (first a): 'b'", names [the second top]
 * only, as the oracle. set_top_file is the same with
 * a file-level location (a Quartus default top): its errors print "<file>: error: …" and top_at
 * stays 0 with top_file set.
 */
odin3_status odin3_project_set_top(odin3_project *proj, odin3_bytes name, odin3_loc at);
odin3_status odin3_project_set_top_file(odin3_project *proj, odin3_bytes name,
                                        const odin3_proj_path *file);

/* Parses a count (ASCII digits, at most UINT32_MAX): `syntax` "expected a count" or "count too
 * large" at at, names [text]. */
odin3_status odin3_project_count(odin3_project *proj, odin3_bytes text, odin3_loc at,
                                 uint32_t *out);

/* The length of the IDENT ([A-Za-z_][A-Za-z0-9_$]*) starting at text[from], 0 when none. */
size_t odin3_proj_ident_len(odin3_bytes text, size_t from);

/* Whether word is wholly the oracle's NUMBER: -?[0-9][0-9_]* or
 * ([0-9][0-9_]*)?'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ_?]+ (ASCII digits only). Shared by the .o3proj
 * `param` and the Quartus `set_parameter` value typing. */
bool odin3_proj_is_number(odin3_bytes word);

/* Plain appends (NO_MEMORY). add_rule copies cells into rule_cells for a MAP rule and sets its
 * span; add_override copies params into override_params for a CELL override and sets its span. */
odin3_status odin3_project_add_param(odin3_project *proj, const odin3_proj_param *param);
odin3_status odin3_project_add_arch(odin3_project *proj, const odin3_proj_arch *arch);
odin3_status odin3_project_add_rule(odin3_project *proj, const odin3_proj_rule *rule,
                                    odin3_proj_ids cells);
odin3_status odin3_project_add_inventory(odin3_project *proj, const odin3_proj_inventory *inv);
odin3_status odin3_project_add_override(odin3_project *proj, const odin3_proj_override *ov,
                                        odin3_proj_kvs params);

/* Adds name to the ignored set (PRJ-8; sorted and made unique at finish). NO_MEMORY. */
odin3_status odin3_project_add_ignored(odin3_project *proj, odin3_bytes name);

/* Sorts ignored and duplicates by bytes, drops repeats, and marks the record finished. NO_MEMORY
 * (not finished; the two sets hold the same names). */
odin3_status odin3_project_finish(odin3_project *proj);

/* --- the dump (PRJ-4) ---------------------------------------------------------------------- */

/*
 * Appends the record as JSON: "{\n", one `  "key": value` line per README key in README order
 * (files libfiles libdirs libext incdirs defines top params arch rules inventory overrides flow
 * output ignored duplicates), each array or object on one line with Python's ", " and ": "
 * separators, "}\n". Paths are resolved; top, flow, output and a bare define's value are null when
 * absent. Strings are UTF-8 with JSON escapes for '"', '\\' and control bytes; a byte that is not
 * part of valid UTF-8 is written as \u00XX. NO_MEMORY (out unchanged).
 */
odin3_status odin3_project_dump(const odin3_project *proj, odin3_strbuf *out);

/* The record's names for languages ("verilog", …), NULL for an unknown value. */
const char *odin3_lang_name(odin3_lang lang);

/* --- readers (Tasks 3–5, 14) --------------------------------------------------------------- */

/* A Quartus read's wanted revision: from --revision (at 0: unlocated errors) or `import …
 * revision r` (at = the import word). */
typedef struct odin3_quartus_req {
    odin3_bytes revision;
    bool has_revision;
    odin3_loc at;
} odin3_quartus_req;

/* Reads a -f/-F file list (PRJ-11). */
odin3_status odin3_read_flist(odin3_project *proj, const odin3_proj_path *file);

/* Reads a .qpf or .qsf project (PRJ-12..14). */
odin3_status odin3_read_quartus(odin3_project *proj, const odin3_proj_path *file,
                                const odin3_quartus_req *req);

/* Reads an .o3proj file (PRJ-10). */
odin3_status odin3_read_o3proj(odin3_project *proj, const odin3_proj_path *file);

/* Reads an Odin II XML configuration (PRJ-15). */
odin3_status odin3_read_odin2(odin3_project *proj, const odin3_proj_path *file);

/* --- entry dispatch (PRJ-16; entry.c) ------------------------------------------------------ */

/* What read_project reads. */
typedef struct odin3_project_request {
    const char *entry;    /* as given; a relative entry resolves against the working directory */
    uint8_t format;       /* odin3_proj_format; 0: from the entry's suffix */
    const char *revision; /* --revision; NULL: none ("" is a revision) */
    const char *top;      /* 1D's --top; NULL: none */
} odin3_project_request;

/*
 * Fills the design's project record from req (PRJ-16). The entry is made absolute against the
 * working directory and normalized (point (i)); it must be a regular file (unlocated
 * `missing_file`, names [entry as given]). Format: req->format, else by the entry's suffix
 * (case-sensitive): .o3proj, .qpf/.qsf (Quartus), .qip (`unknown_file_type`: not an entry), .xml
 * (`unknown_file_type` "no Odin II XML reader yet", as is format ODIN2), anything else a file
 * list. A revision with a non-Quartus entry is an unlocated `syntax` ("--revision needs a .qpf or
 * .qsf entry"). Then the reader runs; then req->top replaces the project's top (an info line
 * "<entry>: --top <new> replaces the project top <old>" when they differ; a bad name is an
 * unlocated `syntax`); then finish and the info lines "<entry>: ignored: A, B, …" and "<entry>:
 * listed more than once, read once: P, …" (resolved paths), each only when non-empty; <entry> is
 * the entry as given. Errors: PARSE (logged once), IO, NO_MEMORY — each drops the record
 * (odin3_design_drop_project); INVALID_ARG for a NULL req or entry, or when the record is already
 * finished (logged "odin3: error: project already read from <entry>"; that record is kept).
 */
odin3_status odin3_project_read(odin3_design *design, const odin3_project_request *req);

#endif
