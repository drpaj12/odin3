/*
 * elab2_assign.c — stage 2's signals, word cells and continuous assignments (2D Task 18; EL-28,
 * EL-30, EL-31): signals in the scratch arena and the wiring that extends and truncates them, the
 * one function that makes a word cell with genrtlil's parameter rules and the cell-pins cap of
 * point (aa), the net lvalue (an assignment's, an instance output's or a gate output's) and the
 * assignment of a right side to it (Task 19: claimed first; by construction when a cell drives the
 * lhs nets, else by merge). The expression walk is elab2_expr.c, selects elab2_select.c, the driver
 * tables elab2_drive.c.
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

/* --- lvalues: the nets a continuous assignment, an instance output or a gate output drives ---- */

void odin3_elab2_lvalue_init(odin3_elab2_lvalue *lv) {
    odin3_vec_init(&lv->nets, sizeof(odin3_net_id));
    odin3_vec_init(&lv->bits, sizeof(odin3_elab2_lvbit));
    odin3_vec_init(&lv->work, sizeof(odin3_ast_id));
    lv->dropped = false;
}

void odin3_elab2_lvalue_free(odin3_elab2_lvalue *lv) {
    odin3_vec_free(&lv->nets);
    odin3_vec_free(&lv->bits);
    odin3_vec_free(&lv->work);
}

static const char *lv_name(const odin3_elab2 *e2, const odin3_symbol *rec) {
    return odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
}

/* The net or variable the IDENT or HIER_NAME names, with its wire; NULL after the error. */
static const odin3_symbol *lv_symbol(odin3_elab2 *e2, odin3_ast_id name, bool word) {
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, name));
    odin3_loc loc = odin3_ast_loc(e2->elab, name);
    if (rec == NULL || (rec->kind != ODIN3_SYM_NET && rec->kind != ODIN3_SYM_VAR)) {
        odin3_elab2_error(e2, loc, "'%s' is not a net or variable",
                          rec != NULL ? lv_name(e2, rec) : odin3_ast_name_str(e2->elab, name));
    } else if (rec->n_unpacked > 0) {
        odin3_elab2_error(e2, loc,
                          word ? "continuous assignment to a word of memory '%s' is not supported"
                               : "'%s' is a memory; select a word",
                          lv_name(e2, rec));
    } else if (rec->ir_wire.v == 0 || rec->ir_module.v != e2->mod_id.v) {
        odin3_elab2_error(e2, loc, "'%s' is not a net or variable of this module",
                          lv_name(e2, rec));
    } else {
        return rec;
    }
    return NULL;
}

/* One lvalue bit: its net (0 for a bit outside its net) and where it came from. */
static odin3_status lv_push(odin3_elab2_lvalue *lv, odin3_net_id net, odin3_elab2_lvbit bit) {
    odin3_net_id *slot = odin3_vec_push(&lv->nets);
    odin3_elab2_lvbit *info = slot != NULL ? odin3_vec_push(&lv->bits) : NULL;
    if (info == NULL) {
        if (slot != NULL) {
            odin3_vec_pop(&lv->nets);
        }
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    *info = bit;
    return ODIN3_OK;
}

/* Every bit of a wire. */
static odin3_status lv_whole_wire(const odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_wire_id wire,
                                  odin3_ast_id node) {
    uint32_t width = odin3_wire_width(e2->mod, wire);
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        st = lv_push(lv, odin3_wire_net(e2->mod, wire, bit),
                     (odin3_elab2_lvbit){{wire, bit}, node, true});
    }
    return st;
}

/* Every bit of the symbol's wire. */
static odin3_status lv_whole(const odin3_elab2 *e2, odin3_elab2_lvalue *lv, const odin3_symbol *rec,
                             odin3_ast_id node) {
    return lv_whole_wire(e2, lv, rec->ir_wire, node);
}

static odin3_status lv_name_target(odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_ast_id name) {
    const odin3_symbol *rec = lv_symbol(e2, name, false);
    return rec == NULL ? ODIN3_ERR_PARSE : lv_whole(e2, lv, rec, name);
}

/* The select being taken apart: its node, the wire it selects from (0: a word outside its array,
 * every bit dropped), the object and its range. */
typedef struct lv_sel {
    odin3_ast_id node;
    odin3_wire_id wire;
    const odin3_elab2_selobj *obj;
    const odin3_elab2_selrange *range;
} lv_sel;

/* The bits a constant select of the wire covers, from the lsb; the ones outside it are dropped. */
static odin3_status lv_select_bits(odin3_elab2 *e2, odin3_elab2_lvalue *lv, const lv_sel *ls) {
    odin3_ast_id sel = ls->node;
    const odin3_elab2_selobj *obj = ls->obj;
    const odin3_elab2_selrange *range = ls->range;
    bool outside = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < range->width; j++) {
        int64_t pos = odin3_elab2_sel_pos(obj, odin3_elab2_sel_index(range, j));
        outside |= pos < 0;
        odin3_net_id net = {0};
        odin3_elab2_lvbit bit = {{{0}, 0}, sel, false};
        if (pos >= 0) {
            net = odin3_wire_net(e2->mod, ls->wire, (uint32_t)pos);
            bit.wb = (odin3_wirebit){ls->wire, (uint32_t)pos};
        }
        st = lv_push(lv, net, bit);
    }
    if (outside) {
        lv->dropped = true;
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, sel),
                         "assignment to bits outside '%s' [%d:%d] is ignored", obj->name,
                         (int)obj->msb, (int)obj->lsb);
    }
    return st;
}

/* The bits a select of a word outside its array covers: dropped (the warning was the word's). */
static odin3_status lv_select_absent(odin3_elab2_lvalue *lv, const lv_sel *ls) {
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < ls->range->width; j++) {
        st = lv_push(lv, (odin3_net_id){0}, (odin3_elab2_lvbit){{{0}, 0}, ls->node, false});
    }
    lv->dropped = true;
    return st;
}

/* The range of a bit or part select of obj, which must be constant (IEEE 6.1.2; Yosys rejects it
 * too); the bits go onto the lvalue. */
static odin3_status lv_select_range(odin3_elab2 *e2, odin3_elab2_lvalue *lv, const lv_sel *target) {
    odin3_ast_id sel = target->node;
    odin3_elab2_selrange range;
    odin3_sig index = {NULL, 0, false};
    bool has_index = odin3_ast_sub(e2->elab, sel) != ODIN3_AST_SELECT_PART;
    odin3_elab2_expr_req req = {odin3_ast_child(e2->elab, sel, 1), {0, false, false}, NULL, NULL};
    odin3_status st = has_index ? odin3_elab2_expr(e2, &req, &index) : ODIN3_OK;
    st = st != ODIN3_OK
             ? st
             : odin3_elab2_sel_range(e2, sel, target->obj, has_index ? &index : NULL, &range);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!range.constant || !range.known) {
        odin3_elab2_error(
            e2, odin3_ast_loc(e2->elab, sel),
            "continuous assignment to '%s' with a non-constant index is not supported",
            target->obj->name);
        return ODIN3_ERR_PARSE;
    }
    lv_sel ls = *target;
    ls.range = &range;
    return ls.wire.v == 0 ? lv_select_absent(lv, &ls) : lv_select_bits(e2, lv, &ls);
}

/* --- words of arrays (EL-27) ------------------------------------------------------------------ */

/* The word an lvalue SELECT chain names (word is the SELECT that completes the word), once its
 * indices are generated and constant. An array that stayed a memory cannot be assigned
 * continuously. */
static odin3_status lv_word_of(odin3_elab2 *e2, odin3_ast_id word, const odin3_elab2_wordref *ref,
                               odin3_elab2_word *out) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref->mem);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, mem->sym);
    odin3_loc loc = odin3_ast_loc(e2->elab, word);
    if (!mem->to_regs && mem->failed) {
        return ODIN3_ERR_PARSE; /* reported with the array */
    }
    if (!mem->to_regs) {
        odin3_elab2_error(e2, loc,
                          "continuous assignment to a word of memory '%s' is not supported",
                          lv_name(e2, rec));
        return ODIN3_ERR_PARSE;
    }
    odin3_sig *idx = odin3_arena_alloc(e2->scratch, (size_t)ref->n * sizeof *idx);
    if (idx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < ref->n; k++) {
        odin3_elab2_expr_req req = {
            odin3_elab2_mem_word_index(e2, word, k), {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &req, &idx[k]);
    }
    odin3_elab2_mem_rd rd = {ref->mem, word, idx, ref->n, NULL};
    st = st != ODIN3_OK ? st : odin3_elab2_mem_word(e2, &rd, true, out);
    if (st == ODIN3_OK && out->kind == ODIN3_WORD_VARIABLE) {
        odin3_elab2_error(
            e2, loc, "continuous assignment to '%s' with a non-constant index is not supported",
            lv_name(e2, rec));
        return ODIN3_ERR_PARSE;
    }
    return st;
}

/* A whole word as the target: its wire's bits, or dropped bits for a word outside the array. */
static odin3_status lv_word(odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_ast_id sel,
                            const odin3_elab2_wordref *ref) {
    odin3_elab2_word word;
    odin3_status st = lv_word_of(e2, sel, ref, &word);
    if (st != ODIN3_OK) {
        return st;
    }
    if (word.kind == ODIN3_WORD_WIRE) {
        return lv_whole_wire(e2, lv, word.wire, sel);
    }
    uint32_t width = odin3_elab2_mem_get(e2, ref->mem)->width;
    lv->dropped = true;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        st = lv_push(lv, (odin3_net_id){0}, (odin3_elab2_lvbit){{{0}, 0}, sel, false});
    }
    return st;
}

/* A bit or part select of a word (`w[1][2:1]`): the select's range within the word. */
static odin3_status lv_word_select(odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_ast_id sel,
                                   const odin3_elab2_wordref *ref) {
    odin3_ast_id word_node = odin3_ast_child(e2->elab, sel, 0);
    odin3_elab2_word word;
    odin3_status st = lv_word_of(e2, word_node, ref, &word);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, ref->mem);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, mem->sym);
    odin3_elab2_selobj obj = {rec->has_packed ? rec->packed.msb : 0,
                              rec->has_packed ? rec->packed.lsb : 0, mem->width, lv_name(e2, rec)};
    lv_sel ls = {sel, word.kind == ODIN3_WORD_WIRE ? word.wire : (odin3_wire_id){0}, &obj, NULL};
    return lv_select_range(e2, lv, &ls);
}

/* A bit or part select target, or a word of an array (EL-27). */
static odin3_status lv_select(odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_ast_id sel) {
    odin3_ast_id object = odin3_ast_child(e2->elab, sel, 0);
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, object);
    odin3_elab2_wordref ref;
    if (odin3_elab2_mem_word_of(e2, sel, &ref)) {
        return lv_word(e2, lv, sel, &ref);
    }
    if (kind == ODIN3_AST_SELECT && odin3_elab2_mem_word_of(e2, object, &ref)) {
        return lv_word_select(e2, lv, sel, &ref);
    }
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, sel),
                          "the target of a continuous assignment must be a net, variable or "
                          "concatenation of them");
        return ODIN3_ERR_PARSE;
    }
    const odin3_symbol *rec = lv_symbol(e2, object, true);
    if (rec == NULL) {
        return ODIN3_ERR_PARSE;
    }
    odin3_elab2_selobj obj;
    odin3_elab2_sel_object(e2, object, odin3_wire_width(e2->mod, rec->ir_wire), &obj);
    lv_sel ls = {sel, rec->ir_wire, &obj, NULL};
    return lv_select_range(e2, lv, &ls);
}

static odin3_status lv_push_parts(const odin3_elab2 *e2, odin3_elab2_lvalue *lv,
                                  odin3_ast_id concat) {
    odin3_ast_span parts = odin3_ast_children(e2->elab, concat);
    for (uint32_t i = 0; i < parts.n; i++) {
        odin3_ast_id *slot = odin3_vec_push(&lv->work);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = parts.ids[i]; /* the last part pops first: the lsb */
    }
    return ODIN3_OK;
}

/* One lhs node taken apart. */
static odin3_status lv_node(odin3_elab2 *e2, odin3_elab2_lvalue *lv, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e2->elab, node)) {
    case ODIN3_AST_CONCAT:
        return lv_push_parts(e2, lv, node);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
    case ODIN3_AST_DECLARATOR:
        return lv_name_target(e2, lv, node);
    case ODIN3_AST_SELECT:
        return lv_select(e2, lv, node);
    default:
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node),
                          "the target of a continuous assignment must be a net, variable or "
                          "concatenation of them");
        return ODIN3_ERR_PARSE;
    }
}

odin3_status odin3_elab2_lvalue_make(odin3_elab2 *e2, odin3_ast_id lhs, odin3_elab2_lvalue *lv) {
    odin3_ast_id *slot = odin3_vec_push(&lv->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = lhs;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && lv->work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&lv->work, lv->work.len - 1);
        odin3_vec_pop(&lv->work);
        st = lv_node(e2, lv, node);
    }
    return st;
}

/* --- continuous assignments (EL-28) ---------------------------------------------------------- */

/* One assignment being made: its lvalue, the nodes and what the diagnostics need. */
typedef struct asg_run {
    odin3_elab2 *e2;
    odin3_elab2_lvalue lv;
    odin3_ast_id lhs, rhs;
} asg_run;

/* The lhs and rhs of net_assign: a NET_ASSIGN, or a DECLARATOR with its declaration assignment. */
static odin3_status asg_parts(asg_run *ar, odin3_ast_id node) {
    const odin3_elab2 *e2 = ar->e2;
    if (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_DECLARATOR) {
        ar->rhs = odin3_ast_child(e2->elab, node, 0);
        ar->lhs = node;
    } else {
        ar->lhs = odin3_ast_child(e2->elab, node, 0);
        ar->rhs = odin3_ast_child(e2->elab, node, 1);
    }
    return odin3_elab2_lvalue_make(ar->e2, ar->lhs, &ar->lv);
}

/* A `reg` target is accepted as a net, with Yosys's warning (once per piece of the lvalue). */
static void asg_warn_variables(const asg_run *ar) {
    odin3_ast_id last = {0};
    for (size_t i = 0; i < ar->lv.bits.len; i++) {
        odin3_ast_id piece = ((const odin3_elab2_lvbit *)ar->lv.bits.data)[i].node;
        if (piece.v == last.v) {
            continue;
        }
        last = piece;
        odin3_ast_id name = piece;
        while (odin3_ast_kind_of(ar->e2->elab, name) == ODIN3_AST_SELECT) {
            name = odin3_ast_child(ar->e2->elab, name, 0);
        }
        const odin3_symbol *rec =
            odin3_symbol_get(ar->e2->st, odin3_elab_sym_of(&ar->e2->bind, name));
        if (rec != NULL && rec->kind == ODIN3_SYM_VAR) {
            odin3_elab2_warn(ar->e2, odin3_ast_loc(ar->e2->elab, piece),
                             "'%s' is a variable assigned by a continuous assignment; treated as "
                             "a net",
                             lv_name(ar->e2, rec));
        }
    }
}

static bool asg_all_dropped(const asg_run *ar) {
    for (size_t i = 0; i < ar->lv.nets.len; i++) {
        if (((const odin3_net_id *)ar->lv.nets.data)[i].v != 0) {
            return false;
        }
    }
    return true;
}

/* The rhs at the lhs width (context {L, true, false}), by construction when it can. */
static odin3_status asg_rhs(asg_run *ar) {
    uint32_t width = (uint32_t)ar->lv.nets.len;
    const odin3_net_id *dest = ar->lv.dropped ? NULL : (const odin3_net_id *)ar->lv.nets.data;
    odin3_elab2_expr_req req = {ar->rhs, {width, true, false}, dest, NULL};
    odin3_sig sig;
    odin3_status st = odin3_elab2_expr(ar->e2, &req, &sig);
    st = st != ODIN3_OK ? st : odin3_elab2_extend(ar->e2, &sig, width, sig.is_signed, &sig);
    odin3_elab2_lvconn conn = {&ar->lv, &sig, ODIN3_DRV_ASSIGN, ar->lhs, ar->rhs};
    return st != ODIN3_OK ? st : odin3_elab2_lvalue_connect(ar->e2, &conn);
}

/* Whether branch is the constant z (every bit, at the width of the context). */
static bool asg_is_z(odin3_elab2 *e2, odin3_ast_id branch, uint32_t width) {
    if (odin3_ast_kind_of(e2->elab, branch) != ODIN3_AST_NUMBER) {
        return false;
    }
    odin3_sig sig;
    odin3_elab2_expr_req req = {branch, {width, true, false}, NULL, NULL};
    if (odin3_elab2_expr(e2, &req, &sig) != ODIN3_OK) {
        return false;
    }
    odin3_status st = odin3_elab2_extend(e2, &sig, width, sig.is_signed, &sig);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        if (sig.bits[i] != odin3_sb_of_bit(ODIN3_BIT_Z)) {
            return false;
        }
    }
    return st == ODIN3_OK;
}

/* `c ? d : 'bz` and `c ? 'bz : d` (D3): the $tribuf driving the lvalue; *handled says whether the
 * rhs was one. */
static odin3_status asg_tribuf(asg_run *ar, bool *handled) {
    odin3_elab2 *e2 = ar->e2;
    *handled = false;
    uint32_t width = (uint32_t)ar->lv.nets.len;
    if (odin3_ast_kind_of(e2->elab, ar->rhs) != ODIN3_AST_TERNARY) {
        return ODIN3_OK;
    }
    odin3_ast_id first = odin3_ast_child(e2->elab, ar->rhs, 1);
    odin3_ast_id second = odin3_ast_child(e2->elab, ar->rhs, 2);
    bool first_z = asg_is_z(e2, first, width);
    bool second_z = asg_is_z(e2, second, width);
    if (first_z == second_z) {
        return ODIN3_OK;
    }
    *handled = true;
    odin3_elab2_tri_req tri = {.lv = &ar->lv,
                               .data_node = first_z ? second : first,
                               .cond_node = odin3_ast_child(e2->elab, ar->rhs, 0),
                               .invert = first_z,
                               .node = ar->rhs,
                               .claim = ar->lhs};
    return odin3_elab2_tribuf(e2, &tri);
}

odin3_status odin3_elab2_assign(odin3_elab2 *e2, odin3_ast_id net_assign) {
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, net_assign);
    if (e2->mod == NULL || (kind != ODIN3_AST_NET_ASSIGN && kind != ODIN3_AST_DECLARATOR)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_elab2_assign: not a NET_ASSIGN or DECLARATOR, or no module is emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    asg_run ar = {.e2 = e2};
    odin3_elab2_lvalue_init(&ar.lv);
    odin3_status st = asg_parts(&ar, net_assign);
    bool handled = false;
    if (st == ODIN3_OK) {
        asg_warn_variables(&ar);
        st = asg_tribuf(&ar, &handled);
    }
    bool granted = false;
    odin3_elab2_lvclaim claim = {&ar.lv, ODIN3_DRV_ASSIGN, ar.lhs, false, NULL, NULL};
    st = st != ODIN3_OK || handled ? st : odin3_elab2_lvalue_claim(e2, &claim, &granted);
    if (st == ODIN3_OK && !handled && !granted) {
        st = ODIN3_ERR_PARSE;
    }
    if (st == ODIN3_OK && !handled && !asg_all_dropped(&ar)) {
        st = asg_rhs(&ar);
    }
    odin3_elab2_lvalue_free(&ar.lv);
    return st;
}
