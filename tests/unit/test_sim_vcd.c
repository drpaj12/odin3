/*
 * test_sim_vcd.c — the VCD writer (SW-2, SW-3 text) and its equality with odin3_sim_cycle (SW-4).
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "sim/vcd.h"
#include "sim/vectors.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

enum {
    LOG_TEXT = 4096,
    TEXT_MAX = 8192,
    CYCLES = 32,
    SEED = 6,
    LIMIT_BYTES = 300,
    CODE_94_SQUARED = 94 * 94
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
    odin3_log_set_sink(NULL, NULL);
    odin3_sim_signals_destroy(g_sig);
    g_sig = NULL;
    odin3_sim_destroy(g_sim);
    g_sim = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

static void run_pass(const char *name, const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pass_run(design, name, odin3_bytes_cstr(args)),
                                  name);
}

static odin3_sim *build_ok(void) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_sim_build(design, odin3_design_top(design), &sim),
                                  log_text);
    return sim;
}

/* Reads fixtures/<file>, runs proc opt lower for Verilog, builds the simulator and its table. */
static void load(const char *file) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_SIM_FIXTURES, file);
    if (strstr(file, ".blif") != NULL) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(design, path), log_text);
    } else {
        run_pass("read_verilog", path);
        run_pass("proc", "");
        run_pass("opt", "");
        run_pass("lower", "");
    }
    g_sim = build_ok();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_create(g_sim, &g_sig));
}

/* The whole content of f from the start, NUL-terminated in text. */
static void slurp(FILE *file, char *text, size_t cap) {
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    size_t got = fread(text, 1, cap - 1, file);
    text[got] = '\0';
}

static void test_codes(void) {
    char code[ODIN3_SIM_VCD_CODE_MAX];
    odin3_sim_vcd_code(0, code);
    TEST_ASSERT_EQUAL_STRING("!", code);
    odin3_sim_vcd_code(1, code);
    TEST_ASSERT_EQUAL_STRING("\"", code);
    odin3_sim_vcd_code(93, code);
    TEST_ASSERT_EQUAL_STRING("~", code);
    odin3_sim_vcd_code(94, code);
    TEST_ASSERT_EQUAL_STRING("!\"", code);
    odin3_sim_vcd_code(95, code);
    TEST_ASSERT_EQUAL_STRING("\"\"", code);
    odin3_sim_vcd_code(CODE_94_SQUARED, code);
    TEST_ASSERT_EQUAL_STRING("!!\"", code);
}

static void write_signal(const char *name, uint8_t bit) {
    uint32_t sid = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_find(g_sig, odin3_bytes_cstr(name), &sid));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sid);
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_signals_write(g_sig, sid, &bit, 1, &why));
}

static void test_netnames_one_cycle_exact(void) {
    load("netnames.blif");
    FILE *file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    odin3_sim_vcd vcd;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_begin(&vcd, g_sig, file));
    write_signal("a", 1);
    write_signal("b", 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_cycle(&vcd, g_sim, 0));
    odin3_sim_vcd_free(&vcd);
    char text[TEXT_MAX];
    slurp(file, text, sizeof text);
    (void)fclose(file);
    TEST_ASSERT_EQUAL_STRING(
        "$version odin3 sim $end\n$timescale 1ns $end\n$scope module netnames $end\n"
        "$var wire 1 ! clk $end\n$var wire 1 \" a $end\n$var wire 1 # b $end\n"
        "$var wire 1 $ y $end\n$var wire 1 % n1 $end\n$var wire 1 & q $end\n"
        "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n0!\n0\"\n0#\n0$\n0%\n0&\n$end\n"
        "#2\n1\"\n1#\n1%\n#5\n1!\n1$\n1&\n#10\n0!\n",
        text);
}

static size_t count_of(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *cur = strstr(text, needle); cur != NULL; cur = strstr(cur + 1, needle)) {
        count++;
    }
    return count;
}

/* The offsets of the needles in text, in order; every one must exist. */
static void expect_in_order(const char *text, const char *const *needles, size_t count) {
    const char *from = text;
    for (size_t k = 0; k < count; k++) {
        const char *at = strstr(from, needles[k]);
        TEST_ASSERT_NOT_NULL_MESSAGE(at, needles[k]);
        from = at + strlen(needles[k]);
    }
}

static void test_hier_scopes_and_ranges(void) {
    load("hier.v");
    FILE *file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    odin3_sim_vcd vcd;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_begin(&vcd, g_sig, file));
    odin3_sim_vcd_free(&vcd);
    char text[TEXT_MAX];
    slurp(file, text, sizeof text);
    (void)fclose(file);
    /* the u0 block (opened and closed) comes before u1 opens */
    const char *const order[] = {"$scope module hier $end\n", "$var wire 4 \" a [3:0] $end\n",
                                 "$scope module u0 $end\n", "$upscope $end\n",
                                 "$scope module u1 $end\n"};
    expect_in_order(text, order, sizeof order / sizeof order[0]);
    TEST_ASSERT_EQUAL_UINT(3, count_of(text, "$scope"));
    TEST_ASSERT_EQUAL_UINT(3, count_of(text, "$upscope"));
}

/* The line text of every cycle of a run driven by the same PRNG sequence. */
static void run_lines(odin3_sim *sim, odin3_sim_vcd *vcd, odin3_strbuf *lines) {
    odin3_prng prng;
    odin3_prng_seed(&prng, SEED);
    for (uint32_t cyc = 0; cyc < CYCLES; cyc++) {
        odin3_sim_drive_random(sim, &prng);
        if (vcd != NULL) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_cycle(vcd, sim, cyc));
        } else {
            odin3_sim_cycle(sim);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_line(sim, cyc, lines));
    }
}

static void test_cycle_equals_sim_cycle(void) {
    load("traffic.v");
    odin3_sim *plain = build_ok();
    FILE *file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    odin3_sim_vcd vcd;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_begin(&vcd, g_sig, file));
    odin3_strbuf with_vcd;
    odin3_strbuf without;
    odin3_strbuf_init(&with_vcd);
    odin3_strbuf_init(&without);
    run_lines(g_sim, &vcd, &with_vcd);
    run_lines(plain, NULL, &without);
    TEST_ASSERT_EQUAL_STRING(without.data, with_vcd.data);
    odin3_strbuf_free(&with_vcd);
    odin3_strbuf_free(&without);
    odin3_sim_vcd_free(&vcd);
    odin3_sim_destroy(plain);
    (void)fclose(file);
}

/* A begin with the fail-th allocation failing; true when it succeeded (then one cycle runs). */
static bool begin_once(FILE *file, int fail) {
    odin3_sim_vcd vcd;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_sim_vcd_begin(&vcd, g_sig, file);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    if (st != ODIN3_OK) {
        TEST_ASSERT_NULL(vcd.last);
        return false;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vcd_cycle(&vcd, g_sim, 0));
    odin3_sim_vcd_free(&vcd);
    return true;
}

static bool begin_failing_at(int fail) {
    FILE *file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    bool done = begin_once(file, fail);
    (void)fclose(file);
    return done;
}

static void test_begin_oom(void) {
    load("hier.v");
    int fail = 0;
    while (!begin_failing_at(fail)) {
        fail++;
        TEST_ASSERT_LESS_THAN_INT(64, fail);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, fail);
}

static void test_begin_io_error(void) {
    FILE *full = fopen("/dev/full", "wb");
    if (full == NULL) {
        TEST_IGNORE_MESSAGE("no /dev/full");
    }
    load("netnames.blif");
    TEST_ASSERT_EQUAL_INT(0, setvbuf(full, NULL, _IONBF, 0));
    odin3_sim_vcd vcd;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_sim_vcd_begin(&vcd, g_sig, full));
    TEST_ASSERT_NULL(vcd.last);
    (void)fclose(full);
}

/* The number of entries under dir named w.vcd*. */
static int vcd_entries_in(const char *dir) {
    int count = 0;
    DIR *handle = opendir(dir);
    TEST_ASSERT_NOT_NULL(handle);
    for (const struct dirent *ent = readdir(handle); ent != NULL; ent = readdir(handle)) {
        count += strncmp(ent->d_name, "w.vcd", strlen("w.vcd")) == 0 ? 1 : 0;
    }
    (void)closedir(handle);
    return count;
}

/* sim --vcd to a path in a fresh directory, with writes beyond LIMIT_BYTES refused. */
static void sim_vcd_limited(const char *dir, const char *tail) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/traffic.v", ODIN3_SIM_FIXTURES);
    run_pass("read_verilog", path);
    run_pass("proc", "");
    run_pass("opt", "");
    run_pass("lower", "");
    char args[1024];
    (void)snprintf(args, sizeof args, "1 3 %s/v.vec --vcd %s/%s", dir, dir, tail);
    struct rlimit old;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    struct rlimit tight = {LIMIT_BYTES, old.rlim_max};
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &tight));
    odin3_status st = odin3_pass_run(design, "sim", odin3_bytes_cstr(args));
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &old));
    (void)signal(SIGXFSZ, prev);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
}

static void test_pass_io_error_leaves_no_file(void) {
    char dir[] = "/tmp/odin3-test-sim-vcd-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    sim_vcd_limited(dir, "w.vcd");
    char vcd[600];
    (void)snprintf(vcd, sizeof vcd, "%s/w.vcd", dir);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "w.vcd"), log_text);
    TEST_ASSERT_NOT_EQUAL_INT(0, access(vcd, F_OK));
    TEST_ASSERT_EQUAL_INT(0, vcd_entries_in(dir));
    char vec[600];
    (void)snprintf(vec, sizeof vec, "%s/v.vec", dir);
    (void)unlink(vec);
    (void)rmdir(dir);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_codes);
    RUN_TEST(test_netnames_one_cycle_exact);
    RUN_TEST(test_hier_scopes_and_ranges);
    RUN_TEST(test_cycle_equals_sim_cycle);
    RUN_TEST(test_begin_oom);
    RUN_TEST(test_begin_io_error);
    RUN_TEST(test_pass_io_error_leaves_no_file);
    return UNITY_END();
}
