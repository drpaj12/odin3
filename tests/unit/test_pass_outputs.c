/*
 * test_pass_outputs.c — unit tests for the write_json, write_verilog, write_dot and sim passes.
 */
#include "backends/blif/writer.h"
#include "backends/dot/writer.h"
#include "backends/json/writer.h"
#include "backends/verilog/writer.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif

enum { LOG_CAP = 1 << 14, FILE_CAP = 1 << 16 };

static const char *const FF = ODIN3_BLIF_FIXTURES "/ff.parmys.blif";
static const char *const OUT = "odin3_po_test.out";
static const char *const OUT2 = "odin3_po_test2.out";

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;
static char file_text[FILE_CAP];

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%s\n", msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

static odin3_status run(const char *name, const char *args) {
    return odin3_pass_run(design, name, odin3_bytes_cstr(args));
}

/* The whole file at path, NUL-terminated in file_text ("" when it cannot be read). */
static const char *slurp(const char *path) {
    file_text[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        size_t len = fread(file_text, 1, FILE_CAP - 1, file);
        file_text[len] = '\0';
        (void)fclose(file);
    }
    return file_text;
}

void setUp(void) {
    log_text[0] = '\0';
    log_len = 0;
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    (void)remove(OUT);
    (void)remove(OUT2);
}

static void read_ff(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_blif", FF));
}

static void test_registered_in_order(void) {
    static const char *const names[] = {"read_blif",    "read_techlib",  "write_blif",
                                        "write_json",   "write_verilog", "write_dot",
                                        "check",        "compact",       "stats",
                                        "sim",          "hierarchy",     "write_verilog_ast",
                                        "read_project", "dump_project",  "preprocess"};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], odin3_pass_at(i)->name);
    }
}

static void test_write_json(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", OUT));
    TEST_ASSERT_NOT_NULL(strstr(slurp(OUT), "\"creator\": \"Odin III\""));
    TEST_ASSERT_NOT_NULL(strstr(file_text, "\"dff\""));
}

static void test_write_verilog_name_styles(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog", OUT));
    TEST_ASSERT_NOT_NULL(strstr(slurp(OUT), "module dff"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog", "--name-style short "
                                                         "odin3_po_test2.out"));
    char shorter[FILE_CAP];
    (void)snprintf(shorter, sizeof shorter, "%s", slurp(OUT2));
    TEST_ASSERT_EQUAL_STRING(shorter, slurp(OUT)); /* short is the default */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run("write_verilog", "odin3_po_test2.out --name-style provenance"));
    TEST_ASSERT_NOT_NULL(strstr(slurp(OUT2), "@"));
}

static void test_write_verilog_bad_name_style(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_verilog", "x.v --name-style long"));
    TEST_ASSERT_TRUE(log_has("write_verilog: --name-style expects short or provenance"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_verilog", "x.v --name-style"));
    TEST_ASSERT_TRUE(log_has("write_verilog: --name-style expects a value"));
}

static void test_write_dot_budget_and_focus(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_dot", OUT));
    TEST_ASSERT_EQUAL_INT(0, strncmp(slurp(OUT), "digraph", strlen("digraph")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          run("write_dot", "odin3_po_test2.out --max-nodes 1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run("write_dot", "odin3_po_test2.out --max-nodes 1 --focus dff"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_dot", "--focus cone(q) odin3_po_test2.out"));
    char focus[256];
    (void)snprintf(focus, sizeof focus, "%s --focus %s:9", OUT2, FF);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_dot", focus));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_dot", "x.dot --focus nosuch"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_dot", "x.dot --focus a --focus b"));
    TEST_ASSERT_TRUE(log_has("write_dot: --focus given twice"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_dot", "x.dot --max-nodes 0"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("write_dot", "x.dot --max-nodes 1x"));
    TEST_ASSERT_TRUE(log_has("write_dot: --max-nodes expects a positive number"));
}

static void test_write_arguments(void) {
    read_ff();
    static const char *const bad[][2] = {
        {"write_json", ""},
        {"write_json", "a.json b.json"},
        {"write_json", "--focus x"},
        {"write_verilog", "a.v --focus x"},
        {"write_dot", "--name-style short a.dot"},
        {"write_dot", "-x a.dot"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, run(bad[i][0], bad[i][1]), bad[i][1]);
    }
    TEST_ASSERT_TRUE(log_has("write_json: expects one path (and its options)"));
    TEST_ASSERT_TRUE(log_has("write_json: unknown argument '--focus'"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("write_json", "/nonexistent/dir/a.json"));
}

static void test_sim_writes_vectors_deterministically(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("sim", "16 7 odin3_po_test.out"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("sim", "16 7 odin3_po_test2.out"));
    char first[FILE_CAP];
    (void)snprintf(first, sizeof first, "%s", slurp(OUT));
    TEST_ASSERT_EQUAL_STRING(first, slurp(OUT2));
    TEST_ASSERT_EQUAL_INT(0, strncmp(first, "# odin3 sim dff seed 7 cycles 16\n# input rst 1\n",
                                     strlen("# odin3 sim dff seed 7 cycles 16\n# input rst 1\n")));
    TEST_ASSERT_NOT_NULL(strstr(first, "# clock clk 0\n# output q 1\n0 "));
    TEST_ASSERT_NOT_NULL(strstr(first, "\n15 "));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("sim", "16 8 odin3_po_test2.out"));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(first, slurp(OUT2))); /* another seed, other inputs */
}

static void test_sim_summary(void) {
    read_ff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("sim", "16 7"));
    TEST_ASSERT_TRUE(log_has("sim: dff: 16 cycles, seed 7; last cycle: 15 "));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("sim", "0 1"));
    TEST_ASSERT_TRUE(log_has("sim: dff: 0 cycles, seed 1; last cycle: none"));
}

static void test_sim_arguments_and_refusals(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("sim", "4 1"));
    TEST_ASSERT_TRUE(log_has("sim: the design has no top module"));
    read_ff();
    static const char *const bad[] = {
        "", "4", "x 1", "-1 1", "4 y", "4294967296 1", "4 1 a.vec b", "4 18446744073709551616"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, run("sim", bad[i]), bad[i]);
    }
    TEST_ASSERT_TRUE(log_has("sim: expects <cycles> <seed> [<file>]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run("sim", "4 1 /nonexistent/dir/a.vec"));
}

/* --- processes (2D EL-33) -------------------------------------------------------------------- */

static bool exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        (void)fclose(file);
    }
    return file != NULL;
}

/* proc_fixture_design, with m as the top (so sim would otherwise find its top). */
static void design_with_process(void) {
    odin3_module *module = proc_fixture_design(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, odin3_module_id_of(module)));
    log_text[0] = '\0';
    log_len = 0;
}

static void expect_only_line(const char *line) {
    char want[LOG_CAP];
    (void)snprintf(want, sizeof want, "%s\n", line);
    TEST_ASSERT_EQUAL_STRING(want, log_text);
    log_text[0] = '\0';
    log_len = 0;
}

/* The writers called directly (no pass manager) refuse before opening the path. */
static void test_writers_refuse_processes(void) {
    static const char *const k_line = "module 'm' has processes; run proc first";
    design_with_process();
    (void)remove(OUT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT));
    expect_only_line(k_line);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_write(design, OUT));
    expect_only_line(k_line);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, OUT, NULL));
    expect_only_line(k_line);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, OUT, NULL));
    expect_only_line(k_line);
    TEST_ASSERT_FALSE(exists(OUT));
}

/* An existing file at the path is left as it was. */
static void test_writer_refusal_leaves_the_path(void) {
    FILE *file = fopen(OUT, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fputs("keep", file) < 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    design_with_process();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, OUT, NULL));
    TEST_ASSERT_EQUAL_STRING("keep", slurp(OUT));
}

/* The REFUSE passes of this file, through the pass manager. */
static void test_output_passes_refuse_processes(void) {
    static const char *const passes[][2] = {
        {"write_json", "odin3_po_test.out"},
        {"write_verilog", "odin3_po_test.out"},
        {"write_dot", "odin3_po_test.out"},
        {"sim", "4 1"},
    };
    design_with_process();
    for (size_t i = 0; i < sizeof passes / sizeof passes[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, run(passes[i][0], passes[i][1]),
                                      passes[i][0]);
        char line[128];
        (void)snprintf(line, sizeof line, "pass '%s': module 'm' has processes; run proc first",
                       passes[i][0]);
        expect_only_line(line);
    }
    TEST_ASSERT_FALSE(exists(OUT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_registered_in_order);
    RUN_TEST(test_write_json);
    RUN_TEST(test_write_verilog_name_styles);
    RUN_TEST(test_write_verilog_bad_name_style);
    RUN_TEST(test_write_dot_budget_and_focus);
    RUN_TEST(test_write_arguments);
    RUN_TEST(test_sim_writes_vectors_deterministically);
    RUN_TEST(test_sim_summary);
    RUN_TEST(test_sim_arguments_and_refusals);
    RUN_TEST(test_writers_refuse_processes);
    RUN_TEST(test_writer_refusal_leaves_the_path);
    RUN_TEST(test_output_passes_refuse_processes);
    return UNITY_END();
}
