/*
 * test_sim_vectors_in.c — the vector line writer and the input-vector reader (SV-2, SV-3).
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/vectors.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { LONG_LINE = 10240, OOM_ROUNDS = 64, LOG_TEXT = 4096, TRAFFIC_CYCLES = 16, TRAFFIC_SEED = 5 };

static odin3_design *design;
static odin3_sim *sim;
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
    sim = NULL;
}

void tearDown(void) {
    odin3_sim_destroy(sim);
    sim = NULL;
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_sim *build(void) {
    odin3_sim *out = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_sim_build(design, (odin3_module_id){1}, &out),
                                  log_text);
    return out;
}

static void load_blif(const char *path) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(design, path), log_text);
    sim = build();
}

static void load_netnames(void) {
    load_blif(ODIN3_SIM_FIXTURES "/netnames.blif");
}

static odin3_status parse_text(const char *text, odin3_sim_vectors_in *in) {
    return odin3_sim_vectors_parse(sim, "v.vec", odin3_bytes_cstr(text), in);
}

typedef struct parse_case {
    const char *text;
    const char *msg;
} parse_case;

static void expect_parse_error(parse_case tc) {
    odin3_sim_vectors_in in;
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_text(tc.text, &in));
    TEST_ASSERT_NULL(in.bits);
    TEST_ASSERT_EQUAL_UINT32(0, in.cycles);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, tc.msg), log_text);
}

static void test_parse_written_file(void) {
    load_netnames();
    odin3_sim_vectors_in in;
    const char *text = "# t seed 7 cycles 2\n# clock clk 0\n# input a 1\n# input b 1\n"
                       "# output y 1\n0 10 0\n1 01 1\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_text(text, &in), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, in.cycles);
    TEST_ASSERT_EQUAL_UINT64(7, in.seed);
    TEST_ASSERT_EQUAL_UINT32(2, in.n_bits);
    const uint8_t want[] = {1, 0, 0, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, in.bits, 4);
    odin3_sim_vectors_in_free(&in);
    TEST_ASSERT_NULL(in.bits);
}

static void test_parse_bare_lines_crlf_comments(void) {
    load_netnames();
    odin3_sim_vectors_in in;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_text("\n0 11\r\n# note\n\t\n1 00 x\n", &in),
                                  log_text);
    TEST_ASSERT_EQUAL_UINT32(2, in.cycles);
    TEST_ASSERT_EQUAL_UINT64(0, in.seed);
    const uint8_t want[] = {1, 1, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, in.bits, 4);
    odin3_sim_vectors_in_free(&in);
}

static void test_parse_errors(void) {
    load_netnames();
    expect_parse_error(
        (parse_case){"0 1\n", "v.vec:1: expected 2 input bits of 0 and 1, found `1`"});
    expect_parse_error(
        (parse_case){"0 1x\n", "v.vec:1: expected 2 input bits of 0 and 1, found `1x`"});
    expect_parse_error(
        (parse_case){"0 -\n", "v.vec:1: expected 2 input bits of 0 and 1, found `-`"});
    expect_parse_error((parse_case){"1 11\n", "v.vec:1: expected cycle 0, found `1`"});
    expect_parse_error(
        (parse_case){"0 11 0 9\n", "v.vec:1: expected `<cycle> <inputs> [<outputs>]`"});
    expect_parse_error(
        (parse_case){"# input a 2\n0 11\n",
                     "v.vec:1: port line `# input a 2` does not match the design: expected "
                     "`# clock clk 0`"});
    expect_parse_error((parse_case){"# clock clk 0\n0 11\n",
                                    "v.vec:1: the header lists 1 port lines, the design has 4"});
    expect_parse_error((parse_case){"# t seed 1 cycles 3\n0 11\n",
                                    "v.vec:2: the title says 3 cycles, the file has 1"});
}

static void test_parse_no_input_design(void) {
    char path[] = "/tmp/odin3_vin_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fputs(".model c\n.outputs y\n.names y\n1\n.end\n", fp);
    (void)fclose(fp);
    load_blif(path);
    (void)remove(path);
    expect_parse_error(
        (parse_case){"0 1\n", "v.vec:1: expected `-`: the design has no non-clock input bit"});
    odin3_sim_vectors_in in;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_text("0 -\n", &in), log_text);
    TEST_ASSERT_EQUAL_UINT32(0, in.n_bits);
    TEST_ASSERT_EQUAL_UINT32(1, in.cycles);
    odin3_sim_vectors_in_free(&in);
}

static void test_title_only_before_vectors(void) {
    load_netnames();
    odin3_sim_vectors_in in;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_text("0 11\n# t seed 9 cycles 5\n", &in),
                                  log_text);
    TEST_ASSERT_EQUAL_UINT32(1, in.cycles);
    TEST_ASSERT_EQUAL_UINT64(0, in.seed);
    odin3_sim_vectors_in_free(&in);
}

static void test_header_edge_cases(void) {
    load_netnames();
    expect_parse_error(
        (parse_case){"0 11\n# input a 1\n", "v.vec:2: a port line after the first vector line"});
    expect_parse_error((parse_case){"# clock clk 0\n# input a 1\n0 11\n",
                                    "v.vec:2: the header lists 2 port lines, the design has 4"});
    expect_parse_error(
        (parse_case){"# clock clk 0\n# input a 1\n# input b 1\n# output y 1\n# output z 1\n0 11\n",
                     "v.vec:5: the header lists 5 port lines, the design has 4"});
    expect_parse_error((parse_case){"# t seed 1 cycles 1\n0 11\n1 00\n",
                                    "v.vec:3: the title says 1 cycles, the file has 2"});
}

/* Text of `prefix`, LONG_LINE copies of chr and `suffix` in text. */
static void long_text(odin3_strbuf *text, const char *prefix, char chr, const char *suffix) {
    char block[LONG_LINE];
    memset(block, chr, sizeof block);
    odin3_strbuf_init(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(text, odin3_bytes_cstr(prefix)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(text, (odin3_bytes){block, sizeof block}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(text, odin3_bytes_cstr(suffix)));
}

typedef struct capped_case {
    const char *prefix; /* the text before the long run */
    char chr;           /* the run's character */
    const char *suffix; /* the text after it */
    const char *msg;    /* what the error log must hold */
} capped_case;

static void expect_capped(capped_case tc) {
    odin3_strbuf text;
    long_text(&text, tc.prefix, tc.chr, tc.suffix);
    expect_parse_error((parse_case){text.data, tc.msg});
    TEST_ASSERT_TRUE(strlen(log_text) < 400);
    odin3_strbuf_free(&text);
}

static void test_long_port_line_is_capped(void) {
    load_netnames();
    expect_capped((capped_case){"# input ", 'a', "\n0 11\n",
                                "aaa...` does not match the design: expected `# clock clk 0`"});
}

static void test_long_words_are_capped(void) {
    load_netnames();
    expect_capped(
        (capped_case){"0 ", '1', "\n", "v.vec:1: expected 2 input bits of 0 and 1, found `1111"});
    TEST_ASSERT_NOT_NULL(strstr(log_text, "11...`"));
    expect_capped((capped_case){"", '7', " 11\n", "v.vec:1: expected cycle 0, found `7777"});
    TEST_ASSERT_NOT_NULL(strstr(log_text, "77...`"));
}

/* A temporary file holding text (size 0: truncated to `size` bytes instead). */
static void make_temp(char *path, const char *text, off_t size) {
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    if (size > 0) {
        TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, size));
    } else {
        TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)write(fd, text, strlen(text)));
    }
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

static void test_read_oversize_file(void) {
    load_netnames();
    char path[] = "/tmp/odin3_vin_big_XXXXXX";
    make_temp(path, "", (off_t)ODIN3_SIM_VECTORS_MAX_BYTES + 1);
    odin3_sim_vectors_in in;
    odin3_status st = odin3_sim_vectors_read(sim, path, &in);
    (void)remove(path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_NOT_NULL(strstr(log_text, ": larger than 268435456 bytes"));
    TEST_ASSERT_NULL(in.bits);
}

/* One read of path with allocation `at` failing; true when it worked. */
static bool read_with_failure(const char *path, long at) {
    odin3_sim_vectors_in in;
    odin3_util_set_alloc_fail_after(at);
    odin3_status st = odin3_sim_vectors_read(sim, path, &in);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY,
                             "an injected failure gave another status");
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(2, in.cycles);
        odin3_sim_vectors_in_free(&in);
        return true;
    }
    TEST_ASSERT_NULL(in.bits);
    return false;
}

/* The reader with every allocation in turn failing: OK or NO_MEMORY, never a leak (ASan). */
static void test_read_alloc_failures(void) {
    load_netnames();
    char path[] = "/tmp/odin3_vin_oom_XXXXXX";
    make_temp(path,
              "# t seed 3 cycles 2\n# clock clk 0\n# input a 1\n# input b 1\n"
              "# output y 1\n0 10 0\n1 01 1\n",
              0);
    long at = 0;
    while (at < OOM_ROUNDS && !read_with_failure(path, at)) {
        at++;
    }
    (void)remove(path);
    TEST_ASSERT_TRUE_MESSAGE(at < OOM_ROUNDS, "the reader never succeeded within the rounds");
}

static void test_read_missing_file(void) {
    load_netnames();
    odin3_sim_vectors_in in;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_sim_vectors_read(sim, "/nonexistent/x.vec", &in));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "sim: cannot read `/nonexistent/x.vec`"));
}

/* Writes the header and TRAFFIC_CYCLES random lines (seed TRAFFIC_SEED) of the traffic light. */
static void write_random(odin3_strbuf *text, const odin3_sim_vectors *run) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_header(sim, run, text));
    odin3_prng prng;
    odin3_prng_seed(&prng, run->seed);
    for (uint32_t cyc = 0; cyc < run->cycles; cyc++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_step(sim, &prng, cyc, text));
    }
}

static void test_drive_then_line_round_trips(void) {
    const char *fixture = ODIN3_SIM_FIXTURES "/traffic.v";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_verilog", odin3_bytes_cstr(fixture)), log_text);
    const char *passes[] = {"proc", "opt", "lower"};
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK, odin3_pass_run(design, passes[i], odin3_bytes_cstr("")), log_text);
    }
    sim = build();
    const odin3_sim_vectors run = {"t", TRAFFIC_SEED, TRAFFIC_CYCLES};
    odin3_strbuf first;
    odin3_strbuf_init(&first);
    write_random(&first, &run);
    odin3_sim_vectors_in in;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_sim_vectors_parse(sim, "a.vec", odin3_bytes_cstr(first.data), &in),
        log_text);
    TEST_ASSERT_EQUAL_UINT32(TRAFFIC_CYCLES, in.cycles);
    TEST_ASSERT_EQUAL_UINT64(TRAFFIC_SEED, in.seed);
    odin3_sim_destroy(sim);
    sim = build(); /* a fresh simulation: the reset state */
    odin3_strbuf second;
    odin3_strbuf_init(&second);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_header(sim, &run, &second));
    for (uint32_t cyc = 0; cyc < in.cycles; cyc++) {
        odin3_sim_vectors_drive(sim, &in, cyc);
        odin3_sim_cycle(sim);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_line(sim, cyc, &second));
    }
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_sim_vectors_in_free(&in);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_written_file);
    RUN_TEST(test_parse_bare_lines_crlf_comments);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_parse_no_input_design);
    RUN_TEST(test_title_only_before_vectors);
    RUN_TEST(test_header_edge_cases);
    RUN_TEST(test_long_port_line_is_capped);
    RUN_TEST(test_long_words_are_capped);
    RUN_TEST(test_read_oversize_file);
    RUN_TEST(test_read_alloc_failures);
    RUN_TEST(test_read_missing_file);
    RUN_TEST(test_drive_then_line_round_trips);
    return UNITY_END();
}
