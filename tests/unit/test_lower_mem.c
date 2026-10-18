/*
 * test_lower_mem.c — 3B Task 8 (spec §7, §10, §14.1, §14.3): `$mem` to flip-flops and
 * multiplexers. The lowered module runs in the 1E simulator against the independent C model of
 * lower_memref.c: write all addresses then read all, one and two write ports (the later wins),
 * OFFSET 0, 1, -2 and a wrapping one, ABITS smaller and larger than clog2(SIZE) (an address that
 * reaches no word reads x, writes to it are lost), byte enables, RD_EN low on a sync port,
 * transparent reads, INIT contents read before any write, a ROM, a $mem with no read port, a write
 * port with constant-0 enables; the structure counts of a small memory; the errors, the caps, the
 * INFO/WARN lines and the report entry; `created <= estimate`; OOM at every allocation.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "lower_memref.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 8192,
    MSG_LEN = 200,
    N_PARAMS = 13,
    RANDOM_CYCLES = 300,
    MAX_BITS = MEMREF_MAX_WORDS * MEMREF_MAX_WIDTH,
    P_RD_CLK = 0,
    P_RD_EN = 1,
    P_RD_ADDR = 2,
    P_WR_CLK = 4,
    P_WR_EN = 5
};

static char log_text[LOG_TEXT];

/* Masks over the read and write ports: the clock polarity opposite to the rest (setUp resets). */
static uint8_t g_rd_flip;
static uint8_t g_wr_flip;

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    (void)level;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    g_rd_flip = 0;
    g_wr_flip = 0;
    odin3_log_set_sink(text_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_lower_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
}

/* --- the cases ---------------------------------------------------------------------------- */

/* How a write port's WR_EN is wired: module inputs per bit, constant 0, constant 1, or one input
 * net for all its bits. */
typedef enum en_kind { EN_INPUTS, EN_ZERO, EN_ONE, EN_SHARED } en_kind;

typedef struct mcase {
    memref_cfg cfg;
    const char *name;
    bool use_init;
    bool share_clock; /* every clock pin on one net (else one net per port bit) */
    bool falling;     /* CLK_POLARITY 0 */
    bool async_write;
    en_kind en[MEMREF_MAX_PORTS];
} mcase;

typedef struct mem_params {
    uint8_t rd_en[MEMREF_MAX_PORTS];
    uint8_t rd_pol[MEMREF_MAX_PORTS];
    uint8_t rd_tr[MEMREF_MAX_PORTS];
    uint8_t wr_en[MEMREF_MAX_PORTS];
    uint8_t wr_pol[MEMREF_MAX_PORTS];
    uint8_t init[MAX_BITS];
    odin3_value v[N_PARAMS];
} mem_params;

/* INIT bit of word k bit j: a fixed pattern of 0, 1 and x. */
static uint8_t init_pattern(uint32_t word, uint32_t bit) {
    return (uint8_t)((word * 7U + bit * 3U + word / 3U) % 3U);
}

static odin3_value bits_value(const uint8_t *bits, uint32_t len) {
    return (odin3_value){ODIN3_VAL_BITS, 0, bits, len, 0, 0};
}

static void fill_port_bits(const mcase *cs, mem_params *pp) {
    const memref_cfg *cfg = &cs->cfg;
    for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
        pp->rd_en[rport] = cfg->rd_sync[rport] ? ODIN3_BIT_1 : ODIN3_BIT_0;
        bool rd_low = cs->falling != (((g_rd_flip >> rport) & 1) != 0);
        pp->rd_pol[rport] = rd_low ? ODIN3_BIT_0 : ODIN3_BIT_1;
        pp->rd_tr[rport] = cfg->rd_trans[rport] ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    for (uint32_t wport = 0; wport < cfg->n_wr; wport++) {
        pp->wr_en[wport] = cs->async_write ? ODIN3_BIT_0 : ODIN3_BIT_1;
        bool wr_low = cs->falling != (((g_wr_flip >> wport) & 1) != 0);
        pp->wr_pol[wport] = wr_low ? ODIN3_BIT_0 : ODIN3_BIT_1;
    }
}

static void fill_init(const mcase *cs, mem_params *pp) {
    for (uint32_t k = 0; k < cs->cfg.size && cs->use_init; k++) {
        for (uint32_t j = 0; j < cs->cfg.width; j++) {
            uint8_t pat = init_pattern(k, j);
            pp->init[k * cs->cfg.width + j] = pat == 2 ? ODIN3_BIT_X : pat;
        }
    }
}

static void make_params(lf *fix, const mcase *cs, mem_params *pp) {
    const memref_cfg *cfg = &cs->cfg;
    fill_port_bits(cs, pp);
    fill_init(cs, pp);
    uint32_t n_init = cs->use_init ? cfg->size * cfg->width : 0;
    pp->v[0] = (odin3_value){ODIN3_VAL_STRING, 0, NULL, 0, lf_str(fix, "mem"), 0};
    pp->v[1] = odin3_value_int(cfg->size);
    pp->v[2] = odin3_value_int(cfg->offset);
    pp->v[3] = odin3_value_int(cfg->abits);
    pp->v[4] = odin3_value_int(cfg->width);
    pp->v[5] = odin3_value_int(cfg->n_rd);
    pp->v[6] = odin3_value_int(cfg->n_wr);
    pp->v[7] = bits_value(pp->rd_en, cfg->n_rd);
    pp->v[8] = bits_value(pp->rd_pol, cfg->n_rd);
    pp->v[9] = bits_value(pp->rd_tr, cfg->n_rd);
    pp->v[10] = bits_value(pp->wr_en, cfg->n_wr);
    pp->v[11] = bits_value(pp->wr_pol, cfg->n_wr);
    pp->v[12] = bits_value(pp->init, n_init);
}

typedef struct md {
    lf fix;
    mcase cs;
    mem_params pp;
    odin3_module *mod;
    odin3_node_id node;
} md;

/* Moves the pin (port, bit) of the cell to `net`. */
typedef struct pin_ref {
    uint32_t port;
    uint32_t bit;
} pin_ref;

static void retarget(md *dd, pin_ref ref, odin3_net_id net) {
    odin3_pinslice pins = odin3_node_port(dd->mod, dd->node, ref.port);
    odin3_pin_id pin = {pins.first.v + ref.bit};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(dd->mod, pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(dd->mod, pin, net));
}

static odin3_net_id const_net(md *dd, bool one) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(dd->fix.design,
                                         lf_str(&dd->fix, one ? "$_CONST1_" : "$_CONST0_"), &tid));
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(dd->mod, 0, (odin3_prov_id){0}, &net));
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {tid, 0, {0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(dd->mod, &spec, &port, &node));
    return net;
}

static void wire_enables(md *dd) {
    const memref_cfg *cfg = &dd->cs.cfg;
    for (uint32_t wport = 0; wport < cfg->n_wr; wport++) {
        en_kind kind = dd->cs.en[wport];
        odin3_net_id net = kind == EN_SHARED
                               ? lf_wnet(&dd->fix, dd->mod, "WR_EN", wport * cfg->width)
                               : (odin3_net_id){0};
        net = kind == EN_ZERO || kind == EN_ONE ? const_net(dd, kind == EN_ONE) : net;
        for (uint32_t j = 0; kind != EN_INPUTS && j < cfg->width; j++) {
            retarget(dd, (pin_ref){P_WR_EN, wport * cfg->width + j}, net);
        }
    }
}

/* Every clock pin on the net of the first clock pin (write port 0, else read port 0). */
static void wire_clocks(md *dd) {
    const memref_cfg *cfg = &dd->cs.cfg;
    if (!dd->cs.share_clock) {
        return;
    }
    odin3_net_id net = lf_wnet(&dd->fix, dd->mod, cfg->n_wr > 0 ? "WR_CLK" : "RD_CLK", 0);
    for (uint32_t wport = 1; wport < cfg->n_wr; wport++) {
        retarget(dd, (pin_ref){P_WR_CLK, wport}, net);
    }
    for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
        retarget(dd, (pin_ref){P_RD_CLK, rport}, net);
    }
}

static void md_make(md *dd, const mcase *cs) {
    dd->cs = *cs;
    lf_begin(&dd->fix);
    make_params(&dd->fix, cs, &dd->pp);
    lf_cell_spec spec = {"$mem", dd->pp.v, N_PARAMS, "dut", {0}};
    dd->node = lf_one_cell(&dd->fix, &spec, &dd->mod);
    wire_enables(dd);
    wire_clocks(dd);
}

static odin3_status md_lower(md *dd, const char *args) {
    return odin3_pass_run(dd->fix.design, "lower", odin3_bytes_cstr(args));
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

static void describe(const mcase *cs, char *msg) {
    (void)snprintf(msg, MSG_LEN, "%s (size %u w %u abits %u off %d rd %u wr %u)", cs->name,
                   cs->cfg.size, cs->cfg.width, cs->cfg.abits, cs->cfg.offset, cs->cfg.n_rd,
                   cs->cfg.n_wr);
}

static void md_lowered(md *dd, const mcase *cs, const char *args) {
    md_make(dd, cs);
    char msg[MSG_LEN];
    describe(cs, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, md_lower(dd, args), msg);
    check_netlist_view(dd->mod);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(&dd->fix, dd->mod), msg);
}

/* --- the simulator side ------------------------------------------------------------------- */

static void drive(odin3_sim *sim, const char *name, uint64_t value) {
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        if (strcmp(odin3_sim_input_name(sim, port), name) != 0) {
            continue;
        }
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim, port); bit++) {
            odin3_sim_bit at = {port, bit};
            if (!odin3_sim_input_is_clock(sim, at)) {
                TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                      odin3_sim_set_input(sim, at, ((value >> bit) & 1U) != 0));
            }
        }
    }
}

static uint64_t read_port(const odin3_sim *sim, const char *name) {
    uint64_t value = 0;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        if (strcmp(odin3_sim_output_name(sim, port), name) != 0) {
            continue;
        }
        for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
            bool got = false;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &got));
            value |= got ? (uint64_t)1 << bit : 0U;
        }
    }
    return value;
}

static void drive_inputs(odin3_sim *sim, const memref_cfg *cfg, const memref_in *in) {
    uint64_t en = 0;
    uint64_t raddr = 0;
    uint64_t waddr = 0;
    uint64_t wen = 0;
    uint64_t wdata = 0;
    for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
        en |= in->rd_en[rport] ? (uint64_t)1 << rport : 0U;
        raddr |= in->rd_addr[rport] << (rport * cfg->abits);
    }
    for (uint32_t wport = 0; wport < cfg->n_wr; wport++) {
        waddr |= in->wr_addr[wport] << (wport * cfg->abits);
        wen |= in->wr_en[wport] << (wport * cfg->width);
        wdata |= in->wr_data[wport] << (wport * cfg->width);
    }
    drive(sim, "RD_EN", en);
    drive(sim, "RD_ADDR", raddr);
    drive(sim, "WR_ADDR", waddr);
    drive(sim, "WR_EN", wen);
    drive(sim, "WR_DATA", wdata);
}

typedef struct harness {
    md dd;
    odin3_sim *sim;
    memref model;
    char msg[MSG_LEN];
} harness;

static uint8_t init_bit_of(const mcase *cs, uint32_t idx) {
    return init_pattern(idx / cs->cfg.width, idx % cs->cfg.width);
}

/* The model of the case with its INIT. */
static void model_start(const mcase *cs, memref *model) {
    uint8_t init[MAX_BITS];
    for (uint32_t idx = 0; idx < cs->cfg.size * cs->cfg.width; idx++) {
        init[idx] = init_bit_of(cs, idx);
    }
    memref_init(model, &cs->cfg, cs->use_init ? init : NULL);
}

static void compare(harness *hh, const memref_in *in, uint32_t cycle) {
    const memref_cfg *cfg = &hh->dd.cs.cfg;
    uint64_t out = read_port(hh->sim, "RD_DATA");
    for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
        memref_val want = memref_read(&hh->model, in, rport);
        uint64_t got = (out >> (rport * cfg->width)) & (((uint64_t)1 << cfg->width) - 1U);
        char msg[MSG_LEN * 2];
        (void)snprintf(msg, sizeof msg, "%s: cycle %u read port %u: got %llx want %llx (def %llx)",
                       hh->msg, cycle, rport, (unsigned long long)got,
                       (unsigned long long)want.value, (unsigned long long)want.defined);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(want.value & want.defined, got & want.defined, msg);
    }
}

/* Lowers the case and starts the simulator and the model; checks the idle outputs. */
static void harness_start(harness *hh, const mcase *cs, const char *args) {
    md_lowered(&hh->dd, cs, args);
    describe(cs, hh->msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_sim_build(hh->dd.fix.design, odin3_module_id_of(hh->dd.mod), &hh->sim),
        hh->msg);
    model_start(cs, &hh->model);
    memref_in idle;
    memset(&idle, 0, sizeof idle);
    compare(hh, &idle, 0);
}

static void harness_end(harness *hh) {
    odin3_sim_destroy(hh->sim);
    lf_end(&hh->dd.fix);
}

static uint64_t full_mask(const memref_cfg *cfg) {
    return ((uint64_t)1 << cfg->width) - 1U;
}

/* Writes that all enable bits equally when the case wires its enables as one net, constant 1 or
 * constant 0 (the simulator drives the shared input; a constant ignores the input). */
static void adapt_enables(const mcase *cs, memref_in *in) {
    for (uint32_t wport = 0; wport < cs->cfg.n_wr; wport++) {
        uint64_t full = full_mask(&cs->cfg);
        if (cs->en[wport] == EN_SHARED) {
            in->wr_en[wport] = (in->wr_en[wport] & 1U) != 0 ? full : 0U;
        } else if (cs->en[wport] == EN_ONE) {
            in->wr_en[wport] = full;
        } else if (cs->en[wport] == EN_ZERO) {
            in->wr_en[wport] = 0;
        }
    }
}

static uint32_t g_cycle;

static void harness_cycle(harness *hh, const memref_in *raw) {
    memref_in in = *raw;
    adapt_enables(&hh->dd.cs, &in);
    drive_inputs(hh->sim, &hh->dd.cs.cfg, &in);
    odin3_sim_cycle(hh->sim);
    memref_step(&hh->model, &in);
    compare(hh, &in, ++g_cycle);
}

/* --- scripts ------------------------------------------------------------------------------ */

/* Write every address of the space, then read every address (async and sync ports alike). */
static void script_write_then_read(harness *hh) {
    const memref_cfg *cfg = &hh->dd.cs.cfg;
    uint64_t space = (uint64_t)1 << cfg->abits;
    for (uint64_t addr = 0; addr < space && cfg->n_wr > 0; addr++) {
        memref_in in;
        memset(&in, 0, sizeof in);
        in.wr_addr[0] = addr;
        in.wr_en[0] = full_mask(cfg);
        in.wr_data[0] = (addr * 5U + 3U) & full_mask(cfg);
        harness_cycle(hh, &in);
    }
    for (uint64_t addr = 0; addr < space; addr++) {
        memref_in in;
        memset(&in, 0, sizeof in);
        for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
            in.rd_addr[rport] = addr;
            in.rd_en[rport] = true;
        }
        harness_cycle(hh, &in);
    }
}

/* Two ports write the same address in one cycle: the later one wins. */
static void script_priority(harness *hh) {
    const memref_cfg *cfg = &hh->dd.cs.cfg;
    for (uint64_t addr = 0; addr < ((uint64_t)1 << cfg->abits) && cfg->n_wr > 1; addr++) {
        memref_in in;
        memset(&in, 0, sizeof in);
        for (uint32_t wport = 0; wport < cfg->n_wr; wport++) {
            in.wr_addr[wport] = addr;
            in.wr_en[wport] = full_mask(cfg);
            in.wr_data[wport] = (addr + (uint64_t)wport * 3U + 1U) & full_mask(cfg);
        }
        for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
            in.rd_addr[rport] = addr;
            in.rd_en[rport] = true;
        }
        harness_cycle(hh, &in);
        memset(in.wr_en, 0, sizeof in.wr_en);
        harness_cycle(hh, &in);
    }
}

static uint64_t draw(odin3_prng *prng, uint64_t bound) {
    return odin3_prng_next(prng) % bound;
}

/* A write enable mask: none, a random one, or all bits. */
static uint64_t random_enable(odin3_prng *prng, const memref_cfg *cfg, uint64_t mode) {
    if (mode == 0) {
        return 0U;
    }
    return mode == 1 ? draw(prng, full_mask(cfg) + 1U) : full_mask(cfg);
}

static void random_inputs(odin3_prng *prng, const memref_cfg *cfg, memref_in *in) {
    uint64_t space = (uint64_t)1 << cfg->abits;
    memset(in, 0, sizeof *in);
    for (uint32_t rport = 0; rport < cfg->n_rd; rport++) {
        in->rd_addr[rport] = draw(prng, space);
        in->rd_en[rport] = draw(prng, 4) != 0;
    }
    for (uint32_t wport = 0; wport < cfg->n_wr; wport++) {
        uint64_t mode = draw(prng, 4);
        in->wr_addr[wport] =
            draw(prng, 3) == 0 && cfg->n_rd > 0 ? in->rd_addr[0] : draw(prng, space);
        in->wr_en[wport] = random_enable(prng, cfg, mode);
        in->wr_data[wport] = draw(prng, full_mask(cfg) + 1U);
    }
}

static void script_random(harness *hh, uint64_t seed) {
    odin3_prng prng;
    odin3_prng_seed(&prng, seed);
    for (uint32_t step = 0; step < RANDOM_CYCLES; step++) {
        memref_in in;
        random_inputs(&prng, &hh->dd.cs.cfg, &in);
        harness_cycle(hh, &in);
    }
}

typedef void (*script_fn)(harness *hh);

static void run_script(const mcase *cs, script_fn script, const char *args) {
    harness hh;
    g_cycle = 0;
    harness_start(&hh, cs, args);
    script(&hh);
    harness_end(&hh);
}

static void run_random(const mcase *cs, uint64_t seed, const char *args) {
    harness hh;
    g_cycle = 0;
    harness_start(&hh, cs, args);
    script_random(&hh, seed);
    harness_end(&hh);
}

/* --- the case table ------------------------------------------------------------------------- */

#define SYNC true
#define ASYNC false

static const mcase k_cases[] = {
    {{4, 3, 2, 1, 1, 0, {ASYNC}, {false}}, "plain async", false, true, false, false, {EN_INPUTS}},
    {{4, 3, 2, 1, 1, 0, {SYNC}, {false}}, "plain sync", false, true, false, false, {EN_INPUTS}},
    {{5, 2, 3, 2, 2, 1, {ASYNC, SYNC}, {false, false}},
     "offset 1, size 5, two writes",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{6, 4, 3, 1, 1, -2, {SYNC}, {false}},
     "offset -2, byte enables",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{8, 2, 2, 1, 1, 0, {ASYNC}, {false}},
     "abits below clog2(size)",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{3, 2, 4, 1, 1, 0, {SYNC}, {true}},
     "abits above clog2(size), transparent",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{3, 2, 4, 1, 2, 0, {SYNC}, {true}},
     "abits above, two writes, transparent",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{16, 1, 4, 2, 2, 0, {ASYNC, SYNC}, {false, true}},
     "16x1, async + transparent",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{1, 5, 0, 1, 1, 0, {ASYNC}, {false}},
     "one word, no address",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{2, 2, 1, 3, 1, -1, {ASYNC, SYNC, SYNC}, {false, false, true}},
     "three read ports, offset -1",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{7, 3, 4, 1, 1, 13, {ASYNC}, {false}},
     "addresses wrap (offset 13, abits 4)",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{7, 3, 4, 2, 1, 13, {SYNC, SYNC}, {false, true}},
     "wrapping, sync",
     false,
     true,
     false,
     false,
     {EN_INPUTS}},
    {{5, 3, 3, 2, 2, 0, {ASYNC, SYNC}, {false, true}},
     "init, two writes, transparent",
     true,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{4, 2, 2, 2, 2, 0, {ASYNC, SYNC}, {false, true}},
     "shared enable nets",
     false,
     true,
     false,
     false,
     {EN_SHARED, EN_SHARED}},
    {{4, 2, 2, 1, 2, 0, {SYNC}, {true}},
     "constant enables: 1 and 0",
     false,
     true,
     false,
     false,
     {EN_ONE, EN_ZERO}},
    {{4, 3, 2, 1, 1, 0, {SYNC}, {false}},
     "separate clocks per port",
     false,
     false,
     false,
     false,
     {EN_INPUTS}},
    {{16, 1, 4, 1, 2, 0, {SYNC}, {true}},
     "one sync transparent read, two writes",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{16, 1, 4, 2, 1, 0, {ASYNC, SYNC}, {false, true}},
     "async + transparent reads, one write",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{4, 1, 2, 2, 2, 0, {ASYNC, SYNC}, {false, true}},
     "async + transparent reads, two writes, 1 bit",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
    {{4, 2, 2, 2, 2, 0, {ASYNC, SYNC}, {false, true}},
     "async + transparent reads, two writes, 2 bits",
     false,
     true,
     false,
     false,
     {EN_INPUTS, EN_INPUTS}},
};

enum { N_CASES = (int)(sizeof k_cases / sizeof k_cases[0]) };

static void test_write_all_addresses_then_read_all(void) {
    for (int i = 0; i < N_CASES; i++) {
        run_script(&k_cases[i], script_write_then_read, "");
    }
}

static void test_two_write_ports_the_later_one_wins(void) {
    for (int i = 0; i < N_CASES; i++) {
        if (k_cases[i].cfg.n_wr > 1) {
            run_script(&k_cases[i], script_priority, "");
        }
    }
}

static void test_random_cycles_follow_the_model(void) {
    for (int i = 0; i < N_CASES; i++) {
        run_random(&k_cases[i], 1000U + (uint64_t)i, "");
    }
}

static void test_init_contents_are_read_before_any_write(void) {
    for (int i = 0; i < N_CASES; i++) {
        mcase cs = k_cases[i];
        cs.use_init = true;
        run_script(&cs, script_write_then_read, "");
        run_random(&cs, 2000U + (uint64_t)i, "");
    }
}

static void test_falling_edge_with_keep_is_a_dff_n(void) {
    mcase cs = k_cases[2];
    cs.falling = true;
    cs.name = "falling edge, keep";
    run_random(&cs, 77, "--neg-edge keep");
    md dd;
    md_lowered(&dd, &cs, "--neg-edge keep");
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&dd.fix, dd.mod, "$_DFF_P_"));
    TEST_ASSERT_TRUE(lf_count_type(&dd.fix, dd.mod, "$_DFF_N_") > 0);
    TEST_ASSERT_NULL(strstr(log_text, "clock inverter"));
    lf_end(&dd.fix);
}

static void test_falling_edge_by_default_gets_one_shared_inverter_and_a_warning(void) {
    mcase cs = k_cases[2];
    cs.falling = true;
    md dd;
    md_lowered(&dd, &cs, "--report test_lower_mem.report.json");
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&dd.fix, dd.mod, "$_DFF_N_"));
    TEST_ASSERT_TRUE(lf_count_type(&dd.fix, dd.mod, "$_DFF_P_") > 0);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "falling-edge '$mem' 'dut' gets a clock inverter"));
    lf_end(&dd.fix);
}

static const mcase k_small = {
    {4, 2, 2, 1, 1, 0, {ASYNC}, {false}}, "small", false, true, false, false, {EN_SHARED}};

/* --- transparent reads (PHASE3 #40) ------------------------------------------------------- */

/* One sync read port transparent to one write port, all on one clock net and edge. */
static mcase transparent_case(void) {
    mcase cs = k_small;
    cs.cfg.rd_sync[0] = SYNC;
    cs.cfg.rd_trans[0] = true;
    cs.name = "transparent";
    return cs;
}

static void test_a_transparent_read_warns_with_the_memory_and_port(void) {
    mcase cs = transparent_case();
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(log_text, "lower: module 'm': memory 'dut' read port 0 is transparent; lowered "
                         "with a write-to-read bypass (Parmys reads the old value: PHASE3 #40)"),
        log_text);
    lf_end(&dd.fix);
    cs.cfg.rd_trans[0] = false;
    log_text[0] = '\0';
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_NULL(strstr(log_text, "is transparent"));
    lf_end(&dd.fix);
}

/* The flip-flops and muxes of the cell as lowered; `cs` with the transparent flag as given. */
typedef struct counts {
    uint32_t dffs;
    uint32_t muxes;
} counts;

static counts lowered_counts(const mcase *cs) {
    md dd;
    md_lowered(&dd, cs, "--neg-edge keep");
    counts cc = {lf_count_type(&dd.fix, dd.mod, "$_DFF_P_") +
                     lf_count_type(&dd.fix, dd.mod, "$_DFF_N_"),
                 lf_count_type(&dd.fix, dd.mod, "$_MUX_")};
    lf_end(&dd.fix);
    return cc;
}

/* A bypass adds registers and muxes; no bypass leaves the counts of the read-first lowering. */
static void expect_bypass(const mcase *cs, bool bypass) {
    mcase plain = *cs;
    plain.cfg.rd_trans[0] = false;
    counts base = lowered_counts(&plain);
    counts with = lowered_counts(cs);
    if (bypass) {
        TEST_ASSERT_TRUE(with.dffs > base.dffs);
        TEST_ASSERT_TRUE(with.muxes > base.muxes);
    } else {
        TEST_ASSERT_EQUAL_UINT32(base.dffs, with.dffs);
        TEST_ASSERT_EQUAL_UINT32(base.muxes, with.muxes);
    }
}

static void test_a_transparent_read_on_the_write_clock_gets_a_bypass(void) {
    mcase cs = transparent_case();
    expect_bypass(&cs, true);
}

static void test_a_transparent_read_on_the_other_edge_gets_no_bypass(void) {
    mcase cs = transparent_case();
    g_rd_flip = 1U;
    expect_bypass(&cs, false);
    g_rd_flip = 0;
    g_wr_flip = 1U;
    expect_bypass(&cs, false);
}

static void test_a_transparent_read_on_another_clock_net_gets_no_bypass(void) {
    mcase cs = transparent_case();
    cs.share_clock = false;
    expect_bypass(&cs, false);
}

static void test_a_transparent_read_bypasses_only_the_writes_on_its_clock(void) {
    mcase cs = k_cases[2]; /* two write ports on one net; read port 1 is sync */
    cs.cfg.rd_trans[1] = true;
    cs.cfg.rd_trans[0] = false;
    counts one = lowered_counts(&cs);
    cs.cfg.rd_trans[1] = false;
    counts none = lowered_counts(&cs);
    TEST_ASSERT_TRUE(one.dffs > none.dffs);
    cs.cfg.rd_trans[1] = true;
    g_rd_flip = 2U; /* read port 1 on the other edge: no write port is seen */
    counts flipped = lowered_counts(&cs);
    TEST_ASSERT_EQUAL_UINT32(none.dffs, flipped.dffs);
    TEST_ASSERT_EQUAL_UINT32(none.muxes, flipped.muxes);
}

static void test_a_read_past_size_is_noted(void) {
    mcase cs = k_cases[2]; /* size 5 in a 3-bit address space */
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_NOT_NULL(strstr(log_text, "memory 'dut' reads of addresses past SIZE 5 give x"));
    lf_end(&dd.fix);
    log_text[0] = '\0';
    md_lowered(&dd, &k_small, "");
    TEST_ASSERT_NULL(strstr(log_text, "past SIZE"));
    lf_end(&dd.fix);
}

/* --- structure ------------------------------------------------------------------------------ */

static void test_structure_of_a_small_memory(void) {
    md dd;
    md_lowered(&dd, &k_small, "");
    /* 8 flip-flops; per word one AND (select & enable) and 2 write muxes; the decoder: 2 NOT and
     * 4 AND for the four words; the read tree: 3 muxes a bit. */
    TEST_ASSERT_EQUAL_UINT32(8, lf_count_type(&dd.fix, dd.mod, "$_DFF_P_"));
    TEST_ASSERT_EQUAL_UINT32(8 + 6, lf_count_type(&dd.fix, dd.mod, "$_MUX_"));
    TEST_ASSERT_EQUAL_UINT32(4 + 4, lf_count_type(&dd.fix, dd.mod, "$_AND_"));
    TEST_ASSERT_EQUAL_UINT32(2, lf_count_type(&dd.fix, dd.mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_UINT32(8 + 14 + 8 + 2, lf_gate_nodes(&dd.fix, dd.mod));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "lower: module 'm': '$mem' 'dut' lowered to 8 flip-flops"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: lowered 1 $mem as soft logic"));
    lf_end(&dd.fix);
}

static void test_a_sync_read_adds_a_register_and_an_enable_mux_per_bit(void) {
    mcase cs = k_small;
    cs.cfg.rd_sync[0] = SYNC;
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_EQUAL_UINT32(8 + 2, lf_count_type(&dd.fix, dd.mod, "$_DFF_P_"));
    TEST_ASSERT_EQUAL_UINT32(8 + 6 + 2, lf_count_type(&dd.fix, dd.mod, "$_MUX_"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lowered to 10 flip-flops"));
    lf_end(&dd.fix);
}

static void test_a_rom_has_no_flip_flops_and_reads_its_init(void) {
    mcase cs = k_cases[0];
    cs.cfg.n_wr = 0;
    cs.use_init = true;
    harness hh;
    g_cycle = 0;
    harness_start(&hh, &cs, "");
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&hh.dd.fix, hh.dd.mod, "$_DFF_P_"));
    script_write_then_read(&hh);
    harness_end(&hh);
}

static void test_a_mem_with_no_read_port_is_deleted(void) {
    mcase cs = k_cases[0];
    cs.cfg.n_rd = 0;
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_EQUAL_UINT32(0, lf_gate_nodes(&dd.fix, dd.mod));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "'$mem' 'dut' lowered to 0 flip-flops"));
    lf_end(&dd.fix);
}

static void test_a_write_port_with_constant_zero_enable_is_skipped(void) {
    mcase two = k_small;
    two.cfg.n_wr = 2;
    two.en[1] = EN_ZERO;
    mcase one = k_small;
    md two_ports;
    md one_port;
    md_lowered(&two_ports, &two, "");
    md_lowered(&one_port, &one, "");
    TEST_ASSERT_EQUAL_UINT32(lf_gate_nodes(&one_port.fix, one_port.mod),
                             lf_gate_nodes(&two_ports.fix, two_ports.mod));
    lf_end(&two_ports.fix);
    lf_end(&one_port.fix);
}

static void test_a_constant_one_enable_needs_no_and_per_word(void) {
    mcase cs = k_small;
    cs.en[0] = EN_ONE;
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_EQUAL_UINT32(4, lf_count_type(&dd.fix, dd.mod, "$_AND_")); /* the decoder only */
    lf_end(&dd.fix);
}

static void test_per_bit_enables_get_an_and_per_bit(void) {
    mcase cs = k_small;
    cs.en[0] = EN_INPUTS;
    md dd;
    md_lowered(&dd, &cs, "");
    TEST_ASSERT_EQUAL_UINT32(8 + 4, lf_count_type(&dd.fix, dd.mod, "$_AND_"));
    lf_end(&dd.fix);
}

static void test_init_bits_go_to_the_flip_flops(void) {
    mcase cs = k_small;
    cs.use_init = true;
    md dd;
    md_lowered(&dd, &cs, "");
    uint32_t seen[4] = {0, 0, 0, 0};
    for (uint32_t id = 1; id < odin3_module_node_end(dd.mod); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_node_live(dd.mod, node)
                ? odin3_celltype_get(dd.fix.design, odin3_node_type(dd.mod, node))
                : NULL;
        if (def != NULL && strcmp(def->name, "$_DFF_P_") == 0) {
            seen[odin3_node_param(dd.mod, node, 0)->i]++;
        }
    }
    uint32_t want[4] = {0, 0, 0, 0};
    for (uint32_t idx = 0; idx < 8; idx++) {
        uint8_t pat = init_pattern(idx / 2, idx % 2);
        want[pat == 2 ? 3 : pat]++;
    }
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], seen[i]);
    }
    lf_end(&dd.fix);
}

/* --- estimate, errors, caps, report --------------------------------------------------------- */

static uint32_t port_width(const memref_cfg *cfg, uint32_t port) {
    const uint32_t widths[8] = {cfg->n_rd,
                                cfg->n_rd,
                                cfg->n_rd * cfg->abits,
                                cfg->n_rd * cfg->width,
                                cfg->n_wr,
                                cfg->n_wr * cfg->width,
                                cfg->n_wr * cfg->abits,
                                cfg->n_wr * cfg->width};
    return widths[port];
}

static void check_estimate(const mcase *cs) {
    md dd;
    md_lowered(&dd, cs, "");
    uint32_t widths[8];
    for (uint32_t port = 0; port < 8; port++) {
        widths[port] = port_width(&cs->cfg, port);
    }
    uint64_t estimate = lf_estimate("$mem", dd.pp.v, widths, 8);
    uint32_t gates = lf_gate_nodes(&dd.fix, dd.mod);
    char msg[MSG_LEN];
    describe(cs, msg);
    TEST_ASSERT_TRUE_MESSAGE(gates <= estimate, msg);
    lf_end(&dd.fix);
}

static void test_the_estimate_bounds_the_gates(void) {
    for (int i = 0; i < N_CASES; i++) {
        check_estimate(&k_cases[i]);
    }
    mcase big = {{33, 5, 6, 2, 3, 0, {SYNC, SYNC}, {true, true}},
                 "33x5, three writes",
                 true,
                 true,
                 false,
                 false,
                 {EN_INPUTS, EN_INPUTS, EN_INPUTS}};
    check_estimate(&big);
    big.cfg.offset = -9;
    big.cfg.size = 40;
    check_estimate(&big);
}

/* The lowering must refuse `cs` with `text` in the log, the design intact. */
typedef struct refusal {
    const char *args;
    const char *text;
} refusal;

static void expect_refused(const mcase *cs, refusal why) {
    md dd;
    md_make(&dd, cs);
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, md_lower(&dd, why.args));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, why.text), log_text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&dd.fix);
}

static void test_an_asynchronous_write_port_is_an_error(void) {
    mcase cs = k_cases[0];
    cs.async_write = true;
    expect_refused(&cs,
                   (refusal){"", "lower: module 'm': '$mem' 'dut' has an asynchronous write port"});
}

static void test_write_ports_on_different_clocks_are_an_error(void) {
    mcase cs = k_cases[2];
    cs.share_clock = false;
    expect_refused(&cs,
                   (refusal){"", "lower: module 'm': '$mem' 'dut' has write ports on different "
                                 "clocks or polarities"});
}

static void test_write_ports_on_one_net_with_opposite_edges_are_an_error(void) {
    mcase cs = k_cases[2]; /* one clock net, two write ports */
    g_wr_flip = 2U;
    expect_refused(&cs,
                   (refusal){"", "lower: module 'm': '$mem' 'dut' has write ports on different "
                                 "clocks or polarities"});
}

static void test_the_bit_cap_is_an_error_with_nothing_built(void) {
    mcase cs = k_cases[0]; /* 4 words x 3 bits = 12 bits */
    expect_refused(&cs, (refusal){"--max-mem-bits 11",
                                  "lower: module 'm': '$mem' 'dut' has 12 bits (cap 11); hardmap "
                                  "it or raise --max-mem-bits"});
    md dd;
    md_lowered(&dd, &cs, "--max-mem-bits 12");
    lf_end(&dd.fix);
}

static void test_an_address_wider_than_64_bits_is_an_error(void) {
    mcase cs = k_cases[0];
    cs.cfg.abits = 65;
    expect_refused(&cs, (refusal){"", "lower: module 'm': '$mem' 'dut' has ABITS 65 (above 64)"});
}

static void test_the_default_bit_cap_comes_from_the_limits(void) {
    odin3_lower_limits limits = {0, 0, 8};
    odin3_lower_test_set_limits(&limits);
    expect_refused(&k_cases[0], (refusal){"", "has 12 bits (cap 8)"});
}

static void test_a_constant_zero_port_does_not_trip_the_async_error(void) {
    mcase cs = k_cases[0];
    cs.async_write = true;
    cs.en[0] = EN_ZERO;
    md dd;
    md_lowered(&dd, &cs, "");
    lf_end(&dd.fix);
}

static void test_a_mem_with_no_read_port_ignores_the_errors(void) {
    mcase cs = k_cases[0];
    cs.cfg.n_rd = 0;
    cs.async_write = true;
    md dd;
    md_lowered(&dd, &cs, "--max-mem-bits 1");
    lf_end(&dd.fix);
}

static void test_the_mem_error_comes_before_any_other_cell_is_lowered(void) {
    mcase cs = k_cases[0];
    md dd;
    md_make(&dd, &cs);
    odin3_value params[5];
    lf_binary spec = {false, false, 2, 2, 2};
    lf_binary_params(&spec, params);
    lf_cell_spec other = {"$and", params, 5, "other", {0}};
    (void)lf_cell_prefixed(&dd.fix, dd.mod, &other, "o_");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, md_lower(&dd, "--max-mem-bits 1"));
    TEST_ASSERT_EQUAL_UINT32(2, lf_word_nodes(&dd.fix, dd.mod));
    lf_end(&dd.fix);
}

static void test_the_report_lists_the_mem(void) {
    md dd;
    md_lowered(&dd, &k_small, "--report test_lower_mem.report.json");
    FILE *file = fopen("test_lower_mem.report.json", "rport");
    TEST_ASSERT_NOT_NULL(file);
    char text[1024];
    size_t got = fread(text, 1, sizeof text - 1, file);
    (void)fclose(file);
    text[got] = '\0';
    TEST_ASSERT_NOT_NULL(
        strstr(text, "{\"module\": \"m\", \"cell\": \"dut\", \"type\": \"$mem\", \"bits\": 8}"));
    lf_end(&dd.fix);
}

static void test_standalone_memrd_and_memwr_are_errors(void) {
    lf fix;
    lf_begin(&fix);
    odin3_module *mod = lf_module(&fix, "m");
    lf_cell_spec rd = {"$memrd", NULL, 0, "rport", {0}};
    (void)lf_cell(&fix, mod, &rd);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(fix.design, "lower", odin3_bytes_cstr("")));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: standalone $memrd/$memwr 'rport' (use $mem)"));
    lf_end(&fix);
}

/* --- OOM -------------------------------------------------------------------------------------- */

static void oom_sweep(const mcase *cs) {
    bool done = false;
    for (long step = 0; !done && step < 6000; step++) {
        md dd;
        md_make(&dd, cs);
        odin3_strbuf before;
        odin3_strbuf after;
        odin3_strbuf_init(&before);
        odin3_strbuf_init(&after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &before));
        odin3_util_set_alloc_fail_after(step);
        odin3_status st = md_lower(&dd, "");
        odin3_util_set_alloc_fail_after(-1);
        odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(dd.mod, opts));
        done = st == ODIN3_OK;
        if (!done && odin3_node_live(dd.mod, dd.node)) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &after));
            TEST_ASSERT_EQUAL_STRING(before.data, after.data);
        }
        odin3_strbuf_free(&before);
        odin3_strbuf_free(&after);
        lf_end(&dd.fix);
    }
    TEST_ASSERT_TRUE(done);
}

static void test_oom_at_every_allocation_leaves_a_valid_design(void) {
    mcase cs = {{3, 2, 2, 2, 2, 1, {ASYNC, SYNC}, {false, true}},
                "oom",
                true,
                true,
                false,
                false,
                {EN_INPUTS, EN_SHARED}};
    oom_sweep(&cs);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_write_all_addresses_then_read_all);
    RUN_TEST(test_two_write_ports_the_later_one_wins);
    RUN_TEST(test_random_cycles_follow_the_model);
    RUN_TEST(test_init_contents_are_read_before_any_write);
    RUN_TEST(test_falling_edge_with_keep_is_a_dff_n);
    RUN_TEST(test_falling_edge_by_default_gets_one_shared_inverter_and_a_warning);
    RUN_TEST(test_a_transparent_read_warns_with_the_memory_and_port);
    RUN_TEST(test_a_transparent_read_on_the_write_clock_gets_a_bypass);
    RUN_TEST(test_a_transparent_read_on_the_other_edge_gets_no_bypass);
    RUN_TEST(test_a_transparent_read_on_another_clock_net_gets_no_bypass);
    RUN_TEST(test_a_transparent_read_bypasses_only_the_writes_on_its_clock);
    RUN_TEST(test_a_read_past_size_is_noted);
    RUN_TEST(test_structure_of_a_small_memory);
    RUN_TEST(test_a_sync_read_adds_a_register_and_an_enable_mux_per_bit);
    RUN_TEST(test_a_rom_has_no_flip_flops_and_reads_its_init);
    RUN_TEST(test_a_mem_with_no_read_port_is_deleted);
    RUN_TEST(test_a_write_port_with_constant_zero_enable_is_skipped);
    RUN_TEST(test_a_constant_one_enable_needs_no_and_per_word);
    RUN_TEST(test_per_bit_enables_get_an_and_per_bit);
    RUN_TEST(test_init_bits_go_to_the_flip_flops);
    RUN_TEST(test_the_estimate_bounds_the_gates);
    RUN_TEST(test_an_asynchronous_write_port_is_an_error);
    RUN_TEST(test_write_ports_on_different_clocks_are_an_error);
    RUN_TEST(test_write_ports_on_one_net_with_opposite_edges_are_an_error);
    RUN_TEST(test_the_bit_cap_is_an_error_with_nothing_built);
    RUN_TEST(test_an_address_wider_than_64_bits_is_an_error);
    RUN_TEST(test_the_default_bit_cap_comes_from_the_limits);
    RUN_TEST(test_a_constant_zero_port_does_not_trip_the_async_error);
    RUN_TEST(test_a_mem_with_no_read_port_ignores_the_errors);
    RUN_TEST(test_the_mem_error_comes_before_any_other_cell_is_lowered);
    RUN_TEST(test_the_report_lists_the_mem);
    RUN_TEST(test_standalone_memrd_and_memwr_are_errors);
    RUN_TEST(test_oom_at_every_allocation_leaves_a_valid_design);
    return UNITY_END();
}
