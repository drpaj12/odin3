/*
 * mem_sim.c — the simulate hooks of a `$mem` (mem_sim.h; design note
 * docs/specs/2026-11-11-sim-mem-hook-design.md). Semantics follow src/passes/lower/mem.c: the
 * 4-state rules are those of the `$_MUX_` trees and chains `lower` builds, so both paths agree.
 * One implementation serves both modes: in 2-state mode no value is unknown, and an x constant
 * reads 0.
 */
#include "sim/mem_sim.h"

#include "sim/cell.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { CODE_0 = 0, CODE_1 = 1, CODE_X = 2, MAX_ABITS = 64, MAX_CAP_SHIFT = 62, MEM_NODE_PORTS = 8 };

/* E (edge cell) port numbers. */
enum {
    E_CLK,
    E_WR_EN,
    E_WR_ADDR,
    E_WR_DATA,
    E_RD_EN,
    E_RD_ADDR,
    E_RD_DATA,
    E_WR_CLK,
    E_RD_CLK,
    E_N_PORTS
};

/* A (async read cell) port numbers. */
enum { A_RD_ADDR, A_RD_DATA, A_N_PORTS };

#define MEM_PORT(pname, pdir)                                                                      \
    { pname, pdir, false, 0, NULL, NULL, NULL, false }

static const odin3_port_def k_edge_ports[E_N_PORTS] = {
    MEM_PORT("CLK", ODIN3_DIR_IN),      MEM_PORT("WR_EN", ODIN3_DIR_IN),
    MEM_PORT("WR_ADDR", ODIN3_DIR_IN),  MEM_PORT("WR_DATA", ODIN3_DIR_IN),
    MEM_PORT("RD_EN", ODIN3_DIR_IN),    MEM_PORT("RD_ADDR", ODIN3_DIR_IN),
    MEM_PORT("RD_DATA", ODIN3_DIR_OUT), MEM_PORT("WR_CLK", ODIN3_DIR_IN),
    MEM_PORT("RD_CLK", ODIN3_DIR_IN),
};

static const odin3_port_def k_async_ports[A_N_PORTS] = {
    MEM_PORT("RD_ADDR", ODIN3_DIR_IN),
    MEM_PORT("RD_DATA", ODIN3_DIR_OUT),
};

/* --- geometry --------------------------------------------------------------------------------- */

static uint64_t sat_mul(uint64_t lhs, uint64_t rhs) {
    return rhs != 0 && lhs > UINT64_MAX / rhs ? UINT64_MAX : lhs * rhs;
}

static bool flag_set(const odin3_value *val, uint32_t idx) {
    return val->kind == ODIN3_VAL_BITS && idx < val->len && val->bits[idx] == ODIN3_BIT_1;
}

static uint64_t param_u(const odin3_value *params, uint32_t idx) {
    return params[idx].i > 0 ? (uint64_t)params[idx].i : 0U;
}

void odin3_sim_mem_geometry(const odin3_value *params, odin3_sim_mem_geo *geo) {
    memset(geo, 0, sizeof *geo);
    geo->size = param_u(params, ODIN3_SIM_MEM_P_SIZE);
    geo->abits = (uint32_t)param_u(params, ODIN3_SIM_MEM_P_ABITS);
    geo->width = (uint32_t)param_u(params, ODIN3_SIM_MEM_P_WIDTH);
    geo->n_rd = (uint32_t)param_u(params, ODIN3_SIM_MEM_P_RD_PORTS);
    geo->n_wr = (uint32_t)param_u(params, ODIN3_SIM_MEM_P_WR_PORTS);
    geo->offset = params[ODIN3_SIM_MEM_P_OFFSET].i;
    geo->mask = geo->abits >= MAX_ABITS ? UINT64_MAX : ((uint64_t)1 << geo->abits) - 1U;
    bool narrow = geo->abits < MAX_ABITS && geo->mask + 1U < geo->size;
    geo->words = narrow ? geo->mask + 1U : geo->size;
    for (uint32_t port = 0; port < geo->n_rd; port++) {
        geo->n_sync += flag_set(&params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], port) ? 1U : 0U;
    }
    uint64_t staged = sat_mul(geo->n_wr, odin3_sim_mem_stage_stride(geo));
    uint64_t array = sat_mul(geo->words, geo->width);
    geo->shared_size = staged > UINT64_MAX - array ? UINT64_MAX : array + staged;
}

uint64_t odin3_sim_mem_stage_stride(const odin3_sim_mem_geo *geo) {
    return 1U + 2U * (uint64_t)geo->width + geo->abits;
}

bool odin3_sim_is_mem_type(const odin3_celltype_def *def) {
    return def->gran == ODIN3_GRAN_WORD && def->n_ports == MEM_NODE_PORTS &&
           strcmp(def->name, "$mem") == 0;
}

/* --- the cell as the hooks use it ------------------------------------------------------------- */

typedef struct mctx {
    const odin3_sim_cell *cell;
    odin3_sim_mem_geo geo;
    uint8_t *regs;  /* the registers of this cell's synchronous read ports */
    uint8_t *arr;   /* the words (the shared block) */
    uint8_t *stage; /* the staged writes, one record per write port (the shared block) */
    uint8_t xval;   /* what an x constant reads: x (4-state) or 0 */
} mctx;

static void load_ctx(mctx *mc, const odin3_sim_cell *cell, bool four) {
    mc->cell = cell;
    odin3_sim_mem_geometry(cell->params, &mc->geo);
    mc->regs = cell->state;
    mc->arr = cell->aux;
    mc->stage = cell->aux + (size_t)mc->geo.words * mc->geo.width;
    mc->xval = four ? (uint8_t)CODE_X : (uint8_t)CODE_0;
}

/* True when port `port` of the per-port clock span `span` (E_WR_CLK, E_RD_CLK) is on this cell's
 * clock: the builder gives such a pin this cell's clock slot and every other port the zero slot. */
static bool on_this_clock(const odin3_sim_cell *cell, uint32_t span, uint32_t port) {
    const odin3_sim_span *clocks = &cell->ports[span];
    return port < clocks->width && clocks->idx[port] == cell->ports[E_CLK].idx[0];
}

/* --- 4-state gate functions on codes (sim4.c) --------------------------------------------------
 */

static bool unknown(uint8_t code) {
    return (code & 2U) != 0;
}

static uint8_t and4(uint8_t lhs, uint8_t rhs) {
    if ((!unknown(lhs) && lhs == 0) || (!unknown(rhs) && rhs == 0)) {
        return CODE_0;
    }
    return unknown(lhs) || unknown(rhs) ? (uint8_t)CODE_X : (uint8_t)CODE_1;
}

/* a when equal and known in both, else x (the unknown-select merge of `$_MUX_`). */
static uint8_t merge4(uint8_t lhs, uint8_t rhs) {
    return lhs == rhs && !unknown(lhs) ? lhs : (uint8_t)CODE_X;
}

static uint8_t xnor4(uint8_t lhs, uint8_t rhs) {
    return unknown(lhs) || unknown(rhs) ? (uint8_t)CODE_X : (uint8_t)(lhs == rhs);
}

/* --- addresses -------------------------------------------------------------------------------- */

/* An address as read from the value array: the bits (unknown ones 0) and a mask of the known. */
typedef struct maddr {
    uint64_t val;
    uint64_t known;
} maddr;

static maddr load_addr(const uint8_t *values, const uint32_t *idx, uint32_t abits) {
    maddr addr = {0, 0};
    for (uint32_t k = 0; k < abits; k++) {
        uint8_t code = values[idx[k]];
        addr.known |= unknown(code) ? 0U : (uint64_t)1 << k;
        addr.val |= code == CODE_1 ? (uint64_t)1 << k : 0U;
    }
    return addr;
}

static maddr load_addr_codes(const uint8_t *codes, uint32_t abits) {
    maddr addr = {0, 0};
    for (uint32_t k = 0; k < abits; k++) {
        addr.known |= unknown(codes[k]) ? 0U : (uint64_t)1 << k;
        addr.val |= codes[k] == CODE_1 ? (uint64_t)1 << k : 0U;
    }
    return addr;
}

/* The word index of a known address (the caller checks it is below geo->words). */
static uint64_t word_of(const odin3_sim_mem_geo *geo, uint64_t addr) {
    return (addr - (uint64_t)geo->offset) & geo->mask;
}

/* The raw address of word k. */
static uint64_t key_of(const odin3_sim_mem_geo *geo, uint64_t word) {
    return (word + (uint64_t)geo->offset) & geo->mask;
}

/* True when word k can be the target of addr: every known address bit equals the word's key. */
static bool may_match(const odin3_sim_mem_geo *geo, const maddr *addr, uint64_t word) {
    return ((key_of(geo, word) ^ addr->val) & addr->known) == 0;
}

/* --- reads ------------------------------------------------------------------------------------ */

/*
 * An address with x/z bits reads the merge of every word the known bits allow, and x when an
 * address they allow has no word: x unless all are the same known value (nested `$_MUX_` merges).
 */
static void read_unknown(const mctx *mc, const maddr *addr, uint8_t *dst) {
    const odin3_sim_mem_geo *geo = &mc->geo;
    uint64_t found = 0;
    for (uint64_t word = 0; word < geo->words; word++) {
        if (!may_match(geo, addr, word)) {
            continue;
        }
        const uint8_t *src = mc->arr + word * geo->width;
        for (uint32_t bit = 0; bit < geo->width; bit++) {
            dst[bit] = found == 0 || dst[bit] == src[bit] ? src[bit] : (uint8_t)CODE_X;
        }
        found++;
    }
    uint32_t free_bits = geo->abits - (uint32_t)__builtin_popcountll(addr->known & geo->mask);
    bool missing = free_bits > MAX_CAP_SHIFT || found < ((uint64_t)1 << free_bits);
    for (uint32_t bit = 0; bit < geo->width; bit++) {
        dst[bit] = missing || unknown(dst[bit]) ? (uint8_t)CODE_X : dst[bit];
    }
}

/* The word at the address on the pins aidx, into dst (WIDTH bytes). */
/* BASE + OFF, or NULL when BASE is NULL (a zero-width port such as ABITS = 0 has no pin array;
 * NULL + 0 is undefined behaviour). */
static const uint32_t *pins_at(const uint32_t *base, size_t off) {
    return base == NULL ? NULL : base + off;
}

static void read_word(const mctx *mc, const uint32_t *aidx, uint8_t *dst) {
    const odin3_sim_mem_geo *geo = &mc->geo;
    maddr addr = load_addr(mc->cell->values, aidx, geo->abits);
    if (addr.known != geo->mask) {
        read_unknown(mc, &addr, dst);
        return;
    }
    uint64_t word = word_of(geo, addr.val);
    if (word < geo->words) {
        memcpy(dst, mc->arr + word * geo->width, geo->width);
    } else {
        memset(dst, mc->xval, geo->width);
    }
}

/* --- writes ----------------------------------------------------------------------------------- */

/* The pins of one write port. */
typedef struct wpins {
    const uint32_t *en;
    const uint32_t *data;
    const uint32_t *addr;
} wpins;

static wpins write_pins(const mctx *mc, uint32_t port) {
    const odin3_sim_span *ports = mc->cell->ports;
    return (wpins){pins_at(ports[E_WR_EN].idx, (size_t)port * mc->geo.width),
                   pins_at(ports[E_WR_DATA].idx, (size_t)port * mc->geo.width),
                   pins_at(ports[E_WR_ADDR].idx, (size_t)port * mc->geo.abits)};
}

/* One word takes the data under select = AND(dec, en[bit]), dec 1 (exact) or x. */
static void write_word(const mctx *mc, const uint8_t *en, const uint8_t *data, uint8_t *word,
                       uint8_t dec) {
    for (uint32_t bit = 0; bit < mc->geo.width; bit++) {
        uint8_t sel = and4(dec, en[bit]);
        if (sel == CODE_1) {
            word[bit] = data[bit];
        } else if (sel != CODE_0) {
            word[bit] = merge4(word[bit], data[bit]);
        }
    }
}

static bool any_enable(const mctx *mc, const wpins *pins) {
    for (uint32_t bit = 0; bit < mc->geo.width; bit++) {
        if (mc->cell->values[pins->en[bit]] != CODE_0) {
            return true;
        }
    }
    return false;
}

/* The record of write port `port` in the shared block: pending flag, WR_EN, WR_DATA, WR_ADDR. */
static uint8_t *stage_rec(const mctx *mc, uint32_t port) {
    return mc->stage + (size_t)port * (size_t)odin3_sim_mem_stage_stride(&mc->geo);
}

/* Write port `port` at this edge: its inputs are copied to the record (a port that cannot write
 * leaves none), so a write is applied after every read the same step makes, whatever the order
 * of the clocks' cells. */
static void stage_write(const mctx *mc, uint32_t port) {
    wpins pins = write_pins(mc, port);
    if (!any_enable(mc, &pins)) {
        return;
    }
    const uint8_t *values = mc->cell->values;
    uint8_t *rec = stage_rec(mc, port);
    uint8_t *en = rec + 1;
    uint8_t *data = en + mc->geo.width;
    uint8_t *addr = data + mc->geo.width;
    rec[0] = 1;
    for (uint32_t bit = 0; bit < mc->geo.width; bit++) {
        en[bit] = values[pins.en[bit]];
        data[bit] = values[pins.data[bit]];
    }
    for (uint32_t k = 0; k < mc->geo.abits; k++) {
        addr[k] = values[pins.addr[k]];
    }
}

/* Applies the record of write port `port` to the array. */
static void apply_write(const mctx *mc, const uint8_t *rec) {
    const odin3_sim_mem_geo *geo = &mc->geo;
    const uint8_t *en = rec + 1;
    const uint8_t *data = en + geo->width;
    maddr addr = load_addr_codes(data + geo->width, geo->abits);
    if (addr.known == geo->mask) {
        uint64_t word = word_of(geo, addr.val);
        if (word < geo->words) {
            write_word(mc, en, data, mc->arr + word * geo->width, CODE_1);
        }
        return;
    }
    for (uint64_t word = 0; word < geo->words; word++) {
        if (may_match(geo, &addr, word)) {
            write_word(mc, en, data, mc->arr + word * geo->width, CODE_X);
        }
    }
}

/* Applies every pending write in port-index order (the later port wins), then clears them. */
static void commit_writes(const mctx *mc) {
    for (uint32_t port = 0; port < mc->geo.n_wr; port++) {
        uint8_t *rec = stage_rec(mc, port);
        if (rec[0] != 0) {
            apply_write(mc, rec);
            rec[0] = 0;
        }
    }
}

/* --- synchronous reads ------------------------------------------------------------------------ */

/* (read address == write address) of read port rport and write port wport, 4-state. */
static uint8_t addr_equal(const mctx *mc, uint32_t rport, uint32_t wport) {
    const uint8_t *values = mc->cell->values;
    const uint32_t *raddr = pins_at(mc->cell->ports[E_RD_ADDR].idx, (size_t)rport * mc->geo.abits);
    wpins pins = write_pins(mc, wport);
    uint8_t same = CODE_1;
    for (uint32_t k = 0; k < mc->geo.abits; k++) {
        same = and4(same, xnor4(values[raddr[k]], values[pins.addr[k]]));
    }
    return same;
}

/* Transparent read: the data a write port writes to the read address in this cycle replaces the
 * bits of the freshly read word (ports in order, the later one wins). */
static void bypass(const mctx *mc, uint32_t rport, uint8_t *word) {
    const uint8_t *values = mc->cell->values;
    for (uint32_t wport = 0; wport < mc->geo.n_wr; wport++) {
        wpins pins = write_pins(mc, wport);
        if (!on_this_clock(mc->cell, E_WR_CLK, wport) || !any_enable(mc, &pins)) {
            continue; /* lower: a read sees the writes of its own clock net and edge only */
        }
        uint8_t same = addr_equal(mc, rport, wport);
        for (uint32_t bit = 0; bit < mc->geo.width; bit++) {
            uint8_t cond = and4(values[pins.en[bit]], same);
            uint8_t data = values[pins.data[bit]];
            if (cond == CODE_1) {
                word[bit] = data;
            } else if (cond != CODE_0) {
                word[bit] = merge4(word[bit], data);
            }
        }
    }
}

/* The register of sync port `slot` (read port rport) takes the word as it was before the edge. */
/* Read port rport, the slot-th synchronous one. */
typedef struct sync_port {
    uint32_t rport;
    uint32_t slot;
} sync_port;

static void sync_read(const mctx *mc, sync_port port) {
    uint32_t rport = port.rport;
    uint32_t slot = port.slot;
    const odin3_sim_cell *cell = mc->cell;
    const odin3_sim_mem_geo *geo = &mc->geo;
    uint8_t enable = cell->values[cell->ports[E_RD_EN].idx[rport]];
    if (enable == CODE_0) {
        return;
    }
    uint8_t *word = cell->scratch;
    uint8_t *reg = mc->regs + (size_t)slot * geo->width;
    read_word(mc, pins_at(cell->ports[E_RD_ADDR].idx, (size_t)rport * geo->abits), word);
    if (flag_set(&cell->params[ODIN3_SIM_MEM_P_RD_TRANSPARENT], rport)) {
        bypass(mc, rport, word);
    }
    for (uint32_t bit = 0; bit < geo->width; bit++) {
        reg[bit] = enable == CODE_1 ? word[bit] : merge4(reg[bit], word[bit]);
    }
}

static void edge_update(const mctx *mc) {
    const odin3_sim_cell *cell = mc->cell;
    uint32_t slot = 0;
    for (uint32_t rport = 0; rport < mc->geo.n_rd; rport++) {
        if (on_this_clock(cell, E_RD_CLK, rport)) {
            sync_read(mc, (sync_port){rport, slot++});
        }
    }
    for (uint32_t wport = 0; wport < mc->geo.n_wr; wport++) {
        if (on_this_clock(cell, E_WR_CLK, wport)) {
            stage_write(mc, wport);
        }
    }
    if ((cell->flags & ODIN3_SIM_CELL_MEM_DEFER) == 0) {
        commit_writes(mc);
    }
}

/* --- INIT ------------------------------------------------------------------------------------- */

static uint8_t init_code(const odin3_value *init, uint64_t at, bool four) {
    uint8_t bit = init->kind == ODIN3_VAL_BITS && at < init->len ? init->bits[at] : ODIN3_BIT_X;
    if (four) {
        return bit == ODIN3_BIT_0 || bit == ODIN3_BIT_1 ? bit : (uint8_t)CODE_X;
    }
    return bit == ODIN3_BIT_1 ? (uint8_t)CODE_1 : (uint8_t)CODE_0;
}

/* The owner of the shared block sets the array from INIT and clears the staged writes. */
static void init_array(const mctx *mc, bool four) {
    const odin3_value *init = &mc->cell->params[ODIN3_SIM_MEM_P_INIT];
    uint64_t total = mc->geo.words * mc->geo.width;
    for (uint64_t at = 0; at < total; at++) {
        mc->arr[at] = init_code(init, at, four);
    }
    memset(mc->arr + total, 0, (size_t)mc->geo.n_wr * (size_t)odin3_sim_mem_stage_stride(&mc->geo));
}

/* --- the hooks -------------------------------------------------------------------------------- */

static void edge_hook(const odin3_sim_cell *cell, bool four) {
    mctx mc;
    load_ctx(&mc, cell, four);
    if (cell->event == ODIN3_SIM_INIT) {
        if (cell->n_state > 0) {
            memset(mc.regs, mc.xval, cell->n_state);
        }
        if ((cell->flags & ODIN3_SIM_CELL_MEM_INIT) != 0) {
            init_array(&mc, four);
        }
    } else if (cell->event == ODIN3_SIM_COMB) {
        commit_writes(&mc); /* the writes of the edges just taken (first in the settle order) */
        const odin3_sim_span *out = &cell->ports[E_RD_DATA];
        for (uint32_t k = 0; k < out->width; k++) {
            cell->values[out->idx[k]] = mc.regs[k];
        }
    } else {
        odin3_sim_event want =
            (cell->flags & ODIN3_SIM_CELL_MEM_NEG) != 0 ? ODIN3_SIM_NEGEDGE : ODIN3_SIM_POSEDGE;
        if (cell->event == want) {
            edge_update(&mc);
        }
    }
}

static void async_hook(const odin3_sim_cell *cell, bool four) {
    mctx mc;
    load_ctx(&mc, cell, four);
    if (cell->event == ODIN3_SIM_INIT) {
        if ((cell->flags & ODIN3_SIM_CELL_MEM_INIT) != 0) {
            init_array(&mc, four);
        }
        return;
    }
    if (cell->event != ODIN3_SIM_COMB) {
        return;
    }
    const odin3_sim_span *out = &cell->ports[A_RD_DATA];
    uint32_t width = mc.geo.width;
    for (uint32_t port = 0; port < out->width / width; port++) {
        read_word(&mc, pins_at(cell->ports[A_RD_ADDR].idx, (size_t)port * mc.geo.abits),
                  cell->scratch);
        for (uint32_t bit = 0; bit < width; bit++) {
            cell->values[out->idx[(size_t)port * width + bit]] = cell->scratch[bit];
        }
    }
}

static void edge2(const odin3_sim_cell *cell) {
    edge_hook(cell, false);
}
void odin3_sim_mem_edge4(const odin3_sim_cell *cell) {
    edge_hook(cell, true);
}
static void async2(const odin3_sim_cell *cell) {
    async_hook(cell, false);
}
void odin3_sim_mem_async4(const odin3_sim_cell *cell) {
    async_hook(cell, true);
}

const odin3_celltype_def odin3_sim_mem_edge_def = {"$mem",
                                                   ODIN3_GRAN_WORD,
                                                   ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0,
                                                   k_edge_ports,
                                                   E_N_PORTS,
                                                   NULL,
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   edge2,
                                                   NULL};

const odin3_celltype_def odin3_sim_mem_async_def = {"$mem",
                                                    ODIN3_GRAN_WORD,
                                                    ODIN3_CT_SEQ_LEVEL,
                                                    k_async_ports,
                                                    A_N_PORTS,
                                                    NULL,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    async2,
                                                    NULL};
