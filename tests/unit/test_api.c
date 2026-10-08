/*
 * test_api.c — unit tests for the ABI v1 entry points in odin3.h.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#ifndef EXAMPLE_PLUGIN_PATH
#error "EXAMPLE_PLUGIN_PATH must be defined by the build"
#endif

void setUp(void) {
}
void tearDown(void) {
}

enum { VERSION_BUF_LEN = 32, PREFIX_LEN = 6 };

static void test_version_string_matches_macros(void) {
    char expected[VERSION_BUF_LEN];
    (void)snprintf(expected, sizeof expected, "%d.%d.%d", ODIN3_VERSION_MAJOR, ODIN3_VERSION_MINOR,
                   ODIN3_VERSION_PATCH);
    TEST_ASSERT_EQUAL_STRING(expected, odin3_version_string());
}

static void test_abi_version_matches_header(void) {
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ODIN3_ABI_VERSION, odin3_abi_version());
    TEST_ASSERT_EQUAL_UINT32(1, odin3_abi_version()); /* 1B added ODIN3_ERR_CHECK */
}

static void test_check_status_has_a_name(void) {
    TEST_ASSERT_EQUAL_INT(6, ODIN3_ERR_CHECK);
    TEST_ASSERT_EQUAL_INT(7, ODIN3_STATUS_COUNT);
    TEST_ASSERT_EQUAL_STRING("ODIN3_ERR_CHECK", odin3_status_string(ODIN3_ERR_CHECK));
}

static void test_every_status_has_a_name(void) {
    for (int status = 0; status < ODIN3_STATUS_COUNT; status++) {
        const char *name = odin3_status_string((odin3_status)status);
        TEST_ASSERT_EQUAL_INT(0, strncmp(name, "ODIN3_", PREFIX_LEN));
        TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(name, "ODIN3_STATUS_UNKNOWN"));
    }
    TEST_ASSERT_EQUAL_STRING("ODIN3_OK", odin3_status_string(ODIN3_OK));
}

static void test_out_of_range_status_is_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("ODIN3_STATUS_UNKNOWN", odin3_status_string(ODIN3_STATUS_COUNT));
}

static void test_plugin_load_rejects_null(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_plugin_load(NULL));
}

static void test_plugin_load_missing_file_is_io_error(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_plugin_load("/nonexistent/odin3_plugin.so"));
}

static void test_plugin_without_init_is_plugin_error(void) {
    /* libm is always present and exports no odin3_plugin_init. */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PLUGIN, odin3_plugin_load("libm.so.6"));
}

static void test_plugin_load_example_plugin(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_plugin_load(EXAMPLE_PLUGIN_PATH));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_version_string_matches_macros);
    RUN_TEST(test_abi_version_matches_header);
    RUN_TEST(test_check_status_has_a_name);
    RUN_TEST(test_every_status_has_a_name);
    RUN_TEST(test_out_of_range_status_is_unknown);
    RUN_TEST(test_plugin_load_rejects_null);
    RUN_TEST(test_plugin_load_missing_file_is_io_error);
    RUN_TEST(test_plugin_without_init_is_plugin_error);
    RUN_TEST(test_plugin_load_example_plugin);
    return UNITY_END();
}
