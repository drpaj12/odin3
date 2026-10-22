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

/* Reads lib/abc/<name> into buf (NUL-terminated); returns its length. */
static size_t read_lib(const char *path, char *buf, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(buf, 1, cap - 1, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE(len > 0 && len < cap - 1);
    buf[len] = '\0';
    return len;
}

/* The start of text up to the line that begins with stop, comment lines left out. */
static size_t logic_prefix(const char *text, char *out, size_t cap, const char *stop) {
    size_t used = 0;
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) + 1 : strlen(line);
        if (strncmp(line, stop, strlen(stop)) == 0) {
            break;
        }
        if (line[0] != '#') {
            TEST_ASSERT_TRUE(used + len < cap);
            memcpy(out + used, line, len);
            used += len;
        }
        line += len;
    }
    out[used] = '\0';
    return used;
}

/* The embedded gates script equals lib/abc/gates.abc, and its optimisation lines (before `map -a`)
 * equal vtr.abc's (before `if -K`): the two flows differ only in the mapper (gatemap spec G-5). */
static void test_gates_script(void) {
    static char gates[FILE_CAP];
    static char vtr[FILE_CAP];
    static char gates_opt[FILE_CAP];
    static char vtr_opt[FILE_CAP];
    size_t len = read_lib(ODIN3_SOURCE_DIR "/lib/abc/gates.abc", gates, sizeof gates);
    TEST_ASSERT_EQUAL_UINT32(len, odin3_abc_gates_script_len);
    TEST_ASSERT_EQUAL_MEMORY(gates, odin3_abc_gates_script, len);
    (void)read_lib(ODIN3_SOURCE_DIR "/lib/abc/vtr.abc", vtr, sizeof vtr);
    size_t n_gates = logic_prefix(gates, gates_opt, sizeof gates_opt, "map -a");
    size_t n_vtr = logic_prefix(vtr, vtr_opt, sizeof vtr_opt, "if -K");
    TEST_ASSERT_TRUE(n_gates > 0);
    TEST_ASSERT_EQUAL_size_t(n_vtr, n_gates);
    TEST_ASSERT_EQUAL_STRING(vtr_opt, gates_opt);
}

static void test_no_lut(void) {
    odin3_abc_script script = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_default(0, ODIN3_ABC_EXCHANGE_BLIF, &script));
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    TEST_ASSERT_EQUAL_STRING(
        "abc: builtin:lib/abc/vtr.abc:10: the script uses {K} but abc was given no --lut", g_last);
    TEST_ASSERT_NULL(script.arena);
}

/* --lut 13 to 16 are legal, but linked ABC maps at most 12 inputs (final review C1). */
static void test_k_above_linked_cap(void) {
    odin3_abc_script script = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_default(13, ODIN3_ABC_EXCHANGE_AIG, &script));
    TEST_ASSERT_EQUAL_STRING(
        "abc: builtin:lib/abc/vtr.abc:10: the script uses {K} = 13, but linked "
        "ABC maps LUTs of at most 12 inputs ('&st' and '&synch2' crash on "
        "wider ones at the pin)",
        g_last);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_default(12, ODIN3_ABC_EXCHANGE_AIG, &script));
    odin3_abc_script_free(&script);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_equal_file);
    RUN_TEST(test_gates_script);
    RUN_TEST(test_commands_k6);
    RUN_TEST(test_no_lut);
    RUN_TEST(test_k_above_linked_cap);
    return UNITY_END();
}
