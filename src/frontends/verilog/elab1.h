/*
 * elab1.h — stage 1 of elaboration (2D spec §2–§3; Verilog only): the run context, its
 * diagnostics (the error cap, warn-once, infos, the pending kinds of plan point (l)), the
 * definition table and module lookup (EL-6), top selection (EL-7) and, until Task 11 brings the
 * specialization queue and passes E/W, the reachability walk and the writer of the UNIT and its
 * MODULE shells. Stage 1 never touches the IR; it reads the design's cell types, modules and
 * project record. Every traversal uses an explicit stack (odin3_ast_cursor) or a worklist.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ELAB1_H
#define ODIN3_FRONTENDS_VERILOG_ELAB1_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
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
 * loc) and the context stops (odin3_elab1_stopped); later errors log nothing.
 */
void odin3_elab1_error(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/*
 * A warning at parsed_node's location, once per (parsed node, message format) for the run, so a
 * construct elaborated several times warns once (point (v): the key is the node's ID and the
 * format's index in the run's format list, formats compared by text). Out of memory for the key
 * latches ODIN3_ERR_NO_MEMORY (odin3_elab1_status) and logs nothing.
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

/* --- the top, reachability and the shells (elab1_top.c, elab1.c) --------------------------- */

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
 * The reachability walk until Task 11's queue (EL-6, EL-9): from top, breadth first in source
 * order, every INSTANTIATION of a reached MODULE's body is resolved (a black box's body is not
 * walked); unresolved_module is reported at the INSTANTIATION ("unresolved_module: module 'm' is
 * not defined"), a taken name at its MODULE. The reached definitions are kept in BFS order (the
 * top first); then the design-unit modules not reached are named in one info line, "not
 * instantiated under the top, not elaborated: 'a', 'b'". ODIN3_OK; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab1_reach(odin3_elab1 *e1, uint32_t top);

/*
 * Task 10's pass W: a new ELABORATED store with one UNIT named after the top and one MODULE per
 * reached non-black-box definition whose name is not taken (that one failed), in BFS order, each
 * with its parsed range and name, a ports LIST when the source has one, a params LIST when it would
 * hold entries. Every unit-level directive, module attribute, port, parameter and item whose kind
 * is pending is reported (odin3_elab1_not_yet) and left out; a MODULE whose kind is forced pending
 * is reported and not written. *out receives the unsealed store (owned by the caller).
 * ODIN3_ERR_NO_MEMORY or a builder's status (nothing in *out).
 */
odin3_status odin3_elab1_write_unit(odin3_elab1 *e1, odin3_ast **out);

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
    bool reached;  /* reached from the top (odin3_elab1_reach) */
    bool reported; /* its Q7 (a library unit's UDP) or taken-name error is logged */
} odin3_elab1_defx;

struct odin3_elab1 {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_ast *parsed;
    odin3_ast *elab; /* the store being written (write_unit), owned until handed out */
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
    odin3_vec order;            /* uint32_t: the reached definitions, BFS order, the top first */
    odin3_u64map *by_name;      /* name → (first def + 1) | (last def + 1) << 32 */
    odin3_u64map *instantiated; /* names any MODULE body instantiates */
    odin3_u64map *noted;        /* names whose resolve info line is logged */
    odin3_u64map *warned;       /* warn_once keys: parsed node << 32 | format index */
    odin3_vec warn_fmts;        /* const char *: the warning formats in first-use order */
    bool pend_seen[ODIN3_PEND_END];
};

#endif
