/*
 * odin3.h — the public C ABI of Odin III.
 *
 * This is the only header that plugins (.so), the CLI, and the Python binding
 * (plugins/python/odin3.py, via cffi) may include. Everything else in src/ is
 * private. Spec: docs/DESIGN.md §15.3, docs/specs/2026-10-09-1D-abi-design.md.
 *
 * ABI v0 (Phase 0 skeleton): version queries, status codes, and plugin loading.
 * ABI v1 (Phase 1, 1B): adds ODIN3_ERR_CHECK.
 * ABI v2 (Phase 1, 1C): adds ODIN3_ERR_PARSE.
 * ABI v3 (Phase 1, 1D): design handles, passes and pass scripts, ID-based access to modules,
 * nodes, pins, nets and wires, string attributes, cell-type and pass registration (plugins),
 * provenance visitors, logging.
 * ABI v4 (Phase 2, 2A): the source manager and AST stores (kinds, sub-kinds, flags, caps, read
 * and build functions, keep levels); odin3_source gains the raw source-manager loc.
 * ABI v4 (cont., 2D): $proc processes and process counts, value kind PROC, pass processes slot,
 * library units, odin3_ast_elaborate and odin3_elab_options.
 * ABI v4 (cont., 3D): pass-run arguments and run information; odin3_pass_set_abc_script;
 * ODIN3_ERR_ENGINE and the ABC engine (odin3_abc_engine_register).
 * ABI v4 (cont., 2F): odin3_pass_set_opt_snapshot.
 * ABI v4 (cont., 7.4b): odin3_design_copy.
 * ABI v5 (v1.0.2 gate mapping): ABC engine load_genlib and get_cells, odin3_abc_cells,
 *   ODIN3_ABC_ENGINE_VERSION 2.
 * ABI v5 (cont., v1.0.3 S1): simulations (odin3_simulation_*): open a top, find signals, set
 *   inputs, step, read values, reset, close; passes are refused while a simulation is open.
 * ABI v6 (v1.1.0 SystemVerilog): odin3_sv_reader_register, the seam the read_verilog pass drives
 *   for a .sv file through the slang adapter (adapters/slang/odin3_slang.h, DESIGN §4.2).
 *
 * The block between ODIN3_CDEF_BEGIN and ODIN3_CDEF_END is read verbatim by the
 * Python binding as a cffi cdef: keep it free of preprocessor directives and
 * attributes.
 *
 * Stack: Odin III never recurses over a design, but constant evaluation during elaboration
 * (read_verilog) nests in C up to 256 levels (a parameter whose value reads a later parameter,
 * a function's return range; ODIN3_ELAB_MAX_REENTRY_DEPTH, 2D spec v4.6). Call the library from a
 * thread with at least 1 MB of stack in a release build and 4 MB under the sanitizers (measured:
 * 256 levels need under 1 MB at -O3, about 2 MB under gcc ASan and 4 MB under clang ASan with
 * -fsanitize=fuzzer-no-link). The main thread's 8 MB default is enough; a secondary thread's
 * default may not be (512 KB on macOS): give it the size explicitly.
 */
#ifndef ODIN3_ODIN3_H
#define ODIN3_ODIN3_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN3_VERSION_MAJOR 1
#define ODIN3_VERSION_MINOR 1
#define ODIN3_VERSION_PATCH 0

/* ODIN3_CDEF_BEGIN */

/* Incremented on every incompatible change to this header. */
enum { ODIN3_ABI_VERSION = 6 };

/* Result of every fallible Odin III function. ODIN3_OK is always 0. */
typedef enum odin3_status {
    ODIN3_OK = 0,
    ODIN3_ERR_INVALID_ARG = 1,
    ODIN3_ERR_NO_MEMORY = 2,
    ODIN3_ERR_IO = 3,
    ODIN3_ERR_PLUGIN = 4,
    ODIN3_ERR_ABI_MISMATCH = 5,
    ODIN3_ERR_CHECK = 6,  /* the IR violates an invariant (docs/IR.md section 9) */
    ODIN3_ERR_PARSE = 7,  /* malformed input file (logged as file:line: message) */
    ODIN3_ERR_ENGINE = 8, /* the ABC engine failed or is unavailable (3D) */
    ODIN3_STATUS_COUNT = 9
} odin3_status;

/*
 * Returns the library version as "MAJOR.MINOR.PATCH".
 * The string is static; the caller must not free it. Never fails.
 */
const char *odin3_version_string(void);

/*
 * Returns ODIN3_ABI_VERSION as compiled into the library, so a plugin or
 * binding built against one header can detect a library built from another.
 * Never fails.
 */
uint32_t odin3_abi_version(void);

/*
 * Returns a short human-readable name for a status, e.g. "ODIN3_ERR_IO".
 * The string is static; the caller must not free it. Values outside the enum
 * return "ODIN3_STATUS_UNKNOWN". Never fails.
 */
const char *odin3_status_string(odin3_status status);

/*
 * Entry point every shared-object plugin must export under the name
 * "odin3_plugin_init". The host passes its ODIN3_ABI_VERSION; a plugin built
 * for a different ABI returns ODIN3_ERR_ABI_MISMATCH. It registers its passes
 * (odin3_pass_register) and cell types (odin3_celltype_register) from here.
 */
typedef odin3_status (*odin3_plugin_init_fn)(uint32_t host_abi_version);

/*
 * Loads the shared object at `path` and calls its odin3_plugin_init.
 * Returns ODIN3_ERR_INVALID_ARG if `path` is NULL, ODIN3_ERR_IO if the file
 * cannot be loaded, ODIN3_ERR_PLUGIN if it does not export odin3_plugin_init
 * (the object is unloaded then), or whatever status the plugin's init returns.
 * Once init has been called the plugin stays loaded for the life of the
 * process, even when init fails: registrations are not undone, so whatever it
 * registered before failing (passes, cell types) remains in effect and
 * usable. Nothing is returned for the caller to free.
 */
odin3_status odin3_plugin_load(const char *path);

/* --- conventions of the IR access functions below ------------------------------------------ *
 *
 * Handles and IDs. A design is an opaque odin3_design *. Every IR object is named by plain
 * uint32_t IDs (docs/IR.md IR-5): a module by its design-global module ID (1, 2, …), a node, pin,
 * net or wire by its module's ID plus its module-local ID, bundled in an odin3_ref. ID 0 is
 * "none" and never names an object. IDs stay valid until a `compact` pass renumbers the module
 * (module IDs never change).
 *
 * Iteration. Module-local IDs run from 1 to one before the store's end (odin3_module_get_*_end),
 * in creation order; deleted ("dead") objects keep their ID until compact, so skip them with
 * odin3_*_is_live. Accessors also answer for a dead object (what it held when it died), with one
 * exception: the text of a dead $proc node's PROC parameter (odin3_node_get_param_text) is refused.
 *
 * Failures. Every function below that takes a design (but odin3_design_destroy) returns
 * odin3_status. ODIN3_ERR_INVALID_ARG, logged as "<function>: invalid argument …", for a NULL
 * design, name or output pointer, a module ID that is not a module of the design, an object ID
 * that is 0 or past its store's end, or an index out of range; the outputs are left unchanged on
 * any failure. (A pass or script that runs logs its own errors.) ODIN3_ERR_NO_MEMORY where a
 * function says it can allocate. Readers take a const design and never change the IR.
 *
 * Strings. A const char * returned through an output is owned by the design: never free it. It
 * is NUL-terminated and stays valid until the next IR mutation of the design (any pass run or
 * script, odin3_attr_set_string, odin3_design_set_top_module) or the design's destruction,
 * whichever comes first; fetch it again after a mutation. "" stands for "no name".
 * ------------------------------------------------------------------------------------------- */

/* A design: modules, cell types, names, provenance. Opaque; create and destroy it here. */
typedef struct odin3_design odin3_design;

/* A module-local object (node, pin, net or wire): its module's ID and its own ID. */
typedef struct odin3_ref {
    uint32_t module;
    uint32_t id;
} odin3_ref;

/* Kinds of objects that carry names and attributes. */
typedef enum odin3_objkind {
    ODIN3_OBJ_NODE,
    ODIN3_OBJ_NET,
    ODIN3_OBJ_WIRE,
    ODIN3_OBJ_MODULE
} odin3_objkind;

/* An object of any kind: module ID, kind (an odin3_objkind value; uint32_t fixes the layout) and
 * module-local ID (ignored for ODIN3_OBJ_MODULE). */
typedef struct odin3_obj {
    uint32_t module;
    uint32_t kind;
    uint32_t id;
} odin3_obj;

/* A run of consecutive pin IDs, first .. first + count - 1; count 0 means empty (first 0). */
typedef struct odin3_span {
    uint32_t first;
    uint32_t count;
} odin3_span;

/* Direction of a port as seen from the cell: an IN pin reads its net, an OUT pin drives it, an
 * INOUT pin does both (so a $port_in's pin is OUT: it drives the module's net). */
typedef enum odin3_dir { ODIN3_DIR_IN, ODIN3_DIR_OUT, ODIN3_DIR_INOUT } odin3_dir;

/* What a cell type is (granularity tags, docs/IR.md IR-9): word-level, bit-level, hard block,
 * black box, a module of the design, or a module port node. */
typedef enum odin3_granularity {
    ODIN3_GRAN_WORD,
    ODIN3_GRAN_BIT,
    ODIN3_GRAN_HARD,
    ODIN3_GRAN_BLACKBOX,
    ODIN3_GRAN_MODULE,
    ODIN3_GRAN_PORT
} odin3_granularity;

/*
 * Kind of a parameter value (IR-10): integer, 4-state bit vector, string, SOP cover, and (2D) a
 * process body: the BODY parameter of a built-in $proc node (docs/IR.md IR-19), an opaque blob
 * only the core reads (odin3_node_get_param_text gives its text). A plugin cell type cannot
 * declare a PROC parameter.
 */
typedef enum odin3_value_kind {
    ODIN3_VAL_INT,
    ODIN3_VAL_BITS,
    ODIN3_VAL_STRING,
    ODIN3_VAL_COVER,
    ODIN3_VAL_PROC
} odin3_value_kind;

/* --- design -------------------------------------------------------------------------------- */

/*
 * Creates an empty design whose cell-type table holds every cell type registered so far.
 * The caller owns it and frees it with odin3_design_destroy. Returns NULL on out of memory.
 */
odin3_design *odin3_design_create(void);

/*
 * Frees the design and everything it owns (every string it handed out). NULL is a no-op. Close
 * every simulation of the design first (odin3_simulation_close): a simulation still open when the
 * design is destroyed is a caller error (a debug build asserts).
 */
void odin3_design_destroy(odin3_design *design);

/*
 * Makes *out a deep copy of src: every module with its nodes, pins, nets, wires, attributes and
 * provenance records, the design's strings and cell types, the source manager, the top module and
 * the pass-run table. Every ID (module, node, pin, net, wire, provenance record, string, cell type)
 * is the same in the copy, so an ID read from one design names the same object in the other, and
 * a pass on the copy never touches src; the copy may outlive src. Shared, not copied: the
 * cell-type arena (tech-library data, width expressions) and the architecture record, both
 * immutable once registered and reference counted, so a read_techlib or read_arch design copies
 * and hardmap, partial_map and the writers work on the copy. Not copied: held AST stores (passes
 * that need the AST see none; provenance resolves through the copied source manager), the project
 * record (defaults apply on the copy) and the change journals (off in the copy). src must not be
 * mutated during the call. A black-box or tech-library read on the copy allocates in the shared
 * cell-type arena, which is not thread safe: do not do that on two designs of one family at once.
 *
 * The caller owns *out and frees it with odin3_design_destroy. *out is NULL on failure. Failures:
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument; ODIN3_ERR_NO_MEMORY on out of memory, with
 * nothing left allocated. src is not changed.
 */
odin3_status odin3_design_copy(const odin3_design *src, odin3_design **out);

/*
 * Runs the pass named name on the design, with args as its argument text (NULL or "" for none;
 * words split on blanks, "double quotes" keep blanks), e.g. ("read_blif", "a.blif"). The pass
 * manager opens a provenance run named after the pass, checks the IR before and after (Debug
 * builds, or odin3_pass_set_check), and logs its time. Returns the pass's own failure status
 * (logged); ODIN3_ERR_CHECK when a check around it fails; ODIN3_ERR_INVALID_ARG for a NULL design
 * or name (as the conventions say), or (logged by the pass manager) an unknown pass, bad
 * arguments or an unclosed quote; ODIN3_ERR_NO_MEMORY.
 * Mutates the IR (see Strings above). Nothing is returned for the caller to free. After a pass
 * fails, the design stays valid but may no longer compute what its input did (a pass can stop in
 * the middle of a rewrite), so the caller must discard it (odin3_design_destroy).
 * While any simulation of the design is open (odin3_simulation_open) every pass is refused with
 * ODIN3_ERR_INVALID_ARG (logged "pass <name>: the design has <n> open simulation(s); close them
 * first (odin3_simulation_close)"); a refused call leaves the design untouched.
 */
odin3_status odin3_design_run_pass(odin3_design *design, const char *name, const char *args);

/*
 * *module gets the design's top module (DESIGN §4.0; set by read_blif, hierarchy or
 * odin3_design_set_top_module), 0 when none is set. Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say.
 */
odin3_status odin3_design_get_top_module(const odin3_design *design, uint32_t *module);

/*
 * Makes module the design's top module. Counts as an IR mutation. Fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say. The top is unchanged on failure.
 */
odin3_status odin3_design_set_top_module(odin3_design *design, uint32_t module);

/*
 * *count gets the number of modules. Modules are never deleted, so their IDs are 1 .. count.
 * Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_design_get_module_count(const odin3_design *design, uint32_t *count);

/*
 * *module gets the ID of module `index` (0-based, creation order). ODIN3_ERR_INVALID_ARG (logged)
 * for an index >= the module count, besides the conventions.
 */
odin3_status odin3_design_get_module_at(const odin3_design *design, uint32_t index,
                                        uint32_t *module);

/*
 * *module gets the ID of the module named name, 0 when there is none (black boxes and other
 * cell types are not modules). ODIN3_ERR_INVALID_ARG (logged) for a NULL name. O(modules).
 */
odin3_status odin3_design_lookup_module(const odin3_design *design, const char *name,
                                        uint32_t *module);

/*
 * True when a read_project pass has filled the design's project record (2B PRJ-1; the record
 * itself leaves the process only through the dump_project pass). False for a NULL design, before
 * any read_project and after a failed one. Never fails.
 */
bool odin3_design_has_project(const odin3_design *design);

/* --- passes and pass scripts --------------------------------------------------------------- */

/* Number of registered passes (built-ins first, then plugin passes in registration order).
 * Never fails. */
uint32_t odin3_pass_get_count(void);

/*
 * *name gets the name of pass `index` (registry order). The string lives as long as the process;
 * never free it. ODIN3_ERR_INVALID_ARG (logged) for an index >= odin3_pass_get_count() or a NULL
 * name.
 */
odin3_status odin3_pass_get_name(uint32_t index, const char **name);

/*
 * *help gets the one-line usage text of pass `index` (what `odin3 --help` prints); it lives as
 * long as the process. ODIN3_ERR_INVALID_ARG (logged) for an index >= odin3_pass_get_count() or a
 * NULL help.
 */
odin3_status odin3_pass_get_help(uint32_t index, const char **help);

/*
 * Turns the IR check before and after every pass on or off in Release builds (Debug builds always
 * check; CLAUDE.md rule 3). Process-wide; off by default. Never fails.
 */
void odin3_pass_set_check(bool check);

/*
 * Sets the requested top module name for later passes (the CLI's --top; DESIGN §4.0: it wins over
 * a file's own top): read_blif applies it after reading and `hierarchy` uses it when given no
 * --top. NULL clears it. The string is copied. Process-wide. To set a design's actual top, see
 * odin3_design_set_top_module. Fails only with ODIN3_ERR_NO_MEMORY (the old name is kept).
 */
odin3_status odin3_pass_set_top_name(const char *name);

/*
 * Sets the requested Quartus revision for later read_project passes (the CLI's --revision; 2B
 * PRJ-16), so the flag reaches a read_project inside a script; a read_project's own --revision
 * wins. "" is a revision; NULL clears it. The string is copied. Process-wide. A revision with an
 * entry that is not a .qpf/.qsf makes that read_project fail. Fails only with ODIN3_ERR_NO_MEMORY
 * (the old revision is kept).
 */
odin3_status odin3_pass_set_revision(const char *revision);

/*
 * Sets the ABC script path of later passes (the CLI's --abc-script; 3D AB-20, AB-27), as given:
 * read_project resolves it against the working directory and replaces the project's abc_script
 * with it (an info line when they differ; a path that is not a regular file makes that
 * read_project fail with missing_file), and the abc pass runs it when no project was read; an
 * `abc --script` argument wins for its run. NULL clears it. The string is copied. Process-wide.
 * Fails only with ODIN3_ERR_NO_MEMORY (the old path is kept).
 */
odin3_status odin3_pass_set_abc_script(const char *path);

/*
 * Makes every later opt-family sub-pass call that changes the design write its design as RTLIL
 * text before and after into dir (the CLI's --opt-snapshot; 2F OP-16), as
 * <dir>/<NNNN>-<sub>-before.il and <dir>/<NNNN>-<sub>-after.il: NNNN is the call sequence (four
 * digits from 0001, counted from this call) and <sub> the sub-pass (opt_expr, opt_merge, ...).
 * dir must exist; it is copied. NULL turns snapshots off. Process-wide. Every call restarts the
 * sequence at 0001. A snapshot that cannot be written (a cell with no RTLIL form, an I/O error)
 * logs WARN "opt: snapshot skipped: <reason>" and the sub-pass call goes on with its own result.
 * Fails only with ODIN3_ERR_NO_MEMORY (the old directory and sequence are kept).
 */
odin3_status odin3_pass_set_opt_snapshot(const char *dir);

/*
 * How later passes' located diagnostics name files (the CLI's --diag-paths; 2B PRJ-18): false
 * (the default) as the path was given, true as its absolute resolved path. Applied to the design
 * before each pass runs. Process-wide. Never fails.
 */
void odin3_pass_set_diag_paths(bool resolved);

/* How a script's errors are located: "origin:line: …" (a file) or "origin: command k: …". */
typedef enum odin3_script_loc { ODIN3_SCRIPT_BY_LINE, ODIN3_SCRIPT_BY_COMMAND } odin3_script_loc;

/* Where a script comes from: origin is its path, or e.g. "-p" for an inline script. */
typedef struct odin3_script_src {
    const char *origin;
    odin3_script_loc loc;
} odin3_script_src;

/*
 * Runs a pass script (NUL-terminated text) on the design: commands separated by ';' or newlines,
 * '#' comments to the end of the line, "double quotes" keep blanks, ';' and '#'. Every pass name
 * is looked up before anything runs (an unknown one is "<loc>: unknown pass '<name>'",
 * ODIN3_ERR_PARSE); then the commands run in order through the pass manager until one fails, whose
 * status is returned ("<loc>: pass '<name>' failed: <status>" logged). ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL design, text or src.origin; ODIN3_ERR_NO_MEMORY. Mutates the IR. After a
 * failed pass the design must be discarded, as for odin3_design_run_pass. A design whose
 * `read_verilog` failed holds valid but incomplete modules and is fit only for destruction (2D:
 * the IR has no module-delete primitive, and the top is not set). While any simulation of the
 * design is open (odin3_simulation_open) a script with a command is refused before it runs
 * anything, with ODIN3_ERR_INVALID_ARG (logged as for odin3_design_run_pass); a refused call
 * leaves the design untouched.
 */
odin3_status odin3_design_run_script(odin3_design *design, const char *text, odin3_script_src src);

/*
 * Reads the script file at path and runs it as odin3_design_run_script (located by line, origin
 * path). ODIN3_ERR_IO (logged as "path: cannot read …") when it cannot be read.
 */
odin3_status odin3_design_run_script_file(odin3_design *design, const char *path);

/*
 * Splits a script and looks up every pass name, running nothing (same errors as
 * odin3_design_run_script, without a design), so a typo anywhere fails before any pass runs.
 */
odin3_status odin3_script_resolve(const char *text, odin3_script_src src);

/* As odin3_script_resolve for the script file at path (ODIN3_ERR_IO when it cannot be read). */
odin3_status odin3_script_resolve_file(const char *path);

/* --- plugin passes ------------------------------------------------------------------------- */

/*
 * The body of a plugin pass. design is the design the pass runs on; args is the text after the
 * pass name in the script, trimmed, NUL-terminated and valid during the call only ("" for none;
 * double quotes are kept as written); user is odin3_plugin_pass.user. The pass may call every
 * reader function and odin3_attr_set_string on design. It returns ODIN3_OK or a failure status,
 * logging why (odin3_log_write); ODIN3_ERR_INVALID_ARG for bad arguments.
 */
typedef odin3_status (*odin3_pass_run_fn)(odin3_design *design, const char *args, void *user);

/*
 * Whether a pass may run on a design that still has processes ($proc nodes, before `proc`; 2D
 * EL-33). REFUSE (the default) makes the pass manager fail the pass without running it, logging
 * "pass '<name>': module '<m>' has processes; run proc first" (ODIN3_ERR_INVALID_ARG). OK lets it
 * run; such a pass must then treat every $proc node as opaque (no constant propagation or CSE
 * through it; its O pins drive unknown values, its I pins read).
 */
enum { ODIN3_PASS_PROCESSES_REFUSE = 0, ODIN3_PASS_PROCESSES_OK = 1 };

/*
 * A pass as a plugin describes it. name: non-empty printable ASCII without blanks, ';', '#' or '"'
 * (a script word); help: its one-line usage, shown by `odin3 --help`; run: its body; user:
 * passed to run as is. processes: NULL (ODIN3_PASS_PROCESSES_REFUSE, what a plugin built before
 * this slot had a meaning passes) or the address of a uint8_t holding ODIN3_PASS_PROCESSES_REFUSE
 * or _OK (an enumeration constant has no address), read once at registration (the plugin need not
 * keep the byte). reserved: must be all NULL (later ABI versions give the slots a meaning). The
 * layout is the one of four reserved slots: processes was the first.
 */
typedef struct odin3_plugin_pass {
    const char *name;
    const char *help;
    odin3_pass_run_fn run;
    void *user;
    const uint8_t *processes;
    void *reserved[3];
} odin3_plugin_pass;

/*
 * Registers a pass (from odin3_plugin_init, typically), after the built-ins and every pass
 * registered before it; scripts and odin3_design_run_pass then run it like a built-in: in its own
 * provenance run, with the IR checked before and after (Debug builds, or odin3_pass_set_check),
 * timed and logged. name and help are copied; user must stay valid while the pass can run.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL pass, name, help or run, a bad name, a name already
 * registered (built-in or not), a processes value above ODIN3_PASS_PROCESSES_OK or a reserved
 * slot that is not NULL; ODIN3_ERR_NO_MEMORY. Nothing is registered on failure. A pass whose
 * processes is NULL or REFUSE is refused on a design with processes (see
 * ODIN3_PASS_PROCESSES_REFUSE). Process-wide; not thread-safe; a pass cannot be unregistered.
 */
odin3_status odin3_pass_register(const odin3_plugin_pass *pass);

/* --- modules ------------------------------------------------------------------------------- */

/* *name gets the module's name (see Strings). Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say. */
odin3_status odin3_module_get_name(const odin3_design *design, uint32_t module, const char **name);

/*
 * *end gets one past the last node, net or wire ID of the module: iterate IDs 1 .. end - 1 and
 * skip dead objects (odin3_node_is_live, …). Each fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say.
 */
odin3_status odin3_module_get_node_end(const odin3_design *design, uint32_t module, uint32_t *end);
odin3_status odin3_module_get_net_end(const odin3_design *design, uint32_t module, uint32_t *end);
odin3_status odin3_module_get_wire_end(const odin3_design *design, uint32_t module, uint32_t *end);

/*
 * *count gets the number of live nodes, nets or wires of the module (port nodes and port wires
 * included; what the `stats` pass prints). O(store size). Each fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_module_get_node_count(const odin3_design *design, uint32_t module,
                                         uint32_t *count);
odin3_status odin3_module_get_net_count(const odin3_design *design, uint32_t module,
                                        uint32_t *count);
odin3_status odin3_module_get_wire_count(const odin3_design *design, uint32_t module,
                                         uint32_t *count);

/*
 * *count gets the number of live process nodes ($proc, before the `proc` pass lowers them; 2D
 * EL-33) of the module. O(1): the IR keeps the count. A module with a nonzero count makes every
 * pass registered as ODIN3_PASS_PROCESSES_REFUSE fail on the design. Nothing is returned for the
 * caller to free. Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_module_get_proc_count(const odin3_design *design, uint32_t module,
                                         uint32_t *count);

/* *count gets the number of module ports. Fails only with ODIN3_ERR_INVALID_ARG, as the conventions
 * say. */
odin3_status odin3_module_get_port_count(const odin3_design *design, uint32_t module,
                                         uint32_t *count);

/*
 * For port `index` (0-based, declaration order) of the module: *node gets its port node (cell
 * type $port_in, $port_out or $port_inout), *wire the port's wire, which carries the port's name.
 * Each fails only with ODIN3_ERR_INVALID_ARG, as the conventions say (an index >= the port count
 * included).
 */
odin3_status odin3_module_get_port_node(const odin3_design *design, uint32_t module, uint32_t index,
                                        uint32_t *node);
odin3_status odin3_module_get_port_wire(const odin3_design *design, uint32_t module, uint32_t index,
                                        uint32_t *wire);

/*
 * *id gets the live node, net or wire of the module named name, 0 when there is none (a net is
 * also found by its alias names). O(1). Each fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say (a NULL name included).
 */
odin3_status odin3_module_lookup_node(const odin3_design *design, uint32_t module, const char *name,
                                      uint32_t *id);
odin3_status odin3_module_lookup_net(const odin3_design *design, uint32_t module, const char *name,
                                     uint32_t *id);
odin3_status odin3_module_lookup_wire(const odin3_design *design, uint32_t module, const char *name,
                                      uint32_t *id);

/* --- nodes --------------------------------------------------------------------------------- */

/* *live gets whether the node is live (not deleted). Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say. */
odin3_status odin3_node_is_live(const odin3_design *design, odin3_ref node, bool *live);

/*
 * *name gets the name of the node's cell type, e.g. "$sop" or a module name (see Strings).
 * Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_node_get_type_name(const odin3_design *design, odin3_ref node,
                                      const char **name);

/* *gran gets the granularity of the node's cell type. Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say. */
odin3_status odin3_node_get_granularity(const odin3_design *design, odin3_ref node,
                                        odin3_granularity *gran);

/* *name gets the node's instance name, "" when it has none (see Strings). Fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say. */
odin3_status odin3_node_get_name(const odin3_design *design, odin3_ref node, const char **name);

/*
 * *count gets the number of parameters of the node's cell type (every node has them all).
 * Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_node_get_param_count(const odin3_design *design, odin3_ref node,
                                        uint32_t *count);

/*
 * For parameter `index` (definition order) of the node: its name (see Strings), its kind, its
 * value as an integer (ODIN3_VAL_INT only; ODIN3_ERR_INVALID_ARG, logged, for another kind), or
 * its value as text (see Strings): an INT in decimal; BITS as one character per bit from '0',
 * '1', 'x', 'z', most significant bit first ("" for no bits); a STRING as is; a COVER as BLIF
 * .names rows, each its input characters ('0', '1', '-'), a space when it has inputs, its
 * output character and '\n'; a PROC (the BODY of a $proc node) as the RTLIL process block the
 * core prints for the node (2D spec section 13): first the node's attributes, one
 * "attribute \key value" line each, then "process <name>", its assign lines, its switches
 * ("switch <sig>" preceded by "attribute \full_case 1" / "attribute \parallel_case 1" lines when
 * flagged, then each "case <patterns>" line with the case's body, and "end"), its sync lines with
 * their update lines, and "end"; each line ends in '\n', the attribute, process and final end lines
 * at no indent and the lines inside indented two spaces per level; each position is spelled as the
 * net on its pin. ODIN3_ERR_INVALID_ARG (logged) for an index >= the parameter count, besides the
 * conventions, and for a PROC that cannot be printed (the node is dead, unlike the other kinds,
 * or a position is on an unconnected pin). odin3_node_get_param_text may intern the text in the
 * design's string table (not an IR change; other strings stay valid): ODIN3_ERR_NO_MEMORY then.
 */
odin3_status odin3_node_get_param_name(const odin3_design *design, odin3_ref node, uint32_t index,
                                       const char **name);
odin3_status odin3_node_get_param_kind(const odin3_design *design, odin3_ref node, uint32_t index,
                                       odin3_value_kind *kind);
odin3_status odin3_node_get_param_int(const odin3_design *design, odin3_ref node, uint32_t index,
                                      int64_t *value);
odin3_status odin3_node_get_param_text(const odin3_design *design, odin3_ref node, uint32_t index,
                                       const char **text);

/*
 * *pins gets all pins of the node: consecutive IDs in port order, then bit order (LSB first).
 * Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_node_get_pins(const odin3_design *design, odin3_ref node, odin3_span *pins);

/* *count gets the number of ports of the node's cell type. Fails only with ODIN3_ERR_INVALID_ARG,
 * as the conventions say. */
odin3_status odin3_node_get_port_count(const odin3_design *design, odin3_ref node, uint32_t *count);

/*
 * For port `port` (definition order) of the node: its pins (LSB first; empty for a width-0
 * port), name (see Strings), direction, and width (its pin count, which follows the node's
 * parameters). ODIN3_ERR_INVALID_ARG (logged) for port >= the port count, besides the
 * conventions.
 */
odin3_status odin3_node_get_port_pins(const odin3_design *design, odin3_ref node, uint32_t port,
                                      odin3_span *pins);
odin3_status odin3_node_get_port_name(const odin3_design *design, odin3_ref node, uint32_t port,
                                      const char **name);
odin3_status odin3_node_get_port_dir(const odin3_design *design, odin3_ref node, uint32_t port,
                                     odin3_dir *dir);
odin3_status odin3_node_get_port_width(const odin3_design *design, odin3_ref node, uint32_t port,
                                       uint32_t *width);

/* --- pins ---------------------------------------------------------------------------------- */

/*
 * For a pin: the node it belongs to, its port index on that node, its bit within the port (LSB
 * 0), and the net it is connected to (0 when unconnected). A pin lives while its node does.
 * Each fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_pin_get_node(const odin3_design *design, odin3_ref pin, uint32_t *node);
odin3_status odin3_pin_get_port(const odin3_design *design, odin3_ref pin, uint32_t *port);
odin3_status odin3_pin_get_bit(const odin3_design *design, odin3_ref pin, uint32_t *bit);
odin3_status odin3_pin_get_net(const odin3_design *design, odin3_ref pin, uint32_t *net);

/* --- nets ---------------------------------------------------------------------------------- */

/* *live gets whether the net is live (not deleted or merged away). Fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say. */
odin3_status odin3_net_is_live(const odin3_design *design, odin3_ref net, bool *live);

/*
 * *name gets the net's own name (see Strings), "" when it has none; names it was also known by
 * are its aliases. Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_net_get_name(const odin3_design *design, odin3_ref net, const char **name);

/*
 * A net's pins are its drivers (OUT and INOUT pins) first, then its sinks; the order within each
 * part is not significant. odin3_net_get_pin_count and odin3_net_get_driver_count give the number
 * of pins and of drivers (pins 0 .. drivers - 1 are the drivers); odin3_net_get_pin_at gives pin
 * `index`; odin3_net_get_driver gives the first driver, 0 when there is none. Each fails only
 * with ODIN3_ERR_INVALID_ARG, as the conventions say (for pin_at, an index >= the pin count
 * included).
 */
odin3_status odin3_net_get_pin_count(const odin3_design *design, odin3_ref net, uint32_t *count);
odin3_status odin3_net_get_driver_count(const odin3_design *design, odin3_ref net, uint32_t *count);
odin3_status odin3_net_get_pin_at(const odin3_design *design, odin3_ref net, uint32_t index,
                                  uint32_t *pin);
odin3_status odin3_net_get_driver(const odin3_design *design, odin3_ref net, uint32_t *pin);

/*
 * The net's aliases, oldest first (IR-2: names it was also known by, e.g. after a merge; its own
 * name is not an alias): *count gets how many, *name alias `index` (see Strings): a bare name as
 * is, a wire bit as "wire[i]" with the bit's declared index i. Both are O(aliases):
 * odin3_net_get_alias_name walks the alias chain from the start each call (O(index)), so reading
 * every alias of a net costs O(aliases^2). odin3_net_get_alias_count fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say; odin3_net_get_alias_name also for an index >=
 * the alias count, and with ODIN3_ERR_NO_MEMORY (it may intern the "wire[i]" text, as
 * odin3_node_get_param_text does).
 */
odin3_status odin3_net_get_alias_count(const odin3_design *design, odin3_ref net, uint32_t *count);
odin3_status odin3_net_get_alias_name(const odin3_design *design, odin3_ref net, uint32_t index,
                                      const char **name);

/* --- wires --------------------------------------------------------------------------------- */

/* *live gets whether the wire is live (not deleted). Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say. */
odin3_status odin3_wire_is_live(const odin3_design *design, odin3_ref wire, bool *live);

/* *name gets the wire's name, "" when it has none (see Strings). Fails only with
 * ODIN3_ERR_INVALID_ARG, as the conventions say. */
odin3_status odin3_wire_get_name(const odin3_design *design, odin3_ref wire, const char **name);

/* --- attributes ---------------------------------------------------------------------------- */

/*
 * *value gets the string attribute key of obj (a node, net, wire or module), NULL when obj has no
 * attribute key (see Strings). Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say:
 * also for a NULL key, a kind out of range, or an attribute that is not a string.
 */
odin3_status odin3_attr_get_string(const odin3_design *design, odin3_obj obj, const char *key,
                                   const char **value);

/*
 * Sets the string attribute key (non-empty) of the live object obj to a copy of value, replacing
 * any earlier value. An IR mutation (see Strings). ODIN3_ERR_INVALID_ARG, as the conventions
 * say, also for a NULL or empty key, a NULL value, a dead object or a kind out of range;
 * ODIN3_ERR_NO_MEMORY. The IR is unchanged on failure.
 */
odin3_status odin3_attr_set_string(odin3_design *design, odin3_obj obj, const char *key,
                                   const char *value);

/* --- cell types ---------------------------------------------------------------------------- */

/*
 * Cell-type flags (docs/IR.md IR-8). TRISTATE: output pins may share a net with other tristate
 * or inout drivers (a bus). ANYVIEW: legal in every view whatever the granularity (constants).
 * Simulation (1E): SEQ_EDGE, edge-triggered storage whose outputs depend only on its state (it
 * cuts the simulator's combinational graph); SEQ_LEVEL, a level-sensitive latch; at most one of
 * the two. CLOCK_PIN0: port 0 is the clock (edge) or enable (level) pin, a scalar input of
 * constant width 1. PROCESS (2D): the type is the built-in process cell $proc, whose live nodes
 * the IR counts per module (docs/IR.md IR-19); no other type, and no plugin type, may have it.
 */
enum {
    ODIN3_CT_TRISTATE = 1,
    ODIN3_CT_ANYVIEW = 2,
    ODIN3_CT_SEQ_EDGE = 4,
    ODIN3_CT_SEQ_LEVEL = 8,
    ODIN3_CT_CLOCK_PIN0 = 16,
    ODIN3_CT_PROCESS = 32
};

/*
 * The simulator's view of one cell (1E). Opaque in ABI v3: its layout and the hook contract are
 * src/sim/cell.h's, not frozen here, so only a plugin built from this source tree can implement
 * the simulation hooks below.
 */
typedef struct odin3_sim_cell odin3_sim_cell;

/* Evaluates one cell (src/sim/cell.h: never allocates, logs or fails). */
typedef void (*odin3_sim_fn)(const odin3_sim_cell *cell);

/* *bytes gets the scratch bytes simulate needs for one cell (docs/IR.md, the 1E hooks). */
typedef odin3_status (*odin3_sim_scratch_fn)(const odin3_sim_cell *cell, uint32_t *bytes);

/*
 * One port of a plugin cell type. name: printable ASCII without blanks, '=', '[' or ']' (a BLIF
 * formal). dir: an odin3_dir value. Width: the INT parameter named width_param when it is not NULL
 * (the node's value of it; width must then be 0), else the constant width. scalar: written without
 * brackets by the writers (a 1-bit port that is not a vector: width 1 and no width_param).
 */
typedef struct odin3_plugin_port {
    const char *name;
    uint32_t dir;
    uint32_t width;
    const char *width_param;
    bool scalar;
} odin3_plugin_port;

/*
 * One parameter of a plugin cell type. name: printable ASCII without blanks. kind: an
 * odin3_value_kind value. dflt: the default of an INT parameter; any other kind defaults to empty
 * (no bits, "", no cover rows), and dflt must be 0.
 *
 * Neither port nor parameter has reserved slots: a later ABI version that adds per-port width
 * functions or expressions, or non-INT defaults, adds them as arrays parallel to ports or params
 * reached through a reserved slot of odin3_plugin_celltype, so these layouts never change.
 */
typedef struct odin3_plugin_param {
    const char *name;
    uint32_t kind;
    int64_t dflt;
} odin3_plugin_param;

/*
 * A cell type as a plugin describes it: plain data (docs/IR.md IR-8, IR-11). name: printable
 * ASCII without blanks (a BLIF `.subckt` word). gran: an
 * odin3_granularity value, one of WORD, BIT, HARD or BLACKBOX (MODULE and PORT types belong to
 * the IR); flags: ODIN3_CT_* bits; ports and params: n_ports and n_params entries in definition
 * order (NULL only when the count is 0); names unique within each array, and every width_param
 * names an INT parameter. simulate and sim_scratch_bytes are the 1E simulation hooks, each may be
 * NULL (no simulate: the simulator refuses the type; no sim_scratch_bytes: no scratch).
 * reserved: must be all NULL, so a later ABI version can add hooks there without moving a field.
 */
typedef struct odin3_plugin_celltype {
    const char *name;
    uint32_t gran;
    uint32_t flags;
    const odin3_plugin_port *ports;
    const odin3_plugin_param *params;
    uint32_t n_ports;
    uint32_t n_params;
    odin3_sim_fn simulate;
    odin3_sim_scratch_fn sim_scratch_bytes;
    void *reserved[4];
} odin3_plugin_celltype;

/*
 * Registers a process-global cell type (from odin3_plugin_init, typically). The definition and
 * its strings and arrays are copied, so the caller may free them afterwards. Designs created
 * afterwards hold the type (odin3_design_create) and their readers instantiate it by name (a BLIF
 * `.subckt`, its parameters derived from the connected pins as for any type); existing designs do
 * not see it. ODIN3_ERR_INVALID_ARG (logged) for a NULL definition, a reserved slot that is not
 * NULL, a granularity, direction, kind or flag out of range (ODIN3_VAL_PROC and ODIN3_CT_PROCESS
 * belong to the built-in $proc and are out of range here), a name with a character it may not
 * hold, a scalar port that is not 1 bit wide, a width_param with a non-zero width, a non-INT
 * parameter with a non-zero dflt, a definition the IR rejects (a missing or duplicate name, a
 * width_param that names no INT parameter, inconsistent simulation flags), or a name already
 * registered; ODIN3_ERR_NO_MEMORY. Nothing is registered on failure. Process-wide; not thread-safe;
 * a type cannot be unregistered.
 */
odin3_status odin3_celltype_register(const odin3_plugin_celltype *def);

/* --- provenance ---------------------------------------------------------------------------- */

/*
 * A source location: file as the reader recorded it ("" when unknown; see Strings), 1-based line
 * and column of the start, and of the end (end_line, end_col; 0 when not recorded), and (ABI v4)
 * loc, the raw source-manager location (0 when unknown, e.g. from a BLIF reader): decode it, its
 * macro and include chain included, with the odin3_srcman_* functions.
 */
typedef struct odin3_source {
    const char *file;
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
    uint32_t loc;
} odin3_source;

/* Receives one source location (valid during the call only) with the user pointer given. */
typedef void (*odin3_source_visit)(const odin3_source *src, void *user);

/*
 * Visits the source locations obj comes from (docs/IR.md IR-12): every location of every source
 * or imported record its provenance reaches through its parents, depth-first in parent order, so
 * the first call is the location that names the object; each record once. obj is a node, net or
 * wire (live or dead; a pin's is its node's) or a module; an object without provenance visits
 * nothing. visit runs after the walk, so it may call reader functions on design (the visitors
 * included); it must not change the design.
 * ODIN3_ERR_INVALID_ARG as the conventions say (a NULL visit, a kind out of range included);
 * ODIN3_ERR_NO_MEMORY before any call.
 */
odin3_status odin3_prov_visit_sources(const odin3_design *design, odin3_obj obj,
                                      odin3_source_visit visit, void *user);

/*
 * An object found from a source location: the object, and whether it is live. An object that
 * compact has since freed (its history survives as a tombstone, IR-6) has obj.id 0 and live false.
 */
typedef struct odin3_prov_object {
    odin3_obj obj;
    bool live;
} odin3_prov_object;

/* Receives one object found (valid during the call only) with the user pointer given. */
typedef void (*odin3_prov_object_visit)(const odin3_prov_object *found, void *user);

/*
 * Visits every object that comes from line `line` of file (the reader's spelling of the path;
 * columns ignored): the objects whose records have that location, then everything derived from
 * them, breadth-first (IR §6 forward navigation), live and dead, modules included. A file the
 * design never read visits nothing. Builds a provenance index over the whole design on each call
 * (time and memory linear in the design); visit runs after the query and may call reader
 * functions on design (the visitors included); it must not change the design. ODIN3_ERR_INVALID_ARG
 * as the conventions say (a NULL file or visit included); ODIN3_ERR_NO_MEMORY before any call.
 */
odin3_status odin3_prov_visit_objects(const odin3_design *design, const char *file, uint32_t line,
                                      odin3_prov_object_visit visit, void *user);

/*
 * *count gets one past the last pass-run ID of the design's pass-run table (docs/IR.md IR-12):
 * runs are numbered 1 .. count - 1 in the order they began (every pass, reader and front end run
 * is one), so a design no run has touched has count 1. Fails only with ODIN3_ERR_INVALID_ARG, as
 * the conventions say.
 */
odin3_status odin3_design_get_passrun_count(const odin3_design *design, uint32_t *count);

/*
 * The row of pass run `run` (1 .. count - 1, see odin3_design_get_passrun_count): *name gets its
 * pass name, *args the text after the pass name as the script or caller gave it, trimmed, and
 * *info its run information, blank-separated key=value words the pass recorded (a value holding a
 * blank or any other byte outside `!`..`~`, a `"` or a `\` is in double quotes with `\"` and `\\`;
 * `abc` writes `script=… sha256=… abc=… K=… exchange=… depth=…`); "" for none. A pass sets its
 * information at most once, while it runs; rows never change afterwards. Strings as the
 * conventions say (see Strings). ODIN3_ERR_INVALID_ARG, as the conventions say, for run 0,
 * run >= count or any NULL output.
 */
odin3_status odin3_design_get_passrun(const odin3_design *design, uint32_t run, const char **name,
                                      const char **args, const char **info);

/* --- logging ------------------------------------------------------------------------------- */

/* Severity of a log message; a lower value is more severe. */
typedef enum odin3_log_level {
    ODIN3_LOG_ERROR,
    ODIN3_LOG_WARN,
    ODIN3_LOG_INFO,
    ODIN3_LOG_DEBUG,
    ODIN3_LOG_LEVEL_COUNT
} odin3_log_level;

/* Receives each delivered message (NUL-terminated, no trailing newline; valid during the call
 * only) with the user pointer given to odin3_log_set_sink. A message logged from inside a sink is
 * counted but not delivered. */
typedef void (*odin3_log_sink)(odin3_log_level level, const char *msg, void *user);

/*
 * Messages above max_level are dropped (counted, not delivered); process-wide, ODIN3_LOG_INFO by
 * default. ODIN3_ERR_INVALID_ARG for a level out of range (the level is unchanged then).
 */
odin3_status odin3_log_set_level(odin3_log_level max_level);

/* The current maximum delivered level (see odin3_log_set_level). Never fails. */
odin3_log_level odin3_log_get_level(void);

/*
 * Sends delivered messages to sink (with user) instead of stderr; NULL restores stderr
 * ("odin3: <level>: <msg>" lines). Process-wide. Never fails.
 */
void odin3_log_set_sink(odin3_log_sink sink, void *user);

/*
 * Logs msg at level through Odin III's log (filter, counts, sink), e.g. from a plugin. Longer
 * messages are cut to 1023 characters ending in "...". ODIN3_ERR_INVALID_ARG for a NULL msg or a
 * level out of range (nothing is logged).
 */
odin3_status odin3_log_write(odin3_log_level level, const char *msg);

/* --- AST and source manager (ABI v4, 2A) ---------------------------------------------------- *
 *
 * A design owns one source manager (odin3_srcman, AST-1) and the AST stores (odin3_ast) of its
 * read runs, at most one per (run, form) (docs/specs/2026-10-09-2A-ast-design.md §3, §4, §9).
 * Both are opaque and owned by the design: there is no destroy call; they live until the design
 * is destroyed (an AST may go earlier at the end of its read run, unless kept: keep levels below).
 *
 * Locations. A location (a "loc") is one uint32_t offset into the design's virtual source space;
 * 0 is "unknown". Every buffer (a file, a macro expansion, a macro-argument run, scratch text)
 * occupies [start, start + len]; a buffer ID is its creation index from 1.
 *
 * Nodes. A node is named by its store-local uint32_t ID, dense from 1 in creation order; every
 * child ID is smaller than its parent's. A node's children are its positional slots first (an
 * absent optional slot is 0), then its tail (spec §4.2, §4.3).
 *
 * Failures. As the IR functions above: ODIN3_ERR_INVALID_ARG, logged as "<function>: invalid
 * argument …", for a NULL handle or output pointer, a node ID that is 0 or not below the store's
 * node end, an index out of range, or a value out of its enum; the outputs are unchanged on any
 * failure. Builders also return the internal builder's statuses (spec §5, §10): INVALID_ARG for
 * misuse of the builder (a shape the slot table rejects, a sealed store), ODIN3_ERR_PARSE for a
 * cap (logged, located through the source manager), ODIN3_ERR_NO_MEMORY with the store unchanged.
 *
 * Strings. A const char * output is owned by the design (names, kind spellings: until the design
 * is destroyed) or by the handle that returned it where the function says so.
 * ------------------------------------------------------------------------------------------- */

/* An AST store and the design's source manager (opaque; owned by the design). */
typedef struct odin3_ast odin3_ast;
typedef struct odin3_srcman odin3_srcman;

/* Node kinds (spec §4.3): NONE = 0, then the slot table's rows in order; Phase 5 appends. */
typedef enum odin3_ast_kind {
    ODIN3_AST_NONE = 0,
    ODIN3_AST_UNIT = 1,
    ODIN3_AST_DIRECTIVE = 2,
    ODIN3_AST_MODULE = 3,
    ODIN3_AST_UDP_DECL = 4,
    ODIN3_AST_CONFIG_DECL = 5,
    ODIN3_AST_LIST = 6,
    ODIN3_AST_PORT_REF = 7,
    ODIN3_AST_PORT_DECL = 8,
    ODIN3_AST_DECLARATOR = 9,
    ODIN3_AST_RANGE = 10,
    ODIN3_AST_NET_DECL = 11,
    ODIN3_AST_VAR_DECL = 12,
    ODIN3_AST_PARAM_DECL = 13,
    ODIN3_AST_DEFPARAM = 14,
    ODIN3_AST_GENVAR_DECL = 15,
    ODIN3_AST_EVENT_DECL = 16,
    ODIN3_AST_STRENGTH = 17,
    ODIN3_AST_DELAY = 18,
    ODIN3_AST_CONT_ASSIGN = 19,
    ODIN3_AST_NET_ASSIGN = 20,
    ODIN3_AST_ALWAYS = 21,
    ODIN3_AST_INITIAL = 22,
    ODIN3_AST_INSTANTIATION = 23,
    ODIN3_AST_INSTANCE = 24,
    ODIN3_AST_CONNECTION = 25,
    ODIN3_AST_GATE_DECL = 26,
    ODIN3_AST_FUNCTION_DECL = 27,
    ODIN3_AST_TASK_DECL = 28,
    ODIN3_AST_SPECIFY_BLOCK = 29,
    ODIN3_AST_ATTR = 30,
    ODIN3_AST_GENERATE = 31,
    ODIN3_AST_GEN_FOR = 32,
    ODIN3_AST_GEN_IF = 33,
    ODIN3_AST_GEN_CASE = 34,
    ODIN3_AST_GEN_BLOCK = 35,
    ODIN3_AST_SEQ_BLOCK = 36,
    ODIN3_AST_PAR_BLOCK = 37,
    ODIN3_AST_BLOCKING_ASSIGN = 38,
    ODIN3_AST_NONBLOCKING_ASSIGN = 39,
    ODIN3_AST_PROC_CONT_ASSIGN = 40,
    ODIN3_AST_IF = 41,
    ODIN3_AST_CASE = 42,
    ODIN3_AST_CASE_ITEM = 43,
    ODIN3_AST_FOR = 44,
    ODIN3_AST_WHILE = 45,
    ODIN3_AST_REPEAT = 46,
    ODIN3_AST_FOREVER = 47,
    ODIN3_AST_TIMING_STMT = 48,
    ODIN3_AST_WAIT = 49,
    ODIN3_AST_DISABLE = 50,
    ODIN3_AST_EVENT_TRIGGER = 51,
    ODIN3_AST_TASK_CALL = 52,
    ODIN3_AST_NULL_STMT = 53,
    ODIN3_AST_EVENT_CONTROL = 54,
    ODIN3_AST_EVENT_EXPR = 55,
    ODIN3_AST_NUMBER = 56,
    ODIN3_AST_REAL = 57,
    ODIN3_AST_STRING = 58,
    ODIN3_AST_IDENT = 59,
    ODIN3_AST_HIER_NAME = 60,
    ODIN3_AST_SELECT = 61,
    ODIN3_AST_CONCAT = 62,
    ODIN3_AST_REPLICATE = 63,
    ODIN3_AST_UNARY = 64,
    ODIN3_AST_BINARY = 65,
    ODIN3_AST_TERNARY = 66,
    ODIN3_AST_CALL = 67,
    ODIN3_AST_MINTYPMAX = 68,
    ODIN3_AST_KIND_COUNT = 69 /* one past the last kind */
} odin3_ast_kind;

/* The two subsets of the one AST format (AST-9); fixed when a store is created. */
typedef enum odin3_ast_form {
    ODIN3_AST_FORM_PARSED = 1,
    ODIN3_AST_FORM_ELABORATED = 2
} odin3_ast_form;

/* Sub-kinds (node sub field), per kind, in the slot table's order; each enum ends with its count.
 */
typedef enum odin3_ast_directive_kind {
    ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE = 0,     /* default_nettype */
    ODIN3_AST_DIRECTIVE_TIMESCALE = 1,           /* timescale */
    ODIN3_AST_DIRECTIVE_CELLDEFINE = 2,          /* celldefine */
    ODIN3_AST_DIRECTIVE_ENDCELLDEFINE = 3,       /* endcelldefine */
    ODIN3_AST_DIRECTIVE_RESETALL = 4,            /* resetall */
    ODIN3_AST_DIRECTIVE_BEGIN_KEYWORDS = 5,      /* begin_keywords */
    ODIN3_AST_DIRECTIVE_END_KEYWORDS = 6,        /* end_keywords */
    ODIN3_AST_DIRECTIVE_UNCONNECTED_DRIVE = 7,   /* unconnected_drive */
    ODIN3_AST_DIRECTIVE_NOUNCONNECTED_DRIVE = 8, /* nounconnected_drive */
    ODIN3_AST_DIRECTIVE_PRAGMA = 9,              /* pragma */
    ODIN3_AST_DIRECTIVE_COUNT = 10
} odin3_ast_directive_kind;
typedef enum odin3_ast_direction {
    ODIN3_AST_PORT_DECL_INPUT = 0,  /* input */
    ODIN3_AST_PORT_DECL_OUTPUT = 1, /* output */
    ODIN3_AST_PORT_DECL_INOUT = 2,  /* inout */
    ODIN3_AST_PORT_DECL_COUNT = 3
} odin3_ast_direction;
typedef enum odin3_ast_net_kind {
    ODIN3_AST_NET_DECL_WIRE = 0,     /* wire */
    ODIN3_AST_NET_DECL_TRI = 1,      /* tri */
    ODIN3_AST_NET_DECL_WAND = 2,     /* wand */
    ODIN3_AST_NET_DECL_WOR = 3,      /* wor */
    ODIN3_AST_NET_DECL_TRIAND = 4,   /* triand */
    ODIN3_AST_NET_DECL_TRIOR = 5,    /* trior */
    ODIN3_AST_NET_DECL_TRI0 = 6,     /* tri0 */
    ODIN3_AST_NET_DECL_TRI1 = 7,     /* tri1 */
    ODIN3_AST_NET_DECL_TRIREG = 8,   /* trireg */
    ODIN3_AST_NET_DECL_UWIRE = 9,    /* uwire */
    ODIN3_AST_NET_DECL_SUPPLY0 = 10, /* supply0 */
    ODIN3_AST_NET_DECL_SUPPLY1 = 11, /* supply1 */
    ODIN3_AST_NET_DECL_COUNT = 12
} odin3_ast_net_kind;
typedef enum odin3_ast_var_kind {
    ODIN3_AST_VAR_DECL_REG = 0,      /* reg */
    ODIN3_AST_VAR_DECL_INTEGER = 1,  /* integer */
    ODIN3_AST_VAR_DECL_TIME = 2,     /* time */
    ODIN3_AST_VAR_DECL_REAL = 3,     /* real */
    ODIN3_AST_VAR_DECL_REALTIME = 4, /* realtime */
    ODIN3_AST_VAR_DECL_COUNT = 5
} odin3_ast_var_kind;
typedef enum odin3_ast_param_kind {
    ODIN3_AST_PARAM_DECL_PARAMETER = 0,  /* parameter */
    ODIN3_AST_PARAM_DECL_LOCALPARAM = 1, /* localparam */
    ODIN3_AST_PARAM_DECL_SPECPARAM = 2,  /* specparam */
    ODIN3_AST_PARAM_DECL_COUNT = 3
} odin3_ast_param_kind;
typedef enum odin3_ast_strength_kind {
    ODIN3_AST_STRENGTH_DRIVE = 0,  /* drive */
    ODIN3_AST_STRENGTH_CHARGE = 1, /* charge */
    ODIN3_AST_STRENGTH_COUNT = 2
} odin3_ast_strength_kind;
typedef enum odin3_ast_always_kind {
    ODIN3_AST_ALWAYS_ALWAYS = 0,       /* always */
    ODIN3_AST_ALWAYS_ALWAYS_FF = 1,    /* always_ff */
    ODIN3_AST_ALWAYS_ALWAYS_COMB = 2,  /* always_comb */
    ODIN3_AST_ALWAYS_ALWAYS_LATCH = 3, /* always_latch */
    ODIN3_AST_ALWAYS_COUNT = 4
} odin3_ast_always_kind;
typedef enum odin3_ast_gate_kind {
    ODIN3_AST_GATE_DECL_AND = 0,       /* and */
    ODIN3_AST_GATE_DECL_NAND = 1,      /* nand */
    ODIN3_AST_GATE_DECL_OR = 2,        /* or */
    ODIN3_AST_GATE_DECL_NOR = 3,       /* nor */
    ODIN3_AST_GATE_DECL_XOR = 4,       /* xor */
    ODIN3_AST_GATE_DECL_XNOR = 5,      /* xnor */
    ODIN3_AST_GATE_DECL_BUF = 6,       /* buf */
    ODIN3_AST_GATE_DECL_NOT = 7,       /* not */
    ODIN3_AST_GATE_DECL_BUFIF0 = 8,    /* bufif0 */
    ODIN3_AST_GATE_DECL_BUFIF1 = 9,    /* bufif1 */
    ODIN3_AST_GATE_DECL_NOTIF0 = 10,   /* notif0 */
    ODIN3_AST_GATE_DECL_NOTIF1 = 11,   /* notif1 */
    ODIN3_AST_GATE_DECL_NMOS = 12,     /* nmos */
    ODIN3_AST_GATE_DECL_PMOS = 13,     /* pmos */
    ODIN3_AST_GATE_DECL_CMOS = 14,     /* cmos */
    ODIN3_AST_GATE_DECL_RNMOS = 15,    /* rnmos */
    ODIN3_AST_GATE_DECL_RPMOS = 16,    /* rpmos */
    ODIN3_AST_GATE_DECL_RCMOS = 17,    /* rcmos */
    ODIN3_AST_GATE_DECL_TRAN = 18,     /* tran */
    ODIN3_AST_GATE_DECL_RTRAN = 19,    /* rtran */
    ODIN3_AST_GATE_DECL_TRANIF0 = 20,  /* tranif0 */
    ODIN3_AST_GATE_DECL_TRANIF1 = 21,  /* tranif1 */
    ODIN3_AST_GATE_DECL_RTRANIF0 = 22, /* rtranif0 */
    ODIN3_AST_GATE_DECL_RTRANIF1 = 23, /* rtranif1 */
    ODIN3_AST_GATE_DECL_PULLUP = 24,   /* pullup */
    ODIN3_AST_GATE_DECL_PULLDOWN = 25, /* pulldown */
    ODIN3_AST_GATE_DECL_COUNT = 26
} odin3_ast_gate_kind;
typedef enum odin3_ast_proc_assign_kind {
    ODIN3_AST_PROC_CONT_ASSIGN_ASSIGN = 0,   /* assign */
    ODIN3_AST_PROC_CONT_ASSIGN_DEASSIGN = 1, /* deassign */
    ODIN3_AST_PROC_CONT_ASSIGN_FORCE = 2,    /* force */
    ODIN3_AST_PROC_CONT_ASSIGN_RELEASE = 3,  /* release */
    ODIN3_AST_PROC_CONT_ASSIGN_COUNT = 4
} odin3_ast_proc_assign_kind;
typedef enum odin3_ast_case_kind {
    ODIN3_AST_CASE_CASE = 0,  /* case */
    ODIN3_AST_CASE_CASEZ = 1, /* casez */
    ODIN3_AST_CASE_CASEX = 2, /* casex */
    ODIN3_AST_CASE_COUNT = 3
} odin3_ast_case_kind;
typedef enum odin3_ast_edge {
    ODIN3_AST_EVENT_EXPR_NONE = 0,    /* none */
    ODIN3_AST_EVENT_EXPR_POSEDGE = 1, /* posedge */
    ODIN3_AST_EVENT_EXPR_NEGEDGE = 2, /* negedge */
    ODIN3_AST_EVENT_EXPR_COUNT = 3
} odin3_ast_edge;
typedef enum odin3_ast_base {
    ODIN3_AST_NUMBER_DEC = 0, /* dec */
    ODIN3_AST_NUMBER_BIN = 1, /* bin */
    ODIN3_AST_NUMBER_OCT = 2, /* oct */
    ODIN3_AST_NUMBER_HEX = 3, /* hex */
    ODIN3_AST_NUMBER_COUNT = 4
} odin3_ast_base;
typedef enum odin3_ast_select_kind {
    ODIN3_AST_SELECT_BIT = 0,        /* bit */
    ODIN3_AST_SELECT_PART = 1,       /* part */
    ODIN3_AST_SELECT_PART_PLUS = 2,  /* part_plus */
    ODIN3_AST_SELECT_PART_MINUS = 3, /* part_minus */
    ODIN3_AST_SELECT_COUNT = 4
} odin3_ast_select_kind;
typedef enum odin3_ast_unary_op {
    ODIN3_AST_UNARY_PLUS = 0,  /* + */
    ODIN3_AST_UNARY_MINUS = 1, /* - */
    ODIN3_AST_UNARY_LNOT = 2,  /* ! */
    ODIN3_AST_UNARY_NOT = 3,   /* ~ */
    ODIN3_AST_UNARY_AND = 4,   /* & */
    ODIN3_AST_UNARY_NAND = 5,  /* ~& */
    ODIN3_AST_UNARY_OR = 6,    /* | */
    ODIN3_AST_UNARY_NOR = 7,   /* ~| */
    ODIN3_AST_UNARY_XOR = 8,   /* ^ */
    ODIN3_AST_UNARY_XNOR = 9,  /* ~^ */
    ODIN3_AST_UNARY_COUNT = 10
} odin3_ast_unary_op;
typedef enum odin3_ast_binary_op {
    ODIN3_AST_BINARY_ADD = 0,     /* + */
    ODIN3_AST_BINARY_SUB = 1,     /* - */
    ODIN3_AST_BINARY_MUL = 2,     /* * */
    ODIN3_AST_BINARY_DIV = 3,     /* / */
    ODIN3_AST_BINARY_MOD = 4,     /* % */
    ODIN3_AST_BINARY_POW = 5,     /* ** */
    ODIN3_AST_BINARY_EQ = 6,      /* == */
    ODIN3_AST_BINARY_NE = 7,      /* != */
    ODIN3_AST_BINARY_CASE_EQ = 8, /* === */
    ODIN3_AST_BINARY_CASE_NE = 9, /* !== */
    ODIN3_AST_BINARY_LAND = 10,   /* && */
    ODIN3_AST_BINARY_LOR = 11,    /* || */
    ODIN3_AST_BINARY_LT = 12,     /* < */
    ODIN3_AST_BINARY_LE = 13,     /* <= */
    ODIN3_AST_BINARY_GT = 14,     /* > */
    ODIN3_AST_BINARY_GE = 15,     /* >= */
    ODIN3_AST_BINARY_AND = 16,    /* & */
    ODIN3_AST_BINARY_OR = 17,     /* | */
    ODIN3_AST_BINARY_XOR = 18,    /* ^ */
    ODIN3_AST_BINARY_XNOR = 19,   /* ~^ */
    ODIN3_AST_BINARY_SHL = 20,    /* << */
    ODIN3_AST_BINARY_SHR = 21,    /* >> */
    ODIN3_AST_BINARY_ASHL = 22,   /* <<< */
    ODIN3_AST_BINARY_ASHR = 23,   /* >>> */
    ODIN3_AST_BINARY_COUNT = 24
} odin3_ast_binary_op;

/* Flags (AST-7): per kind, bits from 0 in the row's order; fields as _SHIFT/_MASK. */

enum {
    ODIN3_AST_F_MODULE_MACROMODULE = 1U << 0,

    ODIN3_AST_F_PORT_REF_DOTTED = 1U << 0,

    ODIN3_AST_F_PORT_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_PORT_DECL_IN_HEADER = 1U << 1,
    ODIN3_AST_F_PORT_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_PORT_DECL_DT_MASK = 0x7U << 2,
    ODIN3_AST_F_PORT_DECL_NET_SHIFT = 5, /* 0 = unspecified, else 1 + odin3_ast_net_kind */
    ODIN3_AST_F_PORT_DECL_NET_MASK = 0xFU << 5,

    ODIN3_AST_F_NET_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_NET_DECL_VECTORED = 1U << 1,
    ODIN3_AST_F_NET_DECL_SCALARED = 1U << 2,

    ODIN3_AST_F_VAR_DECL_SIGNED = 1U << 0,

    ODIN3_AST_F_PARAM_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_PARAM_DECL_IN_HEADER = 1U << 1,
    ODIN3_AST_F_PARAM_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_PARAM_DECL_DT_MASK = 0x7U << 2,

    ODIN3_AST_F_STRENGTH_S0_SHIFT = 0, /* odin3_ast_drive_value or odin3_ast_charge_value */
    ODIN3_AST_F_STRENGTH_S0_MASK = 0xFU << 0,
    ODIN3_AST_F_STRENGTH_S1_SHIFT = 4,
    ODIN3_AST_F_STRENGTH_S1_MASK = 0xFU << 4,

    ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC = 1U << 0,
    ODIN3_AST_F_FUNCTION_DECL_SIGNED = 1U << 1,
    ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_FUNCTION_DECL_DT_MASK = 0x7U << 2,

    ODIN3_AST_F_TASK_DECL_AUTOMATIC = 1U << 0,

    ODIN3_AST_F_ATTR_METACOMMENT = 1U << 0,

    ODIN3_AST_F_TASK_CALL_SYSTEM = 1U << 0,
    ODIN3_AST_F_TASK_CALL_HIER = 1U << 1,

    ODIN3_AST_F_CASE_ITEM_DEFAULT = 1U << 0,

    ODIN3_AST_F_EVENT_CONTROL_STAR = 1U << 0,
    ODIN3_AST_F_EVENT_CONTROL_NO_PARENS = 1U << 1,

    ODIN3_AST_F_EVENT_EXPR_AFTER_OR = 1U << 0,

    ODIN3_AST_F_NUMBER_SIGNED = 1U << 0,

    ODIN3_AST_F_STRING_EMPTY = 1U << 0, /* the empty string "": the node's name is 0 */

    ODIN3_AST_F_CALL_SYSTEM = 1U << 0,
    ODIN3_AST_F_CALL_HIER = 1U << 1,
};

/* The defined-flag mask of each kind with flags (the F column); check rejects other bits. */
enum {
    ODIN3_AST_FM_MODULE = ODIN3_AST_F_MODULE_MACROMODULE,
    ODIN3_AST_FM_PORT_REF = ODIN3_AST_F_PORT_REF_DOTTED,
    ODIN3_AST_FM_PORT_DECL = ODIN3_AST_F_PORT_DECL_SIGNED | ODIN3_AST_F_PORT_DECL_IN_HEADER |
                             ODIN3_AST_F_PORT_DECL_DT_MASK | ODIN3_AST_F_PORT_DECL_NET_MASK,
    ODIN3_AST_FM_NET_DECL =
        ODIN3_AST_F_NET_DECL_SIGNED | ODIN3_AST_F_NET_DECL_VECTORED | ODIN3_AST_F_NET_DECL_SCALARED,
    ODIN3_AST_FM_VAR_DECL = ODIN3_AST_F_VAR_DECL_SIGNED,
    ODIN3_AST_FM_PARAM_DECL = ODIN3_AST_F_PARAM_DECL_SIGNED | ODIN3_AST_F_PARAM_DECL_IN_HEADER |
                              ODIN3_AST_F_PARAM_DECL_DT_MASK,
    ODIN3_AST_FM_STRENGTH = ODIN3_AST_F_STRENGTH_S0_MASK | ODIN3_AST_F_STRENGTH_S1_MASK,
    ODIN3_AST_FM_FUNCTION_DECL = ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC |
                                 ODIN3_AST_F_FUNCTION_DECL_SIGNED |
                                 ODIN3_AST_F_FUNCTION_DECL_DT_MASK,
    ODIN3_AST_FM_TASK_DECL = ODIN3_AST_F_TASK_DECL_AUTOMATIC,
    ODIN3_AST_FM_ATTR = ODIN3_AST_F_ATTR_METACOMMENT,
    ODIN3_AST_FM_TASK_CALL = ODIN3_AST_F_TASK_CALL_SYSTEM | ODIN3_AST_F_TASK_CALL_HIER,
    ODIN3_AST_FM_CASE_ITEM = ODIN3_AST_F_CASE_ITEM_DEFAULT,
    ODIN3_AST_FM_EVENT_CONTROL =
        ODIN3_AST_F_EVENT_CONTROL_STAR | ODIN3_AST_F_EVENT_CONTROL_NO_PARENS,
    ODIN3_AST_FM_EVENT_EXPR = ODIN3_AST_F_EVENT_EXPR_AFTER_OR,
    ODIN3_AST_FM_NUMBER = ODIN3_AST_F_NUMBER_SIGNED,
    ODIN3_AST_FM_STRING = ODIN3_AST_F_STRING_EMPTY,
    ODIN3_AST_FM_CALL = ODIN3_AST_F_CALL_SYSTEM | ODIN3_AST_F_CALL_HIER,
};

/* Values of the DT field (PORT_DECL, PARAM_DECL, FUNCTION_DECL). */
typedef enum odin3_ast_dt {
    ODIN3_AST_DT_NONE = 0,
    ODIN3_AST_DT_REG,
    ODIN3_AST_DT_INTEGER,
    ODIN3_AST_DT_REAL,
    ODIN3_AST_DT_REALTIME,
    ODIN3_AST_DT_TIME
} odin3_ast_dt;

/* Values of STRENGTH's S0/S1 fields: drive strengths (sub DRIVE), charge (sub CHARGE, in S0). */
typedef enum odin3_ast_drive_value {
    ODIN3_AST_DRIVE_NONE = 0,
    ODIN3_AST_DRIVE_HIGHZ,
    ODIN3_AST_DRIVE_WEAK,
    ODIN3_AST_DRIVE_PULL,
    ODIN3_AST_DRIVE_STRONG,
    ODIN3_AST_DRIVE_SUPPLY
} odin3_ast_drive_value;
typedef enum odin3_ast_charge_value {
    ODIN3_AST_CHARGE_NONE = 0,
    ODIN3_AST_CHARGE_SMALL,
    ODIN3_AST_CHARGE_MEDIUM,
    ODIN3_AST_CHARGE_LARGE
} odin3_ast_charge_value;

/*
 * Caps (spec §3.5, §10): exceeding one is a located ODIN3_ERR_PARSE. Node and child-table
 * counts are bounded by the 32-bit ID space (then ODIN3_ERR_NO_MEMORY).
 */
enum {
    ODIN3_AST_MAX_DEPTH = 32768,          /* node height, builder-enforced */
    ODIN3_AST_MAX_NUMBER_BITS = 1 << 20,  /* bits of one literal */
    ODIN3_AST_MAX_DECIMAL_DIGITS = 4096,  /* digits of a decimal literal */
    ODIN3_AST_MAX_STRING_BYTES = 1 << 17, /* a string literal (lexer-checked) */
    ODIN3_AST_MAX_IDENT_BYTES = 4096,     /* an identifier (lexer-checked) */
    ODIN3_AST_MAX_TEXT_BYTES = 1 << 20,   /* an opaque TEXT payload */
    ODIN3_AST_MAX_CHILDREN = 1 << 24,     /* children of one node */
    ODIN3_SRC_MAX_FILE_BYTES = 1 << 28,   /* one file buffer */
    ODIN3_SRC_MAX_BUFFERS = 1 << 24       /* buffers of one source manager */
};

/* Which held stores the end of a read run keeps (AST-16; the CLI's --keep-ast[=all]). */
typedef enum odin3_keep_ast {
    ODIN3_KEEP_AST_NONE = 0,   /* the read destroys its stores when it ends */
    ODIN3_KEEP_AST_ELABORATED, /* elaborated stores are kept (--keep-ast) */
    ODIN3_KEEP_AST_ALL         /* parsed and elaborated stores are kept (--keep-ast=all) */
} odin3_keep_ast;

/* Source-manager buffer kinds (spec §3.1). */
typedef enum odin3_srcbuf_kind {
    ODIN3_SRCBUF_FILE = 1,  /* a file's bytes, or the <command line> buffer */
    ODIN3_SRCBUF_EXPANSION, /* a macro body as spelled in its definition */
    ODIN3_SRCBUF_MACRO_ARG, /* one contiguous spelled run of a substituted argument */
    ODIN3_SRCBUF_SCRATCH    /* pasted or stringified text that exists nowhere */
} odin3_srcbuf_kind;

/*
 * A FILE buffer (spec §3.1): name = path as given, resolved = absolute path, library (strtab IDs
 * from odin3_ast_build_intern; resolved and library 0 for <command line>); parent = loc of the
 * `include that opened it, 0 for a project file; len = bytes. reserved must be zero.
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
 * An EXPANSION, MACRO_ARG or SCRATCH buffer (spec §3.1 table): kind (odin3_srcbuf_kind); name =
 * macro name (strtab; 0 for SCRATCH); parent = the use site (EXPANSION), the formal inside the
 * body (MACRO_ARG) or the operator inside the body (SCRATCH); parent_end = one past it (0 =
 * unknown); def = loc of the first byte as spelled (0 for SCRATCH; offset k <-> def + k, within
 * def's buffer); len = bytes. reserved must be zero.
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

/*
 * One segment of a stream's map (spec §3.3): output byte out_offset onward is spelled at loc
 * onward until the next segment (loc 0: inserted bytes). stream = the buffer ID of the stream's
 * project FILE buffer. reserved must be zero.
 */
typedef struct odin3_segment {
    uint32_t stream;
    uint32_t out_offset;
    uint32_t loc;
    uint32_t reserved[2];
} odin3_segment;

/* A decoded location: the FILE buffer ID, 1-based line and column in bytes (Clang's rule). */
typedef struct odin3_srcpos {
    uint32_t buffer;
    uint32_t line;
    uint32_t col;
    uint32_t reserved[2];
} odin3_srcpos;

/*
 * A node's scalar fields for a builder call (spec §5): kind (odin3_ast_kind), sub (the kind's
 * sub-kind enum, 0 for none), flags (the kind's ODIN3_AST_F_* bits), loc and end (the source
 * range; end 0 = unknown), name (strtab ID from odin3_ast_build_intern, 0 = none), payload (a
 * payload ID from odin3_ast_number_new_str, odin3_ast_build_real or odin3_ast_build_text for the
 * payload kinds NUMBER, REAL and the TEXT kinds; 0 otherwise). reserved must be zero.
 */
typedef struct odin3_ast_spec {
    uint8_t kind;
    uint8_t sub;
    uint16_t flags;
    uint32_t loc;
    uint32_t end;
    uint32_t name;
    uint32_t payload;
    uint32_t reserved[2];
} odin3_ast_spec;

/* Comment kinds and flags (spec §7). */
typedef enum odin3_ast_comment_kind {
    ODIN3_AST_COMMENT_LINE = 1, /* // ... */
    ODIN3_AST_COMMENT_BLOCK     /* a block comment */
} odin3_ast_comment_kind;

enum { ODIN3_AST_COMMENT_METACOMMENT = 1 /* a synopsys/synthesis directive */ };

/*
 * A comment to record: loc/end (raw locs; loc in a FILE buffer, end 0 or in the same buffer and
 * not before loc), kind (odin3_ast_comment_kind), flags (ODIN3_AST_COMMENT_*), the text bytes
 * text_ptr[0 .. text_len) (copied). reserved must be zero.
 */
typedef struct odin3_ast_comment_spec {
    uint32_t loc;
    uint32_t end;
    uint32_t kind;
    uint32_t flags;
    const char *text_ptr;
    uint32_t text_len;
    uint32_t reserved[2];
} odin3_ast_comment_spec;

/* --- the design's source manager and AST stores ------------------------------------------- */

/*
 * *sm gets the design's source manager, created on first use (AST-1); it lives as long as the
 * design. ODIN3_ERR_INVALID_ARG as the conventions say; ODIN3_ERR_NO_MEMORY (*sm unchanged) when
 * creating it fails.
 */
odin3_status odin3_design_get_srcman(odin3_design *design, odin3_srcman **sm);

/*
 * Creates an empty AST store of form (an odin3_ast_form value) for pass run `run` (non-zero),
 * using the design's string table and source manager, and hands it to the design, which holds it
 * as the store of (run, form) and destroys it with itself (or at the end of its read run, see the
 * keep levels). *ast gets the store (borrowed). ODIN3_ERR_INVALID_ARG (logged) as the conventions
 * say, also for an unknown form, run 0, or a store of the same run and form already held;
 * ODIN3_ERR_NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_design_new_ast(odin3_design *design, uint32_t form, uint32_t run,
                                  odin3_ast **ast);

/*
 * *ast gets the held store of pass run `run` and form (an odin3_ast_form value), NULL when the
 * design holds none (borrowed). ODIN3_ERR_INVALID_ARG as the conventions say, also for an unknown
 * form.
 */
odin3_status odin3_design_get_ast(const odin3_design *design, uint32_t run, uint32_t form,
                                  odin3_ast **ast);

/*
 * The held stores, in the order the design received them: *count gets how many, *ast store
 * `index` (borrowed; read its run and form with odin3_ast_get_run and odin3_ast_get_form). Each
 * fails only with ODIN3_ERR_INVALID_ARG, as the conventions say (an index >= the count included).
 */
odin3_status odin3_design_get_ast_count(const odin3_design *design, uint32_t *count);
odin3_status odin3_design_get_ast_at(const odin3_design *design, uint32_t index, odin3_ast **ast);

/*
 * Sets which stores the end of a read run keeps (default ODIN3_KEEP_AST_NONE; the CLI's
 * --keep-ast and --keep-ast=all). A script that names a pass wanting the AST raises it to
 * ODIN3_KEEP_AST_ELABORATED before it runs. Applies to later reads. ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL design or a level out of range (the level is unchanged then).
 */
odin3_status odin3_design_set_keep_ast(odin3_design *design, odin3_keep_ast level);

/* *level gets the keep level. Fails only with ODIN3_ERR_INVALID_ARG, as the conventions say. */
odin3_status odin3_design_get_keep_ast(const odin3_design *design, odin3_keep_ast *level);

/* --- AST readers ----------------------------------------------------------------------------- */

/*
 * A node's fields: its kind (odin3_ast_kind), sub-kind, flags, source range (loc; end, 0 =
 * unknown), name (the bytes as interned, "" for none; owned by the design), and child count
 * (slots and tail; 0 for payload kinds). Each fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say.
 */
odin3_status odin3_ast_get_kind(const odin3_ast *ast, uint32_t node, uint32_t *kind);
odin3_status odin3_ast_get_sub(const odin3_ast *ast, uint32_t node, uint32_t *sub);
odin3_status odin3_ast_get_flags(const odin3_ast *ast, uint32_t node, uint32_t *flags);
odin3_status odin3_ast_get_loc(const odin3_ast *ast, uint32_t node, uint32_t *loc);
odin3_status odin3_ast_get_end(const odin3_ast *ast, uint32_t node, uint32_t *end);
odin3_status odin3_ast_get_name(const odin3_ast *ast, uint32_t node, const char **name);
odin3_status odin3_ast_get_nchild(const odin3_ast *ast, uint32_t node, uint32_t *nchild);

/*
 * *child gets child `index` of the node (slots first, then the tail), 0 for an absent optional
 * slot. ODIN3_ERR_INVALID_ARG (logged) for an index >= the child count, besides the conventions.
 */
odin3_status odin3_ast_get_child(const odin3_ast *ast, uint32_t node, uint32_t index,
                                 uint32_t *child);

/*
 * The node's spellings: *name gets its kind's name ("UNIT" … "MINTYPMAX"), or its sub-kind's
 * spelling ("+", "posedge", "casez" …; "" when its kind has none). Static strings. Each fails
 * only with ODIN3_ERR_INVALID_ARG, as the conventions say.
 */
odin3_status odin3_ast_get_kind_name(const odin3_ast *ast, uint32_t node, const char **name);
odin3_status odin3_ast_get_sub_name(const odin3_ast *ast, uint32_t node, const char **name);

/*
 * A NUMBER node's literal as written (spec §4.5): *bits gets its bits as one character each from
 * '0', '1', 'x', 'z' and '?', most significant first ('?' for every z bit when a ? digit was
 * written: the store keeps one such flag per literal); *width gets its declared size, 0 when
 * unsized. Base and signedness are the node's sub-kind and ODIN3_AST_F_NUMBER_SIGNED. The text
 * is interned in the design's string table (not an IR change; it lives as long as the design).
 * ODIN3_ERR_INVALID_ARG (logged) for a node that is not a NUMBER, besides the conventions;
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_ast_get_number_str(const odin3_ast *ast, uint32_t node, const char **bits,
                                      uint32_t *width);

/* A REAL node's value. ODIN3_ERR_INVALID_ARG (logged) for any other node, besides the
 * conventions. */
odin3_status odin3_ast_get_real(const odin3_ast *ast, uint32_t node, double *value);

/*
 * A TEXT-payload node's raw bytes (SPECIFY_BLOCK, UDP_DECL, CONFIG_DECL): *text gets them (not
 * NUL-terminated; owned by the store), *len their count. ODIN3_ERR_INVALID_ARG (logged) for any
 * other node, besides the conventions.
 */
odin3_status odin3_ast_get_text(const odin3_ast *ast, uint32_t node, const char **text,
                                uint32_t *len);

/*
 * The ATTR nodes attached to the node (spec §7): *count gets how many, *attr attribute `index`.
 * Each fails only with ODIN3_ERR_INVALID_ARG, as the conventions say (an index >= the count
 * included).
 */
odin3_status odin3_ast_get_attr_count(const odin3_ast *ast, uint32_t node, uint32_t *count);
odin3_status odin3_ast_get_attr_at(const odin3_ast *ast, uint32_t node, uint32_t index,
                                   uint32_t *attr);

/*
 * Store-level fields: *end gets one past the last node ID (iterate 1 .. end - 1), *form its
 * odin3_ast_form, *run the pass run that built it. The roots are its UNIT nodes in creation
 * order: *count gets how many, *root root `index`. Each fails only with ODIN3_ERR_INVALID_ARG, as
 * the conventions say (an index >= the root count included).
 */
odin3_status odin3_ast_get_node_end(const odin3_ast *ast, uint32_t *end);
odin3_status odin3_ast_get_form(const odin3_ast *ast, uint32_t *form);
odin3_status odin3_ast_get_run(const odin3_ast *ast, uint32_t *run);
odin3_status odin3_ast_get_root_count(const odin3_ast *ast, uint32_t *count);
odin3_status odin3_ast_get_root_at(const odin3_ast *ast, uint32_t index, uint32_t *root);

/*
 * (ABI v4, 2D) *index gets where the library UNITs of a parsed store begin (2C §4.2 step 3, 2A
 * §9 (b)): the root index of the first UNIT read from a -v library file or a -y library
 * directory. Every root before it came from a design file, every root at or past it from a
 * library; 0 when the store has none (a read always has a design UNIT first, so root 0 is never
 * a library UNIT) and for an elaborated store. Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say.
 */
odin3_status odin3_ast_get_library_units_from(const odin3_ast *ast, uint32_t *index);

/*
 * (ABI v4, 2D) The options of odin3_ast_elaborate (EL-3): nolatches (Yosys -nolatches: the
 * unassigned paths of a combinational block are x), nomem2reg (never replace a memory by
 * registers except where forced). top, top_at and auto_blackbox are options of the front end that
 * writes the store (stage 1 chose the top and the black boxes); they are ignored here.
 * reserved_pad (the byte after the three flags, explicit so the layout has no hidden padding) and
 * reserved must be zero.
 */
typedef struct odin3_elab_options {
    uint32_t top;
    uint32_t top_at;
    bool nolatches, nomem2reg, auto_blackbox;
    uint8_t reserved_pad;
    uint32_t reserved[4];
} odin3_elab_options;

/*
 * (ABI v4, 2D) Elaborates the ELABORATED store `ast` (spec EL-2 steps 4 and 5; the seam an adapter
 * that builds the elaborated form itself, such as the slang adapter, drives) into IR in `design`:
 * binds it (the symbol table becomes the store's), makes one IR module per MODULE with its ports,
 * then every body, and on success sets the design's top to the UNIT's first MODULE and logs
 * `read_verilog: elaborated top 'T': ...`. The store must be held by the design (made with
 * odin3_design_new_ast, form ODIN3_AST_FORM_ELABORATED), complete (an adapter ends it with
 * odin3_ast_finish; this call does not check that) and in the form the spec's EL-4 describes; it is
 * never destroyed and, being held, stays so whatever the keep level. Its provenance run is the
 * store's (odin3_ast_get_run): the SOURCE records of the IR carry that run, which must be a pass
 * run of the design (a plugin cannot yet learn a pass-run ID, so until an ABI function opens one
 * the slang adapter supplies the run of the read it performs itself). `opts` may be NULL (the
 * defaults). Because an adapter's store is untrusted, 2A's check of the elaborated form runs in
 * every build before anything is bound. Errors in the source the store came from are located
 * diagnostics and ODIN3_ERR_PARSE: the modules made stay in the design (valid, incomplete) and the
 * top is not set, so after such a failure the design is fit only for destruction.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL design or ast, non-zero reserved words, a store that is
 * not elaborated (`odin3_ast_elaborate: the store is not elaborated`), not held by the design, of
 * an unknown run, that breaks the elaborated form (also found while the modules are made, such as a
 * black-box carrier whose ports or parameters differ from its definition's) or has a symbol table
 * already (so a second call on one store); ODIN3_ERR_NO_MEMORY (the IR made so far stays valid);
 * ODIN3_ERR_CHECK (logged) for an inconsistency inside the stage itself (a 2D bug, never a property
 * of the input). Once a call has bound the store to its symbol table, the store stays bound even
 * if that call fails, so any later call on it is ODIN3_ERR_INVALID_ARG.
 */
odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast,
                                 const odin3_elab_options *opts);

/* --- AST builders (adapters; spec §5) ------------------------------------------------------- */

/*
 * Appends a node with fields *spec whose children are ids[0 .. n) (copied; 0 for an absent
 * optional slot) and stores its ID in *node. The shape is validated against the slot table
 * (INVALID_ARG, logged, for a violation or a sealed store); more children than
 * ODIN3_AST_MAX_CHILDREN or a height above ODIN3_AST_MAX_DEPTH is ODIN3_ERR_PARSE located at
 * spec->loc; ODIN3_ERR_NO_MEMORY. ids may be NULL when n is 0. Nothing changes on failure.
 */
odin3_status odin3_ast_build_node(odin3_ast *ast, const odin3_ast_spec *spec, const uint32_t *ids,
                                  uint32_t n, uint32_t *node);

/*
 * List building on the store's pending stack (Bison list rules, visitor walks): *mark gets the
 * stack's height; odin3_ast_build_push pushes a node (0 for an empty argument slot, or an ID made
 * already); odin3_ast_build_marked appends a node with fields *spec whose children are the
 * entries from mark to the top and pops them (as odin3_ast_build_node; the stack is unchanged on
 * failure); odin3_ast_build_unwind pops back to mark (error recovery; a mark at or above the top
 * changes nothing; a sealed store's mark is 0). Each fails with ODIN3_ERR_INVALID_ARG (logged)
 * for a NULL argument; push and marked also for a sealed store, an ID not made yet (push) or a
 * mark above the top (marked), and with ODIN3_ERR_NO_MEMORY; marked with ODIN3_ERR_PARSE as
 * odin3_ast_build_node.
 */
odin3_status odin3_ast_build_mark(const odin3_ast *ast, uint32_t *mark);
odin3_status odin3_ast_build_push(odin3_ast *ast, uint32_t node);
odin3_status odin3_ast_build_marked(odin3_ast *ast, const odin3_ast_spec *spec, uint32_t mark,
                                    uint32_t *node);
odin3_status odin3_ast_build_unwind(odin3_ast *ast, uint32_t mark);

/*
 * Stores a NUMBER payload from bits, one character per bit from '0', '1', 'x', 'z' and '?' (a z
 * written as ?), most significant first, at least one, and width (the declared size, 0 =
 * unsized); *payload gets its payload ID, for a NUMBER node's spec. The bits are stored as
 * written, LSB first, never sized (odin3_ast_get_number_str reads them back). ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument, a sealed store, an empty string or another character;
 * ODIN3_ERR_PARSE located at loc for more than ODIN3_AST_MAX_NUMBER_BITS bits or a wider width;
 * ODIN3_ERR_NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_ast_number_new_str(odin3_ast *ast, uint32_t loc, uint32_t width,
                                      const char *bits, uint32_t *payload);

/* Stores a REAL payload and its payload ID in *payload. ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument, a sealed store or a non-finite value (inf or nan); ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_ast_build_real(odin3_ast *ast, double value, uint32_t *payload);

/*
 * Copies text[0 .. len) (an opaque construct's raw source bytes) into the store as a TEXT payload
 * and stores its payload ID in *payload. ODIN3_ERR_INVALID_ARG (logged) for a NULL argument (text
 * may be NULL when len is 0) or a sealed store; ODIN3_ERR_PARSE (logged) above
 * ODIN3_AST_MAX_TEXT_BYTES; ODIN3_ERR_NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_ast_build_text(odin3_ast *ast, const char *text, uint32_t len,
                                  uint32_t *payload);

/*
 * Attaches the ATTR nodes attrs[0 .. n) (n >= 1, copied) to node, once (spec §7).
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a sealed store, a node not made or itself an
 * ATTR, an entry that is not a made ATTR, or a node already annotated; ODIN3_ERR_NO_MEMORY. Nothing
 * changes on failure.
 */
odin3_status odin3_ast_build_attach(odin3_ast *ast, uint32_t node, const uint32_t *attrs,
                                    uint32_t n);

/*
 * Records a comment in the store's side table (spec §7), in stream order; odin3_ast_finish sorts
 * the table. ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a sealed store, non-zero
 * reserved fields, an unknown kind or flag, a loc not in a FILE buffer, or an end that is neither
 * 0 nor a later loc of that buffer; ODIN3_ERR_NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_ast_build_comment(odin3_ast *ast, const odin3_ast_comment_spec *spec);

/*
 * Interns the NUL-terminated str in the design's string table (names, macro and file names of
 * the source manager) and stores its ID in *name. ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_ast_build_intern(odin3_ast *ast, const char *str, uint32_t *name);

/*
 * Seals the store: sorts the comment table, frees the build scratch; a later builder call is
 * ODIN3_ERR_INVALID_ARG. ODIN3_ERR_INVALID_ARG (logged) for NULL or a store already sealed. The
 * readers work before and after.
 */
odin3_status odin3_ast_finish(odin3_ast *ast);

/* --- the SystemVerilog reader seam (ABI v6, v1.1.0; DESIGN §4.2) ----------------------------- */

/*
 * One macro a SystemVerilog read defines (a project record's `define): name is the macro name;
 * with has_value the body is value (possibly ""), without it the body is empty (`define name
 * with nothing after it). Both strings are NUL-terminated and borrowed: valid during the read.
 */
typedef struct odin3_sv_define {
    const char *name;
    const char *value;
    bool has_value;
    uint8_t reserved_pad;
} odin3_sv_define;

/*
 * A SystemVerilog read's inputs and options: the resolved source paths in listing order, the top
 * module name (NULL or "" = choose automatically), the project record's include search
 * directories and macro definitions (in project order), and the read's elaboration options.
 * read_verilog fills one for every SystemVerilog read (the arrays empty when the record has
 * none). The pointers are borrowed for the duration of the read call only.
 */
typedef struct odin3_sv_options {
    const char *const *paths;
    uint32_t npaths;
    const char *top;
    const char *const *incdirs;
    uint32_t nincdirs;
    const odin3_sv_define *defines;
    uint32_t ndefines;
    odin3_elab_options elab;
} odin3_sv_options;

/*
 * The body of a SystemVerilog reader, as the read_verilog pass drives it for a design whose
 * source files are .sv/.svh (or a project's SYSTEMVERILOG entries): design is the design the read
 * runs on, run the pass run read_verilog opened (the AST store the reader makes carries it, so the
 * IR's SOURCE records point at it) and opts the read's inputs (the resolved paths, the top) and
 * options (the project's include directories and defines, so a project's `include and `ifdef
 * resolve as they do for Verilog; and its elaboration options). A reader creates the elaborated
 * AST store itself (odin3_design_new_ast with ODIN3_AST_FORM_ELABORATED and run), fills it
 * through the builders above, seals it with odin3_ast_finish and drives stage 2 with
 * odin3_ast_elaborate. It returns ODIN3_OK, a located ODIN3_ERR_PARSE for source errors (the
 * design is then fit only for destruction, as after any failed read) or another odin3_status.
 * The adapter at adapters/slang exports the one function of this shape, odin3_read_slang; the
 * core never links it (it is C++; the core stays C17).
 */
typedef odin3_status (*odin3_sv_read_fn)(odin3_design *design, uint32_t run,
                                         const odin3_sv_options *opts);

/*
 * Makes read the process's SystemVerilog reader for later reads (the CLI registers the slang
 * adapter's odin3_read_slang at start-up). Process-wide, not thread-safe, final: a second call
 * with a different function is ODIN3_ERR_INVALID_ARG ("sv reader already registered"), and so is
 * a NULL one; every call fails only with ODIN3_ERR_INVALID_ARG.
 */
odin3_status odin3_sv_reader_register(odin3_sv_read_fn read);

/* --- source manager -------------------------------------------------------------------------- */

/*
 * Adds a FILE buffer (line 1 at offset 0 is implicit) and stores its ID in *buffer.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a name that is 0 or not a string ID, a
 * resolved or library that is not a string ID, a parent that is not a loc, or non-zero reserved
 * fields; ODIN3_ERR_PARSE (located at parent) for a file over ODIN3_SRC_MAX_FILE_BYTES, too many
 * buffers or an exhausted location space; ODIN3_ERR_NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_srcman_build_file(odin3_srcman *sm, const odin3_srcfile_spec *spec,
                                     uint32_t *buffer);

/*
 * Adds an EXPANSION, MACRO_ARG or SCRATCH buffer and stores its ID in *buffer.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a bad kind, a missing name (SCRATCH takes
 * 0), a parent that is 0 or not a loc, a parent_end that is neither 0 nor a loc, a def that is
 * not a loc (0 for SCRATCH) or whose buffer cannot hold len bytes from it, or non-zero reserved
 * fields; ODIN3_ERR_PARSE (located at parent) for too many buffers or an exhausted location
 * space; ODIN3_ERR_NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_srcman_build_expansion(odin3_srcman *sm, const odin3_expansion_spec *spec,
                                          uint32_t *buffer);

/*
 * Records that a line starts at offset of FILE buffer `buffer` (offsets strictly increasing per
 * buffer, above 0, at most its length). ODIN3_ERR_INVALID_ARG (logged) otherwise or for a NULL
 * sm; ODIN3_ERR_NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_srcman_build_line(odin3_srcman *sm, uint32_t buffer, uint32_t offset);

/*
 * Appends a segment to its stream's map (spec §3.3). ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument, a stream that is not a project FILE buffer, an out_offset that does not increase
 * within the stream, a loc that is neither 0 nor a loc, or non-zero reserved fields;
 * ODIN3_ERR_NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_srcman_build_segment(odin3_srcman *sm, const odin3_segment *seg);

/*
 * *loc gets the location of byte `offset` of buffer `buffer` (offset at most the buffer's length:
 * one past its end is a loc of its own). Fails only with ODIN3_ERR_INVALID_ARG, as the
 * conventions say (an unknown buffer or an offset past its end included).
 */
odin3_status odin3_srcman_get_loc(const odin3_srcman *sm, uint32_t buffer, uint32_t offset,
                                  uint32_t *loc);

/*
 * Decoding (spec §3.2). For loc, which must be 0 or a loc of a buffer made so far:
 * odin3_srcman_get_position fills *pos with the buffer, line and column of its file location
 * (ODIN3_ERR_INVALID_ARG for 0, which has none); odin3_srcman_get_file_loc gives its file
 * location (Clang's getFileLoc: an argument token reports its spelling, a body token the macro's
 * use), odin3_srcman_get_expansion_loc where the outermost macro was used (getExpansionLoc), and
 * odin3_srcman_get_spelling one spelling step (0 in a SCRATCH buffer); 0 gives 0. Each fails only
 * with ODIN3_ERR_INVALID_ARG, as the conventions say (a loc past the space made so far included).
 */
odin3_status odin3_srcman_get_position(const odin3_srcman *sm, uint32_t loc, odin3_srcpos *pos);
odin3_status odin3_srcman_get_file_loc(const odin3_srcman *sm, uint32_t loc, uint32_t *out);
odin3_status odin3_srcman_get_expansion_loc(const odin3_srcman *sm, uint32_t loc, uint32_t *out);
odin3_status odin3_srcman_get_spelling(const odin3_srcman *sm, uint32_t loc, uint32_t *out);

/*
 * *text gets "file:line:col" of loc's file location with the path as given ("<unknown>:0:0" for
 * 0), in a buffer of the source manager valid until the next odin3_srcman_get_format call on it
 * or its next mutation. ODIN3_ERR_INVALID_ARG as for odin3_srcman_get_spelling;
 * ODIN3_ERR_NO_MEMORY (*text unchanged).
 */
odin3_status odin3_srcman_get_format(odin3_srcman *sm, uint32_t loc, const char **text);

/* --- the ABC engine (3D) -------------------------------------------------------------------- */
enum { ODIN3_ABC_ENGINE_VERSION = 2, ODIN3_ABC_FAIL_TEXT_BYTES = 256 };

typedef enum odin3_abc_fail_kind {
    ODIN3_ABC_FAIL_NONE = 0,
    ODIN3_ABC_FAIL_COMMAND = 1,  /* a command returned non-zero (code = its value) */
    ODIN3_ABC_FAIL_EXIT = 2,     /* ABC called exit(code) or _exit(code) on the session thread */
    ODIN3_ABC_FAIL_ABORT = 3,    /* ABC called abort() on the session thread */
    ODIN3_ABC_FAIL_ASSERT = 4,   /* an ABC assertion failed (text = "file:line: expr") */
    ODIN3_ABC_FAIL_RESULT = 5,   /* no network, or a malformed one, where one was asked for */
    ODIN3_ABC_FAIL_DISABLED = 6, /* an earlier escape disabled the engine for this process */
    ODIN3_ABC_FAIL_BUSY = 7      /* a session is already open (one at a time, process-wide) */
} odin3_abc_fail_kind;

typedef struct odin3_abc_fail {
    uint32_t kind; /* odin3_abc_fail_kind */
    int32_t code;
    char text[ODIN3_ABC_FAIL_TEXT_BYTES]; /* NUL-terminated detail, "" when none */
} odin3_abc_fail;

/* One line of ABC's output (stdout and stderr merged), without its newline and without trailing
 * blanks or '\r'; valid during the call only, and called only from inside an engine call. The
 * callback must not call the engine: it runs while the call that produced the line finishes. */
typedef void (*odin3_abc_text_fn)(void *user, const char *line);

/* Odin III's exchange layout, equal to ABC's MiniAIG at the pin (aig/miniaig/miniaig.h; the
 * adapter converts if a pin bump changes it). objs has 2 * n_objs entries: object 0 is constant
 * 0; a PI is (0x7FFFFFFF, 0x7FFFFFFF), an AND (lit0, lit1) with lit0 < lit1 < 2 * index, a PO
 * (lit, 0x7FFFFFFF); the last n_regs PIs and POs are register outputs and inputs, in order. */
typedef struct odin3_abc_aig {
    const int32_t *objs;
    uint32_t n_objs;
    uint32_t n_regs;
} odin3_abc_aig;

/* Odin III's exchange layout, equal to ABC's MiniLUT at the pin (minilut.h). fanins has
 * lut_size * n_objs entries (0x7FFFFFFF unused; a PO has its driver in slot 0 and 0x7FFFFFFE in
 * slot 1); objects 0 and 1 are constants, then PIs, LUTs in topological order, POs. truths has
 * W * n_objs 32-bit words, W = (lut_size > 5 ? 1 << (lut_size - 5) : 1); a k-input object's
 * function is bits 0 .. 2^k - 1 of its W words, word 0 and bit 0 least significant, variable v
 * = fanin v. ci_from has (number of PIs) entries: the index of the loaded AIG's PI each PI came
 * from (ABC's names, AB-16), or -1; NULL when no names came back. */
typedef struct odin3_abc_luts {
    const int32_t *fanins;
    const uint32_t *truths;
    const int32_t *ci_from;
    uint32_t n_objs;
    uint32_t n_regs;
    uint32_t lut_size;
} odin3_abc_luts;

/* The network space get_cells reads (ABI 5): the network space's mapped network (ABC's map), or
 * the & space's cell mapping (&nf) converted with ABC's own conversion. */
typedef enum odin3_abc_cells_space {
    ODIN3_ABC_CELLS_NTK = 0,
    ODIN3_ABC_CELLS_GIA = 1
} odin3_abc_cells_space;

/* A gate-mapped network (ABI 5): net in odin3_abc_luts' layout with lut_size the widest gate
 * (at least 2): objects 0 and 1 the constants (a constant node of ABC's network is object 0
 * or 1), the PIs (real inputs, then register outputs), the gates in topological order, the
 * POs (real outputs, then register inputs); net.truths holds each gate's function over its
 * fanins in pin order, net.ci_from as for get_luts. gate has net.n_objs entries: the index
 * into names of a gate object's library gate, -1 for every other object. names holds n_names
 * NUL-terminated gate names, each once. */
typedef struct odin3_abc_cells {
    odin3_abc_luts net;
    const int32_t *gate;
    const char *const *names;
    uint32_t n_names;
} odin3_abc_cells;

/*
 * An ABC engine; one session per ABC run (§4.4). All but close return ODIN3_OK, ODIN3_ERR_ENGINE
 * (*fail filled; any call may fail EXIT, ABORT or ASSERT) or ODIN3_ERR_NO_MEMORY.
 * open: fresh ABC state, generators and libc rand() reset, output to text (NULL: dropped).
 *   BUSY, DISABLED.
 * load_aig: aig (copied) becomes the & space's network, its PIs named by index, and the name
 *   map's reference. RESULT.
 * exec: one already-checked command line, run as given. COMMAND.
 * get_luts: the & space's mapping with ci_from, owned by the session until close or the next
 *   call. RESULT.
 * name_map: aligned (the loaded AIG's PI, PO, register counts) as ABC's LUT side; *lits gets one
 *   MiniAIG literal or -1 per object, valid until close or the next name_map. RESULT.
 * load_genlib: text (a NUL-terminated genlib, copied) becomes the session's gate library (ABC's
 *   Mio_LibraryReadBuffer, named, then Mio_UpdateGenlib). Only the canonical form the core's genlib
 *   writer emits is read: "GATE name area out=formula;" lines, each followed by its explicit
 *   "PIN name phase" lines with six numbers (at most 6 PINs, each named by the formula, no generic
 *   pin), every number (area too) digits[.digits], printable ASCII with single spaces and '\n'
 *   only (no comments, tabs, '\r', '\\' or LATCH).
 *   Anything else is RESULT "ABC cannot read the gate library (line N: reason)"; a formula ABC
 *   cannot parse is RESULT "ABC cannot read the gate library (gate \"name\")". Load it before the
 *   mapping script: once any exec has run in the session, load_genlib is RESULT "load the gate
 *   library before the mapping script" (a mapped network, its backup, an &save copy or an undo
 *   would point into the library it frees). RESULT "no gate library (text is NULL)".
 * get_cells: the gate-mapped network of space (an odin3_abc_cells_space), owned by the session
 *   until close or the next call. RESULT "no gate-mapped network in the network space", "no cell
 *   mapping in the & space", "a node without a library gate", "a box that is not a latch", "a gate
 *   with N inputs (at most 6)", "no network space N", "nowhere to put the cells (cells is NULL)",
 *   "a fanin that is not a gate, an input or a constant".
 * close: frees all the session returned; after an escape, leaks ABC's state and disables.
 * Every call of a session must come from the thread that opened it; the linked engine treats a
 * call from another thread as an adapter assertion (ASSERT), which disables it for the process.
 */
typedef struct odin3_abc_engine {
    uint32_t version;       /* ODIN3_ABC_ENGINE_VERSION */
    const char *name;       /* "linked"; a test double names itself */
    const char *abc_commit; /* 40 lower-case hex digits */
    void *self;
    odin3_status (*open)(void *self, odin3_abc_text_fn text, void *user, odin3_abc_fail *fail);
    odin3_status (*load_aig)(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail);
    odin3_status (*exec)(void *self, const char *command, odin3_abc_fail *fail);
    odin3_status (*get_luts)(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail);
    odin3_status (*name_map)(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                             odin3_abc_fail *fail);
    odin3_status (*load_genlib)(void *self, const char *text, odin3_abc_fail *fail);
    odin3_status (*get_cells)(void *self, uint32_t space, odin3_abc_cells *cells,
                              odin3_abc_fail *fail);
    void (*close)(void *self);
    void *reserved[4]; /* NULL */
} odin3_abc_engine;

/*
 * Makes *engine (copied; self and the strings must outlive the process) the process's ABC engine.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL engine or name, another version, a NULL function, a
 * non-NULL reserved slot, an abc_commit that is not 40 lower-case hex digits, or an engine already
 * registered ("abc engine already registered (<name>)"). Process-wide, not thread-safe, final;
 * an engine call from another thread than the session's own disables the linked engine for the
 * process (see odin3_abc_engine).
 */
odin3_status odin3_abc_engine_register(const odin3_abc_engine *engine);

/* --- simulation (S1) ---------------------------------------------------------------------------
 *
 * Per-cycle simulation of a top module through the 2-state, cycle-based netlist simulator
 * (spec docs/specs/2026-10-21-s1-sim-api-design.md). The design must reach the netlist view
 * first: a Verilog design runs `read_verilog; proc; opt; lower`, a BLIF or JSON netlist needs no
 * pass. Hierarchical names use "."; signal IDs start at 1 (0 is "none").
 */

/* An open simulation of one top module (opaque; odin3_simulation_open, odin3_simulation_close). */
typedef struct odin3_simulation odin3_simulation;

/* What a signal is, in odin3_simulation_signal.kind. */
typedef enum odin3_simulation_kind {
    ODIN3_SIMULATION_INPUT = 0,   /* a top input port (or one bit of it) the caller sets */
    ODIN3_SIMULATION_CLOCK = 1,   /* a top input port whose every bit is a clock: steps toggle it */
    ODIN3_SIMULATION_OUTPUT = 2,  /* a top output port (or one bit of it) */
    ODIN3_SIMULATION_INTERNAL = 3 /* any other wire or net, at any depth */
} odin3_simulation_kind;

/* A signal (odin3_simulation_get_signal). Strings are owned by the simulation (SA-10). */
typedef struct odin3_simulation_signal {
    const char *name; /* full name: the scope's path, ".", the leaf; the leaf alone in scope 0 */
    const char *leaf; /* the wire's or net's own name, or "name[i]" for a bit select */
    uint32_t width;   /* bits, >= 1 */
    uint32_t kind;    /* an odin3_simulation_kind value */
    int32_t msb;      /* declared range of the wire (a bit select: its index twice; a net: 0, 0) */
    int32_t lsb;
    uint32_t scope;     /* the scope it belongs to (0 = the top) */
    odin3_obj obj;      /* the wire (ODIN3_OBJ_WIRE) or net (ODIN3_OBJ_NET) in the scope's module */
    uint32_t first_bit; /* vector position in obj of the signal's bit 0 (0 but for a bit select) */
    uint32_t reserved;  /* reserved, zero: fills the tail padding (size 56) for later use */
} odin3_simulation_signal;

/* An expanded module instance (odin3_simulation_get_scope). Strings as for signals (SA-10). */
typedef struct odin3_simulation_scope {
    const char *name; /* instance path from the top, "." separated; "" for scope 0 */
    const char *leaf; /* instance name ("#<node ID>" when unnamed); the top module's name for 0 */
    uint32_t parent;  /* parent scope; 0 for scope 0 */
    uint32_t module;  /* the module this scope is an instance of */
    odin3_ref inst;   /* the instance node in the parent scope's module; {0, 0} for scope 0 */
} odin3_simulation_scope; /* 32 bytes, no padding: no reserved slot */

/*
 * SA-1. Builds the simulation of `module` (0: the design's top). *out gets a handle the caller owns
 * and frees with odin3_simulation_close; NULL on any failure. Failures, all logged:
 * ODIN3_ERR_INVALID_ARG for a NULL design or out, a module ID that is not a module of the design,
 * module 0 when the design has no top ("odin3_simulation_open: the design has no top module"), a
 * design with processes ("module '<m>' has processes; run proc first"), and every build rejection
 * of the simulator (a cell type without a simulate hook: "run `lower` first"; a clock not driven
 * by a top input port; an inout top port; a combinational loop; two drivers on a net; the
 * flattening budget); ODIN3_ERR_NO_MEMORY. The design is not changed. On success the simulation
 * is in its power-on state (odin3_simulation_reset) and the design counts one more open
 * simulation: until it is closed every pass and script command on the design is refused (SA-11:
 * ODIN3_ERR_INVALID_ARG, "pass <name>: the design has <n> open simulation(s); close them first
 * (odin3_simulation_close)"). Readers, string attribute writes and odin3_design_set_top_module stay
 * allowed; odin3_design_copy gives a copy with no open simulation. Several simulations of one
 * design may be open at once, each with its own state. A design and all its simulations are used
 * by one thread at a time.
 */
odin3_status odin3_simulation_open(odin3_design *design, uint32_t module, odin3_simulation **out);

/*
 * SA-2. Frees the simulation and every string it handed out; the design counts one fewer open
 * simulation. NULL is a no-op. It must be called before odin3_design_destroy of its design.
 */
void odin3_simulation_close(odin3_simulation *sim);

/*
 * SA-3. One scope per expanded module instance, in expansion order: scope 0 is the top, then each
 * instance in the order the simulator expanded it (breadth first: a scope's instances in node-ID
 * order, appended after all scopes already listed). *count gets the number of scopes; get_scope
 * fills *info (strings owned by the simulation, valid until close). ODIN3_ERR_INVALID_ARG (logged)
 * for a NULL argument or scope >= count.
 */
odin3_status odin3_simulation_get_scope_count(const odin3_simulation *sim, uint32_t *count);
odin3_status odin3_simulation_get_scope(const odin3_simulation *sim, uint32_t scope,
                                        odin3_simulation_scope *info);

/*
 * SA-4. *count gets the number of listed signals, IDs 1..count, in this order: the top's input
 * ports in port order; its output ports in port order; then for each scope in scope order: its
 * module's live wires with a public name (named, not starting with '$') in wire-ID order, skipping
 * the top's own port wires; then its module's live nets with a public name of their own and no wire
 * membership at all, in net-ID order, width 1. A top input port is CLOCK when every bit of it
 * drives a clock, else INPUT; a child scope's port wires are INTERNAL. Signals that
 * odin3_simulation_find_signal appends are not counted. ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument.
 */
odin3_status odin3_simulation_get_signal_count(const odin3_simulation *sim, uint32_t *count);

/*
 * Fills *info for signal `signal`: a listed one (1..count) or one find_signal appended. Strings are
 * owned by the simulation and valid until close (appending signals never moves them).
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or an unknown signal.
 */
odin3_status odin3_simulation_get_signal(const odin3_simulation *sim, uint32_t signal,
                                         odin3_simulation_signal *info);

/*
 * SA-5. *signal gets the ID of the signal named `name`, or 0 when the simulated netlist has no
 * such name (ODIN3_OK in both cases). The lookup, in order: (1) a listed signal whose full name
 * equals name (the first of equal names); (2) a signal appended before under the same text; (3) in
 * the current scope (scope 0 first), a bit select "base[n]" of a live wire with a bit of declared
 * index n: a new 1-bit signal of that bit's kind (a top input port bit INPUT, or CLOCK when it is a
 * clock; a top output port bit OUTPUT; else INTERNAL); (4) in the current scope, a live wire named
 * name ('$' included), else a live net whose name or alias is name: a new signal (a net: width 1),
 * kind as in (3) for a top port wire, else INTERNAL; (5) a hierarchical name: at the first "."
 * from the left whose prefix names a live instance with a scope, enter that scope and resolve the
 * rest by (3) to (5); no backtracking. Steps 3 to 5 append the new signal (ID total + 1, ...);
 * get_signal_count still returns the listed count. So a flattened design and the unflattened one
 * answer the same names. A name that `opt` or `abc` removed is not found (keep it with (* keep *),
 * or simulate before that pass). ODIN3_ERR_INVALID_ARG (logged) for a NULL argument;
 * ODIN3_ERR_NO_MEMORY (logged) when appending fails.
 */
odin3_status odin3_simulation_find_signal(odin3_simulation *sim, const char *name,
                                          uint32_t *signal);

/*
 * SA-7. Sets an INPUT signal for the following settles and steps (it keeps its value until set
 * again). set takes the value LSB first in value, for width <= 64; set_bits takes count bytes,
 * bits[i] the value (0 or 1) of bit i, count equal to the width (the buffer is used only during
 * the call). A clock bit inside an INPUT signal must be given as 0 and is left alone.
 * ODIN3_ERR_INVALID_ARG (logged; nothing changes) for: NULL arguments, signal 0 or unknown, a kind
 * other than INPUT, width > 64 (set), a value with a bit set at or above the width (set), count
 * not equal to the width, a byte other than 0 or 1, a clock bit given as 1.
 */
odin3_status odin3_simulation_set(odin3_simulation *sim, uint32_t signal, uint64_t value);
odin3_status odin3_simulation_set_bits(odin3_simulation *sim, uint32_t signal, const uint8_t *bits,
                                       uint32_t count);

/*
 * SA-8. Reads any signal as of the last settle (open, reset, settle or step). get: width <= 64,
 * LSB first in *value, upper bits 0. get_bits: count equal to the width, bits[i] = bit i (the
 * buffer is used only during the call). ODIN3_ERR_INVALID_ARG (logged; output untouched) for NULL
 * arguments, an unknown signal, width > 64 (get), a wrong count.
 */
odin3_status odin3_simulation_get(const odin3_simulation *sim, uint32_t signal, uint64_t *value);
odin3_status odin3_simulation_get_bits(const odin3_simulation *sim, uint32_t signal, uint8_t *bits,
                                       uint32_t count);

/*
 * SA-9, SC-3. One settle: the combinational effect of inputs just set shows in the values, no
 * state advances and the cycle counter is unchanged. Allocates nothing; ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL sim.
 */
odin3_status odin3_simulation_settle(odin3_simulation *sim);

/*
 * SA-9, SC-2. Runs `cycles` cycles (0: nothing) and adds cycles to the cycle counter. One cycle
 * ticks every clock of the top together: settle with the inputs as set; raise every clock input
 * and send the rising edge to every edge-triggered cell; settle; lower every clock and send the
 * falling edge; settle. Values read afterwards are those after the last settle; the values match
 * the `sim` pass's vector file for the same inputs. A design with no clock steps too. Allocates
 * nothing; time per cycle is linear in the flattened design. ODIN3_ERR_INVALID_ARG (logged) for a
 * NULL sim.
 */
odin3_status odin3_simulation_step(odin3_simulation *sim, uint32_t cycles);

/*
 * SA-9. *cycle gets the number of cycles stepped since open or the last reset.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument.
 */
odin3_status odin3_simulation_get_cycle(const odin3_simulation *sim, uint64_t *cycle);

/*
 * SA-9, SC-4. Power-on reset: every value and state bit 0 (inputs included, clocks low), every
 * sequential cell gets its INIT again (INIT 1 starts at 1, others at 0), one settle, cycle counter
 * 0. It is not the design's reset input, which is an ordinary input the caller sets and steps.
 * Appended signals stay valid. Allocates nothing; ODIN3_ERR_INVALID_ARG (logged) for a NULL sim.
 */
odin3_status odin3_simulation_reset(odin3_simulation *sim);

/* ODIN3_CDEF_END */

#ifdef __cplusplus
}
#endif

#endif /* ODIN3_ODIN3_H */
