/*
 * test_lower_ff.c — 3B Task 6 (spec §6.1, §14.1, §14.3; PHASE3 decision 19): the eleven word
 * flip-flops. The structure of spec §6.1's table (node types and counts) for every type x
 * CLK_POLARITY x polarity of every control x width {1, 4, 33}; the netlist view; init propagation;
 * --neg-edge both ways; --async error; the once-per-design warning; the --report JSON; and the
 * behaviour: the lowered module in the 1E simulator against the independent reference stepper
 * (lower_ffref.c), sampled at the end of each cycle, exhaustively over short input sequences at W =
 * 1 and on random sequences at larger widths. The enable-first variant (hidden option) has its own
 * reference.
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_ffref.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
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

enum { LOG_TEXT = 8192, MSG_LEN = 160, MAX_PARAMS = 6, SEQ_BITS = 15, WALK_STEPS = 700 };

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
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

typedef struct ffc {
    ffref_cfg cfg;
    bool clk_high;
    uint64_t init_val; /* the init attribute bits that are 0 or 1 ... */
    uint64_t init_def; /* ... where this mask says the bit is defined (else x) */
    const char *args;  /* the lower arguments, "" for none */
} ffc;

/* The parameters of each type in definition order: W width, C CLK_POLARITY, E EN_POLARITY, P the
 * polarity of the reset or load, V the *_VALUE, S SET_POLARITY, R CLR_POLARITY. */
static const char *const k_param_codes[FFREF_KIND_COUNT] = {
    "WC", "WCE", "WCPV", "WCEPV", "WCEPV", "WCPV", "WCEPV", "WCP", "WCEP", "WCSR", "WCSRE"};

typedef struct case_key {
    ffref_kind kind;
    uint32_t width;
} case_key;

#define MAKE_CASE(kind, width) make_case_of((case_key){(kind), (width)})

static ffc make_case_of(case_key key) {
    ffref_kind kind = key.kind;
    uint32_t width = key.width;
    ffc cs;
    memset(&cs, 0, sizeof cs);
    cs.cfg.kind = kind;
    cs.cfg.width = width;
    cs.cfg.en_high = true;
    cs.cfg.ctl_high = true;
    cs.cfg.set_high = true;
    cs.cfg.clr_high = true;
    cs.clk_high = true;
    cs.args = "";
    return cs;
}

static void describe(const ffc *cs, char *msg) {
    (void)snprintf(
        msg, MSG_LEN, "%s W=%u clk=%d en=%d ctl=%d set=%d clr=%d value=%llx init=%llx/%llx %s",
        ffref_name(cs->cfg.kind), cs->cfg.width, cs->clk_high, cs->cfg.en_high, cs->cfg.ctl_high,
        cs->cfg.set_high, cs->cfg.clr_high, (unsigned long long)cs->cfg.value,
        (unsigned long long)cs->init_val, (unsigned long long)cs->init_def, cs->args);
}

/* The parameter values of the cell, in definition order; bits holds the BITS value. */
static uint32_t make_params(const ffc *cs, odin3_value *params, uint8_t *bits) {
    for (uint32_t i = 0; i < cs->cfg.width; i++) {
        bits[i] = (uint8_t)(((cs->cfg.value >> i) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    const char *code = k_param_codes[cs->cfg.kind];
    uint32_t count = 0;
    for (; code[count] != '\0'; count++) {
        switch (code[count]) {
        case 'W':
            params[count] = odin3_value_int(cs->cfg.width);
            break;
        case 'C':
            params[count] = odin3_value_int(cs->clk_high);
            break;
        case 'E':
            params[count] = odin3_value_int(cs->cfg.en_high);
            break;
        case 'P':
            params[count] = odin3_value_int(cs->cfg.ctl_high);
            break;
        case 'S':
            params[count] = odin3_value_int(cs->cfg.set_high);
            break;
        case 'R':
            params[count] = odin3_value_int(cs->cfg.clr_high);
            break;
        default:
            params[count] = (odin3_value){ODIN3_VAL_BITS, 0, bits, cs->cfg.width, 0, 0};
            break;
        }
    }
    return count;
}

/* A one-cell module "m" with the cell "dut". */
typedef struct ffd {
    lf fix;
    odin3_module *mod;
    odin3_node_id node;
} ffd;

static void ffd_make(ffd *dd, const ffc *cs) {
    lf_begin(&dd->fix);
    odin3_value params[MAX_PARAMS];
    uint8_t bits[FFREF_MAX_WIDTH];
    uint32_t count = make_params(cs, params, bits);
    lf_cell_spec spec = {ffref_name(cs->cfg.kind), params, count, "dut", {0}};
    dd->node = lf_one_cell(&dd->fix, &spec, &dd->mod);
    for (uint32_t bit = 0; bit < cs->cfg.width; bit++) {
        if (((cs->init_def >> bit) & 1U) != 0) {
            odin3_net_id net = lf_wnet(&dd->fix, dd->mod, "Q", bit);
            odin3_bit val = ((cs->init_val >> bit) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(dd->mod, net, val));
        }
    }
}

static odin3_status ffd_lower(ffd *dd, const char *args) {
    return odin3_pass_run(dd->fix.design, "lower", odin3_bytes_cstr(args));
}

static void check_netlist_view(odin3_module *mod) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
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

static const char *ctl_name(ffref_kind kind) {
    if (kind == FFREF_ADFF || kind == FFREF_ADFFE) {
        return "ARST";
    }
    if (kind == FFREF_SDFF || kind == FFREF_SDFFE || kind == FFREF_SDFFCE) {
        return "SRST";
    }
    return kind == FFREF_ALDFF || kind == FFREF_ALDFFE ? "ALOAD" : NULL;
}

static bool has_load(ffref_kind kind) {
    return kind == FFREF_ALDFF || kind == FFREF_ALDFFE;
}

static bool has_setclr(ffref_kind kind) {
    return kind == FFREF_DFFSR || kind == FFREF_DFFSRE;
}

/* Drives the inputs of one cycle and runs it. */
static void run_cycle(odin3_sim *sim, ffref_kind kind, const ffref_in *in) {
    drive(sim, "D", in->data);
    if (ffref_has_en(kind)) {
        drive(sim, "EN", in->en ? 1U : 0U);
    }
    if (ctl_name(kind) != NULL) {
        drive(sim, ctl_name(kind), in->ctl ? 1U : 0U);
    }
    if (has_load(kind)) {
        drive(sim, "AD", in->load);
    }
    if (has_setclr(kind)) {
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

/* The state the simulator starts in: INIT 1 starts at 1; 0 and x at 0. */
static uint64_t start_state(const ffc *cs) {
    return cs->init_val & cs->init_def;
}

/* The tracker of what a walk has visited: [state bit][input code] at W = 1. */
typedef struct visited {
    uint8_t seen[2][1U << 6];
} visited;

/* Runs `len` cycles of `seq` on a fresh simulator of the lowered module against the stepper;
 * returns the index of the first mismatching cycle + 1, or 0 when all agree (-1: no simulator). */
static int run_sequence(const ffd *dd, const ffc *cs, const ffref_in *seq, uint32_t len) {
    odin3_sim *sim = NULL;
    if (odin3_sim_build(dd->fix.design, odin3_module_id_of(dd->mod), &sim) != ODIN3_OK) {
        return -1;
    }
    uint64_t state = start_state(cs);
    int bad = read_q(sim) == state ? 0 : 1;
    for (uint32_t step = 0; bad == 0 && step < len; step++) {
        run_cycle(sim, cs->cfg.kind, &seq[step]);
        state = ffref_step(&cs->cfg, state, &seq[step]);
        bad = read_q(sim) == state ? 0 : (int)step + 1;
    }
    odin3_sim_destroy(sim);
    return bad;
}

/* The number of input bits per cycle at W = 1: D, EN, the control, AD, SET and CLR. */
static uint32_t input_bits(ffref_kind kind) {
    return 1U + (ffref_has_en(kind) ? 1U : 0U) + (ctl_name(kind) != NULL ? 1U : 0U) +
           (has_load(kind) ? 1U : 0U) + (has_setclr(kind) ? 2U : 0U);
}

/* The inputs of a W = 1 cycle from a code of input_bits(kind) bits. */
static ffref_in unpack(ffref_kind kind, uint32_t code) {
    ffref_in in;
    memset(&in, 0, sizeof in);
    uint32_t pos = 0;
    in.data = (code >> pos++) & 1U;
    in.en = ffref_has_en(kind) ? ((code >> pos++) & 1U) != 0 : true;
    in.ctl = ctl_name(kind) != NULL ? ((code >> pos++) & 1U) != 0 : false;
    in.load = has_load(kind) ? (code >> pos++) & 1U : 0U;
    if (has_setclr(kind)) {
        in.set = (code >> pos++) & 1U;
        in.clr = (code >> pos++) & 1U;
    }
    return in;
}

/* Every input sequence of length <= `len` bits budget (SEQ_BITS / bits, at most 5) on W = 1. */
static void exhaustive_w1(const ffd *dd, const ffc *cs, uint32_t max_len) {
    char msg[MSG_LEN];
    describe(cs, msg);
    uint32_t bits = input_bits(cs->cfg.kind);
    uint32_t len = SEQ_BITS / bits > max_len ? max_len : SEQ_BITS / bits;
    for (uint32_t length = 1; length <= len; length++) {
        uint64_t count = (uint64_t)1 << (bits * length);
        for (uint64_t num = 0; num < count; num++) {
            ffref_in seq[5];
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

/* Random inputs of a cell wider than one bit: a quarter of the cycles have the control at its
 * level, and SET / CLR are sparse at theirs. */
static ffref_in wide_inputs(const ffc *cs, odin3_prng *prng) {
    uint64_t all =
        cs->cfg.width >= FFREF_MAX_WIDTH ? ~(uint64_t)0 : ((uint64_t)1 << cs->cfg.width) - 1U;
    uint32_t code = (uint32_t)odin3_prng_next(prng);
    uint64_t rnd = odin3_prng_next(prng);
    ffref_in in;
    memset(&in, 0, sizeof in);
    in.data = odin3_prng_next(prng) & all;
    in.load = odin3_prng_next(prng) & all;
    in.set = rnd & odin3_prng_next(prng) & all;
    in.clr = (rnd >> 7) & odin3_prng_next(prng) & all;
    in.set = cs->cfg.set_high ? in.set : ~in.set & all;
    in.clr = cs->cfg.clr_high ? in.clr : ~in.clr & all;
    in.en = (code & 1U) != 0;
    in.ctl = (code & 6U) == 0 ? cs->cfg.ctl_high : !cs->cfg.ctl_high;
    return in;
}

/* Every (state, input) pair of a one-bit cell has been visited. */
static void assert_all_visited(const visited *seen, const ffc *cs, const char *msg) {
    for (uint32_t code = 0; code < (1U << input_bits(cs->cfg.kind)); code++) {
        TEST_ASSERT_TRUE_MESSAGE(seen->seen[0][code] != 0 && seen->seen[1][code] != 0, msg);
    }
}

/* One long random walk on a single simulator; at W = 1 it must visit every (state, input) pair. */
static void random_walk(const ffd *dd, const ffc *cs, const walk_opts *opts) {
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
        ffref_in in = cs->cfg.width == 1 ? unpack(cs->cfg.kind, code) : wide_inputs(cs, &prng);
        seen.seen[state & 1U][code] = 1;
        run_cycle(sim, cs->cfg.kind, &in);
        state = ffref_step(&cs->cfg, state, &in);
        char why[MSG_LEN + 48];
        (void)snprintf(why, sizeof why, "%s: cycle %u", msg, step + 1);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(state, read_q(sim), why);
    }
    odin3_sim_destroy(sim);
    if (cs->cfg.width == 1) {
        assert_all_visited(&seen, cs, msg);
    }
}

/* Lowers the case and runs `body` on the result; keep for a falling edge (a clock the simulator
 * accepts). */
static void lower_case(ffd *dd, const ffc *cs) {
    ffd_make(dd, cs);
    char msg[MSG_LEN];
    describe(cs, msg);
    char args[96];
    (void)snprintf(args, sizeof args, "%s%s", cs->clk_high ? "" : "--neg-edge keep ", cs->args);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ffd_lower(dd, args), msg);
    check_netlist_view(dd->mod);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(&dd->fix, dd->mod), msg);
}

/* --- the reference stepper's own test ------------------------------------------------------- */

typedef struct hand_step {
    ffref_in in;
    uint64_t want;
} hand_step;

static void run_hand(const ffref_cfg *cfg, uint64_t start, const hand_step *steps, uint32_t count) {
    uint64_t state = start;
    for (uint32_t i = 0; i < count; i++) {
        state = ffref_step(cfg, state, &steps[i].in);
        char msg[48];
        (void)snprintf(msg, sizeof msg, "%s step %u", ffref_name(cfg->kind), i);
        TEST_ASSERT_EQUAL_HEX64_MESSAGE(steps[i].want, state, msg);
    }
}

static void test_the_stepper_matches_hand_computed_sequences(void) {
    ffc cs = MAKE_CASE(FFREF_DFFE, 4);
    const hand_step dffe[] = {{{5, 0, 0, 0, true, false}, 5},
                              {{9, 0, 0, 0, false, false}, 5},
                              {{9, 0, 0, 0, true, false}, 9}};
    run_hand(&cs.cfg, 0, dffe, 3);
    cs.cfg.en_high = false; /* active-low enable: en = 0 loads */
    const hand_step dffe_low[] = {{{3, 0, 0, 0, false, false}, 3}, {{7, 0, 0, 0, true, false}, 3}};
    run_hand(&cs.cfg, 0, dffe_low, 2);

    ffc sd = MAKE_CASE(FFREF_SDFFE, 4); /* SRST_VALUE 3: reset over enable */
    sd.cfg.value = 3;
    const hand_step sdffe[] = {{{7, 0, 0, 0, true, false}, 7}, {{1, 0, 0, 0, false, true}, 3}};
    run_hand(&sd.cfg, 0, sdffe, 2);
    sd.cfg.kind = FFREF_SDFFCE; /* enable over reset: EN = 0 holds even with SRST */
    const hand_step sdffce[] = {{{7, 0, 0, 0, true, false}, 7},
                                {{1, 0, 0, 0, false, true}, 7},
                                {{1, 0, 0, 0, true, true}, 3}};
    run_hand(&sd.cfg, 0, sdffce, 3);

    ffc ad = MAKE_CASE(FFREF_ADFFE, 4);
    ad.cfg.value = 3;
    const hand_step adffe[] = {{{7, 0, 0, 0, false, true}, 3}, {{7, 0, 0, 0, false, false}, 3}};
    run_hand(&ad.cfg, 0, adffe, 2);
    ad.cfg.enable_first = true; /* Parmys: EN = 0 holds even with ARST */
    const hand_step parmys[] = {{{7, 0, 0, 0, false, true}, 0}, {{7, 0, 0, 0, true, true}, 3}};
    run_hand(&ad.cfg, 0, parmys, 2);

    ffc al = MAKE_CASE(FFREF_ALDFF, 4);
    al.cfg.ctl_high = false; /* ALOAD active low */
    const hand_step aldff[] = {{{1, 6, 0, 0, true, false}, 6}, {{1, 6, 0, 0, true, true}, 1}};
    run_hand(&al.cfg, 0, aldff, 2);

    ffc sr = MAKE_CASE(FFREF_DFFSRE, 4); /* clear wins over set; both over the enable */
    const hand_step dffsre[] = {{{8, 0, 3, 5, true, false}, 10}, /* force 0111: 0010, bit 3 = D */
                                {{0, 0, 0, 1, false, false}, 10 & ~1U},
                                {{15, 0, 0, 0, false, false}, 10 & ~1U}};
    run_hand(&sr.cfg, 0, dffsre, 3);
}

/* --- structure (spec §6.1's table) ---------------------------------------------------------- */

static uint32_t muxes_per_bit(ffref_kind kind) {
    static const uint32_t k_muxes[FFREF_KIND_COUNT] = {0, 1, 1, 2, 2, 1, 2, 1, 2, 2, 3};
    return k_muxes[kind];
}

static void check_structure(const ffc *cs, bool invert) {
    ffd dd;
    char msg[MSG_LEN];
    describe(cs, msg);
    ffd_make(&dd, cs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ffd_lower(&dd, invert ? "" : "--neg-edge keep"), msg);
    uint32_t width = cs->cfg.width;
    bool falling = !cs->clk_high;
    uint32_t nots = falling && invert ? 1U : 0U;
    lf *fix = &dd.fix;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(width * muxes_per_bit(cs->cfg.kind),
                                     lf_count_type(fix, dd.mod, "$_MUX_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(falling && !invert ? 0U : width,
                                     lf_count_type(fix, dd.mod, "$_DFF_P_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(falling && !invert ? width : 0U,
                                     lf_count_type(fix, dd.mod, "$_DFF_N_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(nots, lf_count_type(fix, dd.mod, "$_NOT_"), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(width * (muxes_per_bit(cs->cfg.kind) + 1U) + nots,
                                     lf_gate_nodes(fix, dd.mod), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_word_nodes(fix, dd.mod), msg);
    check_netlist_view(dd.mod);
    lf_end(fix);
}

static void test_structure_of_every_type_polarity_and_width(void) {
    static const uint32_t k_widths[] = {1, 4, 33};
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        for (uint32_t widx = 0; widx < 3; widx++) {
            for (uint32_t pol = 0; pol < 4; pol++) {
                ffc cs = MAKE_CASE((ffref_kind)kind, k_widths[widx]);
                cs.clk_high = (pol & 1U) != 0;
                cs.cfg.en_high = cs.cfg.ctl_high = cs.cfg.set_high = cs.cfg.clr_high =
                    (pol & 2U) != 0;
                check_structure(&cs, true);
                if (!cs.clk_high) {
                    check_structure(&cs, false);
                }
            }
        }
    }
}

static bool one_bit_port(const char *name) {
    static const char *const k_names[] = {"CLK", "EN", "ARST", "SRST", "ALOAD"};
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strcmp(name, k_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* lw_est_* of the case's cell, from its widths (W = 5 for the vector ports). */
static uint64_t estimate_of(ffd *dd, const ffc *cs) {
    const odin3_celltype_def *def =
        odin3_celltype_get(dd->fix.design, odin3_node_type(dd->mod, dd->node));
    uint32_t widths[LW_MAX_PORTS];
    for (uint32_t port = 0; port < def->n_ports; port++) {
        widths[port] = one_bit_port(def->ports[port].name) ? 1U : cs->cfg.width;
    }
    odin3_value params[MAX_PARAMS];
    uint8_t bits[FFREF_MAX_WIDTH];
    TEST_ASSERT_EQUAL_UINT32(def->n_params, make_params(cs, params, bits));
    return lf_estimate(def->name, params, widths, def->n_ports);
}

static void test_the_size_estimate_bounds_the_gates(void) {
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        for (uint32_t clk = 0; clk < 2; clk++) {
            ffc cs = MAKE_CASE((ffref_kind)kind, 5);
            cs.clk_high = clk != 0;
            ffd dd;
            ffd_make(&dd, &cs);
            uint64_t estimate = estimate_of(&dd, &cs);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, ""));
            TEST_ASSERT_TRUE_MESSAGE(lf_gate_nodes(&dd.fix, dd.mod) <= estimate,
                                     ffref_name((ffref_kind)kind));
            lf_end(&dd.fix);
        }
    }
}

/* --- init, --neg-edge, --async ---------------------------------------------------------------- */

/* The INIT parameter of the $_DFF_ node whose Q is the net of bit `bit` of wire Q; -1: none. */
static int init_of_bit(ffd *dd, uint32_t bit) {
    odin3_net_id want = lf_wnet(&dd->fix, dd->mod, "Q", bit);
    for (uint32_t id = 1; id < odin3_module_node_end(dd->mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(dd->mod, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(dd->fix.design, odin3_node_type(dd->mod, node));
        if (strncmp(def->name, "$_DFF_", 6) != 0) {
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
    static const ffref_kind k_kinds[] = {FFREF_DFF, FFREF_SDFFCE, FFREF_ADFF, FFREF_DFFSRE};
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        ffc cs = MAKE_CASE(k_kinds[i], 3);
        cs.init_def = 3U; /* bit 0 = 1, bit 1 = 0, bit 2 undefined */
        cs.init_val = 1U;
        ffd dd;
        ffd_make(&dd, &cs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, ""));
        TEST_ASSERT_EQUAL_INT(1, init_of_bit(&dd, 0));
        TEST_ASSERT_EQUAL_INT(0, init_of_bit(&dd, 1));
        TEST_ASSERT_EQUAL_INT(3, init_of_bit(&dd, 2));
        lf_end(&dd.fix);
        cs.init_def = 0; /* no init attribute: unknown */
        ffd_make(&dd, &cs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, ""));
        TEST_ASSERT_EQUAL_INT(3, init_of_bit(&dd, 0));
        lf_end(&dd.fix);
    }
}

static void test_neg_edge_invert_shares_one_not_and_warns_per_cell(void) {
    ffc cs = MAKE_CASE(FFREF_DFFE, 3);
    cs.clk_high = false;
    ffd dd;
    ffd_make(&dd, &cs);
    odin3_value params[MAX_PARAMS];
    uint8_t bits[FFREF_MAX_WIDTH];
    uint32_t count = make_params(&cs, params, bits);
    lf_cell_spec second = {"$dff", params, 2, "other", {0}};
    (void)count;
    (void)lf_cell_prefixed(&dd.fix, dd.mod, &second, "o_");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, "--neg-edge invert"));
    TEST_ASSERT_EQUAL_UINT32(2, lf_count_type(&dd.fix, dd.mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_UINT32(6, lf_count_type(&dd.fix, dd.mod, "$_DFF_P_"));
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&dd.fix, dd.mod, "$_DFF_N_"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "lower: module 'm': falling-edge '$dffe' 'dut' gets a clock inverter"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "lower: module 'm': falling-edge '$dff' 'other' gets a clock inverter"));
    lf_end(&dd.fix);
    log_text[0] = '\0';
    ffd_make(&dd, &cs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, "--neg-edge keep"));
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&dd.fix, dd.mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_UINT32(3, lf_count_type(&dd.fix, dd.mod, "$_DFF_N_"));
    TEST_ASSERT_NULL(strstr(log_text, "falling-edge"));
    lf_end(&dd.fix);
}

static void test_async_error_names_the_cell_and_changes_nothing(void) {
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        ffc cs = MAKE_CASE((ffref_kind)kind, 2);
        ffd dd;
        ffd_make(&dd, &cs);
        odin3_strbuf before;
        odin3_strbuf after;
        odin3_strbuf_init(&before);
        odin3_strbuf_init(&after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &before));
        log_text[0] = '\0';
        odin3_status st = ffd_lower(&dd, "--async error");
        char want[96];
        (void)snprintf(want, sizeof want, "lower: module 'm': asynchronous control on '%s' 'dut'",
                       ffref_name((ffref_kind)kind));
        if (ffref_is_async((ffref_kind)kind)) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, st, want);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, want), want);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &after));
            TEST_ASSERT_EQUAL_STRING(before.data, after.data);
            TEST_ASSERT_EQUAL_UINT32(1, lf_word_nodes(&dd.fix, dd.mod));
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, want);
            TEST_ASSERT_EQUAL_UINT32(0, lf_word_nodes(&dd.fix, dd.mod));
        }
        odin3_strbuf_free(&before);
        odin3_strbuf_free(&after);
        lf_end(&dd.fix);
    }
}

/* Three async cells in two modules (9 bits in 3 cells) and a synchronous one. */
static void build_mixed_design(lf *fix) {
    lf_begin(fix);
    odin3_module *m1 = lf_module(fix, "m1");
    odin3_module *m2 = lf_module(fix, "m2");
    static const struct {
        ffref_kind kind;
        uint32_t width;
        bool in_m2;
        const char *prefix;
        const char *name;
    } k_cells[] = {{FFREF_ADFF, 4, false, "a_", "ca"},
                   {FFREF_ALDFF, 3, false, "b_", "cb"},
                   {FFREF_DFFSR, 2, true, "c_", "cc"},
                   {FFREF_SDFF, 7, false, "d_", "cd"}};
    for (size_t i = 0; i < sizeof k_cells / sizeof k_cells[0]; i++) {
        ffc cs = MAKE_CASE(k_cells[i].kind, k_cells[i].width);
        odin3_value params[MAX_PARAMS];
        uint8_t bits[FFREF_MAX_WIDTH];
        uint32_t count = make_params(&cs, params, bits);
        lf_cell_spec spec = {ffref_name(k_cells[i].kind), params, count, k_cells[i].name, {0}};
        (void)lf_cell_prefixed(fix, k_cells[i].in_m2 ? m2 : m1, &spec, k_cells[i].prefix);
    }
}

static void test_the_async_warning_is_once_per_design_with_exact_counts(void) {
    lf fix;
    build_mixed_design(&fix);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(fix.design, "lower", odin3_bytes_cstr("")));
    const char *want =
        "lower: asynchronous reset/set/load made synchronous on 9 flip-flop bits in 3 cells "
        "(PHASE3 decision 19); the netlist differs from the RTL on reset timing; see --report";
    const char *first = strstr(log_text, want);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + strlen(want), "asynchronous reset/set/load made synchronous"));
    lf_end(&fix);
    log_text[0] = '\0';
    ffc cs = MAKE_CASE(FFREF_SDFFE, 3); /* a synchronous design: no warning */
    ffd dd;
    ffd_make(&dd, &cs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ffd_lower(&dd, ""));
    TEST_ASSERT_NULL(strstr(log_text, "asynchronous reset/set/load"));
    lf_end(&dd.fix);
}

static void assert_has(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), needle);
}

static void assert_lacks(const char *text, const char *needle) {
    TEST_ASSERT_NULL_MESSAGE(strstr(text, needle), needle);
}

static void test_the_report_lists_async_and_neg_edge_cells(void) {
    lf fix;
    build_mixed_design(&fix);
    static const char k_path[] = "test_lower_ff.report.json";
    char args[64];
    (void)snprintf(args, sizeof args, "--report %s", k_path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(fix.design, "lower", odin3_bytes_cstr(args)));
    char text[2048] = "";
    FILE *file = fopen(k_path, "r");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(text, 1, sizeof text - 1, file);
    text[got] = '\0';
    (void)fclose(file);
    assert_has(text, "\"async_to_sync\": [");
    assert_has(text, "{\"module\": \"m1\", \"cell\": \"ca\", \"type\": \"$adff\", \"bits\": 4, "
                     "\"controls\": [\"ARST\"]}");
    assert_has(text, "\"type\": \"$aldff\", \"bits\": 3, \"controls\": [\"ALOAD\"]");
    assert_has(text, "{\"module\": \"m2\", \"cell\": \"cc\", \"type\": \"$dffsr\", \"bits\": 2, "
                     "\"controls\": [\"SET\", \"CLR\"]}");
    assert_lacks(text, "\"type\": \"$sdff\"");
    assert_has(text, "\"neg_edge\": []");
    lf_end(&fix);
}

/* --- behaviour ------------------------------------------------------------------------------ */

static void test_w1_exhaustive_short_sequences_for_every_type_and_init(void) {
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        for (uint32_t init = 0; init < 3; init++) {
            ffc cs = MAKE_CASE((ffref_kind)kind, 1);
            cs.cfg.value = init & 1U;
            cs.init_def = init < 2 ? 1U : 0U;
            cs.init_val = init;
            ffd dd;
            lower_case(&dd, &cs);
            exhaustive_w1(&dd, &cs, init == 0 ? 5U : 3U);
            lf_end(&dd.fix);
        }
    }
}

/* The case of polarity code `pol` (5 bits), or false when it names a control the type lacks. */
static bool polarity_case(ffc *cs, uint32_t pol) {
    cs->clk_high = (pol & 1U) != 0;
    cs->cfg.en_high = (pol & 2U) != 0;
    cs->cfg.ctl_high = (pol & 4U) != 0;
    cs->cfg.set_high = (pol & 8U) != 0;
    cs->cfg.clr_high = (pol & 16U) != 0;
    cs->cfg.value = (pol >> 1) & 1U;
    ffref_kind kind = cs->cfg.kind;
    bool no_en = !ffref_has_en(kind) && !cs->cfg.en_high;
    bool no_ctl = ctl_name(kind) == NULL && !cs->cfg.ctl_high;
    bool no_setclr = !has_setclr(kind) && (!cs->cfg.set_high || !cs->cfg.clr_high);
    return !no_en && !no_ctl &&
           !no_setclr; /* a missing control's polarity: the case is done once */
}

static void test_w1_every_polarity_visits_every_state_and_input(void) {
    uint64_t seed = 1;
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        for (uint32_t pol = 0; pol < 32; pol++) {
            ffc cs = MAKE_CASE((ffref_kind)kind, 1);
            if (!polarity_case(&cs, pol)) {
                continue;
            }
            ffd dd;
            lower_case(&dd, &cs);
            random_walk(&dd, &cs, &(walk_opts){seed++, WALK_STEPS});
            lf_end(&dd.fix);
        }
    }
}

static void test_wide_cells_follow_the_stepper_on_random_sequences(void) {
    static const uint32_t k_widths[] = {4, 33};
    uint64_t seed = 77;
    for (uint32_t kind = 0; kind < FFREF_KIND_COUNT; kind++) {
        for (uint32_t widx = 0; widx < 2; widx++) {
            for (uint32_t pol = 0; pol < 4; pol++) {
                ffc cs = MAKE_CASE((ffref_kind)kind, k_widths[widx]);
                cs.clk_high = (pol & 1U) != 0;
                cs.cfg.en_high = cs.cfg.ctl_high = cs.cfg.set_high = (pol & 2U) != 0;
                cs.cfg.clr_high = (pol & 2U) == 0;
                cs.cfg.value = 0x5A5A5A5A5AULL * (seed % 7 + 1);
                cs.init_def = 0x0F0F0F0F0FULL;
                cs.init_val = 0xA5A5A5A5A5ULL;
                ffd dd;
                lower_case(&dd, &cs);
                random_walk(&dd, &cs, &(walk_opts){seed++, 120});
                lf_end(&dd.fix);
            }
        }
    }
}

static void test_the_enable_first_variant_matches_its_own_reference(void) {
    static const ffref_kind k_kinds[] = {FFREF_ADFFE, FFREF_ALDFFE, FFREF_DFFSRE};
    uint64_t seed = 900;
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        for (uint32_t width = 1; width <= 4; width += 3) {
            ffc cs = MAKE_CASE(k_kinds[i], width);
            cs.cfg.enable_first = true;
            cs.cfg.value = 2;
            cs.args = "--async-enable-first";
            ffd dd;
            lower_case(&dd, &cs);
            if (width == 1) {
                exhaustive_w1(&dd, &cs, 5U);
            } else {
                random_walk(&dd, &cs, &(walk_opts){seed++, 200});
            }
            lf_end(&dd.fix);
        }
    }
}

/* Spec §16 row 4: the default and the Parmys shape differ exactly with the enable low and the
 * reset (load, set or clear) active; a counter-example sequence for each. */
static void test_the_two_enable_orders_differ_on_a_counter_example(void) {
    static const ffref_kind k_kinds[] = {FFREF_ADFFE, FFREF_ALDFFE, FFREF_DFFSRE};
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        ffc cs = MAKE_CASE(k_kinds[i], 1);
        cs.cfg.value = 1;
        ffref_in in;
        memset(&in, 0, sizeof in);
        in.load = 1;
        in.set = 1;
        in.ctl = true;
        in.en = false; /* the enable is low, the control active, the value 1, the state 0 */
        uint64_t reset_first = ffref_step(&cs.cfg, 0, &in);
        cs.cfg.enable_first = true;
        uint64_t enable_first = ffref_step(&cs.cfg, 0, &in);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(1, reset_first, ffref_name(k_kinds[i]));
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, enable_first, ffref_name(k_kinds[i]));
        cs.cfg.enable_first = false;
        ffd dd;
        lower_case(&dd, &cs);
        int bad = run_sequence(&dd, &cs, &in, 1);
        TEST_ASSERT_EQUAL_INT(0, bad); /* the netlist takes the first order */
        lf_end(&dd.fix);
    }
}

/* --- out of memory ---------------------------------------------------------------------------- */

static void test_oom_at_every_allocation_leaves_a_valid_design(void) {
    static const ffref_kind k_kinds[] = {FFREF_SDFFE, FFREF_ADFFE, FFREF_DFFSRE};
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        ffc cs = MAKE_CASE(k_kinds[i], 4);
        cs.clk_high = false;
        bool done = false;
        for (long step = 0; !done && step < 2000; step++) {
            ffd dd;
            ffd_make(&dd, &cs);
            odin3_strbuf before;
            odin3_strbuf after;
            odin3_strbuf_init(&before);
            odin3_strbuf_init(&after);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&dd.fix, &before));
            odin3_util_set_alloc_fail_after(step);
            odin3_status st = ffd_lower(&dd, "");
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
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_stepper_matches_hand_computed_sequences);
    RUN_TEST(test_structure_of_every_type_polarity_and_width);
    RUN_TEST(test_the_size_estimate_bounds_the_gates);
    RUN_TEST(test_init_values_come_from_the_init_attribute);
    RUN_TEST(test_neg_edge_invert_shares_one_not_and_warns_per_cell);
    RUN_TEST(test_async_error_names_the_cell_and_changes_nothing);
    RUN_TEST(test_the_async_warning_is_once_per_design_with_exact_counts);
    RUN_TEST(test_the_report_lists_async_and_neg_edge_cells);
    RUN_TEST(test_w1_exhaustive_short_sequences_for_every_type_and_init);
    RUN_TEST(test_w1_every_polarity_visits_every_state_and_input);
    RUN_TEST(test_wide_cells_follow_the_stepper_on_random_sequences);
    RUN_TEST(test_the_enable_first_variant_matches_its_own_reference);
    RUN_TEST(test_the_two_enable_orders_differ_on_a_counter_example);
    RUN_TEST(test_oom_at_every_allocation_leaves_a_valid_design);
    return UNITY_END();
}
