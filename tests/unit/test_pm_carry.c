/*
 * test_pm_carry.c — partial_map 4.5 carry chains from the arch record (4.2 spec C-1 to C-4; plan
 * Tasks 11 and 12): the `adder` model decides whether `$add` and `$sub` map (S-3), `--min-add`,
 * the chain shape with an arch record equal to 3C's, the chain sanity warning (C-2) and the
 * carry pack pattern count (C-3). The sweep archs need VTR_ROOT and are ignored without it.
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
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LOG_TEXT = 16384, NAME_CAP = 1024, MAX_ARCH = 4194304, MIN_ADD_TEST = 8, SKIP_EXIT = 77 };

#ifndef ODIN3_ARCH_DATA_DIR
#error "ODIN3_ARCH_DATA_DIR must name tests/unit/data/arch"
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static void assert_logged(const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), needle);
}

static void assert_not_logged(const char *needle) {
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, needle), needle);
}

/* --- builders ------------------------------------------------------------------------------ */

/* A module with one cell per entry of widths: `$add` (or `$sub`) u<i> of widths[i] bits, Y one
 * bit wider. */
static void build_adds(const char *type, const uint32_t *widths, size_t count, pb_result *res,
                       bool with_lib) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    char buf[512];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("module \\m\n")));
    uint32_t port = 1;
    for (size_t i = 0; i < count; i++) {
        (void)snprintf(buf, sizeof buf,
                       "  wire width %" PRIu32 " input %" PRIu32 " \\a%zu\n  wire width %" PRIu32
                       " input %" PRIu32 " \\b%zu\n  wire width %" PRIu32 " output %" PRIu32
                       " \\y%zu\n",
                       widths[i], port, i, widths[i], port + 1, i, widths[i] + 1, port + 2, i);
        port += 3;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(buf)));
    }
    for (size_t i = 0; i < count; i++) {
        (void)snprintf(buf, sizeof buf,
                       "  cell %s \\u%zu\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                       "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
                       "\n    parameter \\Y_WIDTH %" PRIu32
                       "\n    connect \\A \\a%zu\n    connect \\B \\b%zu\n    connect \\Y \\y%zu\n"
                       "  end\n",
                       type, i, widths[i], widths[i], widths[i] + 1, i, i, i);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(buf)));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("end\n")));
    if (with_lib) {
        hm_build_ok(text.data, res);
    } else {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text.data, res));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_design_set_top(res->design, odin3_module_id_of(res->module)));
    }
    odin3_strbuf_free(&text);
}

#ifndef PM_CARRY_VTR
static void read_arch_fixture(pb_result *res, const char *file) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_read(res->design, path));
}
#endif

/* The record of the fixture or sweep arch at path, parsed with its library made. */
static odin3_arch *parse_arch(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *text = calloc(MAX_ARCH, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, MAX_ARCH - 1, fp);
    (void)fclose(fp);
    odin3_arch *arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(arch);
    const odin3_arch_src src = {path, {text, got}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_parse(arch, &src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_make_library(arch));
    free(text);
    return arch;
}

#ifndef PM_CARRY_VTR
static odin3_arch *parse_fixture(const char *file) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, file);
    return parse_arch(path);
}
#endif

#ifdef PM_CARRY_VTR
/* The path of a sweep arch under VTR_ROOT, or the test is ignored. */
static void sweep_path(const char *rel, char *path, size_t cap) {
    const char *root = getenv("VTR_ROOT"); /* main has checked it is set */
    TEST_ASSERT_NOT_NULL(root);
    (void)snprintf(path, cap, "%s/vtr_flow/arch/%s", root, rel);
}
#endif

/* --- accessors (C-2, C-3) ------------------------------------------------------------------- */

#ifndef PM_CARRY_VTR
static void test_accessors_chain_fixture(void) {
    odin3_arch *arch = parse_fixture("adder_chain.xml");
    TEST_ASSERT_TRUE(odin3_arch_chain_pattern_reaches_adder(arch));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_arch_adder_cin_patterns(arch));
    odin3_arch_destroy(arch);
}
#endif

#ifndef PM_CARRY_VTR
static void test_accessors_nochain_fixtures(void) {
    static const char *const files[] = {"adder_nochain.xml", "adder_samename.xml", "no_adder.xml",
                                        "acc_27.xml", "acc_none.xml"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        odin3_arch *arch = parse_fixture(files[i]);
        TEST_ASSERT_FALSE_MESSAGE(odin3_arch_chain_pattern_reaches_adder(arch), files[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, odin3_arch_adder_cin_patterns(arch), files[i]);
        odin3_arch_destroy(arch);
    }
}
#endif

#ifdef PM_CARRY_VTR
static void test_accessors_sweep_archs(void) {
    static const char *const archs[] = {
        "timing/k6_frac_N10_frac_chain_mem32K_40nm.xml",
        "COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml"};
    for (size_t i = 0; i < sizeof archs / sizeof archs[0]; i++) {
        char path[NAME_CAP];
        sweep_path(archs[i], path, sizeof path);
        odin3_arch *arch = parse_arch(path);
        TEST_ASSERT_TRUE_MESSAGE(odin3_arch_chain_pattern_reaches_adder(arch), archs[i]);
        TEST_ASSERT_TRUE_MESSAGE(odin3_arch_adder_cin_patterns(arch) >= 1, archs[i]);
        odin3_arch_destroy(arch);
    }
}

static void test_accessors_k4(void) {
    char path[NAME_CAP];
    sweep_path("timing/k4_N4_90nm.xml", path, sizeof path);
    odin3_arch *k4 = parse_arch(path);
    TEST_ASSERT_FALSE(odin3_arch_chain_pattern_reaches_adder(k4));
    odin3_arch_destroy(k4);
}
#endif

/* --- C-1: the model decides ----------------------------------------------------------------- */

#ifndef PM_CARRY_VTR
static void test_no_adder_model_leaves_add_and_sub_soft(void) {
    static const uint32_t widths[] = {4, 9};
    pb_result res;
    build_adds("$add", widths, 2, &res, false);
    read_arch_fixture(&res, "no_adder.xml");
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    assert_logged("partial_map: architecture 'no_adder' has no 'adder' model; 2 '$add'/'$sub' "
                  "cells left to lower");
    assert_not_logged("carry pack pattern");
    TEST_ASSERT_EQUAL_UINT64(0, stats.adder);
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "$add"));
    pb_free(&res);
}
#endif

#ifndef PM_CARRY_VTR
static void test_no_adder_model_sub(void) {
    static const uint32_t widths[] = {5};
    pb_result res;
    build_adds("$sub", widths, 1, &res, false);
    read_arch_fixture(&res, "no_adder.xml");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("has no 'adder' model; 1 '$add'/'$sub' cells left to lower");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$sub"));
    pb_free(&res);
}
#endif

/* --- C-4: --min-add -------------------------------------------------------------------------- */

#ifndef PM_CARRY_VTR
static void test_min_add_default_maps_every_width(void) {
    static const uint32_t widths[] = {1, 4, 7, 8, 9};
    pb_result res;
    build_adds("$add", widths, 5, &res, false);
    read_arch_fixture(&res, "acc_27.xml");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32(3 + 6 + 9 + 10 + 11, hm_count(res.module, "adder"));
    pb_free(&res);
}
#endif

#ifndef PM_CARRY_VTR
static void test_min_add_8_leaves_small_adds_soft(void) {
    static const uint32_t widths[] = {4, 7, 8, 9};
    pb_result res;
    build_adds("$add", widths, 4, &res, false);
    read_arch_fixture(&res, "acc_27.xml");
    odin3_partial_map_opts opts = {.min_add = MIN_ADD_TEST};
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_partial_map_run_opts(&res.ctx, res.design, &opts, &stats));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32(10 + 11, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.chains);
    pb_free(&res);
}
#endif

#ifndef PM_CARRY_VTR
static void test_min_add_applies_to_sub_and_without_record(void) {
    static const uint32_t widths[] = {4, 8};
    pb_result res;
    build_adds("$sub", widths, 2, &res, true);
    odin3_partial_map_opts opts = {.min_add = MIN_ADD_TEST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run_opts(&res.ctx, res.design, &opts, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$sub"));
    TEST_ASSERT_EQUAL_UINT32(10, hm_count(res.module, "adder"));
    pb_free(&res);
}
#endif

/* --- the chain shape with a record equals the 3C shape --------------------------------------- */

static char *rtlil_after(const char *arch_path, const char *type) {
    static const uint32_t widths[] = {1, 4, 9};
    pb_result res;
    build_adds(type, widths, 3, &res, arch_path == NULL);
    if (arch_path != NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_read(res.design, arch_path));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &out));
    char *copy = strdup(out.data);
    TEST_ASSERT_NOT_NULL(copy);
    odin3_strbuf_free(&out);
    pb_free(&res);
    return copy;
}

static void assert_shape_equals_3c(const char *arch_path) {
    static const char *const types[] = {"$add", "$sub"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        char *plain = rtlil_after(NULL, types[i]);
        char *with_arch = rtlil_after(arch_path, types[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(plain, with_arch, types[i]);
        free(plain);
        free(with_arch);
    }
}

#ifndef PM_CARRY_VTR
static void test_shape_fixture_record(void) {
    char path[NAME_CAP];
    (void)snprintf(path, sizeof path, "%s/acc_27.xml", ODIN3_ARCH_DATA_DIR);
    assert_shape_equals_3c(path);
}
#endif

#ifdef PM_CARRY_VTR
static void test_shape_k6_and_coffe(void) {
    static const char *const archs[] = {
        "timing/k6_frac_N10_frac_chain_mem32K_40nm.xml",
        "COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml"};
    for (size_t i = 0; i < sizeof archs / sizeof archs[0]; i++) {
        char path[NAME_CAP];
        sweep_path(archs[i], path, sizeof path);
        assert_shape_equals_3c(path);
    }
}
#endif

/* --- C-2, C-3: the sanity warning and the report --------------------------------------------- */

#ifndef PM_CARRY_VTR
static void run_with_fixture(const char *file) {
    static const uint32_t widths[] = {4};
    pb_result res;
    build_adds("$add", widths, 1, &res, false);
    read_arch_fixture(&res, file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    pb_free(&res);
}
#endif

#ifndef PM_CARRY_VTR
static void test_adder_without_pack_pattern_warns(void) {
    run_with_fixture("adder_nochain.xml");
    assert_logged("partial_map: architecture 'adder_nochain' has an 'adder' model but no carry "
                  "pack pattern; chains may not pack");
    assert_not_logged("carry pack patterns");
}
#endif

/* M-2: a pack pattern on `add.cin` / `add.cout` of another pb_type, whose leaf is only named like
 * the adder leaf, does not count. */
#ifndef PM_CARRY_VTR
static void test_same_name_pb_elsewhere_still_warns(void) {
    run_with_fixture("adder_samename.xml");
    assert_logged("partial_map: architecture 'adder_samename' has an 'adder' model but no carry "
                  "pack pattern; chains may not pack");
}
#endif

#ifndef PM_CARRY_VTR
static void test_adder_with_pack_pattern_reports_count(void) {
    run_with_fixture("adder_chain.xml");
    assert_not_logged("chains may not pack");
    assert_logged("partial_map: architecture 'adder_chain' has 2 carry pack patterns");
}
#endif

#ifndef PM_CARRY_VTR
static void test_no_adder_model_gives_no_chain_message(void) {
    run_with_fixture("no_adder.xml");
    assert_not_logged("chains may not pack");
    assert_not_logged("carry pack pattern");
}
#endif

#ifndef PM_CARRY_VTR
static void test_no_record_gives_no_chain_message(void) {
    static const uint32_t widths[] = {4};
    pb_result res;
    build_adds("$add", widths, 1, &res, true);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_not_logged("chains may not pack");
    assert_not_logged("carry pack pattern");
    pb_free(&res);
}
#endif

#ifdef PM_CARRY_VTR
static void run_sweep_arch(const char *rel) {
    static const uint32_t widths[] = {4};
    char path[NAME_CAP];
    sweep_path(rel, path, sizeof path);
    pb_result res;
    build_adds("$add", widths, 1, &res, false);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_read(res.design, path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    pb_free(&res);
}

static void test_sweep_archs_chain_message(void) {
    run_sweep_arch("timing/k6_frac_N10_frac_chain_mem32K_40nm.xml");
    assert_not_logged("chains may not pack");
    assert_logged("carry pack patterns");
    log_text[0] = '\0';
    run_sweep_arch("COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml");
    assert_not_logged("chains may not pack");
    assert_logged("carry pack patterns");
}

static void test_k4_has_no_adder(void) {
    run_sweep_arch("timing/k4_N4_90nm.xml");
    assert_logged("has no 'adder' model; 1 '$add'/'$sub' cells left to lower");
    assert_not_logged("chains may not pack");
}
#endif

int main(void) {
#ifdef PM_CARRY_VTR
    if (getenv("VTR_ROOT") == NULL) {
        (void)fprintf(stderr, "test_pm_carry_vtr: VTR_ROOT not set, skipped\n");
        return SKIP_EXIT;
    }
    UNITY_BEGIN();
    RUN_TEST(test_accessors_sweep_archs);
    RUN_TEST(test_accessors_k4);
    RUN_TEST(test_shape_k6_and_coffe);
    RUN_TEST(test_sweep_archs_chain_message);
    RUN_TEST(test_k4_has_no_adder);
#else
    UNITY_BEGIN();
    RUN_TEST(test_accessors_chain_fixture);
    RUN_TEST(test_accessors_nochain_fixtures);
    RUN_TEST(test_no_adder_model_leaves_add_and_sub_soft);
    RUN_TEST(test_no_adder_model_sub);
    RUN_TEST(test_min_add_default_maps_every_width);
    RUN_TEST(test_min_add_8_leaves_small_adds_soft);
    RUN_TEST(test_min_add_applies_to_sub_and_without_record);
    RUN_TEST(test_shape_fixture_record);
    RUN_TEST(test_adder_without_pack_pattern_warns);
    RUN_TEST(test_adder_with_pack_pattern_reports_count);
    RUN_TEST(test_same_name_pb_elsewhere_still_warns);
    RUN_TEST(test_no_adder_model_gives_no_chain_message);
    RUN_TEST(test_no_record_gives_no_chain_message);
#endif
    return UNITY_END();
}
