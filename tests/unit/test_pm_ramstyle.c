/*
 * test_pm_ramstyle.c — 4.2 spec G-3, T-7 (plan Task 6): the attributes ram_style, rom_style,
 * ramstyle, logic_block, block_ram and no_rw_check on a memory are ignored with one warning each;
 * the mapping equals the one without them. The warning text is exact, the attribute name is matched
 * ignoring case, its value is ignored, and no rule file other than ramstyle.c names the attributes.
 */
#include "hardmap_fixture.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "proc_build.h"
#include "unity.h"
#include "util/log.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LOG_TEXT = 8192, TEXT_SIZE = 4096, PATH_SIZE = 1024, SRC_SIZE = 262144 };

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

/* A 16 x 3 `$mem` with a synchronous read port (it maps hard), after `attrs` (attribute lines). */
static void build_mem(const char *attrs, pb_result *res) {
    char text[TEXT_SIZE];
    int len = snprintf(
        text, sizeof text,
        "module \\m\n  wire width 1 input 1 \\clk\n  wire width 4 input 2 \\ra\n"
        "  wire width 4 input 3 \\wa\n  wire width 3 input 4 \\wd\n  wire width 1 input 5 \\we\n"
        "  wire width 3 output 6 \\q\n%s"
        "  cell $mem \\u\n    parameter \\MEMID \"\\\\mem\"\n    parameter \\SIZE 16\n"
        "    parameter \\OFFSET 0\n    parameter \\ABITS 4\n    parameter \\WIDTH 3\n"
        "    parameter \\RD_PORTS 1\n    parameter \\WR_PORTS 1\n"
        "    parameter \\RD_CLK_ENABLE 1'1\n    parameter \\RD_CLK_POLARITY 1'1\n"
        "    parameter \\RD_TRANSPARENT 1'0\n    parameter \\WR_CLK_ENABLE 1'1\n"
        "    parameter \\WR_CLK_POLARITY 1'1\n    parameter \\INIT 0'\n"
        "    connect \\RD_CLK \\clk\n    connect \\RD_EN 1'1\n    connect \\RD_ADDR \\ra\n"
        "    connect \\RD_DATA \\q\n    connect \\WR_CLK \\clk\n"
        "    connect \\WR_EN { \\we \\we \\we }\n    connect \\WR_ADDR \\wa\n"
        "    connect \\WR_DATA \\wd\n  end\nend\n",
        attrs);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    hm_build_ok(text, res);
}

static uint32_t count_in(const char *needle) {
    uint32_t found = 0;
    for (const char *at = log_text; (at = strstr(at, needle)) != NULL; at += strlen(needle)) {
        found++;
    }
    return found;
}

static void map_ok(pb_result *res) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(res->design, "partial_map", odin3_bytes_cstr("")));
}

static const char k_warning[] =
    "partial_map: memory 'mem': attribute 'ram_style' is ignored; mapping follows the "
    "architecture";

/* The warning text is exact, and the mapping equals the one without the attribute. */
static void test_warning_and_same_mapping(void) {
    pb_result plain;
    pb_result styled;
    build_mem("", &plain);
    map_ok(&plain);
    TEST_ASSERT_EQUAL_UINT32(0, count_in("is ignored"));
    build_mem("  attribute \\ram_style \"block\"\n", &styled);
    map_ok(&styled);
    TEST_ASSERT_EQUAL_UINT32(1, count_in(k_warning));
    const char *const types[] = {"$mem", "$dff", "single_port_ram", "dual_port_ram", "$and"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(hm_count(plain.module, types[i]),
                                 hm_count(styled.module, types[i]));
    }
    TEST_ASSERT_TRUE(hm_count(styled.module, "dual_port_ram") > 0);
    pb_free(&plain);
    pb_free(&styled);
}

/* The six names, their case, and any value. */
static void test_names_case_and_values(void) {
    const char *const attrs[] = {"  attribute \\ram_style \"block\"\n",
                                 "  attribute \\rom_style \"logic\"\n",
                                 "  attribute \\ramstyle \"M9K\"\n",
                                 "  attribute \\logic_block 1\n",
                                 "  attribute \\block_ram 0\n",
                                 "  attribute \\no_rw_check 1\n",
                                 "  attribute \\RAM_STYLE \"distributed\"\n",
                                 "  attribute \\Ram_Style 4'1010\n"};
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        pb_result res;
        log_text[0] = '\0';
        build_mem(attrs[i], &res);
        map_ok(&res);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_in("memory 'mem': attribute '"), attrs[i]);
        TEST_ASSERT_EQUAL_UINT32(1, count_in("is ignored; mapping follows the architecture"));
        TEST_ASSERT_TRUE(hm_count(res.module, "dual_port_ram") > 0);
        pb_free(&res);
    }
}

static void test_other_attribute_silent(void) {
    pb_result res;
    build_mem("  attribute \\keep 1\n  attribute \\src \"t.v:1.1\"\n", &res);
    map_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, count_in("is ignored"));
    pb_free(&res);
}

/* memory_merge warns too, under its own name. */
static void test_memory_merge_warns(void) {
    pb_result res;
    build_mem("  attribute \\ram_style \"block\"\n", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(res.design, "memory_merge", odin3_bytes_cstr("")));
    TEST_ASSERT_EQUAL_UINT32(1, count_in("memory_merge: memory 'mem': attribute 'ram_style' is "
                                         "ignored; mapping follows the architecture"));
    pb_free(&res);
}

/* An explicit RAM instance. */
static void test_ram_instance(void) {
    pb_result res;
    hm_build_ok("module \\m\n  wire width 1 input 1 \\clk\n  wire width 15 input 2 \\a\n"
                "  wire width 1 input 3 \\d\n  wire width 1 input 4 \\w\n"
                "  wire width 1 output 5 \\o\n  attribute \\ram_style \"block\"\n"
                "  cell \\single_port_ram \\r1\n    parameter \\ADDR_WIDTH 15\n"
                "    parameter \\DATA_WIDTH 1\n    connect \\clk \\clk\n    connect \\addr \\a\n"
                "    connect \\data \\d\n    connect \\we \\w\n    connect \\out \\o\n  end\nend\n",
                &res);
    map_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(1, count_in("partial_map: memory 'r1': attribute 'ram_style' is "
                                         "ignored; mapping follows the architecture"));
    pb_free(&res);
}

/* --- no rule reads an attribute ------------------------------------------------------------ */

static const char *const k_names[] = {"ram_style",   "rom_style", "ramstyle",
                                      "logic_block", "block_ram", "no_rw_check"};

static bool names_attribute(const char *text) {
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strstr(text, k_names[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *buf = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    size_t got = fread(buf, 1, SRC_SIZE - 1, file);
    buf[got] = '\0';
    (void)fclose(file);
    return buf;
}

/* Reads dir/name; true when the file names an attribute. */
static bool source_names_them(const char *dir_path, const char *name) {
    char path[PATH_SIZE];
    int len = snprintf(path, sizeof path, "%s/%s", dir_path, name);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    char *text = read_all(path);
    bool found = names_attribute(text);
    free(text);
    return found;
}

static bool is_source(const char *name) {
    size_t len = strlen(name);
    return len > 2 && name[len - 2] == '.';
}

/* Only ramstyle.c names the attributes among the partial_map sources. */
static void test_only_ramstyle_names_them(void) {
    char dir_path[PATH_SIZE];
    int len = snprintf(dir_path, sizeof dir_path, "%s/src/passes/partial_map", ODIN3_SOURCE_DIR);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof dir_path);
    DIR *dir = opendir(dir_path);
    TEST_ASSERT_NOT_NULL(dir);
    uint32_t naming = 0;
    char offender[PATH_SIZE] = "";
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        if (is_source(ent->d_name) && source_names_them(dir_path, ent->d_name)) {
            naming++;
            (void)snprintf(offender, sizeof offender, "%s", ent->d_name);
        }
    }
    (void)closedir(dir);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, naming, offender);
    TEST_ASSERT_EQUAL_STRING("ramstyle.c", offender);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_warning_and_same_mapping);
    RUN_TEST(test_names_case_and_values);
    RUN_TEST(test_other_attribute_silent);
    RUN_TEST(test_memory_merge_warns);
    RUN_TEST(test_ram_instance);
    RUN_TEST(test_only_ramstyle_names_them);
    return UNITY_END();
}
