/*
 * test_sim_four_state.c — the 4-state netlist simulator (design note
 * docs/specs/2026-11-05-sim-4state-design.md): everything starts unknown, a reset makes the
 * registers known, and from then on a 4-state run equals the 2-state run of the same stimulus.
 */
#include "ir/design.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CYCLES = 200, SEED = 5, RESET_CYCLES = 3, CODE_X = 2 };

static odin3_design *design;

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

static void run_pass(const char *name, const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pass_run(design, name, odin3_bytes_cstr(args)),
                                  name);
}

static odin3_sim *build(bool four_state) {
    odin3_sim_options opts = {.four_state = four_state};
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build_opts(design, odin3_design_top(design), &opts, &sim));
    return sim;
}

static void load_traffic(void) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/traffic.v", ODIN3_SIM_FIXTURES);
    run_pass("read_verilog", path);
    run_pass("proc", "");
    run_pass("opt", "");
    run_pass("lower", "");
}

static uint32_t input_named(const odin3_sim *sim, const char *name) {
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        if (strcmp(odin3_sim_input_name(sim, port), name) == 0) {
            return port;
        }
    }
    TEST_FAIL_MESSAGE("no such input");
    return 0;
}

static void test_starts_unknown_and_reset_makes_it_known(void) {
    load_traffic();
    odin3_sim *sim = build(true);
    TEST_ASSERT_TRUE(odin3_sim_is_four_state(sim));
    uint8_t code = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output4(sim, (odin3_sim_bit){0, 0}, &code));
    TEST_ASSERT_EQUAL_UINT8(CODE_X, code);
    uint32_t rst = input_named(sim, "rst");
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        if (port != rst && !odin3_sim_input_is_clock(sim, (odin3_sim_bit){port, 0})) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input4(sim, (odin3_sim_bit){port, 0}, 0));
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input4(sim, (odin3_sim_bit){rst, 0}, 1));
    for (uint32_t i = 0; i < RESET_CYCLES; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_cycle(sim));
    }
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_output4(sim, (odin3_sim_bit){port, bit}, &code));
            TEST_ASSERT_TRUE_MESSAGE(code <= 1U, "output still unknown after reset");
        }
    }
    odin3_sim_destroy(sim);
}

static void test_known_runs_equal_two_state(void) {
    load_traffic();
    odin3_sim *two = build(false);
    odin3_sim *four = build(true);
    uint32_t rst = input_named(four, "rst");
    odin3_prng prng_two;
    odin3_prng prng_four;
    odin3_prng_seed(&prng_two, SEED);
    odin3_prng_seed(&prng_four, SEED);
    for (uint32_t cyc = 0; cyc < CYCLES; cyc++) {
        odin3_sim_drive_random(two, &prng_two);
        odin3_sim_drive_random(four, &prng_four);
        bool reset = cyc < RESET_CYCLES;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(two, (odin3_sim_bit){rst, 0}, reset));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(four, (odin3_sim_bit){rst, 0}, reset));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_cycle(two));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_cycle(four));
        if (cyc < RESET_CYCLES) {
            continue;
        }
        for (uint32_t port = 0; port < odin3_sim_output_count(two); port++) {
            for (uint32_t bit = 0; bit < odin3_sim_output_width(two, port); bit++) {
                uint8_t code_two = 0;
                uint8_t code_four = 0;
                (void)odin3_sim_get_output4(two, (odin3_sim_bit){port, bit}, &code_two);
                (void)odin3_sim_get_output4(four, (odin3_sim_bit){port, bit}, &code_four);
                TEST_ASSERT_EQUAL_UINT8(code_two, code_four);
            }
        }
    }
    odin3_sim_destroy(two);
    odin3_sim_destroy(four);
}

static void test_two_state_refuses_unknown_codes(void) {
    load_traffic();
    odin3_sim *two = build(false);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_set_input4(two, (odin3_sim_bit){0, 0}, CODE_X));
    odin3_sim_destroy(two);
}

/* if (x) takes the else branch, case (x) the default, ?: merges (what iverilog does). */
static void test_if_case_and_conditional_with_unknown_selector(void) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/ifx.v", ODIN3_SIM_FIXTURES);
    run_pass("read_verilog", path);
    run_pass("proc", "");
    run_pass("opt", "");
    run_pass("lower", "--keep-sel");
    odin3_sim *sim = build(true);
    static const char *const k_in[] = {"c", "s", "a", "b"};
    static const uint8_t k_val[] = {CODE_X, CODE_X, 1, 0};
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t port = input_named(sim, k_in[i]);
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim, port); bit++) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_set_input4(sim, (odin3_sim_bit){port, bit}, k_val[i]));
        }
    }
    odin3_sim_settle(sim);
    uint8_t code = 9;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        const char *name = odin3_sim_output_name(sim, port);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_sim_get_output4(sim, (odin3_sim_bit){port, 0}, &code));
        uint8_t want = strcmp(name, "t") == 0 ? (uint8_t)CODE_X : (uint8_t)0;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, code, name);
    }
    odin3_sim_destroy(sim);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_unknown_and_reset_makes_it_known);
    RUN_TEST(test_known_runs_equal_two_state);
    RUN_TEST(test_two_state_refuses_unknown_codes);
    RUN_TEST(test_if_case_and_conditional_with_unknown_selector);
    return UNITY_END();
}
