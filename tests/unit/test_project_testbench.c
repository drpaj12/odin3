/*
 * test_project_testbench.c — unit tests for the project `testbench` key (.o3proj) and the Quartus
 * EDA_TEST_BENCH_* import (spec docs/specs/2026-10-31-project-testbench-design.md sections 1, 2):
 * every keyword and default, the dump text, every error, the qsf mapping with the time-unit
 * conversion and each WARNING.
 */
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "ir/design.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 1024, MSG_MAX = 4096, OUT_MAX = 8192, WARN_MAX = 8, TEXT_MAX = 4096 };

static odin3_design *design;
static odin3_project *proj;
static char captured[MSG_MAX];
static int errors;
static char warnings[WARN_MAX][MSG_MAX];
static int nwarn;
static char out[OUT_MAX];
static char tmp[PATH_BUF / 2];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    } else if (level == ODIN3_LOG_WARN && nwarn < WARN_MAX) {
        (void)snprintf(warnings[nwarn++], MSG_MAX, "%s", msg);
    }
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    errors = 0;
    nwarn = 0;
    captured[0] = '\0';
}

typedef struct file_spec {
    const char *name, *text;
} file_spec;

static void write_file(file_spec spec) {
    const char *text = spec.text;
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, spec.name);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

static const char *const k_files[] = {"tb.v", "tb2.v", "tb3.v", "exp.txt", "p.o3proj", "t.qsf"};

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    design = NULL;
    fresh_design();
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_tbproj_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    write_file((file_spec){"tb.v", "`timescale 1ns/1ps\nmodule tb; endmodule\n"});
    write_file((file_spec){"tb2.v", "`timescale 10 ps / 1 ps\nmodule tb2; endmodule\n"});
    write_file((file_spec){"tb3.v", "module tb3; endmodule\n"});
    write_file((file_spec){"exp.txt", "0 1\n"});
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; i++) {
        char path[PATH_BUF];
        (void)snprintf(path, sizeof path, "%s/%s", tmp, k_files[i]);
        (void)unlink(path);
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_design_destroy(design);
}

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

static odin3_status read_proj(const char *text, const char *name, bool qsf) {
    write_file((file_spec){name, text});
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    odin3_proj_path file = {id(name), id(path), {0}};
    errors = 0;
    captured[0] = '\0';
    odin3_quartus_req req = {{"", 0}, false, {0}};
    return qsf ? odin3_read_quartus(proj, &file, &req) : odin3_read_o3proj(proj, &file);
}

static odin3_status read_o3(const char *text) {
    return read_proj(text, "p.o3proj", false);
}

static odin3_status read_qsf(const char *text) {
    return read_proj(text, "t.qsf", true);
}

/* The dump's "testbenches" line with the temporary directory removed ("" when absent). */
static const char *tbs(void) {
    static char line[OUT_MAX];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &text));
    const char *at = strstr(text.data, "\n  \"testbenches\": ");
    line[0] = '\0';
    if (at != NULL) {
        at += strlen("\n  \"testbenches\": ");
        size_t len = strcspn(at, "\n");
        len -= len > 0 && at[len - 1] == ',';
        (void)snprintf(out, sizeof out, "%.*s", (int)len, at);
        char *hit = NULL;
        size_t tlen = strlen(tmp);
        size_t pos = 0;
        while ((hit = strstr(out + pos, tmp)) != NULL) {
            memmove(hit, hit + tlen + 1, strlen(hit + tlen + 1) + 1);
            pos = (size_t)(hit - out);
        }
        (void)snprintf(line, sizeof line, "%s", out);
    }
    odin3_strbuf_free(&text);
    return line;
}

/* The one error, with the temporary directory shown as "$T". */
static const char *message(void) {
    static char shown[MSG_MAX];
    TEST_ASSERT_EQUAL_INT(1, errors);
    const char *hit = strstr(captured, tmp);
    if (hit == NULL) {
        return captured;
    }
    (void)snprintf(shown, sizeof shown, "%.*s$T%s", (int)(hit - captured), captured,
                   hit + strlen(tmp));
    return shown;
}

static void expect_o3_error(const char *text, const char *expected) {
    fresh_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, read_o3(text), text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, message(), text);
}

/* --- .o3proj ------------------------------------------------------------------------------- */

static void test_minimal_and_defaults(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_o3("testbench t1 file tb.v\n"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"name\": \"t1\", \"file\": \"tb.v\", \"top\": null, \"dut\": null, "
        "\"max_time\": null, \"expect\": null, \"on\": \"rtl\"}]",
        tbs());
}

static void test_all_keywords_any_order(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          read_o3("testbench full on both expect exp.txt max-time 5000 dut u0 "
                                  "top tb file tb.v\n"
                                  "testbench second file tb2.v on netlist\n"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"name\": \"full\", \"file\": \"tb.v\", \"top\": \"tb\", \"dut\": \"u0\", "
        "\"max_time\": 5000, \"expect\": \"exp.txt\", \"on\": \"both\"}, "
        "{\"name\": \"second\", \"file\": \"tb2.v\", \"top\": null, \"dut\": null, "
        "\"max_time\": null, \"expect\": null, \"on\": \"netlist\"}]",
        tbs());
}

static void test_no_testbench_no_dump_key(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_o3("define X\n"));
    TEST_ASSERT_EQUAL_STRING("", tbs());
}

static void test_o3proj_errors(void) {
    expect_o3_error("testbench t1\n",
                    "p.o3proj:1:1: error: syntax: usage: testbench <name> file <path> [top "
                    "<module>] [dut <inst-or-module>] [max-time <T>] [expect <path>] [on "
                    "rtl|netlist|both]");
    expect_o3_error("testbench t1 top tb\n",
                    "p.o3proj:1:1: error: syntax: usage: testbench "
                    "<name> file <path> [top <module>] [dut <inst-or-module>] [max-time <T>] "
                    "[expect <path>] [on rtl|netlist|both]: 't1'");
    expect_o3_error("testbench t1 file tb.v bogus x\n",
                    "p.o3proj:1:1: error: unknown_option: unknown testbench keyword: 'bogus'");
    expect_o3_error("testbench t1 file tb.v file tb.v\n",
                    "p.o3proj:1:1: error: syntax: repeated testbench keyword: 'file'");
    expect_o3_error("testbench t1 file tb.v on sometimes\n",
                    "p.o3proj:1:1: error: syntax: on expects rtl, netlist or both: "
                    "'sometimes'");
    expect_o3_error("testbench t1 file tb.v max-time 0\n",
                    "p.o3proj:1:1: error: syntax: max-time expects a count above 0: '0'");
    expect_o3_error("testbench t1 file tb.v max-time 99999999999999999999\n",
                    "p.o3proj:1:1: error: syntax: max-time expects a count above 0: "
                    "'99999999999999999999'");
    expect_o3_error("testbench t1 file tb.v top 1x\n",
                    "p.o3proj:1:1: error: syntax: expected a module or instance name: '1x'");
    expect_o3_error("testbench 1t file tb.v\n",
                    "p.o3proj:1:1: error: syntax: expected a testbench name: '1t'");
    expect_o3_error("testbench t1 file tb.v\ntestbench t1 file tb2.v\n",
                    "p.o3proj:2:1: error: syntax: testbench given twice: 't1'");
    expect_o3_error("testbench t1 file nothere.v\n",
                    "p.o3proj:1:19: error: missing_file: file not found: '$T/nothere.v'");
}

/* --- .qsf ---------------------------------------------------------------------------------- */

static const char k_full_qsf[] =
    "set_global_assignment -name TOP_LEVEL_ENTITY top\n"
    "set_global_assignment -name EDA_NATIVELINK_SIMULATION_TEST_BENCH tb1 -section_id "
    "eda_simulation\n"
    "set_global_assignment -name EDA_TEST_BENCH_NAME tb1 -section_id eda_simulation\n"
    "set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id tb1\n"
    "set_global_assignment -name EDA_TEST_BENCH_MODULE_NAME tb -section_id tb1\n"
    "set_global_assignment -name EDA_TEST_BENCH_RUN_SIM_FOR \"1 us\" -section_id tb1\n"
    "set_global_assignment -name EDA_DESIGN_INSTANCE_NAME u0 -section_id tb1\n";

static void test_qsf_full_mapping(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_qsf(k_full_qsf));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"tb1\", \"file\": \"tb.v\", \"top\": \"tb\", \"dut\": "
                             "\"u0\", \"max_time\": 1000, \"expect\": null, \"on\": \"rtl\"}]",
                             tbs());
    TEST_ASSERT_EQUAL_INT(0, nwarn);
}

static void test_qsf_order_free_and_two_benches(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_qsf("set_global_assignment -name EDA_TEST_BENCH_FILE tb2.v -section_id b\n"
                 "set_global_assignment -name EDA_TEST_BENCH_RUN_SIM_FOR \"3 ns\" -section_id b\n"
                 "set_global_assignment -name EDA_TEST_BENCH_NAME b -section_id eda_simulation\n"
                 "set_global_assignment -name EDA_TEST_BENCH_NAME a -section_id eda_simulation\n"
                 "set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id a\n"
                 "set_global_assignment -name EDA_TEST_BENCH_DESIGN_INSTANCE_NAME NA -section_id "
                 "a\n"));
    /* tb2.v has `timescale 10 ps: 3 ns = 300 units */
    TEST_ASSERT_EQUAL_STRING(
        "[{\"name\": \"b\", \"file\": \"tb2.v\", \"top\": null, \"dut\": null, \"max_time\": "
        "300, \"expect\": null, \"on\": \"rtl\"}, "
        "{\"name\": \"a\", \"file\": \"tb.v\", \"top\": null, \"dut\": null, \"max_time\": "
        "null, \"expect\": null, \"on\": \"rtl\"}]",
        tbs());
    TEST_ASSERT_EQUAL_INT(0, nwarn);
}

static void test_qsf_nativelink_declares(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_qsf(
            "set_global_assignment -name EDA_NATIVELINK_SIMULATION_TEST_BENCH only -section_id "
            "eda_simulation\n"
            "set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id only\n"));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"only\", \"file\": \"tb.v\", \"top\": null, \"dut\": "
                             "null, \"max_time\": null, \"expect\": null, \"on\": \"rtl\"}]",
                             tbs());
}

static void expect_warning(int idx, const char *text) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(warnings[idx], text), text);
}

static void test_qsf_warnings(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_qsf(
            "set_global_assignment -name EDA_TEST_BENCH_NAME nofile -section_id eda_simulation\n"
            "set_global_assignment -name EDA_TEST_BENCH_NAME two -section_id eda_simulation\n"
            "set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id two\n"
            "set_global_assignment -name EDA_TEST_BENCH_FILE tb2.v -section_id two\n"
            "set_global_assignment -name EDA_TEST_BENCH_ENABLE_STATUS TEST_BENCH_MODE "
            "-section_id eda_simulation\n"
            "set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id ghost\n"
            "set_global_assignment -name EDA_TEST_BENCH_RUN_SIM_FOR \"soon\" -section_id "
            "two\n"));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"two\", \"file\": \"tb.v\", \"top\": null, \"dut\": "
                             "null, \"max_time\": null, \"expect\": null, \"on\": \"rtl\"}]",
                             tbs());
    TEST_ASSERT_EQUAL_INT(5, nwarn);
    expect_warning(0, "extra EDA_TEST_BENCH_FILE not run");
    expect_warning(1, "test bench assignment not mapped");
    expect_warning(1, "EDA_TEST_BENCH_ENABLE_STATUS");
    expect_warning(2, "no declared test bench");
    expect_warning(3, "has no EDA_TEST_BENCH_FILE, dropped");
    expect_warning(4, "is not '<n> <unit>'");
}

static const char k_head[] =
    "set_global_assignment -name EDA_TEST_BENCH_NAME x -section_id eda_simulation\n";

/* A test bench x on file with the RUN_SIM_FOR value; the dump holds max_time text. */
typedef struct time_case {
    const char *file, *run_for, *max_time;
    int warns;
} time_case;

static void expect_max_time(time_case tc) {
    char text[TEXT_MAX];
    (void)snprintf(text, sizeof text,
                   "%sset_global_assignment -name EDA_TEST_BENCH_FILE %s -section_id x\n"
                   "set_global_assignment -name EDA_TEST_BENCH_RUN_SIM_FOR \"%s\" -section_id x\n",
                   k_head, tc.file, tc.run_for);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_qsf(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tbs(), tc.max_time), tc.max_time);
    TEST_ASSERT_EQUAL_INT(tc.warns, nwarn);
}

static void test_qsf_time_conversion(void) {
    expect_max_time((time_case){"tb.v", "7 ns", "\"max_time\": 7,", 0});
    expect_max_time((time_case){"tb.v", "2 us", "\"max_time\": 2000,", 0});
    expect_max_time((time_case){"tb2.v", "3 ns", "\"max_time\": 300,", 0});
    expect_max_time((time_case){"tb.v", "0 ns", "\"max_time\": null", 0});
}

static void test_qsf_time_warnings(void) {
    /* tb3.v has no `timescale: 1 ns assumed */
    expect_max_time((time_case){"tb3.v", "2 us", "\"max_time\": 2000,", 1});
    TEST_ASSERT_NOT_NULL(strstr(warnings[0], "no `timescale"));
    /* 1500 ps in 1 ns units rounds down, with a WARNING */
    expect_max_time((time_case){"tb.v", "1500 ps", "\"max_time\": 1,", 1});
    TEST_ASSERT_NOT_NULL(strstr(warnings[0], "not a whole number"));
}

static void test_qsf_missing_file_is_error(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        read_qsf("set_global_assignment -name EDA_TEST_BENCH_NAME x -section_id eda_simulation\n"
                 "set_global_assignment -name EDA_TEST_BENCH_FILE nothere.v -section_id x\n"));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NOT_NULL(strstr(captured, "missing_file"));
}

static void test_qsf_other_assignments_still_ignored(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_qsf("set_global_assignment -name FOO bar\n"));
    TEST_ASSERT_EQUAL_STRING("", tbs());
    TEST_ASSERT_EQUAL_INT(0, nwarn);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_minimal_and_defaults);
    RUN_TEST(test_all_keywords_any_order);
    RUN_TEST(test_no_testbench_no_dump_key);
    RUN_TEST(test_o3proj_errors);
    RUN_TEST(test_qsf_full_mapping);
    RUN_TEST(test_qsf_order_free_and_two_benches);
    RUN_TEST(test_qsf_nativelink_declares);
    RUN_TEST(test_qsf_warnings);
    RUN_TEST(test_qsf_time_conversion);
    RUN_TEST(test_qsf_time_warnings);
    RUN_TEST(test_qsf_missing_file_is_error);
    RUN_TEST(test_qsf_other_assignments_still_ignored);
    return UNITY_END();
}
