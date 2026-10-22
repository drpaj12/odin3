/*
 * test_sim_abi.c — the simulation C ABI (S1 spec section 3, SA-1 to SA-11) through libodin3.so
 * only: open, scopes, signals, find, set/get, settle, step, reset, close, and the pass refusal.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, LINE = 256, WIDE = 70, TRACE_ROWS = 10, HIER_LISTED = 14 };

static odin3_design *design;
static odin3_simulation *g_a;
static odin3_simulation *g_b;
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
    odin3_simulation_close(g_a);
    g_a = NULL;
    odin3_simulation_close(g_b);
    g_b = NULL;
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers ---------------------------------------------------------------------------------- */

static void run_pass(const char *name, const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(design, name, args), name);
}

/* A fixture file and the passes (space separated, at most 3) to run after reading it. */
typedef struct fixture {
    const char *file;
    const char *tail;
} fixture;

/* Reads the fixture, then runs each pass of its tail. */
static void read_fixture(fixture fix) {
    const char *tail = fix.tail;
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_SIM_FIXTURES, fix.file);
    run_pass("read_verilog", path);
    char words[3][16] = {{0}};
    int count = sscanf(tail, "%15s %15s %15s", words[0], words[1], words[2]);
    for (int i = 0; i < count; i++) {
        run_pass(words[i], "");
    }
}

/* The fixture after read_verilog, proc, opt, lower. */
static void lowered(const char *file) {
    read_fixture((fixture){file, "proc opt lower"});
}

static odin3_simulation *open_top(void) {
    odin3_simulation *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_open(design, 0, &sim));
    TEST_ASSERT_NOT_NULL(sim);
    return sim;
}

static uint32_t sig(odin3_simulation *sim, const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_simulation_find_signal(sim, name, &id), name);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, id, name);
    return id;
}

static uint64_t get(odin3_simulation *sim, const char *name) {
    uint64_t value = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_simulation_get(sim, sig(sim, name), &value),
                                  name);
    return value;
}

static void set(odin3_simulation *sim, const char *name, uint64_t value) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_simulation_set(sim, sig(sim, name), value), name);
}

static uint64_t cycle_of(const odin3_simulation *sim) {
    uint64_t cycle = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_cycle(sim, &cycle));
    return cycle;
}

static odin3_simulation_signal info_of(const odin3_simulation *sim, uint32_t id) {
    odin3_simulation_signal info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_signal(sim, id, &info));
    return info;
}

/* Spec section 10: inputs before each step, outputs and registers after it. */
typedef struct trace_row {
    uint64_t rst, car_ns, car_ew, light_ns, light_ew, state, timer;
} trace_row;

static const trace_row TRACE[TRACE_ROWS] = {
    {1, 0, 0, 2, 0, 0, 0}, {0, 0, 1, 1, 0, 1, 1}, {0, 0, 0, 1, 0, 1, 0}, {0, 0, 0, 0, 2, 2, 3},
    {0, 0, 0, 0, 2, 2, 2}, {0, 0, 0, 0, 2, 2, 1}, {0, 0, 0, 0, 2, 2, 0}, {0, 1, 0, 0, 1, 3, 1},
    {0, 0, 0, 0, 1, 3, 0}, {0, 0, 0, 2, 0, 0, 3},
};

static void trace_step(odin3_simulation *sim, uint32_t row) {
    const trace_row *want = &TRACE[row];
    set(sim, "rst", want->rst);
    set(sim, "car_ns", want->car_ns);
    set(sim, "car_ew", want->car_ew);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_step(sim, 1));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want->light_ns, get(sim, "light_ns"), "light_ns");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want->light_ew, get(sim, "light_ew"), "light_ew");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want->state, get(sim, "state"), "state");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want->timer, get(sim, "timer"), "timer");
}

static void full_trace(odin3_simulation *sim) {
    for (uint32_t row = 0; row < TRACE_ROWS; row++) {
        trace_step(sim, row);
    }
    TEST_ASSERT_EQUAL_UINT64(TRACE_ROWS, cycle_of(sim));
}

/* --- SA-1 open, SA-9 NULL arguments ----------------------------------------------------------- */

static void test_null_arguments(void) {
    odin3_simulation *sim = (odin3_simulation *)&sim;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(NULL, 0, &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(design, 0, NULL));
    odin3_simulation_close(NULL);
    lowered("traffic.v");
    g_a = open_top();
    uint32_t count = 0;
    uint32_t id = 0;
    uint64_t value = 0;
    uint8_t bit = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_signal_count(NULL, &count));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_signal_count(g_a, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_scope_count(g_a, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_scope(g_a, 0, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_signal(g_a, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_cycle(g_a, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_step(NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_settle(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_reset(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_find_signal(g_a, NULL, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_find_signal(g_a, "rst", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get(g_a, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get(g_a, 0, &value));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(NULL, 2, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(g_a, 0, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set_bits(g_a, 2, NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_bits(g_a, 2, NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_bits(NULL, 2, &bit, 1));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "odin3_simulation_step: invalid argument"));
}

static void test_open_needs_a_top(void) {
    odin3_simulation *sim = (odin3_simulation *)&sim;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(design, 0, &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "odin3_simulation_open: the design has no top module"));
    sim = (odin3_simulation *)&sim;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(design, 99, &sim));
    TEST_ASSERT_NULL(sim);
}

static void test_open_refuses_processes(void) {
    read_fixture((fixture){"traffic.v", ""});
    odin3_simulation *sim = (odin3_simulation *)&sim;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(design, 0, &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has processes; run proc first"));
    run_pass("proc", ""); /* nothing held: passes run */
}

static void test_open_needs_lower(void) {
    read_fixture((fixture){"traffic.v", "proc opt"});
    odin3_simulation *sim = (odin3_simulation *)&sim;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_open(design, 0, &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "run `lower` first"));
    run_pass("lower", "");
}

/* --- SC-2 to SC-4, SA-9 ----------------------------------------------------------------------- */

static void test_traffic_reference_trace(void) {
    lowered("traffic.v");
    g_a = open_top();
    TEST_ASSERT_EQUAL_UINT64(2, get(g_a, "light_ns"));
    TEST_ASSERT_EQUAL_UINT64(0, get(g_a, "light_ew"));
    TEST_ASSERT_EQUAL_UINT64(0, cycle_of(g_a));
    full_trace(g_a);
}

/* The width characters of text from first (MSB first) as a value. */
static uint64_t field(const char *text, uint32_t first, uint32_t width) {
    uint64_t value = 0;
    for (uint32_t k = 0; k < width; k++) {
        value = (value << 1U) | (text[first + k] == '1' ? 1U : 0U);
    }
    return value;
}

/* One vector line "c iii oooo": drive, step and compare. */
static void replay_line(const char *line) {
    char in[8] = {0};
    char out[8] = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, sscanf(line, "%*s %7s %7s", in, out), line);
    set(g_a, "rst", field(in, 0, 1));
    set(g_a, "car_ns", field(in, 1, 1));
    set(g_a, "car_ew", field(in, 2, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_step(g_a, 1));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(field(out, 0, 2), get(g_a, "light_ns"), line);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(field(out, 2, 2), get(g_a, "light_ew"), line);
}

static void test_matches_sim_pass_vectors(void) {
    lowered("traffic.v");
    run_pass("sim", "16 3 " ODIN3_TEST_TMP);
    FILE *file = fopen(ODIN3_TEST_TMP, "r");
    TEST_ASSERT_NOT_NULL(file);
    g_a = open_top();
    char line[LINE];
    uint32_t lines = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        if (line[0] != '#') {
            replay_line(line);
            lines++;
        }
    }
    (void)fclose(file);
    (void)remove(ODIN3_TEST_TMP);
    TEST_ASSERT_EQUAL_UINT32(16, lines);
    TEST_ASSERT_EQUAL_UINT64(16, cycle_of(g_a));
}

static void test_settle_shows_combinational(void) {
    lowered("hier.v");
    g_a = open_top();
    TEST_ASSERT_EQUAL_UINT64(15, get(g_a, "y"));
    set(g_a, "a", 5);
    TEST_ASSERT_EQUAL_UINT64(15, get(g_a, "y"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_settle(g_a));
    TEST_ASSERT_EQUAL_UINT64(10, get(g_a, "y"));
    TEST_ASSERT_EQUAL_UINT64(0, cycle_of(g_a));
}

static void test_reset(void) {
    lowered("traffic.v");
    g_a = open_top();
    for (uint32_t row = 0; row < 3; row++) {
        trace_step(g_a, row);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_reset(g_a));
    TEST_ASSERT_EQUAL_UINT64(0, cycle_of(g_a));
    TEST_ASSERT_EQUAL_UINT64(2, get(g_a, "light_ns"));
    TEST_ASSERT_EQUAL_UINT64(0, get(g_a, "rst"));
    TEST_ASSERT_EQUAL_UINT64(0, get(g_a, "state"));
    full_trace(g_a);
}

/* --- SA-7, SA-8 ------------------------------------------------------------------------------- */

/* SA-7 on traffic: a value too wide, an output, a clock. */
static void traffic_rules(void) {
    lowered("traffic.v");
    g_a = open_top();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(g_a, sig(g_a, "car_ns"), 2));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "value wider than the signal"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_simulation_set(g_a, sig(g_a, "light_ns"), 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(g_a, sig(g_a, "clk"), 0));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "not an input"));
}

/* SA-7, SA-8 on wide (70 bits): get and set refuse it, the bit forms take it. */
static void wide_rules(void) {
    lowered("wide.v");
    g_a = open_top();
    uint64_t value = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get(g_a, sig(g_a, "q"), &value));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "width over 64"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(g_a, sig(g_a, "d"), 0));
    uint8_t bits[WIDE];
    uint8_t got[WIDE];
    for (uint32_t i = 0; i < WIDE; i++) {
        bits[i] = i % 3 == 0 ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_set_bits(g_a, sig(g_a, "d"), bits, WIDE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_step(g_a, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_bits(g_a, sig(g_a, "q"), got, WIDE));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bits, got, WIDE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_simulation_get_bits(g_a, sig(g_a, "q"), got, WIDE - 1));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "count is not the width"));
    bits[0] = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_simulation_set_bits(g_a, sig(g_a, "d"), bits, WIDE));
}

static void test_set_and_get_rules(void) {
    traffic_rules();
    odin3_simulation_close(g_a);
    g_a = NULL;
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    wide_rules();
}

static void test_mixed_clock_port(void) {
    lowered("mixclk.v");
    g_a = open_top();
    uint32_t ck = sig(g_a, "ck");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIMULATION_INPUT, info_of(g_a, ck).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_set(g_a, ck, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_set(g_a, ck, 1));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "a clock bit set to 1"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIMULATION_CLOCK, info_of(g_a, sig(g_a, "ck[0]")).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_step(g_a, 1));
    TEST_ASSERT_EQUAL_UINT64(1, get(g_a, "q"));
}

/* --- SA-3 to SA-5, SA-10 ---------------------------------------------------------------------- */

static void test_scopes_and_signal_info(void) {
    lowered("hier.v");
    g_a = open_top();
    uint32_t stage = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_lookup_module(design, "stage", &stage));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, stage);
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_scope_count(g_a, &count));
    TEST_ASSERT_EQUAL_UINT32(3, count);
    odin3_simulation_scope scope;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_scope(g_a, 0, &scope));
    TEST_ASSERT_EQUAL_STRING("", scope.name);
    TEST_ASSERT_EQUAL_STRING("hier", scope.leaf);
    TEST_ASSERT_EQUAL_UINT32(0, scope.inst.id);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_scope(g_a, 1, &scope));
    TEST_ASSERT_EQUAL_STRING("u0", scope.name);
    TEST_ASSERT_EQUAL_STRING("u0", scope.leaf);
    TEST_ASSERT_EQUAL_UINT32(0, scope.parent);
    TEST_ASSERT_EQUAL_UINT32(stage, scope.module);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, scope.inst.id);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_scope(g_a, 3, &scope));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "unknown scope"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_signal_count(g_a, &count));
    TEST_ASSERT_EQUAL_UINT32(HIER_LISTED, count);
    odin3_simulation_signal info = info_of(g_a, 1);
    TEST_ASSERT_EQUAL_STRING("clk", info.name);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SIMULATION_CLOCK, info.kind);
    info = info_of(g_a, sig(g_a, "u0.inner"));
    TEST_ASSERT_EQUAL_STRING("inner", info.leaf);
    TEST_ASSERT_EQUAL_UINT32(1, info.scope);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_OBJ_WIRE, info.obj.kind);
    TEST_ASSERT_EQUAL_UINT32(stage, info.obj.module);
    uint32_t none = 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_find_signal(g_a, "dead_r", &none));
    TEST_ASSERT_EQUAL_UINT32(0, none);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_simulation_get_signal(g_a, 99, &info));
}

/* Finds "<name>[i]" for every declared index i of listed signal id; the next ID is *next. */
static void find_bits(uint32_t id, uint32_t *next) {
    odin3_simulation_signal info = info_of(g_a, id);
    int32_t low = info.lsb < info.msb ? info.lsb : info.msb;
    int32_t high = info.lsb < info.msb ? info.msb : info.lsb;
    char name[LINE];
    for (int32_t i = low; i <= high; i++) {
        (void)snprintf(name, sizeof name, "%s[%d]", info.name, (int)i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(*next, sig(g_a, name), name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(*next, sig(g_a, name), name); /* SA-5 step 2 */
        (*next)++;
    }
}

static void test_names_stay_valid(void) {
    lowered("hier.v");
    g_a = open_top();
    const char *kept = info_of(g_a, 10).name;
    uint32_t next = HIER_LISTED + 1;
    for (uint32_t id = 1; id <= HIER_LISTED; id++) {
        find_bits(id, &next);
    }
    TEST_ASSERT_EQUAL_UINT32(35, next);
    TEST_ASSERT_EQUAL_STRING("u0.inner", kept);
    odin3_simulation_signal last = info_of(g_a, 34);
    TEST_ASSERT_EQUAL_UINT32(1, last.width);
    TEST_ASSERT_EQUAL_STRING("u1.inner[0]", last.name);
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_simulation_get_signal_count(g_a, &count));
    TEST_ASSERT_EQUAL_UINT32(HIER_LISTED, count);
}

/* --- SA-11 ------------------------------------------------------------------------------------ */

static void test_passes_refused_while_open(void) {
    lowered("hier.v");
    g_a = open_top();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "stats", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "pass stats: the design has 1 open simulation(s)"));
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, odin3_design_run_script(design, "stats", src));
    g_b = open_top();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "stats", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has 2 open simulation(s)"));
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_copy(design, &copy));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(copy, "stats", ""));
    odin3_design_destroy(copy);
    odin3_simulation_close(g_a);
    g_a = NULL;
    odin3_simulation_close(g_b);
    g_b = NULL;
    run_pass("stats", "");
}

static void test_two_simulations_independent(void) {
    lowered("traffic.v");
    g_a = open_top();
    g_b = open_top();
    trace_step(g_a, 0);
    trace_step(g_a, 1);
    TEST_ASSERT_EQUAL_UINT64(2, get(g_b, "light_ns"));
    TEST_ASSERT_EQUAL_UINT64(0, cycle_of(g_b));
    TEST_ASSERT_EQUAL_UINT64(2, cycle_of(g_a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_open_needs_a_top);
    RUN_TEST(test_open_refuses_processes);
    RUN_TEST(test_open_needs_lower);
    RUN_TEST(test_traffic_reference_trace);
    RUN_TEST(test_matches_sim_pass_vectors);
    RUN_TEST(test_settle_shows_combinational);
    RUN_TEST(test_reset);
    RUN_TEST(test_set_and_get_rules);
    RUN_TEST(test_mixed_clock_port);
    RUN_TEST(test_scopes_and_signal_info);
    RUN_TEST(test_names_stay_valid);
    RUN_TEST(test_passes_refused_while_open);
    RUN_TEST(test_two_simulations_independent);
    return UNITY_END();
}
