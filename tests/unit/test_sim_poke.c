/*
 * test_sim_poke.c — odin3_sim_poke_state and odin3_sim_signals_poke: `lower --keep-mem` names the
 * words of a memory (`rom[3]`), a poke sets the flip-flops behind a word, the next settle shows it
 * on the outputs; a signal that is not flip-flop state is refused.
 */
#include "ir/design.h"
#include "passes/manager.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/str.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WORD_BITS = 8, WORD = 3, CODE_X = 2, BAD_CODE = 4 };

static odin3_design *design;
static odin3_sim *sim;
static odin3_sim_signals *sigs;

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    char path[512];
    (void)snprintf(path, sizeof path, "%s/poke_mem.v", ODIN3_SIM_FIXTURES);
    const char *steps[][2] = {
        {"read_verilog", path}, {"proc", ""}, {"opt", ""}, {"lower", "--keep-mem"}};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_pass_run(design, steps[i][0], odin3_bytes_cstr(steps[i][1])));
    }
    odin3_sim_options opts = {.four_state = true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build_opts(design, odin3_design_top(design), &opts, &sim));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_create(sim, &sigs));
}

void tearDown(void) {
    odin3_sim_signals_destroy(sigs);
    odin3_sim_destroy(sim);
    odin3_design_destroy(design);
}

static uint32_t find(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_find(sigs, odin3_bytes_cstr(name), &id));
    return id;
}

static void test_poked_rom_word_reaches_the_output(void) {
    uint32_t word = find("rom[3]");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, word);
    uint8_t codes[WORD_BITS] = {1, 0, 1, 0, 0, 1, 0, 1}; /* 0xa5, LSB first */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_poke(sigs, word, codes, WORD_BITS, NULL));
    for (uint32_t bit = 0; bit < 3U; bit++) {
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_sim_set_input4(sim, (odin3_sim_bit){1, bit}, (WORD >> bit) & 1U));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_settle_events(sim));
    uint8_t out[WORD_BITS];
    uint32_t data = find("data");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_read(sigs, data, out, WORD_BITS));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(codes, out, WORD_BITS);
}

static void test_unpoked_word_is_unknown(void) {
    uint8_t out[WORD_BITS];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_read(sigs, find("rom[5]"), out, WORD_BITS));
    TEST_ASSERT_EQUAL_UINT8(CODE_X, out[0]);
}

static void test_refusals(void) {
    uint8_t codes[WORD_BITS] = {0};
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_signals_poke(sigs, find("data"), codes, WORD_BITS, &why));
    TEST_ASSERT_EQUAL_STRING("a bit is not the output of a flip-flop", why);
    codes[2] = BAD_CODE;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_signals_poke(sigs, find("rom[1]"), codes, WORD_BITS, &why));
    TEST_ASSERT_EQUAL_STRING("a code is out of range", why);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_signals_poke(sigs, find("rom[1]"), codes, 1, &why));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_sim_poke_state(sim, 0, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_poked_rom_word_reaches_the_output);
    RUN_TEST(test_unpoked_word_is_unknown);
    RUN_TEST(test_refusals);
    return UNITY_END();
}
