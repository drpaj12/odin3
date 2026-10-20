/*
 * test_pm_mem_width.c — `--mem-width arch` (4.4 spec R-4, S-5; plan Task 10) on the COFFE-like
 * fixture ram_d11.xml: single-port modes (9,40), (10,20), (11,10), dual-port (10,20), (11,10). The
 * default (1-bit slices) is checked beside it. Values are checked on hardmap_ramsim.c.
 */
#include "frontends/arch/arch.h"
#include "hardmap_fixture.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/partial_map/hardmap.h"
#include "pm_mem_fixture.h"
#include "unity.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 16384 };

#define D11_XML ODIN3_ARCH_DATA_DIR "/ram_d11.xml"
#define NONE_XML ODIN3_ARCH_DATA_DIR "/acc_none.xml"
#define WARN_TEXT "--mem-width arch: memories are sliced to the arch's widest mode"

static char log_text[LOG_TEXT];
static const odin3_partial_map_opts ARCH_WIDTH = {.mem_width_arch = true};

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

static uint32_t occurrences(const char *needle) {
    uint32_t found = 0;
    for (const char *at = strstr(log_text, needle); at != NULL; at = strstr(at + 1, needle)) {
        found++;
    }
    return found;
}

/* The data bits of block i of type, and its ADDR_WIDTH. */
typedef struct block {
    int64_t addr;
    int64_t data;
} block;

static block block_of(const pb_result *res, const char *type, uint32_t index) {
    odin3_node_id node = hm_nth(res->module, type, index);
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    return (block){odin3_node_param(res->module, node, 0)->i,
                   odin3_node_param(res->module, node, 1)->i};
}

static bool has_mode(const odin3_arch_ram_mode *modes, uint32_t count, block blk) {
    bool found = false;
    for (uint32_t idx = 0; idx < count && idx < 16; idx++) {
        found = found || (modes[idx].addr == blk.addr && modes[idx].data == blk.data);
    }
    return found;
}

/* M3: every RAM block that was emitted is a real mode of the arch (address pins, data pins). */
static void expect_modes(const pb_result *res) {
    static const char *const k_types[2] = {"single_port_ram", "dual_port_ram"};
    const odin3_arch *arch = odin3_design_arch(res->design);
    for (uint32_t kind = 0; kind < 2; kind++) {
        odin3_arch_ram_mode modes[16];
        uint32_t count = odin3_arch_modes_of(arch, k_types[kind], modes, 16);
        for (uint32_t i = 0; i < hm_count(res->module, k_types[kind]); i++) {
            TEST_ASSERT_TRUE_MESSAGE(has_mode(modes, count, block_of(res, k_types[kind], i)),
                                     "a block that is no mode of the arch");
        }
    }
}

static pb_result run_mem(pmf_case cs, const odin3_partial_map_opts *opts) {
    pb_result res;
    pmf_build(cs, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, opts, NULL));
    if (opts != NULL) {
        expect_modes(&res);
    }
    return res;
}

/* 256 x 32: A' = 8, the widest mode with at least 8 address pins is (9, 40): one block of 32 bits
 * with 9 address pins. */
static void test_256x32_is_one_block(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 8, 32, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    block blk = block_of(&res, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(9, blk.addr);
    TEST_ASSERT_EQUAL_INT64(40, blk.data); /* the (9, 40) mode; 8 data pins padded */
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT32(1, occurrences(WARN_TEXT));
    pb_free(&res);
}

static void test_1024x20_is_one_block(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 10, 20, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    block blk = block_of(&res, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(10, blk.addr);
    TEST_ASSERT_EQUAL_INT64(20, blk.data);
    pb_free(&res);
}

static void test_2048x10_is_one_block(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 11, 10, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    block blk = block_of(&res, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(11, blk.addr);
    TEST_ASSERT_EQUAL_INT64(10, blk.data);
    pb_free(&res);
}

/* 2048 x 32: w = 10, slices of 10, 10, 10 and 2 bits. */
static void test_2048x32_is_four_blocks(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 11, 32, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(4, hm_count(res.module, "single_port_ram"));
    const int64_t want[4] = {10, 10, 10, 10}; /* the last slice is 2 bits, padded */
    for (uint32_t i = 0; i < 4; i++) {
        block blk = block_of(&res, "single_port_ram", i);
        TEST_ASSERT_EQUAL_INT64(11, blk.addr);
        TEST_ASSERT_EQUAL_INT64(want[i], blk.data);
    }
    TEST_ASSERT_EQUAL_UINT32(1, occurrences(WARN_TEXT)); /* once, not once per block */
    pb_free(&res);
}

/* The depth split is unchanged: 13 address bits are 4 leaves of 11, each cut into 10-bit slices. */
static void test_depth_split_then_width(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 13, 12, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(8, hm_count(res.module, "single_port_ram"));
    for (uint32_t i = 0; i < 8; i++) {
        block blk = block_of(&res, "single_port_ram", i);
        TEST_ASSERT_EQUAL_INT64(11, blk.addr);
        TEST_ASSERT_EQUAL_INT64(10, blk.data);
    }
    pb_free(&res);
}

/* Dual port: the DP leaves (10, 20), (11, 10). 256 x 16 gets (10, 20): one block, 10 address pins.
 */
static void test_dp_table(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, true, 8, 16, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "dual_port_ram"));
    block blk = block_of(&res, "dual_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(10, blk.addr);
    TEST_ASSERT_EQUAL_INT64(20, blk.data); /* the (10, 20) mode; 4 data pins padded */
    pb_free(&res);
}

/* An instance: 256 x 32 is one block as well; one with the mode's shape is left as it is. */
static void test_instance(void) {
    pb_result res = run_mem((pmf_case){PMF_INSTANCE, false, 8, 32, false}, &ARCH_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    block blk = block_of(&res, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(9, blk.addr);
    TEST_ASSERT_EQUAL_INT64(40, blk.data); /* the (9, 40) mode; 8 data pins padded */
    pb_free(&res);
    log_text[0] = '\0';
    odin3_hardmap_stats stats;
    pmf_build((pmf_case){PMF_INSTANCE, false, 11, 10, false}, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, &ARCH_WIDTH, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.sp_slices);
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(WARN_TEXT));
    pb_free(&res);
}

/* Off (the default): 1-bit slices at D, no warning. */
static void test_default_is_one_bit(void) {
    pb_result res = run_mem((pmf_case){PMF_MEM, false, 8, 32, false}, NULL);
    TEST_ASSERT_EQUAL_UINT32(32, hm_count(res.module, "single_port_ram"));
    block blk = block_of(&res, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_INT64(11, blk.addr);
    TEST_ASSERT_EQUAL_INT64(1, blk.data);
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(WARN_TEXT));
    pb_free(&res);
}

/* No RAM model: the option is a no-op with the S-3 INFO, no error, no slice warning. */
static void test_no_ram_model_is_noop(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 5, 8, false}, NONE_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, &ARCH_WIDTH, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has no 'single_port_ram' model"));
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(WARN_TEXT));
    TEST_ASSERT_NULL(strstr(log_text, "error"));
    pb_free(&res);
}

/* No record at all: a WARNING, the 3C result. */
static void test_no_record_is_ignored_with_warning(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 4, 2, false}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, &ARCH_WIDTH, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "single_port_ram"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "--mem-width arch needs an architecture record"));
    pb_free(&res);
}

/* The pass option: `partial_map --mem-width arch`; a bad value is an error. */
static void test_pass_option(void) {
    pb_result res;
    pmf_build((pmf_case){PMF_MEM, false, 8, 32, false}, D11_XML, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run(res.design, "partial_map",
                                                                odin3_bytes_cstr("--mem-width 3")));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: --mem-width expects arch or 1"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(res.design, "partial_map", odin3_bytes_cstr("--mem-width arch")));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    pb_free(&res);
}

/* The sliced memory reads back what the `$mem` does, for random reads and writes. */
static void test_sim_256x32(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, false, 8, 32, false}, D11_XML, &ARCH_WIDTH, 0x87U));
}

static void test_sim_2048x32(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, false, 11, 32, false}, D11_XML, &ARCH_WIDTH, 0x403U));
}

static void test_sim_depth_and_width(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, false, 13, 12, false}, D11_XML, &ARCH_WIDTH, 0x1803U));
}

static void test_sim_dp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, true, 8, 16, false}, D11_XML, &ARCH_WIDTH, 0x87U));
}

static void test_sim_instance(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_INSTANCE, false, 8, 32, false}, D11_XML, &ARCH_WIDTH, 0x87U));
}

#define MIXED_XML ODIN3_ARCH_DATA_DIR "/ram_mixed.xml"

/* M4: D is per kind. Mixed arch: SP modes (9, 40), (11, 10), DP mode (10, 20). A 12-bit SP RAM
 * splits once (D = 11), a 12-bit DP RAM twice (D = 10); with or without --mem-width arch. */
static void test_depth_is_per_kind(void) {
    for (int arch_width = 0; arch_width < 2; arch_width++) {
        const odin3_partial_map_opts *opts = arch_width != 0 ? &ARCH_WIDTH : NULL;
        pb_result res;
        pmf_build((pmf_case){PMF_INSTANCE, false, 12, 1, false}, MIXED_XML, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, opts, NULL));
        TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "single_port_ram"));
        TEST_ASSERT_EQUAL_INT64(11, block_of(&res, "single_port_ram", 0).addr);
        pb_free(&res);
        pmf_build((pmf_case){PMF_INSTANCE, true, 12, 1, false}, MIXED_XML, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&res, opts, NULL));
        TEST_ASSERT_EQUAL_UINT32(4, hm_count(res.module, "dual_port_ram"));
        TEST_ASSERT_EQUAL_INT64(10, block_of(&res, "dual_port_ram", 0).addr);
        if (opts != NULL) {
            expect_modes(&res);
        }
        pb_free(&res);
    }
}

static void test_sim_mixed_dp(void) {
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_INSTANCE, true, 12, 1, false}, MIXED_XML, NULL, 0xC03U));
    TEST_ASSERT_TRUE(
        pmf_sim_equal((pmf_case){PMF_MEM, true, 12, 2, false}, MIXED_XML, &ARCH_WIDTH, 0xC03U));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_256x32_is_one_block);
    RUN_TEST(test_1024x20_is_one_block);
    RUN_TEST(test_2048x10_is_one_block);
    RUN_TEST(test_2048x32_is_four_blocks);
    RUN_TEST(test_depth_split_then_width);
    RUN_TEST(test_dp_table);
    RUN_TEST(test_instance);
    RUN_TEST(test_default_is_one_bit);
    RUN_TEST(test_no_ram_model_is_noop);
    RUN_TEST(test_no_record_is_ignored_with_warning);
    RUN_TEST(test_pass_option);
    RUN_TEST(test_depth_is_per_kind);
    RUN_TEST(test_sim_mixed_dp);
    RUN_TEST(test_sim_256x32);
    RUN_TEST(test_sim_2048x32);
    RUN_TEST(test_sim_depth_and_width);
    RUN_TEST(test_sim_dp);
    RUN_TEST(test_sim_instance);
    return UNITY_END();
}
