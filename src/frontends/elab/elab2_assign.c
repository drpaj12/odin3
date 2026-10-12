/*
 * elab2_assign.c — stage 2's signals, word cells and continuous assignments (2D Task 18; EL-28,
 * EL-30, EL-31): signals in the scratch arena and the wiring that extends and truncates them, the
 * one function that makes a word cell with genrtlil's parameter rules and the cell-pins cap of
 * point (aa), and the assignment of a right side to a net lvalue (by construction when a cell
 * drives the lhs nets, else by merge). The expression walk is elab2_expr.c, selects elab2_select.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- signals --------------------------------------------------------------------------------- */

odin3_status odin3_elab2_sig_new(odin3_elab2 *e2, uint32_t width, odin3_sig *out) {
    odin3_sigbit *bits = NULL;
    if (width > 0) {
        bits = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *bits);
        if (bits == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < width; i++) {
            bits[i] = odin3_sb_of_bit(ODIN3_BIT_0);
        }
    }
    *out = (odin3_sig){bits, width, false};
    return ODIN3_OK;
}

odin3_status odin3_elab2_extend(odin3_elab2 *e2, const odin3_sig *in, uint32_t width,
                                bool is_signed, odin3_sig *out) {
    odin3_sig src = *in;
    if (width <= src.width) { /* the low bits are a prefix: shared, not copied */
        *out = (odin3_sig){src.bits, width, is_signed};
        return ODIN3_OK;
    }
    odin3_sig wide;
    odin3_status st = odin3_elab2_sig_new(e2, width, &wide);
    if (st != ODIN3_OK) {
        return st;
    }
    if (src.width > 0) {
        memcpy(wide.bits, src.bits, (size_t)src.width * sizeof *wide.bits);
    }
    odin3_sigbit fill =
        is_signed && src.width > 0 ? src.bits[src.width - 1] : odin3_sb_of_bit(ODIN3_BIT_0);
    for (uint32_t i = src.width; i < width; i++) {
        wide.bits[i] = fill;
    }
    wide.is_signed = is_signed;
    *out = wide;
    return ODIN3_OK;
}

odin3_status odin3_elab2_to_nets(odin3_elab2 *e2, const odin3_sig *sig, odin3_net_id *nets) {
    if (e2->mod == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_to_nets: no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < sig->width; i++) {
        odin3_sigbit sb = sig->bits[i];
        if ((sb & ODIN3_SB_CONST) == 0) {
            nets[i] = (odin3_net_id){sb};
            continue;
        }
        odin3_bit bit = odin3_sb_is_const(sb) ? odin3_sb_bit(sb) : ODIN3_BIT_X; /* ANY: x */
        odin3_status st = odin3_elab2_const_net(e2, bit, &nets[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- word cells (EL-30, EL-31) --------------------------------------------------------------- */

enum { CELL_NONE, CELL_BINARY, CELL_UNARY, CELL_MUX };
enum {
    CELL_MAX_PARAMS = 5, /* a binary cell's: A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH */
    CELL_UNARY_PARAMS = 3,
    CELL_MUX_PARAMS = 1,
    CELL_MAX_PORTS = 4,
    PORT_A = 0,
    PORT_B,
    PORT_S,
    PORT_Y
};

/* The shape of each word type (the rest of odin3_elab2_type is not a cell this function makes). */
static const uint8_t k_shape[ODIN3_E2T_COUNT] = {
    [ODIN3_E2T_ADD] = CELL_BINARY,        [ODIN3_E2T_SUB] = CELL_BINARY,
    [ODIN3_E2T_MUL] = CELL_BINARY,        [ODIN3_E2T_DIV] = CELL_BINARY,
    [ODIN3_E2T_MOD] = CELL_BINARY,        [ODIN3_E2T_POW] = CELL_BINARY,
    [ODIN3_E2T_AND] = CELL_BINARY,        [ODIN3_E2T_OR] = CELL_BINARY,
    [ODIN3_E2T_XOR] = CELL_BINARY,        [ODIN3_E2T_XNOR] = CELL_BINARY,
    [ODIN3_E2T_NOT] = CELL_UNARY,         [ODIN3_E2T_NEG] = CELL_UNARY,
    [ODIN3_E2T_SHL] = CELL_BINARY,        [ODIN3_E2T_SHR] = CELL_BINARY,
    [ODIN3_E2T_SSHR] = CELL_BINARY,       [ODIN3_E2T_SHIFTX] = CELL_BINARY,
    [ODIN3_E2T_EQ] = CELL_BINARY,         [ODIN3_E2T_NE] = CELL_BINARY,
    [ODIN3_E2T_LT] = CELL_BINARY,         [ODIN3_E2T_LE] = CELL_BINARY,
    [ODIN3_E2T_GT] = CELL_BINARY,         [ODIN3_E2T_GE] = CELL_BINARY,
    [ODIN3_E2T_REDUCE_AND] = CELL_UNARY,  [ODIN3_E2T_REDUCE_OR] = CELL_UNARY,
    [ODIN3_E2T_REDUCE_XOR] = CELL_UNARY,  [ODIN3_E2T_REDUCE_XNOR] = CELL_UNARY,
    [ODIN3_E2T_REDUCE_BOOL] = CELL_UNARY, [ODIN3_E2T_LOGIC_NOT] = CELL_UNARY,
    [ODIN3_E2T_LOGIC_AND] = CELL_BINARY,  [ODIN3_E2T_LOGIC_OR] = CELL_BINARY,
    [ODIN3_E2T_MUX] = CELL_MUX,
};

/* One cell being made: the request, its shape, the nets of each port and the parameters. */
typedef struct cell_ctx {
    odin3_elab2 *e2;
    const odin3_elab2_cell_req *req;
    int shape;
    uint64_t pins;
    const odin3_net_id *nets[CELL_MAX_PORTS];
    uint32_t count[CELL_MAX_PORTS];
    odin3_value params[CELL_MAX_PARAMS];
    uint32_t n_params;
    odin3_prov_id prov;
    bool fresh; /* Y nets were made for this cell */
} cell_ctx;

static odin3_status cell_bad(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_cell: %s", why);
    return ODIN3_ERR_INVALID_ARG;
}

/* The shape, the port widths and the pin count; the operands must fit the type. */
static odin3_status cell_plan(cell_ctx *cx) {
    const odin3_elab2_cell_req *req = cx->req;
    cx->shape = k_shape[req->type];
    if (cx->shape == CELL_NONE || req->a == NULL || req->y_width == 0 || req->a->width == 0) {
        return cell_bad("not a word type, or an empty operand");
    }
    cx->count[PORT_A] = req->a->width;
    cx->count[PORT_Y] = req->y_width;
    if (cx->shape == CELL_BINARY) {
        if (req->b == NULL || req->b->width == 0) {
            return cell_bad("a binary cell needs two operands");
        }
        cx->count[PORT_B] = req->b->width;
    } else if (cx->shape == CELL_MUX) {
        if (req->b == NULL || req->s == NULL || req->s->width != 1 ||
            req->a->width != req->y_width || req->b->width != req->y_width) {
            return cell_bad("a $mux needs A and B as wide as Y and a one-bit S");
        }
        cx->count[PORT_B] = req->y_width;
        cx->count[PORT_S] = 1;
    }
    cx->pins = (uint64_t)cx->count[PORT_A] + cx->count[PORT_B] + cx->count[PORT_S] + req->y_width;
    return ODIN3_OK;
}

/* Point (aa): the pins of one cell. Fatal; nothing has been made. */
static odin3_status cell_cap(const cell_ctx *cx) {
    odin3_elab2 *e2 = cx->e2;
    if (cx->pins <= e2->limits->max_cell_pins) {
        return ODIN3_OK;
    }
    odin3_elab2_fatal(e2, odin3_ast_loc(e2->elab, cx->req->node),
                      "cell of %" PRIu64 " pins exceeds the limit of %" PRIu32 " pins", cx->pins,
                      e2->limits->max_cell_pins);
    return ODIN3_ERR_PARSE;
}

static odin3_status cell_port_nets(cell_ctx *cx, unsigned port, const odin3_sig *sig) {
    odin3_net_id *nets = odin3_arena_alloc(cx->e2->scratch, (size_t)sig->width * sizeof *nets);
    if (nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    cx->nets[port] = nets;
    return odin3_elab2_to_nets(cx->e2, sig, nets);
}

/* The nets of the inputs (constants become the module's constant nets). */
static odin3_status cell_inputs(cell_ctx *cx) {
    const odin3_elab2_cell_req *req = cx->req;
    odin3_status st = cell_port_nets(cx, PORT_A, req->a);
    if (st == ODIN3_OK && cx->shape != CELL_UNARY) {
        st = cell_port_nets(cx, PORT_B, req->b);
    }
    if (st == ODIN3_OK && cx->shape == CELL_MUX) {
        st = cell_port_nets(cx, PORT_S, req->s);
    }
    return st;
}

/* Y on fresh unnamed nets (counted against the net-bits cap first), each with the cell's record. */
static odin3_status cell_fresh_nets(cell_ctx *cx) {
    odin3_elab2 *e2 = cx->e2;
    uint32_t width = cx->req->y_width;
    odin3_status st = odin3_elab2_reserve_nets(e2, width, odin3_ast_loc(e2->elab, cx->req->node));
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id *nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *nets);
    if (nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < width; i++) {
        st = odin3_net_create(e2->mod, 0, cx->prov, &nets[i]);
        if (st != ODIN3_OK) {
            while (i-- > 0) {
                (void)odin3_net_delete(e2->mod, nets[i]);
            }
            return st;
        }
    }
    cx->nets[PORT_Y] = nets;
    cx->fresh = true;
    return ODIN3_OK;
}

static void cell_params(cell_ctx *cx) {
    const odin3_elab2_cell_req *req = cx->req;
    odin3_value *param = cx->params;
    if (cx->shape == CELL_MUX) {
        param[0] = odin3_value_int((int64_t)req->y_width);
        cx->n_params = CELL_MUX_PARAMS;
    } else if (cx->shape == CELL_UNARY) {
        param[0] = odin3_value_int(req->a->is_signed ? 1 : 0);
        param[1] = odin3_value_int((int64_t)req->a->width);
        param[2] = odin3_value_int((int64_t)req->y_width);
        cx->n_params = CELL_UNARY_PARAMS;
    } else {
        param[0] = odin3_value_int(req->a->is_signed ? 1 : 0);
        param[1] = odin3_value_int(req->b->is_signed ? 1 : 0);
        param[2] = odin3_value_int((int64_t)req->a->width);
        param[3] = odin3_value_int((int64_t)req->b->width);
        param[4] = odin3_value_int((int64_t)req->y_width);
        cx->n_params = CELL_MAX_PARAMS;
    }
}

/* The node, connected in port order (A, B, S, Y by shape), with its attributes. */
static odin3_status cell_create(cell_ctx *cx, odin3_node_id *node) {
    odin3_elab2 *e2 = cx->e2;
    odin3_netvec ports[CELL_MAX_PORTS];
    uint32_t used = 0;
    static const unsigned k_order[CELL_MAX_PORTS] = {PORT_A, PORT_B, PORT_S, PORT_Y};
    for (unsigned i = 0; i < CELL_MAX_PORTS; i++) {
        if (cx->count[k_order[i]] > 0) {
            ports[used++] = (odin3_netvec){cx->nets[k_order[i]], cx->count[k_order[i]]};
        }
    }
    cell_params(cx);
    odin3_node_spec spec = {.type = e2->types[cx->req->type],
                            .prov = cx->prov,
                            .params = cx->params,
                            .n_params = cx->n_params};
    odin3_status st = odin3_node_create_connected(e2->mod, &spec, ports, node);
    if (st == ODIN3_OK && cx->req->node.v != 0) {
        st = odin3_elab2_attrs(e2, cx->req->node, (odin3_objref){ODIN3_OBJ_NODE, node->v});
        if (st != ODIN3_OK) {
            (void)odin3_node_delete(e2->mod, *node); /* the cell is whole or not there */
        }
    }
    return st;
}

/* Undoes the fresh Y nets of a cell that was not made. */
static void cell_undo(const cell_ctx *cx) {
    if (!cx->fresh) {
        return;
    }
    for (uint32_t i = 0; i < cx->req->y_width; i++) {
        (void)odin3_net_delete(cx->e2->mod, cx->nets[PORT_Y][i]);
    }
}

odin3_status odin3_elab2_cell(odin3_elab2 *e2, const odin3_elab2_cell_req *req, odin3_sig *ysig) {
    if (e2->mod == NULL || (unsigned)req->type >= (unsigned)ODIN3_E2T_COUNT) {
        return cell_bad("no module is being emitted, or an unknown type");
    }
    cell_ctx cx = {.e2 = e2, .req = req};
    odin3_status st = cell_plan(&cx);
    st = st != ODIN3_OK ? st : cell_cap(&cx);
    odin3_elab2_prov_req prov = {req->node, {0}, e2->scope.v};
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &prov, &cx.prov);
    st = st != ODIN3_OK ? st : cell_inputs(&cx);
    if (st == ODIN3_OK && req->dest != NULL) {
        cx.nets[PORT_Y] = req->dest;
    } else if (st == ODIN3_OK) {
        st = cell_fresh_nets(&cx);
    }
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : cell_create(&cx, &node);
    if (st != ODIN3_OK) {
        cell_undo(&cx);
        return st;
    }
    odin3_elab2_counts made = {1, (uint32_t)cx.pins, cx.fresh ? req->y_width : 0, 0};
    st = odin3_elab2_sig_new(e2, req->y_width, ysig);
    for (uint32_t i = 0; st == ODIN3_OK && i < req->y_width; i++) {
        ysig->bits[i] = cx.nets[PORT_Y][i].v;
    }
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* --- continuous assignments (EL-28) ---------------------------------------------------------- */

/* One assignment being made: its lhs nets (LSB first, net 0 for a bit that is dropped), the nets
 * merged away so far and where the lhs is, for the diagnostics. */
typedef struct asg_run {
    odin3_elab2 *e2;
    odin3_vec lhs;     /* odin3_net_id */
    odin3_vec work;    /* odin3_ast_id: the lhs nodes still to take apart */
    odin3_u64map *fwd; /* a net merged away -> the net that replaced it, made on first merge */
    odin3_loc at;
    bool dropped; /* some lhs bit lies outside its net */
} asg_run;

static const char *asg_name(const asg_run *ar, const odin3_symbol *rec) {
    return odin3_strtab_get(odin3_design_strtab(ar->e2->design), rec->name);
}

/* The net or variable the IDENT or HIER_NAME names, with its wire; NULL after the error. */
static const odin3_symbol *asg_symbol(asg_run *ar, odin3_ast_id name, bool word) {
    odin3_elab2 *e2 = ar->e2;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, name));
    odin3_loc loc = odin3_ast_loc(e2->elab, name);
    if (rec == NULL || (rec->kind != ODIN3_SYM_NET && rec->kind != ODIN3_SYM_VAR)) {
        odin3_elab2_error(e2, loc, "'%s' is not a net or variable",
                          rec != NULL ? asg_name(ar, rec) : odin3_ast_name_str(e2->elab, name));
    } else if (rec->n_unpacked > 0) {
        odin3_elab2_error(e2, loc,
                          word ? "continuous assignment to a word of memory '%s' is not supported"
                               : "'%s' is a memory; select a word",
                          asg_name(ar, rec));
    } else if (rec->ir_wire.v == 0 || rec->ir_module.v != e2->mod_id.v) {
        odin3_elab2_error(e2, loc, "'%s' is not a net or variable of this module",
                          asg_name(ar, rec));
    } else {
        return rec;
    }
    return NULL;
}

static odin3_status asg_push(asg_run *ar, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(&ar->lhs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

/* Every bit of the symbol's wire. */
static odin3_status asg_whole(asg_run *ar, const odin3_symbol *rec) {
    uint32_t width = odin3_wire_width(ar->e2->mod, rec->ir_wire);
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        st = asg_push(ar, odin3_wire_net(ar->e2->mod, rec->ir_wire, bit));
    }
    return st;
}

static odin3_status asg_name_target(asg_run *ar, odin3_ast_id name) {
    const odin3_symbol *rec = asg_symbol(ar, name, false);
    return rec == NULL ? ODIN3_ERR_PARSE : asg_whole(ar, rec);
}

/* The bits a constant select of the net covers, from the lsb; the ones outside it are dropped. */
static odin3_status asg_select_bits(asg_run *ar, odin3_ast_id sel, const odin3_symbol *rec,
                                    const odin3_elab2_selobj *obj,
                                    const odin3_elab2_selrange *range) {
    odin3_elab2 *e2 = ar->e2;
    bool outside = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < range->width; j++) {
        int64_t pos = odin3_elab2_sel_pos(obj, odin3_elab2_sel_index(range, j));
        outside |= pos < 0;
        odin3_net_id net = {0};
        if (pos >= 0) {
            net = odin3_wire_net(e2->mod, rec->ir_wire, (uint32_t)pos);
        }
        st = asg_push(ar, net);
    }
    if (outside) {
        ar->dropped = true;
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, sel),
                         "assignment to bits outside '%s' [%d:%d] is ignored", obj->name,
                         (int)obj->msb, (int)obj->lsb);
    }
    return st;
}

/* A bit or part select target: its index must be constant (IEEE 6.1.2; Yosys rejects it too). */
static odin3_status asg_select(asg_run *ar, odin3_ast_id sel) {
    odin3_elab2 *e2 = ar->e2;
    odin3_ast_id object = odin3_ast_child(e2->elab, sel, 0);
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, object);
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, sel),
                          "the target of a continuous assignment must be a net, variable or "
                          "concatenation of them");
        return ODIN3_ERR_PARSE;
    }
    const odin3_symbol *rec = asg_symbol(ar, object, true);
    if (rec == NULL) {
        return ODIN3_ERR_PARSE;
    }
    odin3_elab2_selobj obj;
    odin3_elab2_selrange range;
    odin3_sig index = {NULL, 0, false};
    bool has_index = odin3_ast_sub(e2->elab, sel) != ODIN3_AST_SELECT_PART;
    odin3_elab2_expr_req req = {odin3_ast_child(e2->elab, sel, 1), {0, false, false}, NULL, NULL};
    odin3_elab2_sel_object(e2, object, odin3_wire_width(e2->mod, rec->ir_wire), &obj);
    odin3_status st = has_index ? odin3_elab2_expr(e2, &req, &index) : ODIN3_OK;
    st = st != ODIN3_OK ? st
                        : odin3_elab2_sel_range(e2, sel, &obj, has_index ? &index : NULL, &range);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!range.constant || !range.known) {
        odin3_elab2_error(
            e2, odin3_ast_loc(e2->elab, sel),
            "continuous assignment to '%s' with a non-constant index is not supported", obj.name);
        return ODIN3_ERR_PARSE;
    }
    return asg_select_bits(ar, sel, rec, &obj, &range);
}

static odin3_status asg_push_parts(asg_run *ar, odin3_ast_id concat) {
    odin3_ast_span parts = odin3_ast_children(ar->e2->elab, concat);
    for (uint32_t i = 0; i < parts.n; i++) {
        odin3_ast_id *slot = odin3_vec_push(&ar->work);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = parts.ids[i]; /* the last part pops first: the lsb */
    }
    return ODIN3_OK;
}

/* One lhs node taken apart. */
static odin3_status asg_node(asg_run *ar, odin3_ast_id node) {
    switch (odin3_ast_kind_of(ar->e2->elab, node)) {
    case ODIN3_AST_CONCAT:
        return asg_push_parts(ar, node);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return asg_name_target(ar, node);
    case ODIN3_AST_SELECT:
        return asg_select(ar, node);
    default:
        odin3_elab2_error(ar->e2, odin3_ast_loc(ar->e2->elab, node),
                          "the target of a continuous assignment must be a net, variable or "
                          "concatenation of them");
        return ODIN3_ERR_PARSE;
    }
}

/* The lhs nets of a NET_ASSIGN's lvalue (identifiers, constant selects, concatenations). */
static odin3_status asg_lvalue(asg_run *ar, odin3_ast_id lhs) {
    odin3_ast_id *slot = odin3_vec_push(&ar->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = lhs;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && ar->work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&ar->work, ar->work.len - 1);
        odin3_vec_pop(&ar->work);
        st = asg_node(ar, node);
    }
    return st;
}

/* The lhs and rhs of net_assign: a NET_ASSIGN, or a DECLARATOR with its declaration assignment. */
static odin3_status asg_parts(asg_run *ar, odin3_ast_id node, odin3_ast_id *rhs) {
    const odin3_elab2 *e2 = ar->e2;
    if (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_DECLARATOR) {
        *rhs = odin3_ast_child(e2->elab, node, 0);
        ar->at = odin3_ast_loc(e2->elab, node);
        return asg_name_target(ar, node);
    }
    odin3_ast_id lhs = odin3_ast_child(e2->elab, node, 0);
    *rhs = odin3_ast_child(e2->elab, node, 1);
    ar->at = odin3_ast_loc(e2->elab, lhs);
    return asg_lvalue(ar, lhs);
}

/* Task 19's check, interim: an lhs net that has a driver already is two drivers. */
static odin3_status asg_conflict(const asg_run *ar) {
    odin3_elab2_pend_feature(ar->e2, ar->at, ODIN3_PEND_DRIVERS);
    return ODIN3_ERR_PARSE;
}

static odin3_status asg_check_drivers(const asg_run *ar) {
    for (size_t i = 0; i < ar->lhs.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&ar->lhs, i);
        if (net.v != 0 && odin3_net_driver_count(ar->e2->mod, net) > 0) {
            return asg_conflict(ar);
        }
    }
    return ODIN3_OK;
}

/* A net after the merges so far. */
static odin3_net_id asg_resolve(const asg_run *ar, odin3_net_id net) {
    uint64_t next = 0;
    while (ar->fwd != NULL && odin3_u64map_get(ar->fwd, net.v, &next)) {
        net.v = (uint32_t)next;
    }
    return net;
}

/* lhs takes the value of rhs: the same net is done; else rhs is kept and lhs merged into it. */
static odin3_status asg_bit(asg_run *ar, odin3_net_id lhs, odin3_net_id rhs) {
    lhs = asg_resolve(ar, lhs);
    rhs = asg_resolve(ar, rhs);
    if (lhs.v == rhs.v) {
        return ODIN3_OK;
    }
    if (odin3_net_driver_count(ar->e2->mod, lhs) > 0) {
        return asg_conflict(ar);
    }
    if (ar->fwd == NULL) {
        ar->fwd = odin3_u64map_create(0);
        if (ar->fwd == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_status st = odin3_u64map_put(ar->fwd, (odin3_kv){lhs.v, rhs.v});
    return st != ODIN3_OK ? st : odin3_net_merge(ar->e2->mod, (odin3_net_pair){rhs, lhs});
}

/* Every lhs bit takes its rhs bit (ext has exactly the lhs width). */
static odin3_status asg_connect(asg_run *ar, const odin3_sig *ext) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ar->lhs.len; i++) {
        odin3_net_id lhs = *(const odin3_net_id *)odin3_vec_cat(&ar->lhs, i);
        if (lhs.v == 0) {
            continue;
        }
        odin3_net_id rhs = {0};
        odin3_sig one = {&ext->bits[i], 1, false};
        st = odin3_elab2_to_nets(ar->e2, &one, &rhs);
        st = st != ODIN3_OK ? st : asg_bit(ar, lhs, rhs);
    }
    return st;
}

/* The rhs at the lhs width (context {L, true, false}), by construction when it can. */
static odin3_status asg_rhs(asg_run *ar, odin3_ast_id rhs) {
    uint32_t width = (uint32_t)ar->lhs.len;
    const odin3_net_id *dest = ar->dropped ? NULL : (const odin3_net_id *)ar->lhs.data;
    odin3_elab2_expr_req req = {rhs, {width, true, false}, dest, NULL};
    odin3_sig sig;
    odin3_status st = odin3_elab2_expr(ar->e2, &req, &sig);
    st = st != ODIN3_OK ? st : odin3_elab2_extend(ar->e2, &sig, width, sig.is_signed, &sig);
    return st != ODIN3_OK ? st : asg_connect(ar, &sig);
}

static bool asg_all_dropped(const asg_run *ar) {
    for (size_t i = 0; i < ar->lhs.len; i++) {
        if (((const odin3_net_id *)ar->lhs.data)[i].v != 0) {
            return false;
        }
    }
    return true;
}

odin3_status odin3_elab2_assign(odin3_elab2 *e2, odin3_ast_id net_assign) {
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, net_assign);
    if (e2->mod == NULL || (kind != ODIN3_AST_NET_ASSIGN && kind != ODIN3_AST_DECLARATOR)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_elab2_assign: not a NET_ASSIGN or DECLARATOR, or no module is emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    asg_run ar = {.e2 = e2};
    odin3_vec_init(&ar.lhs, sizeof(odin3_net_id));
    odin3_vec_init(&ar.work, sizeof(odin3_ast_id));
    odin3_ast_id rhs = {0};
    odin3_status st = asg_parts(&ar, net_assign, &rhs);
    st = st != ODIN3_OK ? st : asg_check_drivers(&ar);
    if (st == ODIN3_OK && !asg_all_dropped(&ar)) {
        st = asg_rhs(&ar, rhs);
    }
    odin3_vec_free(&ar.lhs);
    odin3_vec_free(&ar.work);
    odin3_u64map_destroy(ar.fwd);
    return st;
}
