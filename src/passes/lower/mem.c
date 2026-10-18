/*
 * mem.c — `$mem` to flip-flops and multiplexers (3B spec §7, LW-9; plan Task 8), the work Yosys's
 * `memory_map` does. 3C `hardmap` maps what it can to RAM blocks and leaves the rest (asynchronous
 * read ports among them) here. The model of one `$mem` (SIZE words of WIDTH bits, OFFSET, ABITS
 * address bits, RD_PORTS read ports, WR_PORTS write ports, all addresses raw):
 *
 *   word of address a   w = (a - OFFSET) mod 2^ABITS; the word exists when w < SIZE. An address
 * that reaches no word reads x and a write to it is ignored. When SIZE > 2^ABITS the words from
 * 2^ABITS up cannot be addressed and are not built. write               all write ports share one
 * clock edge; in port order, a port writes bit bit of its word when WR_EN[bit] is 1; the later port
 * wins a same-address conflict. async read          the word at the address now. sync read at the
 * edge: if RD_EN, the register takes the word as it was before the edge, or, with RD_TRANSPARENT,
 * the data a write port wrote in the same cycle to the same address (bit by bit, the later port
 * winning); else holds.
 *
 * What is built, per bit: one `$_DFF_P_` per stored bit (INIT[k*WIDTH + bit]; x -> 3) fed by a
 * `$_MUX_` chain over the active write ports (a port whose WR_EN is constant 0 is skipped); no
 * flip-flop when no port is active (a ROM: the INIT constant). A write port's word select is an
 * AND of address literals shared over a prefix trie (about two gates a word). A read tree is a
 * binary `$_MUX_` tree over the words, the lowest address bit at the leaves, an x constant for a
 * missing word. A sync read puts a `$_DFF_P_` behind the tree; a transparent one adds, per write
 * port on its clock, a registered copy of the write data and of (WR_EN[bit] & address equal) and a
 * bypass mux (`Mem::extract_rdff`, `emulate_transparency`). A `$mem` with no read port is deleted.
 * Errors (an async write port, write ports on different clocks, the bit cap) are found by the
 * pre-scan, before any cell is lowered.
 */
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* MEM_BYPASS_GATES: the gates of one write port's bypass per data bit (the size estimate). */
enum { MEM_MAX_ABITS = 64, MEM_INIT_UNKNOWN = 3, MEM_BYPASS_GATES = 7 };

/* --- the geometry -------------------------------------------------------------------------- */

typedef struct mem_geo {
    uint64_t size;  /* SIZE */
    uint64_t words; /* the words an address can reach: min(SIZE, 2^ABITS) */
    uint64_t mask;  /* 2^ABITS - 1 (all ones from 64 bits up) */
    int64_t offset;
    uint32_t abits;
    uint32_t width;
    uint32_t n_rd;
    uint32_t n_wr;
    const odin3_value *rd_clk_enable;
    const odin3_value *rd_clk_pol;
    const odin3_value *rd_transparent;
    const odin3_value *wr_clk_enable;
    const odin3_value *wr_clk_pol;
    const odin3_value *init;
    lw_span rd_clk;
    lw_span rd_en;
    lw_span rd_addr;
    lw_span wr_clk;
    lw_span wr_en;
    lw_span wr_addr;
    lw_span wr_data;
} mem_geo;

/* $mem parameter indices (ir/cells/mem.c, declaration order) of the integers the estimate reads. */
enum {
    MEM_P_SIZE = 1,
    MEM_P_OFFSET = 2,
    MEM_P_ABITS = 3,
    MEM_P_WIDTH = 4,
    MEM_P_RD_PORTS = 5,
    MEM_P_WR_PORTS = 6
};

static uint64_t param_u(const lw_cell *cell, uint32_t idx) {
    return cell->params[idx].i > 0 ? (uint64_t)cell->params[idx].i : 0U;
}

/* The integer parameters (by index: the size estimate has no cell type to look names up in). */
static void geo_counts(const lw_cell *cell, mem_geo *geo) {
    memset(geo, 0, sizeof *geo);
    geo->size = param_u(cell, MEM_P_SIZE);
    geo->abits = (uint32_t)param_u(cell, MEM_P_ABITS);
    geo->width = (uint32_t)param_u(cell, MEM_P_WIDTH);
    geo->n_rd = (uint32_t)param_u(cell, MEM_P_RD_PORTS);
    geo->n_wr = (uint32_t)param_u(cell, MEM_P_WR_PORTS);
    geo->offset = cell->params[MEM_P_OFFSET].i;
    geo->mask = geo->abits >= MEM_MAX_ABITS ? UINT64_MAX : ((uint64_t)1 << geo->abits) - 1U;
    bool narrow = geo->abits < MEM_MAX_ABITS && geo->mask + 1U < geo->size;
    geo->words = narrow ? geo->mask + 1U : geo->size;
}

/* The parameters of the cell (no port nets: the pre-scan has none). */
static void geo_params(const lw_cell *cell, mem_geo *geo) {
    geo_counts(cell, geo);
    geo->rd_clk_enable = lw_cell_param(cell, "RD_CLK_ENABLE");
    geo->rd_clk_pol = lw_cell_param(cell, "RD_CLK_POLARITY");
    geo->rd_transparent = lw_cell_param(cell, "RD_TRANSPARENT");
    geo->wr_clk_enable = lw_cell_param(cell, "WR_CLK_ENABLE");
    geo->wr_clk_pol = lw_cell_param(cell, "WR_CLK_POLARITY");
    geo->init = lw_cell_param(cell, "INIT");
}

static void geo_load(const lw_cell *cell, mem_geo *geo) {
    geo_params(cell, geo);
    geo->rd_clk = lw_cell_span(cell, "RD_CLK");
    geo->rd_en = lw_cell_span(cell, "RD_EN");
    geo->rd_addr = lw_cell_span(cell, "RD_ADDR");
    geo->wr_clk = lw_cell_span(cell, "WR_CLK");
    geo->wr_en = lw_cell_span(cell, "WR_EN");
    geo->wr_addr = lw_cell_span(cell, "WR_ADDR");
    geo->wr_data = lw_cell_span(cell, "WR_DATA");
}

/* Bit `idx` of a per-port BITS parameter. */
static bool flag(const odin3_value *val, uint32_t idx) {
    return val != NULL && idx < val->len && val->bits[idx] == ODIN3_BIT_1;
}

/* The raw address of word k: (k + OFFSET) mod 2^ABITS. */
static uint64_t key_of(const mem_geo *geo, uint64_t word) {
    return (uint64_t)((int64_t)word + geo->offset) & geo->mask;
}

/* The word k at position `pos` of the words sorted by address (a rotation: the address wraps to 0
 * at most once, at word `first`). */
static uint64_t word_at(const mem_geo *geo, uint64_t pos) {
    uint64_t zero = (uint64_t)(-geo->offset) & geo->mask;
    uint64_t word = (zero < geo->words ? zero : 0U) + pos;
    return word >= geo->words ? word - geo->words : word;
}

static odin3_bit init_bit(const mem_geo *geo, uint64_t idx) {
    if (geo->init == NULL || idx >= geo->init->len) {
        return ODIN3_BIT_X;
    }
    return (odin3_bit)geo->init->bits[idx];
}

/* --- the pre-scan (errors known before any change) ------------------------------------------- */

static const char *module_text(const lw_ctx *lw) {
    return odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module));
}

static uint32_t port_named(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}

/* The net on bit `bit` of the cell's port `name`, none when open. */
static odin3_net_id pin_of(const lw_ctx *lw, odin3_node_id node, const char *name, uint32_t bit) {
    const odin3_celltype_def *def =
        odin3_celltype_get(lw->design, odin3_node_type(lw->module, node));
    odin3_pinslice pins = odin3_node_port(lw->module, node, port_named(def, name));
    if (bit >= pins.count) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(lw->module, (odin3_pin_id){pins.first.v + bit});
}

static bool const_net(const lw_ctx *lw, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(lw->module, net) == value;
}

/* True when every WR_EN bit of write port wport is the constant 0 (the port is skipped). */
static bool wr_port_skipped(const lw_ctx *lw, odin3_node_id node, const mem_geo *geo,
                            uint32_t wport) {
    for (uint32_t bit = 0; bit < geo->width; bit++) {
        odin3_net_id en = pin_of(lw, node, "WR_EN", wport * geo->width + bit);
        if (!const_net(lw, en, ODIN3_CONST_0)) {
            return false;
        }
    }
    return true;
}

static void log_mem_error(const lw_ctx *lw, odin3_node_id node, const char *text) {
    char label[LW_LABEL_LEN];
    lw_node_label(lw, node, label, sizeof label);
    odin3_log(ODIN3_LOG_ERROR, "lower: module '%s': '$mem' '%s' %s", module_text(lw), label, text);
}

static bool refuse_size(const lw_ctx *lw, odin3_node_id node, const mem_geo *geo) {
    uint64_t cap = lw->opts->max_mem_bits != 0 ? lw->opts->max_mem_bits
                                               : odin3_lower_limits_now().max_mem_bits;
    uint64_t bits = lw_sat_mul(geo->size, geo->width);
    if (geo->abits > MEM_MAX_ABITS) {
        char text[LW_LABEL_LEN * 2];
        (void)snprintf(text, sizeof text, "has ABITS %" PRIu32 " (above %d)", geo->abits,
                       MEM_MAX_ABITS);
        log_mem_error(lw, node, text);
        return true;
    }
    if (bits > cap) {
        char text[LW_LABEL_LEN * 3];
        (void)snprintf(text, sizeof text,
                       "has %" PRIu64 " bits (cap %" PRIu64 "); hardmap it or raise --max-mem-bits",
                       bits, cap);
        log_mem_error(lw, node, text);
        return true;
    }
    return false;
}

/* An asynchronous write port, or write ports on different clocks or polarities. */
static bool refuse_write_ports(const lw_ctx *lw, odin3_node_id node, const mem_geo *geo) {
    bool have_first = false;
    odin3_net_id first_clk = {0};
    bool first_pol = false;
    for (uint32_t wport = 0; wport < geo->n_wr; wport++) {
        if (wr_port_skipped(lw, node, geo, wport)) {
            continue;
        }
        if (!flag(geo->wr_clk_enable, wport)) {
            log_mem_error(lw, node, "has an asynchronous write port");
            return true;
        }
        odin3_net_id clk = pin_of(lw, node, "WR_CLK", wport);
        bool pol = flag(geo->wr_clk_pol, wport);
        if (have_first && (clk.v != first_clk.v || pol != first_pol)) {
            log_mem_error(lw, node, "has write ports on different clocks or polarities");
            return true;
        }
        have_first = true;
        first_clk = clk;
        first_pol = pol;
    }
    return false;
}

bool lw_mem_refused(const lw_ctx *lw, odin3_node_id node, const char *type_name) {
    if (strcmp(type_name, "$mem") != 0) {
        return false;
    }
    lw_cell cell;
    memset(&cell, 0, sizeof cell);
    cell.node = node;
    cell.def = odin3_celltype_get(lw->design, odin3_node_type(lw->module, node));
    cell.params = odin3_node_param(lw->module, node, 0);
    mem_geo geo;
    geo_params(&cell, &geo);
    if (geo.n_rd == 0) {
        return false; /* deleted: nothing of it is built */
    }
    bool refused = refuse_size(lw, node, &geo);
    return refused || refuse_write_ports(lw, node, &geo);
}

/* --- the builder state ------------------------------------------------------------------------ */

/* A clock as the storage sees it: the net, and whether the flip-flop is `$_DFF_N_`. */
typedef struct mem_clk {
    odin3_net_id net;
    bool negative;
} mem_clk;

/* An inverted clock made already. */
typedef struct mem_inv {
    odin3_net_id src;
    odin3_net_id inv;
} mem_inv;

/* The AND of write enable and word select shared by the bits of a run with the same enable. */
typedef struct mem_share {
    odin3_net_id en;
    odin3_net_id sel;
} mem_share;

typedef struct mem_build {
    lw_ctx *lw;
    const lw_cell *cell;
    mem_geo geo;
    uint64_t dffs;
    bool inverted;
    odin3_vec acts;       /* uint32_t: the write ports that are not skipped */
    odin3_vec decode;     /* odin3_net_id: [active port][word] */
    odin3_vec share;      /* mem_share: per active port, the run of the current word */
    odin3_vec clocks;     /* mem_inv */
    odin3_vec keys;       /* uint64_t: scratch of a read tree */
    lw_bits q;            /* storage Q nets, [word][bit] */
    lw_bits tree;         /* scratch of a read tree */
    odin3_net_id unknown; /* the x constant, the missing arm of a read tree */
} mem_build;

static void build_init(mem_build *mb, lw_ctx *lw, const lw_cell *cell) {
    memset(mb, 0, sizeof *mb);
    mb->lw = lw;
    mb->cell = cell;
    geo_load(cell, &mb->geo);
    odin3_vec_init(&mb->acts, sizeof(uint32_t));
    odin3_vec_init(&mb->decode, sizeof(odin3_net_id));
    odin3_vec_init(&mb->share, sizeof(mem_share));
    odin3_vec_init(&mb->clocks, sizeof(mem_inv));
    odin3_vec_init(&mb->keys, sizeof(uint64_t));
    lw_bits_init(&mb->q);
    lw_bits_init(&mb->tree);
}

static void build_free(mem_build *mb) {
    odin3_vec_free(&mb->acts);
    odin3_vec_free(&mb->decode);
    odin3_vec_free(&mb->share);
    odin3_vec_free(&mb->clocks);
    odin3_vec_free(&mb->keys);
    lw_bits_free(&mb->q);
    lw_bits_free(&mb->tree);
}

static uint32_t act_port(const mem_build *mb, uint32_t ai) {
    return *(const uint32_t *)odin3_vec_cat(&mb->acts, ai);
}

static uint32_t n_acts(const mem_build *mb) {
    return (uint32_t)mb->acts.len;
}

static bool net_const(const mem_build *mb, odin3_net_id net, odin3_const value) {
    return const_net(mb->lw, net, value);
}

/* The write ports that are not skipped, in port order. */
static odin3_status collect_acts(mem_build *mb) {
    const mem_geo *geo = &mb->geo;
    for (uint32_t wport = 0; wport < geo->n_wr; wport++) {
        bool live = false;
        for (uint32_t bit = 0; bit < geo->width && !live; bit++) {
            live = !net_const(mb, geo->wr_en.nets[wport * geo->width + bit], ODIN3_CONST_0);
        }
        uint32_t *slot = live ? odin3_vec_push(&mb->acts) : NULL;
        if (live && slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (slot != NULL) {
            *slot = wport;
        }
    }
    return ODIN3_OK;
}

/* --- clocks and flip-flops ------------------------------------------------------------------- */

/* The clock of storage on `src` with edge `high`: the net itself, `$_DFF_N_` for a falling edge
 * with --neg-edge keep, else one shared `$_NOT_` (a per-cell warning in `book`). */
static odin3_status clock_for(mem_build *mb, odin3_net_id src, bool high, mem_clk *out) {
    *out = (mem_clk){src, false};
    if (high) {
        return ODIN3_OK;
    }
    if (mb->lw->opts->keep_neg_edge) {
        out->negative = true;
        return ODIN3_OK;
    }
    for (size_t i = 0; i < mb->clocks.len; i++) {
        const mem_inv *inv = odin3_vec_cat(&mb->clocks, i);
        if (inv->src.v == src.v) {
            out->net = inv->inv;
            return ODIN3_OK;
        }
    }
    mem_inv *slot = odin3_vec_push(&mb->clocks);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (mem_inv){src, {0}};
    odin3_status st = lw_not(mb->lw, src, &slot->inv);
    out->net = slot->inv;
    mb->inverted = true;
    return st;
}

/* One storage node on the existing net q. */
typedef struct mem_dff_args {
    mem_clk clk;
    odin3_net_id data;
    odin3_bit init;
} mem_dff_args;

static odin3_status make_dff(mem_build *mb, const mem_dff_args *args, odin3_net_id qnet) {
    bool known = args->init == ODIN3_BIT_0 || args->init == ODIN3_BIT_1;
    const odin3_value param = odin3_value_int(known ? (int64_t)args->init : MEM_INIT_UNKNOWN);
    const odin3_net_id in[2] = {args->clk.net, args->data};
    const lw_gate_spec spec = {args->clk.negative ? LW_G_DFF_N : LW_G_DFF_P, in, 2, &param};
    odin3_status st = lw_gate_on(mb->lw, &spec, qnet);
    mb->dffs += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* A register on `clk` with enable `en` (constant 1: always) taking `val`; *qnet is its output. */
typedef struct mem_reg_args {
    mem_clk clk;
    odin3_net_id en;
} mem_reg_args;

static odin3_status make_reg(mem_build *mb, const mem_reg_args *reg, odin3_net_id val,
                             odin3_net_id *qnet) {
    odin3_status st = lw_net(mb->lw, qnet);
    odin3_net_id next = val;
    if (st == ODIN3_OK && !net_const(mb, reg->en, ODIN3_CONST_1)) {
        st = lw_mux(mb->lw, *qnet, val, reg->en, &next);
    }
    mem_dff_args args = {reg->clk, next, ODIN3_BIT_X};
    return st == ODIN3_OK ? make_dff(mb, &args, *qnet) : st;
}

/* --- the write select of each word ------------------------------------------------------------ */

/* The decoder of one write port: literals of the address bits and the chain of ANDs. */
typedef struct mem_dec {
    lw_span addr;
    uint64_t key;  /* the raw address of the word being decoded */
    uint32_t kept; /* how many of its top bits chain[] already holds (shared with the last word) */
    odin3_net_id inverted[MEM_MAX_ABITS];
    odin3_net_id chain[MEM_MAX_ABITS + 1];
} mem_dec;

static odin3_status literal(mem_build *mb, mem_dec *dec, uint32_t bit, bool value,
                            odin3_net_id *out) {
    if (value) {
        *out = dec->addr.nets[bit];
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    if (!odin3_net_valid(dec->inverted[bit])) {
        st = lw_not(mb->lw, dec->addr.nets[bit], &dec->inverted[bit]);
    }
    *out = dec->inverted[bit];
    return st;
}

/* How many of the top address bits `key` shares with `prev` (both ABITS wide, different). */
typedef struct mem_keys {
    uint64_t prev;
    uint64_t key;
} mem_keys;

static uint32_t shared_top(const mem_keys *keys, uint32_t abits) {
    uint64_t diff = keys->prev ^ keys->key;
    uint32_t high = 0;
    while ((diff >> high) > 1U) {
        high++;
    }
    return abits - 1U - high;
}

/* chain[depth] for the top `depth` bits of key, depth in (kept, abits]. */
static odin3_status extend_chain(mem_build *mb, mem_dec *dec) {
    odin3_status st = ODIN3_OK;
    for (uint32_t depth = dec->kept + 1; st == ODIN3_OK && depth <= mb->geo.abits; depth++) {
        uint32_t bit = mb->geo.abits - depth;
        odin3_net_id lit = {0};
        st = literal(mb, dec, bit, ((dec->key >> bit) & 1U) != 0, &lit);
        if (st == ODIN3_OK) {
            st = depth == 1 ? ODIN3_OK : lw_and(mb->lw, dec->chain[depth - 1], lit, &lit);
        }
        dec->chain[depth] = lit;
    }
    return st;
}

/* The select of every word for active port `ai`, into mb->decode[ai * words + k]. */
static odin3_status decode_port(mem_build *mb, uint32_t ai) {
    const mem_geo *geo = &mb->geo;
    mem_dec dec;
    memset(&dec, 0, sizeof dec);
    dec.addr = (lw_span){geo->wr_addr.nets + (size_t)act_port(mb, ai) * geo->abits, geo->abits};
    odin3_status st = geo->abits == 0 ? lw_const(mb->lw, ODIN3_BIT_1, &dec.chain[0]) : ODIN3_OK;
    uint64_t prev = 0;
    for (uint64_t pos = 0; st == ODIN3_OK && pos < geo->words; pos++) {
        uint64_t word = word_at(geo, pos);
        uint64_t key = key_of(geo, word);
        dec.key = key;
        dec.kept = pos == 0 ? 0 : shared_top(&(mem_keys){prev, key}, geo->abits);
        st = extend_chain(mb, &dec);
        odin3_net_id *slot = odin3_vec_at(&mb->decode, (size_t)ai * geo->words + word);
        *slot = dec.chain[geo->abits];
        prev = key;
    }
    return st;
}

/* mb->decode filled with none nets, room for every (port, word); one decoder per active port. */
static odin3_status decode_all(mem_build *mb) {
    odin3_status st = ODIN3_OK;
    size_t total = (size_t)n_acts(mb) * mb->geo.words;
    for (size_t i = 0; i < total; i++) {
        odin3_net_id *slot = odin3_vec_push(&mb->decode);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_net_id){0};
    }
    for (uint32_t ai = 0; st == ODIN3_OK && ai < n_acts(mb); ai++) {
        st = decode_port(mb, ai);
    }
    return st;
}

/* --- storage ---------------------------------------------------------------------------------- */

/* A stored bit (and the active write port a step is looking at). */
typedef struct mem_at {
    uint64_t word;
    uint32_t bit;
    uint32_t ai;
} mem_at;

/* sel = word select & WR_EN[bit] of active port ai (none: the bit is never written there); the AND
 * is shared over a run of bits with the same enable net. */
static odin3_status write_sel(mem_build *mb, const mem_at *at, odin3_net_id *sel) {
    const mem_geo *geo = &mb->geo;
    odin3_net_id en = geo->wr_en.nets[(size_t)act_port(mb, at->ai) * geo->width + at->bit];
    odin3_net_id dec =
        *(const odin3_net_id *)odin3_vec_cat(&mb->decode, at->ai * geo->words + at->word);
    *sel = (odin3_net_id){0};
    if (net_const(mb, en, ODIN3_CONST_0)) {
        return ODIN3_OK;
    }
    if (net_const(mb, en, ODIN3_CONST_1)) {
        *sel = dec;
        return ODIN3_OK;
    }
    mem_share *share = odin3_vec_at(&mb->share, at->ai);
    odin3_status st = ODIN3_OK;
    if (share->en.v != en.v) {
        st = lw_and(mb->lw, dec, en, &share->sel);
        share->en = en;
    }
    *sel = share->sel;
    return st;
}

/* N[word][bit]: q held, then the write ports in order. */
static odin3_status next_state(mem_build *mb, mem_at at, odin3_net_id *next) {
    const mem_geo *geo = &mb->geo;
    odin3_net_id net = *next;
    odin3_status st = ODIN3_OK;
    for (at.ai = 0; st == ODIN3_OK && at.ai < n_acts(mb); at.ai++) {
        odin3_net_id sel = {0};
        st = write_sel(mb, &at, &sel);
        if (st == ODIN3_OK && odin3_net_valid(sel)) {
            odin3_net_id data =
                geo->wr_data.nets[(size_t)act_port(mb, at.ai) * geo->width + at.bit];
            st = lw_mux(mb->lw, net, data, sel, &net);
        }
    }
    *next = net;
    return st;
}

/* The stored bit (word, bit): a ROM bit is its INIT constant, else a flip-flop. */
static odin3_status storage_bit(mem_build *mb, const mem_clk *clk, mem_at at) {
    const mem_geo *geo = &mb->geo;
    odin3_bit init = init_bit(geo, at.word * geo->width + at.bit);
    odin3_net_id qnet = {0};
    odin3_status st = ODIN3_OK;
    if (n_acts(mb) == 0) {
        st = lw_const(mb->lw, init == ODIN3_BIT_0 || init == ODIN3_BIT_1 ? init : ODIN3_BIT_X,
                      &qnet);
        return st == ODIN3_OK ? lw_bits_push(&mb->q, qnet) : st;
    }
    odin3_net_id next = {0};
    st = lw_net(mb->lw, &qnet);
    next = qnet;
    st = st == ODIN3_OK ? next_state(mb, at, &next) : st;
    mem_dff_args args = {*clk, next, init};
    st = st == ODIN3_OK ? make_dff(mb, &args, qnet) : st;
    return st == ODIN3_OK ? lw_bits_push(&mb->q, qnet) : st;
}

static odin3_status storage_all(mem_build *mb) {
    const mem_geo *geo = &mb->geo;
    mem_clk clk = {{0}, false};
    odin3_status st = ODIN3_OK;
    if (n_acts(mb) > 0) {
        uint32_t wport = act_port(mb, 0);
        st = clock_for(mb, geo->wr_clk.nets[wport], flag(geo->wr_clk_pol, wport), &clk);
    }
    for (uint32_t ai = 0; st == ODIN3_OK && ai < n_acts(mb); ai++) {
        mem_share none = {{0}, {0}};
        mem_share *slot = odin3_vec_push(&mb->share);
        st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (slot != NULL) {
            *slot = none;
        }
    }
    for (uint64_t word = 0; st == ODIN3_OK && word < geo->words; word++) {
        for (uint32_t ai = 0; ai < n_acts(mb); ai++) {
            ((mem_share *)odin3_vec_at(&mb->share, ai))->en = (odin3_net_id){0};
        }
        for (uint32_t bit = 0; st == ODIN3_OK && bit < geo->width; bit++) {
            st = storage_bit(mb, &clk, (mem_at){word, bit, 0});
        }
    }
    return st;
}

/* --- read ports ------------------------------------------------------------------------------- */

/* The tree level that selects on address bit `bit` (sel): pairs of words that differ in the lowest
 * remaining key bit become a mux, a lone one a mux with an x arm. n entries in, the new count out.
 */
static odin3_status merge_level(mem_build *mb, odin3_net_id sel, uint32_t *count) {
    uint32_t used = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < *count;) {
        uint64_t key = *(const uint64_t *)odin3_vec_cat(&mb->keys, i);
        bool paired = (key & 1U) == 0 && i + 1 < *count &&
                      *(const uint64_t *)odin3_vec_cat(&mb->keys, i + 1) == key + 1U;
        odin3_net_id lo = (key & 1U) == 0 ? lw_bits_at(&mb->tree, i) : mb->unknown;
        odin3_net_id hi = (key & 1U) == 0 ? mb->unknown : lw_bits_at(&mb->tree, i);
        hi = paired ? lw_bits_at(&mb->tree, i + 1) : hi;
        odin3_net_id mux = {0};
        st = lw_mux(mb->lw, lo, hi, sel, &mux);
        lw_bits_set(&mb->tree, used, mux);
        *(uint64_t *)odin3_vec_at(&mb->keys, used) = key >> 1;
        used++;
        i += paired ? 2U : 1U;
    }
    *count = used;
    return st;
}

/* The async read of data bit bit on port rport: the root of the mux tree over the words. */
typedef struct mem_tap {
    uint32_t rport;
    uint32_t bit;
} mem_tap;

static odin3_status read_tree(mem_build *mb, mem_tap tap, odin3_net_id *root) {
    const mem_geo *geo = &mb->geo;
    lw_bits_clear(&mb->tree);
    odin3_vec_clear(&mb->keys);
    odin3_status st = ODIN3_OK;
    for (uint64_t pos = 0; st == ODIN3_OK && pos < geo->words; pos++) {
        uint64_t word = word_at(geo, pos);
        uint64_t *key = odin3_vec_push(&mb->keys);
        st = key == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (key != NULL) {
            *key = key_of(geo, word);
        }
        st = st == ODIN3_OK
                 ? lw_bits_push(&mb->tree,
                                lw_bits_at(&mb->q, (uint32_t)(word * geo->width + tap.bit)))
                 : st;
    }
    uint32_t count = (uint32_t)geo->words;
    for (uint32_t level = 0; st == ODIN3_OK && level < geo->abits; level++) {
        st = merge_level(mb, geo->rd_addr.nets[(size_t)tap.rport * geo->abits + level], &count);
    }
    if (st == ODIN3_OK) {
        *root = lw_bits_at(&mb->tree, 0);
    }
    return st;
}

/* Where a transparent read port's bypass reads: the read clock and the write port. */
typedef struct mem_bypass {
    uint32_t rport;
    uint32_t ai;
    uint32_t first; /* the index of the port's first data bit in the output vector */
    mem_reg_args reg;
} mem_bypass;

/* AND over the address bits of (read address == write address) of the bypass; const 1 for ABITS 0
 */
static odin3_status address_equal(mem_build *mb, const mem_bypass *bp, odin3_net_id *eq) {
    const mem_geo *geo = &mb->geo;
    if (geo->abits == 0) {
        return lw_const(mb->lw, ODIN3_BIT_1, eq);
    }
    lw_bits bits;
    lw_bits_init(&bits);
    odin3_status st = ODIN3_OK;
    const odin3_net_id *raddr = geo->rd_addr.nets + (size_t)bp->rport * geo->abits;
    const odin3_net_id *waddr = geo->wr_addr.nets + (size_t)act_port(mb, bp->ai) * geo->abits;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < geo->abits; bit++) {
        odin3_net_id same = {0};
        st = lw_xnor(mb->lw, raddr[bit], waddr[bit], &same);
        st = st == ODIN3_OK ? lw_bits_push(&bits, same) : st;
    }
    st = st == ODIN3_OK ? lw_tree(mb->lw, LW_G_AND, lw_bits_span(&bits), eq) : st;
    lw_bits_free(&bits);
    return st;
}

/* The state of a bypass between its bits: the last enable net and the register of its condition. */
typedef struct mem_cond {
    odin3_net_id en;
    odin3_net_id eq;
    odin3_net_id cond_q;
} mem_cond;

/* The bypass of data bit `bit`: a registered copy of the write data and of the condition (WR_EN &
 * address equal, made once per run of bits with the same enable) and a mux; a constant-0 enable
 * has none. */
static odin3_status bypass_bit(mem_build *mb, const mem_bypass *bp, mem_cond *cond, uint32_t bit,
                               lw_bits *data) {
    const mem_geo *geo = &mb->geo;
    size_t base = (size_t)act_port(mb, bp->ai) * geo->width;
    odin3_net_id en = geo->wr_en.nets[base + bit];
    odin3_status st = ODIN3_OK;
    if (net_const(mb, en, ODIN3_CONST_0)) {
        return st;
    }
    if (en.v != cond->en.v) {
        odin3_net_id next = cond->eq;
        st = net_const(mb, en, ODIN3_CONST_1) ? ODIN3_OK : lw_and(mb->lw, en, cond->eq, &next);
        st = st == ODIN3_OK ? make_reg(mb, &bp->reg, next, &cond->cond_q) : st;
        cond->en = en;
    }
    odin3_net_id data_q = {0};
    odin3_net_id mux = {0};
    st = st == ODIN3_OK ? make_reg(mb, &bp->reg, geo->wr_data.nets[base + bit], &data_q) : st;
    odin3_net_id was = lw_bits_at(data, bp->first + bit);
    st = st == ODIN3_OK ? lw_mux(mb->lw, was, data_q, cond->cond_q, &mux) : st;
    if (st == ODIN3_OK) {
        lw_bits_set(data, bp->first + bit, mux);
    }
    return st;
}

/* The bypass of write port ai over the registered read data `data` (width bits). */
static odin3_status bypass_port(mem_build *mb, const mem_bypass *bp, lw_bits *data) {
    mem_cond cond = {{0}, {0}, {0}};
    odin3_status st = address_equal(mb, bp, &cond.eq);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < mb->geo.width; bit++) {
        st = bypass_bit(mb, bp, &cond, bit, data);
    }
    return st;
}

/* The write ports (active ones) a transparent read port rport sees: the same clock net and edge. */
static bool sees_write(const mem_build *mb, const mem_bypass *bp) {
    const mem_geo *geo = &mb->geo;
    uint32_t wport = act_port(mb, bp->ai);
    return geo->wr_clk.nets[wport].v == geo->rd_clk.nets[bp->rport].v &&
           flag(geo->wr_clk_pol, wport) == flag(geo->rd_clk_pol, bp->rport);
}

/* One sync read port: a register per data bit behind the tree, then the bypasses. */
static odin3_status read_sync(mem_build *mb, uint32_t rport, lw_bits *data) {
    const mem_geo *geo = &mb->geo;
    mem_bypass bp = {rport, 0, lw_bits_len(data), {{{0}, false}, geo->rd_en.nets[rport]}};
    odin3_status st =
        clock_for(mb, geo->rd_clk.nets[rport], flag(geo->rd_clk_pol, rport), &bp.reg.clk);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < geo->width; bit++) {
        odin3_net_id root = {0};
        odin3_net_id qnet = {0};
        st = read_tree(mb, (mem_tap){rport, bit}, &root);
        st = st == ODIN3_OK ? make_reg(mb, &bp.reg, root, &qnet) : st;
        st = st == ODIN3_OK ? lw_bits_push(data, qnet) : st;
    }
    for (bp.ai = 0; st == ODIN3_OK && flag(geo->rd_transparent, rport) && bp.ai < n_acts(mb);
         bp.ai++) {
        st = sees_write(mb, &bp) ? bypass_port(mb, &bp, data) : ODIN3_OK;
    }
    return st;
}

static odin3_status read_async(mem_build *mb, uint32_t rport, lw_bits *data) {
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < mb->geo.width; bit++) {
        odin3_net_id root = {0};
        st = read_tree(mb, (mem_tap){rport, bit}, &root);
        st = st == ODIN3_OK ? lw_bits_push(data, root) : st;
    }
    return st;
}

static odin3_status read_all(mem_build *mb, lw_bits *out) {
    odin3_status st = ODIN3_OK;
    for (uint32_t rport = 0; st == ODIN3_OK && rport < mb->geo.n_rd; rport++) {
        st = flag(mb->geo.rd_clk_enable, rport) ? read_sync(mb, rport, out)
                                                : read_async(mb, rport, out);
    }
    return st;
}

/* --- the cell --------------------------------------------------------------------------------- */

/* The warnings, the report entries and the totals of a finished cell. */
static odin3_status book(const mem_build *mb) {
    lw_ctx *lw = mb->lw;
    char label[LW_LABEL_LEN];
    lw_node_label(lw, mb->cell->node, label, sizeof label);
    lw_report_entry entry = {module_text(lw), label, "$mem", mb->geo.size * mb->geo.width, NULL};
    odin3_log(ODIN3_LOG_WARN, "lower: module '%s': '$mem' '%s' lowered to %" PRIu64 " flip-flops",
              entry.module, label, mb->dffs);
    odin3_status st = lw_report_add(lw, LW_REP_MEM, &entry);
    if (st == ODIN3_OK && mb->inverted) {
        odin3_log(ODIN3_LOG_WARN,
                  "lower: module '%s': falling-edge '$mem' '%s' gets a clock inverter",
                  entry.module, label);
        st = lw_report_add(lw, LW_REP_NEG_EDGE, &entry);
    }
    return st;
}

odin3_status lw_build_mem(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    mem_build mb;
    build_init(&mb, lw, cell);
    lw_bits_clear(out);
    odin3_status st = ODIN3_OK;
    if (mb.geo.n_rd > 0) {
        st = collect_acts(&mb);
        st = st == ODIN3_OK ? lw_const(lw, ODIN3_BIT_X, &mb.unknown) : st;
        st = st == ODIN3_OK ? decode_all(&mb) : st;
        st = st == ODIN3_OK ? storage_all(&mb) : st;
        st = st == ODIN3_OK ? read_all(&mb, out) : st;
    }
    st = st == ODIN3_OK ? book(&mb) : st;
    build_free(&mb);
    return st;
}

/* An upper bound of the gates (spec §11), from the widths alone. Per word-bit: the flip-flop, and
 * per write port a mux and an AND; per write port a decoder of about 2 gates a word, A inverters
 * and A ANDs of slack; per read port and data bit the tree (words + 3A + 2 muxes), the register
 * and its enable mux, and per write port the transparent bypass (two registers with their enable
 * muxes, an AND and the output mux: 7); per read port the address compare (2A) and a clock
 * inverter. */
uint64_t lw_est_mem(const lw_cell *cell) {
    mem_geo geo;
    geo_counts(cell, &geo);
    if (geo.n_rd == 0) {
        return 0;
    }
    uint64_t abits = geo.abits;
    uint64_t cells = lw_sat_mul(geo.words, geo.width);
    uint64_t total = lw_sat_mul(cells, lw_sat_add(1U, lw_sat_mul(2U, geo.n_wr)));
    uint64_t decoder = lw_sat_add(lw_sat_mul(2U, geo.words), lw_sat_mul(3U, abits));
    total = lw_sat_add(total, lw_sat_mul(geo.n_wr, decoder));
    uint64_t tree = lw_sat_add(lw_sat_add(geo.words, lw_sat_mul(3U, abits)), 4U);
    uint64_t per_bit = lw_sat_add(tree, lw_sat_mul(MEM_BYPASS_GATES, geo.n_wr));
    uint64_t per_port = lw_sat_add(lw_sat_mul(geo.width, per_bit),
                                   lw_sat_add(lw_sat_mul(2U, lw_sat_mul(abits, geo.n_wr)), 2U));
    total = lw_sat_add(total, lw_sat_mul(geo.n_rd, per_port));
    return lw_sat_add(total, 2U);
}
