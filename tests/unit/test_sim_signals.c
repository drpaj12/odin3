/*
 * test_sim_signals.c — S1a: settle, clock edge, reset, the word-cell hint, frames kept, and the
 * signal table (scopes, HDL names, lookup, read and write).
 */
#include "ir/design.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/signals.h"
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

enum {
    LOG_TEXT = 4096,
    CYCLES = 64,
    SEED_A = 11,
    SEED_B = 9,
    SEED_C = 4,
    RESET_CYCLES = 20,
    MAX_BITS = 8,
    BIG = 100000
};

static odin3_design *design;
static odin3_sim *g_sim;
static odin3_sim_signals *g_sig;
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
    odin3_sim_signals_destroy(g_sig);
    g_sig = NULL;
    odin3_sim_destroy(g_sim);
    g_sim = NULL;
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

/* --- Task 2: the signal table ----------------------------------------------------------------- */

static void open_table(void) {
    g_sim = build_ok();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_create(g_sim, &g_sig));
    TEST_ASSERT_NOT_NULL(g_sig);
}

static void load_blif(const char *file) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_SIM_FIXTURES, file);
    run_pass("read_blif", path);
}

/* The ID of `name`, asserting that the lookup itself succeeds (0: not found). */
static uint32_t id_of(const char *name) {
    uint32_t id = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK,
                                  odin3_sim_signals_find(g_sig, odin3_bytes_cstr(name), &id), name);
    return id;
}

static const odin3_sim_signal *sig_at(uint32_t id) {
    const odin3_sim_signal *sg = odin3_sim_signals_at(g_sig, id);
    TEST_ASSERT_NOT_NULL(sg);
    return sg;
}

static const char *text(uint32_t str) {
    return odin3_sim_signals_text(g_sig, str);
}

/* Writes `value` (LSB first) to the signal `name`. */
static void put(const char *name, uint32_t value) {
    uint32_t id = id_of(name);
    const odin3_sim_signal *sg = sig_at(id);
    uint8_t bits[MAX_BITS] = {0};
    TEST_ASSERT_TRUE(sg->width <= MAX_BITS);
    for (uint32_t k = 0; k < sg->width; k++) {
        bits[k] = (uint8_t)((value >> k) & 1U);
    }
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_write(g_sig, id, bits, sg->width, &why));
}

/* Reads the signal `name` (LSB first). */
static uint32_t peek(const char *name) {
    uint32_t id = id_of(name);
    const odin3_sim_signal *sg = sig_at(id);
    uint8_t bits[MAX_BITS] = {0};
    uint32_t value = 0;
    TEST_ASSERT_TRUE(sg->width <= MAX_BITS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_read(g_sig, id, bits, sg->width));
    for (uint32_t k = 0; k < sg->width; k++) {
        value |= (uint32_t)bits[k] << k;
    }
    return value;
}

static void test_hier_scopes(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    open_table();
    TEST_ASSERT_EQUAL_UINT32(3, odin3_sim_signals_scope_count(g_sig));
    const char *const paths[] = {"", "u0", "u1"};
    const char *const leaves[] = {"hier", "u0", "u1"};
    for (uint32_t sc = 0; sc < 3; sc++) {
        const odin3_sim_scope *scope = odin3_sim_signals_scope(g_sig, sc);
        TEST_ASSERT_NOT_NULL(scope);
        TEST_ASSERT_EQUAL_STRING(paths[sc], text(scope->name));
        TEST_ASSERT_EQUAL_STRING(leaves[sc], text(scope->leaf));
        TEST_ASSERT_EQUAL_UINT32(0, scope->parent);
    }
    TEST_ASSERT_NULL(odin3_sim_signals_scope(g_sig, 3));
}

static void test_hier_listed_names(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    open_table();
    const char *const want[] = {"clk",  "a",    "y",        "z",      "mid",  "kept_r", "u0.clk",
                                "u0.d", "u0.q", "u0.inner", "u1.clk", "u1.d", "u1.q",   "u1.inner"};
    const uint32_t kind[] = {ODIN3_SIM_KIND_CLOCK, ODIN3_SIM_KIND_INPUT, ODIN3_SIM_KIND_OUTPUT,
                             ODIN3_SIM_KIND_OUTPUT};
    TEST_ASSERT_EQUAL_UINT32(14, odin3_sim_signals_listed(g_sig));
    for (uint32_t id = 1; id <= 14; id++) {
        const odin3_sim_signal *sg = sig_at(id);
        TEST_ASSERT_EQUAL_STRING(want[id - 1], text(sg->name));
        TEST_ASSERT_EQUAL_UINT32(id <= 4 ? kind[id - 1] : ODIN3_SIM_KIND_INTERNAL, sg->kind);
    }
    const odin3_sim_signal *sg = sig_at(2);
    TEST_ASSERT_EQUAL_UINT32(4, sg->width);
    TEST_ASSERT_EQUAL_INT32(3, sg->msb);
    TEST_ASSERT_EQUAL_INT32(0, sg->lsb);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OBJ_WIRE, sg->obj_kind);
    TEST_ASSERT_EQUAL_UINT32(0, sg->scope);
    sg = sig_at(10);
    TEST_ASSERT_EQUAL_UINT32(1, sg->scope);
    TEST_ASSERT_EQUAL_STRING("inner", text(sg->leaf));
    TEST_ASSERT_EQUAL_UINT32(1, sg->width);
}

static void test_find(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    open_table();
    TEST_ASSERT_EQUAL_UINT32(10, id_of("u0.inner"));
    TEST_ASSERT_EQUAL_UINT32(15, id_of("a[2]"));
    const odin3_sim_signal *sg = sig_at(15);
    TEST_ASSERT_EQUAL_UINT32(1, sg->width);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_INPUT, sg->kind);
    TEST_ASSERT_EQUAL_INT32(2, sg->msb);
    TEST_ASSERT_EQUAL_INT32(2, sg->lsb);
    TEST_ASSERT_EQUAL_UINT32(2, sg->first_bit);
    TEST_ASSERT_EQUAL_STRING("a[2]", text(sg->leaf));
    TEST_ASSERT_EQUAL_UINT32(15, id_of("a[2]"));
    TEST_ASSERT_EQUAL_UINT32(16, id_of("u1.q[0]"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_INTERNAL, sig_at(16)->kind);
    TEST_ASSERT_EQUAL_UINT32(0, id_of("dead_r"));
    TEST_ASSERT_EQUAL_UINT32(0, id_of("nope"));
    TEST_ASSERT_EQUAL_UINT32(0, id_of("u2.q"));
    TEST_ASSERT_EQUAL_UINT32(0, id_of("a[4]"));
    TEST_ASSERT_EQUAL_UINT32(0, id_of("a[x]"));
    TEST_ASSERT_EQUAL_UINT32(16, odin3_sim_signals_total(g_sig));
    TEST_ASSERT_EQUAL_UINT32(14, odin3_sim_signals_listed(g_sig));
}

static void test_netnames_listing_blif(void) {
    load_blif("netnames.blif");
    open_table();
    const char *const want[] = {"clk", "a", "b", "y", "n1", "q"};
    TEST_ASSERT_EQUAL_UINT32(6, odin3_sim_signals_listed(g_sig));
    for (uint32_t id = 1; id <= 6; id++) {
        TEST_ASSERT_EQUAL_STRING(want[id - 1], text(sig_at(id)->name));
    }
    const odin3_sim_signal *net = sig_at(5);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OBJ_NET, net->obj_kind);
    TEST_ASSERT_EQUAL_UINT32(1, net->width);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_INTERNAL, net->kind);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_CLOCK, sig_at(1)->kind);
}

/* Drives hier: a = 1 for one cycle, then 0; z shows the value 4 cycles later. */
static void check_pipeline(void) {
    put("a", 1);
    odin3_sim_cycle(g_sim);
    put("a", 0);
    TEST_ASSERT_EQUAL_UINT32(1, peek("u0.inner"));
    TEST_ASSERT_EQUAL_UINT32(0, peek("z"));
    odin3_sim_cycle(g_sim);
    TEST_ASSERT_EQUAL_UINT32(1, peek("u0.q"));
    odin3_sim_cycle(g_sim);
    odin3_sim_cycle(g_sim);
    TEST_ASSERT_EQUAL_UINT32(1, peek("z"));
    odin3_sim_cycle(g_sim);
    TEST_ASSERT_EQUAL_UINT32(0, peek("z"));
}

static void test_values_follow_pipeline(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    open_table();
    TEST_ASSERT_EQUAL_UINT32(0, peek("mid"));
    check_pipeline();
    put("a", 5);
    TEST_ASSERT_EQUAL_UINT32(0xF, peek("y"));
    odin3_sim_settle(g_sim);
    TEST_ASSERT_EQUAL_UINT32(0xA, peek("y"));
}

static void test_flattened_names(void) {
    load_v((fixture){"hier.v", "proc opt lower flatten"});
    open_table();
    TEST_ASSERT_EQUAL_UINT32(1, odin3_sim_signals_scope_count(g_sig));
    TEST_ASSERT_NOT_EQUAL(0, id_of("u0.inner"));
    TEST_ASSERT_EQUAL_UINT32(0, id_of("dead_r"));
    check_pipeline();
}

/* The reason a write is refused: a one-bit-per-byte value of `count` bytes for signal `name`. */
static const char *refusal(const char *name, const uint8_t *bits, uint32_t count) {
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_signals_write(g_sig, id_of(name), bits, count, &why));
    TEST_ASSERT_NOT_NULL(why);
    return why;
}

static void test_write_rules(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    open_table();
    const uint8_t zeros[4] = {0, 0, 0, 0};
    const uint8_t bad[4] = {0, 2, 0, 0};
    const char *why = NULL;
    TEST_ASSERT_EQUAL_STRING("not an input", refusal("y", zeros, 4));
    TEST_ASSERT_EQUAL_STRING("not an input", refusal("clk", zeros, 1));
    TEST_ASSERT_EQUAL_STRING("count is not the width", refusal("a", zeros, 3));
    TEST_ASSERT_EQUAL_STRING("a bit is not 0 or 1", refusal("a", bad, 4));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_signals_write(g_sig, 999, zeros, 4, &why));
    TEST_ASSERT_EQUAL_STRING("unknown signal", why);
    TEST_ASSERT_EQUAL_UINT32(0, peek("a"));
}

static void test_mixed_clock_port(void) {
    load_v((fixture){"mixclk.v", "proc opt lower"});
    open_table();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_INPUT, sig_at(id_of("ck"))->kind);
    const uint8_t low_high[2] = {0, 1};
    const uint8_t high_low[2] = {1, 0};
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_write(g_sig, id_of("ck"), low_high, 2, &why));
    bool val = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_input(g_sim, (odin3_sim_bit){0, 1}, &val));
    TEST_ASSERT_TRUE(val);
    TEST_ASSERT_EQUAL_STRING("a clock bit set to 1", refusal("ck", high_low, 2));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_CLOCK, sig_at(id_of("ck[0]"))->kind);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIM_KIND_INPUT, sig_at(id_of("ck[1]"))->kind);
}

/* One create with the fail-th allocation failing; true when it succeeded. */
static bool create_failing_at(int fail) {
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_sim_signals_create(g_sim, &g_sig);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_EQUAL_INT(st == ODIN3_OK, g_sig != NULL);
    return st == ODIN3_OK;
}

/* One find of "a[2]" with the fail-th allocation failing; true when it succeeded. */
static bool find_failing_at(int fail) {
    uint32_t id = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_sim_signals_find(g_sig, odin3_bytes_cstr("a[2]"), &id);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_EQUAL_UINT32(st == ODIN3_OK ? 15 : 14, odin3_sim_signals_total(g_sig));
    TEST_ASSERT_EQUAL_UINT32(st == ODIN3_OK ? 15 : 0, id);
    return st == ODIN3_OK;
}

static void test_signals_oom(void) {
    load_v((fixture){"hier.v", "proc opt lower"});
    g_sim = build_ok();
    int fail = 0;
    while (!create_failing_at(fail)) {
        TEST_ASSERT_TRUE(++fail < BIG);
    }
    odin3_sim_signals_destroy(g_sig);
    g_sig = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_create(g_sim, &g_sig));
    fail = 0;
    while (!find_failing_at(fail)) {
        TEST_ASSERT_TRUE(++fail < BIG);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_settle_edges_equal_cycle);
    RUN_TEST(test_reset_equals_fresh_build);
    RUN_TEST(test_word_cell_hint);
    RUN_TEST(test_hier_scopes);
    RUN_TEST(test_hier_listed_names);
    RUN_TEST(test_find);
    RUN_TEST(test_netnames_listing_blif);
    RUN_TEST(test_flattened_names);
    RUN_TEST(test_values_follow_pipeline);
    RUN_TEST(test_write_rules);
    RUN_TEST(test_mixed_clock_port);
    RUN_TEST(test_signals_oom);
    return UNITY_END();
}
