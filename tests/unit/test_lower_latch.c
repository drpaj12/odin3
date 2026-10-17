/*
 * test_lower_latch.c — 3B Task 7 (spec §6.2, §14.1, §14.3): the four word latches $dlatch $adlatch
 * $dlatchsr $sr. The structure of spec §6.2 (node types and counts) for every type x polarity of
 * every control x width {1, 4, 33}; the netlist view (a latch enable from logic is legal); init
 * propagation; no divergence (no warning, nothing in the async report, accepted by --async error);
 * the estimate bound; and the behaviour: the lowered module in the 1E simulator against the
 * independent reference (lower_latchref.c), sampled at the end of each cycle, exhaustively over
 * input sequences of length <= 4 at W = 1 and on random sequences at larger widths; OOM at every
 * allocation.
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "lower_latchref.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/lower/lower_int.h"
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
    LOG_TEXT = 4096,
    MSG_LEN = 160,
    MAX_PARAMS = 5,
    SEQ_BITS = 16,
    MAX_SEQ = 4,
    WALK_STEPS = 500
};

static char log_text[LOG_TEXT];

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
    odin3_log_set_sink(text_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* --- the cases ---------------------------------------------------------------------------- */

typedef struct ltc {
    ltref_cfg cfg;
    uint64_t init_val; /* the init attribute bits that are 0 or 1 ... */
    uint64_t init_def; /* ... where this mask says the bit is defined (else x) */
} ltc;

/* The parameters of each type in definition order: W width, E EN_POLARITY, P ARST_POLARITY, V the
 * ARST_VALUE, S SET_POLARITY, R CLR_POLARITY. */
static const char *const k_param_codes[LTREF_KIND_COUNT] = {"WE", "WEPV", "WESR", "WSR"};

typedef struct case_key {
    ltref_kind kind;
    uint32_t width;
} case_key;

#define MAKE_CASE(kind, width) make_case_of((case_key){(kind), (width)})

static ltc make_case_of(case_key key) {
    ltref_kind kind = key.kind;
    uint32_t width = key.width;
    ltc cs;
    memset(&cs, 0, sizeof cs);
    cs.cfg.kind = kind;
    cs.cfg.width = width;
    cs.cfg.en_high = true;
    cs.cfg.arst_high = true;
    cs.cfg.set_high = true;
    cs.cfg.clr_high = true;
    return cs;
}

static void describe(const ltc *cs, char *msg) {
    (void)snprintf(msg, MSG_LEN, "%s W=%u en=%d arst=%d set=%d clr=%d value=%llx init=%llx/%llx",
                   ltref_name(cs->cfg.kind), cs->cfg.width, cs->cfg.en_high, cs->cfg.arst_high,
                   cs->cfg.set_high, cs->cfg.clr_high, (unsigned long long)cs->cfg.value,
                   (unsigned long long)cs->init_val, (unsigned long long)cs->init_def);
}

static odin3_value param_of(const ltc *cs, char code, const uint8_t *bits) {
    switch (code) {
    case 'W':
        return odin3_value_int(cs->cfg.width);
    case 'E':
        return odin3_value_int(cs->cfg.en_high);
    case 'P':
        return odin3_value_int(cs->cfg.arst_high);
    case 'S':
        return odin3_value_int(cs->cfg.set_high);
    case 'R':
        return odin3_value_int(cs->cfg.clr_high);
    default:
        return (odin3_value){ODIN3_VAL_BITS, 0, bits, cs->cfg.width, 0, 0};
    }
}

/* The parameter values of the cell, in definition order; bits holds the BITS value. */
static uint32_t make_params(const ltc *cs, odin3_value *params, uint8_t *bits) {
    for (uint32_t i = 0; i < cs->cfg.width; i++) {
        bits[i] = (uint8_t)(((cs->cfg.value >> i) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    const char *code = k_param_codes[cs->cfg.kind];
    uint32_t count = 0;
    for (; code[count] != '\0'; count++) {
        params[count] = param_of(cs, code[count], bits);
    }
    return count;
}

/* A one-cell module "m" with the cell "dut". */
typedef struct ltd {
    lf fix;
    odin3_module *mod;
    odin3_node_id node;
} ltd;

static void ltd_make(ltd *dd, const ltc *cs) {
    lf_begin(&dd->fix);
    odin3_value params[MAX_PARAMS];
    uint8_t bits[LTREF_MAX_WIDTH];
    uint32_t count = make_params(cs, params, bits);
    lf_cell_spec spec = {ltref_name(cs->cfg.kind), params, count, "dut", {0}};
    dd->node = lf_one_cell(&dd->fix, &spec, &dd->mod);
    for (uint32_t bit = 0; bit < cs->cfg.width; bit++) {
        if (((cs->init_def >> bit) & 1U) != 0) {
            odin3_net_id net = lf_wnet(&dd->fix, dd->mod, "Q", bit);
            odin3_bit val = ((cs->init_val >> bit) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(dd->mod, net, val));
        }
    }
}

static odin3_status ltd_lower(ltd *dd, const char *args) {
    return odin3_pass_run(dd->fix.design, "lower", odin3_bytes_cstr(args));
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

/* Lowers the case and checks the result is a netlist. */
static void lower_case(ltd *dd, const ltc *cs) {
    ltd_make(dd, cs);
    char msg[MSG_LEN];
    describe(cs, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ltd_lower(dd, ""), msg);
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
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_set_input(sim, at, ((value >> bit) & 1U) != 0));
        }
    }
}

/* Drives the inputs of one cycle and runs it. */
static void run_cycle(odin3_sim *sim, ltref_kind kind, const ltref_in *in) {
    if (kind != LTREF_SR) {
        drive(sim, "D", in->data);
    }
    if (ltref_has_en(kind)) {
        drive(sim, "EN", in->en ? 1U : 0U);
    }
    if (ltref_has_arst(kind)) {
        drive(sim, "ARST", in->arst ? 1U : 0U);
    }
    if (ltref_has_setclr(kind)) {
        drive(sim, "SET", in->set);
        drive(sim, "CLR", in->clr);
    }
    odin3_sim_cycle(sim);
}

static uint64_t read_q(const odin3_sim *sim) {
    uint64_t value = 0;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        if (strcmp(odin3_sim_output_name(sim, port), "Q") != 0) {
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

/* The state the simulator shows after build: its settle runs with every input 0, the latch
 * starting at INIT (1 starts at 1; 0 and x at 0). */
static uint64_t start_state(const ltc *cs) {
    static const ltref_in k_zero = {0, 0, 0, false, false};
    return ltref_step(&cs->cfg, cs->init_val & cs->init_def, &k_zero);
}

/* The tracker of what a walk has visited: [state bit][input code] at W = 1. */
typedef struct visited {
    uint8_t seen[2][1U << 6];
} visited;

/* Runs `len` cycles of `seq` on a fresh simulator of the lowered module against the reference;
 * returns the index of the first mismatching cycle + 1, or 0 when all agree (-1: no simulator). */
static int run_sequence(const ltd *dd, const ltc *cs, const ltref_in *seq, uint32_t len) {
    odin3_sim *sim = NULL;
    if (odin3_sim_build(dd->fix.design, odin3_module_id_of(dd->mod), &sim) != ODIN3_OK) {
        return -1;
    }
    uint64_t state = start_state(cs);
    int bad = read_q(sim) == state ? 0 : 1;
    for (uint32_t step = 0; bad == 0 && step < len; step++) {
        run_cycle(sim, cs->cfg.kind, &seq[step]);
        state = ltref_step(&cs->cfg, state, &seq[step]);
        bad = read_q(sim) == state ? 0 : (int)step + 1;
    }
    odin3_sim_destroy(sim);
    return bad;
}

/* The number of input bits per cycle at W = 1: D, EN, ARST, SET and CLR. */
static uint32_t input_bits(ltref_kind kind) {
    return (kind != LTREF_SR ? 1U : 0U) + (ltref_has_en(kind) ? 1U : 0U) +
           (ltref_has_arst(kind) ? 1U : 0U) + (ltref_has_setclr(kind) ? 2U : 0U);
}

/* The inputs of a W = 1 cycle from a code of input_bits(kind) bits. */
static ltref_in unpack(ltref_kind kind, uint32_t code) {
    ltref_in in;
    memset(&in, 0, sizeof in);
    uint32_t pos = 0;
    in.data = kind != LTREF_SR ? (code >> pos++) & 1U : 0U;
    in.en = ltref_has_en(kind) ? ((code >> pos++) & 1U) != 0 : true;
    in.arst = ltref_has_arst(kind) ? ((code >> pos++) & 1U) != 0 : false;
    if (ltref_has_setclr(kind)) {
        in.set = (code >> pos++) & 1U;
        in.clr = (code >> pos++) & 1U;
    }
    return in;
}

/* Every input sequence of length 1 to `max_len` (at most MAX_SEQ; fewer when SEQ_BITS is the limit)
 * on W = 1. */
static void exhaustive_w1(const ltd *dd, const ltc *cs, uint32_t max_len) {
    char msg[MSG_LEN];
    describe(cs, msg);
    uint32_t bits = input_bits(cs->cfg.kind);
    uint32_t len = SEQ_BITS / bits > max_len ? max_len : SEQ_BITS / bits;
    for (uint32_t length = 1; length <= len; length++) {
        uint64_t count = (uint64_t)1 << (bits * length);
        for (uint64_t num = 0; num < count; num++) {
            ltref_in seq[MAX_SEQ];
            for (uint32_t step = 0; step < length; step++) {
                seq[step] =
                    unpack(cs->cfg.kind, (uint32_t)(num >> (bits * step)) & ((1U << bits) - 1U));
            }
            int bad = run_sequence(dd, cs, seq, length);
            char why[MSG_LEN + 64];
            (void)snprintf(why, sizeof why, "%s: sequence %llu of length %u, cycle %d", msg,
                           (unsigned long long)num, length, bad);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, bad, why);
        }
    }
}

/* The seed and length of a random walk. */
typedef struct walk_opts {
    uint64_t seed;
    uint32_t steps;
} walk_opts;

/* Random inputs of a cell wider than one bit: SET and CLR are sparse at their level. */
static ltref_in wide_inputs(const ltc *cs, odin3_prng *prng) {
    uint64_t all =
        cs->cfg.width >= LTREF_MAX_WIDTH ? ~(uint64_t)0 : ((uint64_t)1 << cs->cfg.width) - 1U;
    uint32_t code = (uint32_t)odin3_prng_next(prng);
    uint64_t rnd = odin3_prng_next(prng);
    ltref_in in;
    memset(&in, 0, sizeof in);
    in.data = odin3_prng_next(prng) & all;
    in.set = rnd & odin3_prng_next(prng) & all;
    in.clr = (rnd >> 7) & odin3_prng_next(prng) & all;
    in.set = cs->cfg.set_high ? in.set : ~in.set & all;
    in.clr = cs->cfg.clr_high ? in.clr : ~in.clr & all;
    in.en = (code & 1U) != 0;
    in.arst = (code & 6U) == 0 ? cs->cfg.arst_high : !cs->cfg.arst_high;
    return in;
}

/* Every (state, input) pair of a one-bit cell has been visited. */
static void assert_all_visited(const visited *seen, const ltc *cs, const char *msg) {
    for (uint32_t code = 0; code < (1U << input_bits(cs->cfg.kind)); code++) {
        TEST_ASSERT_TRUE_MESSAGE(seen->seen[0][code] != 0 && seen->seen[1][code] != 0, msg);
    }
}

/* One long random walk on a single simulator; at W = 1 it must visit every (state, input) pair. */
static void random_walk(const ltd *dd, const ltc *cs, const walk_opts *opts) {
    char msg[MSG_LEN];
    describe(cs, msg);
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build(dd->fix.design, odin3_module_id_of(dd->mod), &sim));
    odin3_prng prng;
    odin3_prng_seed(&prng, opts->seed);
    visited seen;
    memset(&seen, 0, sizeof seen);
    uint64_t state = start_state(cs);
    for (uint32_t step = 0; step < opts->steps; step++) {
        uint32_t code = (uint32_t)odin3_prng_next(&prng) & ((1U << input_bits(cs->cfg.kind)) - 1U);
        ltref_in in = cs->cfg.width == 1 ? unpack(cs->cfg.kind, code) : wide_inputs(cs, &prng);
        seen.seen[state & 1U][code] = 1;
        run_cycle(sim, cs->cfg.kind, &in);
        state = ltref_step(&cs->cfg, state, &in);
        char why[MSG_LEN + 48];
        (void)snprintf(why, sizeof why, "%s: cycle %u", msg, step + 1);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(state, read_q(sim), why);
    }
    odin3_sim_destroy(sim);
    if (cs->cfg.width == 1) {
        assert_all_visited(&seen, cs, msg);
    }
}

/* --- the reference's own test ----------------------------------------------------------------- */

typedef struct hand_step {
    ltref_in in;
    uint64_t want;
} hand_step;

static void run_hand(const ltref_cfg *cfg, uint64_t start, const hand_step *steps, uint32_t count) {
    uint64_t state = start;
    for (uint32_t i = 0; i < count; i++) {
        state = ltref_step(cfg, state, &steps[i].in);
        char msg[48];
        (void)snprintf(msg, sizeof msg, "%s step %u", ltref_name(cfg->kind), i);
        TEST_ASSERT_EQUAL_HEX64_MESSAGE(steps[i].want, state, msg);
    }
}

static void test_the_reference_matches_hand_computed_sequences(void) {
    ltc dl = MAKE_CASE(LTREF_DLATCH, 4); /* data, set, clr, en, arst */
    const hand_step dlatch[] = {
        {{5, 0, 0, true, false}, 5}, {{9, 0, 0, false, false}, 5}, {{9, 0, 0, true, false}, 9}};
    run_hand(&dl.cfg, 0, dlatch, 3);
    dl.cfg.en_high = false; /* active low: en = 0 is transparent */
    const hand_step dlatch_low[] = {{{3, 0, 0, false, false}, 3}, {{7, 0, 0, true, false}, 3}};
    run_hand(&dl.cfg, 0, dlatch_low, 2);

    ltc ad = MAKE_CASE(LTREF_ADLATCH, 4); /* ARST_VALUE 3: the reset acts with the enable low */
    ad.cfg.value = 3;
    const hand_step adlatch[] = {
        {{7, 0, 0, true, false}, 7}, {{1, 0, 0, false, true}, 3}, {{1, 0, 0, false, false}, 3}};
    run_hand(&ad.cfg, 0, adlatch, 3);

    ltc sd = MAKE_CASE(LTREF_DLATCHSR, 4); /* clear wins over set; both over the enable */
    const hand_step dlatchsr[] = {{{8, 3, 5, true, false}, 10}, /* set 0011, clr 0101: 0010 | D */
                                  {{0, 0, 1, false, false}, 10 & ~1U},
                                  {{15, 0, 0, false, false}, 10 & ~1U}};
    run_hand(&sd.cfg, 0, dlatchsr, 3);

    ltc sr = MAKE_CASE(LTREF_SR, 4);
    sr.cfg.clr_high = false;                                /* CLR active low: a 0 clears */
    const hand_step srs[] = {{{0, 1, 15, false, false}, 1}, /* set bit 0, nothing cleared */
                             {{0, 0, 15, false, false}, 1},
                             {{0, 0, 14, false, false}, 0}};
    run_hand(&sr.cfg, 0, srs, 3);
}

/* --- structure (spec §6.2) ------------------------------------------------------------------ */

typedef struct want_counts {
    uint32_t latch_p;
    uint32_t latch_n;
    uint32_t mux;
    uint32_t or_gate;
    uint32_t not_gate;
} want_counts;

static want_counts expected(const ltc *cs) {
    uint32_t width = cs->cfg.width;
    uint32_t en_low = cs->cfg.en_high ? 0U : 1U;
    uint32_t set_low = cs->cfg.set_high ? 0U : 1U;
    uint32_t clr_low = cs->cfg.clr_high ? 0U : 1U;
    switch (cs->cfg.kind) {
    case LTREF_DLATCH:
        return (want_counts){en_low == 0 ? width : 0U, en_low != 0 ? width : 0U, 0, 0, 0};
    case LTREF_ADLATCH:
        return (want_counts){width, 0, width, 1, en_low + (cs->cfg.arst_high ? 0U : 1U)};
    case LTREF_DLATCHSR:
        return (want_counts){width, 0, 2U * width, 2U * width,
                             en_low + width * (set_low + clr_low)};
    default:
        return (want_counts){width, 0, 0, width, width * (set_low + 1U)};
    }
}

static void check_structure(const ltc *cs) {
    ltd dd;
    char msg[MSG_LEN];
    describe(cs, msg);
    ltd_make(&dd, cs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ltd_lower(&dd, ""), msg);
    want_counts want = expected(cs);
    lf *fix = &dd.fix;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.latch_p, lf_count_type(fix, dd.mod, "$_DLATCH_P_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.latch_n, lf_count_type(fix, dd.mod, "$_DLATCH_N_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.mux, lf_count_type(fix, dd.mod, "$_MUX_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.or_gate, lf_count_type(fix, dd.mod, "$_OR_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.not_gate, lf_count_type(fix, dd.mod, "$_NOT_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.latch_p + want.latch_n + want.mux + want.or_gate +
                                         want.not_gate,
                                     lf_gate_nodes(fix, dd.mod), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(fix, dd.mod), msg);
    check_netlist_view(dd.mod); /* a latch enable from logic is legal */
    lf_end(fix);
}

/* The case of polarity code `pol` (5 bits), or false when it names a control the type lacks. */
static bool polarity_case(ltc *cs, uint32_t pol) {
    cs->cfg.en_high = (pol & 1U) != 0;
    cs->cfg.arst_high = (pol & 2U) != 0;
    cs->cfg.set_high = (pol & 4U) != 0;
    cs->cfg.clr_high = (pol & 8U) != 0;
    cs->cfg.value = (pol >> 4) & 1U;
    ltref_kind kind = cs->cfg.kind;
    bool no_en = !ltref_has_en(kind) && !cs->cfg.en_high;
    bool no_arst = !ltref_has_arst(kind) && (!cs->cfg.arst_high || cs->cfg.value != 0);
    bool no_setclr = !ltref_has_setclr(kind) && (!cs->cfg.set_high || !cs->cfg.clr_high);
    return !no_en && !no_arst && !no_setclr;
}

static void test_structure_of_every_type_polarity_and_width(void) {
    static const uint32_t k_widths[] = {1, 4, 33};
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t widx = 0; widx < 3; widx++) {
            for (uint32_t pol = 0; pol < 16; pol++) {
                ltc cs = MAKE_CASE((ltref_kind)kind, k_widths[widx]);
                if (polarity_case(&cs, pol)) {
                    check_structure(&cs);
                }
            }
        }
    }
}

static void test_dlatch_with_an_active_low_enable_is_a_dlatch_n_on_the_enable(void) {
    ltc cs = MAKE_CASE(LTREF_DLATCH, 3);
    cs.cfg.en_high = false;
    ltd dd;
    lower_case(&dd, &cs);
    TEST_ASSERT_EQUAL_UINT32(3, lf_count_type(&dd.fix, dd.mod, "$_DLATCH_N_"));
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&dd.fix, dd.mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_UINT32(3, lf_gate_nodes(&dd.fix, dd.mod));
    lf_end(&dd.fix);
}

static bool one_bit_port(const char *name) {
    return strcmp(name, "EN") == 0 || strcmp(name, "ARST") == 0;
}

/* lw_est_* of the case's cell, from its widths. */
static uint64_t estimate_of(ltd *dd, const ltc *cs) {
    const odin3_celltype_def *def =
        odin3_celltype_get(dd->fix.design, odin3_node_type(dd->mod, dd->node));
    uint32_t widths[LW_MAX_PORTS];
    for (uint32_t port = 0; port < def->n_ports; port++) {
        widths[port] = one_bit_port(def->ports[port].name) ? 1U : cs->cfg.width;
    }
    odin3_value params[MAX_PARAMS];
    uint8_t bits[LTREF_MAX_WIDTH];
    TEST_ASSERT_EQUAL_UINT32(def->n_params, make_params(cs, params, bits));
    return lf_estimate(def->name, params, widths, def->n_ports);
}

static void test_the_size_estimate_bounds_the_gates(void) {
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t pol = 0; pol < 16; pol++) {
            ltc cs = MAKE_CASE((ltref_kind)kind, 5);
            if (!polarity_case(&cs, pol)) {
                continue;
            }
            ltd dd;
            ltd_make(&dd, &cs);
            uint64_t estimate = estimate_of(&dd, &cs);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, ltd_lower(&dd, ""));
            TEST_ASSERT_TRUE_MESSAGE(lf_gate_nodes(&dd.fix, dd.mod) <= estimate,
                                     ltref_name((ltref_kind)kind));
            lf_end(&dd.fix);
        }
    }
}

/* --- init and "no divergence" ----------------------------------------------------------------- */

/* The INIT parameter of the $_DLATCH_ node whose Q is the net of bit `bit` of wire Q; -1: none. */
static int init_of_bit(ltd *dd, uint32_t bit) {
    odin3_net_id want = lf_wnet(&dd->fix, dd->mod, "Q", bit);
    for (uint32_t id = 1; id < odin3_module_node_end(dd->mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(dd->mod, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(dd->fix.design, odin3_node_type(dd->mod, node));
        if (strncmp(def->name, "$_DLATCH_", 9) != 0) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(dd->mod, node);
        odin3_pin_id qpin = {pins.first.v + 2};
        if (odin3_pin_net(dd->mod, qpin).v == want.v) {
            return (int)odin3_node_param(dd->mod, node, 0)->i;
        }
    }
    return -1;
}

static void test_init_values_come_from_the_init_attribute(void) {
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        ltc cs = MAKE_CASE((ltref_kind)kind, 3);
        cs.init_def = 3U; /* bit 0 = 1, bit 1 = 0, bit 2 undefined */
        cs.init_val = 1U;
        ltd dd;
        ltd_make(&dd, &cs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, ltd_lower(&dd, ""));
        TEST_ASSERT_EQUAL_INT(1, init_of_bit(&dd, 0));
        TEST_ASSERT_EQUAL_INT(0, init_of_bit(&dd, 1));
        TEST_ASSERT_EQUAL_INT(3, init_of_bit(&dd, 2));
        lf_end(&dd.fix);
        cs.init_def = 0; /* no init attribute: unknown */
        ltd_make(&dd, &cs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, ltd_lower(&dd, ""));
        TEST_ASSERT_EQUAL_INT(3, init_of_bit(&dd, 0));
        lf_end(&dd.fix);
    }
}

/* The text of the file `path` (up to 1 KiB). */
static void read_text(const char *path, char *text, size_t size) {
    FILE *file = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(text, 1, size - 1, file);
    text[got] = '\0';
    (void)fclose(file);
}

/* The --report file lists no async and no falling-edge cell. */
static void assert_report_empty(const char *path) {
    char text[1024] = "";
    read_text(path, text, sizeof text);
    TEST_ASSERT_NOT_NULL(strstr(text, "\"async_to_sync\": []"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"neg_edge\": []"));
}

/* One type lowered with `--async error --report`: no warning, an empty async report. */
static void assert_no_divergence(ltref_kind kind) {
    static const char k_path[] = "test_lower_latch.report.json";
    ltc cs = MAKE_CASE(kind, 2);
    ltd dd;
    ltd_make(&dd, &cs);
    log_text[0] = '\0';
    char args[96];
    (void)snprintf(args, sizeof args, "--async error --report %s", k_path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ltd_lower(&dd, args));
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "clock inverter"), log_text);
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "made synchronous"), log_text);
    assert_report_empty(k_path);
    lf_end(&dd.fix);
}

/* Spec §6.2: the latches keep their meaning exactly, so there is no divergence warning, no report
 * entry and nothing for --async error to refuse. */
static void test_latches_have_no_divergence_and_pass_async_error(void) {
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        assert_no_divergence((ltref_kind)kind);
    }
}

/* --- behaviour ------------------------------------------------------------------------------ */

static void test_w1_exhaustive_short_sequences_for_every_type_and_init(void) {
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t init = 0; init < 3; init++) {
            ltc cs = MAKE_CASE((ltref_kind)kind, 1);
            cs.cfg.value = init & 1U;
            cs.init_def = init < 2 ? 1U : 0U;
            cs.init_val = init;
            ltd dd;
            lower_case(&dd, &cs);
            exhaustive_w1(&dd, &cs, MAX_SEQ);
            lf_end(&dd.fix);
        }
    }
}

/* The same over all four polarities of the controls of each type (active low, the swapped arms and
 * the inverters). */
static void test_w1_exhaustive_over_every_polarity(void) {
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t pol = 0; pol < 32; pol++) {
            ltc cs = MAKE_CASE((ltref_kind)kind, 1);
            if (!polarity_case(&cs, pol)) {
                continue;
            }
            ltd dd;
            lower_case(&dd, &cs);
            exhaustive_w1(&dd, &cs, 3);
            lf_end(&dd.fix);
        }
    }
}

static void test_w1_every_polarity_visits_every_state_and_input(void) {
    uint64_t seed = 1;
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t pol = 0; pol < 32; pol++) {
            ltc cs = MAKE_CASE((ltref_kind)kind, 1);
            if (!polarity_case(&cs, pol)) {
                continue;
            }
            ltd dd;
            lower_case(&dd, &cs);
            random_walk(&dd, &cs, &(walk_opts){seed++, WALK_STEPS});
            lf_end(&dd.fix);
        }
    }
}

static void test_wide_cells_follow_the_reference_on_random_sequences(void) {
    static const uint32_t k_widths[] = {4, 33};
    uint64_t seed = 77;
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        for (uint32_t widx = 0; widx < 2; widx++) {
            for (uint32_t pol = 0; pol < 4; pol++) {
                ltc cs = MAKE_CASE((ltref_kind)kind, k_widths[widx]);
                cs.cfg.en_high = cs.cfg.arst_high = cs.cfg.set_high = (pol & 1U) != 0;
                cs.cfg.clr_high = (pol & 2U) != 0;
                cs.cfg.value = 0x5A5A5A5A5AULL * (seed % 7 + 1);
                cs.init_def = 0x0F0F0F0F0FULL;
                cs.init_val = 0xA5A5A5A5A5ULL;
                ltd dd;
                lower_case(&dd, &cs);
                random_walk(&dd, &cs, &(walk_opts){seed++, 150});
                lf_end(&dd.fix);
            }
        }
    }
}

/* Clear wins over set (simlib) and both act with the enable low: one named sequence per type. */
static void test_clear_wins_over_set_with_the_enable_low(void) {
    static const ltref_kind k_kinds[] = {LTREF_DLATCHSR, LTREF_SR};
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        ltc cs = MAKE_CASE(k_kinds[i], 1);
        cs.cfg.en_high = true;
        ltd dd;
        lower_case(&dd, &cs);
        const ltref_in seq[2] = {{1, 1, 0, false, false}, {1, 1, 1, false, false}};
        TEST_ASSERT_EQUAL_UINT64(1, ltref_step(&cs.cfg, 0, &seq[0]));
        TEST_ASSERT_EQUAL_UINT64(0, ltref_step(&cs.cfg, 1, &seq[1]));
        TEST_ASSERT_EQUAL_INT(0, run_sequence(&dd, &cs, seq, 2));
        lf_end(&dd.fix);
    }
}

/* --- out of memory ---------------------------------------------------------------------------- */

static void oom_sweep(const ltc *cs) {
    bool done = false;
    for (long step = 0; !done && step < 2000; step++) {
        ltd dd;
        ltd_make(&dd, cs);
        odin3_strbuf before;
        odin3_strbuf after;
        odin3_strbuf_init(&before);
        odin3_strbuf_init(&after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &before));
        odin3_util_set_alloc_fail_after(step);
        odin3_status st = ltd_lower(&dd, "");
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
    for (uint32_t kind = 0; kind < LTREF_KIND_COUNT; kind++) {
        ltc cs = MAKE_CASE((ltref_kind)kind, 4);
        cs.cfg.en_high = false;
        cs.cfg.set_high = false;
        cs.cfg.clr_high = false;
        cs.cfg.arst_high = false;
        oom_sweep(&cs);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_reference_matches_hand_computed_sequences);
    RUN_TEST(test_structure_of_every_type_polarity_and_width);
    RUN_TEST(test_dlatch_with_an_active_low_enable_is_a_dlatch_n_on_the_enable);
    RUN_TEST(test_the_size_estimate_bounds_the_gates);
    RUN_TEST(test_init_values_come_from_the_init_attribute);
    RUN_TEST(test_latches_have_no_divergence_and_pass_async_error);
    RUN_TEST(test_w1_exhaustive_short_sequences_for_every_type_and_init);
    RUN_TEST(test_w1_exhaustive_over_every_polarity);
    RUN_TEST(test_w1_every_polarity_visits_every_state_and_input);
    RUN_TEST(test_wide_cells_follow_the_reference_on_random_sequences);
    RUN_TEST(test_clear_wins_over_set_with_the_enable_low);
    RUN_TEST(test_oom_at_every_allocation_leaves_a_valid_design);
    return UNITY_END();
}
