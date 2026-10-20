/*
 * elab2_memport.c — stage 2's memory ports and the `$mem` cell (2D Task 20; EL-27, EL-39 reads):
 * the address of a read or write (the raw index of a one-dimensional array; a multi-dimensional
 * array's row-major address, built node for node like Yosys 0.55's normalize_index and
 * index_offset, simplify.cc 483-540), the asynchronous read port and its fresh data nets, the
 * clocked write port (the process generator's, Task 24), and EL-28 step (8): one `$mem` per memory
 * with its ports in source order. `$mem` has simlib's semantics, which subtract OFFSET themselves,
 * so no port subtracts it; an address narrower than ABITS is zero-extended (Yosys's
 * memory_collect).
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab2_mem.h"
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

enum {
    ADDR_CONST_BITS = 32, /* Yosys's integer constants */
    FOLD_MAX_BITS = 62,   /* wider operands are not folded */
    N_PARAMS = 13,        /* the parameters of $mem */
    N_PORTS = 8,          /* its ports */
    P_MEMID = 0,          /* the parameters, in definition order */
    P_SIZE,
    P_OFFSET,
    P_ABITS,
    P_WIDTH,
    P_RD_PORTS,
    P_WR_PORTS,
    P_RD_CLK_ENABLE,
    P_RD_CLK_POLARITY,
    P_RD_TRANSPARENT,
    P_WR_CLK_ENABLE,
    P_WR_CLK_POLARITY,
    P_INIT,
    Q_RD_CLK = 0, /* the ports, in definition order */
    Q_RD_EN,
    Q_RD_ADDR,
    Q_RD_DATA,
    Q_WR_CLK,
    Q_WR_EN,
    Q_WR_ADDR,
    Q_WR_DATA
};

static odin3_loc node_loc(const odin3_elab2 *e2, odin3_ast_id node) {
    return node.v != 0 ? odin3_ast_loc(e2->elab, node) : (odin3_loc){0};
}

/* --- the cap on the cell's pins ----------------------------------------------------------------
 */

/* The pins of the $mem with the given port counts and address width. */
static uint64_t mem_pins(const odin3_elab_mem *mem, uint32_t abits, uint64_t rd, uint64_t wr) {
    return rd * (2U + abits + mem->width) + wr * (1U + 2U * (uint64_t)mem->width + abits);
}

/* Point (aa): the pins of one cell, checked before the port is added. Fatal. */
typedef struct port_add {
    odin3_ast_id node;
    uint32_t addr_width;
    bool is_read;
} port_add;

static odin3_status mem_cap(odin3_elab2 *e2, const odin3_elab_mem *mem, const port_add *add) {
    uint32_t abits = add->addr_width > mem->abits ? add->addr_width : mem->abits;
    uint64_t rd = mem->rd_ports.len + (add->is_read ? 1U : 0U);
    uint64_t wr = mem->wr_ports.len + (add->is_read ? 0U : 1U);
    uint64_t pins = mem_pins(mem, abits, rd, wr);
    return odin3_elab2_pins_cap(e2, node_loc(e2, add->node), pins);
}

/* --- addresses ---------------------------------------------------------------------------------
 */

/* The expression being built: its node (cells' provenance) and its sign, which is the whole
 * expression's (the AND of its leaves' signs, constants being signed) and so every cell's. */
typedef struct addr_run {
    odin3_elab2 *e2;
    odin3_ast_id node;
    bool is_signed;
} addr_run;

static bool fold_value(const odin3_sig *sig, int64_t *value) {
    return sig->width <= FOLD_MAX_BITS && odin3_elab2_sig_i64(sig, value) == 1;
}

/* A constant of ADDR_CONST_BITS bits. */
static odin3_status addr_const(const addr_run *run, int64_t value, odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(run->e2, ADDR_CONST_BITS, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < ADDR_CONST_BITS; k++) {
        out->bits[k] = odin3_sb_of_bit(((uint64_t)value >> k) & 1U ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    out->is_signed = run->is_signed;
    return st;
}

/* lhs op rhs on constants, at the wider operand's width, modulo 2^width. */
static odin3_status addr_fold(const addr_run *run, odin3_elab2_type type, int64_t lhs, int64_t rhs,
                              odin3_sig *out) {
    uint64_t ua = (uint64_t)lhs;
    uint64_t ub = (uint64_t)rhs;
    uint64_t value = ua * ub;
    if (type == ODIN3_E2T_ADD) {
        value = ua + ub;
    } else if (type == ODIN3_E2T_SUB) {
        value = ua - ub;
    }
    uint32_t width = out->width;
    odin3_status st = odin3_elab2_sig_new(run->e2, width, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        out->bits[k] = odin3_sb_of_bit(((value >> k) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    out->is_signed = run->is_signed;
    return st;
}

/* One $add, $sub or $mul of the address, folded when both operands are constants. */
static odin3_status addr_arith(const addr_run *run, odin3_elab2_type type, const odin3_sig *lhs,
                               const odin3_sig *rhs, odin3_sig *out) {
    odin3_sig first = *lhs;
    odin3_sig second = *rhs;
    first.is_signed = second.is_signed = run->is_signed;
    uint32_t width = first.width > second.width ? first.width : second.width;
    int64_t va = 0;
    int64_t vb = 0;
    if (fold_value(&first, &va) && fold_value(&second, &vb)) {
        out->width = width;
        return addr_fold(run, type, va, vb, out);
    }
    odin3_elab2_cell_req req = {type, run->node, &first, &second, width, NULL, NULL};
    odin3_status st = odin3_elab2_cell(run->e2, &req, out);
    out->is_signed = run->is_signed;
    return st;
}

/* One dimension of a flattened array: its range and the stride of one step in it. */
typedef struct addr_dim {
    const odin3_dim *dim;
    uint64_t stride;
} addr_dim;

/* idx - low (when low is not 0), then (width - 1) - that when the dimension is descending (it is
 * counted from its left bound), then * stride (when above 1): Yosys's index_offset. */
static odin3_status addr_term(const addr_run *run, const addr_dim *ad, const odin3_sig *idx,
                              odin3_sig *out) {
    odin3_sig other;
    *out = *idx;
    int32_t low = odin3_elab2_dim_low(ad->dim);
    odin3_status st = ODIN3_OK;
    if (low != 0) {
        st = addr_const(run, low, &other);
        st = st != ODIN3_OK ? st : addr_arith(run, ODIN3_E2T_SUB, out, &other, out);
    }
    if (st == ODIN3_OK && ad->dim->msb > ad->dim->lsb) {
        st = addr_const(run, (int64_t)odin3_elab2_dim_size(ad->dim) - 1, &other);
        st = st != ODIN3_OK ? st : addr_arith(run, ODIN3_E2T_SUB, &other, out, out);
    }
    if (st == ODIN3_OK && ad->stride > 1) {
        st = addr_const(run, (int64_t)ad->stride, &other);
        st = st != ODIN3_OK ? st : addr_arith(run, ODIN3_E2T_MUL, out, &other, out);
    }
    return st;
}

/* The row-major address of a flattened array's word: the terms of its dimensions added in order. */
static odin3_status addr_flat(addr_run *run, const odin3_elab_mem *mem,
                              const odin3_elab2_mem_rd *rd, odin3_sig *out) {
    odin3_dim_span dims = odin3_symbol_unpacked(run->e2->st, mem->sym);
    uint64_t remaining = mem->size;
    odin3_status st = ODIN3_OK;
    run->is_signed = true;
    for (uint32_t k = 0; k < rd->n_idx; k++) {
        run->is_signed = run->is_signed && rd->idx[k].is_signed;
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < dims.n; k++) {
        remaining /= odin3_elab2_dim_size(&dims.dims[k]);
        addr_dim ad = {&dims.dims[k], remaining};
        odin3_sig term;
        st = addr_term(run, &ad, &rd->idx[k], &term);
        if (st != ODIN3_OK) {
            break;
        }
        if (k == 0) {
            *out = term;
        } else {
            st = addr_arith(run, ODIN3_E2T_ADD, out, &term, out);
        }
    }
    return st;
}

odin3_status odin3_elab2_mem_addr(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, odin3_sig *out) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rd->mem);
    if (mem == NULL || rd->n_idx != mem->n_dims || rd->idx == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_mem_addr: not a memory, or a wrong index count");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!mem->flat) {
        *out = rd->idx[0];
        return ODIN3_OK;
    }
    addr_run run = {e2, rd->select, false};
    return addr_flat(&run, mem, rd, out);
}

/* --- ports -------------------------------------------------------------------------------------
 */

/* A request for a port of a memory (not registers), with one index per dimension. */
static bool port_request_ok(const odin3_elab2 *e2, const odin3_elab2_mem_rd *rd) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rd->mem);
    if (mem == NULL || mem->to_regs || rd->n_idx != mem->n_dims || rd->idx == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_mem_read: not a memory, or a wrong index count");
        return false;
    }
    return true;
}

/* The data nets of a new read port, fresh (counted against the net-bits cap first). */
static odin3_status port_data_nets(odin3_elab2 *e2, const odin3_elab_mem *mem, odin3_ast_id node,
                                   odin3_net_id **out) {
    odin3_status st = odin3_elab2_reserve_nets(e2, mem->width, node_loc(e2, node));
    odin3_prov_id prov = {0};
    odin3_elab2_prov_req req = {node, {0}, e2->scope.v};
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &req, &prov);
    odin3_net_id *nets =
        st == ODIN3_OK ? odin3_arena_alloc(e2->scratch, (size_t)mem->width * sizeof *nets) : NULL;
    if (st == ODIN3_OK && nets == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->width; i++) {
        st = odin3_net_create(e2->mod, 0, prov, &nets[i]);
        if (st != ODIN3_OK) {
            while (i-- > 0) {
                (void)odin3_net_delete(e2->mod, nets[i]);
            }
        }
    }
    *out = nets;
    return st;
}

odin3_status odin3_elab2_mem_read(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, odin3_sig *data) {
    if (e2->mod == NULL || !port_request_ok(e2, rd)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_elab_mem *mem = odin3_elab2_mem_at(e2, rd->mem);
    odin3_sig addr;
    odin3_net_id *nets = NULL;
    odin3_status st = odin3_elab2_mem_addr(e2, rd, &addr);
    port_add add = {rd->select, addr.width, true};
    st = st != ODIN3_OK ? st : mem_cap(e2, mem, &add);
    st = st != ODIN3_OK ? st : port_data_nets(e2, mem, rd->select, &nets);
    odin3_elab_mem_rd_port *port = st == ODIN3_OK ? odin3_vec_push(&mem->rd_ports) : NULL;
    if (st == ODIN3_OK && port == NULL) {
        for (uint32_t i = 0; i < mem->width; i++) {
            (void)odin3_net_delete(e2->mod, nets[i]);
        }
        return ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *port = (odin3_elab_mem_rd_port){addr, nets, rd->select};
    mem->abits = addr.width > mem->abits ? addr.width : mem->abits;
    st = odin3_elab2_sig_new(e2, mem->width, data);
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->width; i++) {
        data->bits[i] = nets[i].v;
    }
    odin3_elab2_counts made = {0, 0, mem->width, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* A copy of sig in the scratch arena. */
static odin3_status sig_copy(odin3_elab2 *e2, const odin3_sig *sig, odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, sig->width, out);
    if (st == ODIN3_OK && sig->width > 0) {
        memcpy(out->bits, sig->bits, (size_t)sig->width * sizeof *sig->bits);
    }
    out->is_signed = sig->is_signed;
    return st;
}

static bool write_request_ok(const odin3_elab2 *e2, const odin3_elab2_mem_wr *wr) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, wr->mem);
    bool ok = mem != NULL && !mem->to_regs && wr->addr != NULL && wr->data != NULL &&
              wr->en != NULL && wr->clk.v != 0 && wr->addr->width > 0;
    ok = ok && wr->data->width == mem->width && wr->en->width == mem->width;
    if (!ok) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_mem_write: not a memory, or a malformed port");
    }
    return ok;
}

odin3_status odin3_elab2_mem_write(odin3_elab2 *e2, const odin3_elab2_mem_wr *wr) {
    if (e2->mod == NULL || !write_request_ok(e2, wr)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_elab_mem *mem = odin3_elab2_mem_at(e2, wr->mem);
    port_add add = {wr->stmt, wr->addr->width, false};
    odin3_elab_mem_wr_port port = {.clk = wr->clk, .posedge = wr->posedge, .stmt = wr->stmt};
    odin3_status st = mem_cap(e2, mem, &add);
    st = st != ODIN3_OK ? st : sig_copy(e2, wr->addr, &port.addr);
    st = st != ODIN3_OK ? st : sig_copy(e2, wr->data, &port.data);
    st = st != ODIN3_OK ? st : sig_copy(e2, wr->en, &port.en);
    odin3_elab_mem_wr_port *slot = st == ODIN3_OK ? odin3_vec_push(&mem->wr_ports) : NULL;
    if (st != ODIN3_OK || slot == NULL) {
        return st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
    }
    *slot = port;
    mem->abits = port.addr.width > mem->abits ? port.addr.width : mem->abits;
    return ODIN3_OK;
}

/* --- the $mem cell (EL-28 step (8)) ------------------------------------------------------------
 */

/* The cell being made: the memory, the parameters and the nets of every port. */
typedef struct emit_ctx {
    odin3_elab2 *e2;
    const odin3_elab_mem *mem;
    odin3_value params[N_PARAMS];
    odin3_netvec ports[N_PORTS];
    odin3_net_id *nets[N_PORTS];
    uint64_t pins;
} emit_ctx;

/* A BITS parameter of count bits, every bit `fill`. */
static odin3_status bits_param(odin3_elab2 *e2, uint32_t count, odin3_bit fill, odin3_value *out) {
    uint8_t *bytes = count > 0 ? odin3_arena_alloc(e2->scratch, count) : NULL;
    if (count > 0 && bytes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < count; k++) {
        bytes[k] = (uint8_t)fill;
    }
    *out = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bytes, .len = count};
    return ODIN3_OK;
}

/* WR_CLK_POLARITY: one bit per write port, 1 for a rising edge. */
static odin3_status polarity_param(const emit_ctx *ec, odin3_value *out) {
    uint32_t count = (uint32_t)ec->mem->wr_ports.len;
    odin3_status st = bits_param(ec->e2, count, ODIN3_BIT_0, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        const odin3_elab_mem_wr_port *port = odin3_vec_cat(&ec->mem->wr_ports, k);
        ((uint8_t *)out->bits)[k] = (uint8_t)(port->posedge ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return st;
}

static odin3_status emit_params(emit_ctx *ec) {
    const odin3_elab_mem *mem = ec->mem;
    odin3_value *par = ec->params;
    uint32_t rd = (uint32_t)mem->rd_ports.len;
    uint32_t wr = (uint32_t)mem->wr_ports.len;
    par[P_MEMID] = (odin3_value){.kind = ODIN3_VAL_STRING, .str = mem->name};
    par[P_SIZE] = odin3_value_int(mem->size);
    par[P_OFFSET] = odin3_value_int(mem->offset);
    par[P_ABITS] = odin3_value_int(mem->abits);
    par[P_WIDTH] = odin3_value_int(mem->width);
    par[P_RD_PORTS] = odin3_value_int(rd);
    par[P_WR_PORTS] = odin3_value_int(wr);
    par[P_INIT] = mem->init;
    odin3_status st = bits_param(ec->e2, rd, ODIN3_BIT_0, &par[P_RD_CLK_ENABLE]);
    st = st != ODIN3_OK ? st : bits_param(ec->e2, rd, ODIN3_BIT_0, &par[P_RD_CLK_POLARITY]);
    st = st != ODIN3_OK ? st : bits_param(ec->e2, rd, ODIN3_BIT_0, &par[P_RD_TRANSPARENT]);
    st = st != ODIN3_OK ? st : bits_param(ec->e2, wr, ODIN3_BIT_1, &par[P_WR_CLK_ENABLE]);
    return st != ODIN3_OK ? st : polarity_param(ec, &par[P_WR_CLK_POLARITY]);
}

/* The net of one signal bit: a constant is the module's constant net, any other net as the merges
 * left it. */
static odin3_status bit_net(odin3_elab2 *e2, odin3_sigbit bit, odin3_net_id *net) {
    if ((bit & ODIN3_SB_CONST) == 0) {
        *net = odin3_elab2_resolve(e2, (odin3_net_id){bit});
        return ODIN3_OK;
    }
    return odin3_elab2_const_net(e2, odin3_sb_is_const(bit) ? odin3_sb_bit(bit) : ODIN3_BIT_X, net);
}

/* sig's nets at nets[0 .. width), the bits past sig's width the constant 0 (zero-extension). */
static odin3_status sig_nets(odin3_elab2 *e2, const odin3_sig *sig, odin3_net_id *nets,
                             uint32_t width) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        st = k < sig->width ? bit_net(e2, sig->bits[k], &nets[k])
                            : odin3_elab2_const_net(e2, ODIN3_BIT_0, &nets[k]);
    }
    return st;
}

/* The count of every port's nets and their arrays. */
static odin3_status emit_alloc(emit_ctx *ec) {
    const odin3_elab_mem *mem = ec->mem;
    uint32_t rd = (uint32_t)mem->rd_ports.len;
    uint32_t wr = (uint32_t)mem->wr_ports.len;
    const uint32_t counts[N_PORTS] = {rd,
                                      rd,
                                      rd * mem->abits,
                                      rd * mem->width,
                                      wr,
                                      wr * mem->width,
                                      wr * mem->abits,
                                      wr * mem->width};
    for (uint32_t port = 0; port < N_PORTS; port++) {
        ec->nets[port] =
            counts[port] > 0
                ? odin3_arena_alloc(ec->e2->scratch, (size_t)counts[port] * sizeof(*ec->nets[port]))
                : NULL;
        if (counts[port] > 0 && ec->nets[port] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        ec->ports[port] = (odin3_netvec){ec->nets[port], counts[port]};
        ec->pins += counts[port];
    }
    return ODIN3_OK;
}

/* The nets of the read ports: clock x, enable 1, the address, the fresh data nets. */
static odin3_status emit_reads(emit_ctx *ec) {
    odin3_elab2 *e2 = ec->e2;
    const odin3_elab_mem *mem = ec->mem;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->rd_ports.len; i++) {
        const odin3_elab_mem_rd_port *port = odin3_vec_cat(&mem->rd_ports, i);
        st = odin3_elab2_const_net(e2, ODIN3_BIT_X, &ec->nets[Q_RD_CLK][i]);
        st = st != ODIN3_OK ? st : odin3_elab2_const_net(e2, ODIN3_BIT_1, &ec->nets[Q_RD_EN][i]);
        st = st != ODIN3_OK ? st
                            : sig_nets(e2, &port->addr,
                                       &ec->nets[Q_RD_ADDR][(size_t)i * mem->abits], mem->abits);
        for (uint32_t bit = 0; st == ODIN3_OK && bit < mem->width; bit++) {
            ec->nets[Q_RD_DATA][(size_t)i * mem->width + bit] =
                odin3_elab2_resolve(e2, port->data[bit]);
        }
    }
    return st;
}

/* The nets of the write ports: the clock, the enables, the address and the data. */
static odin3_status emit_writes(emit_ctx *ec) {
    odin3_elab2 *e2 = ec->e2;
    const odin3_elab_mem *mem = ec->mem;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->wr_ports.len; i++) {
        const odin3_elab_mem_wr_port *port = odin3_vec_cat(&mem->wr_ports, i);
        ec->nets[Q_WR_CLK][i] = odin3_elab2_resolve(e2, port->clk);
        st = sig_nets(e2, &port->en, &ec->nets[Q_WR_EN][(size_t)i * mem->width], mem->width);
        st = st != ODIN3_OK ? st
                            : sig_nets(e2, &port->addr,
                                       &ec->nets[Q_WR_ADDR][(size_t)i * mem->abits], mem->abits);
        st = st != ODIN3_OK ? st
                            : sig_nets(e2, &port->data,
                                       &ec->nets[Q_WR_DATA][(size_t)i * mem->width], mem->width);
    }
    return st;
}

/* The node, connected, with the array's SOURCE record and the declaration's attributes. */
static odin3_status emit_node(emit_ctx *ec) {
    odin3_elab2 *e2 = ec->e2;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, ec->mem->sym);
    odin3_elab2_prov_req req = {rec->declarator, {0}, rec->scope.v};
    odin3_elab2_nl_extra nl = {rec->decl, NULL};
    odin3_node_spec spec = {
        .type = e2->types[ODIN3_E2T_MEM], .params = ec->params, .n_params = N_PARAMS};
    odin3_status st = odin3_elab2_prov_nl(e2, &req, &nl, &spec.prov);
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, ec->ports, &node);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    st = odin3_elab2_attrs(e2, rec->decl, obj);
    st = st != ODIN3_OK ? st : odin3_elab2_attrs(e2, rec->declarator, obj);
    if (st != ODIN3_OK) {
        (void)odin3_node_delete(e2->mod, node); /* the cell is whole or not there */
        return st;
    }
    odin3_elab2_counts made = {1, (uint32_t)ec->pins, 0, 0};
    return odin3_elab2_account(e2, &made);
}

static odin3_status emit_one(odin3_elab2 *e2, const odin3_elab_mem *mem) {
    emit_ctx ec = {.e2 = e2, .mem = mem};
    odin3_status st = emit_params(&ec);
    st = st != ODIN3_OK ? st : emit_alloc(&ec);
    st = st != ODIN3_OK ? st : emit_reads(&ec);
    st = st != ODIN3_OK ? st : emit_writes(&ec);
    return st != ODIN3_OK ? st : emit_node(&ec);
}

odin3_status odin3_elab2_mem_emit(odin3_elab2 *e2) {
    if (e2->mod == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_mem_emit: no module is being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < e2->mems.len && !e2->stopped; i++) {
        const odin3_elab_mem *mem = odin3_vec_cat(&e2->mems, i);
        bool has_ports = mem->rd_ports.len + mem->wr_ports.len > 0 || mem->init.len > 0;
        st = !mem->to_regs && has_ports ? emit_one(e2, mem) : ODIN3_OK;
    }
    return st;
}
