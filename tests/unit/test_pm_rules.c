/*
 * test_pm_rules.c — `pm_rules` (4.2 spec S-2, S-3; plan Task 3): the k6 values, the arch
 * constructor, the absent-model rule with and without an arch record, and the source-text guard
 * that keeps the 3C numbers in rules.c alone.
 */
#include "frontends/arch/arch.h"
#include "hardmap_fixture.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "passes/partial_map/rules.h"
#include "proc_build.h"
#include "unity.h"
#include "util/log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LOG_TEXT = 8192, NAME_CAP = 512, MAX_FILE = 262144, K6_WIDTH = 36, K6_DEPTH = 15, K6_MIN = 3 };

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
#endif

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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static void assert_same_numbers(pm_rules a, pm_rules b) {
    TEST_ASSERT_EQUAL(a.have_mul, b.have_mul);
    TEST_ASSERT_EQUAL_UINT32(a.mul_width, b.mul_width);
    TEST_ASSERT_EQUAL_UINT32(a.min_mul, b.min_mul);
    TEST_ASSERT_EQUAL(a.have_adder, b.have_adder);
    TEST_ASSERT_EQUAL_UINT32(a.min_add, b.min_add);
    TEST_ASSERT_EQUAL(a.have_sp, b.have_sp);
    TEST_ASSERT_EQUAL(a.have_dp, b.have_dp);
    TEST_ASSERT_EQUAL_UINT32(a.mem_depth, b.mem_depth);
    TEST_ASSERT_EQUAL_UINT32(a.mem_width, b.mem_width);
}

static void test_k6_fixed_values(void) {
    pm_rules rules = pm_rules_k6_fixed();
    TEST_ASSERT_FALSE(rules.has_record);
    TEST_ASSERT_TRUE(rules.have_mul && rules.have_adder && rules.have_sp && rules.have_dp);
    TEST_ASSERT_EQUAL_UINT32(K6_WIDTH, rules.mul_width);
    TEST_ASSERT_EQUAL_UINT32(K6_MIN, rules.min_mul);
    TEST_ASSERT_EQUAL_UINT32(1, rules.min_add);
    TEST_ASSERT_EQUAL_UINT32(K6_DEPTH, rules.mem_depth);
    TEST_ASSERT_EQUAL_UINT32(1, rules.mem_width);
}

static void test_from_arch_stub_is_k6(void) {
    pm_rules none = pm_rules_from_arch(NULL);
    TEST_ASSERT_FALSE(none.has_record);
    assert_same_numbers(none, pm_rules_k6_fixed());
    odin3_arch *arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(arch);
    pm_rules with = pm_rules_from_arch(arch);
    TEST_ASSERT_TRUE(with.has_record);
    assert_same_numbers(with, pm_rules_k6_fixed());
    odin3_arch_destroy(arch);
}

/* --- S-3: an absent model ------------------------------------------------------------------ */

static const char *const MUL_RTLIL =
    "module \\m\n"
    "  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n  wire width 8 output 3 \\y\n"
    "  cell $mul \\u\n"
    "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n    parameter \\A_WIDTH 4\n"
    "    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 8\n"
    "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\nend\n";

static void test_absent_model_without_record_is_the_h3_error(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(MUL_RTLIL, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: cell type 'multiply' is not registered"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    pb_free(&res);
}

static void test_absent_model_with_record_is_soft(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(MUL_RTLIL, &res));
    odin3_arch *arch = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(res.design, &arch));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "partial_map: architecture 'unnamed' has no 'multiply' model; 1 '$mul' "
                         "cells left to lower"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no 'adder' model; 0 '$add'/'$sub' cells left"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no 'single_port_ram' model; 0 '$mem' cells left"));
    TEST_ASSERT_NULL(strstr(log_text, "error"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    pb_free(&res);
}

static void test_record_with_library_maps_as_before(void) {
    pb_result res;
    hm_build_ok(MUL_RTLIL, &res);
    odin3_arch *arch = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(res.design, &arch));
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.multiply);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_NULL(strstr(log_text, "has no 'multiply' model"));
    pb_free(&res);
}

/* --- the guard: the 3C numbers live in rules.c alone ---------------------------------------- */

/* Drops comments, leaving strings and code. */
static void strip_comments(char *text) {
    char *src = text;
    char *dst = text;
    bool in_str = false;
    while (*src != '\0') {
        if (in_str) {
            in_str = !(*src == '"' && src[-1] != '\\');
            *dst++ = *src++;
        } else if (src[0] == '/' && src[1] == '*') {
            src += 2;
            while (*src != '\0' && !(src[0] == '*' && src[1] == '/')) {
                src++;
            }
            src += *src != '\0' ? 2 : 0;
        } else if (src[0] == '/' && src[1] == '/') {
            while (*src != '\0' && *src != '\n') {
                src++;
            }
        } else {
            in_str = *src == '"';
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* True when text holds the number token 36 or 15 (with an optional U). */
static bool has_k6_literal(const char *text) {
    for (const char *at = text; *at != '\0'; at++) {
        bool starts = at == text || !(isalnum((unsigned char)at[-1]) || at[-1] == '_');
        if (starts && (strncmp(at, "36", 2) == 0 || strncmp(at, "15", 2) == 0)) {
            const char *end = at + 2;
            end += *end == 'U' ? 1 : 0;
            if (!(isalnum((unsigned char)*end) || *end == '_')) {
                return true;
            }
        }
    }
    return false;
}

static bool is_source(const char *name) {
    size_t len = strlen(name);
    return len > 2 && (strcmp(name + len - 2, ".c") == 0 || strcmp(name + len - 2, ".h") == 0);
}

static void check_file(const char *dir, const char *name) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *text = calloc(MAX_FILE, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, MAX_FILE - 1, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < MAX_FILE - 1, path);
    strip_comments(text);
    TEST_ASSERT_FALSE_MESSAGE(has_k6_literal(text), path);
    free(text);
}

static void test_no_k6_literal_outside_rules(void) {
    const char *dir = ODIN3_SOURCE_DIR "/src/passes/partial_map";
    DIR *handle = opendir(dir);
    TEST_ASSERT_NOT_NULL(handle);
    uint32_t checked = 0;
    for (const struct dirent *ent = readdir(handle); ent != NULL; ent = readdir(handle)) {
        if (is_source(ent->d_name) && strcmp(ent->d_name, "rules.c") != 0) {
            check_file(dir, ent->d_name);
            checked++;
        }
    }
    (void)closedir(handle);
    TEST_ASSERT_TRUE(checked >= 10U);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_k6_fixed_values);
    RUN_TEST(test_from_arch_stub_is_k6);
    RUN_TEST(test_absent_model_without_record_is_the_h3_error);
    RUN_TEST(test_absent_model_with_record_is_soft);
    RUN_TEST(test_record_with_library_maps_as_before);
    RUN_TEST(test_no_k6_literal_outside_rules);
    return UNITY_END();
}
