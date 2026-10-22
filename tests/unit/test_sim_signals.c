/*
 * test_sim_signals.c — S1a: settle, clock edge, reset, the word-cell hint, frames kept, and the
 * signal table (scopes, HDL names, lookup, read and write).
 */
#include "ir/design.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "sim/vectors.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 4096, CYCLES = 64, SEED_A = 11, SEED_B = 9, SEED_C = 4, RESET_CYCLES = 20 };

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

/* --- fixtures --------------------------------------------------------------------------------- */

static void run_pass(const char *name, const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pass_run(design, name, odin3_bytes_cstr(args)),
                                  name);
}

/* A fixture file and the passes (space separated) to run after reading it. */
typedef struct fixture {
    const char *file;
    const char *tail;
} fixture;

/* Reads the Verilog fixture, then runs each pass of its tail. */
static void load_v(fixture fix) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_SIM_FIXTURES, fix.file);
    run_pass("read_verilog", path);
    char word[32];
    const char *cur = fix.tail;
    while (*cur != '\0') {
        size_t len = strcspn(cur, " ");
        (void)snprintf(word, sizeof word, "%.*s", (int)len, cur);
        if (len > 0) {
            run_pass(word, "");
        }
        cur += len;
        cur += (*cur == ' ') ? 1 : 0;
    }
}

static odin3_status build(odin3_sim **sim) {
    return odin3_sim_build(design, odin3_design_top(design), sim);
}

static odin3_sim *build_ok(void) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build(&sim));
    TEST_ASSERT_NOT_NULL(sim);
    return sim;
}

/* --- Task 1 ----------------------------------------------------------------------------------- */

static void test_settle_edges_equal_cycle(void) {
    load_v((fixture){"traffic.v", "proc opt lower"});
    odin3_sim *sim_a = build_ok();
    odin3_sim *sim_b = build_ok();
    odin3_prng prng_a;
    odin3_prng prng_b;
    odin3_prng_seed(&prng_a, SEED_A);
    odin3_prng_seed(&prng_b, SEED_A);
    for (uint32_t cyc = 0; cyc < CYCLES; cyc++) {
        odin3_sim_drive_random(sim_a, &prng_a);
        odin3_sim_drive_random(sim_b, &prng_b);
        odin3_sim_cycle(sim_a);
        odin3_sim_settle(sim_b);
        odin3_sim_clock_edge(sim_b, ODIN3_SIM_POSEDGE);
        odin3_sim_clock_edge(sim_b, ODIN3_SIM_NEGEDGE);
        for (uint32_t port = 0; port < odin3_sim_output_count(sim_a); port++) {
            for (uint32_t bit = 0; bit < odin3_sim_output_width(sim_a, port); bit++) {
                bool val_a = false;
                bool val_b = true;
                TEST_ASSERT_EQUAL_INT(
                    ODIN3_OK, odin3_sim_get_output(sim_a, (odin3_sim_bit){port, bit}, &val_a));
                TEST_ASSERT_EQUAL_INT(
                    ODIN3_OK, odin3_sim_get_output(sim_b, (odin3_sim_bit){port, bit}, &val_b));
                TEST_ASSERT_EQUAL_INT(val_a, val_b);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(sim_a->n_hook_calls, sim_b->n_hook_calls);
    odin3_sim_destroy(sim_a);
    odin3_sim_destroy(sim_b);
}

/* Runs run->cycles vector lines of seed run->seed on sim into buf. */
static void run_vectors(odin3_sim *sim, const odin3_sim_vectors *run, odin3_strbuf *buf) {
    odin3_prng prng;
    odin3_prng_seed(&prng, run->seed);
    for (uint32_t cyc = 0; cyc < run->cycles; cyc++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_step(sim, &prng, cyc, buf));
    }
}

static void test_reset_equals_fresh_build(void) {
    load_v((fixture){"traffic.v", "proc opt lower"});
    odin3_sim *sim_a = build_ok();
    odin3_strbuf warm;
    odin3_strbuf_init(&warm);
    run_vectors(sim_a, &(odin3_sim_vectors){"warm", SEED_B, RESET_CYCLES}, &warm);
    odin3_strbuf_free(&warm);
    odin3_sim_reset(sim_a);
    for (uint32_t port = 0; port < odin3_sim_input_count(sim_a); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim_a, port); bit++) {
            bool val = true;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_input(sim_a, (odin3_sim_bit){port, bit}, &val));
            TEST_ASSERT_FALSE(val);
        }
    }
    odin3_sim *sim_b = build_ok();
    odin3_strbuf out_a;
    odin3_strbuf out_b;
    odin3_strbuf_init(&out_a);
    odin3_strbuf_init(&out_b);
    const odin3_sim_vectors run = {"check", SEED_C, CYCLES / 2};
    run_vectors(sim_a, &run, &out_a);
    run_vectors(sim_b, &run, &out_b);
    TEST_ASSERT_EQUAL_STRING(out_a.data, out_b.data);
    odin3_strbuf_free(&out_a);
    odin3_strbuf_free(&out_b);
    odin3_sim_destroy(sim_a);
    odin3_sim_destroy(sim_b);
}

static void test_word_cell_hint(void) {
    load_v((fixture){"traffic.v", "proc opt"});
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, build(&sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "the cell type has no simulate hook; run `lower` first"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_settle_edges_equal_cycle);
    RUN_TEST(test_reset_equals_fresh_build);
    RUN_TEST(test_word_cell_hint);
    return UNITY_END();
}
