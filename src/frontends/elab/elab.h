/*
 * elab.h — elaboration (2D) shared by both stages: the caps (spec §11, EL-46), the options
 * (§2.2, EL-3), the stage APIs (§2.1) and the pending-kind tables of plan point (l).
 *
 * Every cap is checked before the allocation it guards and reported as a located PARSE error
 * naming it (§9.1, plan point (aa)). Tests lower the caps through odin3_elab_test_set_limits
 * (elab_test.h); the elaborator reads the caps in force through odin3_elab_limits_get.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_H
#define ODIN3_FRONTENDS_ELAB_ELAB_H

#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The caps of §11, exact, plus ODIN3_ELAB_MAX_REENTRY_DEPTH (2D Task 11, spec v4.6): constant
 * evaluations nested in C inside another (a typer callback's run: a replication count, a bound, a
 * function's return range; a host's re-entry: a lazily evaluated parameter read by another),
 * which take C stack (4-6 KiB a level under ASan) where function frames do not; the call-depth
 * cap still counts them together with the frames. Past it: `constant evaluation of <what> nested
 * deeper than 256`. And ODIN3_ELAB_MAX_EVAL_WORK (2D Task 9 ruling and fix round 1): the
 * word products one top-level constant evaluation may spend in * / % ** (ceil(width / 64)^2 for
 * each * / %; a ** up to (width - 2) square-and-multiply steps of that, O(w^3), which the width
 * cap alone does not bound). A single ** is checked before it runs (`constant '**' of N bits
 * exceeds the limit of M word operations`), the running total before each operation
 * (`constant evaluation of 'f' exceeds M word operations`).
 */
enum {
    ODIN3_ELAB_MAX_ERRORS = 20,                /* errors per run; then elaboration stops */
    ODIN3_ELAB_MAX_WIDTH = 1 << 20,            /* bits of one object, expression or value */
    ODIN3_ELAB_MAX_CELL_PINS = 1 << 22,        /* pins of one cell 2D creates */
    ODIN3_ELAB_MAX_LOOP_ITERATIONS = 1 << 20,  /* iterations of one loop */
    ODIN3_ELAB_MAX_TOTAL_ITERATIONS = 1 << 24, /* unrolled loop iterations per run */
    ODIN3_ELAB_MAX_EVAL_STEPS = 1 << 26,       /* steps of one top-level constant evaluation */
    ODIN3_ELAB_MAX_CALL_DEPTH = 1024,          /* constant-function frames, inline nesting */
    ODIN3_ELAB_MAX_REENTRY_DEPTH = 256,        /* constant evaluations nested in C (below) */
    ODIN3_ELAB_MAX_HIER_DEPTH = 1024,          /* instance nesting below the top */
    ODIN3_ELAB_MAX_SPECIALIZATIONS = 1 << 16,  /* modules made per run */
    ODIN3_ELAB_MAX_INSTANCES = 1 << 24,        /* instance and gate nodes per run */
    ODIN3_ELAB_MAX_ELAB_NODES = 1 << 25,       /* nodes of the elaborated store */
    ODIN3_ELAB_MAX_NET_BITS = 1 << 26,         /* nets created per run */
    ODIN3_ELAB_MAX_MEMORY_BITS = 1 << 30,      /* SIZE x WIDTH of one declared memory */
    ODIN3_ELAB_MAX_INIT_BITS = 1 << 26,        /* initial-value bits materialised per run */
    ODIN3_ELAB_MAX_READMEM_BYTES = 1 << 28,    /* one $readmem file */
    ODIN3_ELAB_MAX_MODULE_NAME = 128,          /* longer specialization names are hashed */
    ODIN3_ELAB_MAX_EVAL_WORK = 1 << 30,        /* word products of one constant evaluation */
    ODIN3_ELAB_MAX_BYTES_PERCENT = 60          /* the byte budget: this % of physical RAM */
};

/*
 * ODIN3_ELAB_MAX_BYTES, the run-level byte budget: ODIN3_ELAB_MAX_BYTES_PERCENT of physical
 * memory (sysconf pages x page size), computed once; UINT64_MAX when the system does not say.
 */
uint64_t odin3_elab_default_max_bytes(void);

/* The options of one elaboration (§2.2, EL-3). */
typedef struct odin3_elab_opts {
    uint32_t top;       /* strtab ID of "[lib.]mod" (CLI --top, else the project top); 0 = auto */
    odin3_loc top_at;   /* where the top was given (project line); 0 = CLI or none */
    bool nolatches;     /* Yosys -nolatches: unassigned paths of combinational blocks are x */
    bool nomem2reg;     /* never replace a memory by registers except where forced (EL-27) */
    bool auto_blackbox; /* an undefined module becomes a black box, not unresolved_module */
    double stage1_ms;   /* stage 1's time, for stage 2's summary line (odin3_elab_run fills it) */
    uint64_t gen_iters; /* generate-loop iterations stage 1 unrolled: stage 2's loops count on from
                           it against the run's total cap (point (k); odin3_elab_run fills it) */
    uint64_t *gen_iters_out; /* stage 1 writes its count here when set (odin3_elab_run's) */
} odin3_elab_opts;

/* --- the stages (§2.1, EL-2) --------------------------------------------------------------- */

/*
 * Stage 1 (Verilog; src/frontends/verilog/elab1*.c): the parsed store of one read run → a new
 * ELABORATED store of the same run (ctx->run), built with the 2A builders and sealed. opts (NULL:
 * the defaults) must already carry the top (odin3_elab_run's step 1 fills it). Runs the
 * definition table, top selection and module lookup (EL-6, EL-7), then writes the UNIT and its
 * MODULEs (EL-4); a kind whose elaboration has not landed is a located error (point (l)). Never
 * touches the IR and never changes parsed. On success *elab_out receives the store (owned by the
 * caller); otherwise *elab_out is NULL and no store is left: ODIN3_ERR_PARSE after at least one
 * located (or, for the top, unlocated) diagnostic, at most the error cap of them;
 * ODIN3_ERR_NO_MEMORY (stops at once, everything freed); ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * ctx, parsed or elab_out, a store that is not PARSED or belongs to another design.
 */
odin3_status odin3_elab_stage1(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts,
                               odin3_ast **elab_out);

/*
 * Stage 2 (any front end; src/frontends/elab/): an ELABORATED store → IR in ctx->design (EL-2
 * steps 4–5). Binds the store (odin3_elab_bind: the symbol table stays with it), makes every
 * MODULE's IR module and ports (odin3_elab2_ports), then every body in the UNIT's order
 * (odin3_elab2_module), skipping the constructs whose elaboration has not landed with point (l)'s
 * located error. On success it sets the design's top (the UNIT's first MODULE) and logs
 * `read_verilog: elaborated top 'T': M modules (S specializations), P processes, C cells, N nets,
 * t1 + t2 ms` (t1 = opts->stage1_ms). Never destroys elab. A failed run leaves valid, incomplete
 * modules and no top (EL-2; the design is fit only for destruction): ODIN3_ERR_PARSE after at
 * least one located diagnostic (user errors, the caps, the byte budget), at most the error cap of
 * them; ODIN3_ERR_NO_MEMORY (stops at once); ODIN3_ERR_CHECK (a 2D bug); ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL ctx or elab, a store that is not ELABORATED or belongs to another design, or
 * one the binder refuses as a misuse of the seam (EL-4).
 */
odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts);

/* --- not-yet-built constructs (plan point (l), ruling C3; elab_pending.c) ------------------- */

/* The stages a pending table belongs to. */
enum { ODIN3_ELAB_STAGE1 = 1, ODIN3_ELAB_STAGE2 = 2, ODIN3_ELAB_STAGES = 2 };

/*
 * What a pending table holds: an AST kind (odin3_ast_kind, below ODIN3_AST_KIND_COUNT) or one of
 * these features, which have no kind of their own.
 */
typedef enum odin3_elab_pend_feature {
    ODIN3_PEND_END = ODIN3_AST_KIND_COUNT /* no feature is left: Task 24 landed the last one */
} odin3_elab_pend_feature;

/*
 * True when the elaboration of kind_or_feature in stage (ODIN3_ELAB_STAGE1 or _STAGE2) has not
 * landed yet: it is in that stage's table, or a test forced it (odin3_elab_test_pend). Each task
 * that lands a kind removes it from the table in the same commit. False for any other stage or a
 * value at or past ODIN3_PEND_END.
 */
bool odin3_elab_pending(uint8_t stage, uint32_t kind_or_feature);

/*
 * The forms of landed kinds that a later task lands (spec v4.7; review m4): a kind stage 1 writes,
 * in a form it does not yet (stage 1: a port declared in the body (PORT_DECL), Task 15; a
 * specparam (PARAM_DECL), a time or real variable (VAR_DECL), tri0, tri1 and trireg (NET_DECL), a
 * hierarchical call (CALL) and a switch-level or pull gate (GATE_DECL, Q6), Task 16; Task 12
 * landed ordered connections, instance arrays and defparams on cell types). Such a form is
 * reported with
 * point (l)'s line for its kind only while its entry is here; each task removes the entry it lands
 * with its code, and the "tables empty" assertions of Tasks 16 and 24 count these too.
 * odin3_elab_pending_form: whether kind has a pending form in stage; _count: how many kinds do.
 */
bool odin3_elab_pending_form(uint8_t stage, uint32_t kind);
uint32_t odin3_elab_pending_form_count(uint8_t stage);

/*
 * Logs point (l)'s line for a pending kind_or_feature, once per kind per run: when seen (an array
 * of ODIN3_PEND_END flags the caller keeps for the run) already marks it, nothing; otherwise it is
 * marked and "file:line:col: error: elaboration is not available yet (2D): '<KIND>'" is logged at
 * loc (unlocated when loc is 0), <KIND> being the dump's kind name or the feature's text above.
 * The caller counts the error. Never allocates beyond the logger; a NULL seen or an out-of-range
 * value logs nothing.
 */
void odin3_elab_pend_report(odin3_design *design, odin3_loc loc, uint32_t kind_or_feature,
                            bool *seen);

#endif
