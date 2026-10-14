/*
 * expr.h — opt_expr (2F spec §4, OP-4; plan Task 6): the rule enum and names (point (j)), the
 * design-level entry, and the view of one visited cell that the rule files (expr_logic.c,
 * expr_cmp.c, expr_arith.c, expr_mux.c, expr_undriven.c) share with the driver (expr.c) and the
 * helpers (expr_util.c). odin3_opt_expr_run, the module-scope entry for 2E, stays declared in
 * opt.h.
 *
 * Yosys 0.55 (the VTR build, libs/EXTERNAL/yosys/passes/opt/opt_expr.cc) is the oracle: the rule
 * names are its `cover` points, the rules run in its order per cell type, and a visit applies
 * the first rule that fires (Yosys's `goto next_cell`).
 */
#ifndef ODIN3_PASSES_OPT_EXPR_H
#define ODIN3_PASSES_OPT_EXPR_H

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the rules (the counters of --opt-stats; odin3_opt_expr_counts.rule is indexed by them) --- */

typedef enum odin3_opt_rule {
    ODIN3_OPT_RULE_CONST_AND,
    ODIN3_OPT_RULE_CONST_OR,
    ODIN3_OPT_RULE_AND_OR_BUFFER,
    ODIN3_OPT_RULE_CONST_XOR,
    ODIN3_OPT_RULE_CONST_XNOR,
    ODIN3_OPT_RULE_XOR_BUFFER,
    ODIN3_OPT_RULE_XNOR_BUFFER,
    ODIN3_OPT_RULE_REDUCE_XNOR_NOT,
    ODIN3_OPT_RULE_UNARY_BUFFER,
    ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST,
    ODIN3_OPT_RULE_XBIT,
    ODIN3_OPT_RULE_TRIM,
    ODIN3_OPT_RULE_INVERT_DOUBLE,
    ODIN3_OPT_RULE_INVERT_MUXSEL,
    ODIN3_OPT_RULE_GATE_NOT,
    ODIN3_OPT_RULE_GATE_AND,
    ODIN3_OPT_RULE_GATE_OR,
    ODIN3_OPT_RULE_GATE_XOR,
    ODIN3_OPT_RULE_GATE_MUX,
    ODIN3_OPT_RULE_MUX_TO_INV,
    ODIN3_OPT_RULE_TRIBUF_EN1,
    ODIN3_OPT_RULE_TRIBUF_EN0,
    ODIN3_OPT_RULE_EQNEQ_ISNEQ,
    ODIN3_OPT_RULE_EQNEQ_EMPTY,
    ODIN3_OPT_RULE_EQNEQ_RESIZE,
    ODIN3_OPT_RULE_EQNEQ_SWAPCONST,
    ODIN3_OPT_RULE_EQNEQ_UNDEF,
    ODIN3_OPT_RULE_EQNEQ_BUFFER,
    ODIN3_OPT_RULE_EQNEQ_ISNOT,
    ODIN3_OPT_RULE_EQNEQ_CMPZERO,
    ODIN3_OPT_RULE_CONSTSHIFT,
    ODIN3_OPT_RULE_IDENTWRT_A,
    ODIN3_OPT_RULE_IDENTWRT_B,
    ODIN3_OPT_RULE_MUX_BOOL,
    ODIN3_OPT_RULE_MUX_INVERT,
    ODIN3_OPT_RULE_MUX_AND,
    ODIN3_OPT_RULE_MUX_OR,
    ODIN3_OPT_RULE_MUX_UNDEF,
    ODIN3_OPT_RULE_MUX_EMPTY,
    ODIN3_OPT_RULE_MUX_SEL01,
    ODIN3_OPT_RULE_MUX_REDUCE,
    ODIN3_OPT_RULE_MUX_CONST_SEL,
    ODIN3_OPT_RULE_MUX_SAME,
    ODIN3_OPT_RULE_POW_SHIFT,
    ODIN3_OPT_RULE_MUL_SHIFT_ZERO,
    ODIN3_OPT_RULE_MUL_SHIFT_SWAPPED,
    ODIN3_OPT_RULE_MUL_SHIFT_UNSWAPPED,
    ODIN3_OPT_RULE_MUL_LOW_ZEROS,
    ODIN3_OPT_RULE_DIVMOD_ZERO,
    ODIN3_OPT_RULE_DIV_SHIFT,
    ODIN3_OPT_RULE_MOD_MASK,
    ODIN3_OPT_RULE_CLKINV,
    ODIN3_OPT_RULE_FINE_GROUP,
    ODIN3_OPT_RULE_FINE_NEUTRAL_A,
    ODIN3_OPT_RULE_FINE_NEUTRAL_B,
    ODIN3_OPT_RULE_FINE_REDUCE_AND,
    ODIN3_OPT_RULE_FINE_A,
    ODIN3_OPT_RULE_FINE_B,
    ODIN3_OPT_RULE_FINE_ADDSUB,
    ODIN3_OPT_RULE_FINE_EQNE,
    ODIN3_OPT_RULE_FINE_CMP,
    ODIN3_OPT_RULE_UNDRIVEN,
    ODIN3_OPT_RULE_COUNT
} odin3_opt_rule;

_Static_assert((uint32_t)ODIN3_OPT_RULE_COUNT <= (uint32_t)ODIN3_OPT_EXPR_MAX_RULES,
               "every rule has a counter in odin3_opt_expr_counts");

/* The cover name of rule (Yosys's `opt.opt_expr.<name>` point where it has one); NULL for a value
 * that is not a rule. */
const char *odin3_opt_rule_name(odin3_opt_rule rule);

/*
 * opt_expr (2F spec §4) over every module of design, in creation order (odin3_opt_expr_run per
 * module), adding into counts->expr (and counts->fold_type); INFO "opt_expr: N cells rewritten, M
 * cells folded to constants" with this call's totals. Modules with live $proc nodes are optimised
 * around them: a $proc is never visited (opaque). ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument; ODIN3_ERR_NO_MEMORY and the kit's other failures as odin3_opt_expr_run.
 */
odin3_status odin3_opt_expr_design(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* odin3_opt_expr_run with the whole counts aggregate (fold_type included); the design entry's. */
odin3_status odin3_opt_expr_module(odin3_pass_ctx *ctx, odin3_module *module,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- internal: the rule files' view of one visited cell (not for other callers) --------------- */

/* The registry types the rules read or build, in the order of expr.c's name table. */
typedef enum odin3_expr_type {
    ODIN3_EXPR_T_NOT,
    ODIN3_EXPR_T_NEG,
    ODIN3_EXPR_T_REDUCE_AND,
    ODIN3_EXPR_T_REDUCE_OR,
    ODIN3_EXPR_T_REDUCE_XOR,
    ODIN3_EXPR_T_REDUCE_XNOR,
    ODIN3_EXPR_T_REDUCE_BOOL,
    ODIN3_EXPR_T_LOGIC_NOT,
    ODIN3_EXPR_T_LOGIC_AND,
    ODIN3_EXPR_T_LOGIC_OR,
    ODIN3_EXPR_T_AND,
    ODIN3_EXPR_T_OR,
    ODIN3_EXPR_T_XOR,
    ODIN3_EXPR_T_XNOR,
    ODIN3_EXPR_T_SHL,
    ODIN3_EXPR_T_SHR,
    ODIN3_EXPR_T_SSHR,
    ODIN3_EXPR_T_SHIFTX,
    ODIN3_EXPR_T_LT,
    ODIN3_EXPR_T_LE,
    ODIN3_EXPR_T_GT,
    ODIN3_EXPR_T_GE,
    ODIN3_EXPR_T_EQ,
    ODIN3_EXPR_T_NE,
    ODIN3_EXPR_T_ADD,
    ODIN3_EXPR_T_SUB,
    ODIN3_EXPR_T_MUL,
    ODIN3_EXPR_T_DIV,
    ODIN3_EXPR_T_MOD,
    ODIN3_EXPR_T_POW,
    ODIN3_EXPR_T_MUX,
    ODIN3_EXPR_T_PMUX,
    ODIN3_EXPR_T_TRIBUF,
    ODIN3_EXPR_T_G_NOT,
    ODIN3_EXPR_T_G_AND,
    ODIN3_EXPR_T_G_OR,
    ODIN3_EXPR_T_G_XOR,
    ODIN3_EXPR_T_G_XNOR,
    ODIN3_EXPR_T_G_MUX,
    ODIN3_EXPR_T_COUNT
} odin3_expr_type;

/* Parameter indices (celltype definition order): unary, binary, $mux/$tribuf, $pmux. */
enum {
    ODIN3_EXPR_P_UN_SIGNED = 0,
    ODIN3_EXPR_P_UN_AW = 1,
    ODIN3_EXPR_P_UN_YW = 2,
    ODIN3_EXPR_P_BIN_ASIGNED = 0,
    ODIN3_EXPR_P_BIN_BSIGNED = 1,
    ODIN3_EXPR_P_BIN_AW = 2,
    ODIN3_EXPR_P_BIN_BW = 3,
    ODIN3_EXPR_P_BIN_YW = 4,
    ODIN3_EXPR_P_MUX_WIDTH = 0,
    ODIN3_EXPR_P_PMUX_SWIDTH = 1
};

/* A port of the visited cell: its position in the cell type's port list (typed so that a port is
 * never taken for a bit index). */
typedef struct odin3_expr_port {
    uint32_t v;
} odin3_expr_port;

/* Port positions: unary A Y; binary A B Y; $mux/$_MUX_/$pmux A B S Y; $tribuf A EN Y. */
#define ODIN3_EXPR_PORT(index) ((odin3_expr_port){(index)})
#define ODIN3_EXPR_PORT_A ODIN3_EXPR_PORT(0)
#define ODIN3_EXPR_PORT_B ODIN3_EXPR_PORT(1)
#define ODIN3_EXPR_PORT_S ODIN3_EXPR_PORT(2)
#define ODIN3_EXPR_PORT_EN ODIN3_EXPR_PORT(1)

enum { ODIN3_EXPR_MAX_IN = 4, ODIN3_EXPR_MAX_PARAMS = 5 };

/* The module's state and the visited cell's. */
typedef struct odin3_expr_ctx {
    odin3_opt_ctx *oc;
    odin3_module *module;
    odin3_celltype_id type[ODIN3_EXPR_T_COUNT]; /* 0: the registry lacks it */
    odin3_vec marks;                            /* per net: the scan generation that saw it */
    uint32_t gen;
    bool consume_x;
    bool keepdc, mux_undef, mux_bool, fine; /* the options, -full implying the last three */
    /* the visited cell */
    odin3_node_id node;
    odin3_expr_type kind;
    const odin3_celltype_def *def;
    odin3_arena *arena; /* scratch for the visit, made on demand */
    odin3_prov_id prov;
    bool began;     /* the rewrite's operation has begun */
    bool fired;     /* a rule rewrote the cell */
    bool counted;   /* the rule's counters are already in (odin3_opt_fold's) */
    bool all_const; /* the rewrite left every output on a constant */
    bool resume;    /* the rule that fired does not end the visit (Yosys: no `goto next_cell`) */
    odin3_node_id made; /* the node odin3_expr_rebuild made */
    odin3_opt_rule rule;
    const char *rule_text; /* the DEBUG line's rule when it is not rule's name (a fold's) */
} odin3_expr_ctx;

/* One rule: tests the visited cell, and when it applies rewrites it through the primitives below
 * (which set fired). */
typedef odin3_status (*odin3_expr_rule_fn)(odin3_expr_ctx *ec);

/* A signal: nets LSB first, in the visit's arena. */
typedef struct odin3_expr_sig {
    odin3_net_id *net;
    uint32_t n;
} odin3_expr_sig;

/* How many bits of a signal are 0, 1, x, z, or on no constant. */
typedef struct odin3_expr_census {
    uint32_t n, zero, one, xs, zs, other;
} odin3_expr_census;

/* A cell to build: its type and INT parameters, and the nets of its input ports in port order
 * (NULL entries: an old node's, for odin3_expr_rebuild). Outputs are always fresh. */
typedef struct odin3_expr_cell {
    odin3_expr_type type;
    int64_t params[ODIN3_EXPR_MAX_PARAMS];
    uint32_t n_params;
    const odin3_expr_sig *in[ODIN3_EXPR_MAX_IN];
} odin3_expr_cell;

/* --- expr_util.c: reading the cell, signals, building and rewriting ---------------------- */

uint32_t odin3_expr_width(const odin3_expr_ctx *ec, odin3_expr_port port);
uint32_t odin3_expr_y_width(const odin3_expr_ctx *ec);
odin3_net_id odin3_expr_net(const odin3_expr_ctx *ec, odin3_expr_port port, uint32_t bit);
odin3_const odin3_expr_cval(const odin3_expr_ctx *ec, odin3_net_id net);
int64_t odin3_expr_param(const odin3_expr_ctx *ec, uint32_t index);
odin3_expr_port odin3_expr_y_port(const odin3_expr_ctx *ec);

/* Same net, or the same constant value (Yosys SigBit equality, x and z included). */
bool odin3_expr_same(const odin3_expr_ctx *ec, odin3_net_id lhs, odin3_net_id rhs);
bool odin3_expr_is_undef(const odin3_expr_ctx *ec, odin3_net_id net); /* constant x or z */
/* A_SIGNED and B_SIGNED both set: the one case where a binary cell's operands are signed. The
 * mixed-sign ruling (Peter, 2026-10-13; spec 17 #57): on a cell with A_SIGNED != B_SIGNED BOTH
 * operands are zero-extended (simlib, the registry evaluator), so every rule that extends an
 * operand, and the fold, takes its signedness from here rather than from one flag (Yosys's
 * calc.cc extends each operand by its own flag, which no rule here follows). Binary cells only. */
bool odin3_expr_both_signed(const odin3_expr_ctx *ec);

odin3_expr_census odin3_expr_port_census(const odin3_expr_ctx *ec, odin3_expr_port port);
odin3_expr_census odin3_expr_sig_census(const odin3_expr_ctx *ec, const odin3_expr_sig *sig);
bool odin3_expr_fully_const(odin3_expr_census census,
                            bool keepdc);              /* Yosys is_fully_const [and !undef] */
bool odin3_expr_fully_def(odin3_expr_census census);   /* all 0/1 */
bool odin3_expr_fully_undef(odin3_expr_census census); /* all x/z */
bool odin3_expr_fully_zero(odin3_expr_census census);  /* all 0 */

void *odin3_expr_alloc(odin3_expr_ctx *ec, size_t bytes);
odin3_status odin3_expr_sig_new(odin3_expr_ctx *ec, uint32_t n, odin3_expr_sig *out);
odin3_status odin3_expr_read(odin3_expr_ctx *ec, odin3_expr_port port, odin3_expr_sig *out);
odin3_status odin3_expr_fill(odin3_expr_ctx *ec, odin3_bit bit, uint32_t n, odin3_expr_sig *out);
/* Yosys extend_u0: truncates or extends to width by the sign (is_signed) or with 0. */
odin3_status odin3_expr_extend(odin3_expr_ctx *ec, const odin3_expr_sig *in, uint32_t width,
                               bool is_signed, odin3_expr_sig *out);
/* A view of in[from .. from + n) (no copy; the caller keeps it inside in). */
odin3_expr_sig odin3_expr_slice(const odin3_expr_sig *in, uint32_t from, uint32_t n);
/* low followed by high (low is the LSB part). */
odin3_status odin3_expr_join(odin3_expr_ctx *ec, const odin3_expr_sig *low,
                             const odin3_expr_sig *high, odin3_expr_sig *out);
/* A one-bit signal of net. */
odin3_status odin3_expr_one(odin3_expr_ctx *ec, odin3_net_id net, odin3_expr_sig *out);

/* Marks nets for a membership test: begin, set, test. */
odin3_status odin3_expr_mark_begin(odin3_expr_ctx *ec);
void odin3_expr_mark(odin3_expr_ctx *ec, odin3_net_id net);
bool odin3_expr_marked(const odin3_expr_ctx *ec, odin3_net_id net);

/* Builds a helper cell (outputs fresh) and returns its output port's nets (the last port). */
odin3_status odin3_expr_new_cell(odin3_expr_ctx *ec, const odin3_expr_cell *spec,
                                 odin3_node_id *node, odin3_expr_sig *out);
/* A helper $not / $and / ... on unsigned operands of the signals' widths (Y width y_width). */
odin3_status odin3_expr_new_un(odin3_expr_ctx *ec, odin3_expr_type type, const odin3_expr_sig *src,
                               uint32_t y_width, odin3_expr_sig *out);
odin3_status odin3_expr_new_bin(odin3_expr_ctx *ec, odin3_expr_type type, const odin3_expr_sig *lhs,
                                const odin3_expr_sig *rhs, odin3_expr_sig *out);

/* Begins the visit's rewrite (one provenance operation per visit); idempotent. */
odin3_status odin3_expr_begin(odin3_expr_ctx *ec);

/* The rewrites. Each begins the visit's operation, does its kit call and sets fired. */
/* Y = src extended with 0 (or cut) to Y's width: kit_connect. */
odin3_status odin3_expr_wire(odin3_expr_ctx *ec, odin3_opt_rule rule, const odin3_expr_sig *src);
odin3_status odin3_expr_wire_bit(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_bit bit);
odin3_status odin3_expr_wire_net(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_net_id net);
/* The visited cell becomes spec (its name and attributes kept); outputs keep their nets. */
odin3_status odin3_expr_rebuild(odin3_expr_ctx *ec, odin3_opt_rule rule,
                                const odin3_expr_cell *spec);
/* odin3_expr_wire, then node fresh (a helper cell the rule built) takes the visited cell's name and
 * attributes: the Yosys cell that is modified in place, when its outputs change width. */
odin3_status odin3_expr_wire_keep(odin3_expr_ctx *ec, odin3_opt_rule rule,
                                  const odin3_expr_sig *src, odin3_node_id fresh);
/* Counts rule once more without firing (Yosys's cover points that a single action hits twice). */
void odin3_expr_count(odin3_expr_ctx *ec, odin3_opt_rule rule);

/* --- the rules --------------------------------------------------------------------------- */

/* expr_logic.c (§4.2) */
odin3_status odin3_expr_rule_and_or(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_xor(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_unary(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_bitwise(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_invert_double(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_invert_muxsel(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_gate(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_tribuf(odin3_expr_ctx *ec);

/* expr_cmp.c (§4.3: x bits, comparisons, shifts) */
odin3_status odin3_expr_rule_xbit(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_trim(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_eqneq(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_eq1bit(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_cmpzero(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_constshift(odin3_expr_ctx *ec);

/* expr_arith.c (§4.3: identities, $pow, $mul, $div, $mod) */
odin3_status odin3_expr_rule_identwrt(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_pow(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_mul(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_divmod(odin3_expr_ctx *ec);

/* expr_mux.c (§4.3: mux rules) */
odin3_status odin3_expr_rule_mux_bool(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_mux_undef(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_mux_const(odin3_expr_ctx *ec);

/* expr_undriven.c (§4.6): replace_undriven on the module of ec. */
odin3_status odin3_expr_replace_undriven(odin3_expr_ctx *ec);

/* expr_fine.c (§4.5, only with -fine or -full; each is a no-op for the other cell types). The
 * order of a type's list is Yosys's: group, neutral_A, neutral_B, reduce_and, const_a, const_b
 * (all before the xbit rules), the $add/$sub strip, then after folding the $eq/$ne and
 * comparison rules. */
odin3_status odin3_expr_rule_fine_group(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_neutral_a(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_neutral_b(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_reduce_and(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_const_a(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_const_b(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_addsub(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_eqne(odin3_expr_ctx *ec);
odin3_status odin3_expr_rule_fine_cmp(odin3_expr_ctx *ec);

/* expr_ffpol.c (§4.4): one pass over the module's FFs and memories after the invert map is taken
 * (odin3_kit_invert_map): each CLK, EN, ARST, SRST, ALOAD, SET, CLR (a $mem's RD_CLK / WR_CLK
 * bits, a fine FF's C / E) driven by an inverter's output takes the inverter's input and flips its
 * polarity; the node is rebuilt once however many inputs change. Counts ODIN3_OPT_RULE_CLKINV and
 * `rewritten` per input, with one DEBUG line each. NO_MEMORY leaves the IR valid and every FF as
 * it was (the rebuild is the kit's: nothing is destroyed before it can no longer fail). */
odin3_status odin3_opt_expr_ffpol(odin3_opt_ctx *oc, bool x_round);

/* The DEBUG line of a rewrite (§16): "opt_expr: module 'm': <rule> on <type> '<cell>'", the cell
 * being the string name_str (0: none) or else $c<ID of node>. The caller reads both before the
 * rewrite, which deletes the node. Nothing at a level below DEBUG. */
void odin3_expr_log_rewrite(const odin3_opt_ctx *oc, const char *rule, const char *type_name,
                            uint32_t name_str, odin3_node_id node);

#endif
