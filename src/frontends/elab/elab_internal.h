/*
 * elab_internal.h — stage 2's internal interfaces (2D, spec §12, plan point (j)). The first part is
 * the binding (EL-5): the symbol table of an elaborated store and the resolution array stage 2
 * reads, filled by the binder (bind.c), which also checks the rows of the EL-4 form that 2A's check
 * rule 7 does not. The second part (Task 17) is stage 2's context: the IR modules, their ports and
 * wires, provenance, attributes, the error cap and the byte budget (elab2.c, elab2_wires.c). Later
 * tasks add expressions, drivers, instances, processes and memories to it.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H
#define ODIN3_FRONTENDS_ELAB_ELAB_INTERNAL_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
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
struct odin3_elab2 {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_ast *elab;
    odin3_elab_opts opts;
    const odin3_elab_limits *limits;
    odin3_elab_binding bind;
    odin3_symtab *st;
    odin3_celltype_id types[ODIN3_E2T_COUNT];
    odin3_arena *scratch;  /* signals and attribute values of the module being emitted */
    odin3_u64map *modules; /* MODULE node ID -> IR module ID */
    odin3_u64map *warned;  /* (loc, format) hashes of the warnings given */
    odin3_vec built;       /* odin3_module_id of every module made, in order */
    odin3_strbuf text;     /* a name or path being built */
    odin3_vec stack;       /* odin3_ast_id worklist of the item walk */
    odin3_vec chain;       /* odin3_scope_id, innermost first, of a scope path being built */
    /* the module being emitted */
    odin3_module *mod;
    odin3_module_id mod_id;
    odin3_ast_id mod_node;
    uint32_t mod_name; /* strtab ID of the specialization's name */
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
    bool pend_seen[ODIN3_PEND_END];
};

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
 * ATTR `blackbox`) makes no module: Task 19 registers its cell type. A module that cannot be made
 * (its name is a cell type already, or the IR refuses a port) is a located error and
 * ODIN3_ERR_PARSE, and the caller stops stage 2. The net-bits cap (point (aa)) and the byte
 * budget apply. ODIN3_ERR_NO_MEMORY and ODIN3_ERR_INVALID_ARG (a node that is not a MODULE of
 * the store, or already made) as usual; the IR stays valid.
 */
odin3_status odin3_elab2_ports(odin3_elab2 *e2, odin3_ast_id module_node);

/*
 * Emits the body of the module whose ports odin3_elab2_ports made: calls the on_body_start test
 * hook, checks the budget, then EL-28's steps in order (Task 17: (1) the wires; every construct
 * of steps (2)–(8) whose elaboration has not landed is point (l)'s located error, once per kind,
 * and is skipped, so the rest of the module is emitted). ODIN3_OK even after located errors (they
 * are counted); ODIN3_ERR_PARSE when the stage must stop (the error cap, a fatal cap, the
 * budget); ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG for a node whose ports were not made. A
 * black-box MODULE is skipped.
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

/*
 * The SOURCE record of EL-45 for the request (odin3_prov_source, so hash-consed): locs = the
 * node's {loc, end} then the call's when there is one (odin3_srcman_srcloc, raw loc kept), ast =
 * node, hier = `<specialization name>/<scope path>` with each generate or named block scope as
 * `name` or `name[index]` (the module's own scope: the name alone). ODIN3_ERR_NO_MEMORY;
 * ODIN3_ERR_INVALID_ARG (logged) with no module being emitted.
 */
odin3_status odin3_elab2_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req, odin3_prov_id *out);

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

/* True once the stage was stopped (the cap, a fatal cap, the budget). */
bool odin3_elab2_stopped(const odin3_elab2 *e2);

#endif
