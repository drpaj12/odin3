/*
 * test_sim_cycle.c — the cycle engine: clock edges, settles, INIT, the port API, the PRNG.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 4096, MAX_BITS = 8, N_CYCLES = 64, BIG = 100000 };

static odin3_design *design;
static char log_text[LOG_TEXT];

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- IR construction -------------------------------------------------------------------------- */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, &mid));
    return odin3_module_get(design, mid);
}

/* A port as wide as spec says; nets gets its bit nets (LSB first). */
static void add_bus(odin3_module *mod, const odin3_port_spec *spec, odin3_net_id *nets) {
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, spec, &node));
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        nets[k] = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k});
    }
}

static odin3_net_id in_bus(odin3_module *mod, const char *name, uint32_t width,
                           odin3_net_id *nets) {
    odin3_port_spec spec = {intern(name), ODIN3_DIR_IN, width, width == 1, (odin3_prov_id){0}};
    add_bus(mod, &spec, nets);
    return nets[0];
}

static odin3_net_id out_bus(odin3_module *mod, const char *name, uint32_t width,
                            odin3_net_id *nets) {
    odin3_port_spec spec = {intern(name), ODIN3_DIR_OUT, width, width == 1, (odin3_prov_id){0}};
    add_bus(mod, &spec, nets);
    return nets[0];
}

static odin3_net_id input(odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    return in_bus(mod, name, 1, &net);
}

static odin3_net_id output(odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    return out_bus(mod, name, 1, &net);
}

static odin3_net_id net_prov(odin3_module *mod, const char *name, odin3_prov_id prov) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, name ? intern(name) : 0, prov, &id));
    return id;
}

static odin3_net_id net(odin3_module *mod, const char *name) {
    return net_prov(mod, name, (odin3_prov_id){0});
}

/* A node of type with one-bit ports on nets (one per port) and the given parameters. */
static odin3_node_id cell_params(odin3_module *mod, odin3_celltype_id type,
                                 const odin3_net_id *nets, const odin3_value *params) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    odin3_netvec ports[MAX_BITS];
    for (uint32_t i = 0; i < def->n_ports; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, params, params ? def->n_params : 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

static odin3_node_id cell(odin3_module *mod, const char *type, const odin3_net_id *nets) {
    return cell_params(mod, type_id(type), nets, NULL);
}

/* A storage cell with INIT init. */
static void storage(odin3_module *mod, const char *type, const odin3_net_id *nets, int64_t init) {
    const odin3_value params[1] = {odin3_value_int(init)};
    cell_params(mod, type_id(type), nets, params);
}

static void inst(odin3_module *mod, const odin3_module *child, const odin3_net_id *nets) {
    cell(mod, odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(child)), nets);
}

static odin3_sim *build_ok(const odin3_module *top) {
    odin3_sim *sim = NULL;
    odin3_status st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL(sim);
    return sim;
}

static void build_fails(const odin3_module *top, const char *needle) {
    odin3_sim *sim = (odin3_sim *)&sim; /* must be reset to NULL */
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_build(design, odin3_module_id_of(top), &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
}

/* --- driving and sampling whole ports --------------------------------------------------------- */

/* The index of input port name. */
static uint32_t input_port(const odin3_sim *sim, const char *name) {
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        if (strcmp(odin3_sim_input_name(sim, port), name) == 0) {
            return port;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

/* Drives every bit of input port name from the bits of value. */
static void set_in(odin3_sim *sim, const char *name, uint32_t value) {
    uint32_t port = input_port(sim, name);
    for (uint32_t k = 0; k < odin3_sim_input_width(sim, port); k++) {
        odin3_sim_bit at = {port, k};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(sim, at, ((value >> k) & 1U) != 0));
    }
}

static uint32_t get_out(const odin3_sim *sim, uint32_t port) {
    uint32_t value = 0;
    for (uint32_t k = 0; k < odin3_sim_output_width(sim, port); k++) {
        bool bit = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){port, k}, &bit));
        value |= (bit ? 1U : 0U) << k;
    }
    return value;
}

static uint32_t get_in(const odin3_sim *sim, uint32_t port) {
    uint32_t value = 0;
    for (uint32_t k = 0; k < odin3_sim_input_width(sim, port); k++) {
        bool bit = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_input(sim, (odin3_sim_bit){port, k}, &bit));
        value |= (bit ? 1U : 0U) << k;
    }
    return value;
}

/* --- PRNG ------------------------------------------------------------------------------------- */

/* Known answers (an independent Python model of splitmix64 seeding + xorshift64*). */
static void test_prng_known_answers(void) {
    static const uint64_t k_seed1[3] = {0x4b46a55df3611b9bULL, 0xd7e1f1410e763ef4ULL,
                                        0x5f14ec66975f9b06ULL};
    static const uint64_t k_seed42[3] = {0x31b0ece7c4f697a2ULL, 0x9008a3b1cb686f03ULL,
                                         0x7c7173abd97be16fULL};
    odin3_prng one;
    odin3_prng other;
    odin3_prng_seed(&one, 1);
    odin3_prng_seed(&other, 42);
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX64(k_seed1[i], odin3_prng_next(&one));
        TEST_ASSERT_EQUAL_HEX64(k_seed42[i], odin3_prng_next(&other));
    }
    odin3_prng zero;
    odin3_prng_seed(&zero, 0);
    TEST_ASSERT_EQUAL_HEX64(0x7bbcb40d550682d0ULL, odin3_prng_next(&zero));
    /* bits are the top bits of the draws, and roughly balanced */
    odin3_prng_seed(&one, 1);
    TEST_ASSERT_FALSE(odin3_prng_bit(&one)); /* 0x4b... */
    TEST_ASSERT_TRUE(odin3_prng_bit(&one));  /* 0xd7... */
    uint32_t ones = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        ones += odin3_prng_bit(&one) ? 1U : 0U;
    }
    TEST_ASSERT_UINT32_WITHIN(100, 500, ones);
}

/* --- Review Focus 1: opposite edges within one cycle ------------------------------------------ */

/*
 * P then N: qpn = N(P(d)); N then P: qnp = P(N(d)). The rising edge comes first, so a value goes
 * through P then N in one cycle, but through N then P only in the next.
 */
static void test_dff_p_and_n_update_on_opposite_edges(void) {
    odin3_module *top = new_module("top");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id d_in = input(top, "d");
    odin3_net_id qp = output(top, "qp");
    odin3_net_id qpn = output(top, "qpn");
    odin3_net_id qn = output(top, "qn");
    odin3_net_id qnp = output(top, "qnp");
    cell(top, "$_DFF_N_", (odin3_net_id[]){clk, qp, qpn});
    cell(top, "$_DFF_P_", (odin3_net_id[]){clk, d_in, qp});
    cell(top, "$_DFF_N_", (odin3_net_id[]){clk, d_in, qn});
    cell(top, "$_DFF_P_", (odin3_net_id[]){clk, qn, qnp});
    odin3_sim *sim = build_ok(top);
    set_in(sim, "d", 1);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 0)); /* qp: rising edge */
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 1)); /* qpn: the falling edge saw the new qp */
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 2)); /* qn: falling edge */
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 3)); /* qnp: the rising edge saw the old qn */
    set_in(sim, "d", 0);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 2));
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 3));
    TEST_ASSERT_EQUAL_UINT32(0, get_in(sim, 0)); /* the clock is low between cycles */
    odin3_sim_destroy(sim);
}

/* --- a 4-bit counter with enable -------------------------------------------------------------- */

/* count <= count + en, as t0 = en, d_i = q_i ^ t_i, t_{i+1} = t_i & q_i (cells in reverse). */
static odin3_module *make_counter(void) {
    odin3_module *top = new_module("counter");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id carry[4] = {input(top, "en"), net(top, NULL), net(top, NULL), net(top, NULL)};
    odin3_net_id qbits[4];
    out_bus(top, "count", 4, qbits);
    for (uint32_t i = 4; i-- > 0;) {
        odin3_net_id dbit = net(top, NULL);
        cell(top, "$_DFF_P_", (odin3_net_id[]){clk, dbit, qbits[i]});
        cell(top, "$_XOR_", (odin3_net_id[]){qbits[i], carry[i], dbit});
        if (i < 3) {
            cell(top, "$_AND_", (odin3_net_id[]){carry[i], qbits[i], carry[i + 1]});
        }
    }
    return top;
}

static void test_counter_counts_over_20_cycles(void) {
    odin3_sim *sim = build_ok(make_counter());
    uint32_t want = 0;
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 0));
    for (uint32_t cyc = 0; cyc < 20; cyc++) {
        uint32_t en = cyc % 5 != 3 ? 1U : 0U;
        set_in(sim, "en", en);
        odin3_sim_cycle(sim);
        want = (want + en) & 15U;
        TEST_ASSERT_EQUAL_UINT32(want, get_out(sim, 0));
    }
    TEST_ASSERT_EQUAL_UINT32(0, want); /* 16 enabled cycles: it wrapped */
    odin3_sim_destroy(sim);
}

/* The port API on the counter: counts, names, widths. */
static void test_port_names_and_widths(void) {
    odin3_sim *sim = build_ok(make_counter());
    TEST_ASSERT_EQUAL_UINT32(2, odin3_sim_input_count(sim));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_sim_output_count(sim));
    TEST_ASSERT_EQUAL_STRING("clk", odin3_sim_input_name(sim, 0));
    TEST_ASSERT_EQUAL_STRING("en", odin3_sim_input_name(sim, 1));
    TEST_ASSERT_EQUAL_STRING("count", odin3_sim_output_name(sim, 0));
    TEST_ASSERT_NULL(odin3_sim_input_name(sim, 2));
    TEST_ASSERT_NULL(odin3_sim_output_name(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_sim_input_width(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_sim_output_width(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_sim_input_width(sim, 2));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_sim_output_width(sim, 1));
    odin3_sim_destroy(sim);
}

/* The port API on the counter: which input bits are clocks. */
static void test_port_clocks(void) {
    odin3_sim *sim = build_ok(make_counter());
    TEST_ASSERT_EQUAL_INT(1, odin3_sim_input_is_clock(sim, (odin3_sim_bit){0, 0}));
    TEST_ASSERT_EQUAL_INT(0, odin3_sim_input_is_clock(sim, (odin3_sim_bit){1, 0}));
    TEST_ASSERT_EQUAL_INT(0, odin3_sim_input_is_clock(sim, (odin3_sim_bit){0, 1}));
    TEST_ASSERT_EQUAL_INT(0, odin3_sim_input_is_clock(sim, (odin3_sim_bit){2, 0}));
    odin3_sim_destroy(sim);
}

/* A clock bit is never set by the caller (the cycle toggles it). */
static void test_clock_cannot_be_set(void) {
    odin3_sim *sim = build_ok(make_counter());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_set_input(sim, (odin3_sim_bit){0, 0}, true)); /* a clock */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_set_input(sim, (odin3_sim_bit){1, 1}, true));
    odin3_sim_destroy(sim);
}

/* Out-of-range bits fail quietly; the sampling calls leave *value alone. */
static void test_port_range_errors(void) {
    odin3_sim *sim = build_ok(make_counter());
    bool bit = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_set_input(sim, (odin3_sim_bit){2, 0}, true));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_get_output(sim, (odin3_sim_bit){0, 4}, &bit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_get_output(sim, (odin3_sim_bit){1, 0}, &bit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_get_input(sim, (odin3_sim_bit){1, 1}, &bit));
    TEST_ASSERT_TRUE(bit); /* untouched */
    TEST_ASSERT_EQUAL_STRING("", log_text);
    odin3_sim_destroy(sim);
}

/* --- a 2-stage pipeline ----------------------------------------------------------------------- */

/* out = DFF(~DFF(in)), two bits: out after cycle c is ~in of cycle c - 1 (in of cycle 0 is 0). */
static void test_two_stage_pipeline_latency(void) {
    odin3_module *top = new_module("pipe");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id ins[2];
    odin3_net_id outs[2];
    in_bus(top, "in", 2, ins);
    out_bus(top, "out", 2, outs);
    for (uint32_t k = 0; k < 2; k++) {
        odin3_net_id stage1 = net(top, NULL);
        odin3_net_id inv = net(top, NULL);
        cell(top, "$_DFF_P_", (odin3_net_id[]){clk, inv, outs[k]});
        cell(top, "$_NOT_", (odin3_net_id[]){stage1, inv});
        cell(top, "$_DFF_P_", (odin3_net_id[]){clk, ins[k], stage1});
    }
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 0)); /* the second stage's INIT */
    static const uint32_t k_in[] = {1, 2, 3, 0, 2, 1, 1, 3};
    uint32_t prev = 0;
    for (uint32_t cyc = 0; cyc < sizeof k_in / sizeof k_in[0]; cyc++) {
        set_in(sim, "in", k_in[cyc]);
        odin3_sim_cycle(sim);
        TEST_ASSERT_EQUAL_UINT32(~prev & 3U, get_out(sim, 0));
        prev = k_in[cyc];
    }
    odin3_sim_destroy(sim);
}

/* --- level latches ---------------------------------------------------------------------------- */

/* Latches enabled by a non-clock input: transparent while enabled, holding otherwise. */
static void test_level_latch_on_an_input(void) {
    odin3_module *top = new_module("top");
    odin3_net_id en = input(top, "en");
    odin3_net_id d_in = input(top, "d");
    cell(top, "$_DLATCH_P_", (odin3_net_id[]){en, d_in, output(top, "qp")});
    cell(top, "$_DLATCH_N_", (odin3_net_id[]){en, d_in, output(top, "qn")});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_FALSE(odin3_sim_input_is_clock(sim, (odin3_sim_bit){0, 0}));
    static const uint32_t k_steps[][4] = {/* en, d, qp, qn */
                                          {1, 1, 1, 0},
                                          {0, 0, 1, 0},
                                          {0, 1, 1, 1},
                                          {1, 0, 0, 1},
                                          {1, 1, 1, 1}};
    for (uint32_t i = 0; i < sizeof k_steps / sizeof k_steps[0]; i++) {
        set_in(sim, "en", k_steps[i][0]);
        set_in(sim, "d", k_steps[i][1]);
        odin3_sim_cycle(sim);
        TEST_ASSERT_EQUAL_UINT32(k_steps[i][2], get_out(sim, 0));
        TEST_ASSERT_EQUAL_UINT32(k_steps[i][3], get_out(sim, 1));
    }
    odin3_sim_destroy(sim);
}

/*
 * Latches on the clock, fed by a falling-edge flop m: the P latch is transparent while the clock
 * is high and closes before m changes, so it lags m by a cycle; the N latch is open after the
 * falling edge and follows m.
 */
static void test_level_latch_on_the_clock(void) {
    odin3_module *top = new_module("top");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id d_in = input(top, "d");
    odin3_net_id mid = output(top, "m");
    cell(top, "$_DLATCH_P_", (odin3_net_id[]){clk, mid, output(top, "qp")});
    cell(top, "$_DLATCH_N_", (odin3_net_id[]){clk, mid, output(top, "qn")});
    cell(top, "$_DFF_N_", (odin3_net_id[]){clk, d_in, mid});
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_TRUE(odin3_sim_input_is_clock(sim, (odin3_sim_bit){0, 0}));
    set_in(sim, "d", 1);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 2));
    set_in(sim, "d", 0);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 2));
    odin3_sim_destroy(sim);
}

/* --- INIT ------------------------------------------------------------------------------------- */

/* INIT 0, 1, 2, 3 on flops and latches: only INIT 1 starts at 1, visible before any cycle. */
static void test_init_one_and_zero(void) {
    odin3_module *top = new_module("top");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id d_in = input(top, "d");
    odin3_net_id en = input(top, "en");
    odin3_net_id flops[4];
    odin3_net_id latches[4];
    out_bus(top, "f", 4, flops);
    out_bus(top, "l", 4, latches);
    for (int64_t init = 0; init < 4; init++) {
        storage(top, "$_DFF_P_", (odin3_net_id[]){clk, d_in, flops[init]}, init);
        storage(top, "$_DLATCH_P_", (odin3_net_id[]){en, d_in, latches[init]}, init);
    }
    cell(top, "$_FF_", (odin3_net_id[]){d_in, output(top, "g")}); /* default INIT 3 */
    odin3_sim *sim = build_ok(top);
    TEST_ASSERT_EQUAL_UINT32(2, get_out(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(2, get_out(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 2));
    set_in(sim, "d", 1); /* d = 1, latches closed: the flops load 1, the latches hold */
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(15, get_out(sim, 0));
    TEST_ASSERT_EQUAL_UINT32(2, get_out(sim, 1));
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 2)); /* $_FF_: the global clock */
    set_in(sim, "d", 0);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(0, get_out(sim, 0));
    odin3_sim_destroy(sim);
}

/* --- determinism ------------------------------------------------------------------------------ */

/* in a, b, c (+clk); q0 = DFF(a ^ q2), q1 = DFF(q0 & b), q2 = DFF(q1 | c); out q, x = a ^ b. */
static odin3_module *make_mixed(void) {
    odin3_module *top = new_module("mixed");
    odin3_net_id ins[3];
    odin3_net_id qbits[3];
    in_bus(top, "abc", 3, ins);
    odin3_net_id clk = input(top, "clk");
    out_bus(top, "q", 3, qbits);
    odin3_net_id dbits[3] = {net(top, NULL), net(top, NULL), net(top, NULL)};
    cell(top, "$_XOR_", (odin3_net_id[]){ins[0], qbits[2], dbits[0]});
    cell(top, "$_AND_", (odin3_net_id[]){qbits[0], ins[1], dbits[1]});
    cell(top, "$_OR_", (odin3_net_id[]){qbits[1], ins[2], dbits[2]});
    for (uint32_t i = 0; i < 3; i++) {
        cell(top, "$_DFF_P_", (odin3_net_id[]){clk, dbits[i], qbits[i]});
    }
    cell(top, "$_XOR_", (odin3_net_id[]){ins[0], ins[1], output(top, "x")});
    return top;
}

/* Inputs and outputs of N_CYCLES random cycles, packed one byte per cycle each. */
typedef struct trace {
    uint8_t ins[N_CYCLES];
    uint8_t outs[N_CYCLES];
} trace;

static void run_random(odin3_sim *sim, uint64_t seed, trace *out) {
    odin3_prng prng;
    odin3_prng_seed(&prng, seed);
    for (uint32_t cyc = 0; cyc < N_CYCLES; cyc++) {
        odin3_sim_drive_random(sim, &prng);
        TEST_ASSERT_EQUAL_UINT32(0, get_in(sim, 1)); /* the clock is never driven */
        odin3_sim_cycle(sim);
        out->ins[cyc] = (uint8_t)get_in(sim, 0);
        out->outs[cyc] = (uint8_t)(get_out(sim, 0) | get_out(sim, 1) << 3);
    }
}

static void test_same_seed_same_outputs(void) {
    odin3_module *top = make_mixed();
    odin3_sim *one = build_ok(top);
    odin3_sim *two = build_ok(top);
    trace runs[3];
    run_random(one, 7, &runs[0]);
    run_random(two, 7, &runs[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(runs[0].ins, runs[1].ins, N_CYCLES);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(runs[0].outs, runs[1].outs, N_CYCLES);
    odin3_sim_destroy(two);
    two = build_ok(top);
    run_random(two, 8, &runs[2]);
    TEST_ASSERT_TRUE(memcmp(runs[0].ins, runs[2].ins, N_CYCLES) != 0);
    /* one draw per non-clock bit, in port then bit order */
    odin3_prng prng;
    odin3_prng_seed(&prng, 7);
    for (uint32_t cyc = 0; cyc < 2; cyc++) {
        uint32_t want = 0;
        for (uint32_t k = 0; k < 3; k++) {
            want |= (odin3_prng_bit(&prng) ? 1U : 0U) << k;
        }
        TEST_ASSERT_EQUAL_UINT8(want, runs[0].ins[cyc]);
    }
    odin3_sim_destroy(one);
    odin3_sim_destroy(two);
}

/* --- multiple drivers ------------------------------------------------------------------------- */

static odin3_prov_id located(const char *file, uint32_t line) {
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("reader"), &ctx));
    odin3_srcloc loc = {intern(file), line, 1, line, 9};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&ctx, &origin, &prov));
    return prov;
}

static void test_two_cells_driving_a_net_are_rejected(void) {
    odin3_module *top = new_module("top");
    odin3_net_id net_a = input(top, "a");
    odin3_net_id net_o = output(top, "o");
    odin3_net_id clash = net_prov(top, "clash", located("in.blif", 12));
    cell(top, "$_NOT_", (odin3_net_id[]){net_a, clash});
    cell(top, "$_BUF_", (odin3_net_id[]){net_a, clash});
    cell(top, "$_AND_", (odin3_net_id[]){clash, net_a, net_o});
    build_fails(top, "net `clash` has more than one driver");
    TEST_ASSERT_EQUAL_STRING_LEN("in.blif:12: ", log_text, strlen("in.blif:12: "));
}

static void test_input_and_cell_driving_a_net_are_rejected(void) {
    odin3_module *top = new_module("top");
    odin3_net_id net_a = input(top, "a");
    odin3_net_id net_b = input(top, "b");
    cell(top, "$_NOT_", (odin3_net_id[]){net_a, net_b});
    cell(top, "$_BUF_", (odin3_net_id[]){net_b, output(top, "o")});
    build_fails(top, "net `b` has more than one driver");
}

/* An instance output and a parent cell on one parent net: legal per module, not when flat. */
static void test_drivers_across_hierarchy_are_rejected(void) {
    odin3_module *drv = new_module("drv");
    odin3_net_id net_a = input(drv, "a");
    cell(drv, "$_NOT_", (odin3_net_id[]){net_a, output(drv, "y")});
    odin3_module *top = new_module("top");
    odin3_net_id net_i = input(top, "i");
    odin3_net_id net_o = output(top, "o");
    odin3_net_id wired = net(top, "wired");
    inst(top, drv, (odin3_net_id[]){net_i, wired});
    cell(top, "$_BUF_", (odin3_net_id[]){net_i, wired});
    cell(top, "$_BUF_", (odin3_net_id[]){wired, net_o});
    build_fails(top, "net `wired` has more than one driver");
}

/* A test-only tristate bit buffer: Y = A when EN (2-state: nothing when disabled). */
static void tbuf_sim(const odin3_sim_cell *view) {
    if (view->values[view->ports[1].idx[0]] != 0) {
        view->values[view->ports[2].idx[0]] = view->values[view->ports[0].idx[0]];
    }
}

/* Tristate drivers form a bus: several on one net are not a multi-driver error. */
static void test_tristate_drivers_are_allowed(void) {
    static const odin3_port_def k_ports[] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                             {"EN", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                             {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};
    odin3_celltype_def def = {"test_tbuf", ODIN3_GRAN_BIT, ODIN3_CT_TRISTATE, k_ports, 3, NULL, 0,
                              NULL,        NULL,           tbuf_sim,          NULL};
    odin3_celltype_id tbuf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &tbuf));
    odin3_module *top = new_module("top");
    odin3_net_id net_a = input(top, "a");
    odin3_net_id net_b = input(top, "b");
    odin3_net_id sel = input(top, "sel");
    odin3_net_id nsel = net(top, "nsel");
    odin3_net_id bus = output(top, "bus");
    cell(top, "$_NOT_", (odin3_net_id[]){sel, nsel});
    cell_params(top, tbuf, (odin3_net_id[]){net_a, sel, bus}, NULL);
    cell_params(top, tbuf, (odin3_net_id[]){net_b, nsel, bus}, NULL);
    odin3_sim *sim = build_ok(top);
    set_in(sim, "a", 1);
    set_in(sim, "sel", 1);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(1, get_out(sim, 0)); /* a drives the bus */
    odin3_sim_destroy(sim);
}

/* --- size: 100k cells, 64 cycles, linear work, no allocation ---------------------------------- */

/* top: i -> n/2 instances of inv2 (two NOTs each) -> c, and o = DFF(c). */
static odin3_module *make_chain(uint32_t n_cells) {
    odin3_module *inv2 = new_module("inv2");
    odin3_net_id net_a = input(inv2, "a");
    odin3_net_id net_y = output(inv2, "y");
    odin3_net_id net_m = net(inv2, NULL);
    cell(inv2, "$_NOT_", (odin3_net_id[]){net_m, net_y});
    cell(inv2, "$_NOT_", (odin3_net_id[]){net_a, net_m});
    odin3_module *top = new_module("top");
    odin3_net_id prev = input(top, "i");
    odin3_net_id clk = input(top, "clk");
    odin3_net_id chain_out = output(top, "c");
    cell(top, "$_DFF_P_", (odin3_net_id[]){clk, chain_out, output(top, "o")});
    for (uint32_t i = 0; i < n_cells / 2; i++) {
        odin3_net_id next = i + 1 == n_cells / 2 ? chain_out : net(top, NULL);
        inst(top, inv2, (odin3_net_id[]){prev, next});
        prev = next;
    }
    return top;
}

/* Runs N_CYCLES random cycles on a chain of n_cells NOTs; returns the hook calls they made. */
static uint64_t chain_work(uint32_t n_cells) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    odin3_sim *sim = build_ok(make_chain(n_cells));
    TEST_ASSERT_EQUAL_UINT32(n_cells + 1, sim->n_cells);
    odin3_prng prng;
    odin3_prng_seed(&prng, 3);
    uint64_t before = sim->n_hook_calls;
    odin3_util_set_alloc_fail_after(0); /* the next allocation fails and disarms the hook */
    for (uint32_t cyc = 0; cyc < N_CYCLES; cyc++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        uint32_t in = get_in(sim, 0);
        TEST_ASSERT_EQUAL_UINT32(in, get_out(sim, 0)); /* an even number of inversions */
        TEST_ASSERT_EQUAL_UINT32(in, get_out(sim, 1)); /* the flop took it at the rising edge */
    }
    void *probe = odin3_util_malloc(1);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NULL_MESSAGE(probe, "a cycle allocated"); /* the hook was still armed */
    uint64_t work = sim->n_hook_calls - before;
    odin3_sim_destroy(sim);
    return work;
}

static void test_100k_cells_64_cycles_linear(void) {
    /* per cycle: three settles over every cell plus two edge events to the one flop */
    uint64_t big = chain_work(BIG);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)N_CYCLES * (3ULL * (BIG + 1) + 2), big);
    uint64_t small = chain_work(BIG / 4);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)N_CYCLES * (3ULL * (BIG / 4 + 1) + 2), small);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_prng_known_answers);
    RUN_TEST(test_dff_p_and_n_update_on_opposite_edges);
    RUN_TEST(test_counter_counts_over_20_cycles);
    RUN_TEST(test_port_names_and_widths);
    RUN_TEST(test_port_clocks);
    RUN_TEST(test_clock_cannot_be_set);
    RUN_TEST(test_port_range_errors);
    RUN_TEST(test_two_stage_pipeline_latency);
    RUN_TEST(test_level_latch_on_an_input);
    RUN_TEST(test_level_latch_on_the_clock);
    RUN_TEST(test_init_one_and_zero);
    RUN_TEST(test_same_seed_same_outputs);
    RUN_TEST(test_two_cells_driving_a_net_are_rejected);
    RUN_TEST(test_input_and_cell_driving_a_net_are_rejected);
    RUN_TEST(test_drivers_across_hierarchy_are_rejected);
    RUN_TEST(test_tristate_drivers_are_allowed);
    RUN_TEST(test_100k_cells_64_cycles_linear);
    return UNITY_END();
}
