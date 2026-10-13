/*
 * elab2_expr.c — stage 2's expressions (2D Task 18; EL-13, EL-30, EL-31): the host through which
 * the shared typer reads the store's names, and the post-order walk that turns a typed expression
 * into signals and word cells with explicit stacks (no recursion). The cell parameters follow
 * Yosys 0.55's genrtlil.cc node by node: an identifier or select is generated at its own width,
 * an unsized constant, a concatenation and $signed/$unsigned at the context width, a cell at its
 * node's final width, and every cell extends its operands itself through *_SIGNED. All-constant
 * operators fold through odin3_cval_op. The cells, signals and assignments are elab2_assign.c,
 * selects elab2_select.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One node of the walk: how many operands it has emitted, and for a ternary which branches follow
 * its condition (1: then only, 2: else only, 3: both). */
typedef struct ex_frame {
    odin3_ast_id node;
    uint32_t next;
    uint8_t pick;
} ex_frame;

typedef struct ex_run {
    odin3_elab2 *e2;
    const odin3_elab2_expr_req *req;
} ex_run;

/* --- the typer's host (EL-13) ---------------------------------------------------------------- */

static const odin3_symbol *ex_symbol(const odin3_elab2 *e2, odin3_ast_id node) {
    return odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, node));
}

static const char *ex_sym_name(const odin3_elab2 *e2, const odin3_symbol *rec) {
    return odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
}

/* The declared width of a net or variable: its packed range, 1 without one. */
static uint32_t ex_sym_width(const odin3_symbol *rec) {
    if (!rec->has_packed) {
        return 1;
    }
    return (uint32_t)odin3_elab2_dim_size(&rec->packed);
}

/* A localparam is still unsized when its declarator's NUMBER says so (review I11): the symbol
 * holds only the bits and the sign. */
static bool ex_param_unsized(const odin3_elab2 *e2, const odin3_symbol *rec) {
    const odin3_ast_number *num =
        odin3_ast_number_of(e2->elab, odin3_ast_child(e2->elab, rec->declarator, 0));
    return num != NULL && num->width == 0;
}

/* A plain decimal (`5`) keeps its 32 bits in Yosys 0.55 whatever the context; only the apostrophe
 * forms (`'d5`, `'h3`) are widened to it. The AST does not record the apostrophe, so a signed
 * unsized decimal stands for the plain form (`'sd5` is taken as plain too; spec 17 #41). */
static bool ex_plain_decimal(const odin3_ast *ast, odin3_ast_id num) {
    const odin3_ast_number *payload = odin3_ast_number_of(ast, num);
    return payload != NULL && payload->width == 0 &&
           odin3_ast_sub(ast, num) == ODIN3_AST_NUMBER_DEC &&
           (odin3_ast_flags(ast, num) & ODIN3_AST_F_NUMBER_SIGNED) != 0;
}

/* Whether the NUMBER at num (or the one a parameter's declarator holds) extends to its context. */
static bool ex_widens(const odin3_ast *ast, odin3_ast_id num) {
    const odin3_ast_number *payload = odin3_ast_number_of(ast, num);
    return payload != NULL && payload->width == 0 && !ex_plain_decimal(ast, num);
}

static odin3_status ex_name_error(odin3_elab2 *e2, odin3_ast_id node, const char *fmt,
                                  const char *name) {
    odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node), fmt, name);
    return ODIN3_ERR_PARSE;
}

/* The type of an IDENT or HIER_NAME: a net or variable by its range, a parameter by its value. */
static odin3_status ex_name_type(odin3_elab2 *e2, odin3_ast_id node, odin3_etype *out) {
    const odin3_symbol *rec = ex_symbol(e2, node);
    if (rec == NULL) {
        return ex_name_error(e2, node, "'%s' is not declared", odin3_ast_name_str(e2->elab, node));
    }
    const char *name = ex_sym_name(e2, rec);
    if (rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR) {
        if (rec->n_unpacked > 0) {
            return ex_name_error(e2, node, "'%s' is a memory; select a word", name);
        }
        *out = (odin3_etype){ex_sym_width(rec), (rec->flags & ODIN3_SYM_F_SIGNED) != 0, false};
        return ODIN3_OK;
    }
    if ((rec->kind == ODIN3_SYM_PARAM || rec->kind == ODIN3_SYM_LOCALPARAM) && rec->has_value &&
        rec->value.len > 0) {
        *out = (odin3_etype){rec->value.len, rec->value_signed, ex_param_unsized(e2, rec)};
        return ODIN3_OK;
    }
    /* a generate or named block: Yosys makes an implicit net of it (divergence, spec §17) */
    return ex_name_error(e2, node, "'%s' is not a net, variable or parameter", name);
}

/* A SELECT of an array name with all its indices is a word (EL-27): its type is the array's word.
 * Any other select leaves the typer to type the object and the indices (the width stays 0). */
static odin3_status ex_select_leaf(const odin3_elab2 *e2, odin3_ast_id node, odin3_etype *out) {
    odin3_elab2_wordref ref;
    if (!odin3_elab2_mem_word_of(e2, node, &ref)) {
        return ODIN3_OK;
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref.mem);
    *out = (odin3_etype){mem->width, mem->is_signed, false};
    return ODIN3_OK;
}

static odin3_status ex_host_leaf(void *user, odin3_ast_id node, odin3_etype *out) {
    odin3_elab2 *e2 = user;
    switch (odin3_ast_kind_of(e2->elab, node)) {
    case ODIN3_AST_SELECT:
        return ex_select_leaf(e2, node, out);
    case ODIN3_AST_CALL: /* a user function: its result variable's type (Task 23) */
        return odin3_elab2_call_type(e2, node, out);
    default:
        return ex_name_type(e2, node, out);
    }
}

/* A replication count, part-select bound or +:/-: width: a NUMBER after stage 1 (EL-4). */
static odin3_status ex_host_constant(void *user, odin3_ast_id node, int64_t *out) {
    odin3_elab2 *e2 = user;
    bool known = false;
    odin3_status st = odin3_elab2_number_i64(e2, node, out, &known);
    if (st == ODIN3_OK && !known) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node), "%s has x or z bits",
                          odin3_ctype_constant_what(e2->ty));
        return ODIN3_ERR_PARSE;
    }
    return st;
}

static void ex_host_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    odin3_elab2 *e2 = user;
    if (level == ODIN3_LOG_ERROR) {
        odin3_elab2_error(e2, loc, "%s", msg);
    } else if (level == ODIN3_LOG_WARN) {
        odin3_elab2_warn_msg(e2, loc, msg);
    }
}

static odin3_status ex_typer(odin3_elab2 *e2) {
    if (e2->ty != NULL) {
        return ODIN3_OK;
    }
    odin3_vec_init(&e2->frames, sizeof(ex_frame));
    const odin3_ctype_host host = {ex_host_leaf, ex_host_constant, ex_host_report, e2};
    return odin3_ctype_create(e2->elab, &host, &e2->ty);
}

static odin3_etype ex_final(const odin3_elab2 *e2, odin3_ast_id node) {
    return odin3_ctype_final(e2->ty, node);
}

/* --- constants and folding ------------------------------------------------------------------- */

/* A constant signal of the bits (one odin3_bit per byte), repeating the last one up to width. */
static odin3_status ex_bits_sig(odin3_elab2 *e2, const odin3_value *val, uint32_t width,
                                odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        uint32_t from = i < val->len ? i : val->len - 1U;
        out->bits[i] = odin3_sb_of_bit((odin3_bit)val->bits[from]);
    }
    return st;
}

static bool ex_foldable(const odin3_sig *sig) {
    if (sig->width == 0) {
        return false;
    }
    for (uint32_t i = 0; i < sig->width; i++) {
        if (!odin3_sb_is_const(sig->bits[i])) {
            return false;
        }
    }
    return true;
}

static odin3_status ex_to_cval(odin3_elab2 *e2, const odin3_sig *sig, odin3_cval *out) {
    uint8_t *bytes = odin3_arena_alloc(e2->scratch, sig->width);
    if (bytes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < sig->width; i++) {
        bytes[i] = (uint8_t)odin3_sb_bit(sig->bits[i]);
    }
    const odin3_ast_literal lit = {
        {ODIN3_VAL_BITS, 0, bytes, sig->width, 0, 0}, sig->is_signed, false};
    return odin3_cval_from_literal(e2->scratch, &lit, out);
}

static odin3_status ex_from_cval(odin3_elab2 *e2, const odin3_cval *val, odin3_sig *out) {
    odin3_value bits;
    odin3_status st = odin3_cval_to_value(e2->scratch, val, &bits);
    return st != ODIN3_OK ? st : ex_bits_sig(e2, &bits, bits.len, out);
}

/* One operator on constant operands (b NULL for a unary one). */
typedef struct ex_bin {
    odin3_cop cop;
    odin3_elab2_type type;
    const odin3_sig *a, *b;
    uint32_t width;
    bool is_signed;
} ex_bin;

static odin3_status ex_fold(odin3_elab2 *e2, const ex_bin *bin, odin3_sig *out) {
    odin3_cval lhs;
    odin3_cval rhs;
    odin3_cval res;
    odin3_status st = ex_to_cval(e2, bin->a, &lhs);
    if (st == ODIN3_OK && bin->b != NULL) {
        st = ex_to_cval(e2, bin->b, &rhs);
    }
    const odin3_cop_args args = {bin->cop, &lhs, bin->b != NULL ? &rhs : NULL, bin->width,
                                 bin->is_signed};
    st = st != ODIN3_OK ? st : odin3_cval_op(e2->scratch, &args, &res);
    return st != ODIN3_OK ? st : ex_from_cval(e2, &res, out);
}

/* The lhs nets of the assignment when node is the root and its cell is exactly that wide. */
static const odin3_net_id *ex_dest(const ex_run *run, odin3_ast_id node, uint32_t width) {
    const odin3_elab2_expr_req *req = run->req;
    return node.v == req->expr.v && req->dest != NULL && width == req->context.width ? req->dest
                                                                                     : NULL;
}

/* An operator: folded when its operands are constant, else one cell. */
static odin3_status ex_binop(ex_run *run, odin3_ast_id node, const ex_bin *bin, odin3_sig *out) {
    if (ex_foldable(bin->a) && (bin->b == NULL || ex_foldable(bin->b))) {
        return ex_fold(run->e2, bin, out);
    }
    odin3_elab2_cell_req req = {
        bin->type, node, bin->a, bin->b, bin->width, ex_dest(run, node, bin->width), NULL};
    return odin3_elab2_cell(run->e2, &req, out);
}

/* --- leaves ---------------------------------------------------------------------------------- */

static odin3_status ex_number(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_ast_literal lit;
    odin3_status st = odin3_ast_number_value(e2->elab, frame->node, e2->scratch, &lit);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t width = ex_final(e2, frame->node).width;
    bool wide = lit.is_unsized && ex_widens(e2->elab, frame->node) && width > lit.value.len;
    return ex_bits_sig(e2, &lit.value, wide ? width : lit.value.len, out);
}

static odin3_status ex_string(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_cval text;
    odin3_status st = odin3_cval_from_string(
        e2->scratch, odin3_bytes_cstr(odin3_ast_name_str(e2->elab, frame->node)), &text);
    return st != ODIN3_OK ? st : ex_from_cval(e2, &text, out);
}

/* The nets of a wire, each through the reads hook inside a process. */
static odin3_status ex_wire_nets(ex_run *run, odin3_wire_id wire, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    const odin3_elab2_reads *reads = run->req->reads;
    uint32_t width = odin3_wire_width(e2->mod, wire);
    odin3_status st = odin3_elab2_sig_new(e2, width, out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        odin3_net_id net = odin3_wire_net(e2->mod, wire, bit);
        out->bits[bit] = net.v;
        if (reads != NULL) {
            st = reads->bit(reads->user, net, &out->bits[bit]);
        }
    }
    return st;
}

static odin3_status ex_name(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    const odin3_symbol *rec = ex_symbol(e2, frame->node);
    if (rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR) {
        if (rec->ir_wire.v == 0 || rec->ir_module.v != e2->mod_id.v) {
            return ex_name_error(e2, frame->node, "'%s' is not a net or variable of this module",
                                 ex_sym_name(e2, rec));
        }
        return ex_wire_nets(run, rec->ir_wire, out);
    }
    uint32_t width = ex_final(e2, frame->node).width;
    bool wide = ex_param_unsized(e2, rec) &&
                ex_widens(e2->elab, odin3_ast_child(e2->elab, rec->declarator, 0)) &&
                width > rec->value.len;
    return ex_bits_sig(e2, &rec->value, wide ? width : rec->value.len, out);
}

/* --- operands -------------------------------------------------------------------------------- */

/* Operand idx of the node on the frame (a copy: pushing a result moves the stack). */
static odin3_sig ex_operand(const odin3_elab2 *e2, const ex_frame *frame, uint32_t idx) {
    return *(const odin3_sig *)odin3_vec_cat(&e2->vals, e2->vals.len - frame->next + idx);
}

/* The sign genrtlil gives an operand it generates self-determined (genRTLIL() without hints):
 * a name, select or call is unsigned there whatever it is declared as, a unary ~ - + is its
 * operand's, anything else (a literal, an arithmetic or bitwise operator, a ternary) has the
 * sign the typer found. The cells of reductions, `!`, `&&`, `||` and a ternary's $reduce_bool
 * read it as A_SIGNED / B_SIGNED (Yosys 0.55: `&s` is A_SIGNED 0, `&(s + t)` is 1). */
static bool ex_gen_sign(const odin3_elab2 *e2, odin3_ast_id node) {
    const odin3_ast *ast = e2->elab;
    while (odin3_ast_kind_of(ast, node) == ODIN3_AST_UNARY) {
        uint32_t sub = odin3_ast_sub(ast, node);
        if (sub != ODIN3_AST_UNARY_PLUS && sub != ODIN3_AST_UNARY_MINUS &&
            sub != ODIN3_AST_UNARY_NOT) {
            break;
        }
        node = odin3_ast_child(ast, node, 0);
    }
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
    case ODIN3_AST_SELECT:
    case ODIN3_AST_CALL:
        return false;
    default:
        return ex_final(e2, node).is_signed;
    }
}

/* Operand idx of the node, stamped with the sign its cell reads (ex_gen_sign). */
static odin3_sig ex_self_operand(const odin3_elab2 *e2, const ex_frame *frame, uint32_t idx) {
    odin3_sig sig = ex_operand(e2, frame, idx);
    odin3_ast_id kid = odin3_ast_child(e2->elab, frame->node, idx);
    sig.is_signed = ex_gen_sign(e2, kid);
    return sig;
}

static uint32_t ex_max(uint32_t lhs, uint32_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

/* --- unary operators ------------------------------------------------------------------------- */

typedef struct ex_uop {
    odin3_cop cop;
    odin3_elab2_type type;
    bool context; /* the operand takes the context width (~ - +); else it is self-determined */
    bool negated; /* ~& and ~|: the reduction (1 bit) and a $logic_not of it */
} ex_uop;

static const ex_uop k_unary[ODIN3_AST_UNARY_COUNT] = {
    [ODIN3_AST_UNARY_PLUS] = {ODIN3_COP_POS, ODIN3_E2T_COUNT, true, false},
    [ODIN3_AST_UNARY_MINUS] = {ODIN3_COP_NEG, ODIN3_E2T_NEG, true, false},
    [ODIN3_AST_UNARY_LNOT] = {ODIN3_COP_LNOT, ODIN3_E2T_LOGIC_NOT, false, false},
    [ODIN3_AST_UNARY_NOT] = {ODIN3_COP_NOT, ODIN3_E2T_NOT, true, false},
    [ODIN3_AST_UNARY_AND] = {ODIN3_COP_RAND, ODIN3_E2T_REDUCE_AND, false, false},
    [ODIN3_AST_UNARY_NAND] = {ODIN3_COP_RNAND, ODIN3_E2T_REDUCE_AND, false, true},
    [ODIN3_AST_UNARY_OR] = {ODIN3_COP_ROR, ODIN3_E2T_REDUCE_OR, false, false},
    [ODIN3_AST_UNARY_NOR] = {ODIN3_COP_RNOR, ODIN3_E2T_REDUCE_OR, false, true},
    [ODIN3_AST_UNARY_XOR] = {ODIN3_COP_RXOR, ODIN3_E2T_REDUCE_XOR, false, false},
    [ODIN3_AST_UNARY_XNOR] = {ODIN3_COP_RXNOR, ODIN3_E2T_REDUCE_XNOR, false, false},
};

/* ~a, -a, +a: the operand extended to the node's width by wiring (by its own sign) first. */
static odin3_status ex_unary_context(ex_run *run, const ex_frame *frame, const ex_uop *op,
                                     odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig arg = ex_operand(e2, frame, 0);
    odin3_status st =
        odin3_elab2_extend(e2, &arg, ex_max(arg.width, fin.width), arg.is_signed, &arg);
    if (st != ODIN3_OK || op->cop == ODIN3_COP_POS) {
        *out = arg;
        return st;
    }
    return ex_binop(run, frame->node,
                    &(ex_bin){op->cop, op->type, &arg, NULL, arg.width, fin.is_signed}, out);
}

/* !a and the reductions: the operand as generated, Y_WIDTH max(final, 1). */
static odin3_status ex_unary_logic(ex_run *run, const ex_frame *frame, const ex_uop *op,
                                   odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig arg = ex_self_operand(e2, frame, 0);
    uint32_t width = ex_max(fin.width, 1);
    if (!op->negated || ex_foldable(&arg)) {
        return ex_binop(run, frame->node,
                        &(ex_bin){op->cop, op->type, &arg, NULL, width, fin.is_signed}, out);
    }
    /* ~& and ~|: the reduction (1 bit), then $logic_not of it (the parser's form, v4) */
    odin3_sig reduced;
    odin3_elab2_cell_req first = {op->type, frame->node, &arg, NULL, 1, NULL, NULL};
    odin3_status st = odin3_elab2_cell(e2, &first, &reduced);
    if (st != ODIN3_OK) {
        return st;
    }
    return ex_binop(
        run, frame->node,
        &(ex_bin){ODIN3_COP_LNOT, ODIN3_E2T_LOGIC_NOT, &reduced, NULL, width, fin.is_signed}, out);
}

static odin3_status ex_unary(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    uint32_t sub = odin3_ast_sub(run->e2->elab, frame->node);
    if (sub >= ODIN3_AST_UNARY_COUNT) {
        odin3_log(ODIN3_LOG_ERROR, "stage 2: unknown unary operator");
        return ODIN3_ERR_CHECK;
    }
    const ex_uop *op = &k_unary[sub];
    return op->context ? ex_unary_context(run, frame, op, out)
                       : ex_unary_logic(run, frame, op, out);
}

/* --- binary operators ------------------------------------------------------------------------ */

enum { CLS_ARITH, CLS_SHIFT, CLS_POW, CLS_CMP, CLS_CASE, CLS_LOGIC };

typedef struct ex_bop {
    odin3_cop cop;
    odin3_elab2_type type;
    uint8_t cls;
} ex_bop;

static const ex_bop k_binary[ODIN3_AST_BINARY_COUNT] = {
    [ODIN3_AST_BINARY_ADD] = {ODIN3_COP_ADD, ODIN3_E2T_ADD, CLS_ARITH},
    [ODIN3_AST_BINARY_SUB] = {ODIN3_COP_SUB, ODIN3_E2T_SUB, CLS_ARITH},
    [ODIN3_AST_BINARY_MUL] = {ODIN3_COP_MUL, ODIN3_E2T_MUL, CLS_ARITH},
    [ODIN3_AST_BINARY_DIV] = {ODIN3_COP_DIV, ODIN3_E2T_DIV, CLS_ARITH},
    [ODIN3_AST_BINARY_MOD] = {ODIN3_COP_MOD, ODIN3_E2T_MOD, CLS_ARITH},
    [ODIN3_AST_BINARY_POW] = {ODIN3_COP_POW, ODIN3_E2T_POW, CLS_POW},
    [ODIN3_AST_BINARY_EQ] = {ODIN3_COP_EQ, ODIN3_E2T_EQ, CLS_CMP},
    [ODIN3_AST_BINARY_NE] = {ODIN3_COP_NE, ODIN3_E2T_NE, CLS_CMP},
    [ODIN3_AST_BINARY_CASE_EQ] = {ODIN3_COP_EQX, ODIN3_E2T_EQ, CLS_CASE},
    [ODIN3_AST_BINARY_CASE_NE] = {ODIN3_COP_NEX, ODIN3_E2T_NE, CLS_CASE},
    [ODIN3_AST_BINARY_LAND] = {ODIN3_COP_LAND, ODIN3_E2T_LOGIC_AND, CLS_LOGIC},
    [ODIN3_AST_BINARY_LOR] = {ODIN3_COP_LOR, ODIN3_E2T_LOGIC_OR, CLS_LOGIC},
    [ODIN3_AST_BINARY_LT] = {ODIN3_COP_LT, ODIN3_E2T_LT, CLS_CMP},
    [ODIN3_AST_BINARY_LE] = {ODIN3_COP_LE, ODIN3_E2T_LE, CLS_CMP},
    [ODIN3_AST_BINARY_GT] = {ODIN3_COP_GT, ODIN3_E2T_GT, CLS_CMP},
    [ODIN3_AST_BINARY_GE] = {ODIN3_COP_GE, ODIN3_E2T_GE, CLS_CMP},
    [ODIN3_AST_BINARY_AND] = {ODIN3_COP_AND, ODIN3_E2T_AND, CLS_ARITH},
    [ODIN3_AST_BINARY_OR] = {ODIN3_COP_OR, ODIN3_E2T_OR, CLS_ARITH},
    [ODIN3_AST_BINARY_XOR] = {ODIN3_COP_XOR, ODIN3_E2T_XOR, CLS_ARITH},
    [ODIN3_AST_BINARY_XNOR] = {ODIN3_COP_XNOR, ODIN3_E2T_XNOR, CLS_ARITH},
    [ODIN3_AST_BINARY_SHL] = {ODIN3_COP_SHL, ODIN3_E2T_SHL, CLS_SHIFT},
    [ODIN3_AST_BINARY_SHR] = {ODIN3_COP_SHR, ODIN3_E2T_SHR, CLS_SHIFT},
    [ODIN3_AST_BINARY_ASHL] = {ODIN3_COP_SSHL, ODIN3_E2T_SHL, CLS_SHIFT},
    [ODIN3_AST_BINARY_ASHR] = {ODIN3_COP_SSHR, ODIN3_E2T_SSHR, CLS_SHIFT},
};

/* a ** b: Yosys rewrites a constant 2 with an unsigned exponent to $shl(1, b). */
static odin3_status ex_pow(ex_run *run, const ex_frame *frame, const ex_bop *op, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig lhs = ex_operand(e2, frame, 0);
    odin3_sig rhs = ex_operand(e2, frame, 1);
    int64_t base = 0;
    bool fold = ex_foldable(&lhs) && ex_foldable(&rhs);
    if (!fold && !rhs.is_signed && odin3_elab2_sig_i64(&lhs, &base) == 1 && base == 2) {
        odin3_sig one;
        odin3_status st = odin3_elab2_sig_new(e2, lhs.width, &one);
        if (st != ODIN3_OK) {
            return st;
        }
        one.bits[0] = odin3_sb_of_bit(ODIN3_BIT_1);
        one.is_signed = lhs.is_signed;
        return ex_binop(
            run, frame->node,
            &(ex_bin){ODIN3_COP_SHL, ODIN3_E2T_SHL, &one, &rhs, fin.width, fin.is_signed}, out);
    }
    return ex_binop(run, frame->node,
                    &(ex_bin){op->cop, op->type, &lhs, &rhs, fin.width, fin.is_signed}, out);
}

/* The wildcard compare of E9: positions where either side is a constant x or z match anything. */
typedef struct ex_wild {
    odin3_sig a, b; /* the kept positions of each side */
    bool dropped;
} ex_wild;

static bool ex_is_xz(odin3_sigbit sb) {
    return odin3_sb_is_const(sb) &&
           (odin3_sb_bit(sb) == ODIN3_BIT_X || odin3_sb_bit(sb) == ODIN3_BIT_Z);
}

/* Both sides at their common width; the positions neither side leaves to a wildcard. */
static odin3_status ex_wild_cut(odin3_elab2 *e2, const odin3_sig *lhs, const odin3_sig *rhs,
                                ex_wild *out) {
    uint32_t width = ex_max(lhs->width, rhs->width);
    odin3_sig left;
    odin3_sig right;
    odin3_status st = odin3_elab2_extend(e2, lhs, width, lhs->is_signed, &left);
    st = st != ODIN3_OK ? st : odin3_elab2_extend(e2, rhs, width, rhs->is_signed, &right);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(e2, width, &out->a);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(e2, width, &out->b);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < width; i++) {
        if (ex_is_xz(left.bits[i]) || ex_is_xz(right.bits[i])) {
            out->dropped = true;
            continue;
        }
        out->a.bits[kept] = left.bits[i];
        out->b.bits[kept++] = right.bits[i];
    }
    out->a.width = out->b.width = kept;
    return ODIN3_OK;
}

/* a === b and a !== b: exact when both are constant; else a compare of what a wildcard leaves. */
static odin3_status ex_case_eq(ex_run *run, const ex_frame *frame, const ex_bop *op,
                               odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig lhs = ex_operand(e2, frame, 0);
    odin3_sig rhs = ex_operand(e2, frame, 1);
    uint32_t width = ex_max(fin.width, 1);
    if (ex_foldable(&lhs) && ex_foldable(&rhs)) {
        return ex_fold(e2, &(ex_bin){op->cop, op->type, &lhs, &rhs, width, false}, out);
    }
    ex_wild wild = {{NULL, 0, false}, {NULL, 0, false}, false};
    odin3_status st = ex_wild_cut(e2, &lhs, &rhs, &wild);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_loc loc = odin3_ast_loc(e2->elab, frame->node);
    if (wild.dropped) {
        odin3_elab2_warn(e2, loc, "'===' compares against x/z bits; they match any value");
    } else {
        odin3_elab2_warn(e2, loc, "'===' is treated as '==' (synthesis has no x or z)");
    }
    if (wild.a.width == 0) { /* every position matches: === is 1, !== is 0 */
        st = odin3_elab2_sig_new(e2, width, out);
        if (st == ODIN3_OK) {
            out->bits[0] = odin3_sb_of_bit(op->cop == ODIN3_COP_EQX ? ODIN3_BIT_1 : ODIN3_BIT_0);
        }
        return st;
    }
    wild.a.is_signed = wild.b.is_signed = false;
    odin3_elab2_cell_req req = {
        op->type, frame->node, &wild.a, &wild.b, width, ex_dest(run, frame->node, width), NULL};
    return odin3_elab2_cell(e2, &req, out);
}

static odin3_status ex_binary(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    const odin3_elab2 *e2 = run->e2;
    uint32_t sub = odin3_ast_sub(e2->elab, frame->node);
    if (sub >= ODIN3_AST_BINARY_COUNT) {
        odin3_log(ODIN3_LOG_ERROR, "stage 2: unknown binary operator");
        return ODIN3_ERR_CHECK;
    }
    const ex_bop *op = &k_binary[sub];
    if (op->cls == CLS_POW) {
        return ex_pow(run, frame, op, out);
    }
    if (op->cls == CLS_CASE) {
        return ex_case_eq(run, frame, op, out);
    }
    odin3_etype fin = ex_final(e2, frame->node);
    bool logic = op->cls == CLS_LOGIC;
    odin3_sig lhs = logic ? ex_self_operand(e2, frame, 0) : ex_operand(e2, frame, 0);
    odin3_sig rhs = logic ? ex_self_operand(e2, frame, 1) : ex_operand(e2, frame, 1);
    uint32_t width = ex_max(fin.width, 1);
    if (op->cls == CLS_ARITH) {
        width = ex_max(width, ex_max(lhs.width, rhs.width));
    } else if (op->cls == CLS_SHIFT) {
        rhs.is_signed = false; /* the amount is self-determined and unsigned */
    }
    return ex_binop(run, frame->node,
                    &(ex_bin){op->cop, op->type, &lhs, &rhs, width, fin.is_signed}, out);
}

/* --- ternary, concatenation, replication, $signed ------------------------------------------- */

/* The condition decides the branch when it is constant and fully 0/1 (Yosys: is_fully_def). */
static uint8_t ex_pick(const odin3_sig *cond) {
    bool truth = false;
    for (uint32_t i = 0; i < cond->width; i++) {
        odin3_sigbit sb = cond->bits[i];
        if (!odin3_sb_is_const(sb) || ex_is_xz(sb)) {
            return 3;
        }
        truth |= odin3_sb_bit(sb) == ODIN3_BIT_1;
    }
    return truth ? 1 : 2;
}

/* c ? a : b with a constant condition: the chosen branch only, extended to the context. */
static odin3_status ex_ternary_pick(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig chosen = ex_operand(e2, frame, 1);
    return odin3_elab2_extend(e2, &chosen, ex_max(chosen.width, fin.width), fin.is_signed, out);
}

/* The one-bit select: the condition itself, or $reduce_bool of a wider one. */
static odin3_status ex_select_bit(ex_run *run, const ex_frame *frame, odin3_sig cond,
                                  odin3_sig *out) {
    if (cond.width == 1) {
        *out = cond;
        return ODIN3_OK;
    }
    odin3_elab2_cell_req req = {ODIN3_E2T_REDUCE_BOOL, frame->node, &cond, NULL, 1, NULL, NULL};
    return odin3_elab2_cell(run->e2, &req, out);
}

/* A constant condition and branches: the branches merged where the condition is x (IEEE 5-21). */
static odin3_status ex_ternary_fold(odin3_elab2 *e2, const odin3_sig *cond,
                                    const odin3_sig *then_val, const odin3_sig *else_val,
                                    odin3_sig *out) {
    odin3_cval cond_c;
    odin3_cval then_c;
    odin3_cval else_c;
    odin3_cval res;
    odin3_status st = ex_to_cval(e2, cond, &cond_c);
    st = st != ODIN3_OK ? st : ex_to_cval(e2, then_val, &then_c);
    st = st != ODIN3_OK ? st : ex_to_cval(e2, else_val, &else_c);
    const odin3_cval_mux_args args = {&cond_c, &then_c, &else_c};
    st = st != ODIN3_OK ? st : odin3_cval_mux(e2->scratch, &args, &res);
    return st != ODIN3_OK ? st : ex_from_cval(e2, &res, out);
}

/* $mux(A = else, B = then, S = condition) as wide as the branches. */
static odin3_status ex_ternary_mux(ex_run *run, const ex_frame *frame, const odin3_sig *branches,
                                   odin3_sig *out) {
    odin3_sig select;
    odin3_status st = ex_select_bit(run, frame, ex_self_operand(run->e2, frame, 0), &select);
    uint32_t width = branches[0].width;
    odin3_elab2_cell_req req = {ODIN3_E2T_MUX, frame->node, &branches[1],
                                &branches[0],  width,       ex_dest(run, frame->node, width),
                                &select};
    return st != ODIN3_OK ? st : odin3_elab2_cell(run->e2, &req, out);
}

static odin3_status ex_ternary(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    if (frame->pick != 3) {
        return ex_ternary_pick(run, frame, out);
    }
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig cond = ex_operand(e2, frame, 0);
    odin3_sig branches[2] = {ex_operand(e2, frame, 1), ex_operand(e2, frame, 2)}; /* then, else */
    uint32_t width = ex_max(branches[0].width, branches[1].width);
    odin3_status st = odin3_elab2_extend(e2, &branches[0], width, fin.is_signed, &branches[0]);
    st = st != ODIN3_OK ? st
                        : odin3_elab2_extend(e2, &branches[1], width, fin.is_signed, &branches[1]);
    if (st == ODIN3_OK && ex_foldable(&cond) && ex_foldable(&branches[0]) &&
        ex_foldable(&branches[1])) {
        st = ex_ternary_fold(e2, &cond, &branches[0], &branches[1], out);
    } else if (st == ODIN3_OK) {
        st = ex_ternary_mux(run, frame, branches, out);
    }
    return st != ODIN3_OK
               ? st
               : odin3_elab2_extend(e2, out, ex_max(out->width, fin.width), fin.is_signed, out);
}

/* {a, b, ...}: the last part is the least significant; zero-extended to the context. */
static odin3_status ex_concat(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    uint32_t total = 0;
    for (uint32_t k = 0; k < frame->next; k++) {
        total += ex_operand(e2, frame, k).width;
    }
    odin3_sig joined;
    odin3_status st = odin3_elab2_sig_new(e2, total, &joined);
    uint32_t at = 0;
    for (uint32_t k = frame->next; st == ODIN3_OK && k > 0; k--) {
        odin3_sig part = ex_operand(e2, frame, k - 1);
        if (part.width > 0) {
            memcpy(&joined.bits[at], part.bits, (size_t)part.width * sizeof *part.bits);
        }
        at += part.width;
    }
    uint32_t width = ex_max(total, ex_final(e2, frame->node).width);
    return st != ODIN3_OK ? st : odin3_elab2_extend(e2, &joined, width, false, out);
}

/* {n{...}}: the body n times; zero-extended to the context. */
static odin3_status ex_replicate(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_sig body = ex_operand(e2, frame, 0);
    int64_t count = 0;
    bool known = false;
    odin3_status st =
        odin3_elab2_number_i64(e2, odin3_ast_child(e2->elab, frame->node, 0), &count, &known);
    if (st != ODIN3_OK || !known || count < 0 ||
        (uint64_t)count * body.width > e2->limits->max_width) {
        return st != ODIN3_OK ? st : ODIN3_ERR_PARSE; /* the typer reported it */
    }
    odin3_sig joined;
    st = odin3_elab2_sig_new(e2, (uint32_t)count * body.width, &joined);
    for (uint32_t k = 0; st == ODIN3_OK && k < (uint32_t)count; k++) {
        if (body.width > 0) {
            memcpy(&joined.bits[(size_t)k * body.width], body.bits,
                   (size_t)body.width * sizeof *body.bits);
        }
    }
    uint32_t width = ex_max(joined.width, ex_final(e2, frame->node).width);
    return st != ODIN3_OK ? st : odin3_elab2_extend(e2, &joined, width, false, out);
}

/* $signed(a), $unsigned(a): a's bits, extended to the context by the context's sign. */
static odin3_status ex_sign_call(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_etype fin = ex_final(e2, frame->node);
    odin3_sig arg = ex_operand(e2, frame, 0);
    return odin3_elab2_extend(e2, &arg, ex_max(arg.width, fin.width), fin.is_signed, out);
}

/* The x word: a register word outside the array reads x. */
static odin3_status ex_x_word(ex_run *run, uint32_t width, odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(run->e2, width, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        out->bits[k] = odin3_sb_of_bit(ODIN3_BIT_X);
    }
    return st;
}

/* A word of a register array with a variable index (EL-27): a case over the words, inline in the
 * process when the walker has the hook for it, else a combinational process of its own. */
static odin3_status ex_word_variable(ex_run *run, const ex_frame *frame,
                                     const odin3_elab2_wordref *ref, const odin3_sig *idx,
                                     odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_ast_id node = frame->node;
    if (ref->n != 1) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node),
                          "non-constant index into the multi-dimensional register array '%s' is "
                          "not supported",
                          odin3_strtab_get(odin3_design_strtab(e2->design),
                                           odin3_elab2_mem_get(e2, ref->mem)->name));
        return ODIN3_ERR_PARSE;
    }
    const odin3_elab2_regs_rd rd = {node, ref->mem, idx};
    const odin3_elab2_reads *reads = run->req->reads;
    if (reads != NULL && reads->word != NULL) {
        return reads->word(reads->user, &rd, out);
    }
    return odin3_elab2_comb_word(e2, &rd, out);
}

/* A memory word `m[i]` (EL-27): a `$mem` read port, or the word of an array that became
 * registers; a variable index into registers is Task 23's. The operands are the indices. */
static odin3_status ex_word(ex_run *run, const ex_frame *frame, const odin3_elab2_wordref *ref,
                            odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_sig *idx = odin3_arena_alloc(e2->scratch, (size_t)ref->n * sizeof *idx);
    if (idx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < ref->n; k++) {
        idx[k] = ex_operand(e2, frame, k);
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref->mem);
    odin3_elab2_mem_rd rd = {ref->mem, frame->node, idx, ref->n};
    if (!mem->to_regs) {
        return odin3_elab2_mem_read(e2, &rd, out);
    }
    odin3_elab2_word word;
    odin3_status st = odin3_elab2_mem_word(e2, &rd, false, &word);
    if (st != ODIN3_OK) {
        return st;
    }
    if (word.kind == ODIN3_WORD_VARIABLE) {
        return ex_word_variable(run, frame, ref, idx, out);
    }
    return word.kind == ODIN3_WORD_WIRE ? ex_wire_nets(run, word.wire, out)
                                        : ex_x_word(run, mem->width, out);
}

static odin3_status ex_select(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_elab2_wordref ref;
    if (odin3_elab2_mem_word_of(e2, frame->node, &ref)) {
        return ex_word(run, frame, &ref, out);
    }
    odin3_sig object = ex_operand(e2, frame, 0);
    odin3_sig index = frame->next > 1 ? ex_operand(e2, frame, 1) : (odin3_sig){NULL, 0, false};
    const odin3_elab2_expr_req *req = run->req;
    bool root = frame->node.v == req->expr.v;
    odin3_elab2_selargs args = {frame->node, &object, frame->next > 1 ? &index : NULL,
                                root ? req->dest : NULL, req->context.width};
    return odin3_elab2_select_emit(e2, &args, out);
}

/* --- the walk -------------------------------------------------------------------------------- */

static bool ex_is_sign_call(const odin3_elab2 *e2, odin3_ast_id call) {
    if ((odin3_ast_flags(e2->elab, call) & ODIN3_AST_F_CALL_SYSTEM) == 0) {
        return false;
    }
    const char *name = odin3_ast_name_str(e2->elab, call);
    return strcmp(name, "$signed") == 0 || strcmp(name, "$unsigned") == 0;
}

/* A user function call (EL-38): folded by the interpreter when its arguments and body are
 * constant; else inlined into the process through the walker's hook, or, in a continuous context,
 * a combinational process of its own. */
static odin3_status ex_user_call(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    odin3_elab2 *e2 = run->e2;
    odin3_cval val;
    bool is_const = false;
    odin3_status st = odin3_elab2_call_try(e2, frame->node, &val, &is_const);
    if (st != ODIN3_OK) {
        return st;
    }
    if (is_const) {
        return ex_from_cval(e2, &val, out);
    }
    const odin3_elab2_reads *reads = run->req->reads;
    if (reads != NULL && reads->call != NULL) {
        return reads->call(reads->user, frame->node, out);
    }
    return odin3_elab2_comb_call(e2, frame->node, out);
}

/* A ternary's operands: its condition, then the branches its pick leaves (frame->pick). */
static odin3_ast_id ex_kid_ternary(const odin3_ast *ast, const ex_frame *frame, uint32_t idx) {
    odin3_ast_id none = {0};
    if (idx == 0) {
        return odin3_ast_child(ast, frame->node, 0);
    }
    if (frame->pick == 3) {
        return idx < 3 ? odin3_ast_child(ast, frame->node, idx) : none;
    }
    return frame->pick != 0 && idx == 1 ? odin3_ast_child(ast, frame->node, frame->pick) : none;
}

/* A select's operands: its object, then its index (not for [m:l]: its bounds are NUMBERs). A memory
 * word has the index expressions of its dimensions instead (EL-27). */
static odin3_ast_id ex_kid_select(const odin3_elab2 *e2, odin3_ast_id node, uint32_t idx) {
    const odin3_ast *ast = e2->elab;
    odin3_ast_id none = {0};
    odin3_elab2_wordref ref;
    if (odin3_elab2_mem_word_of(e2, node, &ref)) {
        return idx < ref.n ? odin3_elab2_mem_word_index(e2, node, idx) : none;
    }
    if (idx == 0) {
        return odin3_ast_child(ast, node, 0);
    }
    bool indexed = odin3_ast_sub(ast, node) != ODIN3_AST_SELECT_PART;
    return idx == 1 && indexed ? odin3_ast_child(ast, node, 1) : none;
}

/* How many operands a node whose operands are its children in order emits. */
static uint32_t ex_plain_count(const odin3_ast *ast, odin3_ast_id node) {
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_UNARY:
        return 1;
    case ODIN3_AST_BINARY:
        return 2;
    case ODIN3_AST_CONCAT:
        return odin3_ast_nchild(ast, node);
    default:
        return 0;
    }
}

/* The idx-th operand the node emits, 0 when there are no more. A replication emits only its body
 * (the count is a NUMBER); $signed and $unsigned their argument. */
static odin3_ast_id ex_kid(const odin3_elab2 *e2, const ex_frame *frame, uint32_t idx) {
    const odin3_ast *ast = e2->elab;
    odin3_ast_id none = {0};
    switch (odin3_ast_kind_of(ast, frame->node)) {
    case ODIN3_AST_TERNARY:
        return ex_kid_ternary(ast, frame, idx);
    case ODIN3_AST_SELECT:
        return ex_kid_select(e2, frame->node, idx);
    case ODIN3_AST_REPLICATE:
        return idx < 1 ? odin3_ast_child(ast, frame->node, 1) : none;
    case ODIN3_AST_CALL:
        return idx < 1 && ex_is_sign_call(e2, frame->node) ? odin3_ast_child(ast, frame->node, 0)
                                                           : none;
    default:
        return idx < ex_plain_count(ast, frame->node) ? odin3_ast_child(ast, frame->node, idx)
                                                      : none;
    }
}

/* One node whose operands are all emitted. */
static odin3_status ex_node(ex_run *run, const ex_frame *frame, odin3_sig *out) {
    switch (odin3_ast_kind_of(run->e2->elab, frame->node)) {
    case ODIN3_AST_NUMBER:
        return ex_number(run, frame, out);
    case ODIN3_AST_STRING:
        return ex_string(run, frame, out);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return ex_name(run, frame, out);
    case ODIN3_AST_UNARY:
        return ex_unary(run, frame, out);
    case ODIN3_AST_BINARY:
        return ex_binary(run, frame, out);
    case ODIN3_AST_TERNARY:
        return ex_ternary(run, frame, out);
    case ODIN3_AST_CONCAT:
        return ex_concat(run, frame, out);
    case ODIN3_AST_REPLICATE:
        return ex_replicate(run, frame, out);
    case ODIN3_AST_SELECT:
        return ex_select(run, frame, out);
    case ODIN3_AST_CALL:
        return ex_is_sign_call(run->e2, frame->node) ? ex_sign_call(run, frame, out)
                                                     : ex_user_call(run, frame, out);
    default:
        odin3_log(ODIN3_LOG_ERROR, "stage 2: '%s' is not an expression",
                  odin3_ast_kind_name(odin3_ast_kind_of(run->e2->elab, frame->node)));
        return ODIN3_ERR_CHECK;
    }
}

/* The node's operands leave the value stack and its signal, in the node's sign, replaces them. */
static odin3_status ex_finish(ex_run *run, const ex_frame *frame, odin3_sig *sig) {
    odin3_elab2 *e2 = run->e2;
    sig->is_signed = ex_final(e2, frame->node).is_signed;
    for (uint32_t i = 0; i < frame->next; i++) {
        odin3_vec_pop(&e2->vals);
    }
    odin3_sig *slot = odin3_vec_push(&e2->vals);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *sig;
    return ODIN3_OK;
}

static odin3_status ex_open(odin3_elab2 *e2, odin3_ast_id node) {
    ex_frame *slot = odin3_vec_push(&e2->frames);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (ex_frame){node, 0, 0};
    return ODIN3_OK;
}

/* One step: emit the frame's next operand, or build the node when they are all emitted. */
static odin3_status ex_step(ex_run *run) {
    odin3_elab2 *e2 = run->e2;
    ex_frame *top = odin3_vec_at(&e2->frames, e2->frames.len - 1);
    if (odin3_ast_kind_of(e2->elab, top->node) == ODIN3_AST_TERNARY && top->next == 1 &&
        top->pick == 0) {
        odin3_sig cond = *(const odin3_sig *)odin3_vec_cat(&e2->vals, e2->vals.len - 1);
        top->pick = ex_pick(&cond);
    }
    odin3_ast_id kid = ex_kid(e2, top, top->next);
    if (kid.v != 0) {
        top->next++;
        return ex_open(e2, kid);
    }
    ex_frame done = *top;
    odin3_vec_pop(&e2->frames);
    odin3_sig sig;
    odin3_status st = ex_node(run, &done, &sig);
    return st != ODIN3_OK ? st : ex_finish(run, &done, &sig);
}

static bool ex_is_expression(const odin3_elab2 *e2, odin3_ast_id node) {
    return node.v != 0 &&
           odin3_ast_class_has(ODIN3_AST_CLS_E,
                               odin3_ast_kind_info_of((uint32_t)odin3_ast_kind_of(e2->elab, node)));
}

odin3_status odin3_elab2_expr(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out) {
    if (e2->mod == NULL || !ex_is_expression(e2, req->expr)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_expr: not an expression, or no module is emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    /* re-entrant: a user call inlined by the walker generates its own expressions on the stacks
     * above this one's */
    ex_run run = {e2, req};
    size_t frame_base = e2->frames.len;
    size_t val_base = e2->vals.len;
    odin3_status st = ex_typer(e2);
    st = st != ODIN3_OK ? st : odin3_ctype_expr(e2->ty, req->expr, req->context);
    st = st != ODIN3_OK ? st : ex_open(e2, req->expr);
    while (st == ODIN3_OK && e2->frames.len > frame_base) {
        st = ex_step(&run);
    }
    if (st == ODIN3_OK) {
        *out = *(const odin3_sig *)odin3_vec_cat(&e2->vals, val_base);
    }
    while (e2->frames.len > frame_base) {
        odin3_vec_pop(&e2->frames);
    }
    while (e2->vals.len > val_base) {
        odin3_vec_pop(&e2->vals);
    }
    return st;
}

/* --- types without generating, and constant-ness by the AST --------------------------------- */

odin3_status odin3_elab2_self_type(odin3_elab2 *e2, odin3_ast_id expr, odin3_etype *out) {
    if (!ex_is_expression(e2, expr)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_self_type: not an expression");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ex_typer(e2);
    st = st != ODIN3_OK ? st : odin3_ctype_expr(e2->ty, expr, (odin3_etype){0, false, false});
    if (st == ODIN3_OK) {
        *out = odin3_ctype_final(e2->ty, expr);
    }
    return st;
}

/* A system call that keeps its argument constant: $signed, $unsigned, $clog2. */
static bool ex_const_call(const odin3_elab2 *e2, odin3_ast_id node) {
    if ((odin3_ast_flags(e2->elab, node) & ODIN3_AST_F_CALL_SYSTEM) == 0) {
        return false;
    }
    const char *name = odin3_ast_name_str(e2->elab, node);
    return strcmp(name, "$signed") == 0 || strcmp(name, "$unsigned") == 0 ||
           strcmp(name, "$clog2") == 0;
}

/* One entered node of the constant test: false when it reads a variable or calls a function. */
static bool ex_const_node(const odin3_elab2 *e2, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e2->elab, node)) {
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME: {
        const odin3_symbol *rec = ex_symbol(e2, node);
        return rec != NULL && (rec->kind == ODIN3_SYM_PARAM || rec->kind == ODIN3_SYM_LOCALPARAM ||
                               rec->kind == ODIN3_SYM_GENVAR ||
                               odin3_elab2_is_bound(e2, odin3_elab_sym_of(&e2->bind, node)));
    }
    case ODIN3_AST_CALL:
        return ex_const_call(e2, node);
    default:
        return true;
    }
}

odin3_status odin3_elab2_expr_is_const(const odin3_elab2 *e2, odin3_ast_id expr, bool *out) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, expr);
    if (st != ODIN3_OK) {
        return st;
    }
    bool constant = true;
    odin3_ast_event ev;
    odin3_ast_id node;
    while (constant && odin3_ast_cursor_next(&cur, &ev, &node)) {
        constant = ev != ODIN3_AST_ENTER || ex_const_node(e2, node);
    }
    odin3_ast_cursor_free(&cur);
    *out = constant;
    return ODIN3_OK;
}
