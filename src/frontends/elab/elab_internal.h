/*
 * elab_internal.h — stage 2's internal interfaces (2D, spec §12, plan point (j)). The first part is
 * the binding (EL-5): the symbol table of an elaborated store and the resolution array stage 2
 * reads, filled by the binder (bind.c), which also checks the rows of the EL-4 form that 2A's check
 * rule 7 does not. The second part (Task 17) is stage 2's context: the IR modules, their ports and
 * wires, provenance, attributes, the error cap and the byte budget (elab2.c, elab2_wires.c). The
 * third (Task 18) is expressions: signals to word cells, selects and continuous assignments
 * (elab2_expr.c, elab2_select.c, elab2_assign.c). The fourth (Task 19) is drivers, instances and
 * gates: the driver table and claims, wired nets, tri-state, the lvalue an output takes apart
 * (elab2_claim.c, elab2_wired.c, elab2_assign.c), instances and gates (elab2_inst.c), black-box
 * cell types (elab2_bbox.c). The fifth is memories (elab2_mem.c, elab2_memscan.c, elab2_memport.c).
 * Later tasks add processes to it.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/name_resolve.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The binding of an elaborated store (EL-5), stage 2's (freed with it); the symbol table it fills
 * is the store's (odin3_ast_symtab, freed with the store).
 *  - sym_of[node.v] for node.v < n: for every IDENT, HIER_NAME, CALL, TASK_CALL and INSTANTIATION,
 *    the symbol it resolves to (an INSTANTIATION: the MODULE symbol of the store's MODULE it names,
 *    0 for a registered cell type, which stage 2 finds in the registry; a system CALL or TASK_CALL:
 *    0); for every declaring node (DECLARATOR, INSTANCE, MODULE, GEN_BLOCK, named SEQ_BLOCK,
 *    FUNCTION_DECL, TASK_DECL), the symbol it declares (the first GEN_BLOCK of a loop's name and
 *    each further iteration share one BLOCK symbol); 0 elsewhere.
 *  - scope_of: node ID → scope ID for every MODULE, GEN_BLOCK, named SEQ_BLOCK, FUNCTION_DECL and
 *    TASK_DECL (odin3_elab_scope_of).
 */
typedef struct odin3_elab_binding {
    odin3_sym_id *sym_of;
    uint32_t n;
    odin3_u64map *scope_of;
} odin3_elab_binding;

/*
 * Binds elab (EL-5) in one pre-order sweep: creates its symbol table (odin3_symtab_create) and
 * fills *out. Scopes: DESIGN → one LIBRARY per library (the FILE buffer's of each MODULE's loc,
 * "work" when it has none) → MODULE → GENERATE per GEN_BLOCK (index from its slot), BLOCK per
 * named SEQ_BLOCK, FUNCTION, TASK. Symbols: one per declarator (NET, VAR, LOCALPARAM with its
 * value), INSTANCE, BLOCK, FUNCTION (and, in its scope, the VAR of its result, named like it),
 * TASK, and a MODULE per MODULE in its LIBRARY; a black-box MODULE (its ATTR blackbox) has a scope
 * holding its ports only. Names resolve from their scope outward to the MODULE scope, so a name
 * declared later in a scope resolves (divergence §17 #1); an INSTANTIATION names a MODULE of the
 * store. Assignments (odin3_symbol_add_assign, source order, the assigning node): every NET_ASSIGN
 * lvalue and net declaration assignment (CONTINUOUS), every BLOCKING_ASSIGN / NONBLOCKING_ASSIGN
 * lvalue outside function and task bodies.
 *
 * Errors, located (§9.1), counted against the error cap in force (ODIN3_ELAB_MAX_ERRORS: past it,
 * "too many errors; elaboration stopped after N" and the sweep stops):
 *  - user errors (§2.5), then ODIN3_ERR_PARSE: `'x' is not declared`; `'x' is already declared
 *    (first at f.v:L:C)`; `hierarchical reference 'u.w' is not supported`; `'g[5].t' does not
 *    exist (generate block 'g[5]' was not elaborated)`; `'f' is not a function`; `'t' is not a
 *    task`; `width N exceeds the limit of M bits` (a packed range, or a bound outside int32);
 *    `range bound has x or z bits`; `'u' is not a net, variable or parameter` (a name of an
 *    instance, function or task; a block's name binds to its BLOCK symbol, Task 18's carry);
 *  - EL-4 violations, the rows rule 7 does not check (a misuse of the seam), then
 *    ODIN3_ERR_INVALID_ARG: `elaborated form: <rule>` (spec §2.4, v4.9).
 * ODIN3_ERR_NO_MEMORY stops at once. On any failure *out is emptied (nothing to free) and the
 * partly built symbol table stays with the store (it passes check rule 8; odin3_ast_destroy frees
 * it). INVALID_ARG (logged, nothing bound) for a NULL argument, a store that is not ELABORATED or
 * does not hold exactly one UNIT, or one that has a symbol table already. Never changes the nodes;
 * iterative (explicit stacks, no recursion).
 */
odin3_status odin3_elab_bind(odin3_ast *elab, odin3_elab_binding *out);

/* The scope a MODULE, GEN_BLOCK, named SEQ_BLOCK, FUNCTION_DECL or TASK_DECL opened; 0 otherwise
 * (or for a NULL binding). */
odin3_scope_id odin3_elab_scope_of(const odin3_elab_binding *binding, odin3_ast_id node);

/* The symbol node resolves to or declares (sym_of); 0 outside the array or for a NULL binding. */
odin3_sym_id odin3_elab_sym_of(const odin3_elab_binding *binding, odin3_ast_id node);

/* Frees the binding's arrays and empties it; NULL and an empty binding are no-ops. */
void odin3_elab_binding_free(odin3_elab_binding *binding);

/* --- stage 2: signals, cell types, the context (Task 17; EL-2, EL-23, EL-24, EL-45, EL-46) ---- */

/*
 * One bit of a signal: a net ID (module-local), ODIN3_SB_CONST | odin3_bit (a constant bit, whose
 * net odin3_elab2_const_net makes on first use), or ODIN3_SB_ANY (an any-value bit, the process
 * generator's `-`). One encoding for expressions and the process generator (review M21). Macros,
 * not an enum: the values do not fit an int (ISO C17, -Wpedantic).
 */
typedef uint32_t odin3_sigbit;
#define ODIN3_SB_CONST (1U << 31)
#define ODIN3_SB_ANY (ODIN3_SB_CONST | 4U)

/* A signal, LSB first; bits live in the stage-2 scratch arena (valid until the module ends). */
typedef struct odin3_sig {
    odin3_sigbit *bits;
    uint32_t width;
    bool is_signed;
} odin3_sig;

/* The built-in cell types 2D creates, exactly (review R6); the context caches their IDs. */
typedef enum odin3_elab2_type {
    ODIN3_E2T_ADD,
    ODIN3_E2T_SUB,
    ODIN3_E2T_MUL,
    ODIN3_E2T_DIV,
    ODIN3_E2T_MOD,
    ODIN3_E2T_POW,
    ODIN3_E2T_AND,
    ODIN3_E2T_OR,
    ODIN3_E2T_XOR,
    ODIN3_E2T_XNOR,
    ODIN3_E2T_NOT,
    ODIN3_E2T_NEG,
    ODIN3_E2T_SHL,
    ODIN3_E2T_SHR,
    ODIN3_E2T_SSHR,
    ODIN3_E2T_SHIFTX,
    ODIN3_E2T_EQ,
    ODIN3_E2T_NE,
    ODIN3_E2T_LT,
    ODIN3_E2T_LE,
    ODIN3_E2T_GT,
    ODIN3_E2T_GE,
    ODIN3_E2T_REDUCE_AND,
    ODIN3_E2T_REDUCE_OR,
    ODIN3_E2T_REDUCE_XOR,
    ODIN3_E2T_REDUCE_XNOR,
    ODIN3_E2T_REDUCE_BOOL,
    ODIN3_E2T_LOGIC_NOT,
    ODIN3_E2T_LOGIC_AND,
    ODIN3_E2T_LOGIC_OR,
    ODIN3_E2T_MUX,
    ODIN3_E2T_TRIBUF,
    ODIN3_E2T_PMUX,
    ODIN3_E2T_MEM,
    ODIN3_E2T_PROC,
    ODIN3_E2T_CONST0,
    ODIN3_E2T_CONST1,
    ODIN3_E2T_CONSTX,
    ODIN3_E2T_CONSTZ,
    ODIN3_E2T_COUNT
} odin3_elab2_type;

enum { ODIN3_E2_CONSTS = 4 }; /* $_CONST0_, 1, X, Z: indexed by odin3_bit */

/*
 * Stage 2's context (one per odin3_elab_stage2 run). Fields are the stage's own: only elab2*.c
 * and later 2D stage-2 files touch them; tests read the public functions below.
 */
typedef struct odin3_elab2 odin3_elab2;

enum { ODIN3_NL_BAD_TEXT = 64 }; /* bytes of the first malformed `src` text a -netlist run keeps */
struct odin3_elab2 {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_ast *elab;
    odin3_elab_opts opts;
    bool mod_nolatches; /* the options in force for the module being emitted: the run's, or those */
    bool mod_nomem2reg; /* of the `read_verilog --lib` read that supplied it (2D-7b) */
    bool mod_vtr_compat;
    const odin3_elab_limits *limits;
    odin3_elab_binding bind;
    odin3_symtab *st;
    odin3_celltype_id types[ODIN3_E2T_COUNT];
    odin3_arena *scratch;  /* signals and attribute values of the module being emitted */
    odin3_u64map *modules; /* MODULE node ID -> IR module ID */
    odin3_u64map *warned;  /* (loc, format) hashes of the warnings given */
    odin3_vec built;       /* odin3_module_id of every module made, in order */
    odin3_strbuf text;     /* a name or path being built */
    odin3_vec stack;       /* odin3_elab2_item worklist of the item walk */
    odin3_vec chain;       /* odin3_scope_id, innermost first, of a scope path being built */
    odin3_ctype *ty;       /* the expression typer (Task 18), made on first use */
    odin3_vec frames;      /* the expression walk's frames (Task 18) */
    odin3_vec vals;        /* the expression walk's value stack: odin3_sig (Task 18) */
    odin3_u64map *bboxes;  /* black-box MODULE node ID -> its cell type ID (Task 19, EL-11) */
    struct odin3_elab2_drive *drive; /* the driver tables of the module being emitted (Task 19) */
    odin3_vec mems;      /* odin3_elab_mem of the module being emitted, by symbol (Task 20) */
    struct odin3_pg *pg; /* the process generator of the module being emitted (Task 21) */
    odin3_ceval *ev;     /* the interpreter of user function calls (Task 23, EL-17), made on use */
    odin3_ast_id call; /* the CALL or TASK_CALL being inlined: the second loc of EL-45 (Task 23) */
    odin3_u64map *bound; /* symbols of the loops being unrolled: constants to the AST test (T23) */
    uint64_t iterations; /* loop iterations unrolled so far in the run, generate loops included */
    /* the module being emitted */
    odin3_module *mod;
    odin3_module_id mod_id;
    odin3_ast_id mod_node;
    odin3_scope_id scope; /* the scope of the item being emitted (0: the module's own) */
    uint32_t mod_name;    /* strtab ID of the specialization's name */
    odin3_net_id const_net[ODIN3_E2_CONSTS];
    /* errors, accounting */
    uint32_t errors;
    bool stopped;        /* the error cap, a fatal cap or the budget ended the stage */
    odin3_status fatal;  /* NO_MEMORY or CHECK: wins over the located errors */
    uint64_t net_bits;   /* nets created so far, against ODIN3_ELAB_MAX_NET_BITS */
    uint64_t est_bytes;  /* the IR built, by EL-46's per-object estimate */
    uint64_t objects;    /* IR objects made so far */
    uint64_t next_check; /* the object count of the next budget check */
    uint32_t ports_made; /* ports created (the refuse_port test hook counts them) */
    /* read_verilog -netlist (5.4c; netlist_locs.c) */
    const char *asg_loc;     /* the `// LOC` text of the CONT_ASSIGN being emitted; NULL: none */
    odin3_u64map *nl_raw;    /* (module, strtab ID) of every raw declarator name, made on use */
    uint32_t nl_capped;      /* `src` attributes holding more than 8 locations (INFO; X-3) */
    uint32_t nl_bad;         /* `src` attributes that are not locations (kept; L-6) */
    odin3_loc nl_bad_loc;    /* the first of them */
    odin3_name_miss nl_miss; /* hash-shaped declarators the --name-map lacks (H-6) */
    odin3_loc nl_miss_loc;   /* the first of them */
    char nl_bad_text[ODIN3_NL_BAD_TEXT]; /* its text, cut */
    bool pend_seen[ODIN3_PEND_END];
};

/* One item of the module walk: a node of the module or of a generate block, and the scope it is
 * in (0: the module's own). */
typedef struct odin3_elab2_item {
    odin3_ast_id node;
    odin3_scope_id scope;
} odin3_elab2_item;

/* The IR objects a step created, for the byte estimate and the 2^16-object budget check. */
typedef struct odin3_elab2_counts {
    uint32_t nodes, pins, nets, wires;
} odin3_elab2_counts;

/* The cached ID of a built-in type; the zero ID for a NULL context or an out-of-range type. */
odin3_celltype_id odin3_elab2_type_id(const odin3_elab2 *e2, odin3_elab2_type type);

/*
 * A stage-2 context for the elaborated store elab of ctx's run: the cell-type IDs of
 * odin3_elab2_type (looked up once with odin3_celltype_find), the scratch arena and the tables.
 * Neither elab nor ctx is owned. opts NULL: the defaults. Does not bind (odin3_elab_stage2 does).
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL ctx, elab or out, a store that is not ELABORATED or
 * belongs to another design, or a built-in type the design does not have (a 2D bug);
 * ODIN3_ERR_NO_MEMORY. *out is NULL on failure.
 */
odin3_status odin3_elab2_create(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts,
                                odin3_elab2 **out);

/* Frees the context (not the IR it made, not the store); NULL is a no-op. */
void odin3_elab2_destroy(odin3_elab2 *e2);

/* True when MODULE node module carries the ATTR `blackbox` (EL-4): it makes no IR module. */
bool odin3_elab2_is_blackbox(const odin3_elab2 *e2, odin3_ast_id module);

/*
 * Makes the IR module of MODULE node module_node (made by odin3_elab2_ports) the one being
 * emitted: clears the per-module constant nets and the scratch arena. ODIN3_ERR_INVALID_ARG
 * (logged) for a node whose module was not made; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_enter(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * Creates the IR module of MODULE node module_node (named like the node) and its ports, in port
 * order, with odin3_module_add_port (EL-24): each port's wire takes the declared range and sign
 * (IR change C), `scalar` set for a port without a range, a SOURCE record at its PORT_DECL, the
 * attributes of the declaration, and the port symbol's ir_module and ir_wire are recorded
 * (odin3_symbol_set_ir). It runs for every MODULE before any body (EL-2); a black-box MODULE (an
 * ATTR `blackbox`) makes no module: it declares its cell type (odin3_elab2_blackbox). A module that
 * cannot be made (its name is a cell type already, or the IR refuses a port) is a located error and
 * ODIN3_ERR_PARSE, and the caller stops stage 2. The net-bits cap (point (aa)) and the byte
 * budget apply. ODIN3_ERR_NO_MEMORY and ODIN3_ERR_INVALID_ARG (a node that is not a MODULE of
 * the store, or already made) as usual; the IR stays valid.
 */
odin3_status odin3_elab2_ports(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * Emits the body of the module whose ports odin3_elab2_ports made: calls the on_body_start test
 * hook, checks the budget, then EL-28's steps in order ((1) the wires, (2) the pre-scan, (4) the
 * instances and gates, (5) the continuous assignments, (7) the wired nets, then the end-of-module
 * warnings; every construct of the other steps whose elaboration has not landed is point (l)'s
 * located error, once per kind, and is skipped, so the rest of the module is emitted). ODIN3_OK
 * even after located errors (they are counted); ODIN3_ERR_PARSE when the stage must stop (the error
 * cap, a fatal cap, the budget); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a node whose ports
 * were not made. A black-box MODULE is skipped.
 */
odin3_status odin3_elab2_module(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * Step (1) of EL-28 for the module (EL-23): one IR wire per net and variable declarator of the
 * module, of its generate blocks and of the named blocks (and the unnamed ones with locals) of
 * its processes, named by the scope path (`g[0].t`, `blk.count`, `$unnamed_block$1.tmp`; a
 * module-level object keeps its plain name), with the declared range (no range: [0:0]; `integer`:
 * [31:0]), sign, a SOURCE record at the declarator (EL-45) and the declaration's attributes;
 * odin3_symbol_set_ir is called for each symbol. A memory (unpacked dimensions) is Task 20's and
 * makes no wire; a function's or task's locals are inlined (EL-38), not wires. A name an earlier
 * wire has (an escaped identifier spelling a scope path) is a located error and skipped. The
 * net-bits cap applies (fatal). Status as odin3_elab2_module.
 */
odin3_status odin3_elab2_wires(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * The current module's shared constant net for bit (IR-4): the net of its $_CONST0_, $_CONST1_,
 * $_CONSTX_ or $_CONSTZ_ node, both made on first use (unnamed, with the module's record) and
 * cached for the module. ODIN3_ERR_INVALID_ARG (logged) with no module being emitted;
 * ODIN3_ERR_NO_MEMORY with nothing left behind.
 */
odin3_status odin3_elab2_const_net(odin3_elab2 *e2, odin3_bit bit, odin3_net_id *out);

/* What a SOURCE record covers: node (elaborated store), the call that inlined it (0: none) and
 * the scope (0: the module's own). */
typedef struct odin3_elab2_prov_req {
    odin3_ast_id node, call;
    uint32_t scope;
} odin3_elab2_prov_req;

/* What read_verilog -netlist (5.4c) adds to a request: a second node whose `src` attribute counts
 * after the request node's (a declaration beside its declarator, an instantiation beside its
 * instance; 0: none) and the decoded L-5 name of the object (NULL: none). Ignored without
 * -netlist. */
struct odin3_netlist_name;
typedef struct odin3_elab2_prov_nl {
    odin3_ast_id attrs;
    const struct odin3_netlist_name *name;
} odin3_elab2_nl_extra;

/*
 * The SOURCE record of EL-45 for the request (odin3_prov_source, so hash-consed; IMPORTED under
 * read_verilog -netlist): locs = the
 * node's {loc, end} then the call's when there is one (odin3_srcman_srcloc, raw loc kept), ast =
 * node, hier = `<specialization name>/<scope path>` with each generate or named block scope as
 * `name` or `name[index]` (the module's own scope: the name alone). ODIN3_ERR_NO_MEMORY;
 * ODIN3_ERR_INVALID_ARG (logged) with no module being emitted.
 */
odin3_status odin3_elab2_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req, odin3_prov_id *out);

/* odin3_elab2_prov with the -netlist additions nl (NULL: none). Under -netlist the record is
 * IMPORTED (a netlist line, L-7), or the restored DERIVED one (odin3_netlist_prov). */
odin3_status odin3_elab2_prov_nl(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                                 const odin3_elab2_nl_extra *nl, odin3_prov_id *out);

/*
 * Applies the ATTRs attached to node to obj (2A §7, EL-45): no value → INT 1, a NUMBER → BITS
 * (sized as odin3_ast_number_value), a STRING → STRING. A later ATTR of one key replaces an
 * earlier one. ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for an object of another module.
 */
odin3_status odin3_elab2_attrs(odin3_elab2 *e2, odin3_ast_id node, odin3_objref obj);

/*
 * One located error (unlocated when loc is 0) in the diagnostic style, counted against
 * ODIN3_ELAB_MAX_ERRORS; the first one over the cap logs `too many errors; elaboration stopped
 * after 20` instead and stops the stage. odin3_elab2_fatal also stops it (the caps that guard
 * memory). odin3_elab2_warn: one warning, once per (loc, format).
 */
void odin3_elab2_error(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);
void odin3_elab2_fatal(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);
void odin3_elab2_warn(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/*
 * The string table's text for id; "" for an ID with none. The result belongs to the design.
 */
const char *odin3_elab2_str_of(const odin3_elab2 *e2, uint32_t id);

/*
 * The one cap on a cell's pins (ODIN3_ELAB_MAX_CELL_PINS in force): ODIN3_OK when pins is within
 * it, else the fatal `cell of N pins exceeds the limit of M pins` at loc and ODIN3_ERR_PARSE.
 */
odin3_status odin3_elab2_pins_cap(odin3_elab2 *e2, odin3_loc loc, uint64_t pins);

/*
 * The number of indices of a dimension, |msb - lsb| + 1 (a packed range's width, an unpacked
 * dimension's size). Defined in elab2_mem.c.
 */
uint64_t odin3_elab2_dim_size(const odin3_dim *dim);

/*
 * The safety net of an ODIN3_ERR_PARSE ("counted, go on"): a producer that returned it without
 * raising any error would let the read succeed with the construct missing (end_status looks at
 * the error count). This counts one located internal error at loc when no error is counted yet
 * (and the stage was not stopped: the cap's stop line is the count). Call it for every PARSE a
 * producer returns that the walk absorbs.
 */
void odin3_elab2_count_parse(odin3_elab2 *e2, odin3_loc loc);

/*
 * True when node's kind (or the feature) is pending in stage 2 (point (l)): its located line is
 * logged once per kind and counted as an error, and the caller skips the construct. False when
 * the kind has landed. The line is not repeated once the kind was seen.
 */
bool odin3_elab2_pending(odin3_elab2 *e2, odin3_ast_id node);
void odin3_elab2_pend_feature(odin3_elab2 *e2, odin3_loc loc, uint32_t feature);

/*
 * Counts n more nets against ODIN3_ELAB_MAX_NET_BITS (point (aa), checked before the nets exist):
 * over it, `elaboration needs more than 67108864 net bits` at at (fatal, the stage stops) and
 * ODIN3_ERR_PARSE. ODIN3_OK otherwise.
 */
odin3_status odin3_elab2_reserve_nets(odin3_elab2 *e2, uint32_t n, odin3_loc at);

/*
 * Records the IR objects just created (EL-46's estimate: 48 bytes a node, 28 a pin, 48 a net, 64
 * a wire) and runs odin3_elab2_budget again every 2^16 objects. Status as the budget.
 */
odin3_status odin3_elab2_account(odin3_elab2 *e2, const odin3_elab2_counts *made);

/*
 * EL-46's byte budget: the bytes of every AST store the design holds (and the elaborated store,
 * when the design does not hold it yet) plus the estimate of the IR stage 2 built must not exceed
 * the cap in force (60% of physical memory; a test lowers it). Over it: the unlocated
 * `elaboration needs more than N bytes (the budget is 60% of physical memory)`, fatal, and
 * ODIN3_ERR_PARSE. Checked at each module boundary of stage 2 and by odin3_elab2_account.
 */
odin3_status odin3_elab2_budget(odin3_elab2 *e2);

/*
 * True when the module name is a specialization's: a '$' followed by a `P=v` entry, the `$2` of a
 * name already taken, or `h` and 16 hex digits (EL-9); an escaped identifier with a '$' in it is
 * none of them. The summary line and tests/bench/bench_elab.c count with it.
 */
bool odin3_elab2_is_specialization(const char *name);

/* True once the stage was stopped (the cap, a fatal cap, the budget). */
bool odin3_elab2_stopped(const odin3_elab2 *e2);

/*
 * One warning whose text is msg (as the typer reports them), once per (loc, text): a repeat of the
 * same text at the same place is dropped, a different text at it is not. No format is interpreted.
 */
void odin3_elab2_warn_msg(odin3_elab2 *e2, odin3_loc loc, const char *msg);

/* --- stage 2: expressions, selects, assignments (Task 18; EL-28, EL-30, EL-31, EL-32) -------- */

/* The sigbit of a constant bit, and the tests of the encoding. */
static inline odin3_sigbit odin3_sb_of_bit(odin3_bit bit) {
    return ODIN3_SB_CONST | (odin3_sigbit)bit;
}

/* True for a constant 0, 1, x or z (not ODIN3_SB_ANY, not a net). */
static inline bool odin3_sb_is_const(odin3_sigbit sb) {
    return (sb & ODIN3_SB_CONST) != 0 && (sb & ~ODIN3_SB_CONST) < (odin3_sigbit)ODIN3_E2_CONSTS;
}

/* The constant a constant sigbit holds (undefined for any other). */
static inline odin3_bit odin3_sb_bit(odin3_sigbit sb) {
    return (odin3_bit)(sb & ~ODIN3_SB_CONST);
}

/*
 * A signal of `width` bits in the stage-2 scratch arena (valid until the module ends), every bit
 * the constant 0, unsigned. A width of 0 gives an empty signal. ODIN3_ERR_NO_MEMORY; *out is
 * unchanged on failure.
 */
odin3_status odin3_elab2_sig_new(odin3_elab2 *e2, uint32_t width, odin3_sig *out);

/*
 * The `rv` substitution hook of the process generator (Task 21): inside a process the current value
 * of a net replaces the net, bit by bit. NULL outside processes (and in every Task 18 call).
 */
typedef struct odin3_elab2_regs_rd {
    odin3_ast_id select;  /* the word SELECT */
    uint32_t mem;         /* the register array's table index */
    const odin3_sig *idx; /* the index of the one dimension, generated */
} odin3_elab2_regs_rd;

/*
 * The hooks a process gives the expression walker (Task 23): call yields the value of a user
 * function CALL by inlining it in the process (EL-38; NULL: a continuous context, which makes a
 * combinational process of its own), word the value of a register-array word with a variable index
 * as a case inline in the process (EL-27; NULL: likewise a process of its own). Both may be NULL.
 */
typedef struct odin3_elab2_reads {
    odin3_status (*bit)(void *user, odin3_net_id net, odin3_sigbit *out);
    void *user;
    odin3_status (*call)(void *user, odin3_ast_id call, odin3_sig *out);
    odin3_status (*word)(void *user, const odin3_elab2_regs_rd *rd, odin3_sig *out);
} odin3_elab2_reads;

/*
 * One expression to generate. context is odin3_ctype_expr's (width 0: self-determined; an
 * assignment passes {L(lhs), true, false}). dest is NULL, or holds context.width nets that the
 * outermost cell drives by construction (EL-28) when that cell is exactly that wide; nothing else
 * ever uses it. reads is NULL outside processes.
 */
typedef struct odin3_elab2_expr_req {
    odin3_ast_id expr;
    odin3_etype context;
    const odin3_net_id *dest;
    const odin3_elab2_reads *reads;
} odin3_elab2_expr_req;

/*
 * Types the expression (odin3_ctype_expr over the store's names: nets and variables by their wire,
 * parameters by their value, EL-13) and emits it post-order with explicit stacks (no recursion),
 * each node yielding a signal (EL-30). An operator whose operands are all constant is folded by the
 * evaluator (odin3_cval_op: no cell); otherwise one word cell per operator, with genrtlil's
 * parameters (operand widths as generated: an identifier or select at its own width, an unsized
 * constant and a concatenation at the context width; *_SIGNED = the operand's sign in context;
 * Y_WIDTH = the node's final width, max(final, 1) for comparisons, reductions and logical
 * operators), the node's attributes and its SOURCE record. Extension and truncation are wiring.
 * *out is the root's signal at the width it was generated (not extended to the context), signed as
 * the root is in context. An identifier that names a generate or named block is `'x' is not a net,
 * variable or parameter` (Yosys makes an implicit net; §17); a memory word `m[i]` is a $mem read
 * port or a register word (EL-27, Task 20); a call of a user function is the pending
 * continuous-context call (Task 23). ODIN3_ERR_PARSE after a located error (counted; the caller
 * goes on with the next item) or when a fatal cap stopped the stage (the cell-pins cap of point
 * (aa), the net-bits cap, the byte budget); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a node
 * that is no expression or a stage with no module being emitted. Nothing is attached for an
 * expression that failed before its last cell; cells made earlier stay (valid, unused).
 */
odin3_status odin3_elab2_expr(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);

/*
 * The same walk, suspendable at a user call (PR 2D-7, review I6): the generator that inlines calls
 * runs the callee from its own frame stack instead of from inside the walk, so one call inside
 * another costs no C stack. The walk's state is e2->frames and e2->vals above frame_base and
 * val_base (stack discipline: whatever runs while the walk is suspended must leave both as it found
 * them). req.reads must outlive the walk; its call hook is not called: a non-constant call of a
 * user function suspends the walk with walk->call set, and the caller gives the call's value to
 * odin3_elab2_walk_resume. A walk with no reads->call hook (a continuous context) never suspends.
 * The cells made and their order are those of odin3_elab2_expr.
 */
typedef struct odin3_elab2_walk {
    odin3_elab2_expr_req req;
    size_t frame_base, val_base;
    odin3_ast_id call; /* the call awaiting its value, or 0 */
    uint32_t held_next;
    uint8_t held_pick;
} odin3_elab2_walk;

/* Types the expression and opens its root. ODIN3_ERR_INVALID_ARG as odin3_elab2_expr. */
odin3_status odin3_elab2_walk_begin(odin3_elab2 *e2, const odin3_elab2_expr_req *req,
                                    odin3_elab2_walk *walk);

/* Steps until the walk is done or suspended at a call (walk->call.v != 0). */
odin3_status odin3_elab2_walk_run(odin3_elab2 *e2, odin3_elab2_walk *walk);

/* The suspended call's value is the node's signal; the walk goes on with odin3_elab2_walk_run. */
odin3_status odin3_elab2_walk_resume(odin3_elab2 *e2, odin3_elab2_walk *walk,
                                     const odin3_sig *value);

/* A finished walk's root signal; pops the walk's stacks. */
void odin3_elab2_walk_take(odin3_elab2 *e2, odin3_elab2_walk *walk, odin3_sig *out);

/* Pops whatever a walk (finished or not) left on the stacks (an error unwinds through this). */
void odin3_elab2_walk_abort(odin3_elab2 *e2, odin3_elab2_walk *walk);

/*
 * The self-determined type of an expression (EL-13): its width and sign, typed without generating
 * anything (no cell, no net). The typer's located errors and warnings are given as for
 * odin3_elab2_expr (ODIN3_ERR_PARSE after an error); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG
 * (logged) for a node that is no expression.
 */
odin3_status odin3_elab2_self_type(odin3_elab2 *e2, odin3_ast_id expr, odin3_etype *out);

/*
 * Whether expr is constant by the AST alone: its leaves are literals and parameters (a localparam,
 * a genvar), and its calls are $signed, $unsigned or $clog2 of such. No variable, net, memory word
 * or user call; nothing is generated. This is the test of the process generator's folding of
 * `if`/`case` and of its non-constant lvalue selects (Yosys folds an AST constant only); a
 * variable that merely holds a constant at that point (rv) is not constant here. Iterative.
 * ODIN3_ERR_NO_MEMORY (*out unchanged).
 */
odin3_status odin3_elab2_expr_is_const(const odin3_elab2 *e2, odin3_ast_id expr, bool *out);

/*
 * odin3_elab2_expr for a SELECT node (EL-32 reads): constant selects (the bits at the declared
 * indices; out-of-range bits are x with the warning `range select [9:6] of 'w' is out of bounds;
 * bits outside [7:0] are x`, or `bit select [9] of 'w' is out of bounds; the bit is x`; a part
 * select written against its declaration is the error `part-select [0:3] of 'w' is reversed
 * relative to its declaration [7:0]`) and non-constant reads through $shiftx with genrtlil's
 * offset computation. INVALID_ARG (logged) for a node that is not a SELECT; otherwise as
 * odin3_elab2_expr.
 */
odin3_status odin3_elab2_select(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);

/*
 * One word cell (EL-30, EL-31) with genrtlil's parameter rules: A_WIDTH, B_WIDTH = the widths of
 * a and b as given, A_SIGNED, B_SIGNED = their is_signed, Y_WIDTH = y_width (a $mux: WIDTH =
 * y_width, A = a, B = b, S = the one bit of s). Y drives dest (y_width nets, already made) when
 * given, else y_width fresh unnamed nets. The node's attributes and SOURCE record (scope: the
 * item being emitted) go on the cell. The cell-pins cap (point (aa)) is checked first: over it,
 * `cell of N pins exceeds the limit of M pins` at node (fatal), ODIN3_ERR_PARSE and nothing is
 * made. *y is the Y signal, unsigned. Constants in a and b become the module's constant nets.
 * type must be one of the word types of odin3_elab2_type (ODIN3_E2T_ADD .. ODIN3_E2T_MUX), else
 * ODIN3_ERR_INVALID_ARG (logged). ODIN3_ERR_NO_MEMORY with the IR unchanged but for constant nets.
 */
typedef struct odin3_elab2_cell_req {
    odin3_elab2_type type;
    odin3_ast_id node;
    const odin3_sig *a, *b;
    uint32_t y_width;
    const odin3_net_id *dest;
    const odin3_sig *s; /* $mux only (added to the brief's fields at the end: agent default) */
} odin3_elab2_cell_req;

odin3_status odin3_elab2_cell(odin3_elab2 *e2, const odin3_elab2_cell_req *req, odin3_sig *ysig);

/*
 * The nets of sig's bits, LSB first, into nets[0 .. sig->width): a net as is, a constant bit the
 * module's constant net (ODIN3_SB_ANY: the $_CONSTX_ net), made on first use. ODIN3_ERR_NO_MEMORY;
 * ODIN3_ERR_INVALID_ARG (logged) with no module being emitted.
 */
odin3_status odin3_elab2_to_nets(odin3_elab2 *e2, const odin3_sig *sig, odin3_net_id *nets);

/*
 * sig at `width` bits (EL-13): truncated to its low bits, or extended by repeating its top bit
 * when is_signed (the constant 0 for an empty signal) and with the constant 0 otherwise. Wiring
 * only: no cell and no net. out->is_signed = is_signed. ODIN3_ERR_NO_MEMORY; *out is unchanged
 * on failure. out may be in.
 */
odin3_status odin3_elab2_extend(odin3_elab2 *e2, const odin3_sig *in, uint32_t width,
                                bool is_signed, odin3_sig *out);

/* --- selects: the object, the range and the bit positions (elab2_select.c) ------------------ */

/* The object of a select: its declared range as written, its width and its name for messages. */
typedef struct odin3_elab2_selobj {
    int32_t msb, lsb;
    uint32_t width;
    const char *name;
} odin3_elab2_selobj;

/* What a select covers: the indices left .. right as written (left is the most significant bit
 * of the result), `width` bits. constant: the index was a constant (always for [m:l]); known: and
 * it has no x or z bit (else the result is all x and left, right are 0). */
typedef struct odin3_elab2_selrange {
    int64_t left, right;
    uint32_t width;
    bool constant, known;
} odin3_elab2_selrange;

/* The declared index of bit j of the result (bit 0 is the right end). */
static inline int64_t odin3_elab2_sel_index(const odin3_elab2_selrange *range, uint32_t bit) {
    return range->left >= range->right ? range->right + (int64_t)bit : range->right - (int64_t)bit;
}

/*
 * The bit position (LSB first) of declared index idx in obj, or -1 outside it: lsb + k for a
 * descending range (msb >= lsb), lsb - k for an ascending one.
 */
int64_t odin3_elab2_sel_pos(const odin3_elab2_selobj *obj, int64_t idx);

/*
 * The object of a select, from the node it names: an IDENT or HIER_NAME of a net, variable or
 * parameter has its declared range (none: [0:0] for a net or variable, [width-1:0] for a
 * parameter) and name; any other node has [width-1:0] and the name `expression`. width is the
 * signal's width.
 */
void odin3_elab2_sel_object(const odin3_elab2 *e2, odin3_ast_id object, uint32_t width,
                            odin3_elab2_selobj *out);

/*
 * What SELECT node select covers in obj: [m:l] from its two NUMBER bounds (a range written
 * against the declaration is the error `part-select [0:3] of 'x' is reversed relative to its
 * declaration [7:0]`, ODIN3_ERR_PARSE), a bit select from index, a [b +: w] or [b -: w] from index
 * (the base) and its NUMBER width, oriented as the declaration. index is the evaluated index
 * (NULL for [m:l]); one with a non-constant bit makes range->constant false, one with an x or z
 * bit range->known false. ODIN3_OK otherwise.
 */
odin3_status odin3_elab2_sel_range(odin3_elab2 *e2, odin3_ast_id select,
                                   const odin3_elab2_selobj *obj, const odin3_sig *index,
                                   odin3_elab2_selrange *range);

/*
 * The indices a [b +: w] (plus) or [b -: w] select covers when its base is the known value base,
 * oriented as the declaration of obj: range->width is w on entry, left and right are set (a bit
 * select is plus with w = 1). A base far outside the object is clamped to a band that stays
 * outside it. Wiring only: no diagnostics.
 */
void odin3_elab2_sel_orient(const odin3_elab2_selobj *obj, bool plus, int64_t base,
                            odin3_elab2_selrange *range);

/* What the walk hands a SELECT's emission: its operands already emitted, and the lhs nets the
 * root may drive by construction (dest, dest_width of them; NULL when this is not the root). */
typedef struct odin3_elab2_selargs {
    odin3_ast_id node;
    const odin3_sig *object;
    const odin3_sig *index; /* NULL for [m:l] */
    const odin3_net_id *dest;
    uint32_t dest_width;
} odin3_elab2_selargs;

/* The signal of the select (unsigned): constant or $shiftx, as odin3_elab2_select. */
odin3_status odin3_elab2_select_emit(odin3_elab2 *e2, const odin3_elab2_selargs *args,
                                     odin3_sig *out);

/*
 * The value of a NUMBER node (a select bound or width, a replication count: EL-4 makes them
 * NUMBERs) as an integer, in *out, with *known false when it has an x or z bit (or is outside
 * int64). ODIN3_ERR_INVALID_ARG (logged: a misuse of the seam) for another kind of node;
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_number_i64(odin3_elab2 *e2, odin3_ast_id node, int64_t *out, bool *known);

/*
 * The value of a signal of constant 0 and 1 bits as an integer (two's complement when
 * sig->is_signed; a value past int64 saturates), in *out. Returns 1 for such a signal, 0 for a
 * constant one with an x or z bit (*out untouched) and -1 when a bit is a net (or ODIN3_SB_ANY).
 */
int odin3_elab2_sig_i64(const odin3_sig *sig, int64_t *out);

/*
 * A continuous assignment (EL-28; A1): net_assign is a NET_ASSIGN (lhs, rhs) or a DECLARATOR with a
 * value (a net declaration assignment `wire w = e;`, N7). The rhs is generated at max(L(lhs),
 * L(rhs)) (context {L(lhs), true, false}), truncated or extended to L(lhs) by its sign, and each
 * lhs bit becomes the rhs bit: by construction when the rhs is a cell made for it (its Y pins are
 * the lhs nets), else by merging the two nets (odin3_net_merge: the rhs net is kept), with the
 * same-net no-op. The lhs is a net or variable, a constant bit or part select of one, or a
 * concatenation of those; a non-constant select is `continuous assignment to 'y' with a
 * non-constant index is not supported`, bits outside the net are dropped with `assignment to bits
 * outside 'y' [7:0] is ignored`. Task 19: every lhs bit is claimed before anything is attached
 * (A4, `uwire`, `supply`, wired nets, the tri-state and `inout` rules: odin3_elab2_claim), a
 * `reg` target warns `'r' is a variable assigned by a continuous assignment; treated as a net`,
 * and `c ? d : 'bz` (either branch an all-z constant) is a `$tribuf` (D3). Statuses as
 * odin3_elab2_expr.
 */
odin3_status odin3_elab2_assign(odin3_elab2 *e2, odin3_ast_id net_assign);

/* The continuous assignment `assign lhs = rhs` of two expression nodes (a statement of an `initial`
 * that --vtr-compat reads so, pg_init.c), quietly: the caller says what it did. Status and errors
 * as odin3_elab2_assign; the nodes are not checked to be assignable (odin3_elab2_lvalue_make's
 * located error). */
odin3_status odin3_elab2_assign_nodes(odin3_elab2 *e2, odin3_ast_id lhs, odin3_ast_id rhs);
/* --- stage 2: drivers, instances, gates, wired nets (Task 19; EL-10, EL-11, EL-26, EL-28, EL-29)
 * -- */

/* What drives a net bit, by the driver table (a vec of odin3_drv indexed by net ID). */
typedef enum odin3_drv_kind {
    ODIN3_DRV_NONE = 0,
    ODIN3_DRV_PORT_IN,   /* an input port of the module (driven from outside) */
    ODIN3_DRV_CONST,     /* a shared constant net, or a supply net */
    ODIN3_DRV_CELL,      /* the output of a cell 2D made */
    ODIN3_DRV_INSTANCE,  /* an output pin of an instance */
    ODIN3_DRV_GATE,      /* a gate's output */
    ODIN3_DRV_ASSIGN,    /* a continuous assignment */
    ODIN3_DRV_PROCESS,   /* a process (Task 21) */
    ODIN3_DRV_INOUT_PIN, /* an inout pin: the module's own inout port, or a child's */
    ODIN3_DRV_TRIBUF     /* a $tribuf output (D3) */
} odin3_drv_kind;

/* One entry of the driver table: the kind and the AST node of the driver (0: none located). */
typedef struct odin3_drv {
    uint8_t kind; /* odin3_drv_kind */
    odin3_ast_id node;
} odin3_drv;

/* The tables of one module's drivers (elab2_claim.c; elab2_drive.h shares it); opaque to everything
 * else. */
struct odin3_elab2_drive;

/*
 * A driver to claim: n net bits (LSB first; net 0 is a dropped bit, skipped), the kind of driver
 * and its AST node (A4's "also driven at" place of a later conflict, and the place of this one's
 * error).
 */
typedef struct odin3_elab2_claim_req {
    const odin3_net_id *nets;
    uint32_t n;
    uint8_t kind; /* odin3_drv_kind */
    odin3_ast_id node;
} odin3_elab2_claim_req;

/*
 * Checks every bit of req against the driver table BEFORE anything is attached, and records the
 * driver on each bit when all pass: *granted true. A conflict is a located error (counted; the
 * first conflicting bit only) and *granted false with nothing recorded, so the caller attaches
 * nothing:
 *  - A4: `'y[3]' has two drivers (also driven at f.v:L:C)` (a whole-net conflict names the net;
 *    the same net twice in one request is a conflict at the second), located at req->node;
 *  - `'u' is a uwire and has a second driver (first at f.v:L:C)`;
 *  - `supply0`/`supply1` nets are driven by the shared constants, so any driver is A4's error;
 *  - wired nets (`wand wor triand trior`) collect drivers instead (EL-26), which needs the driver
 *    redirected onto a fresh net (odin3_elab2_lvalue_claim does, odin3_elab2_wired chains them);
 *    this net-only function cannot, so a wired net in req is ODIN3_ERR_INVALID_ARG (logged,
 * *granted false, nothing recorded): the caller uses the lvalue form;
 *  - ODIN3_DRV_TRIBUF shares a net only with other tristate drivers and inout pins;
 *    ODIN3_DRV_INOUT_PIN shares a net only with those, and with ordinary logic is `net 'x' is
 *    driven by inout port 'p' of instance 'u' and by logic at f.v:L:C; internal tri-state nets are
 *    not supported (D3)`. Whether a tri-state net may stay one (a top-level port) or becomes logic
 *    is decided at the end of the module (odin3_elab2_tri_logic), not here.
 * ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a kind past the
 * enum, or no module being emitted.
 */
odin3_status odin3_elab2_claim(odin3_elab2 *e2, const odin3_elab2_claim_req *req, bool *granted);

/*
 * What drives bit `wb` of a wire of the module being emitted, as the tables know it so far: the
 * claim recorded on the wire bit (an assignment, an instance output, a gate), else the driver of
 * its net (an input port, a constant, a cell); kind ODIN3_DRV_NONE when nothing does.
 */
odin3_drv odin3_elab2_bit_driver(const odin3_elab2 *e2, odin3_wirebit wb);

/*
 * Step (2) of EL-28 for the module being emitted: starts the driver tables (input ports are
 * ODIN3_DRV_PORT_IN, the module's inout ports ODIN3_DRV_INOUT_PIN, both at the port's declarator),
 * makes `supply0`/`supply1` nets the shared constant nets, registers `uwire` and wired nets, and
 * scans every `always` block's lvalues: the always-assigned bit set (odin3_elab2_always_assigned;
 * EL-27 reads it too; a non-constant index marks the whole object) and the wired-net check `'w' is
 * a wired net (wand) and cannot be assigned in an always block` (located at the lvalue, counted).
 * Status as odin3_elab2_module.
 */
odin3_status odin3_elab2_prescan(odin3_elab2 *e2, odin3_ast_id module_node);

/* True when an always block of the module being emitted assigns bit `bit` of the object symbol
 * names (a memory: bit 0 is the whole memory); false before the pre-scan. */
bool odin3_elab2_always_assigned(const odin3_elab2 *e2, odin3_sym_id sym, uint32_t bit);

/*
 * Step (7) of EL-28 (EL-26): each wired wire (`wand`/`triand` -> $and, `wor`/`trior` -> $or) bit
 * is driven by a chain over its drivers in source order, the last cell driving the wire's nets
 * (k drivers: k - 1 two-input word cells of the wire's width when every driver covers the whole
 * wire, else one-bit cells per bit); a bit with one driver is that driver's net merged into the
 * wire's (pin-based, IR-15) and one with none stays undriven. Every cell's SOURCE record is the
 * wire's declarator. Status as odin3_elab2_module.
 */
odin3_status odin3_elab2_wired(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * The end of the module's emission (EL-28): once per module, `'w' is never driven` for each wire
 * that is not a port with a net that has sinks and no driver, at its declarator, and the pending
 * `'p' and 'q' are connected only to each other and are never driven` of the self-assignments of
 * odin3_elab2_assign whose net is still undriven (their wires are not reported twice). Status as
 * odin3_elab2_module.
 */
odin3_status odin3_elab2_undriven(odin3_elab2 *e2);

/* Deletes the unnamed wires that call frames made and nothing used (an unused local of a callee),
 * after the module's processes are emitted. */
odin3_status odin3_elab2_sweep_frames(odin3_elab2 *e2);

/* Forgets the driver tables (a new module starts); frees them when e2 is destroyed. */
void odin3_elab2_drive_reset(odin3_elab2 *e2);
void odin3_elab2_drive_free(odin3_elab2 *e2);

/*
 * Merges drop into keep (odin3_net_merge) and remembers it: odin3_elab2_resolve follows the
 * forwarding to the net a net became, for ones recorded before the merge. keep == drop and a dead
 * net are ODIN3_ERR_INVALID_ARG from the IR, so callers compare first.
 */
odin3_status odin3_elab2_merge(odin3_elab2 *e2, odin3_net_pair pair);
odin3_net_id odin3_elab2_resolve(const odin3_elab2 *e2, odin3_net_id net);

/* One bit of an lvalue: the wire bit it came from (wire 0: none), the piece of the lvalue naming
 * it (an identifier or select node) and whether the piece names the whole wire. */
typedef struct odin3_elab2_lvbit {
    odin3_wirebit wb;
    odin3_ast_id node;
    bool whole;
} odin3_elab2_lvbit;

/* One position a non-constant select can take: the declared index value the base holds there and
 * the part of the select inside the object: its bits first .. first + count - 1 (bit 0 is the
 * right end), of which bit first sits at wire position pos (LSB first) and bit j at pos + j -
 * first. A slice that straddles an end of the object has count < the select's width. */
typedef struct odin3_elab2_varcand {
    int64_t value;
    uint32_t pos;
    uint32_t first;
    uint32_t count;
} odin3_elab2_varcand;

/* A non-constant select that is a whole procedural target (`y[s]`, `y[b +: w]`: EL-32's case
 * method, Task 22). node is 0 when the lvalue has none. cands are the positions at which the select
 * lies at least partly inside the object and its base can hold the value (the index's
 * self-determined width and sign limit them), by ascending value; lv->nets then holds every bit
 * some position writes. */
typedef struct odin3_elab2_varsel {
    odin3_ast_id node;  /* the SELECT */
    odin3_ast_id index; /* its base index expression, not generated */
    odin3_wire_id wire; /* the wire selected from */
    odin3_elab2_selobj obj;
    uint32_t width;       /* bits written at a position */
    uint32_t index_width; /* the index's self-determined width ... */
    bool index_signed;    /* ... and sign */
    odin3_vec cands;      /* odin3_elab2_varcand */
} odin3_elab2_varsel;

/*
 * Fills vs->width, index_width, index_signed and cands from vs->node (a bit, [b +: w] or [b -: w]
 * SELECT), obj and index, which the caller set. A width that is not positive or too large and the
 * typer's errors are located and counted, ODIN3_ERR_PARSE; the positions times the width over
 * the width cap is `width N exceeds the limit of M bits` at the select. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_varsel_make(odin3_elab2 *e2, odin3_elab2_varsel *vs);

/* A word of a register array with a non-constant index as the whole procedural target (`m[s] <=
 * d`, EL-27's case over the words, Task 23). node is 0 when the lvalue has none; lv->nets then
 * holds the bits of every word, word 0 first. */
typedef struct odin3_elab2_regsel {
    odin3_ast_id node;  /* the word SELECT */
    odin3_ast_id index; /* its index expression, not generated */
    uint32_t mem;       /* the table index of the array */
} odin3_elab2_regsel;

/* A word of a memory (not registers) as the whole procedural target, or a constant bit or part
 * select of it (EL-39, Task 24): the write port the process generator makes. word is the word
 * SELECT, mem its table index; node is 0 when the lvalue has none. lv->nets then holds, for result
 * bit i (LSB first), the position in the word it writes plus one (0: outside the word). */
typedef struct odin3_elab2_memsel {
    odin3_ast_id node;
    uint32_t mem;
} odin3_elab2_memsel;

/* An lvalue taken apart: nets[i] and bits[i] for result bit i (LSB first); net 0 and wire 0 for a
 * bit outside its net. */
typedef struct odin3_elab2_lvalue {
    odin3_vec nets; /* odin3_net_id */
    odin3_vec bits; /* odin3_elab2_lvbit */
    odin3_vec work; /* odin3_ast_id: the nodes still to take apart */
    bool dropped;   /* some bit lies outside its net */
    /* An assignment target inside a process (Task 21), set by the caller after init: a memory word
     * is a write port of its memory, a non-constant select the case method (Task 22; var), and the
     * messages do not speak of continuous assignments. */
    bool procedural;
    odin3_ast_id top;       /* the lhs being taken apart (set by odin3_elab2_lvalue_make) */
    odin3_elab2_varsel var; /* procedural only: the non-constant select that is the target */
    odin3_elab2_regsel
        regs; /* procedural only: the word of a register array with a variable index */
    odin3_elab2_memsel memw; /* procedural only: a word of a memory (nets: positions + 1) */
    bool scan_only; /* procedural only: names of a callee's variables are not resolved (the scan) */
    bool scan_calls; /* procedural only: the scan; an index with a call in it is not generated */
    const odin3_elab2_reads
        *reads; /* procedural only: the hooks the index expressions are read through */
} odin3_elab2_lvalue;

void odin3_elab2_lvalue_init(odin3_elab2_lvalue *lv);
void odin3_elab2_lvalue_free(odin3_elab2_lvalue *lv);

/*
 * Takes lhs apart: an identifier of a net or variable, a constant bit or part select of one, or a
 * concatenation of those (EL-28); the located errors of odin3_elab2_assign (`'x' is not a net or
 * variable`, a memory, `continuous assignment to 'y' with a non-constant index is not supported`,
 * bits outside the net warned and dropped) are counted and ODIN3_ERR_PARSE. A `reg` target warns
 * `'r' is a variable assigned by a continuous assignment; treated as a net` when as_assign.
 * A procedural lvalue (lv->procedural) may be a select whose index is not constant by the AST
 * (odin3_elab2_expr_is_const), alone: lv->var is then filled (the index is not generated) and
 * lv->nets holds every bit some position writes, or none when the index can select nothing inside
 * the object (`assignment to bits outside 'y' [3:0] is ignored`, the bits dropped); inside a
 * concatenation it is `non-constant index into 'y' is not supported in a concatenation target`.
 * A word of a register array with a non-constant index is lv->regs; a word of a memory (or a
 * constant bit or part select of one) is lv->memw, alone only (in a concatenation it is `memory
 * word of 'm' in a concatenation target is not supported`: the process generator splits a
 * concatenation target that holds one before it gets here, as Yosys does). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_lvalue_make(odin3_elab2 *e2, odin3_ast_id lhs, odin3_elab2_lvalue *lv);

/*
 * The claim of an lvalue's bits as odin3_elab2_claim (with the names of the bits for the
 * messages, the first driver's place from the wire-bit claims, and every lvalue bit remembered as
 * driven by node when record is set). A wired bit is redirected onto a fresh net, replacing the
 * lvalue's net and recording the driver for odin3_elab2_wired. kind ODIN3_DRV_INOUT_PIN also
 * takes port and instance (names for the inout message).
 */
typedef struct odin3_elab2_lvclaim {
    odin3_elab2_lvalue *lv;
    uint8_t kind; /* odin3_drv_kind */
    odin3_ast_id node;
    bool record;
    const char *port, *instance; /* ODIN3_DRV_INOUT_PIN only */
} odin3_elab2_lvclaim;

odin3_status odin3_elab2_lvalue_claim(odin3_elab2 *e2, const odin3_elab2_lvclaim *req,
                                      bool *granted);

/*
 * Every lvalue bit takes its bit of sig (which has exactly the lvalue's width): the same net
 * as the lvalue's bit is a driver made by construction (recorded in the table as kind), another
 * net is merged (the signal's kept, odin3_elab2_merge) and the lvalue bit remembered as driven by
 * node; the same net with no driver at all is a self-assignment (reported by odin3_elab2_undriven).
 * Bits claimed with record already are not recorded twice. ODIN3_ERR_NO_MEMORY.
 */
typedef struct odin3_elab2_lvconn {
    odin3_elab2_lvalue *lv;
    const odin3_sig *sig;
    uint8_t kind; /* odin3_drv_kind */
    odin3_ast_id node;
    odin3_ast_id rhs; /* the right side, for the self-assignment warning (0: none) */
} odin3_elab2_lvconn;

odin3_status odin3_elab2_lvalue_connect(odin3_elab2 *e2, const odin3_elab2_lvconn *req);

/*
 * A $tribuf driving an lvalue (D3, Q3): the lvalue's bits are claimed as ODIN3_DRV_TRIBUF at
 * claim (so every D3 and A4 rule applies and nothing is made on a conflict), then one cell with
 * WIDTH the lvalue's width: A the data (data_node generated at the lvalue's width, or the signal
 * data, extended by its sign; inverted through a $not when invert_data), EN the condition
 * (cond_node generated self-determined, or the signal cond; a one-bit condition as is, a wider one
 * through $reduce_bool; inverted through $not (one bit) or $logic_not (wider) when invert), Y the
 * lvalue's nets (fresh ones for a dropped bit). node is the cell's SOURCE record and attributes.
 * Errors as odin3_elab2_assign.
 */
typedef struct odin3_elab2_tri_req {
    odin3_elab2_lvalue *lv;
    odin3_ast_id data_node, cond_node;
    const odin3_sig *data, *cond;
    bool invert, invert_data;
    odin3_ast_id node, claim;
} odin3_elab2_tri_req;

odin3_status odin3_elab2_tribuf(odin3_elab2 *e2, const odin3_elab2_tri_req *req);

/*
 * Step (7b) of EL-28 (EL-29, spec 17 #79), after every net of the module is merged: the `$tribuf`
 * cells of nets that reach no port of the top module become logic as Yosys's `tribuf -logic` does
 * (a cell with a net on a top-level output or inout port keeps its handling). The cells whose Y is
 * the same list of nets are one group, in source order; a group becomes one `$pmux` (WIDTH the
 * nets, S_WIDTH the drivers) whose A is x (no driver enabled), B the drivers' data in order, S
 * their enables and Y the nets, and the `$tribuf` cells are deleted. One warning per group, at its
 * first driver: `tri-state drivers on internal net 'data' converted to logic (no driver enabled: x;
 * several enabled: undefined)`. Located errors, the cell left alone: Y lists that overlap without
 * being the same, a Y with both top-level port and internal nets, an inout port of a module below
 * the top, an inout pin of a child instance on the net. Status as odin3_elab2_module.
 *
 * With e2->mod_vtr_compat, a group of two or more drivers whose data is all input-port bits of the
 * module is resolved as Parmys does instead (elab2_trivtr.c, PHASE2 #27): no `$pmux`, the enables
 * dropped, the nets merged into the input Yosys keeps, and a warning that names the net, the data
 * kept and the data dropped.
 */
odin3_status odin3_elab2_tri_logic(odin3_elab2 *e2);

/* The IR name of a net, variable or instance symbol: its scope path and its name, interned. */
odin3_status odin3_elab2_scoped_name(odin3_elab2 *e2, const odin3_symbol *rec, uint32_t *name);

/*
 * Instances (EL-10, EL-11). instantiation is the holder, an INSTANTIATION node (not an INSTANCE:
 * the AST has no parent pointer); every INSTANCE below it becomes a node of
 * the module's cell type (a module of the store, a black box, or a registered cell type) with its
 * SOURCE record at the INSTANCE, the INSTANTIATION's attributes and, for a module or black box,
 * one `parameter:<NAME>` attribute (BITS or STRING) per parameter of its params LIST. Inputs are
 * generated at their self-determined width and sign (odin3_elab2_expr), resized to the port with
 * Yosys's hierarchy rules (I4) and the warning `port 'a' of instance 'u' (module 'sub') is 8
 * bits; the connection of 4 bits is extended` / `... is truncated` (none for a constant);
 * an unconnected input is the $_CONSTX_ net; an output or inout takes a net lvalue (odin3_elab2_
 * lvalue_make) whose bits are claimed (ODIN3_DRV_INSTANCE, ODIN3_DRV_INOUT_PIN): bits past the
 * port are driven by the port's top bit when the child declares the port signed, else by 0, and
 * port bits past the connection (an unconnected output) go to fresh nets. A cell type's missing
 * parameters are inferred from the connection widths (odin3_celltype_infer_params). Errors are
 * located and counted (ODIN3_ERR_PARSE, the caller goes on with the next item). INVALID_ARG for a
 * node that is not an INSTANTIATION, or no module being emitted; NO_MEMORY.
 *
 * A black box's port widths come from its cell type (odin3_elab2_blackbox): a cell takes the exact
 * widths of its own parameter set. A set the cell type has no widths for (a registered table that
 * lacks the instance's values, as after a second read, or a width parameter that is not a valid
 * width) is the located error `instance 'u' of 'bb': port 'a' has no width for its parameter
 * values (see the port_width line above)`. A PARSE that raised no error at all is counted as an
 * internal error (odin3_elab2_count_parse), so a read never succeeds with an instance dropped.
 */
odin3_status odin3_elab2_instance(odin3_elab2 *e2, odin3_ast_id instantiation);

/*
 * Gates (Q1-Q3), lowered as Yosys's simplify does. gate_decl is the holder, a GATE_DECL node (not
 * one of its INSTANCEs); every INSTANCE below it is lowered. and/or/xor: a left fold over the
 * inputs of two-input word cells (one bit each for one-bit terminals), the last driving the output;
 * nand/nor/xnor: the fold, then $not; buf/not: one assignment of the (inverted) input per output;
 * bufif0/bufif1/notif0/notif1: a $tribuf with D3's rules (the enable inverted through $not for the
 * *if0 forms, the data for the notif forms). Statuses as odin3_elab2_instance.
 */
odin3_status odin3_elab2_gate(odin3_elab2 *e2, odin3_ast_id gate_decl);

/*
 * EL-11: declares the black-box cell type of a `(* blackbox *)` or implicit black-box MODULE
 * (odin3_celltype_declare_blackbox, once per definition, under its base name): its ports in port
 * order, a parameter per PARAM_DECL (elab2_bbox.c). The declaration's port widths are the
 * MODULE's, the default specialization's. Spec v4.19 (Peter decision 3): the definition's other
 * parameter sets arrive as carrier MODULEs (stage 1: a black-box MODULE whose `blackbox` ATTR is
 * valued with the definition's name); this call reads them all, and each port whose width differs
 * in some set gets a width expression over the parameter values (a table with one entry per set),
 * so a cell takes its set's exact widths from its parameters. Two sets with equal values (they
 * differ in their literals' sizes only) but different widths of a port are the located error
 * `black box 'bb': port 'p' has different widths in parameter sets with equal values`. A carrier
 * itself declares nothing. A mismatch with a registered type is the located error `black box 'x'
 * does not match the registered cell type: <why>`. Called by odin3_elab2_ports.
 */
odin3_status odin3_elab2_blackbox(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * The value a parameter node holds, as the cell type's parameter pdef wants it: an INT (a NUMBER),
 * BITS or STRING. ODIN3_ERR_PARSE (no diagnostic) for a value that does not fit the kind;
 * ODIN3_ERR_NO_MEMORY. Strings are the design's, bits are in the scratch arena.
 */
odin3_status odin3_elab2_param_value(odin3_elab2 *e2, const odin3_param_def *pdef,
                                     odin3_ast_id node, odin3_value *out);

/*
 * Pushes the items of GEN_BLOCK or MODULE node (its tail after nslots slots) on e2->stack, in the
 * scope they are in, so the first pops first. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_push_items(odin3_elab2 *e2, odin3_ast_id node, uint32_t nslots,
                                    odin3_scope_id scope);

/* --- stage 2: initial values (Task 24; EL-40) ------------------------------------------------- */

/*
 * EL-40 for the module being emitted (EL-28, after the instances and the continuous assignments,
 * so the drivers of a net are known): every `initial` block and variable initialiser (a `reg r =
 * v;` or a port's) in source order through the interpreter, with a store of the bits they write.
 * An `initial` is run statement by statement; a statement that is not constant (it reads an input,
 * a net driven by logic, or calls what is not constant) is ignored with `initial statement is not
 * constant and is ignored` (once per statement) and leaves its targets as they were. Loops, `if`,
 * `case` and constant function calls work as in EL-17; a memory word takes constant indices.
 * `$readmemh`/`$readmemb` as a statement of the block is odin3_elab2_readmem. The results, per bit
 * or word with a known (0, 1 or z) value: an output of an `always` block gets the `init` attribute
 * of its wire (x where unknown), a memory kept as a `$mem` its `INIT`; a variable nothing else
 * drives has its net merged onto the shared constant net; a net driven by a continuous assignment,
 * an instance, a gate or an input port keeps its value out: `initial value of 'w' is ignored: 'w'
 * is driven by a continuous assignment` (`an instance`, `a gate`, `an input port`), once per wire.
 * The init-bits cap of point (aa) is `initial values need more than N bits` at the statement.
 * Status as odin3_elab2_module (ODIN3_OK after located errors, which are counted).
 * ODIN3_ERR_INVALID_ARG (logged) for a module that is not the one being emitted.
 */
odin3_status odin3_elab2_initials(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * `$readmemh("f", m [, start [, finish]])` / `$readmemb` (S4) as a statement of an `initial`:
 * parsed as Yosys 0.55's readmem does (blank-separated tokens, `//` and a block comment, `@hex`
 * addresses, each word a sized literal of the memory's width in base 16 or 2), filling the
 * one-dimensional array m from start (default: its lowest index) toward finish (default: its
 * highest), into the state of the initial run store->user belongs to (a store handed out by
 * odin3_elab2_initials). A relative path is resolved against the directory of the source file
 * holding the call (point (y); an absolute one is used as given). Located errors at the call,
 * counted, ODIN3_ERR_PARSE: `cannot open 'f' for $readmemh`, `bad address '@zz' in 'f' line N`,
 * `bad word 'g' in 'f' line N`, `'f' is larger than N bytes (the $readmem limit)` (point (aa)),
 * and the shape errors of the arguments. ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG (logged) for a
 * NULL argument or a node that is not a system TASK_CALL.
 */
odin3_status odin3_elab2_readmem(const odin3_elab2 *e2, odin3_ast_id call,
                                 const odin3_ceval_store *store);

/* --- stage 2: memories (Task 20; EL-27, EL-39 reads) ---------------------------------------- */

/* One asynchronous read port of a memory, in source order. */
typedef struct odin3_elab_mem_rd_port {
    odin3_sig addr;           /* the raw index, or the row-major address of a flattened array */
    const odin3_net_id *data; /* the RD_DATA nets (the word's width of them, scratch arena) */
    odin3_ast_id node;        /* the word select that read it */
} odin3_elab_mem_rd_port;

/* One clocked write port of a memory, in source order (pg_memw.c adds them). */
typedef struct odin3_elab_mem_wr_port {
    odin3_sig addr, data, en; /* addr at its own width; data and en one bit per word bit */
    odin3_net_id clk;
    bool posedge;
    odin3_ast_id stmt;
} odin3_elab_mem_wr_port;

/*
 * An array of the module being emitted (EL-27): one memory (a `$mem`) or one register per word.
 * The table `odin3_elab2.mems` is sorted by symbol (declaration order), so a lookup by symbol is a
 * binary search. width, size, offset and flat follow the declaration: width the word's packed
 * width, size the number of words (the product of the dimensions' sizes), offset the lowest
 * declared index of a one-dimensional array (0 for a flattened multi-dimensional one, `flat`),
 * abits the widest address of any port so far (at least 1).
 */
typedef struct odin3_elab_mem {
    odin3_sym_id sym;
    bool to_regs;
    uint32_t width, size, abits;
    int32_t offset;
    bool flat;
    odin3_vec rd_ports, wr_ports;
    odin3_value init; /* the initial contents (EL-40, Task 24): empty BITS = all x */
    /* the fields the brief did not list (agent default) */
    bool is_signed;
    bool failed;   /* an error was reported for the array (no second one for its accesses) */
    uint32_t name; /* strtab ID of the array's IR name: the MEMID, and the base of word names */
    uint32_t n_dims;
    odin3_vec words; /* odin3_wire_id of word position 0 .. size - 1 (registers per word) */
} odin3_elab_mem;

/* The memory at table index mem; NULL outside the table. */
const odin3_elab_mem *odin3_elab2_mem_get(const odin3_elab2 *e2, uint32_t mem);

/*
 * The mem2reg decision of EL-27 for the module being emitted (EL-28 step (2), after the pre-scan):
 * one table entry per array (a declarator with unpacked dimensions, in the module's, generate and
 * named-block scopes; a function's or task's are not the module's), then the rules in Yosys's
 * mem2reg_as_needed order: (1) a net array, `(* mem2reg *)` on the array or the module; (2) a write
 * in an `always` whose event list has not exactly one edge event; (3) a read after a blocking
 * write in the same block; (4) the array inside an lvalue's index or in a concatenation lvalue;
 * (5) every write outside `initial` has a constant index. `(* nomem2reg *)` on the array or its
 * module exempts rules 2-5; `--nomem2reg` (opts.nomem2reg) disables them all, rule 1 included,
 * and an array it leaves that is assigned by `assign` or in such a block is the located error
 * `array 'w' cannot be a memory (it is written in a combinational block) and --nomem2reg is set`.
 * Rules 2-5 warn at the declarator, `replacing memory 'm' with list of registers; see f.v:12`,
 * naming the places Yosys names (the first write of each kind, each block's first blocking write,
 * the first read after it, the first complex lvalue), newest first. An array of SIZE x WIDTH bits
 * above the memory-bits cap is the fatal `memory 'm' of N bits exceeds the limit of M bits`. An
 * array that becomes registers makes its words as wires named `m[k]` (k the declared index; a
 * flattened array's the row-major position), with a SOURCE record at the declarator, the
 * declaration's attributes and the net-bits cap. ODIN3_OK even after located errors;
 * ODIN3_ERR_PARSE when a fatal cap stopped the stage; ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG
 * for a module that is not the one being emitted.
 */
odin3_status odin3_elab2_mem_decide(odin3_elab2 *e2, odin3_ast_id module_node);

/* How the word SELECT chain of a node names a table entry. */
typedef struct odin3_elab2_wordref {
    uint32_t mem; /* the table index */
    uint32_t n;   /* the number of indices: the array's dimensions */
} odin3_elab2_wordref;

/*
 * True when node is a SELECT whose chain of bit selects down to an IDENT or HIER_NAME is exactly as
 * deep as the unpacked dimensions of the array that name is (a word `m[i]`, `m[i][j]`), and the
 * array is in the table; then *ref says which. A chain with a part select in it, a deeper chain
 * (the outer selects are selects of the word) and a shallower one are not words.
 */
bool odin3_elab2_mem_word_of(const odin3_elab2 *e2, odin3_ast_id node, odin3_elab2_wordref *ref);

/* The index expression of dimension dim (0: the first, outermost) of the word node names. */
odin3_ast_id odin3_elab2_mem_word_index(const odin3_elab2 *e2, odin3_ast_id node, uint32_t dim);

/*
 * One read of a word. select is the word SELECT (provenance, the located messages); idx are the
 * index signals of its dimensions, as generated at their self-determined width and sign (n_idx of
 * them, the array's dimensions).
 */
typedef struct odin3_elab2_mem_rd {
    uint32_t mem;
    odin3_ast_id select;
    const odin3_sig *idx;
    uint32_t n_idx;
} odin3_elab2_mem_rd;

/*
 * Adds an asynchronous read port to the memory (not registers) and gives its data: a signal of
 * the word's width on fresh nets that the `$mem` drives at step (8) (odin3_elab2_mem_emit). The
 * port's address is the raw index of a one-dimensional array (`$mem` subtracts OFFSET itself,
 * simlib; Yosys's front end puts the raw index on its ports), at the width the index was generated
 * at; a flattened array's is the row-major address Yosys builds, (i - low_i) * n_j + (j - low_j)
 * with a descending dimension counting from its left bound, as word cells of the widest operand
 * (constant operands are folded). ABITS becomes the widest address. The pins cap of point (aa) is
 * checked before the port is added (`cell of N pins exceeds the limit of M pins`, fatal).
 * ODIN3_ERR_PARSE after that; ODIN3_ERR_NO_MEMORY with the IR unchanged but for constant nets;
 * ODIN3_ERR_INVALID_ARG (logged) for a memory that is registers or a wrong index count.
 */
odin3_status odin3_elab2_mem_read(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, odin3_sig *data);

/*
 * The address of the word rd names (rd->select and the index signals): the raw index of a
 * one-dimensional array, a flattened one's row-major address (cells for non-constant indices,
 * folded constants), as odin3_elab2_mem_read puts on a read port. The memory is a `$mem` (not
 * registers). ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG (logged) for a wrong index count.
 */
odin3_status odin3_elab2_mem_addr(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, odin3_sig *out);

/*
 * The word position (row-major, from 0) of the declared indices values[0 .. n_dims) of the array at
 * table index mem, or -1 outside the array (or for a mem outside the table). Registers or memory.
 */
int64_t odin3_elab2_mem_pos(const odin3_elab2 *e2, uint32_t mem, const int64_t *values);

/* What a word of an array that became registers is. */
typedef enum odin3_elab2_word_kind {
    ODIN3_WORD_WIRE,   /* the word's wire */
    ODIN3_WORD_MEMORY, /* a word of a memory in a procedural target: a write port (Task 24) */
    ODIN3_WORD_X, /* outside the array, or an index with an x or z bit: reads x, writes nothing */
    ODIN3_WORD_VARIABLE /* an index that is not constant */
} odin3_elab2_word_kind;

typedef struct odin3_elab2_word {
    odin3_elab2_word_kind kind;
    odin3_wire_id wire; /* ODIN3_WORD_WIRE */
} odin3_elab2_word;

/*
 * The word of a register array that rd's constant indices select (rd->idx constant signals):
 * the word's wire; outside the array x with the warning `word select [7] of 'w' is out of bounds;
 * the word is x` (`assignment to word [7] outside array 'w' is ignored` when write is set), and an
 * index with an x or z bit x silently; any index that is not constant is ODIN3_WORD_VARIABLE
 * (nothing logged: the caller reports it, EL-27 / Task 23). ODIN3_ERR_INVALID_ARG (logged) for a
 * memory that is not registers or a wrong index count.
 */
odin3_status odin3_elab2_mem_word(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, bool write,
                                  odin3_elab2_word *out);

/*
 * One clocked write port (EL-39; pg_memw.c adds them from processes, in source order): the word at
 * addr (a signal at its own width, constant or not; no temporary is needed for a constant index
 * or a full-word value) takes data where en is 1, on the clk edge (posedge: rising, else falling).
 * addr, data and en are copied; data and en are as wide as the word. ABITS becomes the widest
 * address. Caps and statuses as odin3_elab2_mem_read.
 */
typedef struct odin3_elab2_mem_wr {
    uint32_t mem;
    const odin3_sig *addr, *data, *en;
    odin3_net_id clk;
    bool posedge;
    odin3_ast_id stmt;
} odin3_elab2_mem_wr;

odin3_status odin3_elab2_mem_write(odin3_elab2 *e2, const odin3_elab2_mem_wr *wr);

/*
 * EL-28 step (8): one `$mem` per memory that has a port or initial contents, ports in source order
 * (a read-only, write-only or unused array: an unused one makes none). MEMID = the array's IR name,
 * WIDTH, SIZE, OFFSET and ABITS as the table says, RD_PORTS and WR_PORTS the port counts, reads
 * asynchronous (RD_CLK_ENABLE, RD_CLK_POLARITY and RD_TRANSPARENT zero, RD_CLK the x net, RD_EN
 * the constant 1), writes clocked (WR_CLK_ENABLE one, WR_CLK_POLARITY from the edge), INIT the
 * initial contents. Addresses are zero-extended to ABITS (Yosys's memory_collect); every net
 * recorded is followed through merges (odin3_elab2_resolve). The node has a SOURCE record at the
 * array's declarator and the declaration's attributes. Status as odin3_elab2_module.
 */
odin3_status odin3_elab2_mem_emit(odin3_elab2 *e2);

/* Forgets the memory table (a new module starts); frees it when e2 is destroyed. */
void odin3_elab2_mem_reset(odin3_elab2 *e2);
void odin3_elab2_mem_free(odin3_elab2 *e2);

/* --- stage 2: processes (Task 21; EL-28 step (6), EL-33, EL-34, EL-35; the generator is pg.h) ---
 */

/* True when net is a bit of a wired net (`wand`, `wor`, `triand`, `trior`) whose drivers the
 * module collects (EL-26); a process cannot drive one (the pre-scan reported it). */
bool odin3_elab2_net_is_wired(const odin3_elab2 *e2, odin3_net_id net);

/*
 * EL-28 step (2), second half: the always-assigned bits of the word wires that the mem2reg decision
 * made after the pre-scan (EL-27): scans the module's `always` blocks again, and for each
 * assignment to a word of a register array (a bit or part select of the word too) marks the bits
 * of that word's wire, or of every word's wire when an index is not a NUMBER (a conservative
 * superset). ODIN3_OK even after located errors; ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a
 * module that is not the one being emitted.
 */
odin3_status odin3_elab2_prescan_words(odin3_elab2 *e2, odin3_ast_id module_node);

/* True when an always block assigns bit `bit` of the wire of a register-array word (false before
 * odin3_elab2_prescan_words, and for any wire that is not one). */
bool odin3_elab2_always_assigned_word(const odin3_elab2 *e2, odin3_wire_id wire, uint32_t bit);

/* Frees the process generator (a new module starts); frees it when e2 is destroyed. */
void odin3_elab2_pg_free(odin3_elab2 *e2);

/* --- user function and task calls (Task 23; EL-17, EL-38; elab2_call.c, pg_call.c) ---------------
 */

/*
 * The type of a user function CALL as the typer asks for it: the function's result variable (its
 * return range, 32 bits for `integer`, 1 bit by default; signed per `signed`/`integer`). A call
 * of an undeclared or non-function name, or of a system function, is a located error and
 * ODIN3_ERR_PARSE.
 */
odin3_status odin3_elab2_call_type(odin3_elab2 *e2, odin3_ast_id call, odin3_etype *out);

/* The FUNCTION_DECL (a CALL) or TASK_DECL (a TASK_CALL) a call names; a located error and
 * ODIN3_ERR_PARSE for an undeclared name, a name of the other kind or a system function. */
odin3_status odin3_elab2_callee(odin3_elab2 *e2, odin3_ast_id call, odin3_ast_id *decl);

/* The result variable of the FUNCTION_DECL decl (its symbol in *sym); NULL when it has none. */
const odin3_symbol *odin3_elab2_fn_result(const odin3_elab2 *e2, odin3_ast_id decl,
                                          odin3_sym_id *sym);

/* The interpreter of the module's constants (EL-17, EL-40), made on first use over the elaborated
 * store. ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_elab2_ceval_get(odin3_elab2 *e2, odin3_ceval **out);

/*
 * Tries the interpreter (EL-17) on a user function CALL: *is_const is true and *out holds the value
 * (the result's own width, in the scratch arena) when the call is constant (constant arguments and
 * a constant body); otherwise OK with *is_const false and nothing logged. A located error of the
 * call itself (`function 'f' has 1 inputs; 2 arguments given`) or a cap is reported and
 * ODIN3_ERR_PARSE. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_call_try(odin3_elab2 *e2, odin3_ast_id call, odin3_cval *out,
                                  bool *is_const);

/*
 * A user function call, or a register-array word with a variable index, met outside a process (a
 * continuous assignment, a port connection, a net declaration assignment): a combinational `$proc`
 * of its own (one ALWAYS sync) computes it into the nets of *out (pg_call.c; odin3_pg_comb_call and
 * odin3_pg_comb_mem_read are these on a generator). The arguments and the index are generated
 * first, in the caller's context. The located errors of the call, ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_comb_call(odin3_elab2 *e2, odin3_ast_id call, odin3_sig *out);
odin3_status odin3_elab2_comb_word(odin3_elab2 *e2, const odin3_elab2_regs_rd *rd, odin3_sig *out);

/* The process generator of the module being emitted, made on first use (Task 21). */
odin3_status odin3_elab2_pg_get(odin3_elab2 *e2, struct odin3_pg **out);

/* Whether a symbol is a loop variable being unrolled: the AST constant test takes it as a constant
 * (EL-37). */
bool odin3_elab2_is_bound(const odin3_elab2 *e2, odin3_sym_id sym);

#endif
