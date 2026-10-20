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

enum {
    LOG_TEXT = 8192,
    NAME_CAP = 512,
    MAX_FILE = 262144,
    MAX_ARCH = 4194304,
    K6_WIDTH = 36,
    K6_DEPTH = 15,
    K6_MIN = 3
};

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

static void assert_same_numbers(pm_rules left, pm_rules right) {
    TEST_ASSERT_EQUAL(left.have_mul, right.have_mul);
    TEST_ASSERT_EQUAL_UINT32(left.mul_width, right.mul_width);
    TEST_ASSERT_EQUAL_UINT32(left.min_mul, right.min_mul);
    TEST_ASSERT_EQUAL(left.have_adder, right.have_adder);
    TEST_ASSERT_EQUAL_UINT32(left.min_add, right.min_add);
    TEST_ASSERT_EQUAL(left.have_sp, right.have_sp);
    TEST_ASSERT_EQUAL(left.have_dp, right.have_dp);
    TEST_ASSERT_EQUAL_UINT32(left.mem_depth, right.mem_depth);
    TEST_ASSERT_EQUAL_UINT32(left.mem_width, right.mem_width);
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

/* Reads the fixture `file` into a fresh record with its library text made. */
static odin3_arch *arch_of(const char *file) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, file);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *text = calloc(MAX_FILE, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, MAX_FILE - 1, fp);
    (void)fclose(fp);
    odin3_arch *arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(arch);
    const odin3_arch_src src = {file, {text, got}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_parse(arch, &src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_make_library(arch));
    free(text);
    return arch;
}

static void test_from_arch_null_is_k6(void) {
    pm_rules none = pm_rules_from_arch(NULL);
    TEST_ASSERT_FALSE(none.has_record);
    assert_same_numbers(none, pm_rules_k6_fixed());
}

static void test_from_arch_27(void) {
    odin3_arch *arch = arch_of("acc_27.xml");
    pm_rules rules = pm_rules_from_arch(arch);
    TEST_ASSERT_TRUE(rules.has_record && rules.have_mul && rules.have_adder);
    TEST_ASSERT_TRUE(rules.have_sp && rules.have_dp);
    TEST_ASSERT_EQUAL_UINT32(27, rules.mul_width);
    TEST_ASSERT_EQUAL_UINT32(11, rules.mem_depth);
    TEST_ASSERT_EQUAL_UINT32(K6_MIN, rules.min_mul);
    TEST_ASSERT_EQUAL_UINT32(1, rules.min_add);
    TEST_ASSERT_EQUAL_UINT32(1, rules.mem_width);
    TEST_ASSERT_EQUAL_STRING("", log_text);
    odin3_arch_destroy(arch);
}

static void test_from_arch_asymmetric_multiplier(void) {
    odin3_arch *arch = arch_of("acc_25x18.xml");
    pm_rules rules = pm_rules_from_arch(arch);
    TEST_ASSERT_FALSE(rules.have_mul);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: 'multiply' model of 'acc_25x18' is 25x18, "
                                          "not square; '$mul' cells are left to lower"));
    odin3_arch_destroy(arch);
}

static void test_from_arch_renamed_multiplier(void) {
    odin3_arch *arch = arch_of("acc_renamed.xml");
    pm_rules rules = pm_rules_from_arch(arch);
    TEST_ASSERT_FALSE(rules.have_mul || rules.have_adder || rules.have_sp || rules.have_dp);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "'multiply' model of 'acc_renamed' does not have the "
                                          "ports a, b and out"));
    odin3_arch_destroy(arch);
}

static void test_from_arch_no_models(void) {
    odin3_arch *arch = arch_of("acc_none.xml");
    pm_rules rules = pm_rules_from_arch(arch);
    TEST_ASSERT_TRUE(rules.has_record);
    TEST_ASSERT_FALSE(rules.have_mul || rules.have_adder || rules.have_sp || rules.have_dp);
    TEST_ASSERT_EQUAL_STRING("", log_text);
    odin3_arch_destroy(arch);
}

static void test_from_arch_k6_equals_fixed(void) {
    const char *root = getenv("VTR_ROOT");
    if (root == NULL) {
        TEST_IGNORE_MESSAGE("VTR_ROOT not set");
    }
    char rel[NAME_CAP];
    (void)snprintf(rel, sizeof rel,
                   "%s/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml", root);
    FILE *fp = fopen(rel, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, rel);
    char *text = calloc(MAX_ARCH, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, MAX_ARCH - 1, fp);
    (void)fclose(fp);
    odin3_arch *arch = odin3_arch_create();
    const odin3_arch_src src = {rel, {text, got}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_parse(arch, &src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_make_library(arch));
    free(text);
    pm_rules rules = pm_rules_from_arch(arch);
    TEST_ASSERT_TRUE(rules.has_record);
    assert_same_numbers(rules, pm_rules_k6_fixed());
    odin3_arch_destroy(arch);
}

static void assert_logged(const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), needle);
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_arch_read(res.design, ODIN3_ARCH_DATA_DIR "/acc_none.xml"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("partial_map: architecture 'acc_none' has no 'multiply' model; 1 '$mul' cells "
                  "left to lower");
    assert_logged("has no 'adder' model; 0 '$add'/'$sub' cells left");
    assert_logged("has no 'single_port_ram' model; 0 '$mem' cells left");
    TEST_ASSERT_NULL(strstr(log_text, "error"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    pb_free(&res);
}

static void test_record_maps_with_its_own_width(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(MUL_RTLIL, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_read(res.design, ODIN3_ARCH_DATA_DIR "/acc_27.xml"));
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.multiply);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    odin3_node_id node = hm_nth(res.module, "multiply", 0);
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    TEST_ASSERT_EQUAL_INT64(27, odin3_node_param(res.module, node, 0)->i);
    TEST_ASSERT_EQUAL_UINT32(27, odin3_node_port(res.module, node, 0).count);
    TEST_ASSERT_EQUAL_UINT32(54, odin3_node_port(res.module, node, 2).count);
    TEST_ASSERT_NULL(strstr(log_text, "has no 'multiply' model"));
    pb_free(&res);
}

static void test_asymmetric_record_leaves_the_mul_soft(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(MUL_RTLIL, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_arch_read(res.design, ODIN3_ARCH_DATA_DIR "/acc_25x18.xml"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "is 25x18, not square; '$mul' cells are left to lower"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "multiply"));
    pb_free(&res);
}

/* --- the guard: the 3C numbers live in rules.c alone ---------------------------------------- */

/* The text after the comment that starts at src; src itself when none does. */
static const char *skip_comment(const char *src) {
    if (src[0] == '/' && src[1] == '*') {
        src += 2;
        while (*src != '\0' && !(src[0] == '*' && src[1] == '/')) {
            src++;
        }
        return *src != '\0' ? src + 2 : src;
    }
    if (src[0] == '/' && src[1] == '/') {
        while (*src != '\0' && *src != '\n') {
            src++;
        }
    }
    return src;
}

/* Copies the string or character literal at src to *dst; returns the text after it. */
static const char *copy_literal(const char *src, char **dst) {
    char quote = *src;
    *(*dst)++ = *src++;
    while (*src != '\0' && *src != quote) {
        bool escape = *src == '\\' && src[1] != '\0';
        *(*dst)++ = *src++;
        if (escape) {
            *(*dst)++ = *src++;
        }
    }
    if (*src != '\0') {
        *(*dst)++ = *src++;
    }
    return src;
}

/* Drops comments, leaving strings and code. */
static void strip_comments(char *text) {
    const char *src = text;
    char *dst = text;
    while (*src != '\0') {
        const char *next = skip_comment(src);
        if (next != src) {
            src = next;
        } else if (*src == '"' || *src == '\'') {
            src = copy_literal(src, &dst);
        } else {
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

/* The contents of path, the caller frees; asserts that it reads and fits. */
static char *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *text = calloc(MAX_FILE, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, MAX_FILE - 1, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < MAX_FILE - 1, path);
    return text;
}

static void check_file(const char *dir, const char *name) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    char *text = read_source(path);
    strip_comments(text);
    bool found = has_k6_literal(text);
    free(text);
    TEST_ASSERT_FALSE_MESSAGE(found, path);
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
    RUN_TEST(test_from_arch_null_is_k6);
    RUN_TEST(test_from_arch_27);
    RUN_TEST(test_from_arch_asymmetric_multiplier);
    RUN_TEST(test_from_arch_renamed_multiplier);
    RUN_TEST(test_from_arch_no_models);
    RUN_TEST(test_from_arch_k6_equals_fixed);
    RUN_TEST(test_absent_model_without_record_is_the_h3_error);
    RUN_TEST(test_absent_model_with_record_is_soft);
    RUN_TEST(test_record_maps_with_its_own_width);
    RUN_TEST(test_asymmetric_record_leaves_the_mul_soft);
    RUN_TEST(test_no_k6_literal_outside_rules);
    return UNITY_END();
}
