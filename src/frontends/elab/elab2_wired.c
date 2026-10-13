/*
 * elab2_wired.c — wired nets and lvalues (2D Task 19; EL-26): the claim and connection of an
 * lvalue (which elab2_assign.c takes apart), the wired-net kinds, and the chains of step (7) that
 * join the drivers of a `wand`/`wor`/`triand`/`trior` wire.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab2_drive.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- lvalues: claiming and connecting -------------------------------------------------------- */

/* A wired bit's driver goes onto a fresh net, which odin3_elab2_wired chains (EL-26). */
static odin3_status redirect_one(odin3_elab2 *e2, struct odin3_elab2_drive *dr, uint32_t index,
                                 odin3_elab2_lvalue *lv) {
    odin3_net_id *net = odin3_vec_at(&lv->nets, index);
    odin3_elab2_net_info info = odin3_elab2_info_get(dr, *net);
    odin3_loc at =
        odin3_ast_loc(e2->elab, ((odin3_elab2_lvbit *)odin3_vec_at(&lv->bits, index))->node);
    odin3_status st = odin3_elab2_reserve_nets(e2, 1, at);
    odin3_net_id fresh = {0};
    st = st != ODIN3_OK ? st : odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &fresh);
    odin3_elab2_wired_rec *rec = odin3_vec_at(&dr->wired, info.wrec - 1U);
    odin3_elab2_wired_op *op = st == ODIN3_OK ? odin3_vec_push(&rec->ops) : NULL;
    if (st == ODIN3_OK && op == NULL) {
        (void)odin3_net_delete(e2->mod, fresh);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *op = (odin3_elab2_wired_op){info.wbit, fresh, dr->stmts};
    *net = fresh;
    ((odin3_elab2_lvbit *)odin3_vec_at(&lv->bits, index))->wb.wire.v = 0;
    odin3_elab2_counts made = {0, 0, 1, 0};
    return odin3_elab2_account(e2, &made);
}

static odin3_status redirect_wired(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                   odin3_elab2_lvalue *lv) {
    odin3_status st = ODIN3_OK;
    bool any = false;
    for (uint32_t i = 0; st == ODIN3_OK && i < lv->nets.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&lv->nets, i);
        if (net.v != 0 && odin3_elab2_info_get(dr, net).wrec != 0) {
            st = redirect_one(e2, dr, i, lv);
            any = true;
        }
    }
    dr->stmts += any ? 1U : 0U;
    return st;
}

odin3_status odin3_elab2_lvalue_claim(odin3_elab2 *e2, const odin3_elab2_lvclaim *req,
                                      bool *granted) {
    if (req == NULL || req->lv == NULL || granted == NULL || e2->mod == NULL ||
        req->kind == ODIN3_DRV_NONE || req->kind > ODIN3_DRV_TRIBUF) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_lvalue_claim: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_elab2_claim_lvalue(e2, dr, req, granted);
    return st == ODIN3_OK && *granted ? redirect_wired(e2, dr, req->lv) : st;
}

/* Notes `lhs = rhs` where both are the net already: reported at the end if nothing drives it. */
static odin3_status note_loop(struct odin3_elab2_drive *dr, odin3_net_id net,
                              const odin3_elab2_lvconn *req, odin3_ast_id lhs) {
    odin3_elab2_loop_note *note = odin3_vec_push(&dr->loops);
    if (note == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *note = (odin3_elab2_loop_note){net, lhs, req->rhs};
    return ODIN3_OK;
}

/* One lvalue bit takes its net: by construction, as a self-assignment, or by merging. */
static odin3_status connect_bit(odin3_elab2 *e2, const odin3_elab2_lvconn *req, uint32_t index,
                                odin3_net_id rhs) {
    struct odin3_elab2_drive *dr = e2->drive;
    const odin3_elab2_lvbit *bit = odin3_vec_cat(&req->lv->bits, index);
    odin3_net_id lhs =
        odin3_elab2_resolve(e2, *(const odin3_net_id *)odin3_vec_cat(&req->lv->nets, index));
    rhs = odin3_elab2_resolve(e2, rhs);
    odin3_drv mine = {req->kind, bit->node};
    odin3_status st = ODIN3_OK;
    if (lhs.v == rhs.v && odin3_net_driver_count(e2->mod, lhs) == 0) {
        st = note_loop(dr, lhs, req, bit->node);
    } else if (lhs.v == rhs.v) {
        st = odin3_elab2_drv_set(dr, lhs, mine);
    } else {
        st = odin3_elab2_merge(e2, (odin3_net_pair){rhs, lhs});
    }
    return st != ODIN3_OK ? st : odin3_elab2_claim_put(dr, bit->wb, mine);
}

odin3_status odin3_elab2_lvalue_connect(odin3_elab2 *e2, const odin3_elab2_lvconn *req) {
    if (req == NULL || req->lv == NULL || req->sig == NULL || e2->mod == NULL ||
        req->sig->width != req->lv->nets.len || odin3_elab2_drive_tables(e2) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_lvalue_connect: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < req->sig->width; i++) {
        if (((const odin3_net_id *)req->lv->nets.data)[i].v == 0) {
            continue;
        }
        odin3_sig one = {&req->sig->bits[i], 1, false};
        odin3_net_id rhs = {0};
        st = odin3_elab2_to_nets(e2, &one, &rhs);
        st = st != ODIN3_OK ? st : connect_bit(e2, req, i, rhs);
    }
    return st;
}

static const odin3_elab2_wired_kind k_wired_kinds[] = {
    {ODIN3_AST_NET_DECL_WAND, false, "wand"},
    {ODIN3_AST_NET_DECL_WOR, true, "wor"},
    {ODIN3_AST_NET_DECL_TRIAND, false, "triand"},
    {ODIN3_AST_NET_DECL_TRIOR, true, "trior"},
};

const odin3_elab2_wired_kind *odin3_elab2_wired_kind_of(uint8_t sub) {
    for (size_t i = 0; i < sizeof k_wired_kinds / sizeof k_wired_kinds[0]; i++) {
        if (k_wired_kinds[i].sub == sub) {
            return &k_wired_kinds[i];
        }
    }
    return NULL;
}

bool odin3_elab2_net_is_wired(const odin3_elab2 *e2, odin3_net_id net) {
    return e2->drive != NULL && net.v != 0 && odin3_elab2_info_get(e2->drive, net).wrec != 0;
}

/* --- step (7): wired nets -------------------------------------------------------------------- */

/* One two-input cell of the chain, acc op operand (both as wide as the cell): to dest when given,
 * else on fresh nets; acc becomes its Y signal. */
static odin3_status chain_cell(odin3_elab2 *e2, const odin3_elab2_wired_rec *rec, odin3_sig *acc,
                               const odin3_sig *operand, const odin3_net_id *dest) {
    odin3_elab2_cell_req req = {.type = rec->is_or ? ODIN3_E2T_OR : ODIN3_E2T_AND,
                                .node = rec->declarator,
                                .a = acc,
                                .b = operand,
                                .y_width = acc->width,
                                .dest = dest};
    return odin3_elab2_cell(e2, &req, acc);
}

/* The ops are whole drivers: runs of exactly `width` ops, one statement each, bits in order. */
static bool all_whole(const odin3_elab2_wired_rec *rec, uint32_t width) {
    const odin3_elab2_wired_op *ops = rec->ops.data;
    size_t i = 0;
    while (i < rec->ops.len) {
        for (uint32_t k = 0; k < width; k++) {
            if (i + k >= rec->ops.len || ops[i + k].stmt != ops[i].stmt || ops[i + k].bit != k) {
                return false;
            }
        }
        if (i + width < rec->ops.len && ops[i + width].stmt == ops[i].stmt) {
            return false;
        }
        i += width;
    }
    return true;
}

/* Merges the single driver of a wired bit into the wire's net. */
static odin3_status join_single(odin3_elab2 *e2, odin3_net_id wire_net, odin3_net_id driver) {
    wire_net = odin3_elab2_resolve(e2, wire_net);
    driver = odin3_elab2_resolve(e2, driver);
    return wire_net.v == driver.v ? ODIN3_OK
                                  : odin3_elab2_merge(e2, (odin3_net_pair){wire_net, driver});
}

/* The nets of ops[first .. first + width) as a signal. */
static odin3_status ops_sig(odin3_elab2 *e2, const odin3_elab2_wired_op *ops, uint32_t width,
                            odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        out->bits[i] = odin3_elab2_resolve(e2, ops[i].net).v;
    }
    return st;
}

/* The wire's own nets, LSB first, in the scratch arena; NULL on out of memory. */
static odin3_net_id *wire_nets(odin3_elab2 *e2, odin3_wire_id wire, uint32_t width) {
    odin3_net_id *nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *nets);
    for (uint32_t i = 0; nets != NULL && i < width; i++) {
        nets[i] = odin3_wire_net(e2->mod, wire, i);
    }
    return nets;
}

/* k whole drivers: one driver is merged into the wire, else k - 1 word cells of its width. */
static odin3_status chain_words(odin3_elab2 *e2, const odin3_elab2_wired_rec *rec, uint32_t width) {
    const odin3_elab2_wired_op *ops = rec->ops.data;
    uint32_t drivers = (uint32_t)(rec->ops.len / width);
    const odin3_net_id *own = wire_nets(e2, rec->wire, width);
    if (own == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    if (drivers == 1) {
        for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
            st = join_single(e2, own[k], ops[k].net);
        }
        return st;
    }
    odin3_sig acc;
    odin3_sig operand;
    st = ops_sig(e2, ops, width, &acc);
    for (uint32_t j = 1; st == ODIN3_OK && j < drivers; j++) {
        st = ops_sig(e2, ops + (size_t)j * width, width, &operand);
        st = st != ODIN3_OK ? st
                            : chain_cell(e2, rec, &acc, &operand, j == drivers - 1U ? own : NULL);
    }
    return st;
}

/* Bit k of a wire: its ops in order, a one-bit chain; counts[k] of them in all. */
typedef struct bit_chain {
    uint32_t *total, *seen;
    odin3_net_id *run; /* the accumulated net of each bit */
    odin3_net_id *own;
} bit_chain;

static odin3_status chain_step(odin3_elab2 *e2, const odin3_elab2_wired_rec *rec,
                               const bit_chain *bc, const odin3_elab2_wired_op *op) {
    uint32_t k = op->bit;
    odin3_net_id net = odin3_elab2_resolve(e2, op->net);
    if (bc->seen[k]++ == 0) {
        bc->run[k] = net;
        return ODIN3_OK;
    }
    odin3_sigbit a_bit = odin3_elab2_resolve(e2, bc->run[k]).v;
    odin3_sigbit b_bit = net.v;
    odin3_sig acc = {&a_bit, 1, false};
    odin3_sig operand = {&b_bit, 1, false};
    bool last = bc->seen[k] == bc->total[k];
    odin3_status st = chain_cell(e2, rec, &acc, &operand, last ? &bc->own[k] : NULL);
    bc->run[k].v = st == ODIN3_OK ? acc.bits[0] : 0U;
    return st;
}

/* Partial drivers: a chain per bit over the drivers that cover it. */
static odin3_status chain_bits(odin3_elab2 *e2, const odin3_elab2_wired_rec *rec, uint32_t width) {
    const odin3_elab2_wired_op *ops = rec->ops.data;
    bit_chain bc = {odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(uint32_t)),
                    odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(uint32_t)),
                    odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(odin3_net_id)),
                    wire_nets(e2, rec->wire, width)};
    if (bc.total == NULL || bc.seen == NULL || bc.run == NULL || bc.own == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(bc.total, 0, (size_t)width * sizeof(uint32_t));
    memset(bc.seen, 0, (size_t)width * sizeof(uint32_t));
    memset(bc.run, 0, (size_t)width * sizeof(odin3_net_id));
    for (size_t i = 0; i < rec->ops.len; i++) {
        bc.total[ops[i].bit]++;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rec->ops.len; i++) {
        st = chain_step(e2, rec, &bc, &ops[i]);
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        st = bc.total[k] == 1 ? join_single(e2, bc.own[k], bc.run[k]) : ODIN3_OK;
    }
    return st;
}

odin3_status odin3_elab2_wired(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_wired: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    size_t count = e2->drive != NULL ? e2->drive->wired.len : 0;
    for (size_t i = 0; st == ODIN3_OK && i < count && !e2->stopped; i++) {
        const odin3_elab2_wired_rec *rec = odin3_vec_cat(&e2->drive->wired, i);
        uint32_t width = odin3_wire_width(e2->mod, rec->wire);
        e2->scope = (odin3_scope_id){rec->scope};
        if (rec->ops.len == 0) {
            continue;
        }
        st = all_whole(rec, width) ? chain_words(e2, rec, width) : chain_bits(e2, rec, width);
    }
    e2->scope = (odin3_scope_id){0};
    return st != ODIN3_OK ? st : e2->fatal;
}
