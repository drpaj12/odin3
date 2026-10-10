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
 *
 * The block between ODIN3_CDEF_BEGIN and ODIN3_CDEF_END is read verbatim by the
 * Python binding as a cffi cdef: keep it free of preprocessor directives and
 * attributes.
 */
#ifndef ODIN3_ODIN3_H
#define ODIN3_ODIN3_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN3_VERSION_MAJOR 0
#define ODIN3_VERSION_MINOR 0
#define ODIN3_VERSION_PATCH 0

/* ODIN3_CDEF_BEGIN */

/* Incremented on every incompatible change to this header. */
enum { ODIN3_ABI_VERSION = 3 };

/* Result of every fallible Odin III function. ODIN3_OK is always 0. */
typedef enum odin3_status {
    ODIN3_OK = 0,
    ODIN3_ERR_INVALID_ARG = 1,
    ODIN3_ERR_NO_MEMORY = 2,
    ODIN3_ERR_IO = 3,
    ODIN3_ERR_PLUGIN = 4,
    ODIN3_ERR_ABI_MISMATCH = 5,
    ODIN3_ERR_CHECK = 6, /* the IR violates an invariant (docs/IR.md section 9) */
    ODIN3_ERR_PARSE = 7, /* malformed input file (logged as file:line: message) */
    ODIN3_STATUS_COUNT = 8
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
 * odin3_*_is_live. Accessors also answer for a dead object (what it held when it died).
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

/* Kind of a parameter value (IR-10): integer, 4-state bit vector, string, SOP cover. */
typedef enum odin3_value_kind {
    ODIN3_VAL_INT,
    ODIN3_VAL_BITS,
    ODIN3_VAL_STRING,
    ODIN3_VAL_COVER
} odin3_value_kind;

/* --- design -------------------------------------------------------------------------------- */

/*
 * Creates an empty design whose cell-type table holds every cell type registered so far.
 * The caller owns it and frees it with odin3_design_destroy. Returns NULL on out of memory.
 */
odin3_design *odin3_design_create(void);

/* Frees the design and everything it owns (every string it handed out). NULL is a no-op. */
void odin3_design_destroy(odin3_design *design);

/*
 * Runs the pass named name on the design, with args as its argument text (NULL or "" for none;
 * words split on blanks, "double quotes" keep blanks), e.g. ("read_blif", "a.blif"). The pass
 * manager opens a provenance run named after the pass, checks the IR before and after (Debug
 * builds, or odin3_pass_set_check), and logs its time. Returns the pass's own failure status
 * (logged); ODIN3_ERR_CHECK when a check around it fails; ODIN3_ERR_INVALID_ARG for a NULL design
 * or name (as the conventions say), or (logged by the pass manager) an unknown pass, bad
 * arguments or an unclosed quote; ODIN3_ERR_NO_MEMORY.
 * Mutates the IR (see Strings above). Nothing is returned for the caller to free.
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
 * (logged) for a NULL design, text or src.origin; ODIN3_ERR_NO_MEMORY. Mutates the IR.
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
 * A pass as a plugin describes it. name: non-empty printable ASCII without blanks, ';' or '#'
 * (a script word); help: its one-line usage, shown by `odin3 --help`; run: its body; user:
 * passed to run as is. reserved: must be all NULL (later ABI versions give the slots a meaning).
 */
typedef struct odin3_plugin_pass {
    const char *name;
    const char *help;
    odin3_pass_run_fn run;
    void *user;
    void *reserved[4];
} odin3_plugin_pass;

/*
 * Registers a pass (from odin3_plugin_init, typically), after the built-ins and every pass
 * registered before it; scripts and odin3_design_run_pass then run it like a built-in: in its own
 * provenance run, with the IR checked before and after (Debug builds, or odin3_pass_set_check),
 * timed and logged. name and help are copied; user must stay valid while the pass can run.
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL pass, name, help or run, a bad name, a name already
 * registered (built-in or not) or a reserved slot that is not NULL; ODIN3_ERR_NO_MEMORY. Nothing
 * is registered on failure. Process-wide; not thread-safe; a pass cannot be unregistered.
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
 * output character and '\n'. ODIN3_ERR_INVALID_ARG (logged) for an index >= the parameter count,
 * besides the conventions. odin3_node_get_param_text may intern the text in the design's string
 * table (not an IR change; other strings stay valid): ODIN3_ERR_NO_MEMORY then.
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
 * constant width 1.
 */
enum {
    ODIN3_CT_TRISTATE = 1,
    ODIN3_CT_ANYVIEW = 2,
    ODIN3_CT_SEQ_EDGE = 4,
    ODIN3_CT_SEQ_LEVEL = 8,
    ODIN3_CT_CLOCK_PIN0 = 16
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
 * NULL, a granularity, direction, kind or flag out of range, a name with a character it may not
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
 * and column of the start, and of the end (end_line, end_col; 0 when not recorded).
 */
typedef struct odin3_source {
    const char *file;
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
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

/* ODIN3_CDEF_END */

#ifdef __cplusplus
}
#endif

#endif /* ODIN3_ODIN3_H */
