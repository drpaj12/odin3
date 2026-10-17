/*
 * lower_int.h — the internals of `lower` shared by its files (3B plan Task 1, spec §3, §12, §13):
 * the context, net vectors, the gate helpers every builder uses, the pairing tree, the per-cell
 * transaction (commit and rollback), the size model and the dispatch table. Private to
 * src/passes/lower.
 *
 * A builder is `odin3_status build_X(lw_ctx *lw, const lw_cell *cell, lw_bits *out)`: a
 * straight-line function over the cell's operand nets that appends bit nodes on fresh nets and
 * puts the net of each output bit into `out`. It creates nodes only through the lw_gate_* helpers
 * (and lw_const / lw_net), which are its only allocation points, so an out-of-memory test hooks
 * them. The driver (lower.c) wraps each builder in one transaction: lw_commit deletes the word
 * node and merges each output net with the builder's net; lw_rollback removes everything a failed
 * builder made.
 */
#ifndef ODIN3_PASSES_LOWER_LOWER_INT_H
#define ODIN3_PASSES_LOWER_LOWER_INT_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/lower/lower.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The widest cell type `lower` meets: $mem has 8 ports. A log line's cell label fits LW_LABEL_LEN.
 */
enum { LW_MAX_PORTS = 8, LW_LABEL_LEN = 64 };

/* A read-only run of nets (an operand's bits, LSB first). A net ID of 0 is an open pin. */
typedef struct lw_span {
    const odin3_net_id *nets;
    uint32_t n;
} lw_span;

/* A growable vector of net IDs: the working form of operands and results. */
typedef struct lw_bits {
    odin3_vec v; /* odin3_net_id */
} lw_bits;

void lw_bits_init(lw_bits *bits);
void lw_bits_free(lw_bits *bits);
void lw_bits_clear(lw_bits *bits);
/* NO_MEMORY leaves the vector unchanged. */
odin3_status lw_bits_push(lw_bits *bits, odin3_net_id net);
uint32_t lw_bits_len(const lw_bits *bits);
/* Replaces bit `idx` (< lw_bits_len). */
void lw_bits_set(lw_bits *bits, uint32_t idx, odin3_net_id net);
odin3_net_id lw_bits_at(const lw_bits *bits, uint32_t idx);
lw_span lw_bits_span(const lw_bits *bits);

/* Parameter indices (celltype definition order): unary, binary. */
enum {
    LW_P_UN_SIGNED = 0,
    LW_P_UN_AW = 1,
    LW_P_UN_YW = 2,
    LW_P_BIN_ASIGNED = 0,
    LW_P_BIN_BSIGNED = 1,
    LW_P_BIN_AW = 2,
    LW_P_BIN_BW = 3,
    LW_P_BIN_YW = 4
};

/* The cell being lowered: a read-only view of its type, parameters and port nets. */
typedef struct lw_cell {
    odin3_node_id node;
    const odin3_celltype_def *def;
    const odin3_value *params;
    uint32_t n_ports;
    uint32_t out_port;          /* the index of the cell's only output port */
    lw_span port[LW_MAX_PORTS]; /* input ports: open pins are fresh undriven nets (spec §8); the
                                 * output port: the nets as they are, 0 for an open pin */
} lw_cell;

/* The bit gates and storage cells the builders create (resolved once per design). */
typedef enum lw_gate {
    LW_G_NOT,
    LW_G_AND,
    LW_G_OR,
    LW_G_XOR,
    LW_G_XNOR,
    LW_G_MUX,
    LW_G_DFF_P,
    LW_G_DFF_N,
    LW_G_DLATCH_P,
    LW_G_DLATCH_N,
    LW_G_COUNT
} lw_kind;

/* The constants a builder may need: $_CONST0_, $_CONST1_, $_CONSTX_ (odin3_bit 0, 1, x). */
enum { LW_N_CONST = 3 };

/* One cached canonical constant net; fresh while the cell that created it is open. */
typedef struct lw_const_slot {
    odin3_net_id net;
    bool fresh;
} lw_const_slot;

/* The lists of the --report file (spec §10), in the order of the JSON keys. */
typedef enum lw_report_kind {
    LW_REP_ASYNC,
    LW_REP_NEG_EDGE,
    LW_REP_MIXED_SIGN,
    LW_REP_MEM,
    LW_REP_COUNT
} lw_report_kind;

/* One report entry: the module and cell (names or "$c<ID>"), the cell type, the bits it covers
 * (0: not applicable) and, for the async list, the controls ("ARST", ...; NULL: none). */
typedef struct lw_report_entry {
    const char *module;
    const char *cell;
    const char *type;
    uint64_t bits;
    const char *controls;
} lw_report_entry;

/* The types of the soft-logic INFO line (spec §10): $add, $mem, $mul, $sub. */
enum { LW_N_SOFT = 4 };

/* Totals the pass logs. */
typedef struct lw_totals {
    uint64_t cells;           /* word cells lowered */
    uint64_t nodes;           /* bit nodes created (constants included) */
    uint64_t modules;         /* modules with at least one word cell */
    uint64_t mixed_sign;      /* cells warned of for A_SIGNED != B_SIGNED */
    uint64_t soft[LW_N_SOFT]; /* cells of $add, $mem, $mul, $sub (the soft-logic INFO line) */
} lw_totals;

typedef struct lw_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    const odin3_lower_opts *opts;
    odin3_celltype_id gate[LW_G_COUNT];
    odin3_celltype_id constant[LW_N_CONST];
    lw_const_slot const_slot[LW_N_CONST];
    odin3_prov_id prov;            /* the current cell's DERIVED record (0: none) */
    odin3_vec created;             /* odin3_node_id: the nodes of the open cell, creation order */
    odin3_vec created_nets;        /* odin3_net_id: the nets it made */
    odin3_vec portnets;            /* odin3_net_id: scratch behind lw_cell.port */
    odin3_vec merged;              /* odin3_net_pair: the merges of the open commit */
    const odin3_value *keep_value; /* the word cell's `keep` attribute, or NULL */
    uint32_t keep_key;             /* strtab ID of "keep", 0 when never interned */
    uint64_t gates;                /* gate nodes the open cell has created (constants excluded) */
    uint64_t cap_cell;             /* the per-cell cap in force */
    uint64_t total_created;        /* nodes created since the run began, for the budget cadence */
    uint64_t next_budget;          /* total_created at which the budget is checked next */
    uint64_t live_nodes;           /* live nodes of the module (kept up to date) */
    lw_totals totals;
    odin3_strbuf namebuf;              /* scratch: a bit node's name being built */
    odin3_node_id cur;                 /* the open cell's word node */
    odin3_strbuf report[LW_REP_COUNT]; /* JSON array bodies, only filled when a path is given */
    uint32_t report_count[LW_REP_COUNT];
} lw_ctx;

/* --- the context (kit.c) ------------------------------------------------------------------ */

/* Zeroes *lw and resolves the gate and constant types; allocates nothing. INVALID_ARG (logged) for
 * a design whose registry lacks one of them. */
odin3_status lw_ctx_init(lw_ctx *lw, odin3_pass_ctx *pass, odin3_design *design,
                         const odin3_lower_opts *opts);
void lw_ctx_free(lw_ctx *lw);

/*
 * Points the context at a module: finds its canonical constant nets (the first live constant node
 * of each value whose Y is connected) and counts its live nodes.
 */
void lw_ctx_enter(lw_ctx *lw, odin3_module *module);

/*
 * Fills *cell for `node` of the current module: type, parameters and the nets of every port.
 * Input pins that are open get a fresh undriven net each (spec §8; recorded for rollback, so call
 * it after lw_begin). The cell must have exactly one output port, else INVALID_ARG (logged).
 */
odin3_status lw_load_cell(lw_ctx *lw, odin3_node_id node, lw_cell *cell);

/* "<name>" of node, else "$c<ID>" (spec §10), into out (size bytes, truncated). */
void lw_node_label(const lw_ctx *lw, odin3_node_id node, char *out, size_t size);

/* --- nets, gates, constants (kit.c) ------------------------------------------------------- */

/* A fresh unnamed net with the open cell's record; recorded for rollback. */
odin3_status lw_net(lw_ctx *lw, odin3_net_id *out);

/* The canonical constant net of bit (0, 1 or x): the module's first live constant node of that
 * value whose Y is connected, else a new node on a new net (counted in `created`). */
odin3_status lw_const(lw_ctx *lw, odin3_bit bit, odin3_net_id *out);

/* One gate of the given kind on `in[0 .. n_in)` (input ports in order) driving a fresh net
 * *out_net. */
odin3_status lw_gate(lw_ctx *lw, lw_kind kind, const odin3_net_id *in, uint32_t n_in,
                     odin3_net_id *out_net);

odin3_status lw_not(lw_ctx *lw, odin3_net_id src, odin3_net_id *out_net);
odin3_status lw_and(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net);
odin3_status lw_or(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net);
odin3_status lw_xor(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net);
odin3_status lw_xnor(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out_net);
/* Y = sel ? rhs : lhs (`$_MUX_`: A when S is 0). */
odin3_status lw_mux(lw_ctx *lw, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id sel,
                    odin3_net_id *out_net);

/*
 * The signedness `lower` extends a cell's operands by (spec §3.3, PHASE2 #25): A_SIGNED alone for a
 * unary cell, A_SIGNED && B_SIGNED for a binary one. A mixed cell (A_SIGNED != B_SIGNED) is read
 * as unsigned and warns here, once per call: `lower: module 'm': cell '<type>' '<cell>' has
 * A_SIGNED != B_SIGNED: treated as unsigned (both-signed rule, Yosys simlib; PHASE2 #25)`. False
 * for a cell without an A_SIGNED parameter.
 */
bool lw_both_signed(lw_ctx *lw, const lw_cell *cell);

/*
 * Yosys's `sig.extend_u0(width, is_signed)` (spec §3.3) into *out (cleared first): the first
 * min(n, width) bits of `in`, then, up to `width`, the top bit repeated when is_signed and the
 * constant-0 net otherwise. `in` must not be empty.
 */
odin3_status lw_extend(lw_ctx *lw, lw_span in, uint32_t width, bool is_signed, lw_bits *out);

/*
 * The pairing tree of simplemap_reduce (spec §4): while more than one net is left, pair adjacent
 * nets (0,1), (2,3), … with `kind` (LW_G_AND, LW_G_OR or LW_G_XOR) into a new level; an odd last
 * net is appended after the pair results. *root gets the single net left; a one-net input creates
 * no node. `in` must not be empty.
 */
odin3_status lw_tree(lw_ctx *lw, lw_kind kind, lw_span in, odin3_net_id *root);

/* --- the transaction (connect.c) ---------------------------------------------------------- */

/* Begins the cell's transaction: clears the created lists, begins a provenance operation, makes
 * the DERIVED record of the cell and reads its `keep`. NO_MEMORY with nothing begun. */
odin3_status lw_begin(lw_ctx *lw, odin3_node_id node);

/*
 * Merges `drop` into `keep` (spec §3.2, kit_connect's rule): drop's pins move to keep, drop's
 * name, wire bits and aliases become aliases of keep. A no-op when keep == drop (the output fed
 * back to its own input: it is left undriven) or either is none. NO_MEMORY as odin3_net_merge.
 */
odin3_status lw_connect(lw_ctx *lw, odin3_net_id keep, odin3_net_id drop);

/*
 * Commits the open cell: deletes `cell->node`, then merges, bit by bit, the net of its output
 * port (the last port) with outs[k] (keep = outs[k]). `outs` has one entry per output bit. A
 * failure of the delete changes nothing (the caller rolls back); a failure of a merge leaves the
 * bits merged so far connected and the others undriven (logged), and is returned.
 */
odin3_status lw_commit(lw_ctx *lw, const lw_cell *cell, const lw_bits *outs);

/* Removes the nodes and nets the open cell made (reverse order); the constants it created go too.
 * Best effort under repeated out-of-memory: a node that cannot be deleted is disconnected and
 * left floating. */
void lw_rollback(lw_ctx *lw);

/* Ends the transaction after a successful commit: the created constants become permanent. */
void lw_end(lw_ctx *lw);

/* --- the report (report.c) ---------------------------------------------------------------- */

/* Appends an entry to list `kind` when the run has a report path (a no-op otherwise). NO_MEMORY. */
odin3_status lw_report_add(lw_ctx *lw, lw_report_kind kind, const lw_report_entry *entry);

/* Writes the JSON file of the lists to the run's report path, atomically; a no-op without a path.
 */
odin3_status lw_report_write(lw_ctx *lw);

/* --- the size model (size.c) -------------------------------------------------------------- */

/*
 * The pre-flight of spec §11: `estimate` (an upper bound on the gate nodes the rule creates, from
 * the widths alone) against the per-cell cap and, with the module's live nodes, the per-module
 * cap. Over a cap: logs `lower: module 'm': '<type>' '<cell>' would create N nodes (cap C)` and
 * returns INVALID_ARG; nothing has been built.
 */
odin3_status lw_check_size(const lw_ctx *lw, const lw_cell *cell, uint64_t estimate);

/* The sum of the port widths a formula reads; saturates instead of wrapping. */
uint64_t lw_sat_add(uint64_t lhs, uint64_t rhs);
uint64_t lw_sat_mul(uint64_t lhs, uint64_t rhs);

/* Yosys's $clog2: the smallest n with 2^n >= value (0 for 0 and 1). */
uint32_t lw_clog2(uint64_t value);

/* --- the rules (bitwise.c; later tasks add the others) --------------------------------------- */

#define LW_DECLARE_BUILDER(name)                                                                   \
    odin3_status lw_build_##name(lw_ctx *lw, const lw_cell *cell, lw_bits *out);                   \
    uint64_t lw_est_##name(const lw_cell *cell)

LW_DECLARE_BUILDER(not );
LW_DECLARE_BUILDER(and);
LW_DECLARE_BUILDER(or);
LW_DECLARE_BUILDER(xor);
LW_DECLARE_BUILDER(xnor);
LW_DECLARE_BUILDER(mux);
LW_DECLARE_BUILDER(reduce_and);
LW_DECLARE_BUILDER(reduce_or);
LW_DECLARE_BUILDER(reduce_xor);
LW_DECLARE_BUILDER(reduce_xnor);
LW_DECLARE_BUILDER(logic_not);
LW_DECLARE_BUILDER(logic_and);
LW_DECLARE_BUILDER(logic_or);
LW_DECLARE_BUILDER(eq);
LW_DECLARE_BUILDER(ne);

/* --- pmux_shift.c: $pmux, $shl $shr $sshr, $shiftx (spec §5.1 to §5.3) ------------------------ */

LW_DECLARE_BUILDER(pmux);
LW_DECLARE_BUILDER(shl);
LW_DECLARE_BUILDER(shr);
LW_DECLARE_BUILDER(sshr);
LW_DECLARE_BUILDER(shiftx);

/* --- alu.c: the carry-lookahead unit, the adder, $add $sub $neg (spec §5.4) ------------------- */

/*
 * techmap.v `_90_lcu_brent_kung`: the carries CO[0 .. W) of the propagate bits `prop` and generate
 * bits `gen` (W bits each, W >= 1) and the carry-in: g[0] |= p[0] & CI, the main tree for
 * i = 1 .. clog2(W) at j = 2^i - 1 step 2^i, then the inverse tree for i = clog2(W) .. 1 at
 * j = 2^i + 2^(i-1) - 1 step 2^i. *out is cleared first. Gates: lw_lcu_gates(W).
 */
odin3_status lw_lcu(lw_ctx *lw, lw_span prop, lw_span gen, odin3_net_id carry_in, lw_bits *out);
uint64_t lw_lcu_gates(uint64_t width);

/* The inputs of lw_alu: A + (BI ? ~B : B) + CI at `width` bits, each operand extended to it. */
typedef struct lw_alu_args {
    lw_span a;
    lw_span b;
    odin3_net_id carry_in;
    bool invert_b; /* BI: B is inverted (a $_NOT_ row) */
    bool is_signed;
    bool need_sum; /* false: the sum bits are not built (the comparisons do not read them) */
    uint32_t width;
} lw_alu_args;

/* The outputs of lw_alu, `width` bits each (sum: none unless need_sum): Y, X (P = AA ^ BB), CO. */
typedef struct lw_alu_out {
    lw_bits sum;
    lw_bits prop;
    lw_bits carry;
} lw_alu_out;

void lw_alu_out_init(lw_alu_out *res);
void lw_alu_out_free(lw_alu_out *res);

/* techmap.v `_90_alu` (P = AA ^ BB, G = AA & BB, CO = lcu, Y = P ^ {CO, CI}). Previous contents of
 * *res are dropped. */
odin3_status lw_alu(lw_ctx *lw, const lw_alu_args *args, lw_alu_out *res);
uint64_t lw_alu_gates(uint64_t width, bool invert_b, bool need_sum);

/* Y[0] = root, Y[k > 0] = the constant-0 net: the result shape of a one-bit cell. */
odin3_status lw_finish_flag(lw_ctx *lw, const lw_cell *cell, odin3_net_id root, lw_bits *out);

LW_DECLARE_BUILDER(add);
LW_DECLARE_BUILDER(sub);
LW_DECLARE_BUILDER(neg);

/* 0 - src at `width` bits (src extended by is_signed) into *out (cleared first): the $neg rule.
 * Gates: lw_alu_gates(width, true, true). */
odin3_status lw_neg_bits(lw_ctx *lw, lw_span src, uint32_t width, bool is_signed, lw_bits *out);

/* --- cmp.c: $lt $le $gt $ge (spec §5.4) ---------------------------------------------------------
 */

LW_DECLARE_BUILDER(lt);
LW_DECLARE_BUILDER(le);
LW_DECLARE_BUILDER(gt);
LW_DECLARE_BUILDER(ge);

/* The four comparisons, in the order of cmp_estimate's arithmetic (LT 0, LE 1, GT 2, GE 3). */
typedef enum lw_cmp_kind { LW_CMP_LT, LW_CMP_LE, LW_CMP_GT, LW_CMP_GE } lw_cmp_kind;

/* The operands of lw_compare: each extended to the wider one's width by is_signed. */
typedef struct lw_cmp_args {
    lw_span a;
    lw_span b;
    lw_cmp_kind kind;
    bool is_signed;
} lw_cmp_args;

/* The flag net of `a <kind> b` (the subtraction without sum bits, alumacc.cc); the rule of
 * $lt $le $gt $ge, shared with the divider. Gates: lw_compare_gates. */
odin3_status lw_compare(lw_ctx *lw, const lw_cmp_args *cmp, odin3_net_id *flag);

/* The shape lw_compare_gates counts: the operand width (the wider one) and the flavour. */
typedef struct lw_cmp_shape {
    uint64_t width;
    lw_cmp_kind kind;
    bool is_signed;
} lw_cmp_shape;
uint64_t lw_compare_gates(const lw_cmp_shape *shape);

/* --- mul.c: the bit heap, the full adder, $mul (spec §5.5) --------------------------------------
 */

/* One column of the bit heap: nets in ascending ID order, `head` the first one still in it. */
typedef struct lw_column {
    lw_bits nets;
    uint32_t head;
} lw_column;

/* maccmap.cc's `bits`: one set of nets per column (weight 2^column). */
typedef struct lw_heap {
    odin3_vec cols; /* lw_column */
    odin3_net_id zero;
} lw_heap;

void lw_heap_init(lw_heap *heap);
void lw_heap_free(lw_heap *heap);
/* Empties the heap and gives it `width` columns; nets equal to `zero` are never added. */
odin3_status lw_heap_reset(lw_heap *heap, uint32_t width, odin3_net_id zero);
uint32_t lw_heap_width(const lw_heap *heap);
uint32_t lw_heap_depth(const lw_heap *heap, uint32_t pos);
/* The smallest net ID of the column (its depth must be > 0), removed. */
odin3_net_id lw_heap_pop(lw_heap *heap, uint32_t pos);
/*
 * maccmap.cc `add(bit, position)`: nothing for a position past the width or for the zero net; a
 * net the column already holds leaves it and is added to the next column instead (cancel and
 * carry, an explicit loop); else the net joins the column. NO_MEMORY leaves the heap valid.
 */
odin3_status lw_heap_add(lw_heap *heap, odin3_net_id net, uint32_t pos);

/* techmap.v `_90_fa`: sum = a ^ b ^ c = out[0], carry = ab | c(a ^ b) = out[1]; 5 gates. */
odin3_status lw_fa(lw_ctx *lw, const odin3_net_id in[3], odin3_net_id out[2]);

LW_DECLARE_BUILDER(mul);

/* --- div.c: the restoring divider, $div, $mod (spec §5.6) ---------------------------------------
 */

/* The outputs of lw_divmod_u: W quotient bits and, when asked for, W remainder bits. */
typedef struct lw_divmod_out {
    lw_bits quot;
    lw_bits rem;
} lw_divmod_out;

void lw_divmod_out_init(lw_divmod_out *res);
void lw_divmod_out_free(lw_divmod_out *res);

/*
 * techmap.v `$__div_mod_u`: A / B and A % B for unsigned operands of the same width W >= 1, W
 * stages. Stage i compares the running remainder with {B, W-1-i zeros}, sets quotient bit W-1-i to
 * the result and keeps the difference when it is set. B = 0 gives quotient all ones and remainder
 * A. Without `want_rem` the last stage's difference is not built. Gates: lw_divmod_u_gates.
 */
typedef struct lw_divmod_args {
    lw_span dividend;
    lw_span divisor;
    bool want_rem;
} lw_divmod_args;
odin3_status lw_divmod_u(lw_ctx *lw, const lw_divmod_args *args, lw_divmod_out *res);
uint64_t lw_divmod_u_gates(uint64_t width, bool want_rem);

LW_DECLARE_BUILDER(div);
LW_DECLARE_BUILDER(mod);

/* --- dispatch (dispatch.c) ---------------------------------------------------------------- */

typedef odin3_status (*lw_build_fn)(lw_ctx *lw, const lw_cell *cell, lw_bits *out);
typedef uint64_t (*lw_estimate_fn)(const lw_cell *cell);

/* A rule: the cell type's name, its builder and its estimate. */
typedef struct lw_rule {
    const char *type_name;
    lw_build_fn build;
    lw_estimate_fn estimate;
} lw_rule;

/* The rule for a cell type name, NULL when there is none. */
const lw_rule *lw_rule_find(const char *type_name);

/* Number of rules and rule `idx` (for the completeness test). */
uint32_t lw_rule_count(void);
const lw_rule *lw_rule_at(uint32_t idx);

/*
 * Word types the registry has and `lower` deliberately has no rule for (spec §5.7, §8): $proc,
 * $tribuf, $pow, $memrd, $memwr. lw_no_rule_listed says so; lw_log_no_rule logs the error of the
 * type (its own text, else "no lowering rule for cell type"). `pending` types get their rule in a
 * later task of the plan (lw_rule_pending); the completeness test reads both lists, so a cell type
 * added to the registry later fails it.
 */
bool lw_no_rule_listed(const char *type_name);
bool lw_rule_pending(const char *type_name);
void lw_log_no_rule(const lw_ctx *lw, odin3_node_id node, const char *type_name);

#endif
