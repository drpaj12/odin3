/*
 * test_pm_mem_arch.c — 4.4 memories from the arch record (spec R-1, R-2, R-5, R-7, S-3, R-8; plan
 * Task 9): the depth limit D comes from the record (COFFE-like fixture: 11), an arch without a RAM
 * model leaves the `$mem` to lower, and an arch with one RAM kind leaves a memory that needs the
 * other soft. The k6 value (15, no record) is unchanged. Value checks use hardmap_ramsim.c on the
 * IR before and after the pass.
 */
#include "hardmap_fixture.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "pm_mem_fixture.h"
#include "unity.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 16384, K6_DEPTH = 15, D11 = 11 };

#define D11_XML ODIN3_ARCH_DATA_DIR "/ram_d11.xml"
#define SP_ONLY_XML ODIN3_ARCH_DATA_DIR "/ram_sp_only.xml"
#define NONE_XML ODIN3_ARCH_DATA_DIR "/acc_none.xml"

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

/* Every live RAM node of type has ADDR_WIDTH addr, DATA_WIDTH data and that many address pins. */
static void expect_blocks(const pb_result *res, const char *type, uint32_t count, uint32_t addr,
                          uint32_t data) {
    TEST_ASSERT_EQUAL_UINT32(count, hm_count(res->module, type));
    for (uint32_t i = 0; i < count; i++) {
        odin3_node_id node = hm_nth(res->module, type, i);
        TEST_ASSERT_EQUAL_INT64(addr, odin3_node_param(res->module, node, 0)->i);
        TEST_ASSERT_EQUAL_INT64(data, odin3_node_param(res->module, node, 1)->i);
        bool dual = strcmp(type, "dual_port_ram") == 0;
        TEST_ASSERT_EQUAL_UINT32(addr, odin3_node_port(res->module, node, dual ? 4 : 2).count);
    }
}

/* R-2: the depth limit is the record's: a 13-bit SP RAM is 2^(13-11) blocks of 11 address bits. */
static void test_r2_sp_instance_depth_11(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_INSTANCE, false, 13, 1, false}, D11_XML, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, &stats));
    expect_blocks(&res, "single_port_ram", 4, D11, 1);
    TEST_ASSERT_EQUAL_UINT64(4, stats.sp_slices);
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "$and")); /* write enables: 2 + 4 */
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "$dff")); /* registered selects (D4) */
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "$mux"));
    pb_free(&res);
}

/* R-5: a 9-bit memory is only padded to D. */
static void test_r5_pad_to_d(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_INSTANCE, false, 9, 1, false}, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    expect_blocks(&res, "single_port_ram", 1, D11, 1);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mux"));
    pb_free(&res);
}

/* An instance that already has D address bits and 1 data bit is left alone. */
static void test_arch_shape_untouched(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_INSTANCE, false, D11, 1, false}, D11_XML, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.sp_slices);
    expect_blocks(&res, "single_port_ram", 1, D11, 1);
    pb_free(&res);
}

/* 3C's case: no record, 16 address bits, D = 15: two blocks. */
static void test_k6_depth_15_unchanged(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_INSTANCE, false, 16, 1, false}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    expect_blocks(&res, "single_port_ram", 2, K6_DEPTH, 1);
    pb_free(&res);
}

/* R-7: a dual-port instance splits on both ports. */
static void test_r7_dp_instance_depth_11(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_INSTANCE, true, 13, 1, false}, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    expect_blocks(&res, "dual_port_ram", 4, D11, 1);
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "$mux")); /* one tree per read port */
    pb_free(&res);
}

/* An inferred `$mem` of 13 address bits: the same split, one block per leaf and data bit. */
static void test_mem_depth_11(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 13, 2, false}, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mem"));
    expect_blocks(&res, "single_port_ram", 8, D11, 1);
    pb_free(&res);
}

/* The 1E-style simulation (hardmap_ramsim.c): the blocks read back what the memory does. */
static void test_sim_instance_sp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_INSTANCE, false, 13, 2, false}, D11_XML, NULL, 0x1803U));
}

static void test_sim_instance_dp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_INSTANCE, true, 13, 1, false}, D11_XML, NULL, 0x1803U));
}

static void test_sim_mem_sp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, false, 13, 2, false}, D11_XML, NULL, 0x1803U));
}

static void test_sim_mem_dp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, true, 12, 1, false}, D11_XML, NULL, 0x0C03U));
}

/* S-3, R-8: no RAM model: the `$mem` is left to lower, one INFO per kind, with the bits. */
static void test_no_ram_model_is_soft(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 5, 8, false}, NONE_XML, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT64(0, stats.sp_slices + stats.dp_slices);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no 'single_port_ram' model; 1 '$mem' cells left"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: 256 bits of '$mem' left soft"));
    TEST_ASSERT_NULL(strstr(log_text, "error"));
    pb_free(&res);
}

/* R-6: an SP-only arch maps a `$mem` that fits one SP block ... */
static void test_sp_only_maps_sp_memory(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 6, 4, false}, SP_ONLY_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    expect_blocks(&res, "single_port_ram", 4, D11, 1);
    pb_free(&res);
}

/* ... and leaves one that needs a DP block soft, with the R-11 warning (no error). */
static void test_sp_only_leaves_dp_memory_soft(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, true, 6, 4, false}, SP_ONLY_XML, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no fixed-subset mapping; left to lower"));
    pb_free(&res);
}

/* R-10: a read enable that is not constant 1 is emulated in logic, not left soft: the RAM reads
 * every cycle, the enable is registered, and a hold register keeps the output while it is 0. */
static void test_read_enable_is_emulated(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 6, 8, true}, D11_XML, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT64(0, stats.soft);
    expect_blocks(&res, "single_port_ram", 8, D11, 1);
    TEST_ASSERT_EQUAL_UINT32(9, hm_count(res.module, "$dff")); /* the enable and 8 hold bits */
    TEST_ASSERT_EQUAL_UINT32(8, hm_count(res.module, "$mux"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "read port 0 has an enable that is not constant 1"));
    pb_free(&res);
}

/* The same value, for random reads, writes and enable (the enable toggles about every other
 * cycle), on one block, on a depth split, and on a dual-port memory. */
static void test_sim_read_enable(void) {
    TEST_ASSERT_TRUE(pmf_sim_equal((pmf_case){PMF_MEM, false, 6, 3, true}, D11_XML, NULL, 0x3FU));
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, false, 13, 2, true}, D11_XML, NULL, 0x1803U));
    TEST_ASSERT_TRUE(pmf_sim_equal((pmf_case){PMF_MEM, true, 5, 2, true}, D11_XML, NULL, 0x1FU));
}

/* Peter 2026-10-20: a memory left soft whose lowering would create more than 50,000 flip-flops
 * draws a located WARNING that names it, its bits and the reason; never an error. */
static void test_big_soft_memory_warns_no_ram_model(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 13, 8, false}, NONE_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "memory 'mem' of 65536 bits (width x depth) stays soft because the architecture "
                  "has no RAM model; lowering it creates that many flip-flops (more than 50000)"));
    pb_free(&res);
}

static void test_big_soft_memory_warns_pattern(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, true, 13, 8, false}, SP_ONLY_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "memory 'mem' of 65536 bits (width x depth) stays soft "
                                          "because it has no fixed-subset mapping"));
    pb_free(&res);
}

/* At the limit (50,000 bits or fewer) there is no such warning. */
static void test_small_soft_memory_does_not_warn(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 12, 8, false}, NONE_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    TEST_ASSERT_NULL(strstr(log_text, "stays soft"));
    pb_free(&res);
}

/* Review M-1: the tech library registers every hard type but the record has no RAM model: the
 * memory phases are skipped, so the INFO and the 50k warning must follow the record. */
static void test_lib_with_ramless_record(void) {
    pb_result res;
    pmf_build_lib_record((pmf_case){PMF_MEM, false, 13, 8, false}, NONE_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no 'single_port_ram' model; 1 '$mem' cells left"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "memory 'mem' of 65536 bits (width x depth) stays soft"));
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_r2_sp_instance_depth_11);
    RUN_TEST(test_r5_pad_to_d);
    RUN_TEST(test_arch_shape_untouched);
    RUN_TEST(test_k6_depth_15_unchanged);
    RUN_TEST(test_r7_dp_instance_depth_11);
    RUN_TEST(test_mem_depth_11);
    RUN_TEST(test_sim_instance_sp);
    RUN_TEST(test_sim_instance_dp);
    RUN_TEST(test_sim_mem_sp);
    RUN_TEST(test_sim_mem_dp);
    RUN_TEST(test_read_enable_is_emulated);
    RUN_TEST(test_sim_read_enable);
    RUN_TEST(test_no_ram_model_is_soft);
    RUN_TEST(test_lib_with_ramless_record);
    RUN_TEST(test_big_soft_memory_warns_no_ram_model);
    RUN_TEST(test_big_soft_memory_warns_pattern);
    RUN_TEST(test_small_soft_memory_does_not_warn);
    RUN_TEST(test_sp_only_maps_sp_memory);
    RUN_TEST(test_sp_only_leaves_dp_memory_soft);
    return UNITY_END();
}
