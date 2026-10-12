/*
 * elab1.h — stage 1 of elaboration (2D spec §2–§3; Verilog only): the run context, its
 * diagnostics (the error cap, warn-once, infos, the pending kinds of plan point (l)), the
 * definition table and module lookup (EL-6), top selection (EL-7), and (Task 11) parameters with
 * their precedence and types (EL-8, EL-16), the specialization table and its FIFO queue (EL-9,
 * §3.4), pass E's environments and pass W's writer of the elaborated store (EL-4), and (Task 12)
 * instances: connections, instance and gate arrays, black boxes and cell-type parameters (EL-10,
 * EL-11). Stage 1 never touches the IR; it reads the design's cell types, modules and project
 * record. Every traversal uses an explicit stack (odin3_ast_cursor, a frame vec) or a worklist.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ELAB1_H
#define ODIN3_FRONTENDS_VERILOG_ELAB1_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_elab1 odin3_elab1;

/* What a definition of the parsed store is (EL-6): a `(* blackbox *)` MODULE is a BLACKBOX. */
typedef enum odin3_elab_def_kind {
    ODIN3_DEF_MODULE = 1,
    ODIN3_DEF_BLACKBOX,
    ODIN3_DEF_UDP,
    ODIN3_DEF_CONFIG
} odin3_elab_def_kind;

/*
 * One definition, in store order (design UNITs first, then library UNITs; each UNIT's children
 * in order). library is the strtab ID of its UNIT's FILE-buffer library ("work" when the buffer
 * has none, as -v/-y files). For a MODULE or black box: the parameter DECLARATORs a `#()` may
 * override are odin3_elab1.params[first_param ..], first the n_header_params of the header's
 * `parameter` groups, then the n_body_params of the body's direct `parameter` items (localparams
 * and specparams excluded); the port positions are odin3_elab1.ports[first_port .. + n_positions):
 * an ANSI header gives one DECLARATOR per position, a non-ANSI one its PORT_REF entries (0 for an
 * empty position). Zero for UDPs and configurations.
 */
typedef struct odin3_elab_def {
    uint8_t kind; /* odin3_elab_def_kind */
    bool library_unit;
    uint32_t name;
    uint32_t library;
    odin3_ast_id node; /* the MODULE, UDP_DECL or CONFIG_DECL */
    uint32_t first_param;
    uint32_t n_header_params;
    uint32_t n_body_params;
    uint32_t first_port;
    uint32_t n_positions;
} odin3_elab_def;

/* What an instantiated name resolves to (EL-6 items 1–7). */
typedef enum odin3_elab_res_kind {
    ODIN3_RES_NONE = 0,         /* unresolved_module (item 7; the caller reports it) */
    ODIN3_RES_MODULE,           /* def: a MODULE or black box (items 1–3) */
    ODIN3_RES_UDP,              /* def: a UDP_DECL (item 4; Q7 is reported at the definition) */
    ODIN3_RES_CELLTYPE,         /* type: a hard or black-box cell type of an earlier read */
    ODIN3_RES_IMPLICIT_BLACKBOX /* --auto-blackbox (item 6) */
} odin3_elab_res_kind;

typedef struct odin3_elab_res {
    uint8_t kind; /* odin3_elab_res_kind */
    uint32_t def;
    odin3_celltype_id type;
} odin3_elab_res;

/* --- the run ------------------------------------------------------------------------------- */

/*
 * EL-2 step 1: *out = *given (the defaults when NULL) with the top filled when given has none:
 * the --top pass option (odin3_pass_get_options().top, interned; top_at 0), else the finished
 * project record's top and top_at. ODIN3_ERR_NO_MEMORY (interning); ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL design or out.
 */
odin3_status odin3_elab1_options(odin3_design *design, const odin3_elab_opts *given,
                                 odin3_elab_opts *out);

/*
 * A new stage-1 context over parsed (borrowed; it must outlive the context) for ctx's run with a
 * copy of *opts (NULL: the defaults), in *out (owned; odin3_elab1_destroy). ODIN3_ERR_NO_MEMORY
 * (with *out NULL); ODIN3_ERR_INVALID_ARG (logged) for a NULL ctx, parsed or out.
 */
odin3_status odin3_elab1_create(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts,
                                odin3_elab1 **out);

/* Frees the context and everything it owns (an unfinished elaborated store included). NULL is a
 * no-op. */
void odin3_elab1_destroy(odin3_elab1 *e1);

/*
 * A located error (unlocated when loc is 0), counted against the run's cap
 * (ODIN3_ELAB_MAX_ERRORS, lowered by odin3_elab_test_set_limits): once the cap's errors are
 * logged, the next error is replaced by "too many errors; elaboration stopped after N" (at its
 * loc) and the context stops (odin3_elab1_stopped); later errors log nothing. An error whose
 * text and loc were logged already (a construct of a module specialized several times) is not
 * logged or counted again (§9.3: never reported twice).
 */
void odin3_elab1_error(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* odin3_elab1_error, then the run stops (odin3_elab1_stopped): a fatal cap (§9.3, point (aa)). */
void odin3_elab1_fatal(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/*
 * A diagnostic the evaluator or typer reports (msg without location or level prefix): an error
 * as odin3_elab1_error, a warning or info once per (level, loc, text) for the run (§9.2).
 */
void odin3_elab1_report(odin3_elab1 *e1, odin3_log_level level, odin3_loc loc, const char *msg);

/*
 * A warning at parsed_node's location, once per (parsed node, message format) for the run, so a
 * construct elaborated several times warns once (point (v): the key is the node's ID and the
 * format's index in the run's format list, formats compared by text). fmt must be a string
 * literal (static storage): the format list keeps the pointer for later comparisons, so a built
 * buffer would leave it dangling (Task 10 review n4). Out of memory for the key latches
 * ODIN3_ERR_NO_MEMORY (odin3_elab1_status) and logs nothing.
 */
void odin3_elab1_warn_once(odin3_elab1 *e1, odin3_ast_id parsed_node, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/* odin3_elab1_warn_once with the once-key a different node from the one the warning is located
 * at (a specparam's declarator keys a warning located at its PARAM_DECL). */
void odin3_elab1_warn_once_at(odin3_elab1 *e1, odin3_ast_id key_node, odin3_ast_id at,
                              const char *fmt, ...) ODIN3_PRINTF(4, 5);

/* An info line at loc (unlocated when loc is 0). */
void odin3_elab1_info(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* True once the error cap stopped the run. */
bool odin3_elab1_stopped(const odin3_elab1 *e1);

/* The errors logged so far (the stop line excluded). */
uint32_t odin3_elab1_errors(const odin3_elab1 *e1);

/* ODIN3_OK, or the fatal status a void helper latched (ODIN3_ERR_NO_MEMORY). */
odin3_status odin3_elab1_status(const odin3_elab1 *e1);

/*
 * Point (l): true when parsed_node's kind is pending in stage 1 (odin3_elab_pending, the table or
 * a test's force); its located line is then logged once per kind per run
 * (odin3_elab_pend_report) and counted as an error under the cap. False when stage 1 elaborates
 * the kind: the caller handles the node.
 */
bool odin3_elab1_pending(odin3_elab1 *e1, odin3_ast_id parsed_node);

/*
 * A node stage 1 reached whose kind is neither pending nor handled: a 2D bug (a kind left the
 * table without its code). Logs "stage 1: no elaboration of '<KIND>' (a 2D bug)" at the node and
 * latches ODIN3_ERR_CHECK (odin3_elab1_status).
 */
void odin3_elab1_unhandled(odin3_elab1 *e1, odin3_ast_id parsed_node);

/* Reports parsed_node pending, or (not pending) as unhandled: stage 1 handles no kind of it yet. */
void odin3_elab1_not_yet(odin3_elab1 *e1, odin3_ast_id parsed_node);

/*
 * Point (l)'s line for a form of a landed kind whose elaboration a later task lands (a port
 * declared in the body, Task 15; a specparam, a real or time variable, a rejected net type, a
 * hierarchical call and a switch-level or pull gate, Task 16): the node's kind is
 * reported as pending (once per kind per run, counted as an error) while the forms table
 * (odin3_elab_pending_form) lists it, and as unhandled (a 2D bug) once it does not.
 */
void odin3_elab1_pend_form(odin3_elab1 *e1, odin3_ast_id parsed_node);

/* --- REJECT and IGNORE rows (elab1_diag.c; §9.1, §9.2) ---------------------------------------- */

/*
 * The REJECT rows that need only the node itself (L9, M11 excepted, N17, N18, E20, E21, T6, T13,
 * T14, T15, T16, T19, F4, Q6, N15, an edge/level mix): reports the node's error, at its
 * location and counted under the cap, and returns ODIN3_ERR_PARSE; ODIN3_OK when the node is not
 * one (nothing is logged). The rows that depend on the context (T12, K12, F2) are the scan's.
 */
odin3_status odin3_elab1_reject(odin3_elab1 *e1, odin3_ast_id node);

/*
 * The IGNORE rows' warnings for the node itself and the slots it holds (the ten rows of §9.2),
 * once per parsed node for the run (point (v)); ODIN3_OK, or the latched ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_ignore(odin3_elab1 *e1, odin3_ast_id node);

/*
 * The scan of definition def's body (a MODULE): every node, in source order, goes through
 * odin3_elab1_reject and odin3_elab1_ignore, and the context rows (T12, K12, F2) are checked.
 * Once per definition (the first specialization to run); *failed is set when it reported an
 * error, and the definition's specializations then skip passes E and W (EL-44). The scan stops
 * at the error cap. ODIN3_OK; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_scan(odin3_elab1 *e1, uint32_t def, bool *failed);

/* The unit-level directive node (outside any module): its IGNORE warning, unless it is read. */
void odin3_elab1_ignore_directive(odin3_elab1 *e1, odin3_ast_id directive);

/* --- definitions and lookup (elab1_lookup.c) ------------------------------------------------ */

/*
 * The definition table (EL-6): every MODULE, UDP_DECL and CONFIG_DECL of the parsed store in
 * store order, indexed by name, with each MODULE's parameters, port positions and the
 * INSTANTIATIONs anywhere in its body (every generate branch: divergence 20). Q7 and S2 are
 * reported at each design unit's UDP_DECL and CONFIG_DECL, whether or not instantiated (a library
 * unit's UDP when an instance resolves to it). The names taken by an earlier read (a hard or
 * black-box cell type, a module of an earlier read, a built-in cell type) are recorded per
 * MODULE and reported when the definition is reached (odin3_elab1_reach): stage 1 checks every
 * definition it is about to specialize. ODIN3_OK (errors are counted, not returned);
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_collect_defs(odin3_elab1 *e1);

/* The number of definitions, and definition def (NULL past the count). */
uint32_t odin3_elab1_def_count(const odin3_elab1 *e1);
const odin3_elab_def *odin3_elab1_def(const odin3_elab1 *e1, uint32_t def);

/*
 * Q7 or S2 at UDP or configuration definition def (§9.1: at the definition, whether or not it is
 * instantiated), once; the pending line instead when its kind is pending (point (l)).
 */
void odin3_elab1_reject_def(odin3_elab1 *e1, uint32_t def);

/*
 * EL-6: what name (a strtab ID) instantiated in a module of library resolves to, in order: a
 * design-unit MODULE in library; one in another library, the first in store order (several
 * libraries: the info line "module 'N' found in libraries 'A', 'B'; using 'A.N'", once per name);
 * a library-unit MODULE, the first in store order; a UDP_DECL; a registered cell type of
 * granularity hard or black box (never a built-in: none has those granularities, so a `$` name
 * never resolves to one); with --auto-blackbox an implicit black box (the info line "module 'N'
 * is not defined; it becomes an implicit black box (--auto-blackbox)", once per name); otherwise
 * ODIN3_RES_NONE, which the caller reports as unresolved_module. The info lines are unlocated.
 */
odin3_elab_res odin3_elab1_resolve(odin3_elab1 *e1, uint32_t name, uint32_t library);

/* --- the top and reachability (elab1_top.c) ------------------------------------------------ */

/*
 * EL-7. Given (opts.top "[lib.]mod"): that design- or library-unit MODULE (a design unit shadows
 * a library unit); not found → "unknown_top: 'm' is not a defined module", several libraries
 * without a prefix → "ambiguous_top: 'm' is defined in several libraries: 'a.m', 'b.m'", a black
 * box → "unknown_top: 'm' is a black box ((* blackbox *)), not a module to elaborate", all at
 * top_at (unlocated when 0). Automatic: the one design-unit MODULE (not a black box) that no
 * MODULE instantiates by name anywhere in its body (info "top module 'lib.m' chosen: the only
 * module no other module instantiates"); none → no_top, several → ambiguous_top, unlocated, the
 * candidates as 'lib.mod' in definition order. ODIN3_OK with *def; ODIN3_ERR_PARSE after the
 * error; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_select_top(odin3_elab1 *e1, uint32_t *def);

/*
 * Definition def is reached (the top, or an instance resolved to it): it is marked for the
 * not-elaborated line, and a MODULE whose name an earlier read took reports its error once, at
 * its keyword (EL-6). True when it can be specialized (a MODULE whose name is free, or a
 * `(* blackbox *)` definition, whose specializations carry its ports: Task 12); false for a taken
 * name (what depends on it is skipped silently, EL-44).
 */
bool odin3_elab1_reach_def(odin3_elab1 *e1, uint32_t def);

/*
 * The info line naming the design-unit modules no instance reached (in definition order), "not
 * instantiated under the top, not elaborated: 'a', 'b'", when there are any. ODIN3_OK;
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_not_elaborated(odin3_elab1 *e1);

/* --- parameters, specializations and passes E/W (Task 11) ----------------------------------- */

/* What a name of pass E's environment is (odin3_elab_entry.kind). */
typedef enum odin3_elab_entry_kind {
    ODIN3_ENTRY_PARAM = 1,  /* a `parameter` a #(), defparam or project may override */
    ODIN3_ENTRY_LOCALPARAM, /* a localparam */
    ODIN3_ENTRY_SIGNAL,     /* a net, variable or port: never a constant */
    ODIN3_ENTRY_GENVAR,
    ODIN3_ENTRY_FUNCTION,
    ODIN3_ENTRY_TASK,
    ODIN3_ENTRY_INSTANCE, /* an instance name (a defparam path's first component) */
    ODIN3_ENTRY_BLOCK     /* a user-named generate block of a construct in the scope (Task 14) */
} odin3_elab_entry_kind;

/*
 * One declaration of pass E's environment: its kind, its declaration node (the DECLARATOR; the
 * FUNCTION_DECL or TASK_DECL; the INSTANCE), its width and sign (SIGNAL: from its folded range,
 * integer 32 signed; a parameter or genvar: its value's), and a parameter's or genvar's value.
 */
typedef struct odin3_elab_entry {
    uint8_t kind; /* odin3_elab_entry_kind */
    odin3_ast_id decl;
    uint32_t width;
    bool is_signed;
    odin3_cval value;
} odin3_elab_entry;

/* The separator of an override path's components (spec v4.7): the unit separator, which no
 * identifier holds, so an escaped `\a.b` is one component; names and messages print '.'. */
#define ODIN3_ELAB_PATH_SEP '\x1f'

/* In place over len bytes: _print turns each ODIN3_ELAB_PATH_SEP into '.', _join each '.' into
 * ODIN3_ELAB_PATH_SEP (a project scope's components). */
void odin3_elab1_path_print(char *data, size_t len);
void odin3_elab1_path_join(char *data, size_t len);

/* Where an override comes from, in precedence order (EL-8: PROJECT is the highest). */
typedef enum odin3_elab_ovr_source {
    ODIN3_OVR_PROJECT = 1,
    ODIN3_OVR_NAMED,
    ODIN3_OVR_DEFPARAM,
    ODIN3_OVR_ORDERED
} odin3_elab_ovr_source;

/*
 * One override of a parameter: path is the strtab ID of "W" (a parameter of the target) or of
 * "u2<SEP>W" (pending: for the instance u2 below it, pushed down, EL-8; ODIN3_ELAB_PATH_SEP joins
 * the components); source an odin3_elab_ovr_source; order the stream order among overrides of one
 * source (the DEFPARAM node's ID, so the first defparam wins; a project override's index in the
 * record's params); value the value as evaluated where it was written (self-determined, not yet
 * converted, EL-16; sized, its sign kept: v4.7, as Yosys passes overrides); at where to report it
 * (the CONNECTION, the defparam's HIER_NAME, the project line); from the node that wrote it (the
 * CONNECTION or DEFPARAM; 0 for a project line). base (added to the brief's fields): the NUMBER
 * sub-kind of the source literal of a value that is still unsized, for point (bb)'s spelling of
 * the final value (since v4.7 only a default is).
 */
typedef struct odin3_elab_override {
    uint32_t path;
    uint8_t source; /* odin3_elab_ovr_source */
    uint32_t order;
    odin3_cval value;
    odin3_loc at;
    odin3_ast_id from;
    uint8_t base; /* the NUMBER sub (ODIN3_AST_NUMBER_*) of an unsized value's literal */
} odin3_elab_override;

/*
 * One decision of pass E, replayed by pass W in order (§3.4): one per INSTANCE of a module item
 * (an instance array included), its value the child specialization (>= 0; a module or a
 * `(* blackbox *)` definition), ODIN3_DECISION_GIVEN_BASE − k for a cell-type or implicit
 * black-box instance whose parameters are odin3_elab1.given record k (Task 12), or
 * ODIN3_DECISION_SKIPPED (a failed one). Task 14 adds the generate decisions: a GEN_IF's 0
 * (then), 1 (else) or ODIN3_DECISION_SKIPPED (none); a GEN_CASE's item index or
 * ODIN3_DECISION_SKIPPED; a GEN_FOR's iteration count n, then n entries of its genvar values.
 */
typedef struct odin3_elab_decision {
    odin3_ast_id node;
    int32_t value;
} odin3_elab_decision;

enum { ODIN3_DECISION_SKIPPED = -1, ODIN3_DECISION_GIVEN_BASE = -3 };

/* A specialization asked for: the definition, its overrides (any order), its depth below the
 * top. */
typedef struct odin3_elab_spec_req {
    uint32_t def;
    const odin3_elab_override *ovr;
    uint32_t n_ovr;
    uint32_t depth;
} odin3_elab_spec_req;

/*
 * The values of req->def's `parameter`s (odin3_elab_def.first_param .., header ones first) with
 * req's overrides applied by EL-8's precedence, each evaluated lazily in declaration order in the
 * definition's environment and converted to its declared type per EL-16 (odin3_cval_to_param),
 * into values[0 .. n_header_params + n_body_params) (planes in e1's scratch arena, valid until
 * the next call); the overrides whose path goes deeper ("u2<SEP>W") are copied to pending[0 ..
 * *n_pending) (room for req->n_ovr). A defparam that loses to a named override warns ("defparam
 * of 'u1.W' is overridden by the named parameter assignment of 'u1'", at its target, once).
 * Errors (§9.1, at the override or the declaration): "'L' is a localparam of module 'm' and
 * cannot be overridden", "module 'm' has no parameter 'X'", the defparam and project texts for an
 * unknown target, "parameter 'P' depends on itself", "parameter 'P' is not constant (it reads
 * 'x')", P4's "parameter type 'real' is not supported". ODIN3_OK; ODIN3_ERR_PARSE after the
 * diagnostics; ODIN3_ERR_NO_MEMORY; a check status for a 2D bug.
 */
odin3_status odin3_elab1_params(odin3_elab1 *e1, const odin3_elab_spec_req *req, odin3_cval *values,
                                odin3_elab_override *pending, uint32_t *n_pending);

/*
 * The specialization of req (EL-9), found by its key (the definition, each parameter's width,
 * flags and bits, the pending overrides sorted by path, one per path: the highest precedence) or
 * created: depth req->depth, named per EL-9 and point (q) (the first one made, the top's, keeps
 * its definition's name: v4.7), appended to the FIFO queue. Caps (point
 * (aa)): more specializations than the cap → "elaboration makes more than N specializations" and a
 * depth past ODIN3_ELAB_MAX_HIER_DEPTH → "instance hierarchy deeper than N at 'u'; recursive
 * instantiation of 'm'?", both at the instance being specialized (odin3_elab1.creating), both
 * fatal. ODIN3_OK with *spec; ODIN3_ERR_PARSE after the diagnostics; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_spec_get(odin3_elab1 *e1, const odin3_elab_spec_req *req, uint32_t *spec);

/*
 * Pass E of specialization spec (§3.4): its environment (every declaration of the module scope
 * with its folded range, every parameter and localparam evaluated), its defparams resolved and
 * pushed down (EL-8), its pending overrides checked against its instances, and each instance
 * resolved (EL-6) and specialized (odin3_elab1_spec_get), with one decision-log entry per
 * INSTANCE. The environment stays for pass W (odin3_elab1_write). ODIN3_OK (errors are counted);
 * ODIN3_ERR_NO_MEMORY; ODIN3_ERR_CHECK.
 */
odin3_status odin3_elab1_expand(odin3_elab1 *e1, uint32_t spec);

/*
 * Pass W of specialization spec: replays its decision log in order and writes its MODULE
 * through the 2A builders (EL-4: the header parameters as PARAM_DECL[localparam]{IN_HEADER} with
 * their final values, spelled per point (bb); ANSI PORT_DECLs, one declarator each; NET_DECL and
 * VAR_DECL with NUMBER bounds; CONT_ASSIGN; INSTANTIATIONs named after their specializations,
 * with one named CONNECTION per parameter and per port; Task 12: ordered connections resolved,
 * instance and gate arrays expanded with sliced connections, cell-type and black-box instances
 * with their parameters, GATE_DECL), left on the store's pending stack.
 * Pending kinds are reported (odin3_elab_pend_report) and nothing is built after any error. A
 * log not consumed exactly is "stage 1: decision log of 'm' not replayed (N of M entries)" and
 * ODIN3_ERR_CHECK (a 2D bug, §16 item 4). The elaborated-node cap applies at every make
 * ("elaboration needs more than N elaborated nodes", fatal). Frees pass E's environment.
 * ODIN3_OK; ODIN3_ERR_NO_MEMORY; ODIN3_ERR_CHECK; a builder's status.
 */
odin3_status odin3_elab1_write(odin3_elab1 *e1, uint32_t spec);

/*
 * §3.4: the elaborated store (e1->elab), the top's specialization (the project's parameters as
 * its overrides), then passes E and W of each specialization in FIFO order, the acyclicity check
 * of the specialization graph (an iterative DFS: "recursive instantiation of 'm' (through 'a')"),
 * the not-elaborated info line and, without errors, the UNIT named after the top's
 * specialization. ODIN3_OK (errors are counted); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_CHECK.
 */
odin3_status odin3_elab1_run_queue(odin3_elab1 *e1, uint32_t top_def);

/* --- instances, connections, arrays, black boxes, cell types (Task 12, elab1_inst.c) ------- */

/*
 * One instance being elaborated: its INSTANTIATION (or GATE_DECL), its INSTANCE, the scope it is
 * in (0, the module scope; Task 14 adds the generate scopes), what its module name resolved to
 * (EL-6; kind ODIN3_RES_NONE for a gate) and spec: for a module or a `(* blackbox *)` definition
 * the child specialization, for a cell type or an implicit black box its parameter record
 * (odin3_elab1.given, once made), else UINT32_MAX.
 */
typedef struct odin3_elab_inst {
    odin3_ast_id instantiation;
    odin3_ast_id instance;
    uint32_t scope;
    odin3_elab_res target;
    uint32_t spec;
} odin3_elab_inst;

/*
 * One connection as odin3_elab1_connect resolved it (odin3_elab1.conns): for a module, one per
 * named port in port order (conn 0: unconnected); for a cell type, a black box or a gate, one
 * per connection as written. name is the port's (0 for an ordered connection to a type port the
 * strtab has no ID for, a gate terminal, an implicit black box's ordered value); dir an
 * odin3_dir; pos the port's position (UINT32_MAX: none). odin3_elab1_inst_array fills the
 * array plan: port_width P (0: unknown, the expression goes whole), width W (the expression's
 * self-determined width) and sliced (W = N × P: each element gets its P bits).
 */
typedef struct odin3_elab1_conn {
    uint32_t name;
    uint8_t dir;
    uint32_t pos;
    odin3_ast_id conn;
    odin3_ast_id expr;
    uint32_t port_width;
    uint32_t width;
    bool sliced;
} odin3_elab1_conn;

/* An instance array's range as folded (odin3_elab1.array): elements left … right, n of them. */
typedef struct odin3_elab1_array {
    int32_t left;
    int32_t right;
    uint32_t n;
} odin3_elab1_array;

/*
 * One parameter of a cell-type or implicit black-box instance (EL-4: "the overrides as given,
 * values resolved"): its name (0: an ordered value of an implicit black box), the override that
 * won (EL-8; its value as evaluated, sized, with its literal's base, at, and from: the #()
 * CONNECTION or DEFPARAM, 0 for a project line), and, for a cell type, the type's parameter kind
 * (odin3_value_kind) with the converted integer of an INT; as_given for an implicit black box's
 * values, written as evaluated.
 */
typedef struct odin3_elab1_gparam {
    uint32_t name;
    bool as_given;
    uint8_t kind;
    int64_t num;
    odin3_elab_override ovr;
} odin3_elab1_gparam;

/* The parameters of one such instance: odin3_elab1.gparams[first .. + count), and its target. */
typedef struct odin3_elab1_given {
    odin3_elab_res target;
    uint32_t first;
    uint32_t count;
} odin3_elab1_given;

/*
 * A port of a specialization as stage 1 evaluates it (odin3_elab1_spec_ports, black boxes): its
 * name (0 for an empty position), direction (odin3_dir), width with its declared range and sign;
 * width 0 when the declaration could not be evaluated (reported where it is elaborated); decl,
 * the parsed node it comes from (an ANSI DECLARATOR or a PORT_REF; 0 for an implicit port).
 */
typedef struct odin3_elab1_port {
    uint32_t name;
    uint8_t dir;
    uint32_t width;
    int64_t msb;
    int64_t lsb;
    bool is_signed;
    bool scalar;
    odin3_ast_id decl;
} odin3_elab1_port;

/*
 * A black box stage 1 met (EL-11; elab1_bbox.c): a `(* blackbox *)` specialization (spec, name the
 * specialization's) or an implicit black box (--auto-blackbox; spec UINT32_MAX, at its first
 * INSTANTIATION), with its ports in order through odin3_elab1.bbports (first_port + 1 … chained
 * by next; 0 = none). Each is written as a ports-only black-box MODULE (EL-4, v4.8).
 */
typedef struct odin3_elab1_bbox {
    uint32_t name;
    uint32_t spec;
    bool implicit;
    odin3_ast_id at;
    uint32_t first_port;
    uint32_t last_port;
    uint32_t n_ports;
} odin3_elab1_bbox;

/* One port of a black box: the port as evaluated (an implicit one: inout, the width of its first
 * connection with an expression, 0 until one), where it was first seen, the next port + 1. */
typedef struct odin3_elab1_bbport {
    odin3_elab1_port port;
    odin3_ast_id at;
    uint32_t next;
} odin3_elab1_bbport;

/*
 * EL-10: inst's connections resolved into odin3_elab1.conns (see odin3_elab1_conn): one per port
 * in port order for a module, a `(* blackbox *)` definition or an implicit black box (the union of
 * its ports, EL-4 v4.8), as written for a cell type or a gate. For a module or a `(* blackbox *)`
 * definition, ordered connections map to positions (a connection with
 * an expression to an empty header position warns "connection N of instance 'u' goes to an unnamed
 * port position and is ignored", once) and named ones to ports by name; a cell type's ports are
 * its type's. Errors (§9.1): "instance 'u' mixes ordered and named connections" (at the INSTANCE),
 * "instance 'u' of 'm' has N connections but 'm' has M ports" (at the INSTANCE), "module 'm' has
 * no port 'p'" ("cell type 't' has no port 'p'"), "port 'p' of instance 'u' is connected twice",
 * and for an output or inout port (I3) "output port 'p' of instance 'u' must be connected to a
 * net" / "… is connected to a constant" (`inout port` for an inout), each at the CONNECTION. A
 * gate's terminals and an implicit black box's connections are taken as written. ODIN3_OK;
 * ODIN3_ERR_PARSE after a diagnostic; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_connect(odin3_elab1 *e1, const odin3_elab_inst *inst);

/*
 * EL-10 instance arrays (I6, Q5): inst's range folded into odin3_elab1.array (elements left …
 * right, Yosys's names u[left] … u[right]) and each connection of odin3_elab1.conns (resolved
 * first) planned: of width W against a port of P bits (gates: P = 1) and N elements, W = P goes
 * whole to every element, W = N × P is sliced (the element at index i gets bits
 * [(i − min(left, right)) × P +: P]: the lowest index the low bits whatever the range's
 * direction, EL-10, as Yosys 0.55 normalizes the range), anything else is
 * "connection of 'p' to instance array 'u' is W bits; expected P or N×P" ("connection K of
 * instance array 'g'" for a gate terminal) at the CONNECTION. Widths are self-determined, typed
 * over pass E's environment. ODIN3_OK; ODIN3_ERR_PARSE after a diagnostic; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_inst_array(odin3_elab1 *e1, const odin3_elab_inst *inst);

/*
 * EL-8 cell-type targets and EL-11: the parameters of a cell-type or implicit black-box instance
 * from odin3_elab1.inst_ovr (its #() values, then the defparams and project lines that reach it),
 * one per name by EL-8's precedence (a lost defparam warns as for a module), appended to
 * odin3_elab1.given as one record. A cell type's ordered values map to its parameters in
 * declaration order; each name must be one of its parameters ("cell type 't' has no parameter
 * 'P'"; a project line: "param 'top.u.P' does not name a parameter"; more ordered values than
 * parameters: "too many parameter values for cell type 't' (N given, M parameters)"), and each
 * value is converted to the parameter's kind (INT: a signed 64-bit integer, else "value of
 * parameter 'P' of cell type 't' is not an integer"; BITS: the bits; STRING: the value). Stage 2
 * infers the parameters not given (odin3_celltype_infer_params). An implicit black box keeps the
 * values as given (ordered ones name 0). A path deeper than one name is the defparam (or project)
 * target error: a cell type has no hierarchy. ODIN3_OK; ODIN3_ERR_PARSE after a diagnostic;
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_celltype_params(odin3_elab1 *e1, const odin3_elab_inst *inst);

/*
 * EL-11 for a `(* blackbox *)` definition's default specialization spec (elab1_bbox.c; v4.10:
 * the definition keeps its name, as Yosys 0.55's module): the info line "body of black box 'm' is
 * ignored" once per definition when its body holds anything but port and parameter declarations,
 * and the ports, evaluated with the default parameter values (errors reported), recorded in the
 * black-box table and, without errors so far, written as the definition's one ports-only
 * black-box MODULE (EL-4, v4.8) on the store's pending stack. ODIN3_OK; ODIN3_ERR_PARSE after a
 * diagnostic; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_blackbox(odin3_elab1 *e1, uint32_t spec);

/*
 * After every specialization (EL-4 v4.8): each implicit black box, in first-use order, written as
 * its ports-only black-box MODULE (inout ports at their EL-11 widths, 1 bit when no connection
 * gave one) on the store's pending stack; nothing after an error. ODIN3_OK; ODIN3_ERR_NO_MEMORY;
 * ODIN3_ERR_PARSE after the elaborated-node cap's fatal error.
 */
odin3_status odin3_elab1_write_implicit(odin3_elab1 *e1);

/*
 * EL-11 --auto-blackbox: inst of an implicit black box (elab1_bbox.c). The black box's ports, made
 * when its name is first met, are the union of the names every instantiation of the name in the
 * sources connects (all inout, in source order; a black box's own body excluded); the first
 * connection with an expression fixes a port's width, and a later one of another width is "implicit
 * black box 'm': port 'a' is P bits at its first instance; this connection is W bits" (an array
 * element's slice counts). An ordered connection is "implicit black box 'm' needs named
 * connections" (at the INSTANCE). ODIN3_OK; ODIN3_ERR_PARSE; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_auto_blackbox(odin3_elab1 *e1, const odin3_elab_inst *inst);

/* The black-box table (internal): entry idx and port idx (NULL past the count); the implicit black
 * box named name + 1 (0: none); the port of box named name + 1 (0: none). */
const odin3_elab1_bbox *odin3_elab1_bbox_at(const odin3_elab1 *e1, uint32_t idx);
const odin3_elab1_bbport *odin3_elab1_bbport_at(const odin3_elab1 *e1, uint32_t idx);
uint32_t odin3_elab1_implicit_box(const odin3_elab1 *e1, uint32_t name);
uint32_t odin3_elab1_bbox_port(const odin3_elab1 *e1, uint32_t box, uint32_t name);

/* Every port of specialization spec into ports (room for its definition's positions), evaluated
 * in a fresh environment with its parameter values; quiet drops the diagnostics. The worst
 * status (ODIN3_ERR_PARSE when a port failed). */
odin3_status odin3_elab1_eval_ports(odin3_elab1 *e1, uint32_t spec, bool quiet,
                                    odin3_elab1_port *ports);

/*
 * A NUMBER or STRING leaf of the elaborated store at from's {loc, end} (pass W's spellings, point
 * (bb)): val's (a STRING; a NUMBER of sub base while unsized, else BIN, SIGNED per value), or, with
 * val NULL, num in the 32-bit integer form (DEC, SIGNED, payload width 32). Scratch bits in arena.
 * The elaborated-node cap is checked first (odin3_elab1_node_room). ODIN3_OK; ODIN3_ERR_PARSE
 * after the cap's fatal error; ODIN3_ERR_NO_MEMORY.
 */
typedef struct odin3_elab1_leaf {
    const odin3_cval *val;
    uint8_t base;
    int64_t num;
    odin3_ast_id from;
} odin3_elab1_leaf;
odin3_status odin3_elab1_make_leaf(odin3_elab1 *e1, odin3_arena *arena,
                                   const odin3_elab1_leaf *leaf, odin3_ast_id *out);

/* The elaborated-node cap before a make (point (aa)): false after its fatal error at from. */
bool odin3_elab1_node_room(odin3_elab1 *e1, odin3_ast_id from);

/*
 * The ports of specialization spec (one per position of its definition), evaluated quietly once
 * with its parameter values and kept in the run arena; NULL with *n 0 out of memory (latched).
 */
const odin3_elab1_port *odin3_elab1_spec_ports(odin3_elab1 *e1, uint32_t spec, uint32_t *n);

/*
 * The instance cap (point (aa)): n more instance or gate nodes, at holder (an INSTANTIATION or
 * GATE_DECL): "elaboration makes more than N instances", fatal, past the cap. True when they fit.
 */
bool odin3_elab1_count_instances(odin3_elab1 *e1, odin3_ast_id holder, uint64_t n);

/* An INSTANCE's array range folded (range bounds, 32-bit), n = |left − right| + 1. ODIN3_OK;
 * ODIN3_ERR_PARSE after a diagnostic; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_fold_array(odin3_elab1 *e1, odin3_ast_id instance, odin3_elab1_array *out);

/* A connection expression's self-determined width, typed over pass E's environment (an
 * undeclared name is an implicit 1-bit net, EL-25). ODIN3_OK; ODIN3_ERR_PARSE after a
 * diagnostic; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_expr_width(odin3_elab1 *e1, odin3_ast_id expr, uint32_t *width);

/* Whether expr is a constant in pass E's environment, quietly (odin3_ceval_try; an expression
 * reading an undeclared name or a scope reference is not), with its value in *val. ODIN3_OK;
 * the evaluator's errors (caps) as PARSE after their diagnostic; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_try_constant(odin3_elab1 *e1, odin3_ast_id expr, odin3_cval *val,
                                      bool *is_const);

/* The self-determined width of node, a subexpression of the last expression typed (0: none). */
uint32_t odin3_elab1_typed_width(const odin3_elab1 *e1, odin3_ast_id node);

/* The decision value of a cell-type or implicit black-box instance with given record idx, and
 * back (ODIN3_DECISION_GIVEN_BASE − idx). */
int32_t odin3_elab1_given_decision(uint32_t idx);
uint32_t odin3_elab1_given_of(int32_t decision);

/* --- internals (elab1*.c only) ------------------------------------------------------------ */

/* Why a MODULE's name is taken (EL-6), recorded by collect_defs and reported when reached. */
typedef enum odin3_elab1_taken {
    ODIN3_TAKEN_NONE = 0,
    ODIN3_TAKEN_HARD,     /* a hard cell type (tech library) */
    ODIN3_TAKEN_BLACKBOX, /* a black-box cell type (a model a BLIF read declared) */
    ODIN3_TAKEN_MODULE,   /* a module of an earlier read: duplicate_module */
    ODIN3_TAKEN_BUILTIN   /* a built-in (or plugin) word, bit or port cell type */
} odin3_elab1_taken;

/* Per definition, beside odin3_elab_def: its INSTANTIATIONs, its name chain and its state. */
typedef struct odin3_elab1_defx {
    uint32_t first_inst; /* odin3_elab1.insts[first_inst .. + n_inst) */
    uint32_t n_inst;
    uint32_t next;            /* the next definition of the same name + 1, 0 = none */
    uint8_t taken;            /* odin3_elab1_taken */
    bool reached;             /* reached from the top (odin3_elab1_reach_def) */
    bool ports_indexed;       /* its port names are in odin3_elab1.port_pos, dirs in port_dirs */
    bool reported;            /* its Q7 (a library unit's UDP) or taken-name error is logged */
    bool body_noted;          /* a black box's "body … is ignored" info is logged (Task 12) */
    bool bb_default_asked;    /* a black box's default specialization was requested (EL-11) */
    bool scanned;             /* its body went through odin3_elab1_scan (Task 16) */
    bool scan_failed;         /* … and the scan reported an error: passes E and W skip it */
    const uint8_t *port_dirs; /* odin3_dir per port position (run arena; Task 12) */
    uint32_t bb_name;         /* a black box: the name all its specializations share; 0 = none */
} odin3_elab1_defx;

/* One specialization (EL-9): everything lives in the run arena. */
typedef struct odin3_elab1_spec {
    uint32_t def;
    uint32_t name; /* strtab ID */
    uint32_t depth;
    uint64_t hash;
    uint32_t next_same_hash; /* + 1, 0 = none */
    const uint8_t *key;
    uint32_t key_len;
    const odin3_cval *values; /* one per `parameter` of the definition */
    const uint8_t *bases;     /* the NUMBER sub of each unsized value's literal */
    uint32_t n_values;
    const odin3_elab_override *pending; /* sorted by path */
    uint32_t n_pending;
    uint32_t first_log; /* odin3_elab1.log[first_log .. + n_log) */
    uint32_t n_log;
    uint32_t first_edge; /* odin3_elab1.edges[first_edge .. + n_edges) */
    uint32_t n_edges;
    const odin3_elab1_port *ports; /* odin3_elab1_spec_ports, once computed (Task 12) */
    uint32_t n_ports;
    bool ports_done;
    bool dflt; /* every parameter has its default value and nothing is pending */
} odin3_elab1_spec;

/* One edge of the specialization graph: an instance of spec `from` specialized as `to`. */
typedef struct odin3_elab1_edge {
    uint32_t from;
    uint32_t to;
    odin3_ast_id inst;
} odin3_elab1_edge;

/* The instance being specialized (for the caps' locations); 0 for the top. */
typedef struct odin3_elab1_creating {
    odin3_ast_id inst;   /* the INSTANCE */
    odin3_ast_id holder; /* the INSTANTIATION (the specialization cap's location) */
    uint32_t parent_def; /* the definition holding it (the depth message's module) */
} odin3_elab1_creating;

/* The state of an environment entry's value or range (pass E is lazy, EL-16). */
typedef enum odin3_elab1_state {
    ODIN3_E1_NEW = 0,
    ODIN3_E1_BUSY,
    ODIN3_E1_DONE,
    ODIN3_E1_FAILED,
    ODIN3_E1_NO_MEMORY /* failed for want of memory: a later read answers NO_MEMORY, not PARSE */
} odin3_elab1_state;

/* One entry of an environment, with what pass E needs beside odin3_elab_entry. */
typedef struct odin3_elab1_slot {
    odin3_elab_entry entry;
    odin3_ast_id group; /* the declaration holding decl (PARAM_DECL, NET_DECL, …, INSTANTIATION) */
    uint32_t name;
    uint32_t scope;
    uint8_t state; /* odin3_elab1_state */
    uint8_t base;  /* the NUMBER sub of an unsized value's literal */
    bool overridden;
    bool bound;          /* GENVAR: holds a generate loop's value (entry.value, Task 14) */
    uint32_t n_unpacked; /* SIGNAL: the unpacked dimensions (a memory) */
    odin3_ast_id group2; /* SIGNAL: the NET_DECL/VAR_DECL merged into a body PORT_DECL (EL-24), or
                            the PORT_DECL merged into a net; 0 when none (Task 15) */
    odin3_ast_id decl2;  /* its DECLARATOR */
    int64_t msb;         /* the declared range; an unranged value: width - 1 and 0 */
    int64_t lsb;
    odin3_elab_override ovr; /* PARAM: the override that won */
} odin3_elab1_slot;

/* One frame of pass W's expression copier: a parsed node, its next child and the mark of the
 * store's pending stack its children start at. */
typedef struct odin3_elab1_frame {
    uint32_t node;
    uint32_t next;
    uint32_t mark;
} odin3_elab1_frame;

/* A folded RANGE of a declaration. */
typedef struct odin3_elab1_range {
    odin3_ast_id node;
    uint8_t state; /* odin3_elab1_state */
    int64_t msb;
    int64_t lsb;
} odin3_elab1_range;

/* --- generate scopes (Task 14; EL-19–EL-22) ------------------------------------------------ */

typedef enum odin3_elab_scope_kind {
    ODIN3_ESCOPE_MODULE = 0,
    ODIN3_ESCOPE_BLOCK,
    ODIN3_ESCOPE_LOCAL /* a function, task or block that declares parameters (Task 15b) */
} odin3_elab_scope_kind;

/*
 * One scope of an environment: the module (scope 0) or one elaborated generate block (one per
 * selected branch, one per loop iteration). name is the block's name (the user's or EL-21's
 * `genblk<n>`; 0 for the module), index the iteration's genvar value when has_index, parent the
 * enclosing scope (UINT32_MAX for the module), counter label_genblks's counter of the scope: the
 * GEN_IF, GEN_FOR and GEN_CASE constructs met so far in its pre-order walk. Each scope's set of
 * `genblk*` names seen so far is internal to elab1_gen.c, copied from the parent on entry.
 */
typedef struct odin3_elab_scope {
    uint8_t kind; /* odin3_elab_scope_kind */
    uint32_t name;
    int32_t index;
    bool has_index;
    uint32_t parent;
    uint32_t counter;
} odin3_elab_scope;

/* Where a scope's items are, beside odin3_elab_scope: child slot of holder (a GEN_BLOCK, a bare
 * item or a NULL_STMT), and the seen-set sequence number at its creation (its copy). */
typedef struct odin3_elab1_scope_x {
    odin3_ast_id holder;
    uint32_t slot;
    uint32_t seq;
} odin3_elab1_scope_x;

/* One component of a hierarchical path: a name and an optional index. */
typedef struct odin3_elab1_comp {
    uint32_t name;
    int32_t index;
    bool has_index;
} odin3_elab1_comp;

/* An environment (EL-12): the scopes of one definition's elaboration (Task 14: the module and its
 * elaborated generate blocks), their declarations (slots, by scope << 32 | name), the folded
 * ranges (by scope << 32 | RANGE) and the evaluator whose host it is. */
/* One port of a module as pass E merged it (EL-24): the header position's declaration and the body
 * declarations of its name. name 0: an empty position (EL-10). */
typedef struct odin3_elab1_mport {
    uint32_t name;
    uint8_t dir; /* ODIN3_AST_PORT_DECL_* */
    uint8_t dt;  /* ODIN3_AST_DT_*: none, reg or integer */
    uint8_t net; /* 1 + the net kind of a net port, else 0 (a variable) */
    bool is_signed;
    bool ansi;
    bool as_integer;    /* an `input integer`, treated as a signed [31:0] net (M12) */
    odin3_ast_id range; /* the RANGE that gives the port's type, or 0 */
    odin3_ast_id group; /* the PORT_DECL (its attributes and location) */
    odin3_ast_id decl;  /* the DECLARATOR, or the empty position's PORT_REF */
} odin3_elab1_mport;

/* An implicit net (EL-25): its name and the first use. */
typedef struct odin3_elab1_implicit_net {
    uint32_t name;
    odin3_ast_id use;
} odin3_elab1_implicit_net;

/* A frame of the implicit-net scan: a node, or the end of a scope that node opened. */
typedef struct odin3_elab1_scan_frame {
    odin3_ast_id node;
    bool leave;
} odin3_elab1_scan_frame;

typedef struct odin3_elab1_env {
    odin3_elab1 *e1;
    uint32_t def;
    odin3_ast_id module;
    odin3_ceval *ev;
    odin3_arena *arena;   /* values and ranges evaluated here */
    odin3_vec slots;      /* odin3_elab1_slot */
    odin3_u64map *names;  /* scope << 32 | name → slot index */
    odin3_vec ranges;     /* odin3_elab1_range */
    odin3_u64map *ranged; /* RANGE node → index into ranges */
    bool quiet;           /* diagnostics dropped (the default-value comparison of EL-9) */
    odin3_ctype *types;   /* the connection typer over this environment (Task 12), lazily made */
    /* Task 14 */
    odin3_vec scopes;    /* odin3_elab_scope; scope 0 is the module */
    odin3_vec scope_x;   /* odin3_elab1_scope_x, one per scope */
    uint32_t cur_scope;  /* where lookups start and declarations are recorded */
    odin3_u64map *seen;  /* scope << 32 | genblk* name → the sequence number it was seen at */
    odin3_u64map *kids;  /* scope << 32 | block name → its first elaborated scope + 1 */
    odin3_u64map *iters; /* first scope << 32 | (uint32_t)index → the iteration's scope + 1 */
    uint32_t seq;        /* the seen-set clock */
    uint32_t made_first; /* the scopes the last odin3_elab1_gen_* made: [first, first + n) */
    uint32_t made_n;
    /* Task 15 */
    odin3_vec ports;         /* odin3_elab1_mport: the merged port list (odin3_elab1_ports) */
    odin3_vec implicit;      /* odin3_elab1_implicit_net, in first-use order */
    odin3_u64map *imp_names; /* implicit (or reported-undeclared) names → 1 */
    odin3_u64map *locals;    /* names of the enclosing blocks and subroutines → depth count */
    odin3_vec scan;          /* odin3_elab1_scan_frame: the implicit-net scan's stack */
    uint8_t nettype;         /* the module's `default_nettype (odin3_elab1_nettype_at) */
} odin3_elab1_env;

/* A new environment of definition def with every declaration of its module scope recorded (no
 * value evaluated yet). ODIN3_ERR_NO_MEMORY (*out NULL). */
odin3_status odin3_elab1_env_create(odin3_elab1 *e1, uint32_t def, odin3_elab1_env **out);
void odin3_elab1_env_destroy(odin3_elab1_env *env);

/* The specialization spec's parameter values as the winning overrides of env (pass E's start). */
void odin3_elab1_env_preload(odin3_elab1_env *env, uint32_t spec);

/* A declaration of env->cur_scope (proto->scope): the first of a name wins; a generate block's
 * name that clashes with another declaration of the scope is M11's error (EL-21). NO_MEMORY. */
odin3_status odin3_elab1_env_add(odin3_elab1_env *env, const odin3_elab1_slot *proto);

/* The slot index of name in scope, UINT32_MAX when none. */
uint32_t odin3_elab1_env_find(const odin3_elab1_env *env, uint32_t scope, uint32_t name);

/* The slot index of name from env->cur_scope outward to the module (EL-22), UINT32_MAX when none.
 */
uint32_t odin3_elab1_env_lookup(const odin3_elab1_env *env, uint32_t name);
odin3_elab1_slot *odin3_elab1_env_slot(odin3_elab1_env *env, uint32_t idx);

/* The value of PARAM or LOCALPARAM slot idx (EL-16), evaluated on first use; the SIGNAL type of a
 * net, variable or port (its range folded). ODIN3_ERR_PARSE after a diagnostic (or silently for
 * a slot that failed before). */
odin3_status odin3_elab1_env_value(odin3_elab1_env *env, uint32_t idx);

/* The folded bounds of a declaration's RANGE (each a constant, 32-bit; `range bound`). */
odin3_status odin3_elab1_env_range(odin3_elab1_env *env, odin3_ast_id range, int64_t *msb,
                                   int64_t *lsb);

/* A constant expression's value, self-determined, in the environment's arena (what: §9.1's
 * <what>); *base the NUMBER sub of an unsized value's literal (NULL: not wanted). */
typedef struct odin3_elab1_eval_req {
    odin3_ast_id expr;
    const char *what;
    uint8_t *base;
} odin3_elab1_eval_req;
odin3_status odin3_elab1_eval(odin3_elab1_env *env, const odin3_elab1_eval_req *req,
                              odin3_cval *out);

/* The same with the value and the evaluator's temporaries in arena (a scratch arena of
 * ODIN3_E1_TMP_CHUNK bytes per chunk, freed by the caller): what a value that outlives the call
 * is not. Lazily evaluated parameters still live in the environment's arena. */
enum { ODIN3_E1_TMP_CHUNK = 2048 };
odin3_status odin3_elab1_eval_in(odin3_elab1_env *env, odin3_arena *arena,
                                 const odin3_elab1_eval_req *req, odin3_cval *out);

/* A constant folded to a 32-bit signed integer (point (bb)): x or z bits are "<what> has x or z
 * bits", a value outside int32 the width error. */
odin3_status odin3_elab1_fold_int(odin3_elab1_env *env, odin3_ast_id expr, const char *what,
                                  int32_t *out);

/* The NUMBER sub-kind an unsized value of expr keeps (point (bb)): a NUMBER's own, a parameter's
 * base, else DEC. */
uint8_t odin3_elab1_value_base(odin3_elab1_env *env, odin3_ast_id expr);

/* What odin3_elab1_params gives, with the bases of unsized values and EL-9's "differs from its
 * default" per parameter (the name lists those), all in e1->scratch (renewed by each call). */
typedef struct odin3_elab1_params_out {
    odin3_cval *values;
    uint8_t *bases;
    bool *differ;
    odin3_elab_override *pending;
    uint32_t n_pending;
} odin3_elab1_params_out;

odin3_status odin3_elab1_params_ex(odin3_elab1 *e1, const odin3_elab_spec_req *req,
                                   odin3_elab1_params_out *out);

/*
 * A value's flags as the specialization key sees them (EL-9: width, signedness, string flag and
 * bits): UNSIZED counts only where it changes the value's meaning, an unsigned value whose top bit
 * is 1 (it extends with ones, a sized one with zeros); elsewhere the extensions agree, so
 * `#(.W(8))` and `#(.W(32'sd8))` on `parameter integer W` share (agent default, Task 11).
 */
uint8_t odin3_elab1_key_flags(const odin3_cval *val);

/* Equal for the key: width, key flags and both planes. */
bool odin3_elab1_same_value(const odin3_cval *lhs, const odin3_cval *rhs);

/* EL-8's precedence: true when lhs takes precedence over rhs (by source, then stream order). */
bool odin3_elab1_override_beats(const odin3_elab_override *lhs, const odin3_elab_override *rhs);

/* A defparam that loses to a named override warns, once, at its target (EL-8). */
void odin3_elab1_override_lost(odin3_elab1 *e1, const odin3_elab_override *loser,
                               const odin3_elab_override *winner);

/* The unknown-target error of a pushed-down override (a defparam's or a project line's). */
void odin3_elab1_bad_target(odin3_elab1 *e1, const odin3_elab_override *ovr);

/* Point (l)'s line for kind at loc (a project line: no node), once per kind, counted. */
void odin3_elab1_pend_at(odin3_elab1 *e1, odin3_loc loc, uint32_t kind);

/* The full dotted text of a defparam's or project override's original path (for messages). */
odin3_status odin3_elab1_override_text(const odin3_elab1 *e1, const odin3_elab_override *ovr,
                                       odin3_strbuf *out);

/* The strtab text of id. */
const char *odin3_elab1_str(const odin3_elab1 *e1, uint32_t id);

/* The specialization spec (NULL past the count). */
const odin3_elab1_spec *odin3_elab1_spec_at(const odin3_elab1 *e1, uint32_t spec);

/* --- generate (Task 14; elab1_gen.c) -------------------------------------------------------- */

/*
 * Pass E of one generate construct in scope (EL-19, EL-20, EL-21): its constant parts evaluated in
 * scope, its decision logged (odin3_elab_decision), and the generate block(s) it selects made as
 * new scopes, child of scope, named by EL-21 (env->made_first, made_n; a loop's iterations in
 * order, each binding the genvar to its value). The construct's expressions enter scope's seen set
 * in Yosys's pre-order. ODIN3_OK after a diagnostic too (nothing is selected, the log says so);
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_gen_for(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);
odin3_status odin3_elab1_gen_if(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);
odin3_status odin3_elab1_gen_case(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);

/* label_genblks (EL-21): `genblk<counter>`, zeros inserted after `genblk` while the name is in the
 * seen set of scope; interned into *name. ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_genblk_name(odin3_elab1 *e1, uint32_t scope, uint32_t counter,
                                     uint32_t *name);

/* G7: hier_name (a HIER_NAME) read in scope resolved through the module's generate blocks to a
 * declaration of the reached scope; E21 "hierarchical reference 'u.w' is not supported", "'g[5].t'
 * does not exist (generate block 'g[5]' was not elaborated)" or "'b.x' is not declared"
 * otherwise. ODIN3_ERR_PARSE after the diagnostic; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_scope_ref(odin3_elab1 *e1, odin3_ast_id hier_name, uint32_t scope);

/* odin3_elab1_scope_ref from env->cur_scope, the declaration's slot into *slot (diagnostics
 * dropped while env->quiet). */
odin3_status odin3_elab1_scope_target(odin3_elab1_env *env, odin3_ast_id hier_name, uint32_t *slot);

/* The scopes of a new environment (scope 0, the module) and their tables; freed by _gen_free. */
odin3_status odin3_elab1_gen_init(odin3_elab1_env *env);
void odin3_elab1_gen_free(odin3_elab1_env *env);

/* The items of scope: children [*first, *end) of *node (a GEN_BLOCK's items, a bare item, none for
 * a NULL_STMT body; the module's items for scope 0). */
void odin3_elab1_scope_items(const odin3_elab1_env *env, uint32_t scope, odin3_ast_id *node,
                             uint32_t *first, uint32_t *end);

/* The components of hier (a HIER_NAME) in the environment's arena, their indices folded in
 * env->cur_scope ("generate block index"). ODIN3_ERR_PARSE after a diagnostic; NO_MEMORY. */
odin3_status odin3_elab1_hier_comps(odin3_elab1_env *env, odin3_ast_id hier,
                                    odin3_elab1_comp **out);

/* A new scope of kind ODIN3_ESCOPE_LOCAL under parent, for the localparams of a function, task or
 * named block (Task 15b; made by the writer after pass E's scopes, so it never shifts their
 * numbers): nothing finds it by name. *out its index. ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_scope_local(odin3_elab1_env *env, uint32_t parent, uint32_t *out);

/* The parameters of group (a PARAM_DECL of a function, task or block) as localparam entries of
 * env->cur_scope (a `parameter` there is a localparam; a specparam is ignored). NO_MEMORY. */
odin3_status odin3_elab1_env_add_params(odin3_elab1_env *env, odin3_ast_id group);

/* The scope record (NULL past the count) and its extension. */
const odin3_elab_scope *odin3_elab1_scope_at(const odin3_elab1_env *env, uint32_t scope);
const odin3_elab1_scope_x *odin3_elab1_scope_ext(const odin3_elab1_env *env, uint32_t scope);

/* Every name starting with `genblk` in node's subtree (a HIER_NAME's path aside) enters scope's
 * seen set, in pre-order (label_genblks's default case). ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_gen_seen(odin3_elab1_env *env, odin3_ast_id node, uint32_t scope);

/* --- declarations, ports, implicit nets, subroutines (Task 15, elab1_decl.c) ---------------- */

/* The net type nothing is implicit under (`default_nettype none). */
enum { ODIN3_NETTYPE_NONE = 0xFF };

/*
 * EL-25: the `default_nettype in effect for module_node (a parsed MODULE): the last
 * DIRECTIVE[default_nettype] before it in project stream order (the UNITs in order, directives
 * inside earlier modules included; one inside a module warns "`default_nettype inside a module
 * takes effect at the next module"), `resetall clearing it; as an ODIN3_AST_NET_DECL_* sub, or
 * ODIN3_NETTYPE_NONE. The table is built on the first call (an out-of-memory latches
 * odin3_elab1_status and answers wire).
 */
uint8_t odin3_elab1_nettype_at(odin3_elab1 *e1, odin3_ast_id module_node);

/*
 * EL-24: the ports of specialization spec's module merged into env->ports (e1->env), in header
 * order: ANSI declarators as written, each non-ANSI name with its body PORT_DECL and the net or
 * variable declaration that follows or precedes it. Errors: "port 'a' is not declared", "'x' is
 * declared as a port but is not in the port list", "port 'q' is declared with ranges [3:0] and
 * [7:0]", M6's "port expression in a module port list is not supported"; M12's warning for
 * `input reg`. ODIN3_OK (errors are counted); ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_ports(odin3_elab1 *e1, uint32_t spec);

/*
 * EL-25 (pass E, one module or generate-scope item at a time): every identifier of item (an
 * expression, statement or connection; not a constant position, a name used as function, task or
 * module, or a block's or subroutine's own local) that resolves to nothing becomes an implicit net
 * of env's implicit list (first use order) with the warning "'x' is implicitly declared", or the
 * error "'x' is not declared (`default_nettype is none)" or N15's "net type 'tri0' is not
 * supported". ODIN3_OK (errors are counted); ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_implicit(odin3_elab1_env *env, odin3_ast_id item);

/* Enter (+1) or leave (-1) the names a SEQ_BLOCK, FUNCTION_DECL or TASK_DECL declares for its body
 * (its ports and locals, and a subroutine's own name) in map (name -> depth count). Shared by the
 * implicit-net scan and the writer's genvar shadowing (EL-19). ODIN3_OK; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab1_scope_names(odin3_u64map *map, const odin3_ast *ast, odin3_ast_id node,
                                     bool enter);

/* The NET_DECL sub an implicit net of `default_nettype type has (wire/tri → wire, wand/triand →
 * wand, wor/trior → wor, uwire). */
uint8_t odin3_elab1_implicit_sub(uint8_t nettype);

/* True for a DIRECTIVE stage 1 reads (`default_nettype, `resetall) and drops from the elaborated
 * form; the other directives are Task 16's. */
bool odin3_elab1_directive_handled(const odin3_elab1 *e1, odin3_ast_id directive);

/* Frees the Task 15 tables of env. */
void odin3_elab1_decl_free(odin3_elab1_env *env);

/* How a path's leading components walk through generate blocks (odin3_elab1_scope_walk). */
typedef enum odin3_elab1_walked {
    ODIN3_WALK_STOP = 0, /* comps[used] is not a block (or every component was used) */
    ODIN3_WALK_NOT_ELAB, /* comps[used] is a block that was not elaborated (that index) */
    ODIN3_WALK_BAD       /* comps[used] names a block with an index it lacks, or no index */
} odin3_elab1_walked;

/* A path to walk: comps[0 .. count) from scope; where the walk stopped. */
typedef struct odin3_elab1_path {
    const odin3_elab1_comp *comps;
    uint32_t count;
    uint32_t scope;
} odin3_elab1_path;

typedef struct odin3_elab1_reach {
    uint32_t reached; /* the scope comps[used] is to be found in */
    uint32_t used;    /* the components walked through blocks */
} odin3_elab1_reach;

/* path's components walked through the elaborated generate blocks: the first found from
 * path->scope outward, the next ones inside the block reached. */
odin3_elab1_walked odin3_elab1_scope_walk(const odin3_elab1_env *env, const odin3_elab1_path *path,
                                          odin3_elab1_reach *out);

struct odin3_elab1 {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_ast *parsed;
    odin3_ast *elab; /* the store being written (run_queue), owned until handed out */
    odin3_elab_opts opts;
    const odin3_elab_limits *limits;
    odin3_status fatal; /* the first fatal status a void helper met */
    uint32_t errors;
    bool stopped;
    uint32_t work;              /* strtab ID of "work" */
    odin3_vec defs;             /* odin3_elab_def */
    odin3_vec defx;             /* odin3_elab1_defx, one per def */
    odin3_vec params;           /* odin3_ast_id, see odin3_elab_def */
    odin3_vec ports;            /* odin3_ast_id */
    odin3_vec insts;            /* odin3_ast_id: INSTANTIATIONs, per def in body order */
    odin3_u64map *by_name;      /* name → (first def + 1) | (last def + 1) << 32 */
    odin3_u64map *instantiated; /* names any MODULE body instantiates */
    odin3_u64map *noted;        /* names whose resolve info line is logged */
    odin3_u64map *warned;       /* warn_once keys: parsed node << 32 | format index */
    odin3_vec warn_fmts;        /* const char *: the warning formats in first-use order */
    odin3_u64map *said;         /* hash of (level, loc, text) → index into said_texts + 1 */
    odin3_vec said_texts;       /* odin3_elab1_said: the diagnostics logged once */
    bool pend_seen[ODIN3_PEND_END];
    /* Task 11 */
    odin3_arena *arena;       /* the run's: specializations, keys, names */
    odin3_arena *scratch;     /* odin3_elab1_params's results (renewed per call) */
    odin3_vec specs;          /* odin3_elab1_spec, in creation (FIFO) order */
    odin3_u64map *spec_hash;  /* key hash → first spec of the hash + 1 */
    odin3_u64map *spec_names; /* specialization names given so far */
    odin3_vec log;            /* odin3_elab_decision, contiguous per specialization */
    odin3_vec edges;          /* odin3_elab1_edge, contiguous per specialization */
    odin3_vec frames;         /* pass W's expression copier stack */
    odin3_u64map *port_pos;   /* def << 32 | port name → position + 1 (built per def) */
    odin3_elab1_env *env;     /* pass E's environment of the specialization being written */
    odin3_elab1_creating creating;
    /* Task 12 */
    uint64_t n_instances;     /* instance and gate nodes made so far (point (aa)'s cap) */
    odin3_vec inst_ovr;       /* odin3_elab_override: the instance's #() values, defparams, … */
    odin3_vec conns;          /* odin3_elab1_conn: odin3_elab1_connect's result */
    odin3_elab1_array array;  /* odin3_elab1_inst_array's range */
    odin3_u64map *conn_seen;  /* the names connected so far of the instance being resolved */
    odin3_vec given;          /* odin3_elab1_given, per specialization (pass E to pass W) */
    odin3_vec gparams;        /* odin3_elab1_gparam */
    odin3_vec bboxes;         /* odin3_elab1_bbox */
    odin3_vec bbports;        /* odin3_elab1_bbport */
    odin3_u64map *bbox_names; /* implicit black box name → index + 1; spec | 1 << 63 → idx + 1 */
    odin3_u64map *bbport_ix;  /* bbox << 32 | port name → bbports index + 1 */
    /* Task 14 */
    uint64_t n_gen_iters; /* generate-loop iterations unrolled so far (the run-level cap) */
    /* Task 15 */
    odin3_u64map *nettypes; /* MODULE node → its `default_nettype (odin3_elab1_nettype_at) */
};

/* A diagnostic logged once (odin3_elab1_report, odin3_elab1_error). */
typedef struct odin3_elab1_said {
    uint8_t level;
    odin3_loc loc;
    const char *text; /* in the run arena */
} odin3_elab1_said;

#endif
