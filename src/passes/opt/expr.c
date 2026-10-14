/*
 * expr.c — opt_expr (2F spec OP-4, §4.1; plan Task 6): the driver (Yosys 0.55 opt_expr.cc
 * `execute` and `replace_const_cells`), the per-type rule tables, the counters and the lines
 * of §16. The rules are in expr_logic.c (§4.2), expr_cmp.c, expr_arith.c and expr_mux.c (§4.3),
 * expr_ffpol.c (§4.4), expr_fine.c (§4.5) and expr_undriven.c (§4.6); their shared helpers are in
 * expr_util.c.
 *
 * One round (`rcc`): snapshot the invert map; fold the FF polarities (expr_ffpol.c); visit the
 * evaluable nodes in odin3_kit_topo order and apply to each the first rule of its type's list that
 * fires. Rules are listed in Yosys's order of opt_expr.cc, filtered by cell type; constant
 * folding (odin3_opt_fold) is the rule at its place in that order.
 */
#include "passes/opt/expr.h"

#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/opt/fold.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LABEL_MAX = 256, FOLD_PREFIX_LEN = 6 }; /* "const." */

/* --- rule names (Yosys's cover points) -------------------------------------------------------- */

static const char *const k_rule_names[ODIN3_OPT_RULE_COUNT] = {
    [ODIN3_OPT_RULE_CONST_AND] = "const_and",
    [ODIN3_OPT_RULE_CONST_OR] = "const_or",
    [ODIN3_OPT_RULE_AND_OR_BUFFER] = "and_or_buffer",
    [ODIN3_OPT_RULE_CONST_XOR] = "const_xor",
    [ODIN3_OPT_RULE_CONST_XNOR] = "const_xnor",
    [ODIN3_OPT_RULE_XOR_BUFFER] = "xor_buffer",
    [ODIN3_OPT_RULE_XNOR_BUFFER] = "xnor_buffer",
    [ODIN3_OPT_RULE_REDUCE_XNOR_NOT] = "reduce_xnor_not",
    [ODIN3_OPT_RULE_UNARY_BUFFER] = "unary_buffer",
    [ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST] = "bitwise_logic_one_const",
    [ODIN3_OPT_RULE_XBIT] = "xbit",
    [ODIN3_OPT_RULE_TRIM] = "trim",
    [ODIN3_OPT_RULE_INVERT_DOUBLE] = "invert.double",
    [ODIN3_OPT_RULE_INVERT_MUXSEL] = "invert.muxsel",
    [ODIN3_OPT_RULE_GATE_NOT] = "gate_not",
    [ODIN3_OPT_RULE_GATE_AND] = "gate_and",
    [ODIN3_OPT_RULE_GATE_OR] = "gate_or",
    [ODIN3_OPT_RULE_GATE_XOR] = "gate_xor",
    [ODIN3_OPT_RULE_GATE_MUX] = "gate_mux",
    [ODIN3_OPT_RULE_MUX_TO_INV] = "mux_to_inv",
    [ODIN3_OPT_RULE_TRIBUF_EN1] = "tribuf_en1",
    [ODIN3_OPT_RULE_TRIBUF_EN0] = "tribuf_en0",
    [ODIN3_OPT_RULE_EQNEQ_ISNEQ] = "eqneq.isneq",
    [ODIN3_OPT_RULE_EQNEQ_EMPTY] = "eqneq.empty",
    [ODIN3_OPT_RULE_EQNEQ_RESIZE] = "eqneq.resize",
    [ODIN3_OPT_RULE_EQNEQ_SWAPCONST] = "eqneq.swapconst",
    [ODIN3_OPT_RULE_EQNEQ_UNDEF] = "eqneq.undef",
    [ODIN3_OPT_RULE_EQNEQ_BUFFER] = "eqneq.buffer",
    [ODIN3_OPT_RULE_EQNEQ_ISNOT] = "eqneq.isnot",
    [ODIN3_OPT_RULE_EQNEQ_CMPZERO] = "eqneq.cmpzero",
    [ODIN3_OPT_RULE_CONSTSHIFT] = "constshift",
    [ODIN3_OPT_RULE_IDENTWRT_A] = "identwrt.a",
    [ODIN3_OPT_RULE_IDENTWRT_B] = "identwrt.b",
    [ODIN3_OPT_RULE_MUX_BOOL] = "mux_bool",
    [ODIN3_OPT_RULE_MUX_INVERT] = "mux_invert",
    [ODIN3_OPT_RULE_MUX_AND] = "mux_and",
    [ODIN3_OPT_RULE_MUX_OR] = "mux_or",
    [ODIN3_OPT_RULE_MUX_UNDEF] = "mux_undef",
    [ODIN3_OPT_RULE_MUX_EMPTY] = "mux_empty",
    [ODIN3_OPT_RULE_MUX_SEL01] = "mux_sel01",
    [ODIN3_OPT_RULE_MUX_REDUCE] = "mux_reduce",
    [ODIN3_OPT_RULE_MUX_CONST_SEL] = "mux_const_sel",
    [ODIN3_OPT_RULE_MUX_SAME] = "mux_same",
    [ODIN3_OPT_RULE_POW_SHIFT] = "pow_shift",
    [ODIN3_OPT_RULE_MUL_SHIFT_ZERO] = "mul_shift.zero",
    [ODIN3_OPT_RULE_MUL_SHIFT_SWAPPED] = "mul_shift.swapped",
    [ODIN3_OPT_RULE_MUL_SHIFT_UNSWAPPED] = "mul_shift.unswapped",
    [ODIN3_OPT_RULE_MUL_LOW_ZEROS] = "mul_low_zeros",
    [ODIN3_OPT_RULE_DIVMOD_ZERO] = "divmod_zero",
    [ODIN3_OPT_RULE_DIV_SHIFT] = "div_shift",
    [ODIN3_OPT_RULE_MOD_MASK] = "mod_mask",
    [ODIN3_OPT_RULE_CLKINV] = "clkinv",
    [ODIN3_OPT_RULE_FINE_GROUP] = "fine.group",
    [ODIN3_OPT_RULE_FINE_NEUTRAL_A] = "fine.neutral_A",
    [ODIN3_OPT_RULE_FINE_NEUTRAL_B] = "fine.neutral_B",
    [ODIN3_OPT_RULE_FINE_REDUCE_AND] = "fine.$reduce_and",
    [ODIN3_OPT_RULE_FINE_A] = "fine.A",
    [ODIN3_OPT_RULE_FINE_B] = "fine.B",
    [ODIN3_OPT_RULE_FINE_ADDSUB] = "fine",
    [ODIN3_OPT_RULE_FINE_EQNE] = "fine.eqne",
    [ODIN3_OPT_RULE_FINE_CMP] = "fine.cmp",
    [ODIN3_OPT_RULE_UNDRIVEN] = "undriven",
};

const char *odin3_opt_rule_name(odin3_opt_rule rule) {
    return (unsigned)rule < ODIN3_OPT_RULE_COUNT ? k_rule_names[rule] : NULL;
}

/* --- the registry types and the rule lists ---------------------------------------------------- */

static const char *const k_type_names[ODIN3_EXPR_T_COUNT] = {
    [ODIN3_EXPR_T_NOT] = "$not",
    [ODIN3_EXPR_T_NEG] = "$neg",
    [ODIN3_EXPR_T_REDUCE_AND] = "$reduce_and",
    [ODIN3_EXPR_T_REDUCE_OR] = "$reduce_or",
    [ODIN3_EXPR_T_REDUCE_XOR] = "$reduce_xor",
    [ODIN3_EXPR_T_REDUCE_XNOR] = "$reduce_xnor",
    [ODIN3_EXPR_T_REDUCE_BOOL] = "$reduce_bool",
    [ODIN3_EXPR_T_LOGIC_NOT] = "$logic_not",
    [ODIN3_EXPR_T_LOGIC_AND] = "$logic_and",
    [ODIN3_EXPR_T_LOGIC_OR] = "$logic_or",
    [ODIN3_EXPR_T_AND] = "$and",
    [ODIN3_EXPR_T_OR] = "$or",
    [ODIN3_EXPR_T_XOR] = "$xor",
    [ODIN3_EXPR_T_XNOR] = "$xnor",
    [ODIN3_EXPR_T_SHL] = "$shl",
    [ODIN3_EXPR_T_SHR] = "$shr",
    [ODIN3_EXPR_T_SSHR] = "$sshr",
    [ODIN3_EXPR_T_SHIFTX] = "$shiftx",
    [ODIN3_EXPR_T_LT] = "$lt",
    [ODIN3_EXPR_T_LE] = "$le",
    [ODIN3_EXPR_T_GT] = "$gt",
    [ODIN3_EXPR_T_GE] = "$ge",
    [ODIN3_EXPR_T_EQ] = "$eq",
    [ODIN3_EXPR_T_NE] = "$ne",
    [ODIN3_EXPR_T_ADD] = "$add",
    [ODIN3_EXPR_T_SUB] = "$sub",
    [ODIN3_EXPR_T_MUL] = "$mul",
    [ODIN3_EXPR_T_DIV] = "$div",
    [ODIN3_EXPR_T_MOD] = "$mod",
    [ODIN3_EXPR_T_POW] = "$pow",
    [ODIN3_EXPR_T_MUX] = "$mux",
    [ODIN3_EXPR_T_PMUX] = "$pmux",
    [ODIN3_EXPR_T_TRIBUF] = "$tribuf",
    [ODIN3_EXPR_T_G_NOT] = "$_NOT_",
    [ODIN3_EXPR_T_G_AND] = "$_AND_",
    [ODIN3_EXPR_T_G_OR] = "$_OR_",
    [ODIN3_EXPR_T_G_XOR] = "$_XOR_",
    [ODIN3_EXPR_T_G_XNOR] = "$_XNOR_",
    [ODIN3_EXPR_T_G_MUX] = "$_MUX_",
};

/* True when every input pin of the node is on a constant net (the folding precondition). */
static bool inputs_const(const odin3_expr_ctx *ec) {
    odin3_pinslice pins = odin3_node_pins(ec->module, ec->node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (odin3_pin_dir(ec->module, pin) == ODIN3_DIR_IN &&
            odin3_expr_cval(ec, odin3_pin_net(ec->module, pin)) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

/* The counter name of a fold of this cell's type: "const.$<type>". */
static const char *fold_text(const odin3_expr_ctx *ec) {
    for (uint32_t i = 0; i < ODIN3_OPT_FOLD_TYPES; i++) {
        const char *name = odin3_opt_fold_name(i);
        if (name != NULL && strcmp(name + FOLD_PREFIX_LEN, ec->def->name) == 0) {
            return name;
        }
    }
    return NULL;
}

/* Constant folding (Yosys's FOLD_*_CELL): every input constant, Y = the 4-state result. */
static odin3_status rule_fold(odin3_expr_ctx *ec) {
    if (!odin3_opt_foldable(ec->oc, odin3_node_type(ec->module, ec->node)) || !inputs_const(ec)) {
        return ODIN3_OK;
    }
    const char *text = fold_text(ec);
    bool folded = false;
    odin3_status st = odin3_expr_begin(ec);
    st = st == ODIN3_OK ? odin3_opt_fold(ec->oc, ec->node, &folded) : st;
    if (st == ODIN3_OK && folded) {
        ec->fired = true;
        ec->counted = true; /* odin3_opt_fold counted it: folded and const.$<type> */
        ec->rule_text = text;
    }
    return st;
}

#define RULES(name, ...) static const odin3_expr_rule_fn name[] = {__VA_ARGS__, NULL}

RULES(k_inv, odin3_expr_rule_fine_group, odin3_expr_rule_fine_neutral_a,
      odin3_expr_rule_fine_const_a, odin3_expr_rule_invert_double, rule_fold);
RULES(k_neg, odin3_expr_rule_unary, odin3_expr_rule_xbit, rule_fold);
RULES(k_reduce_andor, odin3_expr_rule_and_or, odin3_expr_rule_unary, odin3_expr_rule_fine_neutral_a,
      odin3_expr_rule_fine_reduce_and, odin3_expr_rule_fine_const_a, rule_fold);
RULES(k_reduce_xor, odin3_expr_rule_unary, odin3_expr_rule_xbit, rule_fold);
RULES(k_logic_andor, odin3_expr_rule_and_or, odin3_expr_rule_fine_neutral_a,
      odin3_expr_rule_fine_neutral_b, odin3_expr_rule_fine_const_a, odin3_expr_rule_fine_const_b,
      rule_fold);
RULES(k_and, odin3_expr_rule_and_or, odin3_expr_rule_bitwise, odin3_expr_rule_fine_group,
      rule_fold);
RULES(k_or, odin3_expr_rule_and_or, odin3_expr_rule_bitwise, odin3_expr_rule_fine_group,
      odin3_expr_rule_identwrt, rule_fold);
RULES(k_xor, odin3_expr_rule_xor, odin3_expr_rule_bitwise, odin3_expr_rule_fine_group,
      odin3_expr_rule_identwrt, rule_fold);
RULES(k_xnor, odin3_expr_rule_xor, odin3_expr_rule_bitwise, odin3_expr_rule_fine_group, rule_fold);
RULES(k_shift, odin3_expr_rule_xbit, odin3_expr_rule_constshift, rule_fold);
RULES(k_shiftx, odin3_expr_rule_xbit, odin3_expr_rule_trim, odin3_expr_rule_constshift, rule_fold);
RULES(k_cmp, odin3_expr_rule_xbit, rule_fold, odin3_expr_rule_fine_cmp);
RULES(k_eq, odin3_expr_rule_eqneq, odin3_expr_rule_eq1bit, odin3_expr_rule_cmpzero, rule_fold,
      odin3_expr_rule_fine_eqne);
RULES(k_addsub, odin3_expr_rule_fine_addsub, odin3_expr_rule_xbit, odin3_expr_rule_identwrt,
      rule_fold);
RULES(k_mul, odin3_expr_rule_xbit, odin3_expr_rule_identwrt, rule_fold, odin3_expr_rule_mul);
RULES(k_div, odin3_expr_rule_xbit, odin3_expr_rule_identwrt, rule_fold, odin3_expr_rule_divmod);
RULES(k_mod, odin3_expr_rule_xbit, rule_fold, odin3_expr_rule_divmod);
RULES(k_pow, odin3_expr_rule_xbit, rule_fold, odin3_expr_rule_pow);
RULES(k_mux, odin3_expr_rule_invert_muxsel, odin3_expr_rule_mux_bool, odin3_expr_rule_mux_undef,
      rule_fold, odin3_expr_rule_mux_const);
RULES(k_pmux, odin3_expr_rule_mux_undef, rule_fold);
RULES(k_tribuf, odin3_expr_rule_tribuf);
RULES(k_g_not, odin3_expr_rule_invert_double, odin3_expr_rule_gate);
RULES(k_g_andor, odin3_expr_rule_and_or, odin3_expr_rule_gate);
RULES(k_g_xor, odin3_expr_rule_xor, odin3_expr_rule_gate);
RULES(k_g_xnor, odin3_expr_rule_xor);
RULES(k_g_mux, odin3_expr_rule_invert_muxsel, odin3_expr_rule_gate, odin3_expr_rule_mux_bool);

static const odin3_expr_rule_fn *const k_rules[ODIN3_EXPR_T_COUNT] = {
    [ODIN3_EXPR_T_NOT] = k_inv,
    [ODIN3_EXPR_T_NEG] = k_neg,
    [ODIN3_EXPR_T_REDUCE_AND] = k_reduce_andor,
    [ODIN3_EXPR_T_REDUCE_OR] = k_reduce_andor,
    [ODIN3_EXPR_T_REDUCE_XOR] = k_reduce_xor,
    [ODIN3_EXPR_T_REDUCE_XNOR] = k_reduce_xor,
    [ODIN3_EXPR_T_REDUCE_BOOL] = k_reduce_andor,
    [ODIN3_EXPR_T_LOGIC_NOT] = k_inv,
    [ODIN3_EXPR_T_LOGIC_AND] = k_logic_andor,
    [ODIN3_EXPR_T_LOGIC_OR] = k_logic_andor,
    [ODIN3_EXPR_T_AND] = k_and,
    [ODIN3_EXPR_T_OR] = k_or,
    [ODIN3_EXPR_T_XOR] = k_xor,
    [ODIN3_EXPR_T_XNOR] = k_xnor,
    [ODIN3_EXPR_T_SHL] = k_shift,
    [ODIN3_EXPR_T_SHR] = k_shift,
    [ODIN3_EXPR_T_SSHR] = k_shift,
    [ODIN3_EXPR_T_SHIFTX] = k_shiftx,
    [ODIN3_EXPR_T_LT] = k_cmp,
    [ODIN3_EXPR_T_LE] = k_cmp,
    [ODIN3_EXPR_T_GT] = k_cmp,
    [ODIN3_EXPR_T_GE] = k_cmp,
    [ODIN3_EXPR_T_EQ] = k_eq,
    [ODIN3_EXPR_T_NE] = k_eq,
    [ODIN3_EXPR_T_ADD] = k_addsub,
    [ODIN3_EXPR_T_SUB] = k_addsub,
    [ODIN3_EXPR_T_MUL] = k_mul,
    [ODIN3_EXPR_T_DIV] = k_div,
    [ODIN3_EXPR_T_MOD] = k_mod,
    [ODIN3_EXPR_T_POW] = k_pow,
    [ODIN3_EXPR_T_MUX] = k_mux,
    [ODIN3_EXPR_T_PMUX] = k_pmux,
    [ODIN3_EXPR_T_TRIBUF] = k_tribuf,
    [ODIN3_EXPR_T_G_NOT] = k_g_not,
    [ODIN3_EXPR_T_G_AND] = k_g_andor,
    [ODIN3_EXPR_T_G_OR] = k_g_andor,
    [ODIN3_EXPR_T_G_XOR] = k_g_xor,
    [ODIN3_EXPR_T_G_XNOR] = k_g_xnor,
    [ODIN3_EXPR_T_G_MUX] = k_g_mux,
};

/* --- one visit -------------------------------------------------------------------------------- */

/* The registry types by name, once per module; a type the registry lacks stays 0. */
static void resolve_types(odin3_expr_ctx *ec) {
    for (uint32_t i = 0; i < ODIN3_EXPR_T_COUNT; i++) {
        odin3_celltype_id id = odin3_kit_type_named(ec->oc->design, k_type_names[i]);
        if (id.v != 0) {
            ec->type[i] = id;
        }
    }
}

/* The ODIN3_EXPR_T_* kind of the node, ODIN3_EXPR_T_COUNT for a type without rules. */
static odin3_expr_type kind_of(const odin3_expr_ctx *ec, odin3_celltype_id type) {
    for (uint32_t i = 0; i < ODIN3_EXPR_T_COUNT; i++) {
        if (type.v != 0 && ec->type[i].v == type.v) {
            return (odin3_expr_type)i;
        }
    }
    return ODIN3_EXPR_T_COUNT;
}

/* Every input pin is connected (a cell with an open input is left alone). */
static bool inputs_connected(const odin3_expr_ctx *ec) {
    odin3_pinslice pins = odin3_node_pins(ec->module, ec->node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (odin3_pin_dir(ec->module, pin) == ODIN3_DIR_IN &&
            !odin3_net_valid(odin3_pin_net(ec->module, pin))) {
            return false;
        }
    }
    return true;
}

static const char *name_text(const odin3_expr_ctx *ec, uint32_t str) {
    return str == 0 ? NULL : odin3_strtab_get(odin3_design_strtab(ec->oc->design), str);
}

void odin3_expr_log_rewrite(const odin3_opt_ctx *oc, const char *rule, const char *type_name,
                            uint32_t name_str, odin3_node_id node) {
    if (odin3_log_get_level() < ODIN3_LOG_DEBUG) {
        return;
    }
    const odin3_strtab *tab = odin3_design_strtab(oc->design);
    char label[LABEL_MAX];
    odin3_kit_name_label(oc, name_str, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "opt_expr: module '%s': %s on %s '%s'",
              odin3_strtab_get(tab, odin3_module_name(oc->module)), rule, type_name, label);
}

/* DEBUG "opt_expr: module 'm': <rule> on <type> '<cell>'" (rule = §4 name; cell = name or $c<ID>).
 */
static void log_rewrite(const odin3_expr_ctx *ec, uint32_t name_str) {
    const char *rule = ec->rule_text != NULL ? ec->rule_text : odin3_opt_rule_name(ec->rule);
    odin3_expr_log_rewrite(ec->oc, rule, ec->def->name, name_str, ec->node);
}

/* The counters of a rewrite that fired: the rule, and rewritten or folded (a fold counted itself).
 */
static void account(odin3_expr_ctx *ec, uint32_t name_str) {
    odin3_opt_expr_counts *counts = &ec->oc->counts->expr;
    if (!ec->counted) {
        counts->rule[ec->rule]++;
        if (ec->all_const) {
            counts->folded++;
        } else {
            counts->rewritten++;
        }
    }
    log_rewrite(ec, name_str);
}

static void visit_reset(odin3_expr_ctx *ec, odin3_node_id node) {
    odin3_arena_destroy(ec->arena);
    ec->arena = NULL;
    ec->node = node;
    ec->prov = (odin3_prov_id){0};
    ec->began = ec->fired = ec->counted = ec->all_const = ec->resume = false;
    ec->rule = ODIN3_OPT_RULE_COUNT;
    ec->rule_text = NULL;
}

/* Where fn stands in the rule list of kind: its index, (size_t)-1 when it is not there (the
 * visit must not step past the list's NULL end marker). */
static size_t position_of(odin3_expr_type kind, odin3_expr_rule_fn fn) {
    size_t at = 0;
    while (k_rules[kind][at] != NULL && k_rules[kind][at] != fn) {
        at++;
    }
    return k_rules[kind][at] != NULL ? at : (size_t)-1;
}

/*
 * The rule at `fn` rewrote the cell and Yosys goes on with the rewritten cell (eqneq.resize and
 * mux_reduce set no `goto next_cell`): the visit now is of the rebuilt node, of its new type, and
 * continues after the same rule in that type's list. Returns the index to continue after, or
 * (size_t)-1 when there is no such type or list.
 */
static size_t resume_visit(odin3_expr_ctx *ec, odin3_expr_rule_fn fn) {
    odin3_celltype_id type = odin3_node_type(ec->module, ec->made);
    visit_reset(ec, ec->made);
    ec->kind = kind_of(ec, type);
    if (ec->kind == ODIN3_EXPR_T_COUNT || k_rules[ec->kind] == NULL) {
        return (size_t)-1;
    }
    ec->def = odin3_celltype_get(ec->oc->design, type);
    return position_of(ec->kind, fn);
}

/* The binary types that read A_SIGNED and B_SIGNED together (the shifts and $pow take B's flag as
 * the amount's own, and are not mixed in this sense). */
static bool mixed_sign_kind(odin3_expr_type kind) {
    switch (kind) {
    case ODIN3_EXPR_T_AND:
    case ODIN3_EXPR_T_OR:
    case ODIN3_EXPR_T_XOR:
    case ODIN3_EXPR_T_XNOR:
    case ODIN3_EXPR_T_LT:
    case ODIN3_EXPR_T_LE:
    case ODIN3_EXPR_T_GT:
    case ODIN3_EXPR_T_GE:
    case ODIN3_EXPR_T_EQ:
    case ODIN3_EXPR_T_NE:
    case ODIN3_EXPR_T_ADD:
    case ODIN3_EXPR_T_SUB:
    case ODIN3_EXPR_T_MUL:
    case ODIN3_EXPR_T_DIV:
    case ODIN3_EXPR_T_MOD:
        return true;
    default:
        return false;
    }
}

/* Applies to the node the first rule of its type's list that fires (and any it resumes into). */
static odin3_status visit(odin3_expr_ctx *ec, odin3_node_id node) {
    if (!odin3_node_live(ec->module, node)) {
        return ODIN3_OK;
    }
    visit_reset(ec, node);
    ec->kind = kind_of(ec, odin3_node_type(ec->module, node));
    if (ec->kind == ODIN3_EXPR_T_COUNT || k_rules[ec->kind] == NULL || !inputs_connected(ec)) {
        return ODIN3_OK;
    }
    ec->def = odin3_celltype_get(ec->oc->design, odin3_node_type(ec->module, node));
    if (mixed_sign_kind(ec->kind)) {
        odin3_opt_warn_mixed_sign(ec->oc, node, ec->def->name);
    }
    uint32_t name_str = odin3_node_name(ec->module, node);
    odin3_status st = ODIN3_OK;
    for (size_t at = 0; st == ODIN3_OK && k_rules[ec->kind][at] != NULL; at++) {
        odin3_expr_rule_fn fn = k_rules[ec->kind][at];
        st = fn(ec);
        if (st != ODIN3_OK || !ec->fired) {
            continue;
        }
        account(ec, name_str);
        if (!ec->resume) {
            break;
        }
        at = resume_visit(ec, fn);
        if (at == (size_t)-1) {
            break;
        }
    }
    return st;
}

/* --- rounds and the driver ---------------------------------------------------------------------
 */

/* One `replace_const_cells`: invert map, FF polarities (unless -noclkinv), every evaluable node in
 * order. */
static odin3_status round_once(odin3_expr_ctx *ec, bool consume_x, bool *loop_logged) {
    ec->consume_x = consume_x;
    if (!odin3_opt_dirty_pending(ec->oc, consume_x)) {
        return ODIN3_OK; /* incremental: nothing to look at (nothing changes, nothing is built) */
    }
    const odin3_opt_test_hooks *hooks = odin3_opt_test_hooks_now();
    if (hooks->on_expr_setup != NULL) {
        hooks->on_expr_setup(hooks->user, true);
    }
    odin3_status st = odin3_kit_invert_map(ec->oc);
    if (st == ODIN3_OK && !ec->oc->opts->noclkinv) {
        st = odin3_opt_expr_ffpol(ec->oc, consume_x);
    }
    odin3_vec order;
    odin3_vec_init(&order, sizeof(odin3_node_id));
    bool sorted = true;
    st = st == ODIN3_OK ? odin3_kit_topo(ec->oc, &order, &sorted) : st;
    if (hooks->on_expr_setup != NULL) {
        hooks->on_expr_setup(hooks->user, false);
    }
    if (st == ODIN3_OK && !sorted && !*loop_logged) {
        *loop_logged = true;
        odin3_log(ODIN3_LOG_INFO,
                  "opt_expr: module '%s': cells could not be sorted topologically; visiting them "
                  "in ID order",
                  name_text(ec, odin3_module_name(ec->module)));
    }
    for (size_t i = 0; st == ODIN3_OK && i < order.len; i++) {
        odin3_node_id node = *(const odin3_node_id *)odin3_vec_cat(&order, i);
        st = odin3_opt_dirty_take(ec->oc, node, consume_x) ? visit(ec, node) : st;
    }
    odin3_vec_free(&order);
    return st;
}

/* do { do { rcc(false) } while (changed); if (!keepdc) rcc(true) } while (changed) */
static odin3_status run_rounds(odin3_expr_ctx *ec) {
    bool loop_logged = false;
    odin3_status st = ODIN3_OK;
    for (;;) {
        do {
            ec->oc->changed = false;
            st = round_once(ec, false, &loop_logged);
        } while (st == ODIN3_OK && ec->oc->changed);
        if (st != ODIN3_OK || ec->keepdc) {
            break;
        }
        st = round_once(ec, true, &loop_logged);
        if (st != ODIN3_OK || !ec->oc->changed) {
            break;
        }
    }
    return st;
}

/*
 * The call reached its fixed point: no node is dirty any more. In a Debug build an incremental
 * module is first swept in full once, in both kinds of round, and must not change (the dirty set
 * is exact, spec section 19): a rewrite the incremental rounds missed aborts here, where the
 * full-rescan comparison of the tests would only show a different result.
 */
static odin3_status settle(odin3_expr_ctx *ec) {
    odin3_status st = ODIN3_OK;
#ifndef NDEBUG
    if (odin3_opt_dirty_incremental(ec->oc)) {
        bool logged = true;
        odin3_opt_dirty_sweep_all(ec->oc);
        for (uint32_t phase = 0; st == ODIN3_OK && phase < (ec->keepdc ? 1U : 2U); phase++) {
            ec->oc->changed = false;
            st = round_once(ec, phase == 1U, &logged);
            assert(!ec->oc->changed && "opt_expr: the dirty set missed a rewrite");
        }
    }
#endif
    odin3_opt_dirty_settle(ec->oc);
    return st;
}

static odin3_status debug_check(odin3_module *module) {
#ifdef NDEBUG
    (void)module;
    return ODIN3_OK;
#else
    odin3_check_opts opts = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    return odin3_check_module(module, opts);
#endif
}

static void expr_ctx_init(odin3_expr_ctx *ec, odin3_opt_ctx *oc) {
    memset(ec, 0, sizeof *ec);
    ec->oc = oc;
    ec->module = oc->module;
    ec->keepdc = oc->opts->keepdc;
    ec->mux_undef = oc->opts->mux_undef || oc->opts->full;
    ec->mux_bool = oc->opts->mux_bool || oc->opts->full;
    ec->fine = oc->opts->fine || oc->opts->full;
    odin3_vec_init(&ec->marks, sizeof(uint32_t));
    resolve_types(ec);
}

odin3_status odin3_opt_expr_module(odin3_pass_ctx *ctx, odin3_module *module,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    odin3_opt_ctx oc;
    if (counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_expr: no counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_opt_ctx_init(&oc, ctx, module, opts) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG; /* logged by the kit */
    }
    oc.counts = counts;
    odin3_expr_ctx ec;
    expr_ctx_init(&ec, &oc);
    odin3_status st = odin3_opt_canon_consts(&oc);
    if (st == ODIN3_OK && (opts->undriven || opts->full)) {
        st = odin3_expr_replace_undriven(&ec);
    }
    st = st == ODIN3_OK ? run_rounds(&ec) : st;
    st = st == ODIN3_OK ? settle(&ec) : st;
    odin3_arena_destroy(ec.arena);
    odin3_vec_free(&ec.marks);
    odin3_opt_ctx_free(&oc);
    return st == ODIN3_OK ? debug_check(module) : st;
}

odin3_status odin3_opt_expr_run(odin3_pass_ctx *ctx, odin3_module *module,
                                const odin3_opt_opts *opts, odin3_opt_expr_counts *counts) {
    if (counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_expr: no counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_opt_counts local;
    memset(&local, 0, sizeof local);
    odin3_status st = odin3_opt_expr_module(ctx, module, opts, &local);
    counts->rewritten += local.expr.rewritten;
    counts->folded += local.expr.folded;
    for (uint32_t i = 0; i < ODIN3_OPT_EXPR_MAX_RULES; i++) {
        counts->rule[i] += local.expr.rule[i];
    }
    return st;
}

odin3_status odin3_opt_expr_design(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_expr: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t rewritten = counts->expr.rewritten;
    uint32_t folded = counts->expr.folded;
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        st = module != NULL ? odin3_opt_expr_module(ctx, module, opts, counts) : st;
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "opt_expr: %" PRIu32 " cells rewritten, %" PRIu32 " cells folded to constants",
                  counts->expr.rewritten - rewritten, counts->expr.folded - folded);
    }
    return st;
}
