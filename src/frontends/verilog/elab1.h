/*
 * elab1.h — stage 1 of elaboration (2D spec §2–§3; Verilog only): the run context, its
 * diagnostics (the error cap, warn-once, infos, the pending kinds of plan point (l)), the
 * definition table and module lookup (EL-6), top selection (EL-7), and (Task 11) parameters with
 * their precedence and types (EL-8, EL-16), the specialization table and its FIFO queue (EL-9,
 * §3.4), pass E's environments and pass W's writer of the elaborated store (EL-4). Stage 1 never
 * touches the IR; it reads the design's cell types, modules and project record. Every traversal
 * uses an explicit stack (odin3_ast_cursor, a frame vec) or a worklist.
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
 * Point (l)'s line for a form of a landed kind whose elaboration a later task lands (Task 11's
 * split: ordered connections and instance arrays, Task 12; a port declared in the body, Task 15;
 * a specparam, a real or time variable and a rejected net type, Task 16): the node's kind is
 * reported as pending (once per kind per run, counted as an error) whatever the table says.
 */
void odin3_elab1_pend_form(odin3_elab1 *e1, odin3_ast_id parsed_node);

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
 * its keyword (EL-6). True when it can be specialized (a MODULE whose name is free); false for a
 * taken name (what depends on it is skipped silently, EL-44) or a black box.
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
    ODIN3_ENTRY_INSTANCE /* an instance name (a defparam path's first component) */
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

/* Where an override comes from, in precedence order (EL-8: PROJECT is the highest). */
typedef enum odin3_elab_ovr_source {
    ODIN3_OVR_PROJECT = 1,
    ODIN3_OVR_NAMED,
    ODIN3_OVR_DEFPARAM,
    ODIN3_OVR_ORDERED
} odin3_elab_ovr_source;

/*
 * One override of a parameter: path is the strtab ID of "W" (a parameter of the target) or of
 * "u2.W" (pending: for the instance u2 below it, pushed down, EL-8); source an
 * odin3_elab_ovr_source; order the stream order among overrides of one source (the DEFPARAM node's
 * ID, so the first defparam wins; a project override's index in the record's params); value the
 * value as evaluated where it was written (self-determined, not yet converted, EL-16); at where to
 * report it (the CONNECTION, the defparam's HIER_NAME, the project line); from the node that wrote
 * it (the CONNECTION or DEFPARAM; 0 for a project line). base (added to the brief's fields): the
 * NUMBER sub-kind of the source literal of a value that is still unsized, for point (bb)'s
 * spelling of the final value.
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
 * One decision of pass E, replayed by pass W in order (§3.4): Task 11 logs one per INSTANCE of a
 * module item, its value the child specialization (>= 0), ODIN3_DECISION_AS_GIVEN for an instance
 * written as given (a cell type, a black box: EL-4) or ODIN3_DECISION_SKIPPED (a failed one).
 * Task 14 adds the generate decisions.
 */
typedef struct odin3_elab_decision {
    odin3_ast_id node;
    int32_t value;
} odin3_elab_decision;

enum { ODIN3_DECISION_SKIPPED = -1, ODIN3_DECISION_AS_GIVEN = -2 };

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
 * the next call); the overrides whose path goes deeper ("u2.W") are copied to pending[0 ..
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
 * created: depth req->depth, named per EL-9 and point (q), appended to the FIFO queue. Caps (point
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
 * with one named CONNECTION per parameter and per port), left on the store's pending stack.
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
    uint32_t next; /* the next definition of the same name + 1, 0 = none */
    uint8_t taken; /* odin3_elab1_taken */
    bool reached;  /* reached from the top (odin3_elab1_reach_def) */
    bool reported; /* its Q7 (a library unit's UDP) or taken-name error is logged */
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
    ODIN3_E1_FAILED
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
    uint32_t n_unpacked; /* SIGNAL: the unpacked dimensions (a memory) */
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

/* An environment (EL-12): the scopes of one definition's elaboration, their declarations
 * (slots, by scope << 32 | name), the folded ranges and the evaluator whose host it is. Task 11
 * has the module scope (0) only; Task 14 adds the generate scopes. */
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
} odin3_elab1_env;

/* A new environment of definition def with every declaration of its module scope recorded (no
 * value evaluated yet). ODIN3_ERR_NO_MEMORY (*out NULL). */
odin3_status odin3_elab1_env_create(odin3_elab1 *e1, uint32_t def, odin3_elab1_env **out);
void odin3_elab1_env_destroy(odin3_elab1_env *env);

/* The slot index of name in scope, UINT32_MAX when none. */
uint32_t odin3_elab1_env_find(const odin3_elab1_env *env, uint32_t scope, uint32_t name);
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

/* Point (l)'s line for kind at loc (a project line: no node), once per kind, counted. */
void odin3_elab1_pend_at(odin3_elab1 *e1, odin3_loc loc, uint32_t kind);

/* The full dotted text of a defparam's or project override's original path (for messages). */
odin3_status odin3_elab1_override_text(const odin3_elab1 *e1, const odin3_elab_override *ovr,
                                       odin3_strbuf *out);

/* The strtab text of id. */
const char *odin3_elab1_str(const odin3_elab1 *e1, uint32_t id);

/* The specialization spec (NULL past the count). */
const odin3_elab1_spec *odin3_elab1_spec_at(const odin3_elab1 *e1, uint32_t spec);

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
    odin3_elab1_env *env;     /* pass E's environment of the specialization being written */
    odin3_elab1_creating creating;
};

/* A diagnostic logged once (odin3_elab1_report, odin3_elab1_error). */
typedef struct odin3_elab1_said {
    uint8_t level;
    odin3_loc loc;
    const char *text; /* in the run arena */
} odin3_elab1_said;

#endif
