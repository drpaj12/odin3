/*
 * test_abc_default_script.c — the embedded default script (3D spec §7.7, AB-23; plan Task 11):
 * its bytes and SHA-256 equal lib/abc/vtr.abc's, it checks to VTR's eight logic commands with K 6
 * at lines 5-12 in both exchanges, and without --lut it fails at line 10.
 */
#include "passes/abc/abc.h"
#include "passes/abc/script.h"
#include "unity.h"
#include "util/log.h"
#include "util/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
#endif

enum { FILE_CAP = 4096, LINE_CAP = 1024 };

static char g_last[LINE_CAP];
static size_t g_lines;
static unsigned char g_file[FILE_CAP];
static size_t g_file_len;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    g_lines++;
    size_t len = strnlen(msg, sizeof g_last - 1);
    memcpy(g_last, msg, len);
    g_last[len] = '\0';
}

void setUp(void) {
    g_lines = 0;
    odin3_log_set_sink(sink, NULL);
    FILE *file = fopen(ODIN3_SOURCE_DIR "/lib/abc/vtr.abc", "rb");
    TEST_ASSERT_NOT_NULL(file);
    g_file_len = fread(g_file, 1, sizeof g_file, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE(g_file_len > 0 && g_file_len < sizeof g_file);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static odin3_status parse_default(uint32_t lut_k, uint32_t exchange, odin3_abc_script *out) {
    odin3_abc_script_src src = {ODIN3_ABC_DEFAULT_SCRIPT_NAME,
                                NULL,
                                {odin3_abc_default_script, odin3_abc_default_script_len}};
    return odin3_abc_script_parse(&src, lut_k, exchange, out);
}

static void test_bytes_equal_file(void) {
    TEST_ASSERT_EQUAL_UINT32(g_file_len, odin3_abc_default_script_len);
    TEST_ASSERT_EQUAL_MEMORY(g_file, odin3_abc_default_script, g_file_len);
    odin3_sha256 sha;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, g_file, g_file_len);
    odin3_sha256_final(&sha, digest);
    odin3_sha256_hex(digest, hex);
    odin3_abc_script script;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_default(6, ODIN3_ABC_EXCHANGE_AIG, &script));
    TEST_ASSERT_EQUAL_STRING(hex, script.sha256_hex);
    odin3_abc_script_free(&script);
}

/* With K 6: exactly VTR's eight logic commands, at lines 5-12, ending in the ntk space. */
static void check_commands_k6(uint32_t exchange) {
    static const char *const want[] = {"strash", "ifraig -v",  "scorr -v", "dc2 -v",
                                       "dch -f", "if -K 6 -v", "mfs2 -v",  "print_stats"};
    enum { N = sizeof want / sizeof want[0], FIRST_LINE = 5 };
    odin3_abc_script script;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_default(6, exchange, &script));
    TEST_ASSERT_EQUAL_size_t(0, g_lines);
    TEST_ASSERT_EQUAL_UINT32(N, script.n_cmds);
    for (uint32_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], script.cmds[i].line);
        TEST_ASSERT_EQUAL_UINT32(FIRST_LINE + i, script.cmds[i].src_line);
        TEST_ASSERT_NULL(script.cmds[i].alias);
    }
    TEST_ASSERT_TRUE(script.uses_k);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SPACE_NTK, script.final_space);
    odin3_abc_script_free(&script);
}

static void test_commands_k6(void) {
    check_commands_k6(ODIN3_ABC_EXCHANGE_AIG);
    check_commands_k6(ODIN3_ABC_EXCHANGE_BLIF);
}

static void test_no_lut(void) {
    odin3_abc_script script = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_default(0, ODIN3_ABC_EXCHANGE_BLIF, &script));
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    TEST_ASSERT_EQUAL_STRING(
        "abc: builtin:lib/abc/vtr.abc:10: the script uses {K} but abc was given no --lut", g_last);
    TEST_ASSERT_NULL(script.arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_equal_file);
    RUN_TEST(test_commands_k6);
    RUN_TEST(test_no_lut);
    return UNITY_END();
}
